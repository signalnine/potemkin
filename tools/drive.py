#!/usr/bin/env python3
"""Drive q27-init through a pty like a user at the console.

usage: drive.py [--timeout S] [--log FILE] [--cast FILE] [--size COLSxROWS] -- <q27-init argv...>
--cast writes an asciicast v2 recording (tools/cast2gif.py renders it) and
types input one character at a time.
Reads lines to type from stdin, one per turn. Waits for the "> " prompt
before each, prints everything the console showed.
  ~text     type text into whatever owns the console, then read for 5 s
            (a turn followed by ~lines also returns after 30 s of silence)
  ~^]^]     send the escape chord (Ctrl-] twice), then wait for "> "
  ^C        send SIGINT
"""
import codecs, fcntl, json, os, pty, re, select, struct, sys, termios, time

def main():
    args = sys.argv[1:]
    timeout, log, cast, cols, rows = 900.0, None, None, 100, 32
    while args and args[0] != "--":
        if args[0] == "--timeout": timeout = float(args[1]); args = args[2:]
        elif args[0] == "--log": log = args[1]; args = args[2:]
        elif args[0] == "--cast": cast = args[1]; args = args[2:]
        elif args[0] == "--size": cols, rows = map(int, args[1].split("x")); args = args[2:]
        else: sys.exit(__doc__)
    cmd = args[1:]
    lines = [l.rstrip("\n") for l in sys.stdin]
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(cmd[0], cmd)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = open(log, "wb") if log else None
    t0 = time.time()
    rec = None
    dec = codecs.getincrementaldecoder("utf-8")("replace")  # chunks can split a character
    if cast:
        rec = open(cast, "w")
        rec.write(json.dumps({"version": 2, "width": cols, "height": rows, "timestamp": int(t0)}) + "\n")
    def send(data):
        if not rec:
            os.write(fd, data); return
        for i in range(len(data)):  # type it like a person would
            os.write(fd, data[i:i + 1])
            time.sleep(0.04)
    buf = b""
    def pump(until_prompt, secs=None, idle=None):
        # idle: also return once the console has been quiet this long (a tty
        # program the model started is waiting for keystrokes).
        nonlocal buf
        end = time.time() + (secs if secs else timeout)
        last = time.time()
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                if idle and time.time() - last > idle: return True
                try:
                    wpid, _ = os.waitpid(pid, os.WNOHANG)
                    if wpid: return False
                except ChildProcessError:
                    return False
                continue
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return False
            if not chunk: return False
            last = time.time()
            sys.stdout.buffer.write(chunk); sys.stdout.flush()
            if out: out.write(chunk); out.flush()
            if rec: rec.write(json.dumps([round(time.time() - t0, 4), "o", dec.decode(chunk)]) + "\n"); rec.flush()
            buf += chunk
            if until_prompt and buf.endswith(b"\n> "):
                buf = b""
                return True
        if secs: return True
        print("\n[drive] timeout", file=sys.stderr)
        return False
    if not pump(True): return 1
    for i, l in enumerate(lines):
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if l == "^C":
            os.kill(pid, 2); continue
        if l == "~^]^]":
            send(b"\x1d\x1d")
            if not pump(True): return 1
            continue
        if l.startswith("~"):
            time.sleep(0.5)
            send(l[1:].encode() + b"\n")
            pump(False, 5)
            continue
        time.sleep(0.2)
        send(l.encode() + b"\n")
        if not pump(True, idle=30 if nxt.startswith("~") else None): return 1
    os.kill(pid, 15)
    return 0

if __name__ == "__main__":
    sys.exit(main())
