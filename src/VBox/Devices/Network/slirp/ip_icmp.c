/* $Id: ip_icmp.c 53309 2014-11-12 03:47:13Z vboxsync $ */
/** @file
 * NAT - IP/ICMP handling.
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*
 * This code is based on:
 *
 * Copyright (c) 1982, 1986, 1988, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      @(#)ip_icmp.c   8.2 (Berkeley) 1/4/94
 * ip_icmp.c,v 1.7 1995/05/30 08:09:42 rgrimes Exp
 */

#include "slirp.h"
#include "ip_icmp.h"
#ifdef RT_OS_WINDOWS
# include <Icmpapi.h>
# include <Iphlpapi.h>
# include <iprt/ldr.h>
#endif

/* The message sent when emulating PING */
/* Be nice and tell them it's just a psuedo-ping packet */
static const char icmp_ping_msg[] = "This is a psuedo-PING packet used by Slirp to emulate ICMP ECHO-REQUEST packets.\n";

/* list of actions for icmp_error() on RX of an icmp message */
static const int icmp_flush[19] =
{
/* ECHO REPLY (0) */         0,
                             1,
                             1,
/* DEST UNREACH (3) */       1,
/* SOURCE QUENCH (4)*/       1,
/* REDIRECT (5) */           1,
                             1,
                             1,
/* ECHO (8) */               0,
/* ROUTERADVERT (9) */       1,
/* ROUTERSOLICIT (10) */     1,
/* TIME EXCEEDED (11) */     1,
/* PARAMETER PROBLEM (12) */ 1,
/* TIMESTAMP (13) */         0,
/* TIMESTAMP REPLY (14) */   0,
/* INFO (15) */              0,
/* INFO REPLY (16) */        0,
/* ADDR MASK (17) */         0,
/* ADDR MASK REPLY (18) */   0
};

static void icmp_cache_clean(PNATState pData, int iEntries);

int
icmp_init(PNATState pData, int iIcmpCacheLimit)
{
    pData->icmp_socket.so_type = IPPROTO_ICMP;
    pData->icmp_socket.so_state = SS_ISFCONNECTED;
    if (iIcmpCacheLimit < 0)
    {
        LogRel(("NAT: iIcmpCacheLimit is invalid %d, will be alter to default value 100\n", iIcmpCacheLimit));
        iIcmpCacheLimit = 100;
    }
    pData->iIcmpCacheLimit = iIcmpCacheLimit;
#ifndef RT_OS_WINDOWS
# ifndef RT_OS_DARWIN
    pData->icmp_socket.s = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
# else /* !RT_OS_DARWIN */
    pData->icmp_socket.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
# endif /* RT_OS_DARWIN */
    if (pData->icmp_socket.s == -1)
    {
        int rc = RTErrConvertFromErrno(errno);
        LogRel(("NAT: ICMP/ping not available (could not open ICMP socket, error %Rrc)\n", rc));
        return 1;
    }
    fd_nonblock(pData->icmp_socket.s);
    NSOCK_INC();

#else /* RT_OS_WINDOWS */
    if (icmpwin_init(pData) != 0)
        return 1;
#endif /* RT_OS_WINDOWS */

    LIST_INIT(&pData->icmp_msg_head);
    return 0;
}

/**
 * Cleans ICMP cache.
 */
void
icmp_finit(PNATState pData)
{
    icmp_cache_clean(pData, -1);
#ifdef RT_OS_WINDOWS
    icmpwin_finit(pData);
#else
    closesocket(pData->icmp_socket.s);
#endif
}

/*
 * ip here is ip header + 64bytes readed from ICMP packet
 */
