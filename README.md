
<h1 align="center">GR33N</h1>


<p align="center">
  <img alt="C" src="https://img.shields.io/badge/c-%2300599C.svg?style=for-the-badge&logo=c&logoColor=white">
  <img alt="PS3" src="https://img.shields.io/badge/playstation3-%23003791.svg?style=for-the-badge&logo=playstation3&logoColor=white">
  <img alt="Xbox Cloud Gaming" src="https://img.shields.io/badge/Xbox-Cloud%20Gaming-107c10?style=for-the-badge&logo=xbox&logoColor=white">
</p>

A standalone, open-source Xbox Cloud Gaming (xCloud) client for the PlayStation 3 (homebrew). 

## Install

You need a homebrew-enabled PlayStation 3 and an Xbox account with a subscription that allows you to access Xbox Cloud Gaming.

1. Download `gr33n.pkg` from the
   [latest release](https://github.com/actuallyniaxx/GR33N/releases/latest).
2. Transfer the PKG to the PlayStation 3 System with your preffered method or put it on an FAT32 formatted flash drive.
3. Install it from the XMB.
4. Launch GR33N and follow the device-code sign-in procedure that will appear on the screen.

> [!IMPORTANT]
> This procedure will only appear automatically if you don't have a session token on your system.
> If you already had a token and it expired or was manually revoked you can re-login again from the settings tab.

> [!NOTE]
> GR33N shows up in the Network column of the XMB, not among the games — it's a network application


## Building from source

1. Environment

You need [PSL1GHT](https://github.com/ps3dev/psl1ght). On Windows it works fine under WSL2.

`sh
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=$PS3DEV
export PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin`

2. Dependencies

Five native libraries, none of them packaged for the PS3. The scripts fetch, patch and build them into ~/.gr33n-deps/.

Order matters — each one needs the previous one's headers:

`sh
sh deps/build-mbedtls.sh     # TLS and DTLS
sh deps/build-libsrtp.sh     # SRTP
sh deps/build-usrsctp.sh     # SCTP, for the data channels
sh deps/build-libpeer.sh     # WebRTC (applies deps/libpeer-ps3.patch)
sh deps/build-opus.sh        # Opus, fixed-point`

Each script checks what it built and fails loudly if something doesn't add up, rather than leaving you a half-finished .a that explodes at link time.

3. Build
   
`sh
make          # gr33n.self + gr33n.fake.self
make pkg      # installable gr33n.pkg
make run      # ps3load over the network`

4. Tests

`sh
sh t/correr.sh          # all 22
sh t/correr.sh audio    # just one`

They run on a PC, with an ordinary gcc, under ASan and UBSan. They test the arithmetic: RTP reordering, the PCM ring, pad mapping, NAL slicing, message parsing. None of that needs a PS3.

Passing does not mean GR33N works — the decoder, the RSX and lv2's network stack are only testable with the console switched on. It means that if something breaks, it isn't this.

They exist for a specific reason. The PCM ring in aud.c was sized at 8192 pairs, which with an Opus frame of up to 5760 left about 50 ms usable for a 160 ms cushion. The audio would never have started, and nothing was broken: every piece did its job and the result was silence. That doesn't show up on a read. This test caught it on the first run, before a single byte was compiled for the console.

Repository layout
`source/      the program
include/     its headers
shaders/     the two RSX shaders
data/        clip.h264, the test video that gets built into the binary
pkgfiles/    ICON0.PNG and friends, the XMB entry
t/           the PC tests
deps/        what you need to compile: the build-*.sh scripts, the libpeer
             patch, cJSON, the POSIX shim for PSL1GHT`

The source comments are in Spanish. They're also, in places, the only documentation of why the PS3 behaves the way it does — the endianness traps in the H.264 depacketiser, usrsctp's length-prefixed sockaddr_conn, and why there is no select() anywhere in the tree. Worth a translation pass if you're porting this elsewhere.

## Credits

GR33N wouldn't exist without two projects that came first:

[green-nx](https://github.com/rmrf404/green-nx), by rmrf404 — the xCloud client for the Nintendo Switch. Its libpeer patch is what encodes how you actually talk to xCloud, and adapting it was far more honest than rewriting it. It's GPL-3, which is why GR33N is too.
[green-vita](https://github.com/Day-OS/green-vita), by Day-OS — the PS Vita one, which is where the idea that this would fit on an old Sony console came from.

Full third-party licences ([libpeer](https://github.com/sepfy/libpeer), [usrsctp](https://github.com/sctplab/usrsctp), [libSRTP](https://github.com/cisco/libsrtp), [mbedTLS](https://github.com/Mbed-TLS/mbedtls), [Opus](https://github.com/xiph/opus), [cJSON](https://github.com/Davegamble/cjson), [PSL1GHT](https://github.com/ps3dev/psl1ght)) are in [CREDITOS](CREDITOS).

GR33N is an independent homebrew project and is not affiliated with or endorsed by Microsoft, Xbox, Sony, or PlayStation.

GR33N is an independent homebrew project and is not affiliated with or
endorsed by Microsoft, Xbox, Sony, or PlayStation.

## License

GR33N is licensed under the [GNU General Public License version 3](LICENSE).
