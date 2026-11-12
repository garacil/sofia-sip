/*
 * This file is part of the Sofia-SIP package
 *
 * Copyright (C) 2006 Nokia Corporation.
 *
 * Contact: Pekka Pessi <pekka.pessi@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**@CFILE tport_type_udp.c UDP Transport
 *
 * See tport.docs for more detailed description of tport interface.
 *
 * @author Pekka Pessi <Pekka.Pessi@nokia.com>
 * @author Martti Mela <Martti.Mela@nokia.com>
 *
 * @date Created: Fri Mar 24 08:45:49 EET 2006 ppessi
 */

#include "config.h"

#include "tport_internal.h"
#include "sofia-sip/hostdomain.h"

#if HAVE_IP_RECVERR || HAVE_IPV6_RECVERR
#include <linux/types.h>
#include <linux/errqueue.h>
#include <sys/uio.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

/* The local address of a datagram, when the socket is bound to the wildcard
 * address: read with each datagram received, and set with each datagram sent
 * from a chosen address. */
#if defined(IP_PKTINFO) && (!SU_HAVE_IN6 || defined(IPV6_PKTINFO))
#define TPORT_UDP_PKTINFO 1
#else
#define TPORT_UDP_PKTINFO 0
#endif

#if TPORT_UDP_PKTINFO
#include <sched.h>
#endif

/* ---------------------------------------------------------------------- */
/* UDP */

static
int tport_udp_init_client(tport_primary_t *pri,
			  tp_name_t tpn[1],
			  su_addrinfo_t *ai,
			  tagi_t const *tags,
			  char const **return_culprit);

#if HAVE_FUNC
#elif HAVE_FUNCTION
#define __func__ __FUNCTION__
#else
static char const __func__[] = "tport_type_udp";
#endif

tport_vtable_t const tport_udp_client_vtable =
{
  /* vtp_name 		     */ "udp",
  /* vtp_public              */ tport_type_client,
  /* vtp_pri_size            */ sizeof (tport_primary_t),
  /* vtp_init_primary        */ tport_udp_init_client,
  /* vtp_deinit_primary      */ NULL,
  /* vtp_wakeup_pri          */ NULL,
  /* vtp_connect             */ NULL,
  /* vtp_secondary_size      */ sizeof (tport_t),
  /* vtp_init_secondary      */ NULL,
  /* vtp_deinit_secondary    */ NULL,
  /* vtp_shutdown            */ NULL,
  /* vtp_set_events          */ NULL,
  /* vtp_wakeup              */ NULL,
  /* vtp_recv                */ tport_recv_dgram,
  /* vtp_send                */ tport_send_dgram,
  /* vtp_deliver             */ NULL,
  /* vtp_prepare             */ NULL,
  /* vtp_keepalive           */ NULL,
  /* vtp_stun_response       */ NULL,
  /* vtp_next_secondary_timer*/ NULL,
  /* vtp_secondary_timer     */ NULL,
};

tport_vtable_t const tport_udp_vtable =
{
  /* vtp_name 		     */ "udp",
  /* vtp_public              */ tport_type_local,
  /* vtp_pri_size            */ sizeof (tport_primary_t),
  /* vtp_init_primary        */ tport_udp_init_primary,
  /* vtp_deinit_primary      */ NULL,
  /* vtp_wakeup_pri          */ NULL,
  /* vtp_connect             */ NULL,
  /* vtp_secondary_size      */ sizeof (tport_t),
  /* vtp_init_secondary      */ NULL,
  /* vtp_deinit_secondary    */ NULL,
  /* vtp_shutdown            */ NULL,
  /* vtp_set_events          */ NULL,
  /* vtp_wakeup              */ NULL,
  /* vtp_recv                */ tport_recv_dgram,
  /* vtp_send                */ tport_send_dgram,
  /* vtp_deliver             */ NULL,
  /* vtp_prepare             */ NULL,
  /* vtp_keepalive           */ NULL,
  /* vtp_stun_response       */ NULL,
  /* vtp_next_secondary_timer*/ NULL,
  /* vtp_secondary_timer     */ NULL,
};

static void tport_check_trunc(tport_t *tp, su_addrinfo_t *ai);

