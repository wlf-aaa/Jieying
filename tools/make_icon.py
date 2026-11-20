"""Generates assets/jieying.ico and assets/jieying.png (the app icon).

Blue rounded square with four white corner brackets, like a selection frame.
Each size is drawn large and downsampled, so small sizes stay smooth.
Usage: python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SUPERSAMPLE = 1024
ASSETS = Path(__file__).resolve().parent.parent / "assets"


def draw(size: int) -> Image.Image:
    s = SUPERSAMPLE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Vertical gradient body.
    margin = s * 2 // 64
    body = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    bd = ImageDraw.Draw(body)
    top, bottom = (58, 150, 245), (24, 108, 214)
    for y in range(s):
        t = y / (s - 1)
        bd.line([(0, y), (s, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([margin, margin, s - margin, s - margin], radius=s * 14 // 64, fill=255)
    img.paste(body, (0, 0), mask)

    # Corner brackets; thicker at small sizes so they survive downsampling.
    width = round(s * (7 if size <= 24 else 6) / 64)
    a, b = s * 15 / 64, s * 49 / 64
    length = s * 12 / 64
    corners = [
        [(a, a + length), (a, a), (a + length, a)],
        [(b - length, a), (b, a), (b, a + length)],
        [(b, b - length), (b, b), (b - length, b)],
        [(a + length, b), (a, b), (a, b - length)],
    ]
    for points in corners:
        d.line(points, fill="white", width=width, joint="curve")
        for x, y in (points[0], points[2]):  # round caps
            d.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill="white")
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    ASSETS.mkdir(exist_ok=True)
    images = [draw(size) for size in SIZES]
    images[-1].save(ASSETS / "jieying.png")
    images[-1].save(ASSETS / "jieying.ico", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print("wrote", ASSETS / "jieying.ico", "and", ASSETS / "jieying.png")


if __name__ == "__main__":
    main()
