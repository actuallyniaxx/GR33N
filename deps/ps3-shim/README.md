# deps/ps3-shim - the headers PSL1GHT is missing

This exists to compile **usrsctp** with `ppu-gcc`. It isn't GR33N code and
it isn't included from `source/`: only `build-usrsctp.sh` sees it, and only
while it's building that library.

## Why

`deps/sonda-cabeceras.sh` probed for 43 system headers. PSL1GHT provides 27
and is missing 16, and on top of that its `sys/queue.h` exists **without
any of the `TAILQ` macros**, which usrsctp uses all over the place (68
different macros across `LIST`, `SLIST`, `STAILQ` and `TAILQ`).

All of them were asked about at once, on purpose. Before that we went one
at a time, and each header cost a whole copy-compile-paste cycle:
`netinet/in.h` (which did exist, but outside the search path), `sys/uio.h`
(which doesn't exist, but the `struct iovec` type does), and then
`net/if.h`. Three rounds for three lines.

## What's inside, and where it comes from

**Borrowed from FreeBSD, as-is, with its BSD-3-Clause licence intact:**

| File | Why borrowed rather than written |
|---|---|
| `sys/queue.h` | 1097 lines of macros. glibc's is the old 4.4BSD version and is missing 19 of the ones usrsctp needs. Writing it by hand would just be copying it worse. |
| `netinet/ip.h` | `struct ip` is a **wire** structure, with bitfields that change order depending on endianness. FreeBSD's has both variants laid out correctly. Writing it from memory, on the platform where byte order already bit us this week, would be tempting fate. |
| `netinet/udp.h` | Same thing, `struct udphdr`. |
| `netinet/in_systm.h` | Four `typedef`s and nothing else, but for consistency. |

All four have had a block added at the top, clearly marked, that defines
`__packed`, `__aligned` and `__unused`. These come from FreeBSD's
`sys/cdefs.h`, which newlib doesn't have, and without them the compiler
reads `__packed` as a type name. **`__packed` is not cosmetic here**:
without it the compiler can insert padding between fields, and `struct ip`
stops lining up with the bytes coming in over the wire.

**Written for this, with only what usrsctp touches:**

- `net/if.h` - `IFNAMSIZ`, a `struct ifreq` trimmed down to `ifr_name` and
  `ifr_mtu`, and the two name<->index conversion declarations.
- `ifaddrs.h` - `struct ifaddrs` and the two functions.
- `sys/uio.h` - the odd case: the PS3 **has** `struct iovec` but **does
  not have** the file. So this header doesn't define the type (a probe in
  the script decides that, `-DGR33N_HAVE_IOVEC`) but does define
  `UIO_MAXIOV`, which PSL1GHT doesn't bring either and `user_socket.c:585`
  does use.
- `endian.h` and `sys/endian.h` - this console's byte order isn't up for
  debate. It's answered with the macros `ppu-gcc` predefines, plus an
  `#error` in case that ever stops being true.
- `sys/ioctl.h` - just `SIOCGIFMTU` and the declaration of `ioctl`. **No
  implementation, on purpose**: see below.

**Wrappers - these don't replace the console's own header, they wrap it
with `#include_next` and add what's missing:**

- `netinet/in.h` - IPv6 (the types, not support for it), `IP_RECVDSTADDR`
  and `IPPORT_RESERVED`.
- `sys/socket.h` - `struct sockaddr_storage`, `SOCK_SEQPACKET`, and **the
  whole `CMSG_*` family**, which is the one that hid on us this time round
  (see below).
- `sys/time.h` - `timercmp`, `timeradd` and `timersub`, the three BSD
  macros usrsctp uses in nine places.
- `errno.h` - `ERESTART`, and nothing else.

**Empty, and each one says why it's empty:** `net/if_types.h`,
`net/if_var.h`, `net/route.h`, `sys/sysctl.h`, `sys/random.h`,
`netinet/ip_icmp.h`, `netinet/tcp.h`, `machine/in_cksum.h`.

## `ps3_stubs.c`, and the line between an honest stub and a lie

All the missing functions are about **enumerating network interfaces**:
`getifaddrs`, `freeifaddrs`, `if_nametoindex`, `if_indextoname`.

