#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gim2png.py - Decoder de texturas GIM (PSP, "MIG.00.1PSP") -> PNG.

Formato reconstruido y verificado contra las 761 texturas de DJMAX Fever
(ULUS10403). Ver D:\\DJMX\\analysis\\05-gim-format.md para la especificacion.

Uso:
    python gim2png.py <in.gim> <out.png> [--swizzle auto|on|off] [--clut auto|on|off]

Sin dependencias externas: PNG escrito a mano con zlib + struct.
"""
import argparse
import os
import struct
import sys
import zlib

MAGICS = (b"MIG.00.1PSP", b"MIG.00.1", b".GIM")

# ---------------------------------------------------------------- utilidades


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _clamp8(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def write_png(path, w, h, rgba, compress=9):
    """PNG RGBA8 sin dependencias."""
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filtro None
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), compress)) +
           chunk(b"IEND", b""))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(png)
    os.replace(tmp, path)


def write_ppm(path, w, h, rgb):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(rgb))


# ------------------------------------------------------------ bloques GIM

# id de bloque -> nombre (segun especificacion Xentax / psdevwiki)
BLOCK_NAMES = {
    0x01: "block", 0x02: "root", 0x03: "picture", 0x04: "image",
    0x05: "palette", 0x06: "sequence", 0xFF: "fileinfo",
}

FMT_NAMES = {
    0x00: "RGBA5650", 0x01: "RGBA5551", 0x02: "RGBA4444", 0x03: "RGBA8888",
    0x04: "INDEX4", 0x05: "INDEX8", 0x06: "INDEX16", 0x07: "INDEX32",
    0x08: "DXT1", 0x09: "DXT3", 0x0A: "DXT5",
    0x108: "DXT1EXT", 0x109: "DXT3EXT", 0x10A: "DXT5EXT",
}

INDEXED = {0x04: (4, 16), 0x05: (8, 256), 0x06: (8, 256), 0x07: (8, 256)}


class GimBlock(object):
    def __init__(self, ident, size, next_off, data_off, start):
        self.id = ident
        self.size = size
        self.next = next_off
        self.data_off = data_off
        self.start = start
        self.data_start = start + data_off

    def __repr__(self):
        return "<block 0x%02X %s size=0x%X data=0x%X>" % (
            self.id, BLOCK_NAMES.get(self.id, "?"), self.size, self.data_start)


class GimImage(object):
    """Estructura de datos de imagen (o paleta) dentro de un bloque."""

    def __init__(self, base, buf):
        self.base = base                     # offset del bloque de datos
        self.header_size = u16(buf, base + 0x00)
        self.reference = u16(buf, base + 0x02)
        self.format = u16(buf, base + 0x04)
        self.pixel_order = u16(buf, base + 0x06)
        self.width = u16(buf, base + 0x08)
        self.height = u16(buf, base + 0x0A)
        self.bpp = u16(buf, base + 0x0C)
        self.pitch_align = u16(buf, base + 0x0E)
        self.height_align = u16(buf, base + 0x10)
        self.dim_count = u16(buf, base + 0x12)
        self.index_offset = u32(buf, base + 0x18)
        self.first_plane = u32(buf, base + 0x1C)
        self.last_plane = u32(buf, base + 0x20)
        self.plane_mask = u32(buf, base + 0x24)
        self.level_type = u16(buf, base + 0x28)
        self.level_count = u16(buf, base + 0x2A)
        self.frame_type = u16(buf, base + 0x2C)
        self.frame_count = u16(buf, base + 0x2E)
        self.frame_offset = u32(buf, base + 0x30)

    @property
    def fmt_name(self):
        return FMT_NAMES.get(self.format, "??0x%X" % self.format)

    @property
    def swizzled(self):
        # pixel_order: 0 = NORMAL (lineal), 1 = PSPIMAGE (swizzle 16B x 8 lineas)
        return self.pixel_order == 1

    def data_offset(self):
        # offset de los pixeles, relativo al inicio del bloque de datos
        if 0x10 <= self.frame_offset < 0x10000:
            return self.frame_offset
        return self.first_plane

    def data_size(self):
        n = self.last_plane - self.first_plane
        if n <= 0:
            n = self.last_plane - self.data_offset()
        return n

    def __repr__(self):
        return ("<%s fmt=0x%02X(%s) %dx%d bpp=%d order=%d hdr=0x%X "
                "plane=0x%X..0x%X levels=%d frames=%d>" % (
                    "image", self.format, self.fmt_name, self.width,
                    self.height, self.bpp, self.pixel_order, self.header_size,
                    self.first_plane, self.last_plane, self.level_count,
                    self.frame_count))


class GimFile(object):
    def __init__(self, data, path="<mem>"):
        self.path = path
        self.raw = data
        if len(data) < 0x20:
            raise ValueError("fichero demasiado corto")
        if not any(data.startswith(m) for m in MAGICS):
            raise ValueError("magic GIM no reconocido: %r" % data[:16])
        self.magic = data[:16]
        self.blocks = []
        self.image = None
        self.palette = None
        self._walk()

    def _walk(self):
        pos = 0x10
        seen = set()
        images = []
        while 0x10 <= pos + 0x10 <= len(self.raw):
            if pos in seen:
                break
            seen.add(pos)
            ident = u16(self.raw, pos)
            size = u32(self.raw, pos + 4)
            nxt = u32(self.raw, pos + 8)
            doff = u32(self.raw, pos + 12)
            if size == 0 and nxt == 0:
                break
            blk = GimBlock(ident, size, nxt, doff, pos)
            self.blocks.append(blk)
            # intentar interpretar el contenido como estructura de imagen/paleta
            ds = blk.data_start
            if 0 <= ds and ds + 0x34 <= len(self.raw):
                hs = u16(self.raw, ds)
                fmt = u16(self.raw, ds + 4)
                w = u16(self.raw, ds + 8)
                h = u16(self.raw, ds + 10)
                if hs in (0x30, 0x34, 0x38, 0x40) and fmt in FMT_NAMES \
                        and 0 < w <= 4096 and 0 < h <= 4096:
                    images.append((blk, GimImage(ds, self.raw)))
            if nxt == 0:
                break
            npos = pos + nxt
            if npos <= pos or npos > len(self.raw):
                break
            pos = npos
        self.images = [im for _b, im in images]
        if not images:
            raise ValueError("no se encontro estructura de imagen")
        # La imagen principal es la mayor (o la unica indexada).  Una imagen
        # pequena (tipicamente 256x1 RGBA8888) es la paleta CLUT.
        main = None
        pal = None
        indexed = [im for _b, im in images if im.format in INDEXED]
        others = [im for _b, im in images if im.format not in INDEXED]
        if indexed:
            main = indexed[0]
            for im in others:
                if im.width <= 1024 and im.height <= 4:
                    pal = im
                    break
            if pal is None and others:
                pal = others[0]
        else:
            images.sort(key=lambda t: t[1].width * t[1].height, reverse=True)
            main = images[0][1]
        self.image = main
        self.palette = pal

    # ---------------------------------------------------------------- datos

    def pixel_bytes(self):
        img = self.image
        off = img.base + img.data_offset()
        n = img.data_size()
        if n <= 0 or off + n > len(self.raw):
            # fallback: hasta el siguiente bloque / EOF
            n = max(0, len(self.raw) - off)
        return self.raw[off:off + n]

    def palette_bytes(self):
        if self.palette is None:
            return b""
        p = self.palette
        off = p.base + p.data_offset()
        n = p.data_size()
        if n <= 0 or off + n > len(self.raw):
            n = max(0, min(len(self.raw) - off, 256 * 4))
        return self.raw[off:off + n]


# ---------------------------------------------------------------- swizzle

def unswizzle(data, width, height, bpp):
    """Deshace el swizzle PSP (PSPIMAGE): bloques de 16 B en columnas de 8 lineas.

    En el layout swizzled, dentro de cada grupo de 8 filas los bloques de 16
    bytes se guardan columna a columna (8 bloques consecutivos = una columna
    vertical).  Es el mismo patron que usa la GE de PSP.
    """
    if bpp not in (4, 8, 16, 32):
        return data
    row_bytes = (width * bpp + 7) // 8
    blocks_per_row = (row_bytes + 15) // 16
    if blocks_per_row == 0:
        return data
    row_bytes_al = blocks_per_row * 16
    out = bytearray(row_bytes_al * height)
    src = 0
    for y0 in range(0, height, 8):
        rows = min(8, height - y0)
        for bx in range(blocks_per_row):
            for r in range(rows):
                y = y0 + r
                dst = y * row_bytes_al + bx * 16
                out[dst:dst + 16] = data[src:src + 16]
                src += 16
                if src >= len(data):
                    return bytes(out)
    return bytes(out)


def swizzle(data, width, height, bpp):
    """Inversa de unswizzle (para tests)."""
    row_bytes = (width * bpp + 7) // 8
    blocks_per_row = (row_bytes + 15) // 16
    row_bytes_al = blocks_per_row * 16
    out = bytearray(len(data))
    src = 0
    for y0 in range(0, height, 8):
        rows = min(8, height - y0)
        for bx in range(blocks_per_row):
            for r in range(rows):
                y = y0 + r
                off = y * row_bytes_al + bx * 16
                out[src:src + 16] = data[off:off + 16]
                src += 16
    return bytes(out)


# ---------------------------------------------------------------- decoders

def _expand5650(v):
    r = (v & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = ((v >> 11) & 0x1F) * 255 // 31
    return r, g, b, 255


def _expand5551(v):
    r = (v & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x1F) * 255 // 31
    b = ((v >> 10) & 0x1F) * 255 // 31
    a = 255 if (v >> 15) & 1 else 0
    return r, g, b, a


def _expand4444(v):
    r = (v & 0xF) * 17
    g = ((v >> 4) & 0xF) * 17
    b = ((v >> 8) & 0xF) * 17
    a = ((v >> 12) & 0xF) * 17
    return r, g, b, a


def decode_rgba(data, w, h, fmt):
    """Formatos directos 0..3 -> RGBA8."""
    out = bytearray(w * h * 4)
    if fmt == 3:
        n = min(w * h, len(data) // 4)
        out[:n * 4] = data[:n * 4]
        return out
    n = min(w * h, len(data) // 2)
    for i in range(n):
        v = data[i * 2] | (data[i * 2 + 1] << 8)
        if fmt == 0:
            r, g, b, a = _expand5650(v)
        elif fmt == 1:
            r, g, b, a = _expand5551(v)
        else:
            r, g, b, a = _expand4444(v)
        o = i * 4
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b
        out[o + 3] = a
    return out


def decode_clut(data, w, h, fmt, pal, pal_fmt):
    """Formatos indexados 4..7 con paleta."""
    bpp, _n = INDEXED[fmt]
    # paleta -> lista RGBA
    colors = []
    if pal_fmt == 3:
        for i in range(0, len(pal) - 3, 4):
            colors.append(tuple(pal[i:i + 4]))
    else:
        for i in range(0, len(pal) - 1, 2):
            v = pal[i] | (pal[i + 1] << 8)
            if pal_fmt == 0:
                colors.append(_expand5650(v))
            elif pal_fmt == 1:
                colors.append(_expand5551(v))
            else:
                colors.append(_expand4444(v))
    if not colors:
        colors = [(0, 0, 0, 0)]
    out = bytearray(w * h * 4)
    for i in range(w * h):
        if bpp == 4:
            byte = data[i >> 1] if (i >> 1) < len(data) else 0
            idx = (byte >> 4) if (i & 1) == 0 else (byte & 0xF)
        elif bpp == 8:
            idx = data[i] if i < len(data) else 0
        else:
            idx = data[i] if i < len(data) else 0
        c = colors[idx % len(colors)]
        out[i * 4:i * 4 + 4] = bytes(c)
    return out


def _dxt_colors(c0, c1, four):
    def rgb(v):
        return (((v >> 11) & 0x1F) * 255 // 31,
                ((v >> 5) & 0x3F) * 255 // 63,
                (v & 0x1F) * 255 // 31)
    a = rgb(c0)
    b = rgb(c1)
    if four:
        c2 = tuple((2 * a[i] + b[i] + 1) // 3 for i in range(3))
        c3 = tuple((a[i] + 2 * b[i] + 1) // 3 for i in range(3))
        return [a + (255,), b + (255,), c2 + (255,), c3 + (255,)]
    c2 = tuple((a[i] + b[i]) // 2 for i in range(3))
    return [a + (255,), b + (255,), c2 + (255,), (0, 0, 0, 0)]


def _dxt5_alpha(blk, off=8):
    """Bloque alpha DXT5 tal como lo escribe GimConv en este juego.

    Layout observado (rotado respecto al DXT5 estandar):
        +0..5  6 bytes de indices 3 bits (16 texeles)
        +6     alpha0
        +7     alpha1
    """
    aidx = int.from_bytes(blk[off:off + 6], "little")
    a0 = blk[off + 6]
    a1 = blk[off + 7]
    if a0 > a1:
        at = [a0, a1] + [((8 - i) * a0 + (i - 1) * a1) // 7 for i in range(2, 8)]
    else:
        at = [a0, a1] + [((6 - i) * a0 + (i - 1) * a1) // 5 for i in range(2, 6)]
        at += [0, 255]
    return [(at[(aidx >> (3 * i)) & 7]) for i in range(16)]


def _dxt_color(blk):
    """Bloque de color tal como lo escribe GimConv en este juego.

    Layout observado (rotado respecto al DXT5/DXT1 estandar):
        +0..3  16 indices de 2 bits (u32 LE)
        +4..5  color0 RGB565 LE
        +6..7  color1 RGB565 LE
    """
    bits = blk[0] | (blk[1] << 8) | (blk[2] << 16) | (blk[3] << 24)
    c0 = blk[4] | (blk[5] << 8)
    c1 = blk[6] | (blk[7] << 8)
    cols = _dxt_colors(c0, c1, c0 > c1)
    return [cols[(bits >> (2 * i)) & 3] for i in range(16)]


def decode_dxt(data, w, h, fmt):
    """DXT1 (8), DXT3 (9), DXT5 (10) -> RGBA8.

    El layout de bloques de este juego (GimConv) NO es el DXT estandar: los
    campos van rotados (indices primero, luego los dos extremos).  Ver
    _dxt_color / _dxt5_alpha.
    """
    out = bytearray(w * h * 4)
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    if fmt in (9, 0x0A):
        bsize = 16
    else:
        bsize = 8
    for by in range(bh):
        for bx in range(bw):
            off = (by * bw + bx) * bsize
            blk = data[off:off + bsize]
            if len(blk) < bsize:
                blk = blk + bytes(bsize - len(blk))
            cols = _dxt_color(blk)
            if fmt == 0x0A:
                alphas = _dxt5_alpha(blk, 8)
                pixels = [(cols[i][0], cols[i][1], cols[i][2], alphas[i])
                          for i in range(16)]
            elif fmt == 9:
                abits = int.from_bytes(blk[8:16], "little")
                alphas = [((abits >> (4 * i)) & 0xF) * 17 for i in range(16)]
                pixels = [(cols[i][0], cols[i][1], cols[i][2], alphas[i])
                          for i in range(16)]
            else:
                pixels = cols
            for py in range(4):
                y = by * 4 + py
                if y >= h:
                    break
                for px in range(4):
                    x = bx * 4 + px
                    if x >= w:
                        break
                    o = (y * w + x) * 4
                    out[o:o + 4] = bytes(pixels[py * 4 + px])
    return out


# ---------------------------------------------------------------- API

def decode_gim(data, path="<mem>", swizzle_mode="auto", clut_mode="auto"):
    """Devuelve (w, h, rgba_bytes, info_dict)."""
    g = GimFile(data, path)
    img = g.image
    w, h = img.width, img.height
    fmt = img.format
    pix = g.pixel_bytes()

    do_swz = img.swizzled
    if swizzle_mode == "on":
        do_swz = True
    elif swizzle_mode == "off":
        do_swz = False

    if do_swz:
        pix = unswizzle(pix, w, h, img.bpp)

    if fmt in (0, 1, 2, 3):
        rgba = decode_rgba(pix, w, h, fmt)
    elif fmt in INDEXED:
        pal = g.palette_bytes() if clut_mode != "off" else b""
        pal_fmt = g.palette.format if g.palette is not None else 1
        rgba = decode_clut(pix, w, h, fmt, pal, pal_fmt)
    elif fmt in (8, 9, 0x0A):
        rgba = decode_dxt(pix, w, h, fmt)
    else:
        raise ValueError("formato de pixel no soportado: 0x%X" % fmt)

    info = {
        "format": fmt,
        "format_name": img.fmt_name,
        "width": w,
        "height": h,
        "bpp": img.bpp,
        "pixel_order": img.pixel_order,
        "swizzled": do_swz,
        "has_palette": g.palette is not None,
        "palette_format": (g.palette.format if g.palette is not None else None),
        "data_offset": img.base + img.data_offset(),
        "data_size": len(pix),
        "blocks": [(b.id, b.size, b.data_start) for b in g.blocks],
    }
    return w, h, rgba, info


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Convierte texturas GIM (PSP) a PNG")
    ap.add_argument("src", help="fichero .gim de entrada")
    ap.add_argument("dst", help="fichero .png de salida")
    ap.add_argument("--swizzle", default="auto",
                    choices=("auto", "on", "off"),
                    help="forzar/auto deshacer swizzle PSPIMAGE")
    ap.add_argument("--clut", default="auto", choices=("auto", "on", "off"),
                    help="usar/no usar la paleta CLUT")
    ap.add_argument("--info", action="store_true", help="solo mostrar info")
    args = ap.parse_args(argv)

    with open(args.src, "rb") as f:
        data = f.read()
    try:
        w, h, rgba, info = decode_gim(data, args.src, args.swizzle, args.clut)
    except Exception as e:
        print("ERROR: %s: %s" % (args.src, e), file=sys.stderr)
        return 1

    if args.info:
        for k in ("format_name", "width", "height", "bpp", "pixel_order",
                  "swizzled", "has_palette", "data_offset", "data_size"):
            print("%-14s %s" % (k, info[k]))
        return 0

    d = os.path.dirname(os.path.abspath(args.dst))
    if d:
        os.makedirs(d, exist_ok=True)
    write_png(args.dst, w, h, rgba)
    print("%s -> %s  (%dx%d %s%s)" % (
        os.path.basename(args.src), args.dst, w, h, info["format_name"],
        ", swizzle" if info["swizzled"] else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
