/* GR33N - cuanto se tarda en llegar a cada region de xCloud
 *
 * AQUI NO SE HACE PING, Y CONVIENE DECIRLO ANTES QUE NADA.
 *
 * Un ping de verdad es un ICMP echo, y para mandar un ICMP hace falta un
 * socket en crudo. lv2 no lo da: netSocket ofrece SOCK_STREAM y
 * SOCK_DGRAM y ahi se acaba la lista. Asi que "medidor de ping" es el
 * nombre de la funcion en la pantalla, no la descripcion de lo que pasa
 * por el cable.
 *
 * LO QUE SE MIDE DE VERDAD: el apreton de manos TCP contra el 443 del
 * host de la region.
 *
 *     nosotros  --- SYN ------------->  Azure
 *     nosotros  <-------- SYN+ACK ----  Azure     <- aqui vuelve connect()
 *     nosotros  --- ACK ------------->  Azure     (y esto ya no se espera)
 *
 * Eso es UNA ida y vuelta exacta, ni mas ni menos, y es la misma cifra
 * que net_tls.c ya saca en su paso "2/4 ok en N ms". No hace falta
 * inventar nada nuevo: la tuberia ya estaba medida, solo que contra un
 * host y no contra ocho.
 *
 * LO QUE **NO** ES, y esto importa mas que lo anterior:
 *
 *   - No es la latencia del juego. El 443 lo atiende la puerta de
 *     entrada de la region; la maquina que ejecuta el juego es otra, y
 *     el video viaja por UDP a otra direccion todavia. Lo que se mide es
 *     la DISTANCIA a la region, que es lo que decide la mayor parte del
 *     resultado, pero al numero de pantalla hay que sumarle codificar,
 *     descodificar, el vaiven de la red y el camino de vuelta.
 *
 *   - No es comparable con el ping de un juego de PC. Un juego mide UDP
 *     contra su propio servidor. Esto mide TCP contra un balanceador.
 *     Sirve para ordenar regiones entre si, que es justo para lo que
 *     esta puesto, y no para presumir de cifra.
 *
 * TRES MUESTRAS Y SE QUEDA EL MINIMO. El minimo, no la media: en una
 * medida de latencia el ruido solo suma. Una muestra alta puede ser la
 * red, el planificador de la PS3 o que Azure estaba ocupado; una muestra
 * baja no puede ser mas baja que la velocidad de la luz. El minimo de
 * tres es el suelo, y el suelo es lo que se quiere comparar.
 */

#ifndef GR33N_PING_H
#define GR33N_PING_H

#include <ppu-types.h>

#define PING_MUESTRAS   3      /* intentos por region                    */
#define PING_TIMEOUT_MS 2500   /* mas alla de esto, la region es inutil  */
#define PING_PAUSA_US   40000  /* entre intento e intento, por educacion */

typedef enum {
	PING_NADA = 0,   /* todavia no se ha medido nada       */
	PING_MIDIENDO,
	PING_HECHO
} pingState;

int  pingInit(void);
void pingShutdown(void);

/* Mide todas las regiones que authRegionsN() diga que hay. No bloquea:
 * el trabajo va en su propio hilo y los resultados van apareciendo de uno
 * en uno. Llamarlo con una medida en marcha no hace nada. */
void pingMedir(void);

pingState pingEstado(void);

/* Milisegundos hasta la region i, o 0 si todavia no se sabe.
 *
 * CERO NO ES CERO, es "sin medir", y quien lo pinte tiene que enseñar
 * "--" y no un cero: un cero se lee como "instantaneo", que es la lectura
 * mas optimista posible de no tener ni idea. */
u32 pingMs(int i);

/* 1 si esa region contesto, 0 si no se pudo medir. Se separa de pingMs
 * porque "no ha contestado" y "todavia no le ha tocado" son cosas
 * distintas y las dos dan 0 ms. */
int pingFallo(int i);

/* Cual es la mas rapida de las medidas, o -1 si no hay ninguna. */
int pingMejor(void);

/* Cuantas van medidas y de cuantas, para la barra de progreso. */
void pingProgreso(int *hechas, int *total);

#endif /* GR33N_PING_H */
