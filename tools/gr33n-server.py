#!/usr/bin/env python3
"""
GR33N - servidor de pruebas para el PC.

Responde al descubrimiento, hace eco de los pings, recibe el estado del
mando y devuelve un "frame" de video: una rejilla de 24x14 celdas RGB.
Es la otra mitad de source/link.c.

Uso:
    py -3 gr33n-server.py
    py -3 gr33n-server.py --name SALON --quiet

IMPORTANTE si pruebas con RPCS3: ejecuta esto en WINDOWS, no en WSL.
WSL2 vive detras de un NAT y el broadcast no cruza.

El servidor se anuncia por broadcast Y por 127.0.0.1, asi que funciona
tanto con RPCS3 en la misma maquina como con una PS3 real en la LAN.

MODELO DE FRAMES: se responde con un FRAME a cada INPUT, en vez de
empujar frames a 60 Hz por nuestra cuenta. Un servidor de streaming de
verdad empuja, pero aqui el temporizador de Python en Windows tiene una
granularidad malisima y meteria jitter que no es de la red. Asi la
cadencia la marca la consola, limpia.

Formato de cable (big-endian, igual que la PS3):

    u32 magic = 0x47523333 ("GR33")
    u32 type
    ...payload

    HELLO 1  cliente -> broadcast : u32 version
    HERE  2  servidor -> cliente  : u32 version, char name[32]
    PING  3  cliente -> servidor  : u32 seq, u64 t_cliente_us
    PONG  4  servidor -> cliente  : u32 seq, u64 t_cliente_us, u64 t_servidor_us
    LOG   5  cliente -> servidor  : texto
    INPUT 6  cliente -> servidor  : u32 seq, u64 t_cliente_us, u32 botones,
                                    s32 lx, s32 ly, s32 rx, s32 ry
    FRAME 7  servidor -> cliente  : u32 frame_seq, u32 input_seq,
                                    u64 t_input_cliente_us, u32 cols, u32 rows,
                                    u8 rgb[cols*rows*3]
"""

import argparse
import os
import socket
import struct
import sys
import time
from datetime import datetime

MAGIC = 0x47523333
VERSION = 1

MSG_HELLO = 1
MSG_HERE = 2
MSG_PING = 3
MSG_PONG = 4
MSG_LOG = 5
MSG_INPUT = 6
MSG_FRAME = 7

SERVER_PORT = 9330
CLIENT_PORT = 9331

NAME_LEN = 32
ANNOUNCE_PERIOD = 1.0
STATS_PERIOD = 2.0
PEER_TIMEOUT = 5.0

# Debe coincidir con LINK_FRAME_COLS / LINK_FRAME_ROWS en include/link.h
COLS = 24
ROWS = 14

# Espacio en el que la consola integra su mira local. Lo replicamos igual
# para que las dos posiciones sean directamente comparables en pantalla.
SURF_W = 1280
SURF_H = 720

BTN_CROSS = 1 << 14
BTN_CIRCLE = 1 << 13
BTN_TRIANGLE = 1 << 12
BTN_SQUARE = 1 << 15


# --- registro de sesion ---------------------------------------------------
# Cada arranque abre un fichero nuevo en logs/. Marcas de tiempo relativas
# al inicio: para leer una sesion importa cuando paso algo respecto a lo
# anterior, no la hora del reloj.

_logf = None
_t0 = 0.0


def log(msg: str = "") -> None:
    line = f"[{time.monotonic() - _t0:7.2f}] {msg}" if msg else ""
    print(line)
    if _logf:
        _logf.write(line + "\n")
        _logf.flush()   # flush siempre: si la consola se congela, el
                        # fichero tiene que conservar lo ultimo que dijo


