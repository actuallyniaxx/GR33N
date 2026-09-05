/* GR33N - almacen persistente
 *
 * Ajustes y credenciales. Dos cosas distintas que resultan ser el mismo
 * codigo, y por eso se escribe una vez.
 *
 * DONDE ESCRIBE: no hay un sitio garantizado. Si la app esta instalada
 * como PKG existe su carpeta de juego; si arrancas un SELF suelto desde
 * multiMAN, no. Asi que storeInit() PRUEBA una lista de candidatos
 * escribiendo de verdad en cada uno y se queda con el primero que le deja.
 * Preguntar "existe esta carpeta" no vale: existir y poder escribir no son
 * lo mismo, y aqui lo unico que importa es lo segundo.
 *
 * No se crea ningun directorio a proposito: todos los candidatos son
 * rutas que ya existen en cualquier PS3. Menos cosas que puedan fallar.
 */

#ifndef GR33N_STORE_H
#define GR33N_STORE_H

#include <stdio.h>
#include <ppu-types.h>

/* Busca sitio donde escribir. Devuelve 0 si lo encontro. Se puede seguir
 * sin almacen: solo significa que nada sobrevive al apagado.
 *
 * self_path es argv[0]: la ruta del SELF que se esta ejecutando. De ahi
 * sale el primer candidato, que es el bueno — la carpeta donde te han
 * puesto, sin tener que adivinar como se llama. */
int storeInit(const char *self_path);

/* Ruta elegida, o "" si no hay. Para poder decirlo en el log. */
const char *storePath(void);

/* Blobs con nombre. name es solo el nombre del fichero, sin barras. */
int storeSave(const char *name, const void *data, u32 len);
int storeLoad(const char *name, void *data, u32 max, u32 *out_len);
int storeDelete(const char *name);

/* Acceso crudo, para ficheros que se recorren y se amplian en vez de
 * reescribirse enteros: la cache de caratulas son megabytes y volcarla
 * completa cada vez que llega una imagen no tiene sentido.
 *
 * Devuelve NULL si el almacen no esta listo. Cierra tu el fichero. */
FILE *storeOpen(const char *name, const char *mode);

/* --------------------------------------------------------------------- */
/* Credenciales                                                          */
/*                                                                       */
/* El flujo de xCloud es por codigo de dispositivo: sale un codigo en la  */
/* tele, el usuario lo mete en el movil, y a cambio nos dan un token que  */
/* hay que guardar. Sin esto habria que repetir el baile en cada arranque.*/
/* --------------------------------------------------------------------- */

#define STORE_TOKEN_MAX  8192

int  storeSaveToken(const void *data, u32 len);

/* Devuelve la longitud leida, o 0 si no hay token guardado. */
u32  storeLoadToken(void *data, u32 max);

void storeClearToken(void);

/* 1 si hay algo guardado. No dice si sigue siendo valido: eso solo lo
 * sabe el servidor. */
int  storeHasToken(void);

#endif /* GR33N_STORE_H */
