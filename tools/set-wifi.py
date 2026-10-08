#!/usr/bin/env python3
"""Save home Wi-Fi on the LED grid over USB, then report the address it gets.

    python3 tools/set-wifi.py "Network name"

The password is asked for at a hidden prompt and sent straight to the board.
Needs pyserial (PlatformIO's Python has it; this script re-runs itself there).
"""
import getpass, glob, os, shutil, subprocess, sys, time

try:
    import serial
except ImportError:
    # PlatformIO's Python has pyserial. Its venv interpreter resolves to the
    # same binary as plain python3, so guard re-running with an env var.
    pio = shutil.which("pio")
    py = open(pio).readline()[2:].strip() if pio else None
    if py and not os.environ.get("SET_WIFI_REEXEC"):
        os.environ["SET_WIFI_REEXEC"] = "1"
        sys.exit(subprocess.call([py, *sys.argv]))
    sys.exit("pyserial missing: pip3 install pyserial")

ssid = sys.argv[1] if len(sys.argv) > 1 else input("Network name: ")
ports = glob.glob("/dev/cu.usbserial-*")
if not ports:
    sys.exit("No ESP32 found on USB (looked for /dev/cu.usbserial-*).")
password = getpass.getpass(f'Password for "{ssid}" (hidden): ')
if "\t" in ssid + password or "\n" in ssid + password:
    sys.exit("Tabs and newlines aren't supported.")

s = serial.Serial()
s.port, s.baudrate, s.timeout = ports[0], 115200, 0.2
s.dtr = s.rts = False          # don't reset the board on open
s.open()
s.write(f"WIFI\t{ssid}\t{password}\n".encode())
del password

print("Sent. Waiting for the board to join (up to 30 s)...")
buf, end = "", time.time() + 30
while time.time() < end:
    buf += s.read(512).decode(errors="replace")
    while "\n" in buf:
        line, buf = buf.split("\n", 1)
        line = line.strip()
        if line.startswith(("Saved", "Joined", "Couldn't")):
            print(line)
        if line.startswith("Joined"):
            sys.exit(0)
        if line.startswith("Couldn't"):
            sys.exit("Check the network name and password, then run this again.")
sys.exit("No answer from the board. Is it the LED grid firmware?")
