# deps/ps3-shim — las cabeceras que a PSL1GHT le faltan

Esto existe para compilar **usrsctp** con `ppu-gcc`. No es código de GR33N y
no se incluye desde `source/`: sólo lo ve `build-usrsctp.sh`, y sólo mientras
compila esa biblioteca.

## Por qué

`deps/sonda-cabeceras.sh` preguntó por 43 cabeceras de sistema. PSL1GHT trae
27 y le faltan 16, y además su `sys/queue.h` existe **sin ninguna de las
macros `TAILQ`**, que usrsctp usa por todas partes (68 macros distintas de
`LIST`, `SLIST`, `STAILQ` y `TAILQ`).

Se preguntó por todas de golpe a propósito. Antes íbamos de una en una y cada
cabecera costaba un ciclo entero de copiar-compilar-pegar: `netinet/in.h`
(que sí estaba, pero fuera de la ruta de búsqueda), `sys/uio.h` (que no está,
pero el tipo `struct iovec` sí), y luego `net/if.h`. Tres vueltas para tres
líneas.

## Qué hay dentro, y de dónde sale

**Prestadas de FreeBSD, tal cual, con su licencia BSD-3-Clause intacta:**

| Fichero | Por qué prestada y no escrita |
|---|---|
| `sys/queue.h` | 1097 líneas de macros. La de glibc es la versión vieja de 4.4BSD y le faltan 19 de las que usrsctp necesita. Escribirla a mano sería copiarla peor. |
| `netinet/ip.h` | `struct ip` es una estructura de **cable**, con campos de bits que cambian de orden según el endianness. La de FreeBSD trae las dos variantes bien puestas. Escribirla de memoria en la plataforma donde el orden de bytes ya nos mordió esta semana sería tentar a la suerte. |
| `netinet/udp.h` | Igual, `struct udphdr`. |
| `netinet/in_systm.h` | Cuatro `typedef` y nada más, pero por coherencia. |

A las cuatro se les ha añadido, arriba y marcado, un bloque que define
`__packed`, `__aligned` y `__unused`. Vienen del `sys/cdefs.h` de FreeBSD, que
newlib no tiene, y sin ellos el compilador lee `__packed` como un nombre de
tipo. **`__packed` no es cosmético aquí**: sin él, el compilador puede meter
relleno entre campos y `struct ip` deja de calzar sobre los bytes que llegan.

**Escritas para esto, con sólo lo que usrsctp toca:**

- `net/if.h` — `IFNAMSIZ`, un `struct ifreq` recortado a `ifr_name` e
  `ifr_mtu`, y las dos declaraciones de conversión nombre↔índice.
- `ifaddrs.h` — `struct ifaddrs` y las dos funciones.
- `sys/uio.h` — el caso raro: la PS3 **tiene** `struct iovec` pero **no
  tiene** el fichero. Así que esta cabecera no define el tipo (lo decide una
  sonda del script, `-DGR33N_HAVE_IOVEC`) pero sí `UIO_MAXIOV`, que PSL1GHT
  tampoco trae y `user_socket.c:585` sí usa.
- `endian.h` y `sys/endian.h` — el orden de bytes de esta consola no es una
  pregunta. Se responde con las macros que predefine `ppu-gcc`, y con un
  `#error` si alguna vez dejara de ser cierto.
- `sys/ioctl.h` — sólo `SIOCGIFMTU` y la declaración de `ioctl`. **Sin
  implementación, a propósito**: ver abajo.

**Envolturas — no sustituyen a la de la consola, la envuelven con
`#include_next` y le añaden lo que falta:**

- `netinet/in.h` — IPv6 (los tipos, no el soporte), `IP_RECVDSTADDR` y
  `IPPORT_RESERVED`.
- `sys/socket.h` — `struct sockaddr_storage`, `SOCK_SEQPACKET` y **la familia
  `CMSG_*` entera**, que es la que se escondió esta vuelta (ver abajo).
- `sys/time.h` — `timercmp`, `timeradd` y `timersub`, las tres macros de BSD
  que usrsctp usa en nueve sitios.
- `errno.h` — `ERESTART`, y nada más.

**Vacías, y cada una dice por qué lo está:** `net/if_types.h`, `net/if_var.h`,
`net/route.h`, `sys/sysctl.h`, `sys/random.h`, `netinet/ip_icmp.h`,
`netinet/tcp.h`, `machine/in_cksum.h`.

## `ps3_stubs.c`, y la línea que separa un stub honesto de una mentira

Todas las funciones que faltan son de **enumerar interfaces de red**:
`getifaddrs`, `freeifaddrs`, `if_nametoindex`, `if_indextoname`.