usrsctp works in two very different modes. With real sockets, it has to
know what local addresses exist and ask each interface for its MTU. With
**AF_CONN**, which is the one we use, usrsctp **never touches the
network**: we hand it packets with `usrsctp_conninput()` and it hands back
the ones it wants to send through a function of ours. What actually puts
them on the wire is libpeer, over DTLS, on the UDP socket GR33N already
manages.

In AF_CONN, asking about local interfaces isn't hard, it's that **it
doesn't mean anything**. The address that matters is the remote peer's,
and that comes from the ICE negotiation.

That's why the stubs return *there is nothing here* - which is the truth -
rather than an error: a failure there would make usrsctp believe the
machine is broken, when what's actually happening is that the question
doesn't apply.

**And that's why `ioctl` is declared but not implemented.** If some code
path ever tried to ask for the MTU that way, the linker would say so with
an unresolved symbol. That's exactly what we want: better a link error now
than silently returning a made-up MTU and finding out three weeks later,
from fragmented packets.

### The warning worked, and it cost us a whole file

That `ioctl` turned up, along with `socket`, `bind`, `setsockopt`,
`recvmsg` and `sendmsg`, the moment the library compiled all the way
through. **PSL1GHT has none of those names**: its API is `netSocket`,
`netBind`, `netSetSockOpt`, `netRecv`, `netSendTo`, `netClose`, which is
what GR33N has used from day one. As it stood, the EBOOT would not have
linked.

The six came from three places, and none of them were fixed by inventing
a function:

- **`sctp_userspace_get_mtu_from_ifn`** opened a UDP socket to ask an
  interface for its MTU. It was already unreachable - the whole body
  hangs off `if_indextoname()`, which here always returns NULL - so under
  `GR33N_PS3` it returns 1280, the same answer usrsctp itself gives when
  it doesn't know.
- **`user_recv_thread.c` doesn't get compiled.** That's the mode where
  usrsctp opens its *own* sockets, with dedicated threads reading from
  them: 1500 lines that never run under AF_CONN. It exports exactly two
  symbols - `recv_thread_init` and `recv_thread_destroy`, checked with
  `nm` - and here they're empty. **This is what WebRTC does** with
  `usrsctp_init_nothreads()`; the difference is that `nothreads` also
  turns off the timer thread, which we do want, because that's the one
  that retransmits.
- **The two `sendmsg` calls in `user_socket.c`** hang off descriptors
  that are always -1. Under `GR33N_PS3` they're swapped for a loud
  warning: if that ever comes out in the log, usrsctp has decided to send
  over IP instead of over AF_CONN, and that needs looking at the day it
  happens.

And the check stopped being a paragraph asking the reader to go look: the
script **searches for those names** in the list of outstanding symbols and
complains on its own. The previous version did explain it in prose, it was
read, and the six were there anyway. *A warning you have to read is not a
check.*

## The BSD types are generated by the script, not this folder

The borrowed headers use the good old BSD names: `u_int16_t`, `u_char`,
`caddr_t`. newlib provides some and not others, and **exactly which ones
depends on how it was built** - it's not a list you can just know from
memory.

`build-usrsctp.sh` tests them one by one and writes `gr33n_bsdtypes.h`
with **only the ones that are missing**, which then gets pulled in with
`-include`. Bluntly defining all of them would clash with the ones that do
exist (redefining a `typedef` is an error in C99, not a warning), and
defining too few leaves the same failure.

This came out of `in_systm.h` using `u_int16_t`, which `ppu-gcc` doesn't
know: 22 files with the same error. It was the third time a borrowed
header had brought in a dependency the console doesn't have, so this time
the **class** of problem got fixed instead of the one instance.

`n_short`, `n_long` and `n_time` are deliberately NOT on that list:
`netinet/in_systm.h` defines them, which is where they belong, and having
them on both sides would be a duplicate typedef.

## And `BYTE_ORDER`, which is the worst of the lot

`netinet/ip.h` declares `struct ip` **twice**, once per byte order:

```c
#if BYTE_ORDER == LITTLE_ENDIAN
        u_char ip_hl:4, ip_v:4;
#endif
#if BYTE_ORDER == BIG_ENDIAN
        u_char ip_v:4, ip_hl:4;
#endif
```

In C, **an unknown identifier inside a `#if` evaluates to zero**. If
`BYTE_ORDER` isn't defined, both comparisons come out `0 == 0` and **both
branches** get compiled. That's exactly what happened: `duplicate member
'ip_v'`, 22 times.

We got lucky there, because the members clashed and the compiler said so.
**The same mechanism, in a header where the two branches didn't clash,
would have silently settled on the little-endian one, on a big-endian
machine.** That is exactly the failure this document exists to prevent,
produced by a single missing macro.

So the script tests for it and pulls it from the console's own header,
and `netinet/ip.h` carries an `#error` that fires if it doesn't arrive. A
failure like that cannot be allowed to be silent again.

**And the first version of that probe asked the wrong question.** It
included `<machine/endian.h>` and checked that `BYTE_ORDER` came out of it
set correctly. It did. It said *"the console has it set correctly"* - and
the 22 files failed all the same, because **usrsctp never includes that
header at all**. The right question wasn't *"can `BYTE_ORDER` be
obtained"* but *"is `BYTE_ORDER` set by the time `ip.h` is read"*.

Now the script tries `machine/endian.h`, `sys/endian.h` and `endian.h` in
that order, and force-includes the first one that works with `-include`,
across every file. That way it uses **the console's own values**, not
made-up ones that might not match.

## What's missing, measured once and for all

`deps/sonda-tipos.sh` probed for 33 socket types, macros and functions,
using **the same flags the build compiles with**. PSL1GHT provides 28.
Missing:

| Missing | Where the shim provides it |
|---|---|
| `struct sockaddr_storage` | `sys/socket.h` wrapper |
| `struct in6_addr` | `netinet/in.h` wrapper |
| `struct sockaddr_in6` | `netinet/in.h` wrapper |
| `struct in_pktinfo` | `netinet/in.h` wrapper |

And it provides **everything** else, including the parts that could have
hurt the most: `struct msghdr`, `struct cmsghdr` and the complete
`CMSG_*` macros (which usrsctp uses in `sendv`/`recvv`), `struct linger`,
`socklen_t`, `MSG_EOR`, `SO_LINGER`, `pthread_setname_np`. That's more
than you would expect from a 2006 console.

`clock_gettime` came up on the probe's list but **usrsctp never calls it
anywhere** - zero matches across the whole of `usrsctplib`. It was one
question too many, not a real problem.

It took seven rounds to get here, and every one of them said the same
thing in different clothes: a type is missing, in all 22 files at once.
The probe should have been the first thing done, not the eighth.

## `netinet/in.h` is a wrapper, not a replacement

PSL1GHT provides `netinet/in.h` and it works. What it does **not**
provide is IPv6, and usrsctp declares fields of type `struct in6_addr`
and `struct sockaddr_in6` **without guarding them behind `INET6`** -
`user_inpcb.h:72` and `:77`, `usrsctp.h:152`, `user_ip6_var.h:74`. They
sit inside unions that get reserved whole, so the type has to **exist**
even if it's never used. Compiling with plain `-DINET` doesn't avoid
those lines: it only avoids the code that looks at them.

So the shim uses **`#include_next`**, which is exactly the right tool for
this: it continues the search *after* the directory where the file was
found, so it pulls in the whole of PSL1GHT's own header and then adds
what's missing on top. Without that trick, putting a `netinet/in.h` in the
tree would shadow the real one and we'd lose `sockaddr_in`, `htons`, and
everything else.

Both structures are copied from FreeBSD (`sys/netinet6/in6.h`), not
written from memory, and **checked under qemu on big-endian PowerPC64**:
`in6_addr` is 16 bytes and `sockaddr_in6` is 28. That matters because
usrsctp allocates and copies structures that contain them, and a size
different from what its own code expects is silent memory corruption.

`sin6_len` comes first because PSL1GHT's newlib is from the BSD family
and usrsctp is built with `HAVE_SIN6_LEN` - the same reason the
`sockaddr_conn` patch was needed.

