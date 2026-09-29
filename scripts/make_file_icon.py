# 生成 app/pdf_file.ico：LumenPDF 关联 .pdf 后资源管理器里显示的文档图标。
# 每个尺寸单独绘制（8 倍超采样后缩小），小尺寸去掉细节保证清晰。
# 用法：python scripts/make_file_icon.py [输出路径]；需要 Pillow。
import sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\segoeuib.ttf", r"C:\Windows\Fonts\arialbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]

def font(px):
    for f in FONT_CANDIDATES:
        if os.path.exists(f):
            return ImageFont.truetype(f, px)
    return ImageFont.load_default()

def vgrad(w, h, top, bottom):
    g = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g.resize((w, h))

def draw(size):
    k = 8; S = size * k
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    small = size <= 24
    # 页面轮廓（右上角折角）。小尺寸时页面更宽，以免显得细长。
    x0, x1 = (0.16 if small else 0.19) * S, (0.84 if small else 0.81) * S
    y0, y1 = 0.05 * S, 0.95 * S
    fold = (0.30 if small else 0.26) * (x1 - x0)
    page = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    # 柔和投影
    if size >= 32:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(sh).polygon([(x + 0.012 * S, y + 0.02 * S) for x, y in page], fill=(0, 0, 0, 70))
        img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(0.02 * S)))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon(page, fill=255)
    fill = vgrad(S, S, (255, 255, 255, 255), (226, 228, 232, 255))
    img.paste(fill, (0, 0), mask)
    d = ImageDraw.Draw(img)
    stroke = max(k, int(S / 64))
    d.line(page + [page[0]], fill=(120, 124, 132, 255) if small else (150, 154, 162, 255), width=stroke, joint="curve")
    # 折角：浅灰三角 + 边线
    tri = [(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)]
    d.polygon(tri, fill=(206, 210, 216, 255))
    d.line(tri + [tri[0]], fill=(150, 154, 162, 255) if not small else (120, 124, 132, 255), width=stroke, joint="curve")
    # 正文线条（中等以上尺寸）
    if size >= 32:
        lx0 = x0 + 0.13 * (x1 - x0); lh = max(k, int(S * 0.028))
        rows = [(0.20, 0.62), (0.29, 0.78), (0.38, 0.78), (0.47, 0.55)]
        for yy, ww in rows:
            y = y0 + yy * (y1 - y0)
            d.rounded_rectangle([lx0, y, lx0 + ww * (x1 - x0 - 0.26 * (x1 - x0)) , y + lh], radius=lh / 2, fill=(178, 182, 190, 255))
    # 黑色标签：与应用图标的黑底一致，略微伸出页面左侧。
    bx0 = (0.06 if not small else 0.08) * S
    bx1 = (0.74 if not small else 0.84) * S
    by0 = (0.58 if not small else 0.52) * S
    by1 = (0.84 if not small else 0.86) * S
    r = (by1 - by0) * 0.22
    if size >= 48:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle([bx0, by0 + 0.012 * S, bx1, by1 + 0.012 * S], radius=r, fill=(0, 0, 0, 90))
        img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(0.015 * S)))
    label = Image.new("L", (S, S), 0)
    ImageDraw.Draw(label).rounded_rectangle([bx0, by0, bx1, by1], radius=r, fill=255)
    img.paste(vgrad(S, S, (44, 44, 48, 255), (8, 8, 10, 255)), (0, 0), label)
    d = ImageDraw.Draw(img)
    if size >= 32:  # 顶部一道高光，呼应应用图标的玻璃质感
        d.line([(bx0 + r, by0 + stroke), (bx1 - r, by0 + stroke)], fill=(255, 255, 255, 60), width=max(k, stroke // 2))
    # 文字 “PDF”
    if size >= 20:
        text = "PDF"
        target_h = (by1 - by0) * (0.56 if not small else 0.62)
        f = font(int(target_h * 1.38))
        box = d.textbbox((0, 0), text, font=f)
        tw, th = box[2] - box[0], box[3] - box[1]
        maxw = (bx1 - bx0) * 0.80
        if tw > maxw:
            f = font(int(target_h * 1.38 * maxw / tw)); box = d.textbbox((0, 0), text, font=f); tw, th = box[2] - box[0], box[3] - box[1]
        tx = (bx0 + bx1) / 2 - tw / 2 - box[0]
        ty = (by0 + by1) / 2 - th / 2 - box[1]
        d.text((tx, ty), text, font=f, fill=(255, 255, 255, 255))
    else:  # 16 px：白色短线代替文字
        cy = (by0 + by1) / 2; h = max(k, S * 0.07)
        d.rectangle([bx0 + 0.18 * (bx1 - bx0), cy - h / 2, bx1 - 0.18 * (bx1 - bx0), cy + h / 2], fill=(255, 255, 255, 255))
    return img.resize((size, size), Image.LANCZOS)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "app", "pdf_file.ico")
    images = [draw(s) for s in SIZES]
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    preview = Image.new("RGBA", (sum(SIZES) + 12 * len(SIZES), 256), (32, 32, 32, 255))
    x = 6
    for s, im in zip(SIZES, images):
        preview.alpha_composite(im, (x, 256 - s)); x += s + 12
    preview.save(os.path.splitext(out)[0] + "-preview.png")

if __name__ == "__main__":
    main()