usrsctp funciona en dos modos muy distintos. Con sockets de verdad, tiene que
saber qué direcciones locales hay y preguntar el MTU de cada interfaz. Con
**AF_CONN**, que es el nuestro, usrsctp **no toca la red**: le damos los
paquetes con `usrsctp_conninput()` y nos devuelve los que quiere mandar por
una función nuestra. Quien los pone en el cable es libpeer, por DTLS, sobre el
socket UDP que ya maneja GR33N.

En AF_CONN, preguntar por las interfaces locales no es que sea difícil: es que
**no significa nada**. La dirección que importa es la del par remoto, y ésa la
trae la negociación ICE.

Por eso los stubs devuelven *no hay nada* — que es la verdad — y no un error:
un fallo ahí haría que usrsctp se creyera que la máquina está rota, cuando lo
que pasa es que la pregunta no aplica.

**Y por eso `ioctl` se declara pero no se implementa.** Si algún camino
llegara a pedir el MTU por ahí, el enlazador lo diría con un símbolo sin
resolver. Eso es exactamente lo que se quiere: mejor un error de enlace ahora
que devolver en silencio un MTU inventado y descubrirlo tres semanas después
con paquetes fragmentados.

### El aviso funcionó, y se cobró un fichero entero

Ese `ioctl` apareció, junto con `socket`, `bind`, `setsockopt`, `recvmsg` y
`sendmsg`, en cuanto la biblioteca compiló del todo. **PSL1GHT no tiene
ninguno de esos nombres**: su API es `netSocket`, `netBind`, `netSetSockOpt`,
`netRecv`, `netSendTo`, `netClose`, que es la que GR33N usa desde el primer
día. Tal cual, el EBOOT no habría enlazado.

Los seis venían de tres sitios, y ninguno se arregló inventando una función:

- **`sctp_userspace_get_mtu_from_ifn`** abría un socket UDP para preguntarle
  el MTU a una interfaz. Ya era inalcanzable —el cuerpo entero cuelga de
  `if_indextoname()`, que aquí devuelve NULL siempre— así que bajo `GR33N_PS3`
  devuelve 1280, que es la misma respuesta que da el propio usrsctp cuando no
  sabe.
- **`user_recv_thread.c` no se compila.** Es el modo en el que usrsctp abre
  sus *propios* sockets, con hilos dedicados leyendo de ellos: 1500 líneas que
  no se ejecutan nunca con AF_CONN. Exporta exactamente dos símbolos
  —`recv_thread_init` y `recv_thread_destroy`, comprobado con `nm`— y aquí
  están vacíos. **Es lo que hace WebRTC** con `usrsctp_init_nothreads()`; la
  diferencia es que ese `nothreads` apaga también el hilo de temporizadores,
  que nosotros sí queremos porque es el que retransmite.
- **Los dos `sendmsg` de `user_socket.c`** cuelgan de descriptores que valen
  −1 siempre. Bajo `GR33N_PS3` se cambian por un aviso ruidoso: si eso sale
  alguna vez por el log, usrsctp ha decidido mandar por IP en lugar de por
  AF_CONN, y eso hay que verlo el día que pase.

Y la comprobación dejó de ser un párrafo pidiéndole al lector que mirase: el
script **busca esos nombres** en la lista de símbolos pendientes y se queja
solo. La versión anterior sí lo explicaba en prosa, se leyó, y los seis
estaban ahí de todos modos. *Un aviso que hay que leer no es una
comprobación.*

## Los tipos BSD los genera el script, no esta carpeta

Las cabeceras prestadas usan los nombres BSD de toda la vida: `u_int16_t`,
`u_char`, `caddr_t`. Newlib trae unos sí y otros no, y **cuáles exactamente
depende de cómo se compiló** — no es una lista que se pueda saber de memoria.

`build-usrsctp.sh` los prueba uno a uno y escribe `gr33n_bsdtypes.h` con
**sólo los que falten**, que luego mete con `-include`. Definirlos todos a lo
bruto chocaría con los que sí están (redefinir un `typedef` es error en C99,
no aviso), y definir de menos deja el mismo fallo.

Esto salió de que `in_systm.h` usa `u_int16_t` y `ppu-gcc` no lo conoce: 22
ficheros con el mismo error. Era la tercera vez que una cabecera prestada
traía una dependencia que la consola no tiene, así que se resolvió la **clase**
de problema en vez del caso.

`n_short`, `n_long` y `n_time` NO están en esa lista a propósito: los define
`netinet/in_systm.h`, que es su sitio, y ponerlos en los dos lados sería un
typedef duplicado.

## Y `BYTE_ORDER`, que es el peor de todos

`netinet/ip.h` declara `struct ip` **dos veces**, una por orden de bytes:

```c
#if BYTE_ORDER == LITTLE_ENDIAN
        u_char ip_hl:4, ip_v:4;
#endif
#if BYTE_ORDER == BIG_ENDIAN
        u_char ip_v:4, ip_hl:4;
#endif
```

