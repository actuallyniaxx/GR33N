/* Consola FALSA: la de verdad del sistema, pero SIN las macros CMSG.
 * Es lo que creemos que trae PSL1GHT -- los structs si, las macros no --
 * y aisla exactamente la variable que se quiere probar. */
#ifndef FALSA_CONSOLA_SOCKET_H
#define FALSA_CONSOLA_SOCKET_H
#include_next <sys/socket.h>
#undef CMSG_DATA
#undef CMSG_LEN
#undef CMSG_SPACE
#undef CMSG_ALIGN
#undef CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#undef SOCK_SEQPACKET
#endif
