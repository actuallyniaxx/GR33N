#ifndef FALSO_NETINET_IN_H
#define FALSO_NETINET_IN_H
#include <stdint.h>
#define AF_INET       2
#define SOCK_STREAM   1
#define IPPROTO_TCP   6
#define SOL_SOCKET    0xffff
struct in_addr { uint32_t s_addr; };
/* CON sin_len: el ABI de lv2 lo tiene y el codigo lo rellena. */
struct sockaddr_in {
	uint8_t  sin_len;
	uint8_t  sin_family;
	uint16_t sin_port;
	struct in_addr sin_addr;
	char     sin_zero[8];
};
struct sockaddr { uint8_t sa_len; uint8_t sa_family; char sa_data[14]; };
uint16_t htons(uint16_t x);
#endif