struct icmp_msg *
icmp_find_original_mbuf(PNATState pData, struct ip *ip)
{
    struct mbuf *m0;
    struct ip *ip0;
    struct icmp *icp, *icp0;
    struct icmp_msg *icm = NULL;
    int found = 0;
    struct udphdr *udp;
    struct tcphdr *tcp;
    struct socket *head_socket = NULL;
    struct socket *last_socket = NULL;
    struct socket *so = NULL;
    struct in_addr faddr;
    u_short lport, fport;

    faddr.s_addr = ~0;

    lport = ~0;
    fport = ~0;


    LogFlowFunc(("ENTER: ip->ip_p:%d\n", ip->ip_p));
    switch (ip->ip_p)
    {
        case IPPROTO_ICMP:
            icp = (struct icmp *)((char *)ip + (ip->ip_hl << 2));
            LIST_FOREACH(icm, &pData->icmp_msg_head, im_list)
            {
                m0 = icm->im_m;
                ip0 = mtod(m0, struct ip *);
                if (ip0->ip_p != IPPROTO_ICMP)
                {
                    /* try next item */
                    continue;
                }
                icp0 = (struct icmp *)((char *)ip0 + (ip0->ip_hl << 2));
                /*
                 * IP could pointer to ICMP_REPLY datagram (1)
                 * or pointer IP header in ICMP payload in case of
                 * ICMP_TIMXCEED or ICMP_UNREACH (2)
                 *
                 * if (1) and then ICMP (type should be ICMP_ECHOREPLY) and we need check that
                 * IP.IP_SRC == IP0.IP_DST received datagramm comes from destination.
                 *
                 * if (2) then check that payload ICMP has got type ICMP_ECHO and
                 * IP.IP_DST == IP0.IP_DST destination of returned datagram is the same as
                 * one was sent.
                 */
                if (  (   (icp->icmp_type != ICMP_ECHO && ip->ip_src.s_addr == ip0->ip_dst.s_addr)
                       || (icp->icmp_type == ICMP_ECHO && ip->ip_dst.s_addr == ip0->ip_dst.s_addr))
                    && icp->icmp_id == icp0->icmp_id
                    && icp->icmp_seq == icp0->icmp_seq)
                {
                    found = 1;
                    Log(("Have found %R[natsock]\n", icm->im_so));
                    break;
                }
                Log(("Have found nothing\n"));
            }
            break;

        /*
         *  for TCP and UDP logic little bit reverted, we try to find the HOST socket
         *  from which the IP package has been sent.
         */
        case IPPROTO_UDP:
            head_socket = &udb;
            udp = (struct udphdr *)((char *)ip + (ip->ip_hl << 2));
            faddr.s_addr = ip->ip_dst.s_addr;
            fport = udp->uh_dport;
            lport = udp->uh_sport;
            last_socket = udp_last_so;
            /* fall through */

        case IPPROTO_TCP:
            if (head_socket == NULL)
            {
                tcp = (struct tcphdr *)((char *)ip + (ip->ip_hl << 2));
                head_socket = &tcb; /* head_socket could be initialized with udb*/
                faddr.s_addr = ip->ip_dst.s_addr;
                fport = tcp->th_dport;
                lport = tcp->th_sport;
                last_socket = tcp_last_so;
            }
            /* check last socket first */
            if (   last_socket->so_faddr.s_addr == faddr.s_addr
                && last_socket->so_fport == fport
                && last_socket->so_hlport == lport)
            {
                found = 1;
                so = last_socket;
                goto sofound;
            }
            for (so = head_socket->so_prev; so != head_socket; so = so->so_prev)
            {
                /* Should be reaplaced by hash here */
                Log(("trying:%R[natsock] against %RTnaipv4:%d lport=%d hlport=%d\n", so, &faddr, fport, lport, so->so_hlport));
                if (   so->so_faddr.s_addr == faddr.s_addr
                    && so->so_fport == fport
                    && so->so_hlport == lport)
                {
                    found = 1;
                    break;
                }
            }
            break;

        default:
            Log(("NAT:ICMP: unsupported protocol(%d)\n", ip->ip_p));
    }
    sofound:
    if (found == 1 && icm == NULL)
    {
        if (so->so_state == SS_NOFDREF)
        {
            /* socket is shutdowning we've already sent ICMP on it.*/
            Log(("NAT: Received icmp on shutdowning socket (probably corresponding ICMP socket has been already sent)\n"));
            return NULL;
        }
        icm = RTMemAlloc(sizeof(struct icmp_msg));
        icm->im_m = so->so_m;
        icm->im_so = so;
        found = 1;
        Log(("hit:%R[natsock]\n", so));
        /*XXX: this storage not very long,
         * better add flag if it should removed from lis
         */
        LIST_INSERT_HEAD(&pData->icmp_msg_head, icm, im_list);
        pData->cIcmpCacheSize++;
        if (pData->cIcmpCacheSize > pData->iIcmpCacheLimit)
            icmp_cache_clean(pData, pData->iIcmpCacheLimit/2);
        LogFlowFunc(("LEAVE: icm:%p\n", icm));
        return (icm);
    }
    if (found == 1)
    {
        LogFlowFunc(("LEAVE: icm:%p\n", icm));
        return icm;
    }

    LogFlowFunc(("LEAVE: NULL\n"));
    return NULL;
}