int tport_udp_init_primary(tport_primary_t *pri,
			   tp_name_t tpn[1],
			   su_addrinfo_t *ai,
			   tagi_t const *tags,
			   char const **return_culprit)
{
  unsigned rmem = 0, wmem = 0;
  int events = SU_WAIT_IN;
  int s;
#if HAVE_IP_ADD_MEMBERSHIP
  su_sockaddr_t *su = (su_sockaddr_t *)ai->ai_addr;
#endif
  int const one = 1; (void)one;

  s = su_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (s == INVALID_SOCKET)
    return *return_culprit = "socket", -1;

  pri->pri_primary->tp_socket = s;

  if (tport_bind_socket(s, ai, return_culprit) < 0)
    return -1;

  tport_set_tos(s, ai, pri->pri_params->tpp_tos);

#if TPORT_UDP_PKTINFO
  if (SU_SOCKADDR_INADDR_ANY((su_sockaddr_t *)ai->ai_addr) || pri->pri_nifs) {
    /* The socket serves every local address: ask for the address each
     * datagram was sent to, so that it can be answered from that address.
     * A transport that listens on some interfaces only asks for it too: it
     * comes with the interface the datagram arrived on. */
    int error = 0;

    if (ai->ai_family == AF_INET)
      error = setsockopt(s, IPPROTO_IP, IP_PKTINFO, (void *)&one, sizeof one);
#if SU_HAVE_IN6
    else if (ai->ai_family == AF_INET6)
      error = setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO,
			 (void *)&one, sizeof one);
#endif
    if (error < 0)
      SU_DEBUG_3(("setsockopt(%s): %s\n",
		  ai->ai_family == AF_INET ? "IP_PKTINFO" : "IPV6_RECVPKTINFO",
		  su_strerror(su_errno())));
    else
      pri->pri_pktinfo = 1;
  }
#endif

#if HAVE_IP_ADD_MEMBERSHIP
  if (ai->ai_family == AF_INET &&
      IN_MULTICAST(ntohl(su->su_sin.sin_addr.s_addr))) {
    /* Try to join to the multicast group */
    /* Bind to the SIP address like
       <sip:88.77.66.55:5060;maddr=224.0.1.75;transport=udp> */
    struct ip_mreq imr[1];
    struct in_addr iface;

    memset(imr, 0, sizeof imr);

    imr->imr_multiaddr = su->su_sin.sin_addr;

    if (host_is_ip4_address(tpn->tpn_canon) &&
	su_inet_pton(AF_INET, tpn->tpn_canon, &iface) > 0) {
      imr->imr_interface = iface;
    }

    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, imr, (sizeof imr)) < 0) {
      SU_DEBUG_3(("setsockopt(%s): %s\n",
		  "IP_ADD_MEMBERSHIP", su_strerror(su_errno())));
    }
#if HAVE_IP_MULTICAST_LOOP
    else
      if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof one) < 0) {
	SU_DEBUG_3(("setsockopt(%s): %s\n",
		    "IP_MULTICAST_LOOP", su_strerror(su_errno())));
    }
#endif
  }
#endif

#if HAVE_IP_MTU_DISCOVER
  {
    /* Turn off DF flag on Linux */
    int dont = IP_PMTUDISC_DONT;
    if (setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &dont, sizeof(dont)) < 0) {
	SU_DEBUG_3(("setsockopt(%s): %s\n",
		    "IP_MTU_DISCOVER", su_strerror(su_errno())));
    }
  }
#endif

#if HAVE_IP_RECVERR
  if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
    if (setsockopt(s, IPPROTO_IP, IP_RECVERR, &one, sizeof(one)) < 0) {
      if (ai->ai_family == AF_INET)
	SU_DEBUG_3(("setsockopt(%s): %s\n",
		    "IPVRECVERR", su_strerror(su_errno())));
    }
    events |= SU_WAIT_ERR;
  }
#endif
#if HAVE_IPV6_RECVERR
  if (ai->ai_family == AF_INET6) {
    if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one)) < 0)
      SU_DEBUG_3(("setsockopt(IPV6_RECVERR): %s\n", su_strerror(su_errno())));
    events |= SU_WAIT_ERR;
  }
#endif

  tl_gets(tags,
	  TPTAG_UDP_RMEM_REF(rmem),
	  TPTAG_UDP_WMEM_REF(wmem),
	  TAG_END());

  if (rmem != 0 &&
#if HAVE_SO_RCVBUFFORCE
      setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, (void *)&rmem, sizeof rmem) < 0 &&
