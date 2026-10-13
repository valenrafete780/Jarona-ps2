#!/usr/bin/env python3
"""
Convierte jarona/index.html + jarona/audio/*.mp3 en los datos que lee la version PS2:

  data/TEX.DAT     texturas (RGBA8, sin relleno) con una tabla de nombres
  data/TREE.DAT    escena secreta (gif) en 8 bits con paleta: se carga en segundo plano
  data/SFX.DAT     los 4 efectos juntos (s16le, mono, 22050 Hz): cada fichero extra cuesta una latencia fija de disco
  data/*.ULW       musica y pista secreta: mu-law de 8 bits, mono, 22050 Hz (la mitad de tamano, ~37 dB SNR)

Uso:  python3 tools/convert_assets.py ../jarona  [--font /ruta/fuente.ttf]
Requiere: Pillow y ffmpeg.
"""
import argparse, base64, io, os, re, struct, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument("src", help="carpeta 'jarona' (la que contiene index.html y audio/)")
ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data"))
ap.add_argument("--font", default=None)
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)

html = open(os.path.join(args.src, "index.html"), encoding="utf-8").read()
textures = []          # (nombre, PIL.Image RGBA)


def add(name, im):
    assert len(name) < 16
    textures.append((name, im.convert("RGBA")))


def b64img(b64):
    return Image.open(io.BytesIO(base64.b64decode(b64)))


# ---------------- sprites incrustados en el HTML ----------------
for idn in ("flowery", "pose"):
    m = re.search(r'id="%s"[^>]*?src="data:image/png;base64,([A-Za-z0-9+/=]+)"' % idn, html)
    add(idn, b64img(m.group(1)))

chars = re.findall(r'<img class="char"[^>]*?src="data:image/png;base64,([A-Za-z0-9+/=]+)"', html)
for i, c in enumerate(chars):
    add("char%d" % i, b64img(c))

frames = re.search(r'var FRAMES = \[(.*?)\];', html, re.S).group(1)
for i, f in enumerate(re.findall(r'"([A-Za-z0-9+/=]+)"', frames)):
    add("power%d" % i, b64img(f))

om = re.search(r'var OM = \{ cols: (\d+), count: (\d+), w: (\d+), h: (\d+)', html)
cols, count, w, h = map(int, om.groups())
sheet = b64img(re.search(r'omegaImg\.src = "data:image/png;base64,([A-Za-z0-9+/=]+)"', html).group(1)).convert("RGBA")
for i in range(count):
    x, y = (i % cols) * w, (i // cols) * h
    add("om%02d" % i, sheet.crop((x, y, x + w, y + h)))

tree = b64img(re.search(r'<img class="tree"[^>]*src="data:image/gif;base64,([A-Za-z0-9+/=]+)"', html).group(1))
tree_frames, tree_ms = [], []
for i in range(tree.n_frames):
    tree.seek(i)
    tree_frames.append(tree.convert("RGBA"))    # convert() compone cada fotograma completo
    tree_ms.append(int(tree.info.get("duration", 100)))

# ---------------- sombra del suelo (gradiente radial del CSS) ----------------
fw, fh = 102, 18
fl = Image.new("RGBA", (fw, fh))
px = fl.load()
for y in range(fh):
    for x in range(fw):
        dx = (x + .5 - fw / 2) / (fw / 2)
        dy = (y + .5 - fh / 2) / (fh / 2)
        d = (dx * dx + dy * dy) ** .5
        a = max(0.0, 1 - d / 0.7) * .38
        px[x, y] = (59, 31, 0, int(a * 255))
add("floor", fl)

# ---------------- textos ----------------
# La fuente Pixelify Sans no se puede usar en la consola: se dibuja con una TTF y se amplia sin suavizado.
cands = [args.font] if args.font else []
cands += ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf",
          "/Library/Fonts/Arial Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"]
font_path = next((c for c in cands if c and os.path.exists(c)), None)
if not font_path:
    sys.exit("No encuentro ninguna fuente TTF: usa --font /ruta/a/fuente.ttf")
BROWN, CREAM = (59, 31, 0, 255), (255, 243, 166, 255)


