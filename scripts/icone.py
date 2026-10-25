"""The launcher's symbol: a bald head with a short black beard, no face, on the theme's red. Drawn at 4x and scaled
down for smooth edges. Writes assets/icone.png (512) and assets/icone.ico (16 to 256), and out/icone-previa.png.
Usage: py -3 scripts/icone.py"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 2048  # drawing size
out = Path(__file__).resolve().parent.parent / 'assets'

SKIN, EAR, EAR_IN, BEARD = (244, 236, 228, 255), (226, 214, 202, 255), (196, 180, 166, 255), (20, 18, 18, 255)
LIPS = (200, 96, 96, 255)  # the grin's lips
TEETH = (250, 248, 244, 255)
GAP, GAP_LIGHT = (70, 30, 30, 255), (196, 190, 184, 255)  # between the rows, between the teeth
CX, TOP, W, H = S // 2, int(S * 0.15), int(S * 0.235), int(S * 0.62)  # centre, top, half width, height

# the right half of the head, x in half widths from the centre, y from the top (0) to the chin (1)
OUTLINE = [(0.0, 0.0), (0.55, 0.035), (0.86, 0.17), (0.99, 0.36), (0.99, 0.50), (0.93, 0.64), (0.80, 0.78),
           (0.56, 0.92), (0.26, 0.993), (0.0, 1.0)]


def lerp(a, b, t):
    return tuple(int(x + (y - x) * t) for x, y in zip(a, b))


def spline(pts, steps=24):
    """Catmull-Rom through the points, closed."""
    out, n = [], len(pts)
    for i in range(n):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        for k in range(steps):
            t = k / steps
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * p1[j] + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 +
                                    (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in range(2)))
    return out


def head_points(sx=1.0, sy=1.0, dy=0.0):
    right = [(x * sx, y * sy + dy) for x, y in OUTLINE]
    left = [(-x, y) for x, y in reversed(OUTLINE[1:-1])]
    pts = spline(right + left)
    return [(CX + x * W, TOP + y * H) for x, y in pts]


def mask_of(points):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).polygon(points, fill=255)
    return m


def symbol():
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # badge: rounded square, red from top to bottom
    grad = Image.new('RGBA', (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=lerp((214, 38, 38), (118, 12, 14), y / S) + (255,))
    badge = Image.new('L', (S, S), 0)
    ImageDraw.Draw(badge).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    img.paste(grad, (0, 0), badge)

    layer = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    # ears, behind the head: from the brow to below the cheekbone
    ew, et, eb = int(S * 0.052), TOP + int(H * 0.39), TOP + int(H * 0.63)
    for side in (-1, 1):
        ex = CX + side * int(W * 0.97)
        ld.ellipse([ex - ew, et, ex + ew, eb], fill=EAR)
        ld.ellipse([ex - int(ew * 0.45) + side * int(ew * 0.18), et + int((eb - et) * 0.2),
                    ex + int(ew * 0.45) + side * int(ew * 0.18), eb - int((eb - et) * 0.25)], fill=EAR_IN)
    head = mask_of(head_points())
    layer.paste(Image.new('RGBA', (S, S), SKIN), (0, 0), head)
    # beard: a little fuller than the jaw, below a line from the sideburns low across the cheeks
    fuller = mask_of(head_points(sx=1.03, sy=1.0, dy=0.022))
    line = []
    for i in range(201):
        t = i / 100 - 1
        line.append((CX + t * W * 1.1, TOP + H * (0.75 - 0.24 * abs(t) ** 2.4)))
    below = mask_of(line + [(CX + W * 1.1, S), (CX - W * 1.1, S)])
    # the grin's corners and its upper lip; the moustache follows that lip: a band over it, thicker in the middle,
    # its ends running on past the corners and turning up with them
    half, corner = W * 0.72, TOP + H * 0.625

    def lip(x):
        return corner + H * 0.055 * (1 - (x / half) ** 2)

    tw = W * 0.86
    xs = [tw * (i / 50 - 1) for i in range(101)]
    upper = [(CX + x, lip(x) - H * 0.088 * (1 - 0.6 * (x / tw) ** 2)) for x in xs]
    lower = [(CX + x, lip(x) + H * 0.012) for x in reversed(xs)]
    below = Image.composite(Image.new('L', (S, S), 255), below, mask_of(upper + lower))
    # sideburns: up the sides of the head in front of the ears to the ears' top, narrower at the top
    for side in (-1, 1):
        inner = [(0.70, 0.68), (0.76, 0.60), (0.82, 0.53), (0.86, 0.46), (0.89, 0.405)]  # bottom to top
        burn = [(CX + side * W * x, TOP + H * yy) for x, yy in inner] + [(CX + side * W * 1.1, TOP + H * 0.385),
                                                                          (CX + side * W * 1.1, TOP + H * 0.70)]
        below = Image.composite(Image.new('L', (S, S), 255), below, mask_of(burn))
    beard = Image.composite(Image.composite(Image.new('L', (S, S), 255), head, fuller), Image.new('L', (S, S), 0), below)
    layer.paste(Image.new('RGBA', (S, S), BEARD), (0, 0), beard)
    # a grin from cheek to cheek, as the Mask's: lips, two rows of big teeth, the line between them, the gaps
    def y(u, corner, depth, power):  # a curve between the corners, depth below them at the centre
        return corner + depth * (1 - u * u) ** power

    def arc(half, corner, depth, power, n=80):
        return [(CX + half * u, y(u, corner, depth, power)) for u in (i / (n / 2) - 1 for i in range(n + 1))]

    md = ImageDraw.Draw(layer)
    md.polygon(arc(half, corner, H * 0.055, 1.0) + arc(half, corner, H * 0.285, 0.7)[::-1], fill=LIPS)
    th, tc = W * 0.665, corner + H * 0.014
    top, mid, bottom = (H * 0.052, 1.0), (H * 0.150, 0.8), (H * 0.243, 0.7)
    md.polygon(arc(th, tc, *top) + arc(th, tc, *bottom)[::-1], fill=TEETH)
    md.line(arc(th * 0.985, tc, *mid), fill=GAP, width=int(S * 0.009), joint='curve')
    # the gaps, in perspective (wide teeth in front, narrow at the corners): the two front teeth meet at the centre,
    # the lower row sits half a tooth over
    gap = int(S * 0.007)
    for k in range(-3, 4):
        u = math.sin(k / 4.6 * math.pi / 2)
        md.line([(CX + th * u, y(u, tc, *top)), (CX + th * u * 0.985, y(u, tc, *mid))], fill=GAP_LIGHT, width=gap)
    for k in range(-3, 3):
        u = math.sin((k + 0.5) / 4.6 * math.pi / 2)
        md.line([(CX + th * u * 0.985, y(u, tc, *mid)), (CX + th * u * 0.96, y(u, tc, *bottom))], fill=GAP_LIGHT, width=gap)
    # the bald top's shine
    shine = Image.new('L', (S, S), 0)
    ImageDraw.Draw(shine).ellipse([CX - int(W * 0.62), TOP + int(H * 0.05), CX - int(W * 0.05), TOP + int(H * 0.19)], fill=105)
    shine = shine.filter(ImageFilter.GaussianBlur(S // 55))
    layer.paste(Image.new('RGBA', (S, S), (255, 255, 255, 255)), (0, 0), Image.composite(shine, Image.new('L', (S, S), 0), head))
    # a soft shadow of the head on the badge
    shadow = layer.split()[3].filter(ImageFilter.GaussianBlur(S // 70)).point(lambda v: int(v * 0.5))
    img.paste(Image.new('RGBA', (S, S), (45, 0, 0, 255)), (S // 90, S // 60), Image.composite(shadow, Image.new('L', (S, S), 0), badge))
    img.alpha_composite(layer)
    return img


img = symbol()
out.mkdir(exist_ok=True)
big = img.resize((512, 512), Image.LANCZOS)
big.save(out / 'icone.png')
img.resize((256, 256), Image.LANCZOS).save(out / 'icone.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
# preview: big and the small sizes the taskbar uses
prev = Image.new('RGBA', (512 + 16 + 64 + 16 + 32 + 16 + 16 + 16, 512), (30, 30, 34, 255))
prev.alpha_composite(big, (0, 0))
x = 528
for n in (64, 32, 16):
    prev.alpha_composite(img.resize((n, n), Image.LANCZOS), (x, 240))
    x += n + 16
prev.save(out.parent / 'out' / 'icone-previa.png')
print('ok', out / 'icone.ico')