def open_log(name: str, port: int) -> str:
    global _logf, _t0
    os.makedirs("logs", exist_ok=True)
    path = os.path.join("logs",
                        datetime.now().strftime("gr33n-%Y%m%d-%H%M%S.log"))
    _logf = open(path, "w", encoding="utf-8")
    _t0 = time.monotonic()
    _logf.write(f"=== GR33N sesion de depuracion ===\n")
    _logf.write(f"fecha    {datetime.now().isoformat(timespec='seconds')}\n")
    _logf.write(f"servidor {name}  puerto {port}\n")
    _logf.write(f"rejilla  {COLS}x{ROWS} ({COLS*ROWS*3 + 32} bytes por frame)\n")
    _logf.write("-" * 72 + "\n")
    _logf.flush()
    return path


def now_us() -> int:
    return time.monotonic_ns() // 1000


def pkt_here(name: str) -> bytes:
    raw = name.encode("ascii", "replace")[: NAME_LEN - 1]
    raw = raw + b"\x00" * (NAME_LEN - len(raw))
    return struct.pack(">III", MAGIC, MSG_HERE, VERSION) + raw


def pkt_pong(seq: int, t_client: int) -> bytes:
    return struct.pack(">IIIQQ", MAGIC, MSG_PONG, seq, t_client, now_us())


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


class Peer:
    def __init__(self, addr):
        self.addr = addr
        self.pings = 0
        self.inputs = 0
        self.frames = 0
        self.logs = 0
        self.gaps = 0
        self.last_seq = 0
        self.first_seen = time.monotonic()
        self.last_seen = self.first_seen

        # Estado del "juego"
        self.mx = SURF_W // 2
        self.my = SURF_H // 2
        self.frame_seq = 1
        self.phase = 0

    def note_ping(self, seq: int):
        self.pings += 1
        if self.last_seq and seq > self.last_seq + 1:
            self.gaps += seq - self.last_seq - 1
        self.last_seq = seq

    def apply_input(self, buttons: int, lx: int, ly: int):
        # Misma integracion y misma zona muerta que main.c, para que la
        # mira local de la consola y este cursor sean comparables.
        if lx > 24 or lx < -24:
            self.mx += lx // 10
        if ly > 24 or ly < -24:
            self.my += ly // 10
        self.mx = clamp(self.mx, 40, SURF_W - 40)
        self.my = clamp(self.my, 40, SURF_H - 40)
        self.inputs += 1

    def render(self, buttons: int) -> bytes:
        """Genera la rejilla RGB. Fea a proposito: lo que se prueba es el
        camino, no el contenido."""
        self.phase = (self.phase + 1) % (COLS * 4)
        buf = bytearray(COLS * ROWS * 3)

        cx = int(self.mx * COLS / SURF_W)
        cy = int(self.my * ROWS / SURF_H)

        if buttons & BTN_CROSS:
            cursor = (0, 255, 64)
        elif buttons & BTN_CIRCLE:
            cursor = (255, 40, 40)
        elif buttons & BTN_TRIANGLE:
            cursor = (0, 220, 255)
        elif buttons & BTN_SQUARE:
            cursor = (255, 0, 220)
        else:
            cursor = (255, 255, 255)

        band = self.phase // 4

        for y in range(ROWS):
            for x in range(COLS):
                if x == cx and y == cy:
                    r, g, b = cursor
                elif x == cx or y == cy:
                    # Cruz tenue: hace visible el movimiento aunque el
                    # cursor quede fuera de tu foco visual.
                    r, g, b = (60, 90, 70)
                elif x == band:
                    # Banda que barre: si el flujo de frames se corta, se
                    # queda quieta y se ve al instante.
                    r, g, b = (90, 90, 120)
                else:
                    shade = 24 if (x + y) % 2 == 0 else 16
                    r, g, b = (shade, shade + 8, shade)

                i = (y * COLS + x) * 3
                buf[i] = r
                buf[i + 1] = g
                buf[i + 2] = b

        return bytes(buf)

    def pkt_frame(self, input_seq: int, t_input: int, buttons: int) -> bytes:
        head = struct.pack(">IIIIQII", MAGIC, MSG_FRAME, self.frame_seq,
                           input_seq, t_input, COLS, ROWS)
        self.frame_seq += 1
        self.frames += 1
        return head + self.render(buttons)


