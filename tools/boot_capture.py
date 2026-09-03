#!/usr/bin/env python3
"""
Spresense フルブートログキャプチャ (HexaSense 音不出し調査用)

1. COM ポートをオープン (既存アプリは Ctrl+C で停止)
2. NSH プロンプトを確認して `reboot` を送信
3. ブートローダー直後から全シリアル出力をファイルへ保存
4. 主要マーカー (電源警告 / ASMP エラー / ENQUEUE EAGAIN 等) の集計を表示

使い方:
  python tools/boot_capture.py -p COM6 -d 90 -o docs/debug_20260826/boot1.log
"""

import argparse
import os
import re
import sys
import time

import serial

BOOT_HINTS = re.compile(
    r"(?i)(powering on|initializing|asmp|nsh>|nx_start|\[main\]|\[audio\]|spresense)"
)

MARKERS = {
    "codec_power_WARN": r"board_audio_power_control returned false",
    "audio_open_FAIL": r"cannot open /dev/audio",
    "registermq_FAIL": r"REGISTERMQ failed",
    "enqueue_init_fail": r"initial enqueue\(s\) failed",
    "asmp_ERROR": r"\[ASMP\] Error:",
    "mptask_fail": r"mptask_\w+\(.*failed",
    "fallback_single": r"falling back to single-core",
    "cores_up": r"All 6 cores are up",
    "subcore_started": r"Starting SubCore \d",
    "dsp_tag": r"dsp-[0-9]{8}[a-z]",
    "enqueue_eagain": r"ENQUEUE retrying ret=-1",
    "underrun_msg": r"UNDERRUN",
    "buffer_starvation": r"buffer starvation",
    "render_sync_timeout": r"render sync timeout",
    "watchdog": r"\[ASMP\]\[WD\]",
    "panic_fault": r"(?i)(up_hardfault|_assert|PANIC)",
}


def main() -> int:
    ap = argparse.ArgumentParser(description="Spresense full-boot serial capture")
    ap.add_argument("-p", "--port", default="COM6")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-d", "--duration", type=float, default=90.0,
                    help="capture seconds after reboot trigger")
    ap.add_argument("-o", "--out", default="docs/debug_20260826/boot.log")
    args = ap.parse_args()

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:
        print(f"[CAPTURE] Failed to open {args.port}: {exc}")
        return 1

    print(f"[CAPTURE] Opened {args.port} @ {args.baud}. Interrupting any running app...")
    raw = bytearray()
    t_start = time.time()

    def drain(seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            waiting = ser.in_waiting
            if waiting:
                chunk = ser.read(waiting)
                raw.extend(chunk)
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.01)

    # 1. 実行中アプリの中断と NSH 復帰
    for _ in range(3):
        ser.write(b"\x03")
        time.sleep(0.15)
        ser.write(b"\r\n")
        time.sleep(0.35)
    drain(1.5)
    saw_prompt = b"nsh>" in bytes(raw[-512:])
    print(f"\n[CAPTURE] nsh prompt detected: {saw_prompt}")

    # 2. リブート要求 → ブート出力を捕捉
    ser.write(b"reboot\r\n" if saw_prompt else b"\r\nreboot\r\n")
    drain(10.0)
    text_now = bytes(raw).decode("utf-8", "replace")
    tail = text_now[text_now.find("nsh>"):] if "nsh>" in text_now else text_now
    booted = bool(BOOT_HINTS.search(tail[max(0, len(tail) - 2048):]))
    if not booted:
        print("\n[CAPTURE] reboot 応答なし。ボードの RESET ボタンを押してください "
              "(45秒間待機します)...")
        deadline = time.time() + 45.0
        while time.time() < deadline:
            before = len(raw)
            drain(0.5)
            chunk_text = bytes(raw[before:]).decode("utf-8", "replace")
            if BOOT_HINTS.search(chunk_text):
                print("\n[CAPTURE] ブート出力を検出しました。")
                break

    # 3. 残り時間をひたすら記録
    remaining = args.duration - (time.time() - t_start)
    if remaining > 0:
        print(f"\n[CAPTURE] Recording for another {remaining:.0f}s ...")
        drain(remaining)

    ser.close()

    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(f"# HexaSense boot capture | {stamp} | port={args.port}\n")
        fh.write(bytes(raw).decode("utf-8", "replace"))

    # 4. マーカー集計
    text = bytes(raw).decode("utf-8", "replace")
    print(f"\n[CAPTURE] Saved {len(raw)} bytes -> {args.out}")
    print("[CAPTURE] === MARKER SUMMARY ===")
    hit_any = False
    for name, pattern in MARKERS.items():
        found = re.findall(pattern + r".*", text)
        if found:
            hit_any = True
            uniq = found if len(found) <= 4 else found[:3] + [f"... x{len(found)}"]
            print(f"  [{name}] x{len(found)}: " + " | ".join(s.strip()[:100] for s in uniq))
    if not hit_any:
        print("  (no diagnostic markers found)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