En C, **un identificador desconocido dentro de un `#if` vale cero**. Si
`BYTE_ORDER` no está definido, las dos comparaciones dan `0 == 0` y se compilan
**las dos ramas**. Eso pasó: `duplicate member 'ip_v'`, 22 veces.

Ahí tuvimos suerte, porque los miembros chocaban y el compilador lo dijo. **El
mismo mecanismo, en una cabecera donde las dos ramas no chocaran, se habría
quedado con la de little-endian en silencio sobre una máquina big-endian.** Ese
es exactamente el fallo que este documento existe para evitar, producido por
una macro que falta.

Así que el script lo prueba y lo trae de la cabecera de la consola, y
`netinet/ip.h` lleva un `#error` que salta si no llega. Un fallo así no puede
volver a ser silencioso.

**Y la primera versión de esa sonda preguntó lo que no era.** Incluía
`<machine/endian.h>` y comprobaba que de ahí saliera `BYTE_ORDER` bien puesto.
Salía. Dijo *"la consola lo trae bien puesto"* — y los 22 ficheros fallaron
igual, porque **usrsctp no incluye esa cabecera en ningún momento**. La
pregunta buena no era *«se puede conseguir `BYTE_ORDER`»* sino *«está
`BYTE_ORDER` cuando `ip.h` se lee»*.

Ahora el script prueba `machine/endian.h`, `sys/endian.h` y `endian.h` por ese
orden, y mete la primera que funcione con `-include`, forzándola en todos los
ficheros. Se usan **los valores de la consola**, no unos inventados que
podrían no coincidir.

## Lo que falta, medido de una vez

`deps/sonda-tipos.sh` preguntó por 33 tipos, macros y funciones de sockets con
**las mismas opciones con las que compila el build**. PSL1GHT trae 28. Faltan:

| Falta | Dónde lo pone el shim |
|---|---|
| `struct sockaddr_storage` | envoltura de `sys/socket.h` |
| `struct in6_addr` | envoltura de `netinet/in.h` |
| `struct sockaddr_in6` | envoltura de `netinet/in.h` |
| `struct in_pktinfo` | envoltura de `netinet/in.h` |

Y trae **todo** lo demás, incluido lo que más podía doler: `struct msghdr`,
`struct cmsghdr` y las macros `CMSG_*` completas (que usrsctp usa en
`sendv`/`recvv`), `struct linger`, `socklen_t`, `MSG_EOR`, `SO_LINGER`,
`pthread_setname_np`. Eso es más de lo que cabía esperar de una consola de
2006.

`clock_gettime` salió en la lista de la sonda pero **usrsctp no lo llama en
ningún sitio** — cero coincidencias en todo `usrsctplib`. Fue una pregunta de
más, no un problema.

Siete vueltas costó llegar aquí, y todas dijeron lo mismo con distinto traje:
falta un tipo, en los 22 ficheros a la vez. La sonda debería haber sido lo
primero, no lo octavo.

## `netinet/in.h` es una envoltura, no un sustituto

PSL1GHT trae `netinet/in.h` y funciona. Lo que **no** trae es IPv6, y usrsctp
declara campos de tipo `struct in6_addr` y `struct sockaddr_in6` **sin
guardarlos tras `INET6`** — `user_inpcb.h:72` y `:77`, `usrsctp.h:152`,
`user_ip6_var.h:74`. Están dentro de uniones que se reservan enteras, así que
el tipo tiene que **existir** aunque no se use jamás. Compilar con `-DINET` a
secas no evita esas líneas: sólo evita el código que las mira.

Así que el shim usa **`#include_next`**, que es la herramienta exacta para
esto: continúa la búsqueda *después* del directorio donde se encontró el
fichero, o sea que trae la de PSL1GHT entera y luego le añade lo que falta.
Sin ese truco, poner un `netinet/in.h` en el árbol taparía la buena y nos
quedaríamos sin `sockaddr_in`, sin `htons` y sin todo lo demás.

Las dos estructuras están copiadas de FreeBSD (`sys/netinet6/in6.h`), no
escritas de memoria, y **comprobadas bajo qemu en PowerPC64 big-endian**:
`in6_addr` mide 16 bytes y `sockaddr_in6` mide 28. Eso importa porque usrsctp
reserva y copia estructuras que las contienen, y un tamaño distinto del que
espera su propio código es corrupción de memoria silenciosa.

`sin6_len` va delante porque la newlib de PSL1GHT es de la familia BSD y
usrsctp se compila con `HAVE_SIN6_LEN` — la misma razón por la que hizo falta
el parche de `sockaddr_conn`.

