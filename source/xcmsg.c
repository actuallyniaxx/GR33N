/* GR33N - los mensajes de los canales de datos de xCloud. Ver xcmsg.h. */

#include <stdio.h>
#include <string.h>

#include "xcmsg.h"

/* --------------------------------------------------------------------- */
/* JSON en texto                                                         */
/* --------------------------------------------------------------------- */

/* Los constantes van como literales. No hay nada que rellenar y una
 * cadena fija no se puede equivocar al formatearse. */

const char *xcHandshake(void)
{
	return "{\"type\":\"Handshake\",\"version\":\"messageV1\","
	       "\"id\":\"be0bfc6d-1e83-4c8a-90ed-fa8601c5a179\",\"cv\":\"0\"}";
}

const char *xcAuthorizationRequest(void)
{
	/* La accessKey es la del cliente web, literal. No es un secreto
	 * nuestro ni identifica a nadie: es la constante que el servicio
	 * espera para reconocer al cliente. */
	return "{\"message\":\"authorizationRequest\","
	       "\"accessKey\":\"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E\"}";
}

const char *xcKeyframeRequested(void)
{
	return "{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}";
}

int xcIsHandshakeAck(const char *payload, size_t len)
{
	const char *p;

	if (payload == NULL || len == 0) return 0;

	/* Se busca la pareja "type":"HandshakeAck" sin analizar el JSON
	 * entero. Es un mensaje de forma conocida y fija; montar un
	 * analizador para mirar un campo seria mas codigo del que hace
	 * falta y mas sitios donde equivocarse.
	 *
	 * Se busca el VALOR y no solo la palabra: "HandshakeAck" a secas
	 * podria aparecer en cualquier otro campo, y confundir un mensaje
	 * con otro aqui manda toda la secuencia de arranque antes de tiempo. */
	for (p = payload; len >= 12 && *p; p++, len--) {
		if (strncmp(p, "HandshakeAck", 12) != 0) continue;

		/* Y que venga precedido de comillas, para no casar con
		 * "HandshakeAckSomethingElse" ni con un trozo de otra cosa. */
		if (p > payload && p[-1] == '"' && p[12] == '"')
			return 1;
	}

	return 0;
}

size_t xcGamepadChanged(char *out, size_t n, int indice, int anadido)
{
	int r = snprintf(out, n,
	                 "{\"message\":\"gamepadChanged\",\"gamepadIndex\":%d,"
	                 "\"wasAdded\":%s}",
	                 indice, anadido ? "true" : "false");

	return (r > 0 && (size_t)r < n) ? (size_t)r : 0;
}

size_t xcResolution(char *out, size_t n, const char *alias)
{
	/* El selector de resolucion del cliente oficial. Sin esto el
	 * servidor elige por su cuenta y en titulos que lo soportan se va a
	 * 1440p para clientes que parecen de sobremesa. Una PS3 no sostiene
	 * eso ni de lejos. */
	int r = snprintf(out, n,
	                 "{\"message\":\"userRequestedResolutionUpdate\","
	                 "\"resolutionAlias\":\"%s\"}", alias);

	return (r > 0 && (size_t)r < n) ? (size_t)r : 0;
}

/* Mete `contenido` como CADENA dentro del sobre "Message".
 *
 * El contenido es JSON, y ahi dentro va escapado: cada comilla pasa a \"
 * Es la misma doble codificacion que en /ice. Se hace a mano porque lo
 * que se escapa son cadenas que escribimos nosotros aqui al lado -- solo
 * llevan comillas, sin barras ni caracteres de control-- y asi este
 * fichero no depende de cJSON y se puede probar suelto. */
static size_t envolver(char *out, size_t n, const char *ruta,
                       const char *contenido, int contador)
{
	size_t w = 0;
	const char *p;
	int r;

	r = snprintf(out, n, "{\"type\":\"Message\",\"content\":\"");
	if (r < 0 || (size_t)r >= n) return 0;
	w = (size_t)r;

	for (p = contenido; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (w + 2 >= n) return 0;
			out[w++] = '\\';
			out[w++] = *p;
		} else {
			if (w + 1 >= n) return 0;
			out[w++] = *p;
		}
	}

	r = snprintf(out + w, n - w,
	             "\",\"id\":\"5c5f2b40-0000-4000-8000-%012d\","
	             "\"target\":\"%s\",\"cv\":\"\"}",
	             1000 + contador, ruta);
	if (r < 0 || (size_t)r >= n - w) return 0;

	return w + (size_t)r;
}

