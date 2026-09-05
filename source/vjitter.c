/* GR33N - RTP a unidades de acceso H.264. Ver include/vjitter.h. */

#include <stddef.h>
#include <string.h>

#include "vjitter.h"

/* El codigo de arranque de Annex-B: CUATRO BYTES, uno a uno.
 *
 * En libpeer esto era `static const uint32_t nalu_start_4bytecode =
 * 0x01000000;` copiado con memcpy, y ese fue el fallo numero uno de la
 * auditoria de agosto: en little-endian los bytes salen 00 00 00 01, que
 * es lo correcto, y en big-endian salen 01 00 00 00, que no es nada.
 * cellVdec tiraria el flujo entero sin dar un error que apunte aqui.
 *
 * Un array de bytes no tiene orden que equivocar. */
static const u8 ARRANQUE[4] = { 0x00, 0x00, 0x00, 0x01 };

/* --------------------------------------------------------------------- */
/* Cabecera RTP                                                          */
/* --------------------------------------------------------------------- */

/* Los campos se leen byte a byte y se arman a mano, no con un cast a una
 * estructura con campos de bits. El reparto de bits dentro de un byte lo
 * decide la ABI y se da la vuelta entre little y big endian -- el segundo
 * fallo de la auditoria. Asi es igual en las dos. */
static int parse_rtp(const u8 *rtp, size_t len, u16 *seq, u32 *ts,
                     int *marca, const u8 **carga, size_t *carga_len)
{
	size_t cab;
	u8 cc;
	int ext, relleno;

	if (rtp == NULL || len < 12) return 0;

	cc      = (u8)(rtp[0] & 0x0f);
	ext     = (rtp[0] & 0x10) != 0;
	relleno = (rtp[0] & 0x20) != 0;

	*marca = (rtp[1] & 0x80) != 0;
	*seq   = (u16)(((u16)rtp[2] << 8) | rtp[3]);
	*ts    = ((u32)rtp[4] << 24) | ((u32)rtp[5] << 16) |
	         ((u32)rtp[6] << 8)  | (u32)rtp[7];

	cab = 12u + 4u * (size_t)cc;
	if (len < cab) return 0;

	if (ext) {
		size_t palabras;

		if (len < cab + 4) return 0;
		palabras = ((size_t)rtp[cab + 2] << 8) | rtp[cab + 3];
		cab += 4 + 4 * palabras;
		if (len < cab) return 0;
	}

	*carga_len = len - cab;

	/* EL RELLENO SE QUITA, y esto no es cosmetico.
	 *
	 * libwebrtc manda paquetes de relleno para sondear el ancho de banda:
	 * llevan el bit P y el ultimo byte dice cuanto relleno hay. Sin
	 * quitarlo, esos bytes se leen como si fueran un NALU; y un paquete
	 * que sea SOLO relleno --que gasta numero de secuencia igual-- parece
	 * un paquete de video perdido y rompe el reensamblado. */
	if (relleno && *carga_len > 0) {
		u8 pad = rtp[len - 1];
		*carga_len = (pad <= *carga_len) ? *carga_len - pad : 0;
	}

	*carga = rtp + cab;
	return 1;
}

static u8 nal_tipo(const u8 *p, size_t len)
{
	return len ? (u8)(p[0] & 0x1f) : (u8)0;
}

/* Si este paquete puede ser el PRIMERO de un fotograma. */
static int es_cabeza(const u8 *p, size_t len)
{
	u8 t = nal_tipo(p, len);

	if (t == 28) return len > 1 && (p[1] & 0x80) != 0;  /* inicio de FU-A */
	return t >= 1 && t <= 24;                            /* NALU suelto o STAP-A */
}

/* --------------------------------------------------------------------- */
/* Despaquetizado                                                        */
/* --------------------------------------------------------------------- */

/* Anade a la unidad de acceso. Devuelve 0 si no cabe: un fotograma
 * recortado tiene forma de fotograma y el decodificador lo intentaria. */
static int poner(u8 *au, size_t *n, const u8 *d, size_t len)
{
	if (*n + len > VJ_AU_MAX) return 0;
	memcpy(au + *n, d, len);
	*n += len;
	return 1;
}

