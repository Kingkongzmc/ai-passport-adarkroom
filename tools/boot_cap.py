import serial, time, sys
port, seconds = sys.argv[1], float(sys.argv[2])
with serial.Serial(port, 115200, timeout=0.5) as s:
    s.setDTR(False); s.setRTS(True)
    time.sleep(0.1)
    s.setDTR(True); s.setRTS(False)
    time.sleep(0.1)
    s.setDTR(False)
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
