/* Prueba de sobremesa de dos cosas de catalog.c que son pura logica:
 *
 *   1. el anillo de descripciones (desc_put / catDescription), que estaba
 *      al reves: refrescar una entrada la convertia en la siguiente
 *      victima, y un solo lote de 16 se llevaba dos tercios del anillo por
 *      delante, incluida la que se estaba leyendo;
 *
 *   2. la terminacion del barrido, que giraba al cien por cien de CPU en
 *      cuanto habia un titulo sin productId.
 *
 * La logica esta copiada de catalog.c a mano. Si alguien la cambia alli y
 * no aqui, esto deja de cuadrar, que es justo lo que se quiere.
 */

#include <stdio.h>
#include <string.h>

#define CAT_DESC_N     24
#define CAT_DESC_MAX   4096
#define CAT_BATCH      16
#define ART_NONE       0xffffffffu

typedef unsigned u32;

static int fails = 0;

#define CHECK(c, ...) do { if (!(c)) { \
	printf("FALLO %s:%d ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* --------------------------------------------------------------------- */
/* 1. Anillo de descripciones                                            */
/* --------------------------------------------------------------------- */

static char desc_buf[CAT_DESC_N][32];
static u32  desc_idx[CAT_DESC_N];
static u32  desc_next = 0;
static u32  desc_pin  = ART_NONE;

static void ring_init(void)
{
	u32 i;
	for (i = 0; i < CAT_DESC_N; i++) { desc_idx[i] = ART_NONE; desc_buf[i][0] = 0; }
	desc_next = 0;
	desc_pin  = ART_NONE;
}

static const char *desc_get(u32 idx)
{
	u32 i;
	for (i = 0; i < CAT_DESC_N; i++)
		if (desc_idx[i] == idx) return desc_buf[i];
	return "";
}

static void desc_put(u32 idx, const char *txt)
{
	u32 i, tries;

	for (i = 0; i < CAT_DESC_N; i++)
		if (desc_idx[i] == idx) break;

	if (i == CAT_DESC_N) {
		for (tries = 0; tries < CAT_DESC_N; tries++) {
			i = desc_next;
			desc_next = (desc_next + 1) % CAT_DESC_N;
			if (desc_pin == ART_NONE || desc_idx[i] != desc_pin) break;
		}
	}

	desc_idx[i] = ART_NONE;
	snprintf(desc_buf[i], sizeof(desc_buf[0]), "%s", txt);
	desc_idx[i] = idx;
}

static void test_ring(void)
{
	char t[32];
	u32 k;

	/* Refrescar una entrada NO la convierte en la proxima victima. */
	ring_init();
	for (k = 0; k < CAT_DESC_N; k++) { snprintf(t, sizeof(t), "d%u", k); desc_put(k, t); }

	desc_put(5, "d5-refrescada");   /* acierto: no deberia mover el cursor */
	desc_put(100, "nueva");         /* fallo: entra por el cursor          */

	CHECK(strcmp(desc_get(5), "d5-refrescada") == 0,
	      "refrescar la 5 y meter una nueva se la ha llevado: \"%s\"",
	      desc_get(5));

	/* LA QUE SE ESTA MIRANDO sobrevive a un lote entero. Con 24 huecos y
	 * lotes de 16, sin la fijacion la pierdes en dos lotes de cada tres. */
	ring_init();
	for (k = 0; k < CAT_DESC_N; k++) { snprintf(t, sizeof(t), "d%u", k); desc_put(k, t); }

	desc_pin = 7;                    /* la ficha esta abierta en el 7 */

	{
		int lote;
		for (lote = 0; lote < 20; lote++) {
			for (k = 0; k < CAT_BATCH; k++) {
				u32 id = 1000 + (u32)lote * CAT_BATCH + k;
				snprintf(t, sizeof(t), "x%u", id);
				desc_put(id, t);
			}

			CHECK(strcmp(desc_get(7), "d7") == 0,
			      "el lote %d se ha llevado la descripcion fijada", lote);
		}
	}

	/* Sin fijar, el anillo sigue rotando y no se atasca. */
	ring_init();
	desc_pin = ART_NONE;
	for (k = 0; k < 500; k++) {
		snprintf(t, sizeof(t), "y%u", k);
		desc_put(k, t);
		CHECK(strcmp(desc_get(k), t) == 0, "la %u no se guardo", k);
	}

	/* Y las CAT_DESC_N ultimas siguen ahi. */
	for (k = 500 - CAT_DESC_N; k < 500; k++) {
		snprintf(t, sizeof(t), "y%u", k);
		CHECK(strcmp(desc_get(k), t) == 0,
		      "la %u deberia seguir en el anillo", k);
	}

	printf("anillo: ok\n");
}

/* --------------------------------------------------------------------- */
/* 2. Terminacion del barrido                                            */
/* --------------------------------------------------------------------- */

#define N_TIT  200

static struct { int detailed, entitled, has_pid; } tit[N_TIT];
static u32 sweep_at = 0;
static int sweep_done = 0;

/* run_details: devuelve CUANTOS ha pedido, y marca como intentados los que
 * pidio. Se salta los que no tienen productId. */
static u32 run_details(u32 from)
{
	u32 i, n = 0, asked[CAT_BATCH];

	for (i = from; i < N_TIT && n < CAT_BATCH; i++) {
		if (tit[i].detailed || !tit[i].has_pid) continue;
		asked[n++] = i;
	}

	if (!n) return 0;

	for (i = 0; i < n; i++) tit[asked[i]].detailed = 1;
	return n;
}

static int sweep_pump(void)
{
	u32 i;

	if (sweep_done) return 0;

	for (i = sweep_at; i < N_TIT; i++) {
		if (tit[i].detailed || !tit[i].entitled) continue;
		if (!tit[i].has_pid) continue;
		sweep_at = i;
		return run_details(i) > 0;
	}

	for (i = 0; i < N_TIT; i++) {
		if (tit[i].detailed || !tit[i].has_pid) continue;
		return run_details(i) > 0;
	}

	sweep_done = 1;
	return 0;
}

static void test_sweep(int sin_pid_cada)
{
	int vueltas = 0, i;

	memset(tit, 0, sizeof(tit));
	sweep_at = 0;
	sweep_done = 0;

	for (i = 0; i < N_TIT; i++) {
		tit[i].entitled = (i % 4) == 0;
		tit[i].has_pid  = sin_pid_cada ? ((i % sin_pid_cada) != 0) : 1;
	}

	/* El bucle real es `while (...) if (sweep_pump()) continue;` SIN
	 * dormir. Si esto no llega a devolver 0, en la consola es un hilo al
	 * cien por cien para siempre. */
	while (sweep_pump()) {
		if (++vueltas > 10000) {
			CHECK(0, "el barrido no termina (sin_pid_cada=%d)", sin_pid_cada);
			return;
		}
	}

	/* Y una vez terminado, sigue diciendo que no hay nada que hacer. */
	for (i = 0; i < 100; i++)
		CHECK(sweep_pump() == 0, "revive despues de terminar");

	/* Todo lo que tenia productId acabo con datos. */
	for (i = 0; i < N_TIT; i++)
		if (tit[i].has_pid)
			CHECK(tit[i].detailed, "el %d tenia productId y se quedo sin datos", i);

	printf("barrido (uno sin productId cada %d): ok en %d vueltas\n",
	       sin_pid_cada ? sin_pid_cada : 0, vueltas);
}

int main(void)
{
	test_ring();
	test_sweep(0);    /* todos con productId  */
	test_sweep(7);    /* algunos sin          */
	test_sweep(1);    /* NINGUNO con          */
	test_sweep(2);

	if (fails) { printf("\n%d FALLOS\n", fails); return 1; }
	printf("\ntodo bien\n");
	return 0;
}
