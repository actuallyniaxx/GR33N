/* Prueba de sobremesa del macro de combinaciones: que los dos arrays
 * salgan del mismo sitio y no puedan desincronizarse. */
#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
#define GR33N_BTN_SELECT (1u<<0)
#define GR33N_BTN_L3     (1u<<1)
#define GR33N_BTN_R3     (1u<<2)
#define GR33N_BTN_START  (1u<<3)
#define GR33N_BTN_L1     (1u<<10)
#define GR33N_BTN_R1     (1u<<11)
#define GR33N_BTN_CIRCLE (1u<<13)

#define COMBO_LISTA \
	X("SELECT + Atras",   GR33N_BTN_SELECT,              0)                \
	X("SELECT + START",   GR33N_BTN_SELECT,              GR33N_BTN_START)  \
	X("L1 + R1 + START",  GR33N_BTN_L1 | GR33N_BTN_R1,   GR33N_BTN_START)  \
	X("L3 + R3",          GR33N_BTN_L3,                  GR33N_BTN_R3)

static const char *const combo_name[] = {
#define X(n, h, p) n,
	COMBO_LISTA
#undef X
};
static const struct { u32 mantener, pulsar; } combo_btn[] = {
#define X(n, h, p) { (h), (p) },
	COMBO_LISTA
#undef X
};
#define COMBOS_N ((int)(sizeof(combo_name)/sizeof(combo_name[0])))

static int set_exit_combo = 0;
static u32 uiBtnBack(void) { return GR33N_BTN_CIRCLE; }

static int combo_hecho(u32 held, u32 pressed)
{
	int i = (set_exit_combo >= 0 && set_exit_combo < COMBOS_N) ? set_exit_combo : 0;
	u32 mantener = combo_btn[i].mantener;
	u32 pulsar   = combo_btn[i].pulsar ? combo_btn[i].pulsar : uiBtnBack();
	if (mantener == 0) return 0;
	return (held & mantener) == mantener && (pressed & pulsar) != 0;
}

static int fallos = 0;
static void ok(const char *q, int c){ printf("   %-52s %s\n", q, c?"ok":"FALLA"); if(!c) fallos++; }

int main(void)
{
	int i;
	printf("1. las dos listas no pueden desincronizarse\n");
	ok("mismo numero de elementos",
	   (int)(sizeof(combo_btn)/sizeof(combo_btn[0])) == COMBOS_N);
	{
		int sin_mod = 0;
		for (i = 0; i < COMBOS_N; i++) if (combo_btn[i].mantener == 0) sin_mod++;
		ok("todas tienen modificador (ninguna es un boton suelto)", sin_mod == 0);
	}

	printf("\n2. SELECT + Atras (por defecto)\n");
	set_exit_combo = 0;
	ok("SELECT mantenido + O pulsado sale",
	   combo_hecho(GR33N_BTN_SELECT, GR33N_BTN_CIRCLE));
	ok("O suelto NO sale", !combo_hecho(0, GR33N_BTN_CIRCLE));
	ok("SELECT suelto NO sale", !combo_hecho(GR33N_BTN_SELECT, 0));
	ok("al reves (O mantenido, SELECT pulsado) NO sale",
	   !combo_hecho(GR33N_BTN_CIRCLE, GR33N_BTN_SELECT));

	printf("\n3. L1 + R1 + START\n");
	set_exit_combo = 2;
	ok("los dos gatillos + START sale",
	   combo_hecho(GR33N_BTN_L1|GR33N_BTN_R1, GR33N_BTN_START));
	ok("solo L1 + START no basta",
	   !combo_hecho(GR33N_BTN_L1, GR33N_BTN_START));
	ok("START suelto no sale", !combo_hecho(0, GR33N_BTN_START));

	printf("\n4. L3 + R3\n");
	set_exit_combo = 3;
	ok("L3 mantenido + R3 pulsado sale",
	   combo_hecho(GR33N_BTN_L3, GR33N_BTN_R3));
	ok("R3 suelto no sale", !combo_hecho(0, GR33N_BTN_R3));

	printf("\n5. un indice fuera de rango cae en la de por defecto\n");
	set_exit_combo = 99;
	ok("se comporta como SELECT + Atras",
	   combo_hecho(GR33N_BTN_SELECT, GR33N_BTN_CIRCLE));
	set_exit_combo = -1;
	ok("y por abajo tambien", combo_hecho(GR33N_BTN_SELECT, GR33N_BTN_CIRCLE));

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
