#!/usr/bin/env python3
import wave
import numpy as np
import sys
import math

def analyze_wav(filename):
    print(f"==================================================")
    print(f"Analyzing Audio File: {filename}")
    print(f"==================================================")
    with wave.open(filename, 'rb') as wf:
        nchannels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        nframes = wf.getnframes()
        data = wf.readframes(nframes)

    dtype = np.int16 if sampwidth == 2 else np.int32
    raw = np.frombuffer(data, dtype=dtype)
    if nchannels == 2:
        audio = raw.reshape(-1, 2)
        left = audio[:, 0].astype(np.float64) / 32768.0
        right = audio[:, 1].astype(np.float64) / 32768.0
    else:
        left = raw.astype(np.float64) / 32768.0
        right = left

    duration = nframes / framerate
    print(f"Sample Rate: {framerate} Hz, Channels: {nchannels}, Duration: {duration:.2f} s, Frames: {nframes}")

    for ch_name, ch_data in [("Left", left), ("Right", right), ("Mono Sum", (left + right) * 0.5)]:
        peak = np.max(np.abs(ch_data))
        peak_db = 20.0 * math.log10(peak) if peak > 1e-9 else -120.0
        rms = np.sqrt(np.mean(ch_data**2))
        rms_db = 20.0 * math.log10(rms) if rms > 1e-9 else -120.0
        crest_factor_db = peak_db - rms_db

        # Clipping detection (>= 32760 / 32768 = 0.99975)
        clipped_samples = np.sum(np.abs(ch_data) >= (32760.0 / 32768.0))

        print(f"\n--- Channel: {ch_name} ---")
        print(f"  Peak:         {peak:7.4f} ({peak_db:6.2f} dBFS)")
        print(f"  RMS:          {rms:7.4f} ({rms_db:6.2f} dBFS)")
        print(f"  Crest Factor: {crest_factor_db:6.2f} dB  (アタック感・トランジェント指標)")
        print(f"  Clipped:      {clipped_samples} samples ({clipped_samples / len(ch_data) * 100:.4f}%)")

    # Stereo Correlation
    if nchannels == 2:
        corr = np.corrcoef(left, right)[0, 1]
        print(f"\n--- Stereo Analysis ---")
        print(f"  Stereo Correlation (L/R): {corr:.4f}  (+1.0=完全モノラル, 0.0=無相関ステレオ, -1.0=逆相)")

    # Spectral Band Analysis (using FFT)
    # Exclude initial 0.2s and ending silence if any, or use whole song
    mono = (left + right) * 0.5
    fft_vals = np.fft.rfft(mono)
    fft_freqs = np.fft.rfftfreq(len(mono), 1.0 / framerate)
    power_spectrum = np.abs(fft_vals)**2

    total_power = np.sum(power_spectrum)

    # Bands:
    # Low: 20 Hz - 250 Hz
    # Mid: 250 Hz - 4000 Hz
    # High: 4000 Hz - 20000 Hz
    mask_low = (fft_freqs >= 20.0) & (fft_freqs < 250.0)
    mask_mid = (fft_freqs >= 250.0) & (fft_freqs < 4000.0)
    mask_high = (fft_freqs >= 4000.0) & (fft_freqs <= 20000.0)

    p_low = np.sum(power_spectrum[mask_low])
    p_mid = np.sum(power_spectrum[mask_mid])
    p_high = np.sum(power_spectrum[mask_high])
    p_sub = np.sum(power_spectrum[fft_freqs < 20.0]) # DC / infrasound

    ratio_low = p_low / total_power * 100.0
    ratio_mid = p_mid / total_power * 100.0
    ratio_high = p_high / total_power * 100.0
    ratio_sub = p_sub / total_power * 100.0

    # Low/Mid/High dB relative to Mid
    db_low_vs_mid = 10.0 * math.log10(p_low / p_mid) if p_mid > 0 and p_low > 0 else 0
    db_high_vs_mid = 10.0 * math.log10(p_high / p_mid) if p_mid > 0 and p_high > 0 else 0

    print(f"\n--- Frequency Band Energy Distribution ---")
    print(f"  Sub-audible (< 20 Hz):  {ratio_sub:5.2f}% (DCブロック効果)")
    print(f"  Low Band  (20-250 Hz):   {ratio_low:5.2f}% (Kick, Bass) [{db_low_vs_mid:+5.2f} dB re Mid]")
    print(f"  Mid Band  (250-4000 Hz): {ratio_mid:5.2f}% (Melody, Body, Snare)")
    print(f"  High Band (4-20 kHz):    {ratio_high:5.2f}% (Hats, Cymbals, Air) [{db_high_vs_mid:+5.2f} dB re Mid]")

    # Octave-band breakdown
    octaves = [
        (20, 40, "Sub Bass (20-40Hz)"),
        (40, 80, "Low Bass / Kick (40-80Hz)"),
        (80, 160, "Upper Bass (80-160Hz)"),
        (160, 315, "Low Mid / Mud (160-315Hz)"),
        (315, 630, "Mid (315-630Hz)"),
        (630, 1250, "Upper Mid (630-1.25kHz)"),
        (1250, 2500, "Presence (1.25-2.5kHz)"),
        (2500, 5000, "Attack / Snap (2.5-5kHz)"),
        (5000, 10000, "High / Crisp (5-10kHz)"),
        (10000, 20000, "Air / Brilliance (10-20kHz)"),
    ]
    print(f"\n--- 1-Octave Spectral Energy Breakdown ---")
    for fl, fh, name in octaves:
        m = (fft_freqs >= fl) & (fft_freqs < fh)
        p = np.sum(power_spectrum[m])
        ratio = p / total_power * 100.0
        p_db = 10.0 * math.log10(p / total_power) if p > 0 else -120
        bars = '#' * int(ratio * 1.5)
        print(f"  {name:<28}: {ratio:5.2f}% ({p_db:6.1f} dB) {bars}")

if __name__ == '__main__':
    for f in sys.argv[1:]:
        analyze_wav(f)