#endif
      setsockopt(s, SOL_SOCKET, SO_RCVBUF, (void *)&rmem, sizeof rmem) < 0) {
    SU_DEBUG_3(("setsockopt(SO_RCVBUF): %s\n",
		su_strerror(su_errno())));
  }

  if (wmem != 0 &&
#if HAVE_SO_SNDBUFFORCE
      setsockopt(s, SOL_SOCKET, SO_SNDBUFFORCE, (void *)&wmem, sizeof wmem) < 0 &&
#endif
      setsockopt(s, SOL_SOCKET, SO_SNDBUF, (void *)&wmem, sizeof wmem) < 0) {
    SU_DEBUG_3(("setsockopt(SO_SNDBUF): %s\n",
		su_strerror(su_errno())));
  }

  pri->pri_primary->tp_events = events;

  tport_init_compressor(pri->pri_primary, tpn->tpn_comp, tags);

  tport_check_trunc(pri->pri_primary, ai);

#if HAVE_SOFIA_STUN
  tport_stun_server_add_socket(pri->pri_primary);
#endif

  return 0;
}

static
int tport_udp_init_client(tport_primary_t *pri,
			  tp_name_t tpn[1],
			  su_addrinfo_t *ai,
			  tagi_t const *tags,
			  char const **return_culprit)
{
  pri->pri_primary->tp_conn_orient = 1;
  return 0;
}

/** Runtime test making sure MSG_TRUNC work as expected */
static void tport_check_trunc(tport_t *tp, su_addrinfo_t *ai)
{
#if HAVE_MSG_TRUNC
  ssize_t n;
  char buffer[2];
  su_sockaddr_t su[1];
  socklen_t sulen = sizeof su;

  n = su_sendto(tp->tp_socket,
		"TEST", 4, 0,
		(void *)ai->ai_addr, (socklen_t)ai->ai_addrlen);

  if (n != 4)
    return;

  n = su_recvfrom(tp->tp_socket, buffer, sizeof buffer, MSG_TRUNC,
		  (void *)&su, &sulen);

  if (n > (ssize_t)sizeof buffer) {
    tp->tp_trunc = 1;
    return;
  }

  /* XXX - check that su and tp->tp_addrinfo->ai_addr match */
#endif
}

#if TPORT_UDP_PKTINFO
/** Receive a datagram together with the local address it was sent to.
 *
 * @param local    the local address, family 0 if the kernel did not tell it
 * @param ifindex  the index of the interface it arrived on, 0 if not told
 */
static
ssize_t tport_udp_recv_at(tport_t const *self,
			  msg_iovec_t iov[], size_t iovlen,
			  su_sockaddr_t *from, socklen_t *fromlen,
			  su_sockaddr_t local[1],
			  unsigned *ifindex)
{
  struct msghdr hdr[1] = {{0}};
  struct cmsghdr *c;
  union {
    struct cmsghdr align;
    char data[256];
  } control;
  ssize_t n;

  hdr->msg_name = (void *)from;
  hdr->msg_namelen = *fromlen;
  hdr->msg_iov = (struct iovec *)iov;
  hdr->msg_iovlen = iovlen;
  hdr->msg_control = control.data;
  hdr->msg_controllen = sizeof control.data;

  n = recvmsg(self->tp_socket, hdr, 0);
  if (n < 0)
    return n;

  *fromlen = hdr->msg_namelen;
  *ifindex = 0;

  for (c = CMSG_FIRSTHDR(hdr); c; c = CMSG_NXTHDR(hdr, c)) {
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
      struct in_pktinfo info;

      memcpy(&info, CMSG_DATA(c), sizeof info);
      *ifindex = (unsigned)info.ipi_ifindex;

      local->su_len = sizeof local->su_sin;
      local->su_family = AF_INET;
      /* ipi_spec_dst is the address to answer from: the destination of the
       * datagram, or a unicast address of ours when that was a broadcast or
       * multicast address. */
      if (info.ipi_spec_dst.s_addr)
	local->su_sin.sin_addr = info.ipi_spec_dst;
      else
	local->su_sin.sin_addr = info.ipi_addr;
    }
#if SU_HAVE_IN6
    else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
      struct in6_pktinfo info;

      memcpy(&info, CMSG_DATA(c), sizeof info);
      *ifindex = (unsigned)info.ipi6_ifindex;

      local->su_len = sizeof local->su_sin6;
      local->su_family = AF_INET6;
      local->su_sin6.sin6_addr = info.ipi6_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&info.ipi6_addr))
	local->su_sin6.sin6_scope_id = info.ipi6_ifindex;
    }
