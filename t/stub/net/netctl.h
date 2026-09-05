/* Sustituto minimo para comprobar sintaxis en el PC. NO es la cabecera de
 * PSL1GHT: solo lo que usa link.c, copiado por la firma. */
#ifndef STUB_NETCTL_H
#define STUB_NETCTL_H
#include <ppu-types.h>
#define NETCTL_STATE_Disconnected 0
#define NETCTL_STATE_Connecting   1
#define NETCTL_STATE_IPObtained   3
#define NETCTL_INFO_IP_ADDRESS    14
typedef union net_ctl_info { char ip_address[16]; u32 device; } net_ctl_info;
int netCtlInit(void);
void netCtlTerm(void);
int netCtlGetState(s32 *state);
int netCtlGetInfo(s32 code, net_ctl_info *info);
#endif
