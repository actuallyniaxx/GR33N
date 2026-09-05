/* El ciclo del desplazamiento: nunca puede salirse del rango [0, over],
 * ni con longitudes raras ni al dar la vuelta. */
#include <stdio.h>
typedef unsigned int u32;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){printf("FALLO: ");printf(__VA_ARGS__);printf("\n");fails++;} }while(0)

static int pos(int over, u32 clock)
{
	u32 hold = 45, step = 8;
	u32 run  = (u32)over * step;
	u32 total = hold + run + hold + run;
	u32 t = clock % total;
	int from;
	if (t < hold) from = 0;
	else if (t < hold + run) from = (int)((t - hold) / step);
	else if (t < hold + run + hold) from = over;
	else from = over - (int)((t - hold - run - hold) / step);
	if (from < 0) from = 0;
	if (from > over) from = over;
	return from;
}
int main(void)
{
	int over; u32 c;
	for (over = 1; over <= 90; over++) {
		int seen0 = 0, seenmax = 0;
		for (c = 0; c < 20000; c++) {
			int f = pos(over, c);
			if (f < 0 || f > over) { CHECK(0, "over=%d c=%u -> %d fuera", over, c, f); break; }
			if (f == 0) seen0 = 1;
			if (f == over) seenmax = 1;
		}
		CHECK(seen0, "over=%d nunca vuelve al principio", over);
		CHECK(seenmax, "over=%d nunca llega al final", over);
	}
	/* empieza siempre por el principio */
	for (over = 1; over <= 90; over++)
		CHECK(pos(over, 0) == 0, "over=%d no empieza en 0", over);
	/* y avanza de uno en uno, sin saltos */
	for (over = 1; over <= 90; over++) {
		int prev = pos(over, 0);
		for (c = 1; c < 4000; c++) {
			int f = pos(over, c);
			int d = f > prev ? f - prev : prev - f;
			if (d > 1 && !(prev == 0 && f == 0)) {
				/* el unico salto permitido es el reinicio del ciclo */
				u32 total = 45 + (u32)over*8 + 45 + (u32)over*8;
				if (c % total != 0) { CHECK(0, "over=%d salto de %d en c=%u", over, d, c); break; }
			}
			prev = f;
		}
	}
	printf(fails?"%d FALLOS\n":"desplazamiento: todo correcto (%d fallos)\n",fails);
	return fails?1:0;
}