#endif
  }

  return n;
}

/** Send a datagram from the local address @a local.
 *
 * The interface index is left to the routing table: only the source address
 * is chosen.
 */
static
ssize_t tport_udp_send_from(tport_t const *self,
			    msg_iovec_t iov[], size_t iovused,
			    su_sockaddr_t const *to, socklen_t tolen,
			    su_sockaddr_t const *local)
{
  struct msghdr hdr[1] = {{0}};
  struct cmsghdr *c;
  union {
    struct cmsghdr align;
    char data[CMSG_SPACE(sizeof(struct in_pktinfo)) +
	      CMSG_SPACE(sizeof(struct in6_pktinfo))];
  } control;
  ssize_t n;
  int sanity = 100;

  memset(&control, 0, sizeof control);

  hdr->msg_name = (void *)to;
  hdr->msg_namelen = tolen;
  hdr->msg_iov = (struct iovec *)iov;
  hdr->msg_iovlen = iovused;
  hdr->msg_control = control.data;

  if (local->su_family == AF_INET) {
    struct in_pktinfo info;

    memset(&info, 0, sizeof info);
    info.ipi_spec_dst = local->su_sin.sin_addr;

    hdr->msg_controllen = CMSG_SPACE(sizeof info);
    c = CMSG_FIRSTHDR(hdr);
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = IP_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof info);
    memcpy(CMSG_DATA(c), &info, sizeof info);
  }
#if SU_HAVE_IN6
  else if (local->su_family == AF_INET6) {
    struct in6_pktinfo info;

    memset(&info, 0, sizeof info);
    info.ipi6_addr = local->su_sin6.sin6_addr;
    if (IN6_IS_ADDR_LINKLOCAL(&info.ipi6_addr))
      info.ipi6_ifindex = local->su_sin6.sin6_scope_id;

    hdr->msg_controllen = CMSG_SPACE(sizeof info);
    c = CMSG_FIRSTHDR(hdr);
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof info);
    memcpy(CMSG_DATA(c), &info, sizeof info);
  }
#endif
  else {
    su_seterrno(EAFNOSUPPORT);
    return -1;
  }

  /* As su_vsend() does */
  do {
    n = sendmsg(self->tp_socket, hdr, MSG_NOSIGNAL);
    if (n == -1 && errno == EAGAIN)
      sched_yield();
  } while (--sanity > 0 && n == -1 && (errno == EAGAIN || errno == EINTR));

  return n;
}
#endif

/** Receive datagram.
 *
 * @retval -1 error
 * @retval 0  end-of-stream
 * @retval 1  normal receive (should never happen)
 * @retval 2  incomplete recv, call me again (should never happen)
 * @retval 3  STUN keepalive, ignore
 */