static int despaqueta(const u8 *p, size_t len, u8 *au, size_t *n)
{
	u8 t;

	if (len == 0) return 1;
	t = nal_tipo(p, len);

	if (t >= 1 && t <= 23) {
		/* Un NALU entero en un paquete. */
		if (!poner(au, n, ARRANQUE, 4)) return 0;
		return poner(au, n, p, len);
	}

	if (t == 24) {
		/* STAP-A: varios NALU pequenos en el mismo paquete, cada uno
		 * precedido de su longitud en dos bytes big-endian. */
		size_t i = 1;

		while (i + 2 <= len) {
			size_t m = ((size_t)p[i] << 8) | p[i + 1];

			i += 2;
			if (m == 0 || i + m > len) break;
			if (!poner(au, n, ARRANQUE, 4)) return 0;
			if (!poner(au, n, p + i, m))    return 0;
			i += m;
		}
		return 1;
	}

	if (t == 28) {
		/* FU-A: un NALU partido en varios paquetes.
		 *
		 * La cabecera original se reconstruye juntando los tres bits de
		 * arriba del indicador (F y NRI) con los cinco de abajo del
		 * segundo byte (el tipo). Solo en el primer trozo; los demas son
		 * continuacion y van pegados. */
		if (len < 2) return 1;

		if (p[1] & 0x80) {
			u8 cabecera = (u8)((p[0] & 0xe0) | (p[1] & 0x1f));

			if (!poner(au, n, ARRANQUE, 4))  return 0;
			if (!poner(au, n, &cabecera, 1)) return 0;
		}
		return poner(au, n, p + 2, len - 2);
	}

	/* Cualquier otro tipo (STAP-B, MTAP, FU-B) no lo manda xCloud. Se
	 * ignora en vez de inventarse nada. */
	return 1;
}

/* Un IDR de verdad, y SOLO un IDR (tipo 5).
 *
 * xCloud repite SPS y PPS en el flujo sin mandar un IDR detras. Aceptar
 * un SPS suelto como "ya podemos seguir" hace que se reanude sobre
 * fotogramas P cuyas referencias nunca se decodificaron: el decodificador
 * los rellena desde superficies sin inicializar y sale la basura verde
 * que se mantiene sola. Solo un IDR reconstruye la cadena. */
static int hay_clave(const u8 *au, size_t n)
{
	size_t i;

	for (i = 0; i + 4 < n; i++)
		if (au[i] == 0 && au[i+1] == 0 && au[i+2] == 0 && au[i+3] == 1)
			if ((au[i+4] & 0x1f) == 5) return 1;

	return 0;
}

/* --------------------------------------------------------------------- */
/* El deposito de paquetes                                               */
/* --------------------------------------------------------------------- */

static void pool_init(vjBuf *v)
{
	int i;

	for (i = 0; i < VJ_MAX_PKTS - 1; i++)
		v->pool[i].sig = (s16)(i + 1);
	v->pool[VJ_MAX_PKTS - 1].sig = -1;
	v->libre = 0;
}

static s16 pool_coge(vjBuf *v)
{
	s16 i = v->libre;

	if (i < 0) return -1;
	v->libre = v->pool[i].sig;
	v->pool[i].sig = -1;
	return i;
}

static void pool_suelta_lista(vjBuf *v, s16 cabeza)
{
	while (cabeza >= 0) {
		s16 sig = v->pool[cabeza].sig;

		v->pool[cabeza].sig = v->libre;
		v->libre = cabeza;
		cabeza = sig;
	}
}

/* --------------------------------------------------------------------- */
/* Fotogramas                                                            */
/* --------------------------------------------------------------------- */

/* a es mas nuevo que b, contando la vuelta de los 32 bits. */
static int ts_mas_nuevo(u32 a, u32 b)
{
	u32 d = a - b;
	return d != 0 && d < 0x80000000u;
}

static void quita_frame(vjBuf *v, int idx)
{
	pool_suelta_lista(v, v->frames[idx].cabeza);

	memmove(&v->frames[idx], &v->frames[idx + 1],
	        sizeof(vjFrame) * (size_t)(v->n_frames - idx - 1));
	v->n_frames--;
}

static vjFrame *busca_o_crea(vjBuf *v, u32 ts, u64 ahora_ms)
{
	int i;

	for (i = 0; i < v->n_frames; i++)
		if (v->frames[i].ts == ts) return &v->frames[i];

	/* Mas viejo que lo ultimo que ya se solto: llega tarde. */
	if (v->hay_ultimo && !ts_mas_nuevo(ts, v->ultimo_ts)) return NULL;
	if (v->n_frames >= VJ_MAX_FRAMES) return NULL;

	/* En orden ascendente de marca de tiempo. */
	for (i = 0; i < v->n_frames; i++)
		if (ts_mas_nuevo(v->frames[i].ts, ts)) break;

	memmove(&v->frames[i + 1], &v->frames[i],
	        sizeof(vjFrame) * (size_t)(v->n_frames - i));
	v->n_frames++;

	v->frames[i].ts       = ts;
	v->frames[i].visto_ms = ahora_ms;
	v->frames[i].cabeza   = -1;
	v->frames[i].usado    = 1;

	return &v->frames[i];
}

