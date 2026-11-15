"""Generates the figure blocks for coffee.qlm (geometry is easier in Python)."""
import math

def flow():
    out = ["@figure 470 104"]
    steps = [("التحميص", "على نار هادئة"), ("الطحن", "في المهباش"), ("الغلي", "في الدلة"),
             ("الهيل", "يُضاف قبل التقديم"), ("التقديم", "في الفناجين")]
    w, gap = 74, 25
    for i, (a, b) in enumerate(steps):
        x = 470 - w - i * (w + gap)
        fill = "#e3efef" if i % 2 == 0 else "#f3ead7"
        out.append(f'rect {x} 22 {w} 46 radius=9 fill={fill} stroke=accent width=1.2')
        out.append(f'text {x + w/2:.1f} 50 "{a}" font=sansbold size=12 color=accent')
        out.append(f'text {x + w/2:.1f} 88 "{b}" size=8.5 color=grey')
        out.append(f'circle {x + w - 2} 22 9 fill=#c99a3e')
        out.append(f'text {x + w - 2} 25.5 "{"١٢٣٤٥"[i]}" font=sansbold size=9 color=white')
        if i < len(steps) - 1:
            out.append(f'line {x - 4} 45 {x - gap + 4} 45 arrow=end stroke=accent width=1.4')
    out.append("@end")
    return "\n".join(out)

def roast():
    out = ["@figure 470 150"]
    beans = [("فاتح", "#c9a36c"), ("متوسط", "#9b6a3c"), ("داكن", "#6b4226"), ("داكن جداً", "#3a2316")]
    xs = [405, 290, 175, 60]
    for (name, col), cx in zip(beans, xs):
        out.append(f'ellipse {cx} 62 24 34 fill={col} stroke=#2b1a10 width=0.8')
        out.append(f'path "M {cx} 30 C {cx-9} 48, {cx+9} 74, {cx} 94" stroke=#f1e4cf width=1.6')
        out.append(f'text {cx} 122 "{name}" font=sansbold size=11')
    out.append('text 405 14 "الشائع في القهوة الخليجية" size=9 color=accent')
    out.append('line 455 140 15 140 arrow=end stroke=grey width=1')
    out.append("@end")
    return "\n".join(out)

def majlis():
    out = ["@figure 330 250"]
    cx, cy, R = 165, 112, 92
    out.append(f'circle {cx} {cy} {R+22} fill=#f6f1e6 stroke=#e3d6bb width=1')
    out.append(f'text {cx} {cy+4} "المجلس" font=heading size=13 color=#b9a47a')
    # host at the bottom (angle 90deg in screen coords), guests around
    n = 8
    pts = []
    for k in range(n):
        ang = math.radians(90 - k * 360 / n)   # k=0 bottom, going up the right side
        pts.append((cx + R * math.cos(ang), cy + R * math.sin(ang)))
    hx, hy = pts[0]
    out.append(f'circle {hx:.1f} {hy:.1f} 15 fill=accent')
    out.append(f'text {hx:.1f} {hy+4:.1f} "م" font=sansbold size=11 color=white')
    out.append(f'text {hx:.1f} {hy+31:.1f} "المُضيف" font=sansbold size=10 color=accent')
    for k in range(1, n):
        x, y = pts[k]
        out.append(f'circle {x:.1f} {y:.1f} 13 fill=white stroke=#c99a3e width=1.5')
        out.append(f'text {x:.1f} {y+4:.1f} "{"١٢٣٤٥٦٧"[k-1]}" font=sansbold size=10 color=#8a6420')
    # serving order arrows along the inside of the ring
    for k in range(0, n - 1):
        a0 = math.radians(90 - k * 360 / n - 8)
        a1 = math.radians(90 - (k + 1) * 360 / n + 8)
        r = R - 30
        x0, y0 = cx + r * math.cos(a0), cy + r * math.sin(a0)
        x1, y1 = cx + r * math.cos(a1), cy + r * math.sin(a1)
        out.append(f'line {x0:.1f} {y0:.1f} {x1:.1f} {y1:.1f} arrow=end stroke=accent width=1.1')
    x1, y1 = pts[1]
    out.append(f'text {x1+26:.1f} {y1+30:.1f} "يبدأ من اليمين" size=9 color=grey anchor=left')
    out.append("@end")
    return "\n".join(out)

def dallah():
    return """@figure 330 230
ellipse 165 212 150 13 fill=#efe6d3
% handle
path "M 168 92 C 205 92, 214 150, 190 176" stroke=#8a6a1f width=6
path "M 168 92 C 205 92, 214 150, 190 176" stroke=#d4ae55 width=3
% spout
path "M 133 114 C 110 110, 82 98, 52 68 L 47 73 C 74 103, 104 122, 130 128 Z" fill=#d4ae55 stroke=#8a6a1f width=1
% body
path "M 98 202 C 86 162, 124 142, 130 122 L 170 122 C 176 142, 214 162, 202 202 Z" fill=#d4ae55 stroke=#8a6a1f width=1.2
path "M 104 188 C 140 196, 160 196, 196 188" stroke=#8a6a1f width=1
path "M 112 170 C 140 176, 160 176, 188 170" stroke=#f3dc9a width=1.4
rect 94 199 112 9 radius=3 fill=#b8913a stroke=#8a6a1f width=1
% neck
path "M 130 122 C 125 106, 134 96, 136 84 L 164 84 C 166 96, 175 106, 170 122 Z" fill=#dcb85f stroke=#8a6a1f width=1.2
rect 132 79 36 7 radius=2 fill=#b8913a stroke=#8a6a1f width=1
% lid
path "M 134 79 C 138 62, 146 54, 150 44 C 154 54, 162 62, 166 79 Z" fill=#dcb85f stroke=#8a6a1f width=1.2
circle 150 40 5.5 fill=#b8913a stroke=#8a6a1f width=1
% cups
path "M 236 178 L 268 178 C 266 198, 261 206, 252 206 C 243 206, 238 198, 236 178 Z" fill=white stroke=#9a9a9a width=1
ellipse 252 179 16 3 fill=#b98b4e
path "M 278 186 L 306 186 C 304 202, 300 209, 292 209 C 284 209, 280 202, 278 186 Z" fill=white stroke=#9a9a9a width=1
ellipse 292 187 14 2.6 fill=#b98b4e
path "M 248 168 C 242 158, 256 152, 249 140" stroke=#b5b5b5 width=1.2
path "M 257 170 C 251 160, 265 154, 258 142" stroke=#c8c8c8 width=1
% cardamom and dates
ellipse 46 206 7 4 fill=#7da35a stroke=#557a3a width=0.8
ellipse 60 211 7 4 fill=#88ad63 stroke=#557a3a width=0.8
ellipse 38 214 7 4 fill=#7da35a stroke=#557a3a width=0.8
ellipse 222 208 9 5 fill=#6b3a1e stroke=#4a2410 width=0.8
ellipse 212 215 9 5 fill=#7a4524 stroke=#4a2410 width=0.8
@end"""

if __name__ == "__main__":
    import sys
    src = open("coffee.src.qlm", encoding="utf-8").read()
    for name, fn in [("dallah", dallah), ("flow", flow), ("roast", roast), ("majlis", majlis)]:
        src = src.replace("{{" + name + "}}", fn())
    open("coffee.qlm", "w", encoding="utf-8").write(src)
