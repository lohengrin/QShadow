# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guillaume Millet

"""Rasterizes the shapes of qshadow.svg into qshadow.png and qshadow.ico.

Run from the repo root: python icons/make_icons.py   (needs Pillow)
"""
from PIL import Image, ImageDraw

S = 1024  # supersampled canvas, svg units are 256


def render():
    k = S / 256
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def rr(x, y, w, h, r, fill):
        d.rounded_rectangle([x * k, y * k, (x + w) * k - 1, (y + h) * k - 1], r * k, fill=fill)

    rr(0, 0, 256, 256, 56, "#1b1e2b")
    rr(28, 72, 148, 112, 18, "#3a4157")
    d.polygon([(p[0] * k, p[1] * k) for p in [(176, 104), (228, 76), (228, 180), (176, 152)]],
              fill="#e05a4e")
    d.ellipse([(102 - 30) * k, (128 - 30) * k, (102 + 30) * k, (128 + 30) * k], fill="#5b6479")
    rr(60, 196, 66, 14, 7, "#3a4157")
    return img


big = render()
big.resize((256, 256), Image.LANCZOS).save("icons/qshadow.png")
big.save("icons/qshadow.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