int tport_recv_dgram(tport_t *self)
{
  msg_t *msg;
  ssize_t n, veclen, N;
  su_addrinfo_t *ai;
  su_sockaddr_t *from;
  socklen_t fromlen;
  msg_iovec_t iovec[msg_n_fragments] = {{ 0 }};
  uint8_t sample[1];

  /* Simulate packet loss */
  if (self->tp_params->tpp_drop &&
      (unsigned)su_randint(0, 1000) < self->tp_params->tpp_drop) {
    su_recv(self->tp_socket, sample, 1, 0);
    SU_DEBUG_3(("tport(%p): simulated packet loss!\n", (void *)self));
    return 0;
  }

  assert(self->tp_msg == NULL);

#if nomore
  /* We used to resize the buffer, but it fragments the memory */
  N = 65535;
#else
  N = (ssize_t)su_getmsgsize(self->tp_socket);
  if (N == -1) {
    int err = su_errno();
    SU_DEBUG_1(("%s(%p): su_getmsgsize(): %s (%d)\n", __func__, (void *)self,
		su_strerror(err), err));
    return -1;
  }
  if (N == 0) {
    su_recv(self->tp_socket, sample, 1, 0);
    SU_DEBUG_3(("tport(%p): zero length packet", (void *)self));
    return 0;
  }
#endif

  veclen = tport_recv_iovec(self, &self->tp_msg, iovec, N, 1);
  if (veclen == -1)
    return -1;

  msg = self->tp_msg;

  ai = msg_addrinfo(msg);
  from = (su_sockaddr_t *)ai->ai_addr, fromlen = (socklen_t)(ai->ai_addrlen);

#if TPORT_UDP_PKTINFO
  if (self->tp_pri->pri_pktinfo) {
    su_sockaddr_t local[1];
    unsigned ifindex = 0;

    memset(local, 0, sizeof local);
    n = tport_udp_recv_at(self, iovec, veclen, from, &fromlen, local,
			  &ifindex);
    if (n != SOCKET_ERROR) {
      if (!tport_on_interface(self->tp_pri, ifindex)) {
	/* The transport listens on some interfaces only, and this datagram
	 * arrived on another one */
	SU_DEBUG_7(("%s(%p): datagram dropped: "
		    "it did not arrive on an interface of this transport\n",
		    __func__, (void *)self));
	msg_destroy(msg), self->tp_msg = NULL;
	return 0;
      }
      if (self->tp_pri->pri_wildcard)
	msg_set_local_address(msg, local, SU_SOCKADDR_SIZE(local));
    }
  }
  else
#endif
  n = su_vrecv(self->tp_socket, iovec, veclen, 0, from, &fromlen);

  ai->ai_addrlen = fromlen;

  if (n == SOCKET_ERROR) {
    int error = su_errno();
    msg_destroy(msg); self->tp_msg = NULL;
    su_seterrno(error);

    if (su_is_blocking(error))
      return 0;
    else
      return -1;
  }
  else if (n <= 1) {
    SU_DEBUG_1(("%s(%p): runt of "MOD_ZD" bytes\n",
		"tport_recv_dgram", (void *)self, n));
    msg_destroy(msg), self->tp_msg = NULL;
    return 0;
  }

  tport_recv_bytes(self, n, n);

  SU_CANONIZE_SOCKADDR(from);

  if (self->tp_master->mr_dump_file)
    tport_dump_iovec(self, msg, n, iovec, veclen, "recv", "from");

  *sample = *((uint8_t *)iovec[0].mv_base);

  /* Commit received data into buffer. This may relocate iovec contents */
  msg_recv_commit(msg, n, 1);

  if ((sample[0] & 0xf8) == 0xf8)
    /* SigComp */
    return tport_recv_comp_dgram(self, self->tp_comp, &self->tp_msg,
				 from, fromlen);
#if HAVE_SOFIA_STUN
  else if (sample[0] == 0 || sample[0] == 1)
    /* STUN request or response */
    return tport_recv_stun_dgram(self, &self->tp_msg, from, fromlen);
#endif
  else
    return 0;
}

/** Send using su_vsend(). Map IPv4 addresses as IPv6 addresses, if needed. */
ssize_t tport_send_dgram(tport_t const *self, msg_t *msg,
			 msg_iovec_t iov[],
			 size_t iovused)
{
  su_sockaddr_t su[1];
  socklen_t sulen = sizeof su;

  if (tport_is_connection_oriented(self))
    return su_vsend(self->tp_socket, iov, iovused, MSG_NOSIGNAL, NULL, 0);

  msg_get_address(msg, su, &sulen);

#if SU_HAVE_IN6 && defined(IN6_INADDR_TO_V4MAPPED)
  if (su->su_family == AF_INET && self->tp_addrinfo->ai_family == AF_INET6) {
    su_sockaddr_t su0[1];

    memset(su0, 0, sizeof su0);

    su0->su_family = self->tp_addrinfo->ai_family;
    su0->su_port = su->su_port;

    IN6_INADDR_TO_V4MAPPED(&su->su_sin.sin_addr, &su0->su_sin6.sin6_addr);

    memcpy(su, su0, sulen = sizeof(su0->su_sin6));
  }
#endif

  su_soerror(self->tp_socket); /* XXX - we *still* have a race condition */

#if TPORT_UDP_PKTINFO
  if (self->tp_pri->pri_wildcard) {
    su_sockaddr_t local[1];

    if (msg_get_local_address(msg, local, NULL) == 0 &&
	local->su_family == self->tp_addrinfo->ai_family) {
      ssize_t n = tport_udp_send_from(self, iov, iovused, su, sulen, local);
      int error;

      if (n != -1)
	return n;

      error = su_errno();
      if (error != ENETUNREACH && error != EINVAL && error != EADDRNOTAVAIL)
	return -1;

      /* The kernel refuses the source address: it may not be an address of
       * this host any longer.  If the datagram goes out without it, that
       * was the reason, and the routing table chooses from now on. */
      n = su_vsend(self->tp_socket, iov, iovused, MSG_NOSIGNAL, su, sulen);
      if (n != -1) {
	char addr[SU_ADDRSIZE];
	SU_DEBUG_5(("%s(%p): cannot send from %s (%s), "
		    "sent from the address the route picks\n",
		    __func__, (void *)self,
		    su_inet_ntop(local->su_family, SU_ADDR(local),
				 addr, sizeof addr),
		    su_strerror(error)));
	msg_set_local_address(msg, NULL, 0);
	tport_flow_forget(self, su);
      }
      return n;
    }
  }
#endif

  return su_vsend(self->tp_socket, iov, iovused, MSG_NOSIGNAL, su, sulen);
}


