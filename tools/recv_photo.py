#!/usr/bin/env python3
# Demo 6 配套：从串口接收板子 dump 的 base64 照片，还原成 .jpg
#
# 用法（板子端固件烧好、串口没被 monitor 占用时）：
#   ~/.platformio/penv/bin/python tools/recv_photo.py
#   然后短接 D1<->GND 拍照，脚本会自动存出 photos/pic_N.jpg
#
# 退出：Ctrl+C
#
# 原理：板子按这个格式打印——
#   ---PHOTO-BEGIN--- <序号> <字节数>
#   <base64 多行>
#   ---PHOTO-END---
# 脚本就靠这两行标记切出中间的 base64，拼接、解码、写文件。

import base64
import os
import sys
import glob
import serial  # pyserial

BAUD = 115200
OUTDIR = os.path.join(os.path.dirname(__file__), "..", "photos")


def find_port():
    # XIAO 在 mac 上通常是 /dev/cu.usbmodemXXXX
    candidates = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
    if not candidates:
        print("没找到串口设备 (/dev/cu.usbmodem*)，板子插好了吗？")
        sys.exit(1)
    return candidates[0]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    os.makedirs(OUTDIR, exist_ok=True)
    print(f"打开串口 {port} @ {BAUD}")
    print("等待拍照…（短接 D1<->GND 触发；Ctrl+C 退出）")

    ser = serial.Serial(port, BAUD, timeout=1)

    collecting = False
    b64_lines = []
    idx = expected = 0

    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()

        if line.startswith("---PHOTO-BEGIN---"):
            parts = line.split()
            idx = int(parts[1]) if len(parts) > 1 else 0
            expected = int(parts[2]) if len(parts) > 2 else 0
            collecting = True
            b64_lines = []
            print(f"\n开始接收第 {idx} 张，预计 {expected} 字节…")
            continue

        if line == "---PHOTO-END---":
            collecting = False
            try:
                data = base64.b64decode("".join(b64_lines))
            except Exception as e:
                print(f"base64 解码失败: {e}")
                continue
            out = os.path.abspath(os.path.join(OUTDIR, f"pic_{idx}.jpg"))
            with open(out, "wb") as f:
                f.write(data)
            ok = "✅" if (expected == 0 or len(data) == expected) else "⚠️大小对不上"
            print(f"已保存 {out}  ({len(data)} 字节) {ok}")
            continue

        if collecting:
            b64_lines.append(line)
        else:
            # 非照片数据，原样回显，方便看板子的普通日志
            print("  [板子] " + line)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n退出")
