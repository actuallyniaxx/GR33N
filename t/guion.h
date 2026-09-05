/* El guion de la red de mentira: que host resuelve, cual acepta, y
 * cuanto tarda cada intento. */
#ifndef GUION_H
#define GUION_H

#define GUION_MAX 8
#define GUION_US  4

typedef struct {
	char host[128];
	int  resuelve;
	int  acepta;
	int  us[GUION_US];   /* lo que tarda cada intento, en us */
	int  intentos;       /* cuantas veces se ha intentado (lo cuenta la red) */
} guionHost;

extern guionHost guion[GUION_MAX];
extern int       guion_n;
extern int       guion_ruidoso;

#endif