#if !HAVE_IP_RECVERR && !HAVE_IPV6_RECVERR

/** Process UDP error event. */
int tport_udp_error(tport_t const *self, su_sockaddr_t name[1])
{
  if (tport_is_connection_oriented(self))
    name[0] = self->tp_addr[0];
  return su_soerror(self->tp_socket);
}

#else

/** Process UDP error event. */
int tport_udp_error(tport_t const *self, su_sockaddr_t name[1])
{
  struct cmsghdr *c;
  struct sock_extended_err *ee;
  su_sockaddr_t *from;
  char control[512];
  char errmsg[64 + 768];
  struct iovec iov[1];
  struct msghdr msg[1] = {{ 0 }};
  int n;

  msg->msg_name = name, msg->msg_namelen = sizeof(*name);
  msg->msg_iov = iov, msg->msg_iovlen = 1;
  iov->iov_base = errmsg, iov->iov_len = sizeof(errmsg);
  msg->msg_control = control, msg->msg_controllen = sizeof(control);

  n = recvmsg(self->tp_socket, msg, MSG_ERRQUEUE);

  if (n < 0) {
    int err = su_errno();
    if (!su_is_blocking(err))
      SU_DEBUG_1(("%s: recvmsg: %s\n", __func__, su_strerror(err)));
    return 0;
  }

  if ((msg->msg_flags & MSG_ERRQUEUE) != MSG_ERRQUEUE) {
    SU_DEBUG_1(("%s: recvmsg: no errqueue\n", __func__));
    return 0;
  }

  if (msg->msg_flags & MSG_CTRUNC) {
    SU_DEBUG_1(("%s: extended error was truncated\n", __func__));
    return 0;
  }

  if (msg->msg_flags & MSG_TRUNC) {
    /* ICMP message may contain original message... */
    SU_DEBUG_3(("%s: icmp(6) message was truncated (at %d)\n", __func__, n));
  }

  /* Go through the ancillary data */
  for (c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
    if (0
#if HAVE_IP_RECVERR
	|| (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR)
#endif
#if HAVE_IPV6_RECVERR
	|| (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_RECVERR)
#endif
	) {
      char info[128];
      char const *origin;

      ee = (struct sock_extended_err *)CMSG_DATA(c);
      from = (su_sockaddr_t *)SO_EE_OFFENDER(ee);
      info[0] = '\0';

      switch (ee->ee_origin) {
      case SO_EE_ORIGIN_LOCAL:
	origin = "local";
	break;
      case SO_EE_ORIGIN_ICMP:
	origin = "icmp";
	snprintf(info, sizeof(info), " type=%u code=%u",
		 ee->ee_type, ee->ee_code);
	break;
      case SO_EE_ORIGIN_ICMP6:
	origin = "icmp6";
	snprintf(info, sizeof(info), " type=%u code=%u",
		ee->ee_type, ee->ee_code);
	break;
      case SO_EE_ORIGIN_NONE:
	origin = "none";
	break;
      default:
	origin = "unknown";
	break;
      }

      if (ee->ee_info)
	snprintf(info + strlen(info), sizeof(info) - strlen(info),
		 " info=%08x", ee->ee_info);

      SU_DEBUG_3(("%s: %s (%d) [%s%s]\n",
		  __func__, su_strerror(ee->ee_errno), ee->ee_errno,
		  origin, info));
      if (from->su_family != AF_UNSPEC)
	SU_DEBUG_3(("\treported by [%s]:%u\n",
		    su_inet_ntop(from->su_family, SU_ADDR(from),
				 info, sizeof(info)),
		    ntohs(from->su_port)));

      if (msg->msg_namelen == 0)
	name->su_family = AF_UNSPEC;

      SU_CANONIZE_SOCKADDR(name);

      return ee->ee_errno;
    }
  }

  return 0;
}
#endif
