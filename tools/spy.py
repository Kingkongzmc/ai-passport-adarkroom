import serial, time, sys
port, seconds = sys.argv[1], float(sys.argv[2])
with serial.Serial(port, 115200, timeout=0.5) as s:
    # 被动监听:不做 DTR/RTS 复位,保留卡死/panic 现场
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
