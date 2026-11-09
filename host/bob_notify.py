#!/usr/bin/env python3
"""Send a Claude Code status to Bob over USB serial.

Used as a Claude Code hook:  bob_notify.py <working|tool|done|attention|end|idle>
Each event carries the Claude session id ("@abcd1234"), so Bob can track several sessions.
Every call is logged to host/bob.log (last ~500 lines kept).
Reads the hook JSON from stdin (for the project name). Never fails or blocks
Claude: any problem (device unplugged, port busy) is silently ignored.
Override the port with BOB_PORT=/dev/cu.usbmodemXXXX.
"""
import glob
import json
import os
import signal
import sys

VALID = {"working", "tool", "done", "attention", "end", "idle"}
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bob.log")


def log(msg: str) -> None:
    try:
        import time
        lines = open(LOG).read().splitlines()[-499:] if os.path.exists(LOG) else []
        lines.append(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}")
        open(LOG, "w").write("\n".join(lines) + "\n")
    except Exception:
        pass


def main() -> None:
    signal.signal(signal.SIGALRM, lambda *_: sys.exit(0))
    signal.alarm(2)

    state = sys.argv[1] if len(sys.argv) > 1 else ""
    if state not in VALID:
        return

    label = ""
    data = {}
    try:
        data = json.load(sys.stdin) if not sys.stdin.isatty() else {}
        cwd = data.get("cwd") or os.getcwd()
        label = os.path.basename(os.path.normpath(cwd))
    except Exception:
        pass
    event = data.get("hook_event_name", "?") if isinstance(data, dict) else "?"
    sid = "".join(c for c in str(data.get("session_id", "")) if c.isalnum())[:8] if isinstance(data, dict) else ""
    label = "".join(c for c in label if 32 <= ord(c) < 127)[:34]

    if os.path.exists(os.path.join(os.path.dirname(LOG), ".flashing")):
        log(f"{event} -> {state} {label} (skipped: flashing)")
        return

    port = os.environ.get("BOB_PORT") or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        log(f"{event} -> {state} {label} (no device)")
        return

    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        line = f"{state} @{sid} {label}" if sid and state != "idle" else f"{state} {label}"
        os.write(fd, (line.strip() + "\n").encode())
        log(f"{event} -> {line} via {port}")
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
