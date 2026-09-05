/* Stub minimo para poder pasar gcc -fsyntax-only sobre net_tls.c en el PC.
 * NO es la cabecera de PSL1GHT: solo tiene lo que net_tls.c usa. */
#ifndef STUB_NET_H
#define STUB_NET_H
#include <ppu-types.h>
#include <stdint.h>

#define AF_INET      2
#define SOCK_STREAM  1
#define POLLIN       1
#define POLLOUT      4
#define IPPROTO_TCP  6
#define net_errno    (*__net_errno())
int *__net_errno(void);

struct net_in_addr { u32 s_addr; };
struct sockaddr_in {
	u8  sin_len, sin_family;
	u16 sin_port;
	struct net_in_addr sin_addr;
	u8  sin_zero[8];
};
struct net_sockaddr_in {
	u8  sin_len, sin_family;
	u16 sin_port;
	struct net_in_addr sin_addr;
	u8  sin_zero[8];
};
struct net_sockaddr { u8 sa_len, sa_family; char sa_data[14]; };
struct net_hostent { char *h_name; char **h_aliases; int h_addrtype;
                     int h_length; struct net_in_addr **h_addr_list; };
struct pollfd { s32 fd; s16 events; s16 revents; };

s32 netSocket(s32 domain, s32 type, s32 protocol);
s32 netConnect(s32 s, const struct net_sockaddr *addr, u32 addrlen);
s32 netClose(s32 s);
s32 netSend(s32 s, const void *buf, u32 len, s32 flags);
s32 netRecv(s32 s, void *buf, u32 len, s32 flags);
s32 netPoll(struct pollfd *fds, s32 nfds, s32 ms);
struct net_hostent *netGetHostByName(const char *name);
u16 htons(u16 v);
#endif
