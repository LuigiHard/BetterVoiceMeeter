"""Renders the BetterVoiceMeeter logo: three mixer faders on a dark rounded tile.
Run: python Resources/make_logo.py  (needs Pillow). Outputs icon PNGs next to this script."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
S = 2048  # supersampled canvas, downscaled for smooth edges

BG_TOP, BG_BOTTOM = (38, 44, 56), (17, 19, 24)
SLOT = (10, 12, 16)
FADERS = [  # (colour, level 0..1)
    ((90, 180, 255), 0.58),   # blue  - hardware inputs
    ((255, 92, 138), 0.86),   # pink  - Sing Studio
    ((63, 212, 154), 0.40),   # green - buses
]


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return m


def render() -> Image.Image:
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Tile with a vertical gradient.
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        gd.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)) + (255,))
    img.paste(grad, (0, 0), rounded_mask(S, int(S * 0.22)))

    d = ImageDraw.Draw(img)
    slot_w, slot_top, slot_bottom = int(S * 0.085), int(S * 0.20), int(S * 0.80)
    xs = [int(S * 0.27), int(S * 0.50), int(S * 0.73)]
    cap_w, cap_h = int(S * 0.20), int(S * 0.075)

    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    for x, (colour, level) in zip(xs, FADERS):
        # Slot
        d.rounded_rectangle((x - slot_w // 2, slot_top, x + slot_w // 2, slot_bottom), radius=slot_w // 2, fill=SLOT + (255,))
        # Level fill (meter) from the bottom up to the fader cap
        cap_y = int(slot_bottom - (slot_bottom - slot_top) * level)
        fill_w = int(slot_w * 0.62)
        d.rounded_rectangle((x - fill_w // 2, cap_y, x + fill_w // 2, slot_bottom - int(slot_w * 0.19)),
                            radius=fill_w // 2, fill=colour + (255,))
        gdraw.rounded_rectangle((x - fill_w, cap_y, x + fill_w, slot_bottom), radius=fill_w, fill=colour + (120,))
        # Fader cap
        d.rounded_rectangle((x - cap_w // 2, cap_y - cap_h // 2, x + cap_w // 2, cap_y + cap_h // 2),
                            radius=int(cap_h * 0.35), fill=(236, 240, 246, 255))
        d.rounded_rectangle((x - cap_w // 2 + int(cap_w * 0.14), cap_y - int(cap_h * 0.09),
                             x + cap_w // 2 - int(cap_w * 0.14), cap_y + int(cap_h * 0.09)),
                            radius=int(cap_h * 0.09), fill=colour + (255,))

    # Soft glow under the meters, kept inside the tile.
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.03))
    tile = rounded_mask(S, int(S * 0.22))
    glow.putalpha(Image.composite(glow.getchannel("A"), Image.new("L", (S, S), 0), tile))
    return Image.alpha_composite(glow, img)


if __name__ == "__main__":
    big = render()
    for size in (1024, 512, 256, 64, 32):
        big.resize((size, size), Image.LANCZOS).save(HERE / f"icon-{size}.png")
    print("wrote", ", ".join(f"icon-{s}.png" for s in (1024, 512, 256, 64, 32)))
