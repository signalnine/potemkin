#!/usr/bin/env python3
"""Render an asciicast v2 recording (tools/drive.py --cast) to a GIF.

usage: cast2gif.py IN.cast OUT.gif [--speed N] [--think-speed N] [--idle S] [--fps F] [--font PATH] [--size PX]

A small terminal emulator covers what the PotemkinOS console emits: text,
\\r \\n \\b \\t, SGR dim/bold/reset, clear screen, cursor home/position, erase
line. Idle gaps are capped at --idle seconds, dim (thinking) output plays at
--think-speed x, everything else at --speed x, and
frames go to ffmpeg for a palette-optimized GIF. Needs Pillow and ffmpeg.
"""
import json, os, re, shutil, subprocess, sys, tempfile

from PIL import Image, ImageDraw, ImageFont

BG, FG, DIM = (22, 22, 26), (220, 220, 214), (120, 120, 118)


class Screen:
    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.clear()
        self.dim = False

    def clear(self):
        self.cells = [[(" ", False)] * self.cols for _ in range(self.rows)]
        self.x = self.y = 0

    def scroll(self):
        self.cells.pop(0)
        self.cells.append([(" ", False)] * self.cols)
        self.y = self.rows - 1

    def put(self, ch):
        if self.x >= self.cols:
            self.x = 0
            self.y += 1
            if self.y >= self.rows: self.scroll()
        self.cells[self.y][self.x] = (ch, self.dim)
        self.x += 1

    def feed(self, s):
        i = 0
        while i < len(s):
            c = s[i]
            if c == "\x1b":
                m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", s[i:])
                if m:
                    self.csi(m.group(1), m.group(2))
                    i += m.end()
                    continue
                m = re.match(r"\x1b\][^\x07]*(\x07|\x1b\\)", s[i:])
                i += m.end() if m else 2
                continue
            if c == "\n":
                self.y += 1
                if self.y >= self.rows: self.scroll()
            elif c == "\r": self.x = 0
            elif c == "\b": self.x = max(0, self.x - 1)
            elif c == "\t":
                for _ in range(8 - self.x % 8): self.put(" ")
            elif c >= " ": self.put(c)
            i += 1

    def csi(self, args, op):
        nums = [int(a) if a.isdigit() else 0 for a in args.lstrip("?").split(";")] if args else []
        if op == "m":
            for n in nums or [0]:
                if n in (0, 22): self.dim = False
                elif n == 2: self.dim = True
        elif op == "J":
            if (nums or [0])[0] in (2, 3): self.clear()
        elif op == "H" or op == "f":
            r = (nums[0] if nums else 1) or 1
            c = (nums[1] if len(nums) > 1 else 1) or 1
            self.y, self.x = min(r, self.rows) - 1, min(c, self.cols) - 1
        elif op == "K":
            for x in range(self.x, self.cols): self.cells[self.y][x] = (" ", False)
        elif op == "A": self.y = max(0, self.y - (nums[0] if nums else 1))
        elif op == "C": self.x = min(self.cols - 1, self.x + (nums[0] if nums else 1))
        elif op == "D": self.x = max(0, self.x - (nums[0] if nums else 1))

    def snapshot(self):
        return tuple(tuple(r) for r in self.cells), (self.x, self.y)


def render(snap, font, cw, ch, cols, rows, pad=12):
    cells, (cx, cy) = snap
    img = Image.new("RGB", (cols * cw + 2 * pad, rows * ch + 2 * pad), BG)
    d = ImageDraw.Draw(img)
    for y, row in enumerate(cells):
        # draw runs of same style in one call
        x = 0
        while x < cols:
            dim = row[x][1]
            end = x
            while end < cols and row[end][1] == dim: end += 1
            text = "".join(c for c, _ in row[x:end]).rstrip()
            if text: d.text((pad + x * cw, pad + y * ch), text, font=font, fill=DIM if dim else FG)
            x = end
    if cy < rows and cx < cols:
        d.rectangle([pad + cx * cw, pad + cy * ch + ch - 3, pad + cx * cw + cw - 1, pad + cy * ch + ch - 1], fill=FG)
    return img


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    src, dst = a[0], a[1]
    opt = {"--speed": 4.0, "--think-speed": 40.0, "--idle": 1.5, "--fps": 8.0, "--size": 15,
           "--font": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "--hold": 3.0}
    i = 2
    while i < len(a):
        opt[a[i]] = type(opt[a[i]])(a[i + 1]); i += 2
    with open(src) as f:
        head = json.loads(f.readline())
        events = [json.loads(l) for l in f if l.strip()]
    cols, rows = head["width"], head["height"]
    font = ImageFont.truetype(opt["--font"], opt["--size"])
    cw = round(font.getlength("M"))
    ch = round(opt["--size"] * 1.3)

    # Playback clock: gaps capped at --idle; thinking (dim) runs at
    # --think-speed, the rest at --speed. A probe screen tracks dim state.
    probe = Screen(cols, rows)
    t, last, timeline = 0.0, 0.0, []
    for ts, kind, data in events:
        if kind != "o": continue
        gap = ts - last
        last = ts
        speed = opt["--think-speed"] if probe.dim else opt["--speed"]
        t += min(gap / speed, opt["--idle"])
        probe.feed(data)
        timeline.append((t, data))

    scr = Screen(cols, rows)
    step = 1.0 / opt["--fps"]
    frames, k, clock = [], 0, 0.0
    end = timeline[-1][0] if timeline else 0
    while clock <= end + step:
        while k < len(timeline) and timeline[k][0] <= clock:
            scr.feed(timeline[k][1]); k += 1
        snap = scr.snapshot()
        if frames and frames[-1][0] == snap: frames[-1][1] += 1
        else: frames.append([snap, 1])
        clock += step
    frames[-1][1] += int(opt["--hold"] * opt["--fps"])

    tmp = tempfile.mkdtemp(prefix="cast2gif-")
    try:
        lst = []
        for n, (snap, count) in enumerate(frames):
            p = os.path.join(tmp, f"f{n:05d}.png")
            render(snap, font, cw, ch, cols, rows).save(p)
            lst.append(f"file '{p}'\nduration {count / opt['--fps']:.4f}\n")
        lst.append(f"file '{p}'\n")  # concat demuxer needs the last file twice
        with open(os.path.join(tmp, "list.txt"), "w") as f: f.write("".join(lst))
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
                        "-i", os.path.join(tmp, "list.txt"),
                        "-vf", "split[a][b];[a]palettegen=max_colors=32:stats_mode=diff[p];[b][p]paletteuse=dither=none",
                        "-vsync", "vfr", dst], check=True)
    finally:
        shutil.rmtree(tmp)
    print(f"{dst}: {len(frames)} frames, {end:.1f}s of playback, {os.path.getsize(dst) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
