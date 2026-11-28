#!/usr/bin/env python3
"""生成一丢小猫头鹰的像素素材。

流程：
1. 在 72x72 逻辑画布上用固定调色板逐像素绘制。
2. 使用最近邻算法放大 5 倍，输出 360x360 透明 PNG。
3. 输出到 scripts/pet_art/output/，可选用 --install 同步到
   main/xingzhi-assets/（固件构建时由 spiffs_assets_gen.py 转成 .spng）。

不使用抗锯齿、模糊或连续渐变；所有形状都由离散矩形/椭圆像素构成。
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from palette import CANVAS, PALETTE, SCALE  # noqa: E402

OUTPUT_DIR = os.path.join(HERE, "output")
INSTALL_DIR = os.path.normpath(
    os.path.join(HERE, "..", "..", "main", "xingzhi-assets")
)

C = {name: rgba[:3] for name, rgba in PALETTE.items()}
SHADOW_ALPHA = PALETTE["shadow"][3]


class PixelCanvas:
    """72x72 逻辑像素画布，透明背景。"""

    def __init__(self) -> None:
        self.size = CANVAS
        self.pixels = [[None for _ in range(self.size)] for _ in range(self.size)]

    def set(self, x: int, y: int, color: str | tuple[int, int, int], alpha: int = 255) -> None:
        if 0 <= x < self.size and 0 <= y < self.size:
            rgb = C[color] if isinstance(color, str) else color
            self.pixels[y][x] = (rgb[0], rgb[1], rgb[2], alpha)

    def get(self, x: int, y: int):
        if 0 <= x < self.size and 0 <= y < self.size:
            return self.pixels[y][x]
        return None

    def sphere(self, cx: float, cy: float, rx: float, ry: float,
               base: str, light: str, dark: str,
               light_dir: tuple[float, float] = (-0.62, -0.78),
               deepest: str | None = None) -> None:
        """三档球面填充：固定左上受光，做出像素风的体积感。

        只用三档纯色（亮/中/暗），不做渐变，放大后仍是硬边像素块。
        """
        lx, ly = light_dir
        x0, x1 = int(cx - rx - 1), int(cx + rx + 1)
        y0, y1 = int(cy - ry - 1), int(cy + ry + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                nx = (x + 0.5 - cx) / rx
                ny = (y + 0.5 - cy) / ry
                r2 = nx * nx + ny * ny
                if r2 > 1.0:
                    continue
                # 越靠球面边缘，法线越偏向轮廓；用投影决定明暗档位。
                nz = (1.0 - r2) ** 0.5
                dot = nx * lx + ny * ly + nz * 0.55
                if dot > 0.74:
                    color = light
                elif dot > 0.34:
                    color = base
                elif dot > 0.16:
                    color = dark
                else:
                    color = deepest if deepest is not None else dark
                self.set(x, y, color)

    def ellipse(self, cx: float, cy: float, rx: float, ry: float,
                color: str, alpha: int = 255) -> None:
        x0, x1 = int(cx - rx - 1), int(cx + rx + 1)
        y0, y1 = int(cy - ry - 1), int(cy + ry + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.set(x, y, color, alpha)

    def ellipse_outline(self, cx: float, cy: float, rx: float, ry: float,
                        color: str, thickness: float = 1.0) -> None:
        x0, x1 = int(cx - rx - 2), int(cx + rx + 2)
        y0, y1 = int(cy - ry - 2), int(cy + ry + 2)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                d = dx * dx + dy * dy
                if (1.0 - thickness / max(rx, ry)) < d <= 1.0:
                    self.set(x, y, color)

    def rect(self, x0: int, y0: int, x1: int, y1: int, color: str,
             alpha: int = 255) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, color, alpha)

    def hline(self, x0: int, x1: int, y: int, color: str) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.set(x, y, color)

    def line(self, x0: int, y0: int, x1: int, y1: int, color: str) -> None:
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.set(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def to_image(self) -> Image.Image:
        img = Image.new("RGBA", (self.size, self.size), (0, 0, 0, 0))
        px = img.load()
        for y in range(self.size):
            for x in range(self.size):
                if self.pixels[y][x] is not None:
                    px[x, y] = self.pixels[y][x]
        return img.resize((self.size * SCALE, self.size * SCALE), Image.NEAREST)


def draw_sprout(c: PixelCanvas) -> None:
    """头顶绿色小芽，一丢的识别标志。"""
    # Q 版不再长芽，保留空实现以兼容旧调用。
    _ = c


def draw_ear_tufts(c: PixelCanvas, tall: bool = False) -> None:
    top = 2 if tall else 4
    for side, cx in ((-1, 20), (1, 52)):
        for i in range(4 if tall else 3):
            half = max(0, (4 if tall else 3) - i - 1)
            c.hline(cx - half, cx + half, top + i, "brown_dark")
        c.set(cx, top - 1, "outline")


def draw_body(c: PixelCanvas) -> None:
    # Q 版：身体缩得很小，只露出头部下方一小截。
    c.ellipse(36.0, 69.5, 15.0, 2.4, (59, 39, 24), SHADOW_ALPHA)
    c.ellipse(36.0, 62.0, 10.5, 7.5, "outline")
    c.sphere(36.0, 62.0, 9.5, 6.5, "brown", "brown_light", "brown_dark")
    c.ellipse(36.0, 63.5, 6.0, 4.0, "cream")


def draw_wings(c: PixelCanvas, skip_left: bool = False,
               skip_right: bool = False) -> None:
    if not skip_left:
        c.ellipse(24.0, 61.0, 3.6, 5.4, "outline")
        c.ellipse(24.0, 61.0, 2.8, 4.6, "brown_dark")
    if not skip_right:
        c.ellipse(48.0, 61.0, 3.6, 5.4, "outline")
        c.ellipse(48.0, 61.0, 2.8, 4.6, "brown_dark")


def draw_raised_wing(c: PixelCanvas, cx: float, cy: float,
                     side: int = 1) -> None:
    """举起的那只手：平举在身体两侧，不抬到耳朵边上。"""
    c.ellipse(cx, cy, 6.2, 3.6, "outline")
    c.ellipse(cx, cy, 5.2, 2.8, "brown_dark")
    # 手尖一点奶油色，和脚的颜色呼应
    c.ellipse(cx + side * 3.4, cy, 1.8, 1.8, "cream")


def draw_feet(c: PixelCanvas, lift_left: int = 0, lift_right: int = 0) -> None:
    for x0, lift in ((31, lift_left), (38, lift_right)):
        y0 = 66 - lift
        c.rect(x0, y0, x0 + 3, y0 + 2, "outline")
        c.rect(x0 + 1, y0, x0 + 2, y0 + 2, "orange")
        c.hline(x0 + 1, x0 + 2, y0 + 3, "orange_dark")


def draw_head(c: PixelCanvas) -> None:
    # Q 版：头部占满画布宽度，脸盘和眼睛同比放大。
    c.ellipse(36.0, 30.0, 33.0, 28.0, "outline")
    # 球面填充：左上亮、右下暗，给扁平的圆一点体积。
    c.sphere(36.0, 30.0, 32.0, 27.0, "brown", "brown_light", "brown_dark",
             light_dir=(-0.66, -0.75))
    # 面部奶油色圆盘：同样的三档明暗，让脸盘从头上浮起来
    c.sphere(26.0, 33.0, 15.5, 14.0, "cream", "cream_light", "cream_dark")
    c.sphere(46.0, 33.0, 15.5, 14.0, "cream", "cream_light", "cream_dark")
    # 下巴也要是奶油色，否则嘴以下会变成一整块棕色像戴了口罩
    c.sphere(36.0, 44.0, 12.0, 10.5, "cream", "cream_light", "cream_dark")
    # 顶部高光块
    c.ellipse(30.0, 9.0, 8.0, 2.6, "brown_light")


def draw_eye(c: PixelCanvas, cx: float, cy: float, mood: str,
             scale: float = 1.0) -> None:
    s = scale
    if mood in ("blink", "blink_half", "sleep"):
        c.ellipse(cx, cy, 7.2 * s, 6.8 * s, "outline")
        c.ellipse(cx, cy, 6.2 * s, 5.8 * s, "cream")
        ix, iy = int(cx), int(cy)
        n = max(2, int(round(5 * s)))
        if mood == "sleep":
            c.hline(ix - n + 1, ix + n - 1, iy + 2, "eye_black")
            c.set(ix - n, iy + 1, "eye_black")
            c.set(ix + n, iy + 1, "eye_black")
        elif mood == "blink_half":
            c.hline(ix - n, ix + n, iy + 2, "eye_black")
            c.hline(ix - n + 1, ix + n - 1, iy + 3, "eye_black")
            c.hline(ix - n - 1, ix + n + 1, iy - 1, "cream_dark")
        else:
            c.hline(ix - n, ix + n, iy + 1, "eye_black")
            c.hline(ix - n + 1, ix + n - 1, iy + 2, "eye_black")
        return

    c.ellipse(cx, cy, 7.4 * s, 7.0 * s, "outline")
    c.ellipse(cx, cy, 6.4 * s, 6.0 * s, "eye_white")

    pupil_dx, pupil_dy, pupil_r = 0.0, 0.0, 3.4 * s
    if mood == "curious":
        pupil_dy = -0.8 * s
        pupil_r = 3.9 * s
    elif mood == "think":
        pupil_dx, pupil_dy = -1.2 * s, -1.4 * s
        pupil_r = 3.0 * s
    elif mood == "listening":
        pupil_r = 3.8 * s
    elif mood == "happy":
        # 开心：弯月眼
        c.ellipse(cx, cy, 6.4 * s, 6.0 * s, "cream")
        dx = int(round(4 * s))
        dy = int(round(3 * s))
        c.line(int(cx - dx), int(cy + dy), int(cx - dx / 3), int(cy - dy),
               "eye_black")
        c.line(int(cx - dx / 3), int(cy - dy), int(cx + dx), int(cy + dy),
               "eye_black")
        c.line(int(cx - dx), int(cy + dy + 1),
               int(cx - dx / 3), int(cy - dy + 1), "eye_black")
        c.line(int(cx - dx / 3), int(cy - dy + 1),
               int(cx + dx), int(cy + dy + 1), "eye_black")
        return

    if mood == "heart":
        # 爱心眼：白眼球里一颗粉色心。
        c.ellipse(cx, cy, 7.4 * s, 7.0 * s, "outline")
        c.ellipse(cx, cy, 6.4 * s, 6.0 * s, "eye_white")
        ix, iy = int(cx), int(cy)
        heart = (
            ".##.##.",
            "#######",
            "#######",
            "#######",
            ".#####.",
            "..###..",
            "...#...",
        )
        for row, line in enumerate(heart):
            for col, ch in enumerate(line):
                if ch == "#":
                    c.set(ix - 3 + col, iy - 3 + row, (236, 112, 133))
        c.set(ix - 2, iy - 2, (255, 189, 198))
        return

    c.ellipse(cx + pupil_dx, cy + pupil_dy, pupil_r, pupil_r, "eye_black")
    c.ellipse(cx + pupil_dx - 1.2 * s, cy + pupil_dy - 1.2 * s,
              1.1 * s, 1.1 * s, "eye_blue")
    c.set(int(cx + pupil_dx + 0.5), int(cy + pupil_dy + 1.5 * s),
          "eye_white")


def draw_eyes(c: PixelCanvas, mood: str) -> None:
    if mood == "wink":
        # 眨一只眼：左眼正常，右眼闭成一条线。
        draw_eye(c, 26.0, 33.0, "idle", scale=1.55)
        draw_eye(c, 46.0, 33.0, "blink", scale=1.55)
        return
    draw_eye(c, 26.0, 33.0, mood, scale=1.55)
    draw_eye(c, 46.0, 33.0, mood, scale=1.55)


def draw_beak(c: PixelCanvas, openness: int = 0) -> None:
    """openness：0=闭喙，1=微张，2=张大（说话口型的两帧）。

    张开的喙拆成「上喙 / 口腔 / 下喙」三段，纯色块，不用渐变。
    """
    if openness >= 2:
        c.rect(34, 42, 38, 43, "orange")
        c.rect(33, 44, 39, 47, "outline")
        c.rect(34, 48, 38, 49, "orange_dark")
        return
    if openness == 1:
        c.rect(34, 42, 38, 43, "orange")
        c.rect(34, 44, 38, 45, "outline")
        c.rect(35, 46, 37, 46, "orange_dark")
        c.set(36, 42, "orange_light")
        return
    c.rect(34, 42, 38, 45, "orange_dark")
    c.rect(34, 42, 38, 43, "orange")
    c.set(36, 46, "orange_dark")


def draw_blush(c: PixelCanvas) -> None:
    c.rect(15, 41, 19, 42, "blush")
    c.rect(53, 41, 57, 42, "blush")


def draw_zzz(c: PixelCanvas) -> None:
    # 5x5 像素小 Z，避免依赖字体。
    c.rect(60, 6, 64, 6, "cream_light")
    c.set(63, 7, "cream_light")
    c.set(62, 8, "cream_light")
    c.set(61, 9, "cream_light")
    c.rect(60, 10, 64, 10, "cream_light")


def draw_sound_waves(c: PixelCanvas) -> None:
    for i, r in enumerate((9, 12)):
        for dy in range(-r, r + 1):
            x = int(round(64 + (r * r - dy * dy) ** 0.5)) if abs(dy) <= r else 0
            if i == 0:
                c.set(x, 33 + dy, "cream_dark")
        for dy in range(-r, r + 1):
            x = int(round(64 + (r * r - dy * dy) ** 0.5)) if abs(dy) <= r else 0
            if i == 1:
                c.set(x, 33 + dy, "cream_dark")


def draw_egg(c: PixelCanvas) -> None:
    c.ellipse(36.0, 63.0, 14.0, 2.4, (59, 39, 24), SHADOW_ALPHA)
    c.ellipse(36.0, 40.0, 16.5, 21.0, "outline")
    c.ellipse(36.0, 40.0, 15.5, 20.0, "cream")
    c.ellipse(31.0, 33.0, 5.0, 7.0, "cream_light")
    # 棕色斑点
    for (x, y, w, h) in ((28, 46, 3, 2), (40, 33, 3, 3), (43, 45, 2, 2),
                         (33, 52, 2, 2), (24, 38, 2, 2)):
        c.rect(x, y, x + w - 1, y + h - 1, "brown_light")
    draw_sprout(c)


# 口型档位：0=闭上，1=微张，2=张大。说话时在两帧之间轮换。
BEAK_OPENNESS = {
    "happy": 1,
    "speaking": 2,
    "speaking_soft": 1,
}


def build_state(mood: str) -> PixelCanvas:
    c = PixelCanvas()
    if mood == "egg":
        draw_egg(c)
        return c

    tall_ears = mood == "listening"
    # 动作帧：挥手举一只手、伸懒腰举两只手、跺脚左右脚交替抬起。
    hand_left = mood == "stretch"
    hand_right = mood in ("stretch", "wave")
    draw_ear_tufts(c, tall=tall_ears)
    draw_body(c)
    draw_feet(c,
              lift_left=3 if mood == "stomp_r" else 0,
              lift_right=3 if mood == "stomp_l" else 0)
    draw_wings(c, skip_left=hand_left, skip_right=hand_right)
    draw_head(c)
    if hand_left:
        draw_raised_wing(c, 15.0, 58.0, side=-1)
    if hand_right:
        draw_raised_wing(c, 57.0, 58.0, side=1)
    draw_eyes(c, mood)
    draw_beak(c, openness=BEAK_OPENNESS.get(mood, 0))
    if mood in ("happy", "heart"):
        draw_blush(c)
    if mood == "listening":
        draw_sound_waves(c)
    if mood == "sleep":
        draw_zzz(c)
    if mood == "think":
        for i, x in enumerate((53, 56, 59)):
            c.rect(x, 20 + (i % 2), x + 1, 21 + (i % 2), "cream_light")
    draw_sprout(c)
    return c


STATES = [
    ("egg", "ic_pet_egg"),
    ("idle", "ic_pet_idle"),
    ("blink_half", "ic_pet_blink_half"),
    ("blink", "ic_pet_blink"),
    ("happy", "ic_pet_happy"),
    ("curious", "ic_pet_curious"),
    ("think", "ic_pet_think"),
    ("listening", "ic_pet_listening"),
    ("speaking", "ic_pet_speaking"),
    ("speaking_soft", "ic_pet_speaking_soft"),
    ("sleep", "ic_pet_sleep"),
    ("wave", "ic_pet_wave"),
    ("stretch", "ic_pet_stretch"),
    ("stomp_l", "ic_pet_stomp_l"),
    ("stomp_r", "ic_pet_stomp_r"),
    ("heart", "ic_pet_heart"),
    ("wink", "ic_pet_wink"),
]


def make_contact_sheet(images: list[tuple[str, Image.Image]], path: str) -> None:
    cols = 3
    cell = 360
    pad = 16
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * (cell + pad) + pad,
                               rows * (cell + pad) + pad), (11, 16, 32, 255))
    for i, (name, img) in enumerate(images):
        x = pad + (i % cols) * (cell + pad)
        y = pad + (i // cols) * (cell + pad)
        sheet.alpha_composite(img, (x, y))
    sheet.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", action="store_true",
                        help="同步到 main/xingzhi-assets（供固件构建打包）")
    args = parser.parse_args()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    rendered: list[tuple[str, Image.Image]] = []
    for mood, asset in STATES:
        img = build_state(mood).to_image()
        img.save(os.path.join(OUTPUT_DIR, asset + ".png"))
        rendered.append((mood, img))

    make_contact_sheet(rendered, os.path.join(OUTPUT_DIR, "owl_contact_sheet.png"))

    if args.install:
        os.makedirs(INSTALL_DIR, exist_ok=True)
        for _, asset in STATES:
            shutil.copyfile(os.path.join(OUTPUT_DIR, asset + ".png"),
                            os.path.join(INSTALL_DIR, asset + ".png"))
        print("installed to", INSTALL_DIR)

    print("generated", len(rendered), "images in", OUTPUT_DIR)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
