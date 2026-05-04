#!/usr/bin/env python3
"""
scars_probe.py  --  live diagnostic client for ScarsTool's IPC bridge.

Connects to the named pipe \\.\pipe\ScarsTool exposed by the injected DLL,
sends one or more commands, and pretty-prints the JSON replies.

Usage:
    python scars_probe.py ping
    python scars_probe.py mining-probe
    python scars_probe.py mining-watch          # repeat every 250 ms

Exit with Ctrl+C.
"""
import sys, time, json, struct, argparse

PIPE_PATH = r"\\.\pipe\ScarsTool"


def open_pipe():
    # Open in binary read+write mode.  Pipe must exist (DLL must be injected).
    try:
        f = open(PIPE_PATH, "rb+", buffering=0)
    except OSError as e:
        print(f"[!] cannot open {PIPE_PATH}: {e}", file=sys.stderr)
        print("    -> is ScarsTool injected into the running game?",
              file=sys.stderr)
        sys.exit(2)
    return f


def request(f, line):
    if not line.endswith("\n"):
        line += "\n"
    f.write(line.encode("utf-8"))
    # Read a single line response (terminated by \n).
    buf = bytearray()
    while True:
        b = f.read(1)
        if not b:
            break
        if b == b"\n":
            break
        buf.extend(b)
    return buf.decode("utf-8", errors="replace")


def pretty(reply):
    try:
        obj = json.loads(reply)
        return json.dumps(obj, indent=2)
    except Exception:
        return reply


def cmd_watch(f, target, interval_s):
    print(f"[*] watching '{target}' every {interval_s*1000:.0f} ms (Ctrl+C to stop)")
    try:
        while True:
            r = request(f, target)
            print("\033[2J\033[H", end="")   # clear screen
            print(pretty(r))
            time.sleep(interval_s)
    except KeyboardInterrupt:
        print("\n[*] stop")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", nargs="?", default="ping",
                    help="command name to send (e.g. ping, mining-probe, mining-watch)")
    ap.add_argument("args", nargs="*", help="optional command args")
    ap.add_argument("--interval", type=float, default=0.25,
                    help="watch interval in seconds (for *-watch commands)")
    a = ap.parse_args()

    f = open_pipe()

    if a.command.endswith("-watch"):
        target = a.command[:-len("-watch")] + "-probe"
        cmd_watch(f, target, a.interval)
        return

    line = a.command + (" " + " ".join(a.args) if a.args else "")
    r = request(f, line)
    print(pretty(r))


if __name__ == "__main__":
    main()
