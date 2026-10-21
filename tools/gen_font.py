#!/usr/bin/env python3
"""生成显示屏用的 16x16 中文点阵与 8x16 ASCII 点阵。

字符集：
  - ASCII 0x20..0x7E
  - GB2312 区 1（中文标点）、区 3（全角字符）、区 16..55（一级汉字，3755 字）

用法：
    python tools/gen_font.py

需要 Pillow 和 Windows 自带的宋体/黑体。产物 main/font_gb2312_16.c 已提交到仓库，
构建时不需要重新生成。
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "main" / "font_gb2312_16.c"

FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\simsun.ttc", 0),
    (r"C:\Windows\Fonts\simhei.ttf", 0),
    (r"C:\Windows\Fonts\msyh.ttc", 0),
]

ASCII_FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\consola.ttf", 0),
    (r"C:\Windows\Fonts\cour.ttf", 0),
]

CJK_SIZE = 16
ASCII_SIZE = 14
ASCII_WIDTH = 8
ASCII_HEIGHT = 16
ASCII_BASELINE = 13


def _load(candidates: list[tuple[str, int]], size: int) -> ImageFont.FreeTypeFont:
    for path, index in candidates:
        if Path(path).exists():
            print(f"using font: {path}")
            return ImageFont.truetype(path, size, index=index)
    raise FileNotFoundError("未找到可用的字体")


def load_fonts() -> tuple[ImageFont.FreeTypeFont, ImageFont.FreeTypeFont]:
    return _load(FONT_CANDIDATES, CJK_SIZE), _load(ASCII_FONT_CANDIDATES, ASCII_SIZE)


def render(font: ImageFont.FreeTypeFont, ch: str, width: int, height: int,
           baseline: int | None = None) -> bytes:
    """把字符渲染到 width x height 的二值位图，MSB 在左，按行打包。"""
    image = Image.new("1", (width, height), 0)
    draw = ImageDraw.Draw(image)
    x = (width - font.getlength(ch)) / 2
    if baseline is None:
        draw.text((x, 0), ch, font=font, fill=1)
    else:
        draw.text((x, baseline), ch, font=font, fill=1, anchor="ls")
    data = image.tobytes()
    stride = (width + 7) // 8
    assert len(data) == stride * height, (ch, len(data), stride * height)
    return data


def gb2312_chars() -> list[str]:
    """收集 GB2312 中的标点与一级汉字，按 Unicode 升序排列。"""
    chars = set()
    for zone in [1, 3] + list(range(16, 56)):
        for position in range(1, 95):
            try:
                ch = bytes([zone + 0xA0, position + 0xA0]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.add(ch)
    return sorted(chars, key=ord)


def fmt(data: bytes) -> str:
    return ", ".join(f"0x{b:02X}" for b in data)


def emit_ascii(font: ImageFont.FreeTypeFont) -> list[str]:
    lines = [f"const uint8_t font_ascii[FONT_ASCII_COUNT][{ASCII_HEIGHT}] = {{"]
    for code in range(0x20, 0x7F):
        ch = chr(code)
        glyph = render(font, ch, ASCII_WIDTH, ASCII_HEIGHT, baseline=ASCII_BASELINE)
        comment = ch if ch not in ('"', "\\") else " "
        lines.append(f"    {{ {fmt(glyph)} }}, /* 0x{code:02X} '{comment}' */")
    lines.append("};")
    return lines


def emit_cjk(font: ImageFont.FreeTypeFont, chars: list[str]) -> list[str]:
    lines = ["const font_glyph_t font_cjk[] = {"]
    for ch in chars:
        glyph = render(font, ch, CJK_SIZE, CJK_SIZE)
        lines.append(f"    {{ 0x{ord(ch):04X}, {{ {fmt(glyph)} }} }},")
    lines.append("};")
    return lines


def main() -> None:
    cjk_font, ascii_font = load_fonts()
    chars = gb2312_chars()

    out: list[str] = [
        "/* 自动生成，请勿手动修改。生成脚本：tools/gen_font.py */",
        "",
        '#include "display_font.h"',
        "",
    ]
    out += emit_ascii(ascii_font)
    out.append("")
    out += emit_cjk(cjk_font, chars)
    out.append("")
    out.append("const size_t font_cjk_count = sizeof(font_cjk) / sizeof(font_cjk[0]);")
    out.append("")

    OUTPUT.write_text("\n".join(out), encoding="utf-8")

    cjk_bytes = len(chars) * 32
    ascii_bytes = 95 * 16
    print(f"chars: ascii=95 cjk={len(chars)}")
    print(f"glyph bytes: ascii={ascii_bytes} cjk={cjk_bytes} total={ascii_bytes + cjk_bytes}")
    print(f"written: {OUTPUT}")


if __name__ == "__main__":
    main()
