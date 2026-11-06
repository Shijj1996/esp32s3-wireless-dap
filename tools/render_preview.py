"""在 PC 上复现 display.c 的渲染逻辑，输出 PNG 用于检查布局/字体。

不依赖硬件，纯软件：解析 font_pingfang14.c 里的位图与字形描述，
按 display.c 相同的算法（band_begin / band_str / band_flush）合成画面。
"""

import re
import sys
from pathlib import Path

from PIL import Image

W, H = 320, 240
BAND_MAX_H = 32
LINE_H = 16
BASE_LINE = 3

ROOT = Path(r"D:\ESP_Prj\wireless-dap-s3-box")
FONT_C = ROOT / "main" / "font_pingfang14.c"


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


COL_BG = rgb565(0x0B, 0x10, 0x17)
COL_HEAD = rgb565(0x12, 0x1C, 0x28)
COL_ACCENT = rgb565(0x38, 0xC8, 0xE0)
COL_TITLE = rgb565(0xCF, 0xF2, 0xFF)
COL_LABEL = rgb565(0x7E, 0x90, 0xA4)
COL_VALUE = rgb565(0xE8, 0xF0, 0xF8)
COL_OK = rgb565(0x4C, 0xE0, 0x8C)
COL_WARN = rgb565(0xFF, 0xC2, 0x4B)
COL_ERR = rgb565(0xFF, 0x6E, 0x6E)
COL_RULE = rgb565(0x1E, 0x2C, 0x3C)


def load_font():
    src = FONT_C.read_text(encoding="utf-8", errors="replace")

    # 位图数组：从声明处到字形表声明之间，全部 0x..
    bm_start = src.index("const uint8_t g_font14_bitmap[]")
    gl_start = src.index("const g_font14_glyph_t g_font14_glyph[]")
    bitmap = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", src[bm_start:gl_start])]

    glyphs = []
    for g in re.finditer(
        r"bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
        r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)",
        src[gl_start:],
    ):
        bi, aw, bw, bh, ox, oy = (int(v) for v in g.groups())
        glyphs.append(dict(bitmap_index=bi, adv_w=aw, box_w=bw, box_h=bh, ofs_x=ox, ofs_y=oy))

    need = max(g["bitmap_index"] + (g["box_w"] * g["box_h"] + 1) // 2 for g in glyphs)
    print(f"font: {len(bitmap)} bytes bitmap (need {need}), {len(glyphs)} glyphs")
    return bitmap, glyphs


BITMAP, GLYPHS = load_font()

fb = [COL_BG] * (W * H)
band = [COL_BG] * (W * BAND_MAX_H)
band_h = 16


def begin(h, bg):
    global band, band_h
    band_h = min(h, BAND_MAX_H)
    band = [bg] * (W * band_h)


def blend(bg, fg, a):
    if a == 0:
        return bg
    if a == 255:
        return fg
    br, bgc, bb = (bg >> 11) & 0x1F, (bg >> 5) & 0x3F, bg & 0x1F
    fr, fgc, fb_ = (fg >> 11) & 0x1F, (fg >> 5) & 0x3F, fg & 0x1F
    r = (fr * a + br * (255 - a)) // 255
    g = (fgc * a + bgc * (255 - a)) // 255
    b = (fb_ * a + bb * (255 - a)) // 255
    return (r << 11) | (g << 5) | b


def draw_str(x, y0, scale, text, fg):
    cx = x
    baseline = (LINE_H - BASE_LINE) * scale
    for ch in text:
        o = ord(ch)
        if o < 0x20 or o > 0x7F:
            o = ord("?")
        g = GLYPHS[(o - 0x20) + 1]
        bw, bh = g["box_w"], g["box_h"]
        if bw and bh:
            gx = cx + g["ofs_x"] * scale
            gy = baseline - g["ofs_y"] * scale - bh * scale
            for row in range(bh):
                for col in range(bw):
                    nib_idx = row * bw + col
                    byte = BITMAP[g["bitmap_index"] + nib_idx // 2]
                    nib = (byte & 0x0F) if (nib_idx & 1) else (byte >> 4)
                    if nib == 0:
                        continue
                    a = nib * 17
                    for dy in range(scale):
                        for dx in range(scale):
                            px = gx + col * scale + dx
                            py = gy + row * scale + dy + y0
                            if 0 <= px < W and 0 <= py < band_h:
                                i = py * W + px
                                band[i] = blend(band[i], fg, a)
        cx += (g["adv_w"] * scale) // 16
    return cx


def flush(y):
    for row in range(band_h):
        dy = y + row
        if 0 <= dy < H:
            fb[dy * W:(dy + 1) * W] = band[row * W:(row + 1) * W]


def row(y, label, value, status, vc=COL_VALUE, sc=COL_LABEL):
    begin(16, COL_BG)
    draw_str(8, 0, 1, label, COL_LABEL)
    if value:
        draw_str(104, 0, 1, value, vc)
    if status:
        draw_str(248, 0, 1, status, sc)
    flush(y)


def rule(y):
    begin(1, COL_RULE)
    flush(y)


# ---------------- 复现 display.c 的布局 ----------------
begin(32, COL_HEAD)
draw_str(8, 0, 2, "WIRELESS DAP", COL_TITLE)
draw_str(248, 8, 1, "BOX-3", COL_LABEL)
flush(0)

begin(2, COL_ACCENT)
flush(32)

row(36, "WiFi", "ESP32-DAP", "OK", COL_VALUE, COL_OK)
row(52, "Link", "ch 1  -40 dBm", "connected", COL_VALUE, COL_OK)
row(68, "IPv4", "192.168.137.50", ":3240", COL_VALUE, COL_LABEL)
rule(84)
row(88, "USBIP", "listening", "no client", COL_VALUE, COL_LABEL)
row(104, "DAP", "OFF", "norm", COL_LABEL, COL_LABEL)
row(120, "Socket", "55", None)
rule(136)
row(140, "Uptime", "00:12:34", None)
row(156, "Heap", "170 / 194 KB", None)
row(172, "MAC", "74:4D:BD:2E:88:50", None, COL_LABEL, COL_LABEL)
row(188, "SDK", "ESP-IDF v5.5.4", None, COL_LABEL, COL_LABEL)

begin(32, COL_OK)
draw_str(16, 0, 2, "DAP ACTIVE", COL_BG)
flush(208)
# ---------------- 输出 ----------------
out = Image.new("RGB", (W, H))
px = out.load()
for y in range(H):
    for x in range(W):
        v = fb[y * W + x]
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        px[x, y] = (r, g, b)

dst = ROOT / "docs" / "display_preview.png"
out.save(dst)
print(f"saved {dst}")
out.resize((W * 2, H * 2), Image.NEAREST).save(ROOT / "docs" / "display_preview_2x.png")
print("saved 2x preview")
