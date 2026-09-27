#!/usr/bin/env python3
"""Render an oblast (several villages) as one side-by-side time-lapse GIF.

usage: oblast2gif.py OUT.gif --pane NAME=GLOB [--pane ...] [--caption HH:MM=TEXT ...]
                     [--final FILE] [--speed N] [--fps F] [--cols C] [--rows R] [--size PX]

Each pane stitches its asciicast recordings (tools/drive.py --cast) in start
order on one shared wall clock, so reboots and joins line up across panes.
Thinking is hidden: dim lines are kept only when they are tool activity
(the harness prints tool calls dim too). Captions appear at their local
wall-clock time; --final is a text file shown over the last frames.
"""
import glob, json, os, re, shutil, subprocess, sys, tempfile, time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cast2gif import Screen, BG, FG, DIM  # noqa: E402
from PIL import Image, ImageDraw, ImageFont

TOOL_LINE = re.compile(r"^\s{2,}(write|compile|spawn|read|stat|wait|snapshot|fetch|\(|exit=|wrote|error|pid=|"
                       r"still running|timeout|escape|rolled|killed|cached)")
HEAD_BG, CAP_BG, ACCENT = (40, 40, 48), (32, 30, 22), (236, 200, 120)


def filter_stream(events):
    """(t, text) chunks -> (t, text) with thinking removed, tool lines kept."""
    dim, line, out, carry = False, "", [], ""
    for t, data in events:
        # An escape sequence can be split across chunks: hold a trailing
        # partial one until the next chunk completes it.
        data = carry + data
        m = re.search(r"\x1b(\[[0-9;]*)?$", data)
        carry = data[m.start():] if m else ""
        if m: data = data[:m.start()]
        emit = []
        i = 0
        while i < len(data):
            m = re.match(r"\x1b\[([0-9;]*)m", data[i:])
            if m:
                codes = m.group(1).split(";")
                if "2" in codes:
                    dim = True
                elif "0" in codes or m.group(1) == "":
                    if dim and line and TOOL_LINE.match(line):
                        emit.append("\x1b[2m" + line + "\x1b[0m")
                    line = ""
                    dim = False
                i += m.end()
                continue
            c = data[i]
            if dim:
                if c == "\n":
                    if TOOL_LINE.match(line): emit.append("\x1b[2m" + line + "\x1b[0m\r\n")
                    line = ""
                elif c != "\r":
                    line += c
            else:
                emit.append(c)
            i += 1
        if emit: out.append((t, "".join(emit)))
    return out


def load_pane(pattern):
    casts = []
    for f in glob.glob(pattern):
        with open(f) as fh:
            head = json.loads(fh.readline())
            ev = [json.loads(l) for l in fh if l.strip()]
        casts.append((head["timestamp"], head["width"], head["height"], ev))
    casts.sort(key=lambda c: c[0])
    events = []
    for t0, _, _, ev in casts:
        events += [(t0 + e[0], e[2]) for e in ev if e[1] == "o"]
    return filter_stream(events)


