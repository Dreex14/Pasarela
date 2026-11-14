# Compone la imagen de portada del README a partir de dos capturas reales:
#   python tools/make_cover.py <ventana_video.png> <ventana_principal.png> <salida.png>
# Fondo degradado, ventanas con esquinas redondeadas, borde fino y sombra.
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

W, H = 1600, 900
RADIUS = 14


def trim_black(img):
    """Quita el marco negro que deja PrintWindow alrededor de la ventana."""
    rgb = img.convert("RGB")
    mask = rgb.point(lambda v: 255 if v > 12 else 0).convert("L")
    box = mask.getbbox()
    return img.crop(box) if box else img


def rounded(img, radius):
    img = img.convert("RGBA")
    m = Image.new("L", img.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, img.width - 1, img.height - 1), radius, fill=255)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), m))
    border = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(border).rounded_rectangle((0, 0, img.width - 1, img.height - 1), radius,
                                             outline=(255, 255, 255, 38), width=1)
    return Image.alpha_composite(img, border)


def background(W=W, H=H):
    bg = Image.new("RGB", (W, H))
    px = bg.load()
    a, b = (13, 15, 22), (22, 28, 52)
    for y in range(H):
        for x in range(W):
            t = (x / W) * 0.45 + (y / H) * 0.55
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    bg = bg.convert("RGBA")
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    d.ellipse((W * 0.56, -H * 0.39, W * 1.19, H * 0.61), fill=(79, 125, 255, 70))
    d.ellipse((-W * 0.19, H * 0.5, W * 0.38, H * 1.39), fill=(140, 90, 255, 45))
    glow = glow.filter(ImageFilter.GaussianBlur(160))
    return Image.alpha_composite(bg, glow)


def paste_with_shadow(canvas, img, pos, blur=28, offset=18, alpha=170):
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sil = Image.new("RGBA", img.size, (0, 0, 0, alpha))
    sil.putalpha(ImageChops.multiply(img.getchannel("A"), sil.getchannel("A")))
    shadow.paste(sil, (pos[0], pos[1] + offset), sil)
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur))
    canvas = Image.alpha_composite(canvas, shadow)
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(img, pos, img)
    return Image.alpha_composite(canvas, layer)


def main(video_path, main_path, out_path):
    video = trim_black(Image.open(video_path))
    win = trim_black(Image.open(main_path))

    vw = 1180
    video = video.resize((vw, round(video.height * vw / video.width)), Image.LANCZOS)
    mh = 740
    win = win.resize((round(win.width * mh / win.height), mh), Image.LANCZOS)

    canvas = background()
    vx, vy = W - vw - 70, (H - video.height) // 2 - 30
    canvas = paste_with_shadow(canvas, rounded(video, RADIUS), (vx, vy))
    mx, my = 90, (H - win.height) // 2 + 20
    canvas = paste_with_shadow(canvas, rounded(win, RADIUS), (mx, my), blur=34, offset=22, alpha=200)
    canvas.convert("RGB").save(out_path, optimize=True)
    print(out_path, canvas.size)


def single(path, out_path, margin=70):
    """Una sola ventana sobre el mismo fondo (p. ej. la de ajustes)."""
    win = rounded(trim_black(Image.open(path)), RADIUS)
    canvas = background(win.width + 2 * margin, win.height + 2 * margin)
    canvas = paste_with_shadow(canvas, win, (margin, margin - 8))
    canvas.convert("RGB").save(out_path, optimize=True)
    print(out_path, canvas.size)


if __name__ == "__main__":
    if sys.argv[1] == "--una":
        single(sys.argv[2], sys.argv[3])
    else:
        main(*sys.argv[1:4])
