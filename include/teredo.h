/* GR33N - lo que hay que entender de los candidatos que manda xCloud.
 *
 * POR QUE ESTO ES UN FICHERO APARTE Y NO ESTA DENTRO DE session.c.
 *
 * El intercambio de SDP e ICE vive en session.c, que es donde estan
 * ses_call() y el turno de TLS. Pero estas tres funciones no tocan la red
 * ni la consola: son aritmetica sobre cadenas. Y en este proyecto todo lo
 * que es aritmetica pura se compila y se ejecuta en el PC bajo ASan
 * -- dechunk, find_header, textSlice, el anillo de descripciones, el
 * temporizador de DTLS-- porque un fallo aqui descubierto en la consola
 * cuesta una ronda entera y no dice donde esta.
 *
 * Sacarlas del fichero grande es lo unico que permite eso.
 *
 * QUE ES TEREDO Y POR QUE NOS IMPORTA.
 *
 * xCloud NO da su direccion buena en la respuesta SDP. La gotea despues
 * por {sessionPath}/ice, y cuando llega no es una IPv4: es una direccion
 * Teredo (RFC 4380), que es una IPv6 con una IPv4 y un puerto metidos
 * dentro. libpeer se compila aqui con CONFIG_IPV6 a cero, asi que hay que
 * sacar la IPv4 de ahi dentro o no hay con quien hablar.
 *
 * El primer candidato que manda xCloud es ademas relleno: un 13.104.x con
 * prioridad 100 que NO contesta a STUN. Conformarse con el es quedarse
 * sin conexion, asi que hace falta poder mirar la prioridad.
 */

#ifndef GR33N_TEREDO_H
#define GR33N_TEREDO_H

#include <stddef.h>

/* Saca la IPv4 y el puerto de una direccion Teredo en forma expandida.
 *
 * Devuelve 1 si lo ha conseguido y 0 si esa direccion no es Teredo. En
 * ese caso no toca nada de lo que le pasan.
 *
 * `ipv4` se rellena con la forma "a.b.c.d". Necesita 16 bytes.
 */
int teredoDecode(const char *addr6, char *ipv4, size_t ipv4_n, int *port);

/* Normaliza una linea de candidato tal como viene del servidor.
 *
 * Le quita el "a=" de delante si lo trae, recorta espacios y retornos de
 * carro del final, y comprueba que empiece por "candidate:". Devuelve 1
 * si lo de dentro es un candidato utilizable, 0 si no.
 *
 * Existe porque xCloud manda las dos formas -- con "a=" y sin el-- y
 * porque un '\r' al final se lo come libpeer sin rechistar y luego no
 * casa nada.
 */
int candNormaliza(const char *linea, char *fuera, size_t fuera_n);

/* La direccion de un candidato ya normalizado, que es el campo 5.
 *
 *   candidate:<fundacion> <comp> <proto> <prioridad> <DIRECCION> <puerto> ...
 *
 * Devuelve 1 y rellena `dir` y `puerto`; 0 si la linea no tiene forma de
 * candidato.
 */
int candDireccion(const char *cand, char *dir, size_t dir_n, int *puerto);

/* La prioridad, que es el campo 4. Devuelve 0 si no se puede leer.
 *
 * Con esto se distingue el relleno (prioridad 100) del candidato de
 * verdad, que es lo unico que evita quedarse esperando a un servidor que
 * nunca iba a contestar. */
unsigned long candPrioridad(const char *cand);

#endif /* GR33N_TEREDO_H */
