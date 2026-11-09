#!/usr/bin/env python3
"""
mp02.py - lector/extractor independiente de contenedores "MP02" (DJMAX Portable 1/2/Fever/Hot Tunes).

Formato reconstruido a partir de la implementacion de referencia
ADHSoft/Xip-Pak-Extractor, portable12Extractor.py (commit f536949), y verificado
contra los packs de DJMAX Fever (ULUS10403).

Cabecera (16 bytes):
    0x00  "MP0" + version ('2')
    0x04  flag comprimido (0x00 = cuerpo plano, 0x01 = bloques zlib)
    0x05  3 bytes de relleno (0xCC)
    0x08  u32 LE  numero de ficheros
    0x0C  u32 LE  tamano descomprimido total (informativo)

Cuerpo:
    plano:        empieza en 0x10; los offsets de la TOC son relativos a 0x10.
    comprimido:   secuencia de [u32 LE n][n bytes zlib] hasta un u32 0.
                  Los offsets de la TOC son relativos al buffer descomprimido.

TOC: por cada fichero, 0x8C bytes, ofuscados con dos pasadas de XOR:
    h = unxor(raw, i, 0x8C)                      # keystream desplazado por indice
    h = h[:4] + unxor(h[4:], 0, 0x80, alt=True) + h[-8:]
    crc    = u32 LE h[0:4]
    nombre = h[4:0x84] hasta NUL, EUC-KR
    offset = u32 LE h[0x84:0x88]
    size   = u32 LE h[0x88:0x8C]
"""
import argparse
import os
import struct
import sys
import zlib

JAP_TEXT = """……耕一さん……あなたを殺します
私はあなたを、愛してはいませんから…
生きて…ラカン…
百年…貴方を待っていたの…千年…貴方に恋していたわ
私…世界より貴方がほしい……
夜空に星が輝くように溶けた心は離れない
たとえこの手が離れてもふたりがそれを忘れぬ限り"""

HEADER_SIZE = 0x10
ENTRY_SIZE = 0x8C
NAME_LEN = 0x80


def _key(alt: bool) -> bytes:
    start = 0 if alt else 1
    blob = JAP_TEXT.replace("\n", "\r\n").encode("shift-jis")
    return blob[start:start + 256]


def _unxor(data: bytes, key_off: int, size: int, alt: bool = False) -> bytes:
    key = _key(alt)
    key_off &= 255
    out = bytearray(size)
    for i in range(size):
        out[i] = key[(key_off + i) & 255] ^ data[i]
    return bytes(out)


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


class Mp02:
    def __init__(self, path=None, data=None):
        self.path = path
        if data is None:
            with open(path, "rb") as f:
                self.raw = f.read()
        else:
            self.raw = data
        if self.raw[:3] != b"MP0":
            raise ValueError("no es un contenedor MP0x")
        self.version = self.raw[3:4]
        self.compressed = self.raw[4] == 1
        if self.raw[4] not in (0, 1):
            raise ValueError("flag de compresion desconocido: 0x%02X" % self.raw[4])
        self.count = _u32(self.raw, 8)
        self.uncompressed_size = _u32(self.raw, 0x0C)
        self._load_body()
        self._load_toc()

    def _load_body(self) -> None:
        if not self.compressed:
            self.body = self.raw[HEADER_SIZE:]
            return
        pos = HEADER_SIZE
        chunks = []
        # el ultimo bloque puede terminar en EOF sin u32 centinela (p.ej. all6/all7)
        while pos + 4 <= len(self.raw):
            n = _u32(self.raw, pos)
            pos += 4
            if n == 0:
                break
            chunks.append(zlib.decompress(self.raw[pos:pos + n]))
            pos += n
        self.body = b"".join(chunks)

    def _load_toc(self) -> None:
        self.entries = []
        pos = 0
        for i in range(self.count):
            raw = self.body[pos:pos + ENTRY_SIZE]
            pos += ENTRY_SIZE
            if len(raw) != ENTRY_SIZE:
                raise ValueError("TOC truncada en la entrada %d" % i)
            h = _unxor(raw, i, ENTRY_SIZE)
            h = h[:4] + _unxor(h[4:], 0, NAME_LEN, True) + h[-8:]
            name = h[4:4 + NAME_LEN].split(b"\x00")[0]
            self.entries.append({
                "index": i,
                "crc": _u32(h, 0),
                "name": name.decode("euc-kr", errors="replace"),
                "raw_name": name,
                "offset": _u32(h, 4 + NAME_LEN),
                "size": _u32(h, 4 + NAME_LEN + 4),
            })
        self.data_start = pos

    def read(self, entry: dict) -> bytes:
        # los offsets de la TOC son relativos al inicio del cuerpo (offset 0x10 del
        # fichero, o inicio del buffer descomprimido), NO al final de la TOC.
        off = entry["offset"]
        return self.body[off:off + entry["size"]]


def cmd_list(args) -> int:
    mp = Mp02(args.path)
    print("pack        : %s" % os.path.basename(args.path))
    print("version     : MP0%s" % mp.version.decode("latin-1"))
    print("comprimido  : %s" % mp.compressed)
    print("ficheros    : %d" % mp.count)
    print("cuerpo      : %d B (declarado %d)" % (len(mp.body), mp.uncompressed_size))
    print("TOC         : %d B, datos desde 0x%X" % (mp.data_start, mp.data_start))
    print("-" * 90)
    shown = 0
    for e in mp.entries:
        if args.filter and args.filter.lower() not in e["name"].lower():
            continue
        print("%5d  %10d  %08X  %s" % (e["index"], e["size"], e["crc"], e["name"]))
        shown += 1
        if args.limit and shown >= args.limit:
            break
    if args.filter:
        print("-" * 90)
        print("coincidencias: %d" % sum(1 for e in mp.entries if args.filter.lower() in e["name"].lower()))
    return 0


def cmd_extract(args) -> int:
    mp = Mp02(args.path)
    out = args.out or os.path.join("out", os.path.splitext(os.path.basename(args.path))[0])
    n = 0
    total = 0
    for e in mp.entries:
        if args.max_size and e["size"] > args.max_size:
            continue
        if args.filter and args.filter.lower() not in e["name"].lower():
            continue
        rel = e["name"].replace("\\", "/").lstrip("/")
        if ".." in rel.split("/"):
            print("omitido (traversal): %s" % e["name"], file=sys.stderr)
            continue
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
        data = mp.read(e)
        with open(dst, "wb") as f:
            f.write(data)
        n += 1
        total += len(data)
    print("extraidos %d ficheros, %d B -> %s" % (n, total, out))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Lector de contenedores MP02 (DJMAX Portable)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    lp = sub.add_parser("list", help="lista la tabla de contenidos")
    lp.add_argument("path")
    lp.add_argument("--filter", default="")
    lp.add_argument("--limit", type=int, default=0)
    lp.set_defaults(func=cmd_list)

    ep = sub.add_parser("extract", help="extrae ficheros")
    ep.add_argument("path")
    ep.add_argument("--out", default="")
    ep.add_argument("--filter", default="")
    ep.add_argument("--max-size", type=int, default=0)
    ep.set_defaults(func=cmd_extract)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