def text_img(txt, size, k, color=BROWN, shadow=None):
    f = ImageFont.truetype(font_path, size)
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    l, t, r, b = probe.textbbox((0, 0), txt, font=f)
    im = Image.new("RGBA", (r - l + 2, b - t + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.fontmode = "1"      # sin antialias -> pixeles
    if shadow:
        d.text((1 - l, 1 - t), txt, font=f, fill=shadow)
    d.text((-l, -t), txt, font=f, fill=color)
    return im.resize((im.width * k, im.height * k), Image.NEAREST)


def button(txt, size, k, pressed, pad_x, height):
    t = text_img(txt, size, k)
    bw, bh = t.width + pad_x * 2, height
    im = Image.new("RGBA", (bw, bh + 6), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    oy = 3 if pressed else 0
    sh = 1 if pressed else 4
    d.rounded_rectangle((0, oy + sh, bw - 1, oy + bh - 1 + sh), 10, fill=BROWN)           # sombra
    d.rounded_rectangle((0, oy, bw - 1, oy + bh - 1), 10, fill=CREAM, outline=BROWN, width=3)
    im.alpha_composite(t, ((bw - t.width) // 2, oy + (bh - t.height) // 2))
    return im


add("title", text_img("JARONA!", 14, 4, BROWN, CREAM))
add("loading", text_img("Cargando...", 8, 3))
add("hint", text_img("X: tocar    Triangulo: cambiar", 7, 2))
for nm, label, size, k, px_, hh in (("start", "\u00a1Empezar!", 10, 3, 20, 54),
                                    ("btnA", "Cambiar a Omega Flowery", 8, 2, 16, 46),
                                    ("btnB", "Cambiar a Flowery", 8, 2, 16, 46)):
    add(nm, button(label, size, k, False, px_, hh))
    add(nm + "_p", button(label, size, k, True, px_, hh))

# ---------------- TEX.DAT ----------------
off = 8 + len(textures) * 32
entries, blobs = [], []
for name, im in textures:
    data = im.tobytes()
    entries.append(struct.pack("<16sIIII", name.encode(), im.width, im.height, off, len(data)))
    blobs.append(data)
    off += len(data)
with open(os.path.join(args.out, "TEX.DAT"), "wb") as f:
    f.write(b"JTEX" + struct.pack("<I", len(textures)))
    for e in entries:
        f.write(e)
    for b in blobs:
        f.write(b)
print("TEX.DAT: %d texturas, %.2f MB" % (len(textures), off / 1048576))

# ---------------- TREE.DAT (8 bits + paleta) ----------------
pal = []
for fr in tree_frames:
    for px in fr.getdata():
        if px not in pal:
            pal.append(px)
if len(pal) > 256:
    sys.exit("El gif tiene %d colores: no cabe en 8 bits" % len(pal))
index = {p: i for i, p in enumerate(pal)}
tw, th = tree_frames[0].size
with open(os.path.join(args.out, "TREE.DAT"), "wb") as f:
    f.write(b"JTRE" + struct.pack("<IIII", len(tree_frames), tw, th, len(pal)))
    f.write(struct.pack("<%dI" % len(tree_ms), *tree_ms))
    for p in pal:
        f.write(bytes(p))
    for fr in tree_frames:
        f.write(bytes(index[p] for p in fr.getdata()))
print("TREE.DAT: %d fotogramas, %d colores, %.2f MB" % (len(tree_frames), len(pal), os.path.getsize(os.path.join(args.out, "TREE.DAT")) / 1048576))

# ---------------- audio (cada pista por separado: asi se pueden mezclar) ----------------
for name, fmt, ext in (("jarona", "s16le", "PCM"), ("ultimo", "s16le", "PCM"), ("poder", "s16le", "PCM"), ("omega", "s16le", "PCM"),
                       ("secreto", "mulaw", "ULW"), ("musica", "mulaw", "ULW")):
    src = os.path.join(args.src, "audio", name + ".mp3")
    dst = os.path.join(args.out, name.upper() + "." + ext)
    subprocess.check_call(["ffmpeg", "-y", "-v", "error", "-i", src, "-ac", "1", "-ar", "22050", "-f", fmt, dst])
    bps = 2 if fmt == "s16le" else 1
    print("%s: %.1f s, %.2f MB" % (os.path.basename(dst), os.path.getsize(dst) / bps / 22050, os.path.getsize(dst) / 1048576))

# SFX.DAT: "JSFX" + 4 tamanos (jarona, ultimo, poder, omega) + datos; se borran los .PCM sueltos
names = ("JARONA", "ULTIMO", "PODER", "OMEGA")
blobs = [open(os.path.join(args.out, n + ".PCM"), "rb").read() for n in names]
with open(os.path.join(args.out, "SFX.DAT"), "wb") as f:
    f.write(b"JSFX" + struct.pack("<4I", *[len(b) for b in blobs]))
    for b in blobs:
        f.write(b)
for n in names:
    os.remove(os.path.join(args.out, n + ".PCM"))
print("SFX.DAT: %.2f MB" % (os.path.getsize(os.path.join(args.out, "SFX.DAT")) / 1048576))