`sys/socket.h` is another wrapper of the same kind, and it adds `struct
sockaddr_storage`: also copied from FreeBSD and checked under qemu -
**128 bytes, aligned to 8, `ss_family` at offset 1**. All three numbers
matter: RFC 2553 fixes the first two and the third confirms the BSD
layout.

### And one that resolves itself

`user_recv_thread.c` stops dead with an `#error` if it finds neither
`IP_PKTINFO` nor `IP_RECVDSTADDR` - the Linux and BSD ways, respectively,
of asking which address a datagram arrived on. We don't need either one,
but the file still has to compile. The wrapper declares the BSD one
**only if neither is present**, with an `#if` that needs no probe,
because these are macros and the preprocessor can ask about them
directly.

## The round where the probe lied

The types probe said the console provided `CMSG_DATA`, `CMSG_SPACE` and
`CMSG_LEN`. It does not. The test program was this:

```c
#include <sys/socket.h>
int main(void){ struct cmsghdr c; (void)CMSG_DATA(&c);
return (int)(CMSG_SPACE(4)+CMSG_LEN(4)); }
```

And **it compiles even when the macros don't exist**. In C99, calling
something that hasn't been declared is a *warning*: the compiler makes up
`int CMSG_DATA()` and carries on. The probe looked at the exit code, saw
zero, and said `YES`.

This is the third time this week that a probe measured the wrong thing -
first it was `BYTE_ORDER`, asking a header usrsctp never includes, then
`sys/time.h`, asking the filesystem instead of the compiler - and it's
the funniest one, because the whole point of the probe is to not assume.

The worst part isn't the wasted cycle. It's where it led: `CMSG_DATA` and
`CMSG_NXTHDR` return **pointers**, and an implicit call is assumed to
return `int`. In a 32-bit binary like PSL1GHT's, that `int` is the same
size as the pointer. It compiles, it links, it boots, and
`sctp_indata.c` does `memcpy(CMSG_DATA(cmh), ...)` on a truncated
address, on the console itself. No warning at all.

Fixed in three places:

- both probes now compile with `-Werror=implicit-function-declaration
  -Werror=implicit-int`, which turns that warning into the error it
  should always have been;
- `build-usrsctp.sh` compiles the library with the same flags, so no
  macro can ever hide behind an implicit call again;
- there's a **second probe that runs with the real flags** - with this
  tree ahead in the path, not with PSL1GHT's bare headers - and it says,
  for each item, whether the console provides it or we do. The first
  probe could never have answered for the second: they weren't the same
  question.

And a note about missing macros: **they don't always complain about
themselves**. `timercmp(&a, &b, >)` without the macro leaves a stray `>`
as a function argument, and the error you get is

```
error: expected expression before '>' token
```

which doesn't mention `timercmp` anywhere and looks like a syntax error
in usrsctp's own code.

## Verified

The 23 usrsctp files (plus `ps3_stubs.c`) compile cleanly for
**big-endian PowerPC64** with `powerpc64-linux-gnu-gcc`, with this tree
placed ahead in the search path so it wins over glibc's headers, and
**with `-Werror=implicit-function-declaration` turned on**. It isn't
`ppu-gcc` - different libc, different system - but the byte order and
architecture are the same.

The macros the wrappers add are tested **separately, and at runtime**,
because the PC's own glibc masks them: `wrtc/t_cmsg.c` is compiled
against a fake console that has `struct cmsghdr` and `struct timeval` but
has had the macros stripped out with `#undef` - which is the situation we
believe exists on the PS3. It runs under `qemu-ppc64`, big-endian, and
checks 26 things: the arithmetic of `CMSG_ALIGN`/`LEN`/`SPACE`/`DATA`,
that walking a two-message list with `CMSG_FIRSTHDR`/`NXTHDR` doesn't run
off the buffer or invent a third one, the microsecond carry in
`timeradd`/`timersub` - including the negative case that
`sctp_timer.c:534` depends on - and that `SOCK_SEQPACKET`,
`IPPORT_RESERVED` and `ERESTART` don't clash with anything.

What **cannot** be checked on the PC: the interaction with the headers
PSL1GHT **does** provide. There, the console has the final word.
