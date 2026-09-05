# Créditos y licencias

GR33N se distribuye bajo **GPL-3.0**. El texto completo está en `LICENSE`.

La razón de que sea GPL-3 y no algo más permisivo es concreta y está abajo:
el trabajo de **green-nx** es lo que hace viable la parte de WebRTC, es
GPL-3, y se usa como base en lugar de reescribirlo. Eso es una decisión, no
un descuido.

---

## green-nx — GPL-3.0

<https://github.com/rmrf404/green-nx>, de rmrf404 (cliente de xCloud para
Nintendo Switch)

De ahí sale, adaptado a PowerPC big-endian y a PSL1GHT:

- **`deps/libpeer-ps3.patch`**, que parte de su `libpeer-switch.patch`. Son
  41 hunks y prácticamente ninguno es específico de Switch: arreglan fallos
  de libpeer que se dan en cualquier plataforma y, sobre todo, **codifican
  cómo hay que hablarle a xCloud**.
- El diagnóstico del **`sockaddr_conn` con byte de longitud** de usrsctp,
  que se comieron en Switch con la misma familia de newlib. Sin ese apunte
  habrían sido varias tardes de corrupción de memoria en una PS3 sin
  depurador.
- La observación de que el depaquetizador H.264 de libpeer **corrompe
  fotogramas ante cualquier reordenamiento o pérdida** y hay que puentearlo
  y hacer el reensamblado uno mismo.

Lo que **no** viene de ellos, porque no les hacía falta: toda la capa de
big-endian. La Switch es little-endian y la PS3 no, así que las dos trampas
de orden de bytes del depaquetizador —el código de arranque Annex-B al revés
y los campos de bits invertidos— se encontraron aquí, compilando su código
para PowerPC64 y comparando salidas byte a byte.

## libpeer — MIT

<https://github.com/sepfy/libpeer>, de sepfy. Copyright (c) 2021.

Clavado en el commit `9319aa434cb9e893faed0293ba9d2a21eca59c8b`. No es
capricho: master rompió los 41 hunks del parche en agosto de 2026.

## usrsctp — BSD-3-Clause

<https://github.com/sctplab/usrsctp>. Copyright (c) los contribuidores de
FreeBSD.

`deps/ps3-shim/` incluye además cuatro cabeceras **copiadas literalmente de
FreeBSD** con su licencia BSD-3-Clause intacta: `sys/queue.h`,
`netinet/ip.h`, `netinet/udp.h` y `netinet/in_systm.h`. Se copian y no se
escriben a mano a propósito — son estructuras de cable con campos de bits
que cambian de orden según el endianness, y en la plataforma donde eso ya ha
mordido dos veces, escribirlas de memoria sería tentar a la suerte.

## libSRTP — BSD-3-Clause

<https://github.com/cisco/libsrtp>. Copyright (c) Cisco Systems.

## mbedTLS — Apache-2.0

<https://github.com/Mbed-TLS/mbedtls>. Versión 3.6.4.

**No se le aplica ningún parche.** El temporizador de DTLS se engancha por
`MBEDTLS_TIMING_ALT`, que es el mecanismo que la propia biblioteca ofrece
para esto. `deps/mbedtls-ps3/timing_alt.h` y `source/dtls_timer.c` son
nuestros.

## Opus — BSD-3-Clause

<https://opus-codec.org/>, de la Fundacion Xiph.Org. Version 1.4.

Compilado en **punto fijo** (`FIXED_POINT`). La PS3 tiene FPU de sobra,
pero la lista de fuentes se saca de los propios `celt_sources.mk` /
`silk_sources.mk` de Opus llamando a `make`, en vez de mantener a mano una
lista paralela que se quedaria atras a la primera version nueva.

## cJSON — MIT

<https://github.com/DaveGamble/cJSON>. En `deps/cjson/`.

## green-vita — de donde salio la idea

<https://github.com/Day-OS/green-vita>, de Day-OS. El cliente de xCloud
para PS Vita. No se usa codigo suyo, pero fue lo que dejo claro que esto
cabia en una consola vieja de Sony.

## PSL1GHT

<https://github.com/ps3dev/PSL1GHT>. El SDK abierto de PS3 sobre el que se
compila todo esto.

---

## Qué significa GPL-3 aquí, en corto

Si distribuyes GR33N —el `.pkg`, el `.self`, o un binario modificado— tienes
que ofrecer el código fuente correspondiente bajo la misma licencia. Enlazar
bibliotecas MIT, BSD y Apache-2.0 desde un programa GPL-3 es compatible; al
revés no lo sería, y por eso la licencia del conjunto es la más restrictiva
de las que entran.
