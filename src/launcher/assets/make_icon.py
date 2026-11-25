"""The launcher's icon (src/launcher/assets/launcher.ico): the station that is open, as the launcher draws
it - an orange ring on a pale line of the metro map, on black. Drawn large and scaled down. Needs Pillow.

    python src/launcher/assets/make_icon.py src/launcher/assets/launcher.ico [preview.png]
"""
import sys

from PIL import Image, ImageDraw

S = 1024
C = S / 2
BG, EDGE, LINE, ORANGE = (10, 10, 10), (38, 37, 35), (120, 117, 111), (240, 100, 30)


def main(out, preview=None):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((24, 24, S - 24, S - 24), radius=176, fill=EDGE)
    d.rounded_rectangle((44, 44, S - 44, S - 44), radius=158, fill=BG)
    d.rectangle((C - 38, 44, C + 38, S - 44), fill=LINE)   # the line
    d.ellipse((C - 300, C - 300, C + 300, C + 300), fill=BG)  # (a gap around the station)
    d.ellipse((C - 262, C - 262, C + 262, C + 262), fill=ORANGE)
    d.ellipse((C - 112, C - 112, C + 112, C + 112), fill=BG)
    im.save(out, format="ICO", sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    if preview:
        im.resize((256, 256), Image.LANCZOS).save(preview)
    print("written:", out)


if __name__ == "__main__":
    main(*sys.argv[1:3])
