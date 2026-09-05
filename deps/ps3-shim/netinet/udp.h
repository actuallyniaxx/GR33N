/* --- Anadido por GR33N -----------------------------------------------
 *
 * Esta cabecera viene de FreeBSD tal cual, y usa los atributos que su
 * <sys/cdefs.h> define: __packed, __aligned, __unused. La newlib de
 * PSL1GHT trae un sys/cdefs.h mas pobre que no los tiene, y sin ellos el
 * compilador lee `__packed` como si fuera un nombre de tipo y suelta
 * "conflicting types for __packed".
 *
 * __packed IMPORTA DE VERDAD aqui: struct ip y struct udphdr son
 * estructuras de CABLE. Sin el atributo, el compilador puede meter relleno
 * entre campos y la estructura deja de calzar sobre los bytes que llegan.
 * En PowerPC, con campos de 8 y de 16 bits mezclados, eso no es teorico.
 * ------------------------------------------------------------------- */

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.
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
 * 3. Neither the name of the University nor the names of its contributors
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
 */

#ifndef _NETINET_UDP_H_
#define	_NETINET_UDP_H_

#include <sys/types.h>

/*
 * UDP protocol header.
 * Per RFC 768, September, 1981.
 */
struct udphdr {
	u_short	uh_sport;		/* source port */
	u_short	uh_dport;		/* destination port */
	u_short	uh_ulen;		/* udp length */
	u_short	uh_sum;			/* udp checksum */
} __packed;

/*
 * User-settable options (used with setsockopt).
 */
#define	UDP_ENCAP			1	/* int; UDP encapsulation type */

/* Start of reserved space for third-party user-settable options. */
#define	UDP_VENDOR			SO_VENDOR

/*
 * UDP Encapsulation of IPsec Packets options.
 */
/* Encapsulation types. */
#define	UDP_ENCAP_ESPINUDP_NON_IKE	1 /* draft-ietf-ipsec-nat-t-ike-00/01 */
#define	UDP_ENCAP_ESPINUDP		2 /* RFC3948 */

/* Default ESP in UDP encapsulation port. */
#define	UDP_ENCAP_ESPINUDP_PORT		500

/* Maximum UDP fragment size for ESP over UDP. */
#define	UDP_ENCAP_ESPINUDP_MAXFRAGLEN	552

#endif
