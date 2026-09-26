#!/usr/bin/env python3
"""Drive q27-init through a pty like a user at the console.

usage: drive.py [--timeout S] [--log FILE] -- <q27-init argv...>
Reads lines to type from stdin, one per turn. Waits for the "> " prompt
before each, prints everything the console showed.
"""
import os, pty, re, select, sys, time

def main():
    args = sys.argv[1:]
    timeout, log = 900.0, None
    while args and args[0] != "--":
        if args[0] == "--timeout": timeout = float(args[1]); args = args[2:]
        elif args[0] == "--log": log = args[1]; args = args[2:]
        else: sys.exit(__doc__)
    cmd = args[1:]
    lines = [l.rstrip("\n") for l in sys.stdin]
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(cmd[0], cmd)
    out = open(log, "wb") if log else None
    buf = b""
    def pump(until_prompt):
        nonlocal buf
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
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
            sys.stdout.buffer.write(chunk); sys.stdout.flush()
            if out: out.write(chunk); out.flush()
            buf += chunk
            if until_prompt and buf.endswith(b"\n> "):
                buf = b""
                return True
        print("\n[drive] timeout", file=sys.stderr)
        return False
    if not pump(True): return 1
    for l in lines:
        if l == "^C":
            os.kill(pid, 2); continue
        time.sleep(0.2)
        os.write(fd, l.encode() + b"\n")
        if not pump(True): return 1
    os.kill(pid, 15)
    return 0

if __name__ == "__main__":
    sys.exit(main())
