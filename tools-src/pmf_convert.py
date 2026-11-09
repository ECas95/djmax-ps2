"""Convert the game's PSMF movies (.pmf: H.264 video + ATRAC3+ audio) for the PS2 port.

The EE cannot decode H.264 in real time, but the PS2's IPU decodes MPEG-2 (ps2sdk
libmpeg). So, once, offline:
  <name>.m2v   MPEG-2 video elementary stream, same size (480x272) and frame rate
  <name>.at3p  'AT3P', u32 bytes per frame, u32 channels, u32 0, then the movie's
               ATRAC3+ frames unchanged
  <name>.at3   the same frames in a RIFF/WAVE container, only to check them with ffmpeg
The port's sceMpeg HLE (port/ps2/ps2_mpeg.c) plays these when the game opens <name>.pmf.

PSMF audio: private stream 1 (0xBD), substream 0. Each PES payload starts with 4 bytes
(substream id + 3), then units of an 8-byte header 0F D0 hh ll .. followed by one
ATRAC3+ frame of ((hh & 3) << 8 | ll) * 8 + 8 bytes; units may span PES packets.

usage: pmf_convert.py <movie dir> <out dir> [names...]   (needs ffmpeg on PATH)
"""
import os
import struct
import subprocess
import sys




def demux_audio(pmf):
    off = struct.unpack('>I', pmf[8:12])[0]
    i, es = off, bytearray()
    while i < len(pmf) - 6:
        if pmf[i:i + 3] != b'\x00\x00\x01':
            i += 1
            continue
        sid = pmf[i + 3]
        if sid == 0xBA:                                   # MPEG-2 pack header
            i += 14 + (pmf[i + 13] & 7)
            continue
        if sid == 0xB9:                                   # program end
            break
        ln = struct.unpack('>H', pmf[i + 4:i + 6])[0]
        if sid == 0xBD:
            hl = pmf[i + 8]
            payload = pmf[i + 9 + hl:i + 6 + ln]
            if payload and payload[0] == 0x00:
                es += payload[4:]
        i += 6 + ln
    frames, j = [], 0
    while j + 8 <= len(es):
        if es[j] != 0x0F or es[j + 1] != 0xD0:
            j += 1                                        # resync (should not happen)
            continue
        size = (((es[j + 2] & 3) << 8) | es[j + 3]) * 8 + 8
        if j + 8 + size > len(es):
            break
        frames.append(bytes(es[j + 8:j + 8 + size]))
        demux_audio.channels = (es[j + 2] >> 2) & 7
        j += 8 + size
    return frames


def write_riff(path, frames, template_at3):
    """RIFF/WAVE around the frames, fmt (ATRAC3+ 0xFFFE) copied from a game .at3."""
    t = open(template_at3, 'rb').read()
    k = t.index(b'fmt ')
    fmt = t[k:k + 8 + struct.unpack('<I', t[k + 4:k + 8])[0]]
    data = b''.join(frames)
    fact = b'fact' + struct.pack('<III', 8, len(frames) * 2048, 0)
    body = b'WAVE' + fmt + fact + b'data' + struct.pack('<I', len(data)) + data
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)


def main(movie_dir, out_dir, names, template_at3):
    os.makedirs(out_dir, exist_ok=True)
    for name in names:
        src = os.path.join(movie_dir, name + '.pmf')
        pmf = open(src, 'rb').read()
        assert pmf[:4] == b'PSMF', src
        frames = demux_audio(pmf)
        sizes = sorted(set(len(f) for f in frames))
        assert len(sizes) == 1, (name, sizes)
        # 16-byte header: 'AT3P', bytes per frame, channels, 0 -- then the frames
        hdr = b'AT3P' + struct.pack('<III', sizes[0], demux_audio.channels, 0)
        open(os.path.join(out_dir, name + '.at3p'), 'wb').write(hdr + b''.join(frames))
        if template_at3:
            write_riff(os.path.join(out_dir, name + '.at3'), frames, template_at3)
        m2v = os.path.join(out_dir, name + '.m2v')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-an', '-c:v', 'mpeg2video',
                        '-b:v', '3000k', '-maxrate', '6000k', '-bufsize', '1835k', '-bf', '0', '-g', '15',
                        '-pix_fmt', 'yuv420p', '-f', 'mpeg2video', m2v], check=True)
        print('%s: %d ATRAC3+ frames of %d bytes (%.1f s), video %.1f MB'
              % (name, len(frames), sizes[0], len(frames) * 2048 / 44100.0, os.path.getsize(m2v) / 1e6))


if __name__ == '__main__':
    movie_dir, out_dir = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or ['opening', 'ending', 'ons', 'soundstorm']
    main(movie_dir, out_dir, names, os.path.join(out_dir, '..', '..', 'at3ptest.at3'))   # a game AT3+ track (title_bgm1)
