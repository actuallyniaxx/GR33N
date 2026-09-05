/* Prueba del mapeo DS3 -> xCloud y de la zona muerta reescalada. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define GR33N_BTN_SELECT (1u<<0)
#define GR33N_BTN_L3 (1u<<1)
#define GR33N_BTN_R3 (1u<<2)
#define GR33N_BTN_START (1u<<3)
#define GR33N_BTN_UP (1u<<4)
#define GR33N_BTN_RIGHT (1u<<5)
#define GR33N_BTN_DOWN (1u<<6)
#define GR33N_BTN_LEFT (1u<<7)
#define GR33N_BTN_L2 (1u<<8)
#define GR33N_BTN_R2 (1u<<9)
#define GR33N_BTN_L1 (1u<<10)
#define GR33N_BTN_R1 (1u<<11)
#define GR33N_BTN_TRIANGLE (1u<<12)
#define GR33N_BTN_CIRCLE (1u<<13)
#define GR33N_BTN_CROSS (1u<<14)
#define GR33N_BTN_SQUARE (1u<<15)
#include "../include/xcmsg.h"

/* copiadas tal cual de webrtc.c */
static u16 mapear_botones(u32 held){u16 m=0;
 if(held&GR33N_BTN_CROSS)m|=XC_BTN_A; if(held&GR33N_BTN_CIRCLE)m|=XC_BTN_B;
 if(held&GR33N_BTN_SQUARE)m|=XC_BTN_X; if(held&GR33N_BTN_TRIANGLE)m|=XC_BTN_Y;
 if(held&GR33N_BTN_UP)m|=XC_BTN_UP; if(held&GR33N_BTN_DOWN)m|=XC_BTN_DOWN;
 if(held&GR33N_BTN_LEFT)m|=XC_BTN_LEFT; if(held&GR33N_BTN_RIGHT)m|=XC_BTN_RIGHT;
 if(held&GR33N_BTN_L1)m|=XC_BTN_LB; if(held&GR33N_BTN_R1)m|=XC_BTN_RB;
 if(held&GR33N_BTN_L3)m|=XC_BTN_LS; if(held&GR33N_BTN_R3)m|=XC_BTN_RS;
 if(held&GR33N_BTN_START)m|=XC_BTN_MENU; if(held&GR33N_BTN_SELECT)m|=XC_BTN_VIEW;
 return m;}
static s32 eje_ps3(s32 v,int dz){s32 signo=(v<0)?-1:1;s32 a=(v<0)?-v:v;s32 tope=127;
 if(dz<0)dz=0; if(dz>100)dz=100; if(a<=dz)return 0; if(a>tope)a=tope;
 return signo*(((a-dz)*1000)/(tope-dz));}

static int fallos=0;
static void ok(const char*q,int c){printf("   %-52s %s\n",q,c?"ok":"FALLA");if(!c)fallos++;}

int main(void)
{
	printf("1. el mapeo fisico\n");
	ok("Cross -> A",     mapear_botones(GR33N_BTN_CROSS) == XC_BTN_A);
	ok("Circle -> B",    mapear_botones(GR33N_BTN_CIRCLE) == XC_BTN_B);
	ok("Square -> X",    mapear_botones(GR33N_BTN_SQUARE) == XC_BTN_X);
	ok("Triangle -> Y",  mapear_botones(GR33N_BTN_TRIANGLE) == XC_BTN_Y);
	ok("START -> Menu",  mapear_botones(GR33N_BTN_START) == XC_BTN_MENU);
	ok("SELECT -> View", mapear_botones(GR33N_BTN_SELECT) == XC_BTN_VIEW);
	ok("L2/R2 NO son botones de la mascara",
	   mapear_botones(GR33N_BTN_L2|GR33N_BTN_R2) == 0);
	ok("todo a la vez suma sin pisarse",
	   mapear_botones(GR33N_BTN_CROSS|GR33N_BTN_L1|GR33N_BTN_UP)
	   == (XC_BTN_A|XC_BTN_LB|XC_BTN_UP));
	{ int i, bits=0; u16 m = mapear_botones(0xFFFFFFFFu);
	  for(i=0;i<16;i++) if(m&(1u<<i)) bits++;
	  ok("14 botones distintos, ninguno duplicado", bits == 14); }

	printf("\n2. zona muerta con reescalado\n");
	ok("centro = 0",              eje_ps3(0, 24) == 0);
	ok("justo en el umbral = 0",  eje_ps3(24, 24) == 0);
	ok("un paso mas NO salta",    eje_ps3(25, 24) > 0 && eje_ps3(25, 24) < 30);
	printf("      dz=24: v=25 -> %d,  v=76 -> %d,  v=127 -> %d\n",
	       eje_ps3(25,24), eje_ps3(76,24), eje_ps3(127,24));
	ok("el tope sigue siendo 1000", eje_ps3(127, 24) == 1000);
	ok("y por el lado negativo",    eje_ps3(-127, 24) == -1000);
	ok("simetrico",  eje_ps3(-80, 24) == -eje_ps3(80, 24));
	ok("sin zona muerta el tope tambien es 1000", eje_ps3(127, 0) == 1000);
	ok("-128 no desborda ni cambia de signo",
	   eje_ps3(-128, 24) == -1000);

	printf("\n3. el informe completo, con el mapeo dentro\n");
	{
		xcPad p; u8 b[64];
		memset(&p,0,sizeof(p));
		p.botones = mapear_botones(GR33N_BTN_CROSS|GR33N_BTN_R1);
		p.lx = eje_ps3(127,24); p.ly = eje_ps3(-127,24);
		p.rt = 1000;
		ok("38 bytes", xcInputGamepad(b,sizeof(b),5,0.0,&p) == 38);
		/* A|RB = 16|8192 = 8208 = 0x2010. Son DOS bytes, y van en
		 * little-endian: el bajo primero. La version anterior de esta
		 * comprobacion pedia b[16]==8208 con b[16] siendo un u8, cosa
		 * que no puede ser cierta nunca -- y gcc lo decia:
		 * "comparison is always false due to limited range". */
		ok("A|RB en LE (0x2010 -> 10 20)",
		   b[16]==((XC_BTN_A|XC_BTN_RB) & 0xff) &&
		   b[17]==((XC_BTN_A|XC_BTN_RB) >> 8));
		ok("stick izq X a tope", b[18]==0xff && b[19]==0x7f);
		/* ly=-1000, y xcmsg lo NIEGA -> arriba positivo */
		ok("arriba sale POSITIVO (xcmsg invierte)", b[20]==0xff && b[21]==0x7f);
		ok("gatillo derecho a tope", b[28]==0xff && b[29]==0xff);
	}

	printf("\n%s (%d fallos)\n", fallos?"HAY FALLOS":"todo bien", fallos);
	return fallos!=0;
}
