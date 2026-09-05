/* GR33N - capa de mando */

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <io/pad.h>

#include "input.h"
#include "link.h"   /* linkLog: la presion hay que poder verla en el log */

static gr33nPad state;
static padData  last_raw;
static int      have_raw = 0;
static int      puerto_presion = -1;   /* a quien se le ha pedido ya */

int inputInit(void)
{
	s32 ret;

	memset(&state, 0, sizeof(state));
	memset(&last_raw, 0, sizeof(last_raw));
	have_raw = 0;
	state.port = -1;

	ret = ioPadInit(7);
	if (ret != 0)
		printf("[input] ioPadInit fallo: 0x%08x\n", (unsigned)ret);

	return (ret == 0) ? 0 : -1;
}

void inputShutdown(void)
{
	ioPadEnd();
}

void inputPoll(void)
{
	padInfo info;
	padData data;
	u32 held = 0;
	int i, port = -1;

	if (ioPadGetInfo(&info) == 0) {
		for (i = 0; i < MAX_PADS; i++) {
			if (info.status[i]) { port = i; break; }
		}
	}

	/* EL MANDO NO MANDA LA PRESION SI NO SE LA PIDES.
	 *
	 * MEDIDO: con los gatillos analogicos ya escritos, el log daba
	 * "gatillos crudos L=0 R=0" con L2 y R2 a fondo. El nombre del campo
	 * era correcto --PRE_L2 sale de io/pad.h-- y el codigo lo leia bien.
	 * Lo que pasaba es que el DS3 manda un paquete CORTO por defecto, sin
	 * la parte de presion, y esos campos se quedan a cero.
	 *
	 * ioPadSetPressMode le pide el paquete largo. Hay que hacerlo por
	 * puerto y despues de que el mando aparezca, no una vez al arrancar:
	 * un mando que se conecte luego nace otra vez en modo corto.
	 *
	 * Se llama solo cuando CAMBIA el puerto para no repetirlo sesenta
	 * veces por segundo. */
	if (port >= 0 && port != puerto_presion) {
		s32 r = ioPadSetPressMode((u32)port, 1);

		puerto_presion = port;
		linkLog("[input] presion analogica en el puerto %d: %s",
		        port, r == 0 ? "pedida" : "NO la acepta");
	}

	if (port < 0) {
		puerto_presion = -1;
		/* Mando desconectado: soltamos todo, pero de forma limpia para
		 * que quien escuche flancos vea el "released". */
		state.released  = state.held;
		state.pressed   = 0;
		state.held      = 0;
		state.lx = state.ly = state.rx = state.ry = 0;
		state.connected = 0;
		state.port      = -1;
		have_raw        = 0;
		return;
	}

	state.port = port;

	/* ioPadGetData solo rellena si HAY CAMBIO. Con len==0 no hay dato
	 * nuevo y hay que reutilizar el ultimo valido, o el mando parece
	 * soltarse solo entre frames. */
	if (ioPadGetData((u32)port, &data) == 0 && data.len > 0) {
		last_raw = data;
		have_raw = 1;
	}

	if (!have_raw) {
		state.pressed   = 0;
		state.released  = 0;
		state.connected = 1;
		return;
	}

	if (last_raw.BTN_SELECT)   held |= GR33N_BTN_SELECT;
	if (last_raw.BTN_L3)       held |= GR33N_BTN_L3;
	if (last_raw.BTN_R3)       held |= GR33N_BTN_R3;
	if (last_raw.BTN_START)    held |= GR33N_BTN_START;
	if (last_raw.BTN_UP)       held |= GR33N_BTN_UP;
	if (last_raw.BTN_RIGHT)    held |= GR33N_BTN_RIGHT;
	if (last_raw.BTN_DOWN)     held |= GR33N_BTN_DOWN;
	if (last_raw.BTN_LEFT)     held |= GR33N_BTN_LEFT;
	if (last_raw.BTN_L2)       held |= GR33N_BTN_L2;
	if (last_raw.BTN_R2)       held |= GR33N_BTN_R2;
	if (last_raw.BTN_L1)       held |= GR33N_BTN_L1;
	if (last_raw.BTN_R1)       held |= GR33N_BTN_R1;
	if (last_raw.BTN_TRIANGLE) held |= GR33N_BTN_TRIANGLE;
	if (last_raw.BTN_CIRCLE)   held |= GR33N_BTN_CIRCLE;
	if (last_raw.BTN_CROSS)    held |= GR33N_BTN_CROSS;
	if (last_raw.BTN_SQUARE)   held |= GR33N_BTN_SQUARE;

	state.pressed  = held & ~state.held;
	state.released = state.held & ~held;
	state.held     = held;

	/* La presion de los gatillos. Llega SOLO si el paquete es el largo,
	 * o sea si ioPadSetPressMode dijo que si; con el corto estos campos
	 * valen cero aunque el boton este a fondo. */
	state.lt = (s32)last_raw.PRE_L2;
	state.rt = (s32)last_raw.PRE_R2;

	/* El tamano del paquete, UNA vez y cuando cambie. Es la prueba de que
	 * la presion viene de verdad: corto y largo se distinguen aqui, no
	 * adivinando. */
	{
		static int len_visto = -1;

		if ((int)last_raw.len != len_visto) {
			len_visto = (int)last_raw.len;
			linkLog("[input] paquete del mando: len=%d (presion L=%d R=%d)",
			        len_visto, (int)state.lt, (int)state.rt);
		}
	}

	/* Los nubs vienen 0..255 con 128 en reposo. */
	state.lx = (s32)last_raw.ANA_L_H - 128;
	state.ly = (s32)last_raw.ANA_L_V - 128;
	state.rx = (s32)last_raw.ANA_R_H - 128;
	state.ry = (s32)last_raw.ANA_R_V - 128;

	state.connected = 1;
}

const gr33nPad *inputPad(void)
{
	return &state;
}

static const char *btn_names[GR33N_BTN_COUNT] = {
	"SEL", "L3", "R3", "STA",
	"UP", "RGT", "DWN", "LFT",
	"L2", "R2", "L1", "R1",
	"TRI", "CIR", "CRO", "SQU"
};

const char *inputButtonName(int bit)
{
	if (bit < 0 || bit >= GR33N_BTN_COUNT) return "?";
	return btn_names[bit];
}
