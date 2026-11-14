# Genera assets/pasarela.ico (sin dependencias: solo la biblioteca estándar).
# Diseño: cuadrado redondeado azul con un "play" blanco y dos ondas de sonido.
import math
import struct
import sys
import zlib


def render(size):
    ss = 4  # supermuestreo
    n = size * ss
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    acc = [[[0.0, 0.0, 0.0, 0.0] for _ in range(size)] for _ in range(size)]
    r = 0.22 * n
    top = (0x5B, 0x8A, 0xFF)
    bottom = (0x3A, 0x5B, 0xE0)

    def inside_round_rect(x, y):
        m = 0.04 * n
        x0, y0, x1, y1 = m, m, n - m, n - m
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and x0 <= x <= x1 and y0 <= y <= y1

    # triángulo "play"
    tri = [(0.30 * n, 0.30 * n), (0.30 * n, 0.70 * n), (0.58 * n, 0.50 * n)]

    def in_tri(x, y):
        (ax, ay), (bx, by), (cx, cy) = tri
        d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
        d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
        d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    def in_wave(x, y, radius, width):
        cx, cy = 0.50 * n, 0.50 * n
        d = math.hypot(x - cx, y - cy)
        ang = math.atan2(y - cy, x - cx)
        return abs(d - radius) <= width / 2 and abs(ang) <= math.radians(42)

    for yy in range(n):
        for xx in range(n):
            x, y = xx + 0.5, yy + 0.5
            if not inside_round_rect(x, y):
                continue
            t = y / n
            col = tuple(top[i] * (1 - t) + bottom[i] * t for i in range(3))
            white = in_tri(x, y) or in_wave(x, y, 0.20 * n, 0.065 * n) or in_wave(x, y, 0.32 * n, 0.065 * n)
            if white:
                col = (255, 255, 255)
            a = acc[yy // ss][xx // ss]
            a[0] += col[0]
            a[1] += col[1]
            a[2] += col[2]
            a[3] += 1
    out = bytearray()
    for y in range(size):
        out.append(0)  # filtro PNG
        for x in range(size):
            a = acc[y][x]
            cov = a[3] / (ss * ss)
            if a[3]:
                out += bytes([int(a[0] / a[3]), int(a[1] / a[3]), int(a[2] / a[3]), int(round(cov * 255))])
            else:
                out += bytes([0, 0, 0, 0])
    return bytes(out)


def png(size):
    raw = render(size)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main(path):
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [png(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + b"".join(images))
    # Vista previa para el README
    with open(path.replace(".ico", ".png"), "wb") as f:
        f.write(images[-1])


if __name__ == "__main__":
    main(sys.argv[1])
