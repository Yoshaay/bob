#!/usr/bin/env python3
"""Watch Spotify on this Mac and tell Bob when music is playing.

Runs forever (started by the LaunchAgent com.bob.music). Every 2 s it asks
Spotify via AppleScript - without launching Spotify if it's closed - and sends
"music play <artist - title>" / "music stop" to Bob over USB serial.
While playing it repeats the message every 30 s as a keep-alive.
"""
import glob
import os
import subprocess
import sys
import time


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S"), msg, file=sys.stderr, flush=True)

HERE = os.path.dirname(os.path.abspath(__file__))
POLL_S = 2
KEEPALIVE_S = 30

SCRIPT = '''
if application "Spotify" is running then
  tell application "Spotify"
    if player state is playing then
      return "playing" & tab & (artist of current track) & " - " & (name of current track)
    end if
  end tell
end if
return "stopped"
'''


_last_err = ""


def spotify_state():
    try:
        r = subprocess.run(["osascript", "-e", SCRIPT], capture_output=True, text=True, timeout=5)
    except Exception as e:
        log(f"osascript failed: {e}")
        return False, ""
    if r.returncode != 0:
        global _last_err
        if r.stderr.strip() != _last_err:
            _last_err = r.stderr.strip()
            log(f"osascript error: {_last_err}")
        return False, ""
    out = r.stdout.strip()
    if out.startswith("playing"):
        return True, out.split("\t", 1)[1] if "\t" in out else ""
    return False, ""


def send(line: str) -> bool:
    if os.path.exists(os.path.join(HERE, ".flashing")):
        return False
    port = os.environ.get("BOB_PORT") or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        return False
    try:
        fd = os.open(port, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            os.write(fd, (line + "\n").encode())
        finally:
            os.close(fd)
        return True
    except OSError:
        return False


def main() -> None:
    last_sent = None
    last_time = 0.0
    while True:
        playing, track = spotify_state()
        msg = f"music play {track}" if playing else "music stop"
        now = time.time()
        changed = msg != last_sent
        if changed or (playing and now - last_time > KEEPALIVE_S):
            ok = send(msg)
            if changed:
                log(f"{msg} -> {'sent' if ok else 'no device'}")
            if ok:
                last_sent, last_time = msg, now
            elif changed and not playing:
                last_sent = msg  # device gone; nothing to stop
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
