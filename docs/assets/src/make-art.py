#!/usr/bin/env python3
"""Writes docs/assets/{banner-light,banner-dark,logo}.svg: flat and retro
brutalist. The ASCII-art mark puts every character in its own grid cell, so
it lines up in any SVG renderer and font; the words are ordinary text.

usage: python3 docs/assets/src/make-art.py
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent
MONO = ("'JetBrains Mono', 'SF Mono', ui-monospace, Menlo, Consolas, "
        "'DejaVu Sans Mono', 'Courier New', monospace")

# The mark: the game's native code lifted out of its chip.
MARK = ["        /\\        ",
        "       /  \\       ",
        "      /_  _\\      ",
        "        ||        ",
        "   +----||----+   ",
        " --|    ||    |-- ",
        " --|    ''    |-- ",
        " --|          |-- ",
        "   +----------+   ",
        "      |    |      "]
assert all(len(r) == 18 for r in MARK)

INK, YELLOW = '#111111', '#FFD400'
THEMES = {
    'light': dict(bg='#E4DFD1', paper='#F4F1E8', ink=INK, mute='#3A3731'),
    'dark': dict(bg='#000000', paper='#141414', ink='#F4F1E8', mute='#CFCABC'),
}


def esc(c):
    return {'&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;'}.get(c, c)


def grid(lines, x, y, cw, lh, fill, size, center=False):
    """ASCII art at (x, baseline y): one <text> per non-space character."""
    if center:
        x -= max(map(len, lines)) * cw / 2
    out = [f'<g fill="{fill}" font-size="{size}" font-weight="700" text-anchor="middle">']
    for r, line in enumerate(lines):
        for c, ch in enumerate(line):
            if ch != ' ':
                out.append(f'<text x="{x + (c + .5) * cw:.1f}" y="{y + r * lh:.1f}">{esc(ch)}</text>')
    out.append('</g>')
    return "\n    ".join(out)


def text(x, y, size, fill, content, anchor="start", spacing=0):
    """Plain text: labels read best as ordinary runs, not cell by cell."""
    return (f'<text x="{x}" y="{y}" font-size="{size}" font-weight="700" fill="{fill}" '
            f'text-anchor="{anchor}" letter-spacing="{spacing}">{content}</text>')


def banner(t):
    s = [f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 400" width="1280" height="400" role="img" aria-labelledby="t d">
  <title id="t">droidlift</title>
  <desc id="d">droidlift: native Android games on macOS and iPhone. CPU backends: dynarmic, an ARMv7 interpreter, and an ahead-of-time ARMv7 to C recompiler.</desc>
  <rect width="1280" height="400" fill="{t['bg']}"/>
  <g font-family="{MONO}">
  <rect x="19" y="19" width="1242" height="362" fill="{t['paper']}" stroke="{t['ink']}" stroke-width="6"/>
  <rect x="16" y="16" width="1248" height="50" fill="{t['ink']}"/>''']
    s.append(text(42, 49, 20, t['paper'], "DROIDLIFT.SYS :: ARMEABI-V7A -&gt; ARM64"))
    s.append(text(1238, 49, 20, t['paper'], "[_] [#] [X]", anchor="end"))
    # the name: big, heavy, with a hard shadow
    s.append(text(66, 192, 118, YELLOW, "droidlift", spacing=-4))
    s.append(text(58, 184, 118, t['ink'], "droidlift", spacing=-4))
    s.append(text(62, 244, 27, t['ink'], "NATIVE ANDROID GAMES ON MACOS + IPHONE"))
    x = 62
    for label, hot in [("DYNARMIC", False), ("INTERPRETER", False), ("ARMV7 -&gt; C RECOMP", True)]:
        w = len(label.replace("&gt;", ">")) * 13.2 + 40
        s.append(f'''  <rect x="{x + 6:.1f}" y="274" width="{w:.1f}" height="44" fill="{t['ink']}"/>
  <rect x="{x:.1f}" y="268" width="{w:.1f}" height="44" fill="{YELLOW if hot else t['paper']}" stroke="{t['ink']}" stroke-width="4"/>''')
        s.append(text(f"{x + w / 2:.1f}", 297, 21, INK if hot else t['ink'], label, anchor="middle"))
        x += w + 22
    s.append(f'''  <line x1="19" y1="338" x2="1261" y2="338" stroke="{t['ink']}" stroke-width="4"/>''')
    s.append(text(42, 368, 20, t['mute'], "$ droidlift --apk game.apk --cpu recomp --window_"))
    bx, by, bw, bh = 950, 86, 276, 228
    s.append(f'''  <rect x="{bx + 12}" y="{by + 12}" width="{bw}" height="{bh}" fill="{t['ink']}"/>
  <rect x="{bx}" y="{by}" width="{bw}" height="{bh}" fill="{YELLOW}" stroke="{t['ink']}" stroke-width="6"/>''')
    s.append(grid(MARK, bx + (bw - 18 * 14.0) / 2, by + (bh - 10 * 20.0) / 2 + 15, 14.0, 20.0, INK, 25))
    s.append('  </g>\n</svg>\n')
    return "\n".join(s)


def logo():
    bx, by, bw, bh, cw, lh = 40, 40, 412, 412, 22.0, 36.0
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" role="img" aria-labelledby="t">
  <title id="t">droidlift</title>
  <rect width="512" height="512" fill="#E4DFD1"/>
  <rect x="{bx + 20}" y="{by + 20}" width="{bw}" height="{bh}" fill="{INK}"/>
  <rect x="{bx}" y="{by}" width="{bw}" height="{bh}" fill="{YELLOW}" stroke="{INK}" stroke-width="12"/>
  <g font-family="{MONO}">
    {grid(MARK, bx + (bw - 18 * cw) / 2, by + (bh - 10 * lh) / 2 + 28, cw, lh, INK, 38)}
  </g>
</svg>
'''


for name, theme in THEMES.items():
    (OUT / f"banner-{name}.svg").write_text(banner(theme))
(OUT / "logo.svg").write_text(logo())