def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    out, panes, caps = a[0], [], []
    opt = {"--speed": 300.0, "--fps": 5.0, "--cols": 80, "--rows": 30, "--size": 11, "--final": "",
           "--font": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "--hold": 6.0}
    i = 1
    while i < len(a):
        k, v = a[i], a[i + 1]
        if k == "--pane":
            name, pat = v.split("=", 1); panes.append((name, load_pane(pat)))
        elif k == "--caption":
            hm, text = v.split("=", 1); caps.append((hm, text))
        else:
            opt[k] = type(opt[k])(v)
        i += 2
    cols, rows = opt["--cols"], opt["--rows"]
    font = ImageFont.truetype(opt["--font"], opt["--size"])
    big = ImageFont.truetype(opt["--font"], opt["--size"] + 3)
    cw, ch = round(font.getlength("M")), round(opt["--size"] * 1.3)
    t_start = min(p[1][0][0] for p in panes if p[1])
    t_end = max(p[1][-1][0] for p in panes if p[1])
    day = datetime.fromtimestamp(t_start).date()
    caps = sorted((datetime.combine(day, datetime.strptime(hm, "%H:%M").time()).timestamp(), txt) for hm, txt in caps)

    screens = [Screen(cols, rows) for _ in panes]
    idx = [0] * len(panes)
    pw, ph, head, pad = cols * cw + 16, rows * ch + 12, ch + 10, 8
    W, H = len(panes) * pw + (len(panes) + 1) * pad, head + ph + 2 * ch + 3 * pad
    step = opt["--speed"] / opt["--fps"]  # wall seconds per frame
    tmp = tempfile.mkdtemp(prefix="oblast-")
    frames, last_key, clock, n = [], None, t_start, 0
    final = open(opt["--final"]).read().rstrip("\n").split("\n") if opt["--final"] else []
    try:
        while clock <= t_end + step:
            for p, (_, ev) in enumerate(panes):
                while idx[p] < len(ev) and ev[idx[p]][0] <= clock:
                    screens[p].feed(ev[idx[p]][1]); idx[p] += 1
            cap = next((txt for ts, txt in reversed(caps) if ts <= clock), "")
            key = (tuple(s.snapshot() for s in screens), cap)
            if key == last_key:
                frames[-1][1] += 1
            else:
                img = Image.new("RGB", (W, H), BG)
                d = ImageDraw.Draw(img)
                for p, (name, _) in enumerate(panes):
                    x0 = pad + p * (pw + pad)
                    d.rectangle([x0, pad, x0 + pw - 1, pad + head - 1], fill=HEAD_BG)
                    d.text((x0 + 8, pad + 4), name, font=big, fill=ACCENT)
                    cells, _ = screens[p].snapshot()
                    for y, row in enumerate(cells):
                        xx = 0
                        while xx < cols:
                            dim = row[xx][1]; e = xx
                            while e < cols and row[e][1] == dim: e += 1
                            s = "".join(c for c, _ in row[xx:e]).rstrip()
                            if s: d.text((x0 + 8 + xx * cw, pad + head + 6 + y * ch), s, font=font, fill=DIM if dim else FG)
                            xx = e
                cy = pad + head + ph + pad
                d.rectangle([pad, cy, W - pad - 1, cy + 2 * ch], fill=CAP_BG)
                stamp = datetime.fromtimestamp(clock).strftime("%H:%M")
                d.text((pad + 10, cy + ch // 2), f"{stamp}  {cap}", font=big, fill=ACCENT)
                path = os.path.join(tmp, f"f{n:05d}.png"); n += 1
                img.save(path)
                frames.append([path, 1]); last_key = key
            clock += step
        if final:  # the verdict, over the last frame
            img = Image.open(frames[-1][0]).convert("RGB")
            d = ImageDraw.Draw(img)
            bw = max(len(l) for l in final) * round(big.getlength("M")) + 40
            bh = len(final) * (ch + 6) + 30
            x, y = (W - bw) // 2, (H - bh) // 2
            d.rectangle([x, y, x + bw, y + bh], fill=(18, 18, 22), outline=ACCENT, width=2)
            for k, l in enumerate(final):
                d.text((x + 20, y + 15 + k * (ch + 6)), l, font=big, fill=ACCENT if k == 0 else FG)
            path = os.path.join(tmp, f"f{n:05d}.png"); img.save(path)
            frames.append([path, int(opt["--hold"] * opt["--fps"])])
        lst = "".join(f"file '{p}'\nduration {c / opt['--fps']:.4f}\n" for p, c in frames) + f"file '{frames[-1][0]}'\n"
        with open(os.path.join(tmp, "list.txt"), "w") as f: f.write(lst)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", os.path.join(tmp, "list.txt"),
                        "-vf", "split[a][b];[a]palettegen=max_colors=24:stats_mode=diff[p];[b][p]paletteuse=dither=none",
                        "-vsync", "vfr", out], check=True)
    finally:
        shutil.rmtree(tmp)
    print(f"{out}: {len(frames)} frames, {(t_end - t_start) / 3600:.1f} h of wall time, {os.path.getsize(out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