/**
 * iEntries how many entries to leave, if iEntries < 0, clean all
 */
static void icmp_cache_clean(PNATState pData, int iEntries)
{
    int iIcmpCount = 0;
    struct icmp_msg *icm = NULL;
    LogFlowFunc(("iEntries:%d\n", iEntries));
    if (iEntries > pData->cIcmpCacheSize)
    {
        LogFlowFuncLeave();
        return;
    }
    while(!LIST_EMPTY(&pData->icmp_msg_head))
    {
        icm = LIST_FIRST(&pData->icmp_msg_head);
        if (    iEntries > 0
            &&  iIcmpCount < iEntries)
        {
            iIcmpCount++;
            continue;
        }

        LIST_REMOVE(icm, im_list);
        if (icm->im_m)
        {
            pData->cIcmpCacheSize--;
            m_freem(pData, icm->im_m);
        }
        RTMemFree(icm);
    }
    LogFlowFuncLeave();
}

static int
icmp_attach(PNATState pData, struct mbuf *m)
{
    struct icmp_msg *icm;
    struct ip *ip;
    ip = mtod(m, struct ip *);
    Assert(ip->ip_p == IPPROTO_ICMP);
    icm = RTMemAlloc(sizeof(struct icmp_msg));
    icm->im_m = m;
    icm->im_so = m->m_so;
    LIST_INSERT_HEAD(&pData->icmp_msg_head, icm, im_list);
    pData->cIcmpCacheSize++;
    if (pData->cIcmpCacheSize > pData->iIcmpCacheLimit)
        icmp_cache_clean(pData, pData->iIcmpCacheLimit/2);
    return 0;
}

/*
 * Process a received ICMP message.
 */
