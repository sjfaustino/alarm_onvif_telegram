#!/usr/bin/env python3
"""Remote diagnostics for the board. Reads DEBUG_TOKEN from include/secrets.h,
and DASHBOARD_LOGIN ("user:password") too if the dashboard has a login.

  scripts/board.py status              /debug/status as JSON
  scripts/board.py log [--follow]      the log ring; --follow keeps polling
  scripts/board.py coredump [FILE]     download a stored crash dump
  scripts/board.py flash FILE --yes    OTA upload; REBOOTS the live board

Set BOARD_HOST to override the address.
"""
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

HOST = os.environ.get("BOARD_HOST", "192.168.8.132")


def secret(name):
    with open("include/secrets.h") as f:
        m = re.search(r'^\s*#define\s+%s\s+"([^"]+)"' % name, f.read(), re.M)
    return m.group(1) if m else None


def token():
    value = secret("DEBUG_TOKEN")
    if not value:
        sys.exit("No DEBUG_TOKEN in include/secrets.h")
    return value


def get(path):
    headers = {"X-Debug-Token": token()}
    login = secret("DASHBOARD_LOGIN")
    if login:
        # Without it a dashboard login answers 401, and five of those lock this
        # address out of every route.
        headers["Authorization"] = "Basic " + base64.b64encode(login.encode()).decode()
    req = urllib.request.Request(f"http://{HOST}{path}", headers=headers)
    return urllib.request.urlopen(req, timeout=30)


def log(follow):
    # Each response is at most 16 KB, so keep reading until caught up.
    since = 0
    while True:
        with get(f"/debug/log?since={since}") as r:
            if int(r.headers.get("X-Log-Oldest", 0)) > since > 0:
                print("--- log lines were missed ---")
            since = int(r.headers["X-Log-Next"])
            body = r.read()
        sys.stdout.write(body.decode(errors="replace"))
        sys.stdout.flush()
        if body:
            continue
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
        login = secret("DASHBOARD_LOGIN")
        upload = subprocess.run(["curl", "-sS", "--fail", "-m", "300", *(["-u", login] if login else []),
                                 "-F", f"firmware=@{args[1]}", f"http://{HOST}/firmware/update"],
                                stdout=subprocess.DEVNULL)
        if upload.returncode != 0:
            sys.exit("Upload FAILED - the board was not updated.")
        print("Uploaded. The board reboots if the image was accepted; check `status` in about 20 seconds.")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