/* Monta la unidad de acceso si el fotograma esta completo.
 *
 * Completo quiere decir: tiene el paquete con la marca de final, su
 * paquete de secuencia mas baja es cabeza de NALU, y entre los dos no
 * falta ninguno.
 *
 * NO se exige continuidad ENTRE fotogramas a proposito. Los paquetes de
 * relleno de libwebrtc se cuelan entre uno y otro y gastan numeros de
 * secuencia; tratarlos como perdida tiraba todos los fotogramas y
 * congelaba la imagen. */
static int monta(vjBuf *v, vjFrame *f, size_t *n_au)
{
	s16 orden[VJ_MAX_PKTS];
	int n = 0, i, j;
	s16 p;
	u16 seq_marca = 0;
	int hay_marca = 0;

	for (p = f->cabeza; p >= 0; p = v->pool[p].sig) {
		if (v->pool[p].marca) { seq_marca = v->pool[p].seq; hay_marca = 1; }
		if (n < VJ_MAX_PKTS) orden[n++] = p;
	}
	if (!hay_marca) return 0;

	/* Ordenar por distancia a la marca, de mas lejos a mas cerca. La
	 * resta en 16 bits da la vuelta sola, asi que el fotograma que cruza
	 * el 65535 se ordena igual de bien. */
	for (i = 1; i < n; i++) {
		s16 tmp = orden[i];
		u16 d = (u16)(seq_marca - v->pool[tmp].seq);

		for (j = i - 1; j >= 0; j--) {
			if ((u16)(seq_marca - v->pool[orden[j]].seq) >= d) break;
			orden[j + 1] = orden[j];
		}
		orden[j + 1] = tmp;
	}

	if (!es_cabeza(v->pool[orden[0]].datos, v->pool[orden[0]].len))
		return 0;

	for (i = 1; i < n; i++)
		if (v->pool[orden[i]].seq !=
		    (u16)(v->pool[orden[i - 1]].seq + 1))
			return 0;   /* hueco dentro del fotograma: a esperar */

	*n_au = 0;
	for (i = 0; i < n; i++)
		if (!despaqueta(v->pool[orden[i]].datos, v->pool[orden[i]].len,
		                v->au, n_au))
			return 0;   /* no cabe: mejor tirarlo que recortarlo */

	return 1;
}

/* --------------------------------------------------------------------- */

void vjReset(vjBuf *v)
{
	memset(v, 0, sizeof(*v));
	pool_init(v);
	v->esperando_clave = 1;
	v->ultimo_ts = 0;
	v->hay_ultimo = 0;
	v->n_frames = 0;
}

int vjEsperandoClave(const vjBuf *v)
{
	return v->esperando_clave;
}

const vjStats *vjEstado(const vjBuf *v)
{
	return &v->st;
}

int vjStatsRtcp(vjBuf *v, u8 *fraccion_perdida, u32 *perdidos, u32 *mayor_seq)
{
	u32 ext_max, esperados, perdidos_tot, exp_iv, rcv_iv, perd_iv;

	if (!v->hay_seq) return 0;

	ext_max      = v->ciclos + v->max_seq;
	esperados    = ext_max - v->base_seq + 1;
	perdidos_tot = (esperados > v->recibidos) ? esperados - v->recibidos : 0;

	exp_iv = esperados - v->esperados_antes;
	rcv_iv = v->recibidos - v->recibidos_antes;
	v->esperados_antes = esperados;
	v->recibidos_antes = v->recibidos;

	perd_iv = (exp_iv > rcv_iv) ? exp_iv - rcv_iv : 0;

	*fraccion_perdida = (exp_iv == 0 || perd_iv == 0)
	                    ? 0 : (u8)((perd_iv << 8) / exp_iv);
	*perdidos  = perdidos_tot & 0x00ffffffu;
	*mayor_seq = ext_max;
	return 1;
}