void
icmp_input(PNATState pData, struct mbuf *m, int hlen)
{
    register struct ip *ip = mtod(m, struct ip *);
    int icmplen = ip->ip_len;
    uint8_t icmp_type;
    void *icp_buf = NULL;
    int status;
    uint32_t dst;
#if !defined(RT_OS_WINDOWS)
    int ttl;
#endif

    /* int code; */

    LogFlowFunc(("ENTER: m = %lx, m_len = %d\n", (long)m, m ? m->m_len : 0));

    icmpstat.icps_received++;

    /*
     * Locate icmp structure in mbuf, and check
     * that its not corrupted and of at least minimum length.
     */
    if (icmplen < ICMP_MINLEN)
    {
        /* min 8 bytes payload */
        icmpstat.icps_tooshort++;
        goto end_error_free_m;
    }

    m->m_len -= hlen;
    m->m_data += hlen;

    if (cksum(m, icmplen))
    {
        icmpstat.icps_checksum++;
        goto end_error_free_m;
    }

    /* are we guaranteed to have ICMP header in first mbuf? be safe. */
    m_copydata(m, 0, sizeof(icmp_type), (caddr_t)&icmp_type);

    m->m_len += hlen;
    m->m_data -= hlen;

    /* icmpstat.icps_inhist[icp->icmp_type]++; */
    /* code = icp->icmp_code; */

    LogFlow(("icmp_type = %d\n", icmp_type));
    switch (icmp_type)
    {
        case ICMP_ECHO:
            ip->ip_len += hlen;              /* since ip_input subtracts this */
            dst = ip->ip_dst.s_addr;
            if (   CTL_CHECK(dst, CTL_ALIAS)
                || CTL_CHECK(dst, CTL_DNS)
                || CTL_CHECK(dst, CTL_TFTP))
            {
                uint8_t echo_reply = ICMP_ECHOREPLY;
                m_copyback(pData, m, hlen + RT_OFFSETOF(struct icmp, icmp_type),
                           sizeof(echo_reply), (caddr_t)&echo_reply);
                ip->ip_dst.s_addr = ip->ip_src.s_addr;
                ip->ip_src.s_addr = dst;
                icmp_reflect(pData, m);
                goto done;
            }
            else
            {
                struct icmp *icp;
                struct sockaddr_in addr;
#ifdef RT_OS_WINDOWS
                IP_OPTION_INFORMATION ipopt;
                int error;
#endif
                addr.sin_family = AF_INET;
                if ((ip->ip_dst.s_addr & RT_H2N_U32(pData->netmask)) == pData->special_addr.s_addr)
                {
                    /* It's an alias */
                    switch (RT_N2H_U32(ip->ip_dst.s_addr) & ~pData->netmask)
                    {
                        case CTL_DNS:
                        case CTL_ALIAS:
                        default:
                            addr.sin_addr = loopback_addr;
                            break;
                    }
                }
                else
                    addr.sin_addr.s_addr = ip->ip_dst.s_addr;

                if (m->m_next)
                {
                    icp_buf = RTMemAlloc(icmplen);
                    if (!icp_buf)
                    {
                        Log(("NAT: not enought memory to allocate the buffer\n"));
                        goto end_error_free_m;
                    }
                    m_copydata(m, hlen, icmplen, icp_buf);
                    icp = (struct icmp *)icp_buf;
                }
                else
                    icp = (struct icmp *)(mtod(m, char *) + hlen);

#ifndef RT_OS_WINDOWS
                if (pData->icmp_socket.s != -1)
                {
                    ssize_t rc;
                    static bool fIcmpSocketErrorReported;
                    ttl = ip->ip_ttl;
                    Log(("NAT/ICMP: try to set TTL(%d)\n", ttl));
                    status = setsockopt(pData->icmp_socket.s, IPPROTO_IP, IP_TTL,
                                        (void *)&ttl, sizeof(ttl));
                    if (status < 0)
                        Log(("NAT: Error (%s) occurred while setting TTL attribute of IP packet\n",
                                strerror(errno)));
                    rc = sendto(pData->icmp_socket.s, icp, icmplen, 0,
                              (struct sockaddr *)&addr, sizeof(addr));
                    if (rc >= 0)
                    {
                        m->m_so = &pData->icmp_socket;
                        icmp_attach(pData, m);
                        /* don't let m_freem at the end free atached buffer */
                        goto done;
                    }


                    if (!fIcmpSocketErrorReported)
                    {
                        LogRel(("icmp_input udp sendto tx errno = %d (%s)\n",
                                errno, strerror(errno)));
                        fIcmpSocketErrorReported = true;
                    }
                    icmp_error(pData, m, ICMP_UNREACH, ICMP_UNREACH_NET, 0, strerror(errno));
                }
#else /* RT_OS_WINDOWS */
                pData->icmp_socket.so_laddr.s_addr = ip->ip_src.s_addr; /* XXX: hack*/
                pData->icmp_socket.so_icmp_id = icp->icmp_id;
                pData->icmp_socket.so_icmp_seq = icp->icmp_seq;
                memset(&ipopt, 0, sizeof(IP_OPTION_INFORMATION));
                ipopt.Ttl = ip->ip_ttl;
                status = IcmpSendEcho2(pData->icmp_socket.sh /*=handle*/,
                                       pData->phEvents[VBOX_ICMP_EVENT_INDEX] /*=Event*/,
                                       NULL /*=ApcRoutine*/,
                                       NULL /*=ApcContext*/,
                                       addr.sin_addr.s_addr /*=DestinationAddress*/,
                                       icp->icmp_data /*=RequestData*/,
                                       icmplen - ICMP_MINLEN /*=RequestSize*/,
                                       &ipopt /*=RequestOptions*/,
                                       pData->pvIcmpBuffer /*=ReplyBuffer*/,
                                       pData->cbIcmpBuffer /*=ReplySize*/,
                                       1 /*=Timeout in ms*/);
                error = GetLastError();
                if (   status != 0
                    || error == ERROR_IO_PENDING)
                {
                    /* no error! */
                    m->m_so = &pData->icmp_socket;
                    icmp_attach(pData, m);
                    /* don't let m_freem at the end free atached buffer */
                    goto done;
                }
                Log(("NAT: Error (%d) occurred while sending ICMP (", error));
                switch (error)
                {
                    case ERROR_INVALID_PARAMETER:
                        Log(("icmp_socket:%lx is invalid)\n", pData->icmp_socket.s));
                        break;
                    case ERROR_NOT_SUPPORTED:
                        Log(("operation is unsupported)\n"));
                        break;
                    case ERROR_NOT_ENOUGH_MEMORY:
                        Log(("OOM!!!)\n"));
                        break;
                    case IP_BUF_TOO_SMALL:
                        Log(("Buffer too small)\n"));
                        break;
                    default:
                        Log(("Other error!!!)\n"));
                        break;
                }
#endif /* RT_OS_WINDOWS */
            } /* if ip->ip_dst.s_addr == alias_addr.s_addr */
            break;
        case ICMP_UNREACH:
        case ICMP_TIMXCEED:
            /* @todo(vvl): both up cases comes from guest,
             *  indeed right solution would be find the socket
             *  corresponding to ICMP data and close it.
             */
        case ICMP_PARAMPROB:
        case ICMP_SOURCEQUENCH:
        case ICMP_TSTAMP:
        case ICMP_MASKREQ:
        case ICMP_REDIRECT:
            icmpstat.icps_notsupp++;
            break;

        default:
            icmpstat.icps_badtype++;
    } /* switch */

end_error_free_m:
    m_freem(pData, m);

done:
    if (icp_buf)
        RTMemFree(icp_buf);
}