def main() -> int:
    ap = argparse.ArgumentParser(description="Servidor de pruebas de GR33N")
    ap.add_argument("--name", default=socket.gethostname()[: NAME_LEN - 1],
                    help="nombre que se anuncia a la consola")
    ap.add_argument("--port", type=int, default=SERVER_PORT)
    ap.add_argument("--quiet", action="store_true",
                    help="no imprimir estadisticas periodicas")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", args.port))
    except OSError as e:
        print(f"no se pudo abrir el puerto {args.port}: {e}", file=sys.stderr)
        return 1
    sock.settimeout(0.05)

    log_path = open_log(args.name, args.port)

    here = pkt_here(args.name)
    peers: dict = {}
    last_announce = 0.0
    last_stats = time.monotonic()

    print(f"GR33N server  nombre={args.name}  puerto={args.port}")
    print(f"rejilla {COLS}x{ROWS} — {COLS*ROWS*3 + 32} bytes por frame")
    print(f"registrando en {log_path}")
    print("anunciandose por broadcast y por 127.0.0.1 — Ctrl+C para salir")
    print("-" * 72)

    while True:
        now = time.monotonic()

        if now - last_announce >= ANNOUNCE_PERIOD:
            last_announce = now
            for dst in ("255.255.255.255", "127.0.0.1"):
                try:
                    sock.sendto(here, (dst, CLIENT_PORT))
                except OSError:
                    pass

        try:
            data, addr = sock.recvfrom(4096)
        except (socket.timeout, OSError):
            data = None

        if data and len(data) >= 8:
            magic, mtype = struct.unpack(">II", data[:8])
            if magic == MAGIC:
                peer = peers.get(addr[0])
                if peer is None:
                    peer = Peer(addr[0])
                    peers[addr[0]] = peer
                    log(f"[+] consola en {addr[0]}")
                peer.last_seen = now

                if mtype == MSG_HELLO:
                    sock.sendto(here, (addr[0], CLIENT_PORT))

                elif mtype == MSG_PING and len(data) >= 20:
                    seq, t_client = struct.unpack(">IQ", data[8:20])
                    peer.note_ping(seq)
                    sock.sendto(pkt_pong(seq, t_client), (addr[0], CLIENT_PORT))

                elif mtype == MSG_INPUT and len(data) >= 40:
                    (iseq, t_client, buttons,
                     lx, ly, rx, ry) = struct.unpack(">IQIiiii", data[8:40])
                    peer.apply_input(buttons, lx, ly)
                    sock.sendto(peer.pkt_frame(iseq, t_client, buttons),
                                (addr[0], CLIENT_PORT))

                elif mtype == MSG_LOG:
                    text = data[8:].decode("utf-8", "replace").rstrip("\x00")
                    peer.logs += 1
                    # El HUD llega entero en un solo datagrama con saltos
                    # de linea dentro: se desglosa aqui.
                    for ln in text.split("\n"):
                        log(f"[{addr[0]}] {ln}")

        for ip in [ip for ip, p in peers.items() if now - p.last_seen > PEER_TIMEOUT]:
            log(f"[-] {ip} se ha ido")
            del peers[ip]

        if not args.quiet and now - last_stats >= STATS_PERIOD:
            for ip, p in peers.items():
                el = max(1e-9, now - p.first_seen)
                log(f"    {ip}  ping={p.pings} ({p.pings/el:.0f}/s)  "
                    f"in={p.inputs} out={p.frames} ({p.frames/el:.0f}/s)  "
                    f"huecos={p.gaps}  logs={p.logs}")
            last_stats = now


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log("[fin] Ctrl+C")
        if _logf:
            path = _logf.name
            _logf.close()
            print(f"\nsesion guardada en {path}")
        sys.exit(0)
