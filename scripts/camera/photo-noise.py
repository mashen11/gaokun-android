#!/usr/bin/env python3
# 量一张照片的"噪点"：不要用整图/整块的标准差（会被光照渐变和边缘主导，#112 里我先这么量、数字几乎不动），
# 用【高频残差】= 像素减去 5×5 盒式均值后的标准差；再报一下过曝比例（闪光样片的主要问题是过曝不是噪点）。
# 用法：python3 scripts/camera/photo-noise.py a.jpg [b.jpg ...]   （需要 Pillow）
import sys
from PIL import Image, ImageFilter, ImageChops, ImageStat
for f in sys.argv[1:]:
    im = Image.open(f).convert('RGB'); w, h = im.size
    d = ImageChops.difference(im, im.filter(ImageFilter.BoxBlur(2)))
    hf = sum(ImageStat.Stat(d).stddev) / 3
    small = im.resize((200, 200))
    clipped = sum(1 for p in small.getdata() if min(p) >= 250) / 40000
    mean = [round(x) for x in ImageStat.Stat(im).mean]
    print(f"{f}: {w}x{h} meanRGB={mean} hf-noise={hf:.2f} clipped={clipped*100:.0f}%")