void vjRecibe(vjBuf *v, const u8 *rtp, size_t len, u64 ahora_ms,
              void (*emitir)(const u8 *au, size_t n, void *ud),
              void (*nack)(u16 pid, u16 blp, void *ud),
              void *ud, int *pedir_clave)
{
	u16 seq;
	u32 ts;
	int marca;
	const u8 *carga;
	size_t carga_len;
	s16 p;
	vjFrame *f;

	if (!parse_rtp(rtp, len, &seq, &ts, &marca, &carga, &carga_len))
		return;

	v->st.paquetes++;

	/* --- secuencia global: huecos que pedir y estadisticas --- */
	if (!v->hay_seq) {
		v->hay_seq = 1;
		v->base_seq = v->max_seq = seq;
		v->ciclos = v->recibidos = 0;
	}
	v->recibidos++;

	{
		u16 delta = (u16)(seq - v->max_seq);

		if (delta != 0 && delta < 0x8000) {   /* mas nuevo que el maximo */
			u16 hueco = (u16)(delta - 1);

			if (seq < v->max_seq) v->ciclos += 0x10000;

			/* Solo se pide lo razonable: un hueco de mas de 255 no es
			 * reordenacion, es un corte, y pedir doscientas
			 * retransmisiones lo empeora. */
			if (hueco > 0 && hueco <= 255 && nack) {
				u16 pid = (u16)(v->max_seq + 1);

				while (pid != seq) {
					u16 blp = 0, nx = (u16)(pid + 1);
					int b;

					for (b = 0; b < 16 && nx != seq; b++, nx++)
						blp |= (u16)(1u << b);

					nack(pid, blp, ud);
					v->st.nacks++;
					pid = nx;
				}
			}
			v->max_seq = seq;
		}
	}

	if (carga_len == 0) return;   /* relleno: no hay nada que montar */

	f = busca_o_crea(v, ts, ahora_ms);
	if (f == NULL) return;

	/* Duplicado o retransmision de algo que ya tenemos. */
	for (p = f->cabeza; p >= 0; p = v->pool[p].sig)
		if (v->pool[p].seq == seq) return;

	{
		s16 idx = pool_coge(v);

		if (idx < 0) {
			/* Sin sitio. Se tira el fotograma MAS VIEJO --que es el que
			 * mas cerca esta de vencer-- y se pide clave: quedarse sin
			 * memoria en silencio seria congelar la imagen sin decir
			 * por que. */
			v->st.sin_sitio++;
			if (v->n_frames > 0) {
				v->ultimo_ts = v->frames[0].ts;
				v->hay_ultimo = 1;
				quita_frame(v, 0);
				v->esperando_clave = 1;
				if (pedir_clave) *pedir_clave = 1;
			}
			idx = pool_coge(v);
			if (idx < 0) return;
			f = busca_o_crea(v, ts, ahora_ms);
			if (f == NULL) { pool_suelta_lista(v, idx); return; }
		}

		if (carga_len > VJ_PKT_MAX) carga_len = VJ_PKT_MAX;

		v->pool[idx].seq   = seq;
		v->pool[idx].marca = (u8)(marca ? 1 : 0);
		v->pool[idx].len   = (u16)carga_len;
		memcpy(v->pool[idx].datos, carga, carga_len);

		v->pool[idx].sig = f->cabeza;
		f->cabeza = idx;
	}

	/* --- vaciar: soltar lo completo en orden, y tirar lo vencido --- */
	for (;;) {
		size_t n_au = 0;

		if (v->n_frames == 0) break;

		if (monta(v, &v->frames[0], &n_au)) {
			v->ultimo_ts = v->frames[0].ts;
			v->hay_ultimo = 1;
			quita_frame(v, 0);

			if (v->esperando_clave) {
				if (!hay_clave(v->au, n_au)) {
					if (pedir_clave) *pedir_clave = 1;
					v->st.resyncs++;
					continue;       /* sigue tirando */
				}
				v->esperando_clave = 0;
			}

			v->st.fotogramas++;
			v->st.ultima_au = (u32)n_au;
			if (emitir) emitir(v->au, n_au, ud);
			continue;
		}

		/* No se puede montar todavia. Si ya ha esperado bastante, se le
		 * da por perdido y se vuelve a esperar clave: un fotograma P con
		 * una referencia rota NO llega al decodificador. */
		if (ahora_ms - v->frames[0].visto_ms > VJ_ESPERA_MS) {
			v->ultimo_ts = v->frames[0].ts;
			v->hay_ultimo = 1;
			quita_frame(v, 0);
			v->esperando_clave = 1;
			if (pedir_clave) *pedir_clave = 1;
			v->st.tirados++;
			continue;
		}

		break;   /* que lleguen mas paquetes */
	}
}
