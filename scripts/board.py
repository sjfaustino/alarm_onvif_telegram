#!/usr/bin/env python3
"""Remote diagnostics for the board. Reads DEBUG_TOKEN from include/secrets.h,
and DASHBOARD_LOGIN ("user:password") too if the dashboard has a login.

  scripts/board.py status              /debug/status as JSON
  scripts/board.py log [--follow]      the log ring; --follow keeps polling
  scripts/board.py coredump [FILE]     download a stored crash dump
  scripts/board.py flash FILE --yes    OTA upload; REBOOTS the live board
  scripts/board.py sign FILE           write FILE's signed copy, firmware.signed.bin
  scripts/board.py verify FILE         have the board check a signed file; changes nothing
  scripts/board.py keygen              create the signing key (once)

Set BOARD_HOST to override the address, FIRMWARE_SIGNING_KEY the private key's path.
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


SIGNING_KEY = os.environ.get("FIRMWARE_SIGNING_KEY",
                             os.path.join(os.path.expanduser("~"), ".alarm_onvif_telegram", "firmware_signing_key.pem"))
PUBLIC_KEY_HEADER = "include/firmware_signing_key.h"
SIGNED_MAGIC = b"AOTSIG1\n"
SIGNATURE_SLOT = 72  # lib/signed_firmware/signed_firmware.h


def keygen():
    if os.path.exists(SIGNING_KEY):
        sys.exit(f"{SIGNING_KEY} already exists; not replacing it.")
    os.makedirs(os.path.dirname(SIGNING_KEY), exist_ok=True)
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", SIGNING_KEY], check=True)
    public = subprocess.run(["openssl", "pkey", "-in", SIGNING_KEY, "-pubout"], check=True,
                            capture_output=True, text=True).stdout
    pem = [line.strip() for line in public.strip().splitlines()]
    body = "\n".join(f'    "{line}\\n"' for line in pem)
    with open(PUBLIC_KEY_HEADER, "w", newline="\n") as out:
        out.write("#pragma once\n\n// Checks uploaded firmware. Its private half stays off this repository; see\n"
                  "// \"Firmware signing\" in the README.\n"
                  f"static const char FIRMWARE_PUBLIC_KEY_PEM[] =\n{body};\n")
    print(f"Created {SIGNING_KEY}. Back it up: without it, updates need USB.")


def sign(path):
    if not os.path.exists(SIGNING_KEY):
        sys.exit(f"No signing key at {SIGNING_KEY}.")
    signature_path = path + ".sig"
    subprocess.run(["openssl", "dgst", "-sha256", "-sign", SIGNING_KEY, "-out", signature_path, path], check=True)
    with open(signature_path, "rb") as f:
        signature = f.read()
    os.remove(signature_path)
    if len(signature) > SIGNATURE_SLOT:
        sys.exit("Signature longer than the header allows.")
    header = SIGNED_MAGIC + bytes([len(signature)]) + bytes(7) + signature.ljust(SIGNATURE_SLOT, b"\0")
    signed_path = os.path.join(os.path.dirname(path), "firmware.signed.bin")
    with open(path, "rb") as f, open(signed_path, "wb") as out:
        out.write(header + f.read())
    return signed_path


def upload(path, route):
    login = secret("DASHBOARD_LOGIN")
    return subprocess.run(["curl", "-sS", "--fail", "-m", "300", *(["-u", login] if login else []),
                           "-F", f"firmware=@{path}", f"http://{HOST}{route}"], stdout=subprocess.DEVNULL).returncode == 0


def board_wants_signed():
    with get("/debug/status") as r:
        return bool(json.load(r).get("otaSigned"))


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
            more = r.headers.get("X-Log-More") == "1"
            body = r.read()
        sys.stdout.write(body.decode(errors="replace"))
        sys.stdout.flush()
        # The board says whether it had more; a busy log would never run dry.
        if more:
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
        # A board from before signing can't read the signed format.
        image = sign(args[1]) if board_wants_signed() else args[1]
        if not upload(image, "/firmware/update"):
            sys.exit("Upload FAILED - the board was not updated.")
        print("Uploaded. The board reboots if the image was accepted; check `status` in about 20 seconds.")
    elif cmd == "sign" and len(args) >= 2:
        print(sign(args[1]))
    elif cmd == "verify" and len(args) >= 2:
        if not upload(sign(args[1]), "/firmware/verify"):
            sys.exit("REFUSED - see the board's log.")
        print("Signature OK.")
    elif cmd == "keygen":
        keygen()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
