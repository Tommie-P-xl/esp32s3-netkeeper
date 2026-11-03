"""Reset the ESP32-S3 over USB-Serial/JTAG and dump the console for N seconds.

usage: python monitor.py [COM6] [seconds] [--no-reset]

The console is USB-Serial/JTAG CDC with no flow control: if the host does not
drain fast enough the device's TX buffer overflows and bytes are silently lost.
So the read loop drains whatever is available in a tight loop instead of waiting
for a fixed-size read to fill.
"""

import sys
import time

import serial

PORT = "COM6"
SECONDS = 15.0
RESET = True

for arg in sys.argv[1:]:
    if arg == "--no-reset":
        RESET = False
    elif arg.upper().startswith("COM"):
        PORT = arg
    else:
        try:
            SECONDS = float(arg)
        except ValueError:
            pass

# Configure control lines before opening the USB CDC port. pySerial defaults
# both to True; even --no-reset then changes them on open/close and can cause
# an ESP32-S3 USB-UART reset when the monitor exits.
ser = serial.Serial(port=None, baudrate=115200, timeout=0.02)
ser.dtr = False
ser.rts = False
ser.port = PORT
ser.open()
ser.set_buffer_size(rx_size=1024 * 1024)

if RESET:
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(True)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setDTR(True)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.05)

deadline = time.time() + SECONDS
chunks = []
while time.time() < deadline:
    n = ser.in_waiting
    data = ser.read(n if n else 1)
    if data:
        chunks.append(data)
    else:
        time.sleep(0.002)
ser.close()

buf = b"".join(chunks)
sys.stdout.write(buf.decode("utf-8", "replace"))
sys.stdout.write("\n--- bytes: %d ---\n" % len(buf))