size_t xcStartup(char *out, size_t n, int idx,
                 int ancho, int alto, int kbps, int fps)
{
	char cont[512];
	const char *ruta;
	int r;

	switch (idx) {
	case 0:
		ruta = "/streaming/systemUi/configuration";
		r = snprintf(cont, sizeof(cont),
		             "{\"version\":[0,2,0],\"systemUis\":[]}");
		break;
	case 1:
		ruta = "/streaming/properties/clientappinstallidchanged";
		r = snprintf(cont, sizeof(cont),
		             "{\"clientAppInstallId\":"
		             "\"c97d7ee0-73b2-4239-bf1d-9d805a338429\"}");
		break;
	case 2:
		ruta = "/streaming/characteristics/orientationchanged";
		r = snprintf(cont, sizeof(cont), "{\"orientation\":0}");
		break;
	case 3:
		ruta = "/streaming/characteristics/touchinputenabledchanged";
		r = snprintf(cont, sizeof(cont),
		             "{\"touchInputEnabled\":false}");
		break;
	case 4:
		/* El que de verdad importa: aqui se declara lo que aguanta la
		 * consola, y es la unica palanca que tenemos sobre la calidad
		 * que manda el servidor. */
		ruta = "/streaming/characteristics/clientdevicecapabilities";
		r = snprintf(cont, sizeof(cont),
		             "{\"supportsCustomResolution\":true,"
		             "\"supportsHevc\":false,\"supportsHdr\":false,"
		             "\"supportsFps\":%d,\"maxWidth\":%d,"
		             "\"maxHeight\":%d,\"maxBitrateKbps\":%d,"
		             "\"video\":{\"width\":%d,\"height\":%d,"
		             "\"maxWidth\":%d,\"maxHeight\":%d,"
		             "\"maxBitrateKbps\":%d}}",
		             fps, ancho, alto, kbps,
		             ancho, alto, ancho, alto, kbps);
		break;
	case 5:
		ruta = "/streaming/characteristics/dimensionschanged";
		r = snprintf(cont, sizeof(cont),
		             "{\"horizontal\":%d,\"vertical\":%d,"
		             "\"preferredWidth\":%d,\"preferredHeight\":%d,"
		             "\"safeAreaLeft\":0,\"safeAreaTop\":0,"
		             "\"safeAreaRight\":%d,\"safeAreaBottom\":%d,"
		             "\"supportsCustomResolution\":true}",
		             ancho, alto, ancho, alto, ancho, alto);
		break;
	default:
		return 0;
	}

	if (r < 0 || (size_t)r >= sizeof(cont)) return 0;

	return envolver(out, n, ruta, cont, idx);
}

/* --------------------------------------------------------------------- */
/* Binario                                                               */
/* --------------------------------------------------------------------- */

/* De menos a mas significativo, a mano. Nada de memcpy sobre la variable:
 * eso sale al reves en esta maquina. */
static void le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)((v >> 8) & 0xff);
}

static void le32(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)((v >> 8) & 0xff);
	p[2] = (u8)((v >> 16) & 0xff);
	p[3] = (u8)((v >> 24) & 0xff);
}

/* Un double de 8 bytes en little-endian.
 *
 * El double se pasa a enteros por su representacion, no por su valor: con
 * una union se leen los bytes tal como los tiene la maquina --que en la
 * PS3 son big-endian, IEEE 754 igual que en todas partes-- y se escriben
 * en orden inverso. Asi el resultado es el mismo aqui y en el PC, que es
 * lo que permite probarlo fuera de la consola. */
static void le_double(u8 *p, double v)
{
	union { double d; u8 b[8]; } u;
	int i;
	const u8 *src;
	u8 nativo[8];

	u.d = v;
	memcpy(nativo, u.b, 8);

	/* Se detecta el orden de la maquina una vez, en vez de suponerlo. */
	{
		union { u16 s; u8 b[2]; } prueba;
		prueba.s = 0x0102;
		src = nativo;
		if (prueba.b[0] == 0x01) {         /* big-endian */
			for (i = 0; i < 8; i++) p[i] = src[7 - i];
			return;
		}
	}

	for (i = 0; i < 8; i++) p[i] = src[i];
}

/* Los ejes llegan en milesimas (-1000..1000) y salen en el rango de un
 * entero de 16 bits con signo. Se recorta a proposito: un stick viejo
 * puede pasarse del tope y desbordar aqui seria dar la vuelta al signo,
 * o sea el mando yendo al lado contrario. */
static s16 eje(s32 milesimas)
{
	long v;

	if (milesimas >  1000) milesimas =  1000;
	if (milesimas < -1000) milesimas = -1000;

	v = ((long)milesimas * 32767L) / 1000L;
	return (s16)v;
}

static u16 gatillo(s32 milesimas)
{
	long v;

	if (milesimas < 0)    milesimas = 0;
	if (milesimas > 1000) milesimas = 1000;

	v = ((long)milesimas * 65535L) / 1000L;
	return (u16)v;
}

size_t xcInputMetadata(u8 *out, size_t n, u32 sec, u8 max_toques)
{
	if (out == NULL || n < XC_METADATA_LEN) return 0;

	le16(out + 0, 8);          /* tipo de informe: metadatos de cliente */
	le32(out + 2, sec);
	le_double(out + 6, 0.0);
	out[14] = max_toques;

	return XC_METADATA_LEN;
}

size_t xcInputGamepad(u8 *out, size_t n, u32 sec, double ms,
                      const xcPad *pad)
{
	if (out == NULL || pad == NULL || n < XC_GAMEPAD_LEN) return 0;

	le16(out + 0, 2);          /* tipo de informe: mando */
	le32(out + 2, sec);
	le_double(out + 6, ms);

	out[14] = 1;               /* un solo mando en este informe */
	out[15] = pad->indice;

	le16(out + 16, pad->botones);

	/* El eje vertical va INVERTIDO respecto a lo que da la consola: en
	 * xCloud arriba es positivo. */
	le16(out + 18, (u16)eje(pad->lx));
	le16(out + 20, (u16)eje(-pad->ly));
	le16(out + 22, (u16)eje(pad->rx));
	le16(out + 24, (u16)eje(-pad->ry));

	le16(out + 26, gatillo(pad->lt));
	le16(out + 28, gatillo(pad->rt));

	le32(out + 30, 1);         /* identificador de unidad fisica */

	/* Y estos cuatro van en BIG-endian, tal cual, tambien en el cliente
	 * de referencia. No es una errata de nadie: ese campo va asi. */
	out[34] = 0;
	out[35] = 0;
	out[36] = 0;
	out[37] = 1;

	return XC_GAMEPAD_LEN;
}
