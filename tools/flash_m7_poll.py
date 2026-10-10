# tools/flash_m7_poll.py —— M7 固件烧录守护:轮询串口出现 → app-only 烧录
# (0x10000,保留 NVS 存档)→ 抓取启动日志。用户已确认烧录。
# 用法: python flash_m7_poll.py [超时秒,默认900]
import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

IDF_PY = str(Path.home() / ".espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe")
APP_BIN = r"D:\project\AI-PASSPORT\darkroom\build\FoloToy-AI-Passport.bin"
LOG_OUT = r"D:\project\AI-PASSPORT\darkroom\build\m7_boot.log"

BOOT_CAPTURE = r'''
import serial, time, sys
port, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
lines = []
with open(out, "w", encoding="utf-8") as log, \
     serial.Serial(port, 115200, timeout=0.5) as s:
    s.setDTR(False); s.setRTS(True)   # USB-Serial-JTAG 复位序列,重抓完整启动
    time.sleep(0.1)
    s.setDTR(True); s.setRTS(False)
    time.sleep(0.1)
    s.setDTR(False)
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            text = data.decode("utf-8", errors="replace")
            log.write(text); log.flush()
            lines.append(text)
            sys.stdout.write(text); sys.stdout.flush()
'''


def ports_now():
    return {p.device for p in list_ports.comports()}


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main():
    budget = int(sys.argv[1]) if len(sys.argv) > 1 else 900
    known = ports_now()
    print(f"等待设备插入(已有串口: {sorted(known) or '无'};超时 {budget}s)...",
          flush=True)
    deadline = time.time() + budget
    port = None
    while time.time() < deadline:
        now = ports_now()
        fresh = now - known
        if fresh:
            port = sorted(fresh)[0]
            break
        if now:  # 已存在的口也接受(烧录前状态为空,此分支仅为兜底)
            port = sorted(now)[0]
            break
        time.sleep(2)
    if not port:
        print("超时:未见串口设备。", flush=True)
        return 2
    print(f"发现串口 {port},开始 app-only 烧录(0x10000,保留 NVS 存档)...",
          flush=True)

    r = run([IDF_PY, "-m", "esptool", "--chip", "esp32c3", "-p", port,
             "-b", "460800", "--before", "default_reset",
             "--after", "hard_reset",
             "write_flash", "0x10000", APP_BIN])
    if r.returncode != 0:
        print("烧录失败(见上方 esptool 输出)。", flush=True)
        return 1
    print("烧录完成,复位抓取启动日志 40s...", flush=True)
    time.sleep(1.0)
    cap = run([IDF_PY, "-c", BOOT_CAPTURE, port, "40", LOG_OUT])
    print(f"\n启动日志已存 {LOG_OUT}(退出码 {cap.returncode})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
