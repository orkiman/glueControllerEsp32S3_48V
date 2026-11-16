#!/usr/bin/env python3
"""Generates docs/cabinet/wiring-diagram.svg (cabinet wiring, draft)."""
import os

W, H = 1500, 900
C48, C24, C0, CSIG, CGUN, CAC, CPE = ("#d62828", "#e07b00", "#1d4ed8", "#15803d",
                                      "#7c3aed", "#444", "#5a9a00")
o = []


def line(pts, col, w=2.5, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    p = " ".join(f"{x},{y}" for x, y in pts)
    o.append(f'<polyline points="{p}" fill="none" stroke="{col}" stroke-width="{w}"{d}/>')


def dot(x, y, col):
    o.append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{col}"/>')


def box(x, y, w, h, title, sub=None, fill="#f4f4f5", dash=None):
    d = ' stroke-dasharray="6 4"' if dash else ""
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{fill}" '
             f'stroke="#222" stroke-width="1.5"{d}/>')
    o.append(f'<text x="{x + w / 2}" y="{y + 18}" text-anchor="middle" class="b">{title}</text>')
    if sub:
        for i, s in enumerate(sub if isinstance(sub, list) else [sub]):
            o.append(f'<text x="{x + w / 2}" y="{y + 35 + i * 15}" text-anchor="middle" class="s">{s}</text>')


def text(x, y, s, cls="t", anchor="start", col=None):
    f = f' fill="{col}"' if col else ""
    o.append(f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}"{f}>{s}</text>')


def pin(x, y, label, side):
    """Terminal pin on a box edge; label is written inside the box."""
    o.append(f'<circle cx="{x}" cy="{y}" r="4" fill="#fff" stroke="#222" stroke-width="1.5"/>')
    if side == "l":      # pin on left edge, text to the right
        text(x + 9, y + 4, label, "s")
    else:
        text(x - 9, y + 4, label, "s", "end")


def fuse(x, y, name, val, col):
    o.append(f'<rect x="{x}" y="{y - 9}" width="46" height="18" rx="3" fill="#fff" stroke="{col}" stroke-width="2"/>')
    text(x + 23, y - 15, name, "b", "middle")
    text(x + 23, y + 26, val, "s", "middle")


def diode(x, y_k, y_a, name):
    """Vertical diode, cathode at y_k (top), anode at y_a (bottom)."""
    m = (y_k + y_a) / 2
    line([(x, y_k), (x, m - 8)], "#222", 2)
    line([(x, m + 8), (x, y_a)], "#222", 2)
    o.append(f'<polygon points="{x - 9},{m + 8} {x + 9},{m + 8} {x},{m - 8}" fill="#222"/>')
    line([(x - 9, m - 8), (x + 9, m - 8)], "#222", 2.5)
    text(x + 14, m + 4, name, "s")


o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
         'font-family="Arial, Helvetica, sans-serif">')
o.append('<style>.t{font-size:13px;fill:#222}.s{font-size:12px;fill:#333}'
         '.b{font-size:13px;font-weight:bold;fill:#111}.h{font-size:20px;font-weight:bold;fill:#111}'
         '.n{font-size:12px;fill:#555;font-style:italic}</style>')
o.append(f'<rect width="{W}" height="{H}" fill="#fff"/>')
text(30, 34, "Glue controller cabinet - wiring diagram (DRAFT 2)", "h")
text(30, 54, "Fuse values and supply models are provisional. Dashed = optional / not decided.", "n")

# ---------------------------------------------------------------- mains
box(30, 130, 150, 120, "Mains inlet", ["IEC C14", "+ switch", "+ fuse F0 2 A T", "230 V AC"])
line([(180, 190), (215, 190)], CAC, 5)
line([(215, 110), (215, 260)], CAC, 5)
line([(215, 110), (250, 110)], CAC, 5)
line([(215, 260), (250, 260)], CAC, 5)
dot(215, 190, CAC)
text(110, 268, "L, N, PE", "s", "middle")

# ---------------------------------------------------------------- supplies
box(250, 60, 170, 100, "PSU-B  24 V", ["upper stage", "(makes the 48 V)"])
box(250, 210, 170, 100, "PSU-A  24 V", ["lower stage", "(logic + sensors)"])
pin(420, 85, "V+", "r");  pin(420, 135, "V-", "r")
pin(420, 235, "V+", "r"); pin(420, 285, "V-", "r")

# series link PSU-B V-  ->  PSU-A V+
line([(420, 135), (480, 135)], C24)
line([(450, 135), (450, 235)], C24)
dot(450, 135, C24); dot(450, 235, C24)
text(456, 190, "series", "n"); text(456, 204, "link", "n")
diode(480, 85, 135, "D-B");  dot(480, 85, C48)
diode(480, 235, 285, "D-A"); dot(480, 235, C24); dot(480, 285, C0)

# +48 V : PSU-B V+ -> F1 -> X1 (E-stop link) -> CN1-1
line([(420, 85), (520, 85)], C48)
fuse(520, 85, "F1", "4 A T", C48)
line([(566, 85), (665, 85), (665, 370), (700, 370)], C48)
text(672, 200, "+48 V", "b", col=C48)

# +24 V : PSU-A V+ -> F2 -> CN6-1 ;  -> F3 -> sensor rail
line([(420, 235), (740, 235)], C24)
fuse(740, 235, "F2", "1 A T", C24)
line([(786, 235), (1065, 235)], C24)
dot(1030, 235, C24)
line([(1030, 235), (1030, 370), (960, 370)], C24)
fuse(1065, 235, "F3", "0.5 A", C24)
line([(1111, 235), (1200, 235), (1200, 640)], C24)
text(560, 228, "+24 V", "b", col=C24)
text(1210, 228, "+24 V sensors", "b", col=C24)

