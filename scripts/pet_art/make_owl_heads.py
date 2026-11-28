#!/usr/bin/env python3
"""生成宠物页弹窗用的 6 个彩色猫头鹰头像，以及右上角蛋按钮的小图。

头像只保留头部（耳羽 + 脸盘 + 眼睛 + 喙），每个一种主色，用来区分不同应用入口。
画法与 generate_owl.py 完全一致：72x72 逻辑画布，纯色像素块，2 倍最近邻放大后
按内容裁切，输出到 main/xingzhi-assets/ic_pet_head_*.png 与 ic_pet_egg_icon.png。
"""

from __future__ import annotations

import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from generate_owl import PixelCanvas, draw_beak, draw_eyes  # noqa: E402

ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "main", "xingzhi-assets"))
LOGICAL_SCALE = 1

# 文件名 -> (主色, 中文名用途)
HEADS = {
    "ic_pet_head_word": (0x4A, 0x6F, 0xA5),     # 单词丢：灰蓝
    "ic_pet_head_oral": (0x7A, 0x6B, 0xA6),     # 口语丢：灰紫
    "ic_pet_head_math": (0xB0, 0x8A, 0x4E),     # 数学丢：暗金
    "ic_pet_head_podcast": (0x4E, 0x86, 0x67),  # 播客丢：暗绿
    "ic_pet_head_plan": (0xA3, 0x6B, 0x7C),     # 计划丢：暗玫瑰
    "ic_pet_head_task": (0x4E, 0x84, 0x8C),     # 任务丢：暗青
}


def darker(rgb: tuple[int, int, int], k: float = 0.72) -> tuple[int, int, int]:
    return tuple(int(v * k) for v in rgb)


def lighter(rgb: tuple[int, int, int], k: float = 1.35) -> tuple[int, int, int]:
    return tuple(min(255, int(v * k)) for v in rgb)


def draw_ears(c: PixelCanvas, color: tuple[int, int, int]) -> None:
    top = 2
    for cx in (20, 52):
        for i in range(4):
            half = max(0, 4 - i - 1)
            c.hline(cx - half, cx + half, top + i, color)
        c.set(cx, top - 1, "outline")


def draw_head(c: PixelCanvas, fill: tuple[int, int, int]) -> None:
    c.ellipse(36.0, 30.0, 33.0, 28.0, "outline")
    # 和小猫头鹰本体同一套三档明暗，体积感一致。
    c.sphere(36.0, 30.0, 32.0, 27.0, fill, lighter(fill, 1.2),
             darker(fill, 0.72), light_dir=(-0.66, -0.75))
    # 奶油色脸盘
    c.sphere(26.0, 33.0, 15.5, 14.0, "cream", "cream_light", "cream_dark")
    c.sphere(46.0, 33.0, 15.5, 14.0, "cream", "cream_light", "cream_dark")
    c.sphere(36.0, 44.0, 12.0, 10.5, "cream", "cream_light", "cream_dark")
    c.ellipse(30.0, 9.0, 8.0, 2.6, lighter(fill, 1.35))


def draw_body(c: PixelCanvas, fill: tuple[int, int, int]) -> None:
    """和外层大猫头鹰同一套比例：头占绝大部分，身体/腿只露一小截。"""
    c.ellipse(36.0, 69.5, 15.0, 2.4, (59, 39, 24), 190)  # 底部投影
    # 左右翅膀（比身体深一档）
    wing = darker(fill, 0.55)
    for cx in (24.0, 48.0):
        c.ellipse(cx, 61.0, 3.6, 5.4, "outline")
        c.ellipse(cx, 61.0, 2.8, 4.6, wing)
    # 短短的身体
    c.ellipse(36.0, 62.0, 10.5, 7.5, "outline")
    c.sphere(36.0, 62.0, 9.5, 6.5, fill, lighter(fill, 1.2), darker(fill, 0.72))
    c.ellipse(36.0, 63.5, 6.0, 4.0, "cream")
    # 小脚
    for x0 in (31, 38):
        c.rect(x0, 66, x0 + 3, 68, "outline")
        c.rect(x0 + 1, 66, x0 + 2, 68, "orange")
        c.hline(x0 + 1, x0 + 2, 69, "orange_dark")


def canvas_to_square_png(c: PixelCanvas, path: str, block: int = 1) -> None:
    small = c.to_image().resize(
        (c.size * LOGICAL_SCALE, c.size * LOGICAL_SCALE), Image.NEAREST)
    bbox = small.split()[-1].getbbox()
    if bbox is None:
        raise ValueError("empty canvas for " + path)
    cropped = small.crop(bbox)
    side = max(cropped.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(
        cropped, ((side - cropped.width) // 2, (side - cropped.height) // 2))
    if block > 1:
        # 降采样再最近邻放大：把像素块放到约 block 倍，和外面的大猫头鹰同一粗细。
        tiny = square.resize((max(1, side // block), max(1, side // block)),
                             Image.NEAREST)
        square = tiny.resize((side, side), Image.NEAREST)
    square.save(path)


def build_head(rgb: tuple[int, int, int]) -> PixelCanvas:
    c = PixelCanvas()
    draw_ears(c, darker(rgb))
    draw_body(c, rgb)
    draw_head(c, rgb)
    draw_eyes(c, "idle")
    draw_beak(c, 0)
    return c


def main() -> int:
    for name, rgb in HEADS.items():
        canvas_to_square_png(build_head(rgb),
                             os.path.join(ASSETS, name + ".png"), block=2)
        print("wrote", name + ".png")

    # 右上角蛋按钮：只要那颗蛋，不要整张 360 画布。
    egg = PixelCanvas()
    egg.ellipse(36.0, 63.0, 14.0, 2.4, (59, 39, 24), 190)
    egg.ellipse(36.0, 40.0, 16.5, 21.0, "outline")
    egg.ellipse(36.0, 40.0, 15.5, 20.0, "cream")
    egg.ellipse(31.0, 33.0, 5.0, 7.0, "cream_light")
    for (x, y, w, h) in ((28, 46, 3, 2), (40, 33, 3, 3), (43, 45, 2, 2),
                         (33, 52, 2, 2), (24, 38, 2, 2)):
        egg.rect(x, y, x + w - 1, y + h - 1, "brown_light")
    canvas_to_square_png(egg, os.path.join(ASSETS, "ic_pet_egg_icon.png"))
    print("wrote ic_pet_egg_icon.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
