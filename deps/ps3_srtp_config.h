/* GR33N - el config.h que libSRTP espera, escrito a mano para PS3.
 *
 * libSRTP lo genera normalmente autoconf o cmake mirando la maquina. Aqui
 * no corre ninguno de los dos, asi que va a mano, y eso significa que cada
 * linea de este fichero es una decision y no una deteccion.
 *
 * LA LINEA QUE IMPORTA ES WORDS_BIGENDIAN.
 *
 * libSRTP declara sus cabeceras de cable POR DUPLICADO, una version por
 * orden de bytes:
 *
 *     #ifndef WORDS_BIGENDIAN
 *     typedef struct { unsigned char cc:4; unsigned char x:1; ... }
 *     #else
 *     typedef struct { unsigned char version:2; unsigned char p:1; ... }
 *
 * Hace lo correcto, pero elige con esa macro y NO la deduce sola para esta
 * plataforma: solo se autodetecta para __APPLE__ con PowerPC. Sin definirla
 * compila SIN UN SOLO AVISO con las estructuras de little-endian sobre una
 * maquina big-endian, y todos los paquetes SRTP se leen mal. No hay error
 * ni warning: hay video que no descifra.
 *
 * El #error de abajo existe para que eso no pueda pasar en silencio.
 */

#ifndef PS3_SRTP_CONFIG_H
#define PS3_SRTP_CONFIG_H

/* --- lo unico que de verdad decide si esto funciona ------------------ */

#define WORDS_BIGENDIAN 1

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) && !defined(WORDS_BIGENDIAN)
#error "maquina big-endian y WORDS_BIGENDIAN sin definir: SRTP leeria mal todos los paquetes"
#endif
#if (__BYTE_ORDER__ != __ORDER_BIG_ENDIAN__) && defined(WORDS_BIGENDIAN)
#error "WORDS_BIGENDIAN definido en una maquina que no lo es"
#endif
#endif

/* PowerPC es RISC. No es cosmetico: con CPU_CISC, aes.c hace accesos de 32
 * bits sin alinear porque da por hecho que salen gratis. El PPU los tolera
 * pero los paga, y con -O2 GCC puede ademas dar por supuesta la
 * alineacion. La rama CPU_RISC de aes.c (lineas 1252-1390) trabaja byte a
 * byte y es la que corresponde. */
#define CPU_RISC 1

/* --- lo que trae newlib ---------------------------------------------- */

#define HAVE_STDLIB_H   1
#define HAVE_STRING_H   1
#define HAVE_STRINGS_H  1
#define HAVE_STDINT_H   1
#define HAVE_INTTYPES_H 1
#define HAVE_UNISTD_H   1
#define HAVE_SYS_TYPES_H 1

#define HAVE_INT8_T   1
#define HAVE_INT16_T  1
#define HAVE_INT32_T  1
#define HAVE_UINT8_T  1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1

/* --- lo que NO trae la consola --------------------------------------- */

/* NETINET/IN.H SI ESTA, y hace falta.
 *
 * La primera version de este fichero lo dio por ausente razonando que los
 * sockets de la PS3 no son BSD -son netBind/netRecv/netSend de lv2- y por
 * tanto las cabeceras tampoco estarian. Razonable y falso: PSL1GHT trae
 * <netinet/in.h>, y link.c de GR33N lleva semanas incluyendolo para htons.
 *
 * Sin esto, srtp.c no compila: usa ntohs para leer la longitud de la
 * extension de cabecera RTP y se queda sin declararla. Comprobado con el
 * cruzado de PowerPC: 17 de 18 ficheros compilaban y ese era el que no.
 *
 * (En big-endian ntohs es la identidad, asi que se podria definir a mano.
 * Pero si la cabecera de verdad esta ahi, usarla es mejor que escribir una
 * version propia que nadie va a revisar.) */
#define HAVE_NETINET_IN_H 1

/* Estas otras no se declaran porque no se han comprobado y libSRTP no las
 * necesita para lo que compilamos: sus programas de prueba, que se quedan
 * fuera de la lista de ficheros, son los unicos que abren sockets. */
/* HAVE_ARPA_INET_H     - sin comprobar */
/* HAVE_SYS_SOCKET_H    - sin comprobar */
/* HAVE_SOCKET          - no hace falta */
/* HAVE_INET_ATON       - no hace falta */
/* HAVE_INET_PTON       - no hace falta */

/* byteswap.h es de glibc. En newlib no esta, y sin ella libSRTP usa su
 * propia conversion, que es lo que queremos: una menos de la que fiarse. */
/* HAVE_BYTESWAP_H      - no */

/* HAVE_SIGACTION       - no hace falta, es para las pruebas */
/* HAVE_PCAP            - no */
/* HAVE_USLEEP          - si, pero solo lo usan las pruebas */

/* --- la criptografia ------------------------------------------------- */

/* SE USA EL mbedTLS QUE YA ESTA PROBADO EN LA CONSOLA.
 *
 * libSRTP trae su propio AES y su propio SHA1 (aes.c, sha1.c) y tambien
 * sabe delegar en mbedTLS (aes_icm_mbedtls.c, aes_gcm_mbedtls.c,
 * hmac_mbedtls.c). Lo segundo es mejor por dos motivos:
 *
 *   1. mbedTLS lleva semanas haciendo TLS 1.3 desde esta consola contra
 *      cinco servidores de Microsoft. Esta probado en big-endian de la
 *      forma que cuenta: funcionando.
 *   2. La alternativa es meter una SEGUNDA implementacion de AES sin
 *      probar en el mismo binario, en la plataforma donde el orden de
 *      bytes ya nos ha mordido una vez esta semana.
 *
 * GCM va incluido porque DTLS-SRTP puede negociar AEAD_AES_128_GCM ademas
 * del clasico AES_CM_128_HMAC_SHA1_80, y no sabemos cual pedira xCloud
 * hasta que lo veamos. Que sobre codigo es mas barato que descubrir a
 * mitad de un saludo DTLS que falta. */
#define MBEDTLS 1
#define GCM     1

/* --- avisos ---------------------------------------------------------- */

/* Sin esto, err.c intenta escribir a un fichero de registro. */
#define ERR_REPORTING_STDOUT 1

/* Lo pone autoconf a partir del nombre y la version del paquete, y
 * srtp_get_version_string() lo devuelve tal cual. No lo usa nadie mas, pero
 * sin el srtp.c no compila. */
#define PACKAGE_STRING  "libsrtp2 (GR33N/PS3)"
#define PACKAGE_VERSION "2.x"

#endif /* PS3_SRTP_CONFIG_H */
