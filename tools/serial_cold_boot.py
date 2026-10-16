import time
import sys

import serial


port = serial.Serial("COM4", 115200, timeout=0.25, dsrdtr=False, rtscts=False)
port.dtr = False
port.rts = True
time.sleep(0.12)
port.rts = False
end = time.time() + 30
lines = []
while time.time() < end:
    raw = port.readline()
    if raw:
        lines.append(raw.decode("utf-8", "replace").rstrip())
port.close()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
print("\n".join(lines))