/**
 * Send an ICMP message in response to a situation
 *
 * RFC 1122: 3.2.2 MUST send at least the IP header and 8 bytes of header. MAY send more (we do).
 *                 MUST NOT change this header information.
 *                 MUST NOT reply to a multicast/broadcast IP address.
 *                 MUST NOT reply to a multicast/broadcast MAC address.
 *                 MUST reply to only the first fragment.
 *
 * Send ICMP_UNREACH back to the source regarding msrc.
 * It is reported as the bad ip packet.  The header should
 * be fully correct and in host byte order.
 * ICMP fragmentation is illegal.
 *
 * @note: implementation note: MSIZE is 256 bytes (minimal buffer).
 * We always truncate original payload to 8 bytes required by the RFC,
 * so the largest possible datagram is 14 (ethernet) + 20 (ip) +
 * 8 (icmp) + 60 (max original ip with options) + 8 (original payload)
 * = 110 bytes which fits into sinlge mbuf.
 *
 * @note This function will free msrc!
 */

void icmp_error(PNATState pData, struct mbuf *msrc, u_char type, u_char code, int minsize, const char *message)
{
    unsigned ohlen, olen;
    struct mbuf *m;
    struct ip *oip, *ip;
    struct icmp *icp;
    void *payload;

    LogFlow(("icmp_error: msrc = %p, msrc_len = %d\n",
             (void *)msrc, msrc ? msrc->m_len : 0));

    if (RT_UNLIKELY(msrc == NULL))
        goto end_error;

    M_ASSERTPKTHDR(msrc);

    if (   type != ICMP_UNREACH
        && type != ICMP_TIMXCEED
        && type != ICMP_SOURCEQUENCH)
        goto end_error;

    oip = mtod(msrc, struct ip *);
    LogFunc(("msrc: %RTnaipv4 -> %RTnaipv4\n", oip->ip_src, oip->ip_dst));

    if (oip->ip_src.s_addr == INADDR_ANY)
        goto end_error;

    if (oip->ip_off & IP_OFFMASK)
        goto end_error;    /* Only reply to fragment 0 */

    ohlen = oip->ip_hl * 4;
    AssertStmt(ohlen >= sizeof(struct ip), goto end_error);

    olen = oip->ip_len;
    AssertStmt(olen >= ohlen, goto end_error);

    if (oip->ip_p == IPPROTO_ICMP)
    {
        struct icmp *oicp = (struct icmp *)((char *)oip + ohlen);
        /*
         *  Assume any unknown ICMP type is an error. This isn't
         *  specified by the RFC, but think about it..
         */
        if (oicp->icmp_type > ICMP_MAXTYPE || icmp_flush[oicp->icmp_type])
            goto end_error;
    }

    /* undo byte order conversions done in ip_input() */
    HTONS(oip->ip_len);
    HTONS(oip->ip_id);
    HTONS(oip->ip_off);

    m = m_gethdr(pData, M_NOWAIT, MT_HEADER);
    if (RT_UNLIKELY(m == NULL))
        goto end_error;

    m->m_flags |= M_SKIP_FIREWALL;
    m->m_data += if_maxlinkhdr;

    ip = mtod(m, struct ip *);
    m->m_pkthdr.header = (void *)ip;

    /* fill in ip (ip_output0() does the boilerplate for us) */
    ip->ip_tos = ((oip->ip_tos & 0x1E) | 0xC0);  /* high priority for errors */
    /* ip->ip_len will be set later */
    ip->ip_off = 0;
    ip->ip_ttl = MAXTTL;
    ip->ip_p = IPPROTO_ICMP;
    ip->ip_src = alias_addr;
    ip->ip_dst = oip->ip_src;

    /* fill in icmp */
    icp = (struct icmp *)((char *)ip + sizeof(*ip));
    icp->icmp_type = type;
    icp->icmp_code = code;
    icp->icmp_id = 0;
    icp->icmp_seq = 0;

    /* fill in icmp payload: original ip header plus 8 bytes of its payload */
    if (olen > ohlen + 8)
        olen = ohlen + 8;
    payload = (void *)((char *)icp + ICMP_MINLEN);
    memcpy(payload, oip, olen);

    /*
     * Original code appended this message after the payload.  This
     * might have been a good idea for real slirp, as it provided a
     * communication channel with the remote host.  But 90s are over.
     */
    NOREF(message);

    /* hide ip header for icmp checksum calculation */
    m->m_data += sizeof(struct ip);
    m->m_len = ICMP_MINLEN + /* truncated */ olen;

    icp->icmp_cksum = 0;
    icp->icmp_cksum = cksum(m, m->m_len);

    /* reveal ip header */
    m->m_data -= sizeof(struct ip);
    m->m_len += sizeof(struct ip);
    ip->ip_len = m->m_len;

    (void) ip_output0(pData, (struct socket *)NULL, m, 1);

    icmpstat.icps_reflect++;

    /* clear source datagramm in positive branch */
    m_freem(pData, msrc);
    LogFlowFuncLeave();
    return;

end_error:

    /*
     * clear source datagramm in case if some of requirement haven't been met.
     */
    if (msrc)
        m_freem(pData, msrc);

    {
        static bool fIcmpErrorReported;
        if (!fIcmpErrorReported)
        {
            LogRel(("NAT: error occurred while sending ICMP error message\n"));
            fIcmpErrorReported = true;
        }
    }
    LogFlowFuncLeave();
}

/*
 * Reflect the ip packet back to the source
 * Note: m isn't duplicated by this method and more delivered to ip_output then.
 */
void
icmp_reflect(PNATState pData, struct mbuf *m)
{
    register struct ip *ip = mtod(m, struct ip *);
    int hlen = ip->ip_hl << 2;
    register struct icmp *icp;
    LogFlowFunc(("ENTER: m:%p\n", m));

    /*
     * Send an icmp packet back to the ip level,
     * after supplying a checksum.
     */
    m->m_data += hlen;
    m->m_len -= hlen;
    icp = mtod(m, struct icmp *);

    icp->icmp_cksum = 0;
    icp->icmp_cksum = cksum(m, ip->ip_len - hlen);

    m->m_data -= hlen;
    m->m_len += hlen;

    (void) ip_output(pData, (struct socket *)NULL, m);

    icmpstat.icps_reflect++;
    LogFlowFuncLeave();
}
