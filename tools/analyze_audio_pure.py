#!/usr/bin/env python3
import wave
import struct
import math
import sys

# Biquad Filter in Python for exact band energy analysis
class Biquad:
    def __init__(self, b0, b1, b2, a1, a2):
        self.b0 = b0
        self.b1 = b1
        self.b2 = b2
        self.a1 = a1
        self.a2 = a2
        self.s1 = 0.0
        self.s2 = 0.0

    def process(self, x):
        # Direct Form II Transposed
        y = self.b0 * x + self.s1
        self.s1 = self.b1 * x - self.a1 * y + self.s2
        self.s2 = self.b2 * x - self.a2 * y
        return y

def make_lp(fc, fs, q=0.7071):
    w0 = 2.0 * math.pi * fc / fs
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    b0 = (1.0 - cos_w0) * 0.5
    b1 = 1.0 - cos_w0
    b2 = (1.0 - cos_w0) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha
    return Biquad(b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)

def make_hp(fc, fs, q=0.7071):
    w0 = 2.0 * math.pi * fc / fs
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    b0 = (1.0 + cos_w0) * 0.5
    b1 = -(1.0 + cos_w0)
    b2 = (1.0 + cos_w0) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha
    return Biquad(b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)

def make_bp(fc, bw_oct, fs):
    w0 = 2.0 * math.pi * fc / fs
    alpha = math.sin(w0) * math.sinh(math.log(2.0) * 0.5 * bw_oct * w0 / math.sin(w0))
    cos_w0 = math.cos(w0)
    b0 = alpha
    b1 = 0.0
    b2 = -alpha
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha
    return Biquad(b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)

