#!/usr/bin/env python3
"""Remote diagnostics for the board. Reads DEBUG_TOKEN from include/secrets.h.

  scripts/board.py status              /debug/status as JSON
  scripts/board.py log [--follow]      the log ring; --follow keeps polling
  scripts/board.py coredump [FILE]     download a stored crash dump
  scripts/board.py flash FILE --yes    OTA upload; REBOOTS the live board

Set BOARD_HOST to override the address.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

HOST = os.environ.get("BOARD_HOST", "192.168.8.132")


def token():
    with open("include/secrets.h") as f:
        m = re.search(r'^\s*#define\s+DEBUG_TOKEN\s+"([^"]+)"', f.read(), re.M)
    if not m:
        sys.exit("No DEBUG_TOKEN in include/secrets.h")
    return m.group(1)


def get(path):
    req = urllib.request.Request(f"http://{HOST}{path}", headers={"X-Debug-Token": token()})
    return urllib.request.urlopen(req, timeout=30)


def log(follow):
    since = 0
    while True:
        with get(f"/debug/log?since={since}") as r:
            if int(r.headers.get("X-Log-Oldest", 0)) > since > 0:
                print("--- log lines were missed ---")
            since = int(r.headers["X-Log-Next"])
            sys.stdout.write(r.read().decode(errors="replace"))
            sys.stdout.flush()
        if not follow:
            return
        time.sleep(2)


def main():
    args = sys.argv[1:]
    cmd = args[0] if args else ""
    if cmd == "status":
        with get("/debug/status") as r:
            print(json.dumps(json.load(r), indent=2))
    elif cmd == "log":
        log("--follow" in args)
    elif cmd == "coredump":
        path = args[1] if len(args) > 1 else "coredump.elf"
        with get("/debug/coredump") as r, open(path, "wb") as f:
            f.write(r.read())
        print(f"Saved {path}. Decode it with the firmware.elf of the build that crashed.")
    elif cmd == "flash" and len(args) >= 2 and "--yes" in args:
        # A crash dump can only be decoded with the ELF of the build that crashed.
        version = re.search(r'BUILD_VERSION "([^"]+)"', open("include/generated_build_version.h").read()).group(1)
        os.makedirs(".pio/flashed", exist_ok=True)
        elf = os.path.join(os.path.dirname(args[1]), "firmware.elf")
        if os.path.exists(elf):
            shutil.copy(elf, f".pio/flashed/firmware-{version}.elf")
        subprocess.run(["curl", "-sS", "-m", "300", "-F", f"firmware=@{args[1]}",
                        f"http://{HOST}/firmware/update"], check=True, stdout=subprocess.DEVNULL)
        print("Uploaded. The board reboots if the image was accepted; check `status` in about 20 seconds.")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
