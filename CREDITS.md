# Credits and licences

GR33N is distributed under the **GPL-3.0**. The full text is in `LICENSE`.

The reason it's GPL-3 and not something more permissive is specific, and
it's the first entry below: **green-nx** is what makes the WebRTC side
viable at all, it's GPL-3, and it was used as a base rather than rewritten.
That's a decision, not an oversight.

---

## green-nx — GPL-3.0

<https://github.com/rmrf404/green-nx>, by rmrf404 (xCloud client for the
Nintendo Switch)

Adapted from it, to PowerPC big-endian and PSL1GHT:

- **`deps/libpeer-ps3.patch`**, which starts from their
  `libpeer-switch.patch`. It's 41 hunks, and almost none of them are
  Switch-specific: they fix libpeer bugs that bite on any platform and,
  above all, they **encode how you have to talk to xCloud**.
- The diagnosis of usrsctp's **length-prefixed `sockaddr_conn`**, which
  they hit on Switch with the same newlib family. Without that note it
  would have been several evenings of memory corruption on a PS3 with no
  debugger.
- The observation that libpeer's H.264 depacketiser **corrupts frames on
  any reordering or loss**, and that you have to bypass it and do the
  reassembly yourself.

What did **not** come from them, because they didn't need it: the whole
big-endian layer. The Switch is little-endian and the PS3 isn't, so the
depacketiser's two byte-order traps — the Annex-B start code backwards, and
the inverted bitfields — were found here, compiling their code for
PowerPC64 and comparing output byte by byte.

## libpeer — MIT

<https://github.com/sepfy/libpeer>, by sepfy. Copyright (c) 2021.

Pinned to commit `9319aa434cb9e893faed0293ba9d2a21eca59c8b`. Not a whim:
master broke all 41 hunks of the patch in August 2026.

## usrsctp — BSD-3-Clause

<https://github.com/sctplab/usrsctp>. Copyright (c) the FreeBSD
contributors.

`deps/ps3-shim/` also includes four headers **copied verbatim from
FreeBSD**, with their BSD-3-Clause licence intact: `sys/queue.h`,
`netinet/ip.h`, `netinet/udp.h` and `netinet/in_systm.h`. They're copied
rather than hand-written on purpose — they're wire structures with
bitfields whose order changes with endianness, and on the platform where
that has already drawn blood twice, writing them from memory would be
pushing your luck.

## libSRTP — BSD-3-Clause

<https://github.com/cisco/libsrtp>. Copyright (c) Cisco Systems.

## mbedTLS — Apache-2.0

<https://github.com/Mbed-TLS/mbedtls>. Version 3.6.4.

**No patches are applied to it.** The DTLS timer is hooked through
`MBEDTLS_TIMING_ALT`, which is the mechanism the library itself offers for
exactly this. `deps/mbedtls-ps3/timing_alt.h` and `source/dtls_timer.c` are
ours.

## Opus — BSD-3-Clause

<https://opus-codec.org/>, by the Xiph.Org Foundation. Version 1.4.

Built in **fixed point** (`FIXED_POINT`). The PS3 has FPU to spare, but the
source list is pulled from Opus's own `celt_sources.mk` / `silk_sources.mk`
by calling `make`, rather than maintaining a parallel list by hand that
would fall behind on the first new version.

## cJSON — MIT

<https://github.com/DaveGamble/cJSON>. In `deps/cjson/`.

## green-vita — where the idea came from

<https://github.com/Day-OS/green-vita>, by Day-OS. The xCloud client for
the PS Vita. None of its code is used, but it's what made it clear this
would fit on an old Sony console.

## PS3-Moonlight — technical reference

<https://github.com/Cruslan/PS3-Moonlight>, by Cruslan.

Used as a technical reference during the development of GR33N for
PS3-specific implementation details and techniques. In particular, it was
useful for understanding existing approaches to PS3 hardware video
decoding, RSX rendering, audio handling, PS3 input/UI and related
console-specific implementation details.

This is a reference/learning resource; GR33N is a separate project.

## Cell Stream — technical reference

<https://github.com/mohasi/ps3-dev/releases>, by mohasi.

The **Cell Stream** project was another useful technical reference during
development, particularly for PS3 streaming, client/server communication,
input handling and other console-specific implementation details.

This is a reference/learning resource; GR33N is a separate project.

## PSL1GHT

<https://github.com/ps3dev/PSL1GHT>. The open PS3 SDK all of this is built
on.

## AI-assisted development

**Claude (Anthropic)** was used throughout the development of
GR33N as a coding and research assistant.

Its use included code review, log analysis, debugging, code cleanup and
refactoring, technical research, brainstorming and design suggestions, and
writing or modifying portions of the code.

GR33N is therefore **not presented as a project written entirely manually
by its author**. The project was developed iteratively with AI assistance,
with the author responsible for integrating, testing and verifying the
resulting implementation and making the final technical decisions.

---

## What GPL-3 means here, briefly

If you distribute GR33N — the `.pkg`, the `.self`, or a modified binary —
you have to offer the corresponding source under the same licence. Linking
MIT, BSD and Apache-2.0 libraries from a GPL-3 program is compatible; the
other way round would not be, and that's why the licence of the whole is
the most restrictive of the ones that go into it.