# 0 V
line([(420, 285), (1225, 285), (1225, 640)], C0)
dot(640, 285, C0)
line([(640, 285), (640, 400), (700, 400)], C0)
dot(1010, 285, C0)
line([(1010, 285), (1010, 400), (960, 400)], C0)
text(570, 302, "0 V", "b", col=C0)
text(1232, 302, "0 V", "b", col=C0)
# optional PE bond
line([(520, 285), (520, 340)], CPE, 2.5, "6 4")
text(512, 356, "PE bond, one point", "n", "end")
text(512, 372, "(to decide)", "n", "end")

# ---------------------------------------------------------------- board
box(700, 330, 260, 440, "Controller board", fill="#ecfdf5")
text(830, 366, "ESP32-S3 DevKitC", "s", "middle")
text(830, 381, "on J1 / J3", "s", "middle")
left = [("CN1", 370, "1  +48 V in", "2  GND"), ("CN2", 470, "1  Gun 1", "2  Gun 1"),
        ("CN3", 550, "1  Gun 2", "2  Gun 2"), ("CN4", 630, "1  Gun 3", "2  Gun 3"),
        ("CN5", 710, "1  Gun 4", "2  Gun 4")]
for name, y, a, b in left:
    text(708, y - 12, name, "b")
    pin(700, y, a, "l"); pin(700, y + 30, b, "l")
text(952, 358, "CN6", "b", "end")
pin(960, 370, "+24 V in  1", "r"); pin(960, 400, "GND  2", "r")
text(952, 458, "CN7  fast encoder in", "b", "end")
pin(960, 470, "ENC +  1", "r"); pin(960, 500, "ENC -  2", "r")
text(952, 558, "CN9  24 V inputs", "b", "end")
for i, lab in enumerate(["IN1 eye  1", "IN2 spare  2", "IN3 spare  3", "IN4 (24 V enc. alt.)  4"]):
    pin(960, 570 + 30 * i, lab, "r")
for i in (1, 2, 3):
    line([(960, 570 + 30 * i), (985, 570 + 30 * i)], CSIG, 2, "5 4")

# USB
box(770, 800, 120, 46, "USB panel socket", "to the PC")
line([(830, 770), (830, 800)], "#666", 3)
text(838, 790, "USB cable to the DevKit", "n")

# ---------------------------------------------------------------- guns
for i in range(4):
    y = 470 + 80 * i
    box(60, y - 28, 170, 72, f"GUN {i + 1}", fill="#f5f3ff")
    text(145, y + 6, "M16 5-pole socket", "s", "middle")
    pin(230, y, "1", "r"); pin(230, y + 30, "2", "r")
    line([(230, y), (700, y)], CGUN)
    line([(230, y + 30), (700, y + 30)], CGUN)
text(60, 786, "Gun sockets: pin 1 = Gun +, pin 2 = Gun -  (same as the C-420).", "n")
text(60, 802, "Pins 3, 4, 5 not wired (no purge buttons on this machine).", "n")
text(60, 818, "Keep gun wires away from eye / encoder wires.", "n")

# ---------------------------------------------------------------- encoder
box(1280, 385, 180, 105, "ENC", fill="#fefce8")
text(1400, 445, "5-pole", "s", "middle")
pin(1280, 420, "1  +V", "l"); pin(1280, 445, "2  0 V", "l"); pin(1280, 470, "3  signal", "l")
line([(1280, 420), (1200, 420)], C24); dot(1200, 420, C24)
line([(1280, 445), (1225, 445)], C0);  dot(1225, 445, C0)
# series resistor with bypass jumper
box(1040, 440, 120, 44, "R1 1.5 k / 1 W", "JP1 bypasses R1", fill="#fff")
line([(1280, 470), (1160, 470)], CSIG)
line([(1040, 470), (960, 470)], CSIG)
line([(960, 500), (1225, 500)], C0); dot(1225, 500, C0)
text(1040, 520, "24 V encoder: JP1 open.", "n")
text(1040, 536, "5 V encoder: JP1 closed", "n")

# ---------------------------------------------------------------- eye
box(1280, 545, 180, 105, "EYE (START 1)", fill="#fefce8")
text(1400, 605, "3-pole", "s", "middle")
pin(1280, 580, "1  +24 V", "l"); pin(1280, 605, "2  0 V", "l"); pin(1280, 630, "3  signal", "l")
line([(1280, 580), (1200, 580)], C24); dot(1200, 580, C24)
line([(1280, 605), (1225, 605)], C0);  dot(1225, 605, C0)
line([(1280, 630), (1240, 630), (1240, 670), (1000, 670), (1000, 570), (960, 570)], CSIG)

text(1010, 700, "5 V encoder (future): feed ENC pin 1 from a small", "n")
text(1010, 716, "24 V to 5 V converter instead of the +24 V rail.", "n")
text(1010, 740, "Sensors must be PNP (switch +24 V to the input).", "n")

# ---------------------------------------------------------------- legend
lx, ly = 1010, 790
for i, (c, s) in enumerate([(C48, "+48 V (guns)"), (C24, "+24 V"), (C0, "0 V (common)"),
                            (CSIG, "signal"), (CGUN, "gun coil"), (CAC, "mains 230 V AC")]):
    x = lx + (i % 3) * 160
    y = ly + (i // 3) * 24
    line([(x, y), (x + 34, y)], c, 4 if c != CAC else 5)
    text(x + 42, y + 4, s, "s")
text(1010, 850, "D-A, D-B: reverse-protection diode across each supply output,", "n")
text(1010, 866, "cathode on V+, rated at least the supply current.", "n")

o.append("</svg>")
out = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "cabinet", "wiring-diagram.svg")
open(out, "w").write("\n".join(o) + "\n")