def analyze_wav(filename):
    print("=" * 60)
    print(f"  ACOUSTIC & DYNAMICS ANALYSIS: {filename}")
    print("=" * 60)

    wf = wave.open(filename, 'rb')
    nch = wf.getnchannels()
    sw = wf.getsampwidth()
    fs = wf.getframerate()
    nframes = wf.getnframes()

    chunk_size = 8192
    
    # Global stats
    peak_l, peak_r = 0.0, 0.0
    sum_sq_l, sum_sq_r = 0.0, 0.0
    sum_l_r_dot = 0.0
    clip_count = 0

    # Band filters (LR4 equivalent with 2 cascaded biquads)
    # Low: < 250 Hz (2x LP 250Hz)
    lp_low_1 = make_lp(250.0, fs, 0.7071)
    lp_low_2 = make_lp(250.0, fs, 0.7071)
    # High: > 4000 Hz (2x HP 4000Hz)
    hp_high_1 = make_hp(4000.0, fs, 0.7071)
    hp_high_2 = make_hp(4000.0, fs, 0.7071)
    # Mid: HP 250Hz + LP 4000Hz
    mid_hp_1 = make_hp(250.0, fs, 0.7071)
    mid_hp_2 = make_hp(250.0, fs, 0.7071)
    mid_lp_1 = make_lp(4000.0, fs, 0.7071)
    mid_lp_2 = make_lp(4000.0, fs, 0.7071)

    # Sub: < 20 Hz
    sub_lp = make_lp(20.0, fs, 0.7071)

    # Octave bands
    octave_centers = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    octave_filters = [make_bp(fc, 1.0, fs) for fc in octave_centers]
    octave_energy = [0.0] * len(octave_centers)

    sum_sq_low = 0.0
    sum_sq_mid = 0.0
    sum_sq_high = 0.0
    sum_sq_sub = 0.0
    total_samples = 0

    while True:
        frames = wf.readframes(chunk_size)
        if not frames:
            break
        count = len(frames) // (nch * sw)
        total_samples += count

        fmt = f"<{count * nch}h" if sw == 2 else f"<{count * nch}i"
        raw = struct.unpack(fmt, frames)
        scale = 32768.0 if sw == 2 else 2147483648.0

        for i in range(count):
            if nch == 2:
                sl = raw[i * 2 + 0] / scale
                sr = raw[i * 2 + 1] / scale
            else:
                sl = raw[i] / scale
                sr = sl

            # Clipping check
            if abs(sl) >= 0.99975 or abs(sr) >= 0.99975:
                clip_count += 1

            # Peaks
            if abs(sl) > peak_l: peak_l = abs(sl)
            if abs(sr) > peak_r: peak_r = abs(sr)

            # Sum sq
            sum_sq_l += sl * sl
            sum_sq_r += sr * sr
            sum_l_r_dot += sl * sr

            # Mono for spectral bands
            mono = (sl + sr) * 0.5

            # Low band
            y_low = lp_low_2.process(lp_low_1.process(mono))
            sum_sq_low += y_low * y_low

            # High band
            y_high = hp_high_2.process(hp_high_1.process(mono))
            sum_sq_high += y_high * y_high

            # Mid band
            y_mid = mid_lp_2.process(mid_lp_1.process(mid_hp_2.process(mid_hp_1.process(mono))))
            sum_sq_mid += y_mid * y_mid

            # Sub (<20Hz)
            y_sub = sub_lp.process(mono)
            sum_sq_sub += y_sub * y_sub

            # Octaves
            for k in range(len(octave_centers)):
                yo = octave_filters[k].process(mono)
                octave_energy[k] += yo * yo

    wf.close()

    duration = total_samples / fs
    rms_l = math.sqrt(sum_sq_l / total_samples) if total_samples > 0 else 0
    rms_r = math.sqrt(sum_sq_r / total_samples) if total_samples > 0 else 0
    peak_max = max(peak_l, peak_r)
    rms_avg = math.sqrt((sum_sq_l + sum_sq_r) / (2.0 * total_samples))

    peak_db = 20.0 * math.log10(peak_max) if peak_max > 1e-9 else -120.0
    rms_db = 20.0 * math.log10(rms_avg) if rms_avg > 1e-9 else -120.0
    crest_factor_db = peak_db - rms_db

    corr = sum_l_r_dot / (math.sqrt(sum_sq_l * sum_sq_r) + 1e-12)

    print(f"Duration: {duration:.2f} s | Rate: {fs} Hz | Samples: {total_samples}")
    print(f"\n[1] ダイナミクス & ヘッドルーム")
    print(f"  Peak Level:       {peak_max:7.4f} ({peak_db:6.2f} dBFS)")
    print(f"  RMS Level:        {rms_avg:7.4f} ({rms_db:6.2f} dBFS)")
    print(f"  Crest Factor:     {crest_factor_db:6.2f} dB  (12〜16dBが理想的な音楽的ダイナミクス)")
    print(f"  Clipping Samples: {clip_count} ({clip_count / total_samples * 100:.4f}%)")
    headroom_db = -peak_db
    print(f"  Headroom to 0dB:  {headroom_db:6.2f} dB")

    print(f"\n[2] ステレオ音場・定位感")
    print(f"  L/R 相関係数:     {corr:6.4f} (+1.0=モノラル, 0.6〜0.85=心地よいステレオ感)")

    # Bands energy
    tot_bands = sum_sq_low + sum_sq_mid + sum_sq_high + 1e-12
    p_low_pct = (sum_sq_low / tot_bands) * 100.0
    p_mid_pct = (sum_sq_mid / tot_bands) * 100.0
    p_high_pct = (sum_sq_high / tot_bands) * 100.0
    p_sub_pct = (sum_sq_sub / tot_bands) * 100.0

    db_low = 10.0 * math.log10(sum_sq_low / sum_sq_mid) if sum_sq_mid > 0 and sum_sq_low > 0 else 0
    db_high = 10.0 * math.log10(sum_sq_high / sum_sq_mid) if sum_sq_mid > 0 and sum_sq_high > 0 else 0

    print(f"\n[3] 3分割周波数帯域エネルギー比率")
    print(f"  低域 Low  (20-250 Hz):   {p_low_pct:5.2f}% (Kick, Bass) [{db_low:+5.2f} dB re Mid]")
    print(f"  中域 Mid  (250-4000 Hz): {p_mid_pct:5.2f}% (Piano/Guitar/Lead/Snare)")
    print(f"  高域 High (4-20 kHz):    {p_high_pct:5.2f}% (Hats, Cymbals, Air) [{db_high:+5.2f} dB re Mid]")
    print(f"  超低域 (< 20 Hz):        {p_sub_pct:5.2f}% (DC/インフラサウンド漏れ)")

    print(f"\n[4] 1オクターブ・スペクトル分布")
    oct_names = [
        "Sub-bass (31.5Hz)",
        "Bass / Kick (63Hz)",
        "Upper Bass (125Hz)",
        "Low Mid / Body (250Hz)",
        "Mid (500Hz)",
        "Upper Mid (1kHz)",
        "Presence (2kHz)",
        "Attack / Snap (4kHz)",
        "High / Crisp (8kHz)",
        "Air (16kHz)"
    ]
    tot_oct = sum(octave_energy) + 1e-12
    for name, e in zip(oct_names, octave_energy):
        pct = (e / tot_oct) * 100.0
        db = 10.0 * math.log10(e / tot_oct) if e > 0 else -120.0
        bar = "#" * int(pct * 1.5)
        print(f"  {name:<24}: {pct:5.2f}% ({db:6.1f} dB) {bar}")

if __name__ == '__main__':
    for arg in sys.argv[1:]:
        analyze_wav(arg)
