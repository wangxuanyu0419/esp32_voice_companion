import sys
import time

import serial


sys.stdout.reconfigure(encoding="utf-8", errors="replace")
port = serial.Serial("COM4", 115200, timeout=0.25, dsrdtr=False, rtscts=False)
port.dtr = False
port.rts = False
end = time.time() + 180
while time.time() < end:
    raw = port.readline()
    if raw:
        print(raw.decode("utf-8", "replace").rstrip(), flush=True)
port.close()
