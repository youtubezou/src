/*	$OpenBSD: frontend.c,v 1.2 2018/07/10 22:14:19 florian Exp $	*/

/*
 * Copyright (c) 2018 Florian Obser <florian@openbsd.org>
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/uio.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet6/nd6.h>
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/icmp6.h>

#include <errno.h>
#include <event.h>
#include <ifaddrs.h>
#include <imsg.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "rad.h"
#include "frontend.h"
#include "control.h"

struct icmp6_ev {
	struct event		 ev;
	uint8_t			 answer[1500];
	struct msghdr		 rcvmhdr;
	struct iovec		 rcviov[1];
	struct sockaddr_in6	 from;
} icmp6ev;

struct ra_iface {
	TAILQ_ENTRY(ra_iface)		entry;
	struct ra_prefix_conf_head	prefixes;
	char				name[IF_NAMESIZE];
	uint32_t			if_index;
	int				removed;
	int				prefix_count;
	size_t				datalen;
	uint8_t				data[1500];
};

TAILQ_HEAD(, ra_iface)	ra_interfaces;

__dead void		 frontend_shutdown(void);
void			 frontend_sig_handler(int, short, void *);
void			 frontend_startup(void);
void			 icmp6_receive(int, short, void *);
void			 join_all_routers_mcast_group(struct ra_iface *);
void			 leave_all_routers_mcast_group(struct ra_iface *);
void			 merge_ra_interfaces(void);
struct ra_iface		*find_ra_iface_by_id(uint32_t);
struct ra_iface		*find_ra_iface_by_name(char *);
struct ra_iface_conf	*find_ra_iface_conf(struct ra_iface_conf_head *,
			    char *);
struct ra_prefix_conf	*find_ra_prefix_conf(struct ra_prefix_conf_head*,
			    struct in6_addr *, int);
void			 add_new_prefix_to_ra_iface(struct ra_iface *r,
			    struct in6_addr *, int, struct ra_prefix_conf *);
void			 free_ra_iface(struct ra_iface *);
int			 in6_mask2prefixlen(struct in6_addr *);
void			 get_interface_prefixes(struct ra_iface *,
			     struct ra_prefix_conf *);
void			 build_package(struct ra_iface *);
void			 ra_output(struct ra_iface *, struct sockaddr_in6 *);

struct rad_conf	*frontend_conf;
struct imsgev		*iev_main;
struct imsgev		*iev_engine;
int			 icmp6sock = -1, ioctlsock = -1;
struct ipv6_mreq	 all_routers;
struct sockaddr_in6	 all_nodes;
struct msghdr		 sndmhdr;
struct iovec		 sndiov[2];

void
frontend_sig_handler(int sig, short event, void *bula)
{
	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		frontend_shutdown();
	default:
		fatalx("unexpected signal");
	}
}

void
frontend(int debug, int verbose, char *sockname)
{
	struct event		 ev_sigint, ev_sigterm;
	struct passwd		*pw;
	size_t			 rcvcmsglen, sndcmsgbuflen;
	uint8_t			*rcvcmsgbuf;
	uint8_t			*sndcmsgbuf = NULL;

	frontend_conf = config_new_empty();

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	/* XXX pass in from main */
	/* Create rad control socket outside chroot. */
	if (control_init(sockname) == -1)
		fatalx("control socket setup failed");

	if ((pw = getpwnam(RAD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	rad_process = PROC_FRONTEND;
	setproctitle("%s", log_procnames[rad_process]);
	log_procinit(log_procnames[rad_process]);

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	/* XXX pass in from main */
	if ((ioctlsock = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
		fatal("socket");

	if (pledge("stdio inet unix recvfd route mcast", NULL) == -1)
		fatal("pledge");

	event_init();

	/* Setup signal handler. */
	signal_set(&ev_sigint, SIGINT, frontend_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, frontend_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* Setup pipe and event handler to the parent process. */
	if ((iev_main = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);
	imsg_init(&iev_main->ibuf, 3);
	iev_main->handler = frontend_dispatch_main;
	iev_main->events = EV_READ;
	event_set(&iev_main->ev, iev_main->ibuf.fd, iev_main->events,
	    iev_main->handler, iev_main);
	event_add(&iev_main->ev, NULL);

	rcvcmsglen = CMSG_SPACE(sizeof(struct in6_pktinfo)) +
	    CMSG_SPACE(sizeof(int));
	if((rcvcmsgbuf = malloc(rcvcmsglen)) == NULL)
		fatal("malloc");

	icmp6ev.rcviov[0].iov_base = (caddr_t)icmp6ev.answer;
	icmp6ev.rcviov[0].iov_len = sizeof(icmp6ev.answer);
	icmp6ev.rcvmhdr.msg_name = (caddr_t)&icmp6ev.from;
	icmp6ev.rcvmhdr.msg_namelen = sizeof(icmp6ev.from);
	icmp6ev.rcvmhdr.msg_iov = icmp6ev.rcviov;
	icmp6ev.rcvmhdr.msg_iovlen = 1;
	icmp6ev.rcvmhdr.msg_control = (caddr_t) rcvcmsgbuf;
	icmp6ev.rcvmhdr.msg_controllen = rcvcmsglen;

	if (inet_pton(AF_INET6, "ff02::2",
	    &all_routers.ipv6mr_multiaddr.s6_addr) == -1)
		fatal("inet_pton");

	all_nodes.sin6_len = sizeof(all_nodes);
	all_nodes.sin6_family = AF_INET6;
	if (inet_pton(AF_INET6, "ff02::1", &all_nodes.sin6_addr) != 1)
		fatal("inet_pton");

	sndcmsgbuflen = CMSG_SPACE(sizeof(struct in6_pktinfo)) +
	    CMSG_SPACE(sizeof(int));
	if ((sndcmsgbuf = malloc(sndcmsgbuflen)) == NULL)
		fatal("%s", __func__);

	sndmhdr.msg_namelen = sizeof(struct sockaddr_in6);
	sndmhdr.msg_iov = sndiov;
	sndmhdr.msg_iovlen = 1;
	sndmhdr.msg_control = sndcmsgbuf;
	sndmhdr.msg_controllen = sndcmsgbuflen;

	TAILQ_INIT(&ra_interfaces);

	/* Listen on control socket. */
	TAILQ_INIT(&ctl_conns);
	control_listen();

	event_dispatch();

	frontend_shutdown();
}

__dead void
frontend_shutdown(void)
{
	/* Close pipes. */
	msgbuf_write(&iev_engine->ibuf.w);
	msgbuf_clear(&iev_engine->ibuf.w);
	close(iev_engine->ibuf.fd);
	msgbuf_write(&iev_main->ibuf.w);
	msgbuf_clear(&iev_main->ibuf.w);
	close(iev_main->ibuf.fd);

	config_clear(frontend_conf);

	free(iev_engine);
	free(iev_main);

	log_info("frontend exiting");
	exit(0);
}

int
frontend_imsg_compose_main(int type, pid_t pid, void *data, uint16_t datalen)
{
	return (imsg_compose_event(iev_main, type, 0, pid, -1, data,
	    datalen));
}

int
frontend_imsg_compose_engine(int type, pid_t pid, void *data, uint16_t datalen)
{
	return (imsg_compose_event(iev_engine, type, 0, pid, -1, data,
	    datalen));
}

void
frontend_dispatch_main(int fd, short event, void *bula)
{
	static struct rad_conf		*nconf;
	static struct ra_iface_conf	*ra_iface_conf;
	struct imsg			 imsg;
	struct imsgev			*iev = bula;
	struct imsgbuf			*ibuf = &iev->ibuf;
	struct ra_prefix_conf		*ra_prefix_conf;
	int				 n, shut = 0;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_SOCKET_IPC:
			/*
			 * Setup pipe and event handler to the engine
			 * process.
			 */
			if (iev_engine) {
				log_warnx("%s: received unexpected imsg fd "
				    "to frontend", __func__);
				break;
			}
			if ((fd = imsg.fd) == -1) {
				log_warnx("%s: expected to receive imsg fd to "
				   "frontend but didn't receive any",
				   __func__);
				break;
			}

			iev_engine = malloc(sizeof(struct imsgev));
			if (iev_engine == NULL)
				fatal(NULL);

			imsg_init(&iev_engine->ibuf, fd);
			iev_engine->handler = frontend_dispatch_engine;
			iev_engine->events = EV_READ;

			event_set(&iev_engine->ev, iev_engine->ibuf.fd,
			iev_engine->events, iev_engine->handler, iev_engine);
			event_add(&iev_engine->ev, NULL);
			break;
		case IMSG_RECONF_CONF:
			if ((nconf = malloc(sizeof(struct rad_conf))) ==
			    NULL)
				fatal(NULL);
			memcpy(nconf, imsg.data, sizeof(struct rad_conf));
			SIMPLEQ_INIT(&nconf->ra_iface_list);
			break;
		case IMSG_RECONF_RA_IFACE:
			if ((ra_iface_conf = malloc(sizeof(struct
			    ra_iface_conf))) == NULL)
				fatal(NULL);
			memcpy(ra_iface_conf, imsg.data, sizeof(struct
			    ra_iface_conf));
			ra_iface_conf->autoprefix = NULL;
			SIMPLEQ_INIT(&ra_iface_conf->ra_prefix_list);
			SIMPLEQ_INSERT_TAIL(&nconf->ra_iface_list,
			    ra_iface_conf, entry);
			break;
		case IMSG_RECONF_RA_AUTOPREFIX:
			if ((ra_iface_conf->autoprefix = malloc(sizeof(struct
			    ra_prefix_conf))) == NULL)
				fatal(NULL);
			memcpy(ra_iface_conf->autoprefix, imsg.data,
			    sizeof(struct ra_prefix_conf));
			break;
		case IMSG_RECONF_RA_PREFIX:
			if ((ra_prefix_conf = malloc(sizeof(struct
			    ra_prefix_conf))) == NULL)
				fatal(NULL);
			memcpy(ra_prefix_conf, imsg.data,
			    sizeof(struct ra_prefix_conf));
			SIMPLEQ_INSERT_TAIL(&ra_iface_conf->ra_prefix_list,
			    ra_prefix_conf, entry);
			break;
		case IMSG_RECONF_END:
			merge_config(frontend_conf, nconf);
			merge_ra_interfaces();
			nconf = NULL;
			break;
		case IMSG_ICMP6SOCK:
			if ((icmp6sock = imsg.fd) == -1)
				fatalx("%s: expected to receive imsg "
				    "ICMPv6 fd but didn't receive any",
				    __func__);
			event_set(&icmp6ev.ev, icmp6sock, EV_READ | EV_PERSIST,
			    icmp6_receive, NULL);
		case IMSG_STARTUP:
			if (pledge("stdio inet unix route mcast", NULL) == -1)
				fatal("pledge");
			frontend_startup();
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
frontend_dispatch_engine(int fd, short event, void *bula)
{
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg		 imsg;
	struct imsg_send_ra	 send_ra;
	struct ra_iface		*ra_iface;
	int			 n, shut = 0;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_SEND_RA:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(send_ra))
				fatal("%s: IMSG_SEND_RA wrong length: %d",
				    __func__, imsg.hdr.len);
			memcpy(&send_ra, imsg.data, sizeof(send_ra));
			ra_iface = find_ra_iface_by_id(send_ra.if_index);
			if (ra_iface)
				ra_output(ra_iface, &send_ra.to);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
frontend_startup(void)
{
#if 0
	if (!event_initialized(&ev_route))
		fatalx("%s: did not receive a route socket from the main "
		    "process", __func__);

	event_add(&ev_route, NULL);
#endif

	if (!event_initialized(&icmp6ev.ev))
		fatalx("%s: did not receive a icmp6 socket fd from the main "
		    "process", __func__);

	event_add(&icmp6ev.ev, NULL);

	frontend_imsg_compose_main(IMSG_STARTUP_DONE, 0, NULL, 0);
}


void
icmp6_receive(int fd, short events, void *arg)
{
	struct imsg_ra_rs	 ra_rs;
	struct in6_pktinfo	*pi = NULL;
	struct cmsghdr		*cm;
	ssize_t			 len;
	int			 if_index = 0, *hlimp = NULL;
	char			 ntopbuf[INET6_ADDRSTRLEN], ifnamebuf[IFNAMSIZ];

	if ((len = recvmsg(fd, &icmp6ev.rcvmhdr, 0)) < 0) {
		log_warn("recvmsg");
		return;
	}

	/* extract optional information via Advanced API */
	for (cm = (struct cmsghdr *)CMSG_FIRSTHDR(&icmp6ev.rcvmhdr); cm;
	    cm = (struct cmsghdr *)CMSG_NXTHDR(&icmp6ev.rcvmhdr, cm)) {
		if (cm->cmsg_level == IPPROTO_IPV6 &&
		    cm->cmsg_type == IPV6_PKTINFO &&
		    cm->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo))) {
			pi = (struct in6_pktinfo *)(CMSG_DATA(cm));
			if_index = pi->ipi6_ifindex;
		}
		if (cm->cmsg_level == IPPROTO_IPV6 &&
		    cm->cmsg_type == IPV6_HOPLIMIT &&
		    cm->cmsg_len == CMSG_LEN(sizeof(int)))
			hlimp = (int *)CMSG_DATA(cm);
	}

	if (if_index == 0) {
		log_warnx("failed to get receiving interface");
		return;
	}

	if (hlimp == NULL) {
		log_warnx("failed to get receiving hop limit");
		return;
	}

	if (*hlimp != 255) {
		log_warnx("invalid RA or RS with hop limit of %d from %s on %s",
		    *hlimp, inet_ntop(AF_INET6, &icmp6ev.from.sin6_addr,
		    ntopbuf, INET6_ADDRSTRLEN), if_indextoname(if_index,
		    ifnamebuf));
		return;
	}

	log_warnx("RA or RS with hop limit of %d from %s on %s",
	    *hlimp, inet_ntop(AF_INET6, &icmp6ev.from.sin6_addr,
	    ntopbuf, INET6_ADDRSTRLEN), if_indextoname(if_index,
	    ifnamebuf));

	if ((size_t)len > sizeof(ra_rs.packet)) {
		log_warnx("invalid RA or RS with size %ld from %s on %s",
		    len, inet_ntop(AF_INET6, &icmp6ev.from.sin6_addr,
		    ntopbuf, INET6_ADDRSTRLEN), if_indextoname(if_index,
		    ifnamebuf));
		return;
	}

	ra_rs.if_index = if_index;
	memcpy(&ra_rs.from,  &icmp6ev.from, sizeof(ra_rs.from));
	ra_rs.len = len;
	memcpy(ra_rs.packet, icmp6ev.answer, len);

	frontend_imsg_compose_engine(IMSG_RA_RS, 0, &ra_rs, sizeof(ra_rs));
}

void
join_all_routers_mcast_group(struct ra_iface *ra_iface)
{
	log_debug("joining multicast group on %s", ra_iface->name);
	all_routers.ipv6mr_interface = ra_iface->if_index;
	if (setsockopt(icmp6sock, IPPROTO_IPV6, IPV6_JOIN_GROUP,
	    &all_routers, sizeof(all_routers)) == -1)
		fatal("IPV6_JOIN_GROUP(%s)", ra_iface->name);
}

void
leave_all_routers_mcast_group(struct ra_iface *ra_iface)
{
	log_debug("leaving multicast group on %s", ra_iface->name);
	all_routers.ipv6mr_interface = ra_iface->if_index;
	if (setsockopt(icmp6sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
	    &all_routers, sizeof(all_routers)) == -1)
		fatal("IPV6_LEAVE_GROUP(%s)", ra_iface->name);
}

struct ra_iface*
find_ra_iface_by_id(uint32_t if_index) {
	struct ra_iface	*ra_iface;

	TAILQ_FOREACH(ra_iface, &ra_interfaces, entry) {
		if (ra_iface->if_index == if_index)
			return ra_iface;
	}
	return (NULL);
}

struct ra_iface*
find_ra_iface_by_name(char *if_name)
{
	struct ra_iface	*ra_iface;

	TAILQ_FOREACH(ra_iface, &ra_interfaces, entry) {
		if (strcmp(ra_iface->name, if_name) == 0)
			return ra_iface;
	}
	return (NULL);
}

struct ra_iface_conf*
find_ra_iface_conf(struct ra_iface_conf_head *head, char *if_name)
{
	struct ra_iface_conf	*ra_iface_conf;

	SIMPLEQ_FOREACH(ra_iface_conf, head, entry) {
		if (strcmp(ra_iface_conf->name, if_name) == 0)
			return ra_iface_conf;
	}
	return (NULL);
}

void
merge_ra_interfaces(void)
{
	struct ra_iface_conf	*ra_iface_conf;
	struct ra_prefix_conf	*ra_prefix_conf;
	struct ra_iface		*ra_iface, *tmp;
	uint32_t		 if_index;

	TAILQ_FOREACH(ra_iface, &ra_interfaces, entry)
		ra_iface->removed = 1;

	SIMPLEQ_FOREACH(ra_iface_conf, &frontend_conf->ra_iface_list, entry) {
		ra_iface = find_ra_iface_by_name(ra_iface_conf->name);
		if (ra_iface == NULL) {
			log_debug("new interface %s", ra_iface_conf->name);
			if ((if_index = if_nametoindex(ra_iface_conf->name))
			    == 0)
				continue;
			log_debug("adding interface %s", ra_iface_conf->name);
			if ((ra_iface = calloc(1, sizeof(*ra_iface))) == NULL)
				fatal("%s", __func__);

			(void) strlcpy(ra_iface->name, ra_iface_conf->name,
			    sizeof(ra_iface->name));
			ra_iface->if_index = if_index;
			SIMPLEQ_INIT(&ra_iface->prefixes);
			TAILQ_INSERT_TAIL(&ra_interfaces, ra_iface, entry);
			join_all_routers_mcast_group(ra_iface);
		} else {
			log_debug("keeping interface %s", ra_iface_conf->name);
			ra_iface->removed = 0;
		}
	}

	TAILQ_FOREACH_SAFE(ra_iface, &ra_interfaces, entry, tmp) {
		if (ra_iface->removed) {
			TAILQ_REMOVE(&ra_interfaces, ra_iface, entry);
			free_ra_iface(ra_iface);
		}
	}

	TAILQ_FOREACH(ra_iface, &ra_interfaces, entry) {
		while ((ra_prefix_conf = SIMPLEQ_FIRST(&ra_iface->prefixes))
		    != NULL) {
			SIMPLEQ_REMOVE_HEAD(&ra_iface->prefixes,
			    entry);
			free(ra_prefix_conf);
		}
		ra_iface->prefix_count = 0;

		ra_iface_conf = find_ra_iface_conf(
		    &frontend_conf->ra_iface_list, ra_iface->name);

		if (ra_iface_conf->autoprefix)
			get_interface_prefixes(ra_iface,
			    ra_iface_conf->autoprefix);

		log_debug("add static prefixes for %s", ra_iface->name);

		SIMPLEQ_FOREACH(ra_prefix_conf, &ra_iface_conf->ra_prefix_list,
		    entry) {
			add_new_prefix_to_ra_iface(ra_iface,
			    &ra_prefix_conf->prefix,
			    ra_prefix_conf->prefixlen, ra_prefix_conf);
		}
		build_package(ra_iface);
	}
}

void
free_ra_iface(struct ra_iface *ra_iface)
{
	struct ra_prefix_conf	*prefix;

	leave_all_routers_mcast_group(ra_iface);

	while ((prefix = SIMPLEQ_FIRST(&ra_iface->prefixes)) != NULL) {
		SIMPLEQ_REMOVE_HEAD(&ra_iface->prefixes, entry);
		free(prefix);
	}

	free(ra_iface);
}

/* from kame via ifconfig, where it's called prefix() */
int
in6_mask2prefixlen(struct in6_addr *in6)
{
	u_char *nam = (u_char *)in6;
	int byte, bit, plen = 0, size = sizeof(struct in6_addr);

	for (byte = 0; byte < size; byte++, plen += 8)
		if (nam[byte] != 0xff)
			break;
	if (byte == size)
		return (plen);
	for (bit = 7; bit != 0; bit--, plen++)
		if (!(nam[byte] & (1 << bit)))
			break;
	for (; bit != 0; bit--)
		if (nam[byte] & (1 << bit))
			return (0);
	byte++;
	for (; byte < size; byte++)
		if (nam[byte])
			return (0);
	return (plen);
}

void
get_interface_prefixes(struct ra_iface *ra_iface, struct ra_prefix_conf
    *autoprefix)
{
	struct in6_ifreq	 ifr6;
	struct ifaddrs		*ifap, *ifa;
	struct sockaddr_in6	*sin6;
	int			 prefixlen;

	log_debug("%s: %s", __func__, ra_iface->name);

	if (getifaddrs(&ifap) != 0)
		fatal("getifaddrs");

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (strcmp(ra_iface->name, ifa->ifa_name) != 0)
			continue;
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;

		sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;

		if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
			continue;

		memset(&ifr6, 0, sizeof(ifr6));
		(void) strlcpy(ifr6.ifr_name, ra_iface->name,
		    sizeof(ifr6.ifr_name));
		memcpy(&ifr6.ifr_addr, sin6, sizeof(ifr6.ifr_addr));
		
		if (ioctl(ioctlsock, SIOCGIFNETMASK_IN6, (caddr_t)&ifr6) < 0)
			fatal("SIOCGIFNETMASK_IN6");

		prefixlen = in6_mask2prefixlen(&((struct sockaddr_in6 *)
		    &ifr6.ifr_addr)->sin6_addr);

		if (prefixlen == 128)
			continue;

		mask_prefix(&sin6->sin6_addr, prefixlen);

		add_new_prefix_to_ra_iface(ra_iface, &sin6->sin6_addr,
		    prefixlen, autoprefix);

	}
}

struct ra_prefix_conf*
find_ra_prefix_conf(struct ra_prefix_conf_head* head, struct in6_addr *prefix,
    int prefixlen)
{
	struct ra_prefix_conf	*ra_prefix_conf;

	SIMPLEQ_FOREACH(ra_prefix_conf, head, entry) {
		if (ra_prefix_conf->prefixlen == prefixlen &&
		    memcmp(&ra_prefix_conf->prefix, prefix, sizeof(*prefix)) ==
		    0)
			return (ra_prefix_conf);
	}
	return (NULL);
}

void
add_new_prefix_to_ra_iface(struct ra_iface *ra_iface, struct in6_addr *addr,
    int prefixlen, struct ra_prefix_conf *ra_prefix_conf)
{
	struct ra_prefix_conf	*new_ra_prefix_conf;

	if (find_ra_prefix_conf(&ra_iface->prefixes, addr, prefixlen)) {
		log_debug("ignoring duplicate %s/%d prefix",
		    in6_to_str(addr), prefixlen);
		return;
	}

	log_debug("adding %s/%d prefix", in6_to_str(addr), prefixlen);

	if ((new_ra_prefix_conf = calloc(1, sizeof(*ra_prefix_conf))) == NULL)
		fatal("%s", __func__);
	new_ra_prefix_conf->prefix = *addr;
	new_ra_prefix_conf->prefixlen = prefixlen;
	new_ra_prefix_conf->vltime = ra_prefix_conf->vltime;
	new_ra_prefix_conf->pltime = ra_prefix_conf->pltime;
	new_ra_prefix_conf->aflag = ra_prefix_conf->aflag;
	new_ra_prefix_conf->lflag = ra_prefix_conf->lflag;
	SIMPLEQ_INSERT_TAIL(&ra_iface->prefixes, new_ra_prefix_conf, entry);
	ra_iface->prefix_count++;
}

void
build_package(struct ra_iface *ra_iface)
{
	struct nd_router_advert		*ra;
	struct nd_opt_prefix_info	*ndopt_pi;
	struct ra_iface_conf		*ra_iface_conf;
	struct ra_options_conf		*ra_options_conf;
	struct ra_prefix_conf		*ra_prefix_conf;
	uint8_t				*buf;

	ra_iface_conf = find_ra_iface_conf(&frontend_conf->ra_iface_list,
	    ra_iface->name);
	ra_options_conf = &ra_iface_conf->ra_options;

	ra_iface->datalen = sizeof(*ra);
	ra_iface->datalen += sizeof(*ndopt_pi) * ra_iface->prefix_count;

	if (ra_iface->datalen > sizeof(ra_iface->data))
		fatal("%s: packet too big", __func__); /* XXX send multiple */

	buf = ra_iface->data;

	ra = (struct nd_router_advert *)buf;

	memset(ra, 0, sizeof(*ra));

	ra->nd_ra_type = ND_ROUTER_ADVERT;
	ra->nd_ra_curhoplimit = ra_options_conf->cur_hl;
	if (ra_options_conf->m_flag)
		ra->nd_ra_flags_reserved |= ND_RA_FLAG_MANAGED;
	if (ra_options_conf->o_flag)
		ra->nd_ra_flags_reserved |= ND_RA_FLAG_OTHER;
	if (ra_options_conf->dfr)
		ra->nd_ra_router_lifetime =
		    htons(ra_options_conf->router_lifetime);
	ra->nd_ra_reachable = htonl(ra_options_conf->reachable_time);
	ra->nd_ra_retransmit = htonl(ra_options_conf->retrans_timer);
	buf += sizeof(*ra);

	SIMPLEQ_FOREACH(ra_prefix_conf, &ra_iface->prefixes, entry) {
		ndopt_pi = (struct nd_opt_prefix_info *)buf;
		memset(ndopt_pi, 0, sizeof(*ndopt_pi));
		ndopt_pi->nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION;
		ndopt_pi->nd_opt_pi_len = 4;
		ndopt_pi->nd_opt_pi_prefix_len = ra_prefix_conf->prefixlen;
		if (ra_prefix_conf->lflag)
			ndopt_pi->nd_opt_pi_flags_reserved |=
			    ND_OPT_PI_FLAG_ONLINK;
		if (ra_prefix_conf->aflag)
			ndopt_pi->nd_opt_pi_flags_reserved |=
			    ND_OPT_PI_FLAG_AUTO;
		ndopt_pi->nd_opt_pi_valid_time = htonl(ra_prefix_conf->vltime);
		ndopt_pi->nd_opt_pi_preferred_time =
		    htonl(ra_prefix_conf->pltime);
		ndopt_pi->nd_opt_pi_prefix = ra_prefix_conf->prefix;

		buf += sizeof(*ndopt_pi);
	}
}

void
ra_output(struct ra_iface *ra_iface, struct sockaddr_in6 *to)
{

	struct cmsghdr		*cm;
	struct in6_pktinfo	*pi;
	ssize_t			 len;
	int			 hoplimit = 255;

	sndmhdr.msg_name = to;
	sndmhdr.msg_iov[0].iov_base = ra_iface->data;
	sndmhdr.msg_iov[0].iov_len = ra_iface->datalen;

	cm = CMSG_FIRSTHDR(&sndmhdr);
	/* specify the outgoing interface */
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
	pi = (struct in6_pktinfo *)CMSG_DATA(cm);
	memset(&pi->ipi6_addr, 0, sizeof(pi->ipi6_addr));
	pi->ipi6_ifindex = ra_iface->if_index;

	/* specify the hop limit of the packet */
	cm = CMSG_NXTHDR(&sndmhdr, cm);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_HOPLIMIT;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &hoplimit, sizeof(int));

	log_debug("send RA on %s", ra_iface->name);

	len = sendmsg(icmp6sock, &sndmhdr, 0);
	if (len < 0)
		log_warn("sendmsg on %s", ra_iface->name);

}
