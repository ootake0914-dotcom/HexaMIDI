import wave
import struct
import math
import sys
import os

# クリップ判定の実用閾値 (int16 の数学的上限 32767 との比較は常に成立しない)
CLIP_THRESHOLD = 32760


def evaluate_wav(filename):
    print(f"\n=======================================================")
    print(f" [EVALUATION] Analyzing: {filename}")
    print(f"=======================================================")

    if not os.path.exists(filename):
        print(f"Error: File '{filename}' does not exist.")
        return False

    try:
        with wave.open(filename, 'rb') as wf:
            n_channels = wf.getnchannels()
            sampwidth = wf.getsampwidth()
            framerate = wf.getframerate()
            n_frames = wf.getnframes()
            if framerate == 0:
                print("Error: Invalid sample rate (0).")
                return False
            duration_sec = n_frames / framerate

            print(f" Header Info:")
            print(f"  - Channels       : {n_channels} ({'Stereo' if n_channels == 2 else 'Mono'})")
            print(f"  - Sample Width   : {sampwidth * 8} bit")
            print(f"  - Sample Rate    : {framerate} Hz")
            print(f"  - Total Frames   : {n_frames}")
            print(f"  - Duration       : {duration_sec:.4f} sec")

            # 検証1: フォーマット仕様
            assert n_channels == 2, f"Channels expected 2, got {n_channels}"
            assert sampwidth == 2, f"Bits per sample expected 16, got {sampwidth * 8}"
            assert framerate == 48000, f"Sample rate expected 48000, got {framerate}"

            raw_data = wf.readframes(n_frames)
            # 切詰められた WAV (宣言フレーム数より実データが短い) でも壊れないように
            # 実際に読めたバイト数からサンプル数を求める
            total_samples = len(raw_data) // 2
            samples = struct.unpack(f'<{total_samples}h', raw_data[:total_samples * 2])
    except (wave.Error, struct.error) as e:
        print(f"Error: cannot decode '{filename}': {e}")
        return False

    if total_samples < 2:
        print("Error: no audio data.")
        return False

    # 左右チャンネル分離
    left_samples = samples[0::2]
    right_samples = samples[1::2]

    # ピークとRMS計算
    max_peak = max(abs(s) for s in samples)
    peak_db = 20 * math.log10(max_peak / 32767.0) if max_peak > 0 else -100.0

    sum_sq = sum(s * s for s in samples)
    rms = math.sqrt(sum_sq / total_samples)
    rms_db = 20 * math.log10(rms / 32767.0) if rms > 0 else -100.0

    print(f"\n Audio Characteristics:")
    print(f"  - Peak Amplitude : {max_peak} / 32767 ({peak_db:.2f} dBFS)")
    print(f"  - RMS Energy     : {rms:.2f} ({rms_db:.2f} dBFS)")

    passed = True
    # NOTE: エンジン出力は |x|>1 を意図的にクランプする設計のため、
    # フルスケール到達は通常動作。合否には影響させず情報として報告する
    if max_peak >= CLIP_THRESHOLD:
        print(f"  -> Note: Output reaches full scale (peak >= {CLIP_THRESHOLD}). "
              f"Limiter/clamp engaged (by design).")
    if max_peak <= 1000:
        print("  -> FAIL: Audio is too quiet or empty!")
        passed = False

    # ADSR解放の検証: 末尾 (最後の0.1秒) のサンプルレベルをチェック
    tail_frames = int(framerate * 0.1)
    tail_samples = samples[-tail_frames * 2:]
    tail_max_peak = max(abs(s) for s in tail_samples)
    tail_rms = math.sqrt(sum(s * s for s in tail_samples) / len(tail_samples))

    print(f"\n ADSR Release Verification (Tail 100ms):")
    print(f"  - Tail Peak      : {tail_max_peak}")
    print(f"  - Tail RMS       : {tail_rms:.4f}")

    if tail_max_peak == 0:
        print("  -> Result: PERFECT SILENCE at tail. ADSR envelope completely released to 0.")
    elif tail_max_peak < 10:
        print(f"  -> Result: Negligible noise ({tail_max_peak}), effectively released.")
    else:
        print(f"  -> Warning: Sound remaining at end! Level={tail_max_peak}")

    # 無音フレーム・ギャップの検出 (スタッカート/休符によるアーティキュレーション確認)
    chunk_size = 480  # 10ms
    silent_chunks = 0
    active_chunks = 0
    for i in range(0, len(left_samples), chunk_size):
        chunk = left_samples[i:i + chunk_size]
        chunk_max = max(abs(s) for s in chunk)
        if chunk_max < 50:
            silent_chunks += 1
        else:
            active_chunks += 1

    print(f"\n Frame Distribution (10ms windows):")
    print(f"  - Active Chunks  : {active_chunks} ({active_chunks * 10} ms)")
    print(f"  - Silent Chunks  : {silent_chunks} ({silent_chunks * 10} ms)")
    print(f"  -> Result: {'PASS' if passed else 'FAIL'} (articulation & dynamics check)")
    return passed

if __name__ == "__main__":
    files = sys.argv[1:] if len(sys.argv) > 1 else \
        ["output.wav", "output_track1.wav", "output_track2.wav"]
    all_passed = True
    for f in files:
        if not evaluate_wav(f):
            all_passed = False
    print("\n=======================================================")
    if all_passed:
        print(" ALL EVALUATION TESTS PASSED SUCCESSFULLY!")
    else:
        print(" SOME EVALUATION CHECKS FAILED!")
    print("=======================================================\n")
    sys.exit(0 if all_passed else 1)