`sys/socket.h` es otra envoltura igual, y añade `struct sockaddr_storage`:
también copiada de FreeBSD y comprobada bajo qemu — **128 bytes, alineada a 8,
`ss_family` en el desplazamiento 1**. Los tres números importan: RFC 2553 fija
los dos primeros y el tercero confirma la disposición BSD.

### Y una que se resuelve sola

`user_recv_thread.c` se planta con un `#error` si no encuentra ni `IP_PKTINFO`
ni `IP_RECVDSTADDR` — las dos formas, la de Linux y la de BSD, de preguntar a
qué dirección venía un datagrama. No necesitamos ninguna, pero el fichero tiene
que compilar. La envoltura declara la de BSD **sólo si no hay ninguna de las
dos**, con un `#if` que no necesita sonda porque son macros y el preprocesador
sí puede preguntar por ellas.

## La vuelta en la que la sonda mintió

La sonda de tipos dijo que la consola traía `CMSG_DATA`, `CMSG_SPACE` y
`CMSG_LEN`. No las trae. El programa de prueba era éste:

```c
#include <sys/socket.h>
int main(void){ struct cmsghdr c; (void)CMSG_DATA(&c);
return (int)(CMSG_SPACE(4)+CMSG_LEN(4)); }
```

Y **compila aunque las macros no existan**. En C99, llamar a algo que no está
declarado es un *aviso*: el compilador se inventa `int CMSG_DATA()` y sigue
adelante. La sonda miraba el código de salida, veía cero, y decía `SI`.

Es la tercera vez esta semana que una sonda mide donde no es —antes fue
`BYTE_ORDER`, preguntándole a una cabecera que usrsctp no incluye, y
`sys/time.h`, preguntándole al sistema de ficheros en vez de al compilador— y
es la que más gracia tiene, porque la sonda existe justamente para no suponer.

Lo peor no es el ciclo perdido. Es adónde llevaba: `CMSG_DATA` y `CMSG_NXTHDR`
devuelven **punteros**, y una llamada implícita se supone que devuelve `int`.
En un binario de 32 bits como el de PSL1GHT, ese `int` mide lo mismo que el
puntero. Compila, enlaza, arranca, y `sctp_indata.c` hace
`memcpy(CMSG_DATA(cmh), ...)` sobre una dirección truncada dentro de la
consola. Sin aviso ninguno.

Arreglado en tres sitios:

- las dos sondas compilan con `-Werror=implicit-function-declaration
  -Werror=implicit-int`, que convierte ese aviso en el error que siempre debió
  ser;
- el `build-usrsctp.sh` compila la biblioteca con lo mismo, así que ninguna
  macro puede volver a esconderse detrás de una llamada implícita;
- hay una **segunda sonda que corre con las opciones de verdad** —con este
  árbol delante en la ruta, no con las cabeceras de PSL1GHT a secas— y dice
  para cada cosa si la pone la consola o la ponemos nosotros. La primera sonda
  no podía contestar por la segunda: no eran la misma pregunta.

Y una nota sobre las macros que faltan: **no siempre se quejan de sí mismas**.
`timercmp(&a, &b, >)` sin macro deja un `>` suelto como argumento de una
función, y el error que sale es

```
error: expected expression before '>' token
```

que no menciona `timercmp` por ningún lado y parece un fallo de sintaxis en el
código de usrsctp.

## Comprobado

Los 23 ficheros de usrsctp (más `ps3_stubs.c`) compilan limpios para
**PowerPC64 big-endian** con `powerpc64-linux-gnu-gcc`, poniendo este árbol
delante en la ruta de búsqueda para que gane a las cabeceras de la glibc, y
**con `-Werror=implicit-function-declaration` puesto**. No es `ppu-gcc` —otra
libc, otro sistema— pero el orden de bytes y la arquitectura son los mismos.

Las macros que añaden las envolturas están probadas **aparte y en ejecución**,
porque la glibc del PC las tapa: `wrtc/t_cmsg.c` se compila contra una consola
falsa que tiene `struct cmsghdr` y `struct timeval` pero a la que se le han
quitado las macros con `#undef`, o sea la situación que creemos que hay en la
PS3. Se ejecuta en `qemu-ppc64`, big-endian, y comprueba 26 cosas: la
aritmética de `CMSG_ALIGN`/`LEN`/`SPACE`/`DATA`, que recorrer una lista de dos
mensajes con `CMSG_FIRSTHDR`/`NXTHDR` no se sale del buffer ni se inventa un
tercero, el acarreo de microsegundos de `timeradd`/`timersub` —incluido el
caso negativo del que depende `sctp_timer.c:534`— y que `SOCK_SEQPACKET`,
`IPPORT_RESERVED` y `ERESTART` no chocan con nada.

Lo que **no** se puede comprobar en el PC: la interacción con las cabeceras
que PSL1GHT **sí** trae. Ahí manda la consola.
