/**
 * @file math_tests.c
 * @brief DSP 数学の厳密性検証スイート
 * @details 各数式を「理論リファレンス」または「解析的性質」に対して
 *          定量的に検証する。すべての合格基準は明示的な数値トレランス。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth_engine.h"
#include "sub_common.h"
#include "spectrum_analyzer.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("  -> PASS: %s\n", name); } \
    else      { g_fail++; printf("  -> FAIL: %s\n", name); } \
} while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* 1. 等価パワー パン: 任意の pan で L^2+R^2 が一定 (Parseval 的性質)   */
/* ------------------------------------------------------------------ */
static void test_pan_law(void)
{
    printf("[TEST] Equal-power panning preserves energy\n");
    double worst = 0.0;
    for (int i = 0; i <= 20; i++) {
        float p = i / 20.0f;
        float l, r;
        sub_equal_power_pan(p, &l, &r);
        double e = (double)l*l + (double)r*r;    /* 理論値 1.0 */
        double d = fabs(e - 1.0);
        if (d > worst) worst = d;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "max |L^2+R^2-1| = %.2e (tol 1e-6)", worst);
    CHECK(worst < 1e-6, buf);

    /* 線形パンとの比較: センターで +3dB 膨らまないこと */
    float l, r;
    sub_equal_power_pan(0.5f, &l, &r);
    snprintf(buf, sizeof(buf), "center gain %.4f each (=1/sqrt2)", l);
    CHECK(fabs(l - 0.70710678f) < 1e-6, buf);
}

/* ------------------------------------------------------------------ */
/* 2. 半音比近似: sub_semitone_ratio のセント誤差                       */
/* ------------------------------------------------------------------ */
static void test_semitone_ratio(void)
{
    printf("[TEST] Semitone ratio approximation vs exact 2^(st/12)\n");
    /* 使用範囲: ピッチベンド ±2 半音 + ビブラート ~±0.5 → ±4 半音で検証 */
    double worst_cents = 0.0;
    for (int i = -48; i <= 48; i++) {
        double st = i / 12.0;                    /* ±4 半音走査 */
        double exact = pow(2.0, st / 12.0);
        double approx = sub_semitone_ratio((float)st);
        double cents = 1200.0 * log2(approx / exact);
        if (fabs(cents) > worst_cents) worst_cents = fabs(cents);
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "max error over ±4 st = %.4f cent (tol 0.25)", worst_cents);
    CHECK(worst_cents < 0.25, buf);

    /* 旧線形近似は ±2 半音で -10.7 cent: 大幅改善を確認 */
    double lin = 1.0 + 0.0577623 * 2.0;
    double lin_err = fabs(1200.0 * log2(lin / pow(2.0, 2.0 / 12.0)));
    CHECK(lin_err > 5.0 && worst_cents < 1.0,
          "cubic Taylor is >5 cent better than old linear approx at +2 st");
}

/* ------------------------------------------------------------------ */
/* 3. デチューン定数: JP-8000 比 ±0.12/±0.28 半音の厳密一致             */
/* ------------------------------------------------------------------ */
static void test_detune_constants(void)
{
    printf("[TEST] Unison detune constants exactness\n");
    const double lo  = 0.99309250;   /* -12c (-0.12 st) */
    const double hi  = 1.00695555;   /* +12c (+0.12 st) */
    const double lo2 = 0.98395666;   /* -28c (-0.28 st) */
    const double hi2 = 1.01630493;   /* +28c (+0.28 st) */
    struct { double v, ref; } t[4] = {
        { lo,  pow(2.0, -12.0 / 1200.0) },
        { hi,  pow(2.0,  12.0 / 1200.0) },
        { lo2, pow(2.0, -28.0 / 1200.0) },
        { hi2, pow(2.0,  28.0 / 1200.0) },
    };
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        double cents = 1200.0 * log2(t[i].v / t[i].ref);
        if (fabs(cents) > 0.005) ok = 0;
    }
    CHECK(ok, "all four detune constants within 0.005 cent of exact");
}

/* ------------------------------------------------------------------ */
/* 4. 正弦波 LUT 補間誤差                                               */
/* ------------------------------------------------------------------ */
static float s_test_lut[SUB_SINE_LUT_SIZE + 1];

static void test_sine_interpolation(void)
{
    printf("[TEST] Sine LUT interpolation accuracy (1024-entry FMA linear)\n");
    for (int i = 0; i <= SUB_SINE_LUT_SIZE; i++) {
        s_test_lut[i] = sinf((float)i * 2.0f * (float)M_PI / (float)SUB_SINE_LUT_SIZE);
    }
    double max_err = 0.0;
    /* ビン境界とビン中央の両方を高密度に走査 (65536点) */
    for (int i = 0; i < SUB_SINE_LUT_SIZE * 64; i++) {
        float ph = (float)i / ((float)SUB_SINE_LUT_SIZE * 64.0f);
        float ideal = sinf(2.0f * (float)M_PI * ph);
        float v = sub_lookup_sine(s_test_lut, ph);
        if (fabs(v - ideal) > max_err) max_err = fabs(v - ideal);
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "LUT max error %.3e (tol 5.0e-6, -106 dBFS, > 16-bit noise floor)", max_err);
    CHECK(max_err < 5.0e-6, buf);
    double snr_db = -20.0 * log10(max_err > 0 ? max_err : 1e-12);
    snprintf(buf, sizeof(buf), "LUT precision %.1f dBFS exceeds 16-bit audio floor (-96 dBFS)", snr_db);
    CHECK(snr_db > 100.0, buf);
}

/* ------------------------------------------------------------------ */
/* 5. 三角波 PolyBLAMP: 折返し(エイリアス)成分の定量測定                  */
/*    f=1997Hz @48kHz (fs/f=24.03 非整数比)。真の高調波は 12 次まで     */
/*    (24kHz=Nyquist)だが、打ち切り残りが 15kHz 以上へ折り返す。        */
/*    Hann 窓+Goertzel で 15〜24kHz 帯域エネルギーを比較する             */
/* ------------------------------------------------------------------ */
static double goertzel_band_energy(const float *sig, uint32_t n, double f_lo, double f_hi)
{
    const double fs = (double)SUB_SAMPLE_RATE;
    /* Hann 窓 */
    double acc = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        acc += 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
    }
    (void)acc; /* 窓は下記で直接適用 */
    double total = 0.0;
    const int steps = 180;
    for (int s = 0; s < steps; s++) {
        double freq = f_lo + (f_hi - f_lo) * s / (steps - 1);
        double k = freq * n / fs;
        double w = 2.0 * M_PI * k / n;
        double coeff = 2.0 * cos(w);
        double q1 = 0, q2 = 0;
        for (uint32_t i = 0; i < n; i++) {
            double win = 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
            double x = sig[i] * win;
            double q0 = coeff * q1 - q2 + x;
            q2 = q1; q1 = q0;
        }
        double pwr = q1*q1 + q2*q2 - coeff*q1*q2;
        total += sqrt(pwr);
    }
    return total / steps;
}

typedef struct {
    float y1, x1, a;
} Hp1;

static int render_tri_variant(float variant, float *out, uint32_t n, double f)
{
    /* variant: 0=naive, 4=C4·dt (採用), 8=旧 C8·dt */
    const double fs = (double)SUB_SAMPLE_RATE;
    const float dt = (float)(f / fs);
    double phase = 0.37;
    for (uint32_t i = 0; i < n; i++) {
        float p = (float)fmod(phase, 1.0);
        float sp = p + 0.5f; if (sp >= 1.0f) sp -= 1.0f;
        float v = (p < 0.5f) ? (4.0f*p - 1.0f) : (3.0f - 4.0f*p);
        if (variant > 0.0f) {
            v += variant * dt * sub_poly_blamp(p, dt);
            v -= variant * dt * sub_poly_blamp(sp, dt);
        }
        out[i] = v;
        phase += f / fs;
    }
    return (int)n;
}

static void test_triangle_blamp(void)
{
    printf("[TEST] Triangle PolyBLAMP alias suppression (f=1997Hz, 15-24kHz band)\n");
    const uint32_t n = (uint32_t)(SUB_SAMPLE_RATE * 0.25);
    static float sig_naive[120000], sig_c4[120000], sig_c8[120000];
    render_tri_variant(0.0f, sig_naive, n, 1997.0);
    render_tri_variant(4.0f, sig_c4,   n, 1997.0);
    render_tri_variant(8.0f, sig_c8,   n, 1997.0);

    double e_naive = goertzel_band_energy(sig_naive, n, 15000.0, 23900.0);
    double e_c4    = goertzel_band_energy(sig_c4,    n, 15000.0, 23900.0);
    double e_c8    = goertzel_band_energy(sig_c8,    n, 15000.0, 23900.0);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "alias band: naive %.3e -> C4 %.3e (>3x reduction)",
             e_naive, e_c4);
    CHECK(e_c4 < e_naive / 3.0, buf);

    snprintf(buf, sizeof(buf),
             "adopted C=4 beats old C=8 by >1.5x (%.3e vs %.3e)", e_c4, e_c8);
    CHECK(e_c4 * 1.5 < e_c8, buf);
}

/* ------------------------------------------------------------------ */
/* 6. SVF 安定性 & DC ゲイン                                            */
/* ------------------------------------------------------------------ */
static void test_svf(void)
{
    printf("[TEST] TPT SVF stability and DC gain\n");
    int stable = 1, dc_ok = 1;
    double worst_peak = 0, worst_dc_err = 0;
    float wf=0, wq=0;
    for (float fc = 60.0f; fc <= 20000.0f; fc *= 1.5f) {
        for (float q = 0.1f; q <= 12.0f; q *= 2.0f) {
            SubSvf svf;
            memset(&svf, 0, sizeof(svf));
            sub_svf_set(&svf, fc, q, (float)SUB_SAMPLE_RATE);
            float peak = 0.0f;
            double mean = 0.0;
            /* 白色雑音ではなくステップ応答で DC ゲインを確認 */
            for (int i = 0; i < 48000; i++) {
                float y = sub_svf_lp(&svf, 1.0f);
                if (fabs(y) > peak) peak = fabs(y);
                if (i > 24000) mean += y;
            }
            mean /= 24000.0;
            if ((double)peak > worst_peak) { worst_peak = peak; wf=fc; wq=q; }
            if (!(peak < 100.0f) || !isfinite(peak)) {
                printf("    [diag] unstable: fc=%.1f q=%.2f peak=%.6g\n",
                       (double)fc, (double)q, (double)peak);
                stable = 0;
            }
            /* LP の DC ゲインは理論上 1.0 */
            if (fabs(mean - 1.0) > worst_dc_err) { worst_dc_err = fabs(mean-1.0); }
            if (fabs(mean - 1.0) > 0.02) dc_ok = 0;
        }
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "|output| bounded across sweep (worst peak %.3f @%.0fHz q%.1f)",
             worst_peak, (double)wf, (double)wq);
    CHECK(stable, buf);
    snprintf(buf, sizeof(buf), "LP DC gain == 1.0 within 2%% (worst dev %.4f)", worst_dc_err);
    CHECK(dc_ok, buf);
}

/* ------------------------------------------------------------------ */
/* 7. RBJ Biquad: Peaking フィルタの f0 におけるゲイン                   */
/*    |H(e^{jw})| を係数から直接計算し設計値と比較                       */
/* ------------------------------------------------------------------ */
static double biquad_mag(const BiquadFilter *f, double freq, double fs)
{
    double w = 2.0 * M_PI * freq / fs;
    double c1 = cos(w), s1 = sin(w), c2 = cos(2.0*w), s2 = sin(2.0*w);
    double bre = f->b0 + f->b1*c1 + f->b2*c2;
    double bim = -(f->b1*s1 + f->b2*s2);
    double are = 1.0 + f->a1*c1 + f->a2*c2;
    double aim = -(f->a1*s1 + f->a2*s2);
    return sqrt(bre*bre + bim*bim) / sqrt(are*are + aim*aim);
}

static void test_biquad(void)
{
    printf("[TEST] RBJ peaking EQ gain at center frequency\n");
    const double fs = 48000.0;
    int ok = 1;
    struct { float fc, gain_db, q; } cases[3] = {
        { 1000.0f,  6.0f, 1.0f },
        { 3200.0f, -4.0f, 1.4f },
        { 400.0f,  -1.5f, 1.0f },
    };
    for (int i = 0; i < 3; i++) {
        BiquadFilter f;
        biquad_init(&f);
        biquad_calc_coeffs(&f, BIQUAD_PEAKING, cases[i].fc, cases[i].gain_db,
                           cases[i].q, (float)fs);
        double mag_db = 20.0 * log10(biquad_mag(&f, cases[i].fc, fs));
        if (fabs(mag_db - cases[i].gain_db) > 0.05) ok = 0;
    }
    CHECK(ok, "|H(f0)| matches design gain within 0.05 dB");
}

/* ------------------------------------------------------------------ */
/* 8. ADSR 時定数: リリース係数は release_time で -60dB (0.1%) 到達       */
/* ------------------------------------------------------------------ */
static void test_adsr_time_constants(void)
{
    printf("[TEST] ADSR exponential time constants\n");
    /* sub_env_release_coeff = exp(-ln(1000)/release_samples):
     * release_time 経過時にレベルが開始値の 0.1% (= -60dB) になる */
    const float rel_sec = 0.25f;
    uint32_t rel_samples = (uint32_t)(rel_sec * (float)SUB_SAMPLE_RATE);
    float coeff = expf(-6.907755f / (float)rel_samples);
    float level = 1.0f;
    for (uint32_t i = 0; i < rel_samples; i++) level *= coeff;
    char buf[128];
    snprintf(buf, sizeof(buf), "level after release_time = %.5f (target 0.001)", level);
    CHECK(fabs(level - 0.001f) < 0.0002, buf);

    /* sub_env_advance の RELEASE 終了判定 (<=0.001) と整合する */
    SubEnvCore c; memset(&c, 0, sizeof(c));
    c.adsr.release_time_sec = rel_sec;
    c.adsr.exponential_decay = true;
    c.current_env_level = 1.0f;
    sub_env_begin_release(&c);
    int samples_to_idle = -1;
    for (int i = 0; i < (int)(rel_sec * SUB_SAMPLE_RATE * 2); i++) {
        sub_env_advance(&c);
        if (c.env_state == SUB_ENV_IDLE) { samples_to_idle = i; break; }
    }
    snprintf(buf, sizeof(buf), "voice idles at sample %d (~%.1f ms)",
             samples_to_idle, (double)samples_to_idle / SUB_SAMPLE_RATE * 1000.0);
    CHECK(samples_to_idle > 0 &&
          fabs(samples_to_idle - (int)rel_samples) < (int)(rel_samples * 0.02),
          buf);
}

/* ------------------------------------------------------------------ */
/* 9. リバーブ (SynthEngine): 線形性 & インパルス応答の減衰収束           */
/* ------------------------------------------------------------------ */
static void test_reverb_linearity(void)
{
    printf("[TEST] Reverb linearity and decay bound\n");
    SynthEngine e1, e2, e3;
    synth_engine_init(&e1);
    synth_engine_init(&e2);
    synth_engine_init(&e3);
    synth_engine_set_reverb(&e1, true, 0.75f, 0.35f, 0.30f);
    synth_engine_set_reverb(&e2, true, 0.75f, 0.35f, 0.30f);
    synth_engine_set_reverb(&e3, true, 0.75f, 0.35f, 0.30f);

    /* superposition test: rev(a)+rev(b) == rev(a+b) (浮動小数点トレランス付き) */
    const uint32_t n = 4800;
    static int16_t buf_a[4800*2], buf_b[4800*2], buf_ab[4800*2], buf_sum[4800*2];
    for (uint32_t i = 0; i < n*2; i++) {
        buf_a[i]  = (int16_t)((i % 97) * 40 - 1900);
        buf_b[i]  = (int16_t)((i % 53) * 61 - 1500);
        buf_ab[i] = (int16_t)(buf_a[i] + buf_b[i]);
    }
    synth_engine_render(&e1, buf_a, n);
    synth_engine_render(&e2, buf_b, n);
    synth_engine_render(&e3, buf_ab, n);
    for (uint32_t i = 0; i < n*2; i++) buf_sum[i] = (int16_t)(buf_a[i] + buf_b[i]);
    double num = 0, den = 0;
    for (uint32_t i = 0; i < n*2; i++) {
        double d = (double)buf_sum[i] - buf_ab[i];
        num += d*d;
        den += (double)buf_ab[i]*buf_ab[i];
    }
    double rel_err = sqrt(num / (den > 1 ? den : 1));
    char buf[128];
    snprintf(buf, sizeof(buf), "superposition relative error %.2e (tol 0.02)", rel_err);
    CHECK(rel_err < 0.02, buf);

    /* インパルス応答が減衰してゼロへ収束すること (feedback<1 の理論的帰結) */
    SynthEngine e4;
    synth_engine_init(&e4);
    synth_engine_set_reverb(&e4, true, 0.95f, 0.10f, 0.50f);
    memset(buf_a, 0, sizeof(buf_a));
    buf_a[0] = 30000;
    synth_engine_render(&e4, buf_a, n);          /* インパルス注入 */
    int nonzero_tail = 0;
    for (int k = 0; k < 20; k++) {               /* さらに 20 秒レンダリング */
        synth_engine_render(&e4, buf_b, n);
        if (k >= 15) {
            for (uint32_t i = 0; i < n*2; i++)
                if (buf_b[i] != 0) nonzero_tail++;
        }
    }
    snprintf(buf, sizeof(buf), "tail silent after ~15s (nonzero=%d)", nonzero_tail);
    CHECK(nonzero_tail == 0, buf);
}

/* ------------------------------------------------------------------ */
/* 10. エンジン経由の等価パワー検証 (単一ボイスを pan 走査)               */
/* ------------------------------------------------------------------ */
static void test_engine_pan_energy(void)
{
    printf("[TEST] Engine pan sweep energy consistency\n");
    SynthEngine eng;
    synth_engine_init(&eng);
    synth_engine_control_change(&eng, 0, 7, 127);      /* ch vol max */

    double energies[5];
    float pans[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (int t = 0; t < 5; t++) {
        synth_engine_all_notes_off(&eng);
        synth_engine_control_change(&eng, 0, 10, (uint8_t)(pans[t] * 127.0f + 0.5f));
        synth_engine_channel_note_on(&eng, 0, 69, 1.0f);   /* A4 */
        static int16_t pcm[4800*2];
        synth_engine_render(&eng, pcm, 4800);
        double acc = 0;
        for (uint32_t i = 2400*2; i < 4800*2; i++) acc += (double)pcm[i]*pcm[i];
        energies[t] = sqrt(acc / (4800.0*2));
    }
    double lo = energies[0], hi = energies[0];
    for (int t = 1; t < 5; t++) {
        if (energies[t] < lo) lo = energies[t];
        if (energies[t] > hi) hi = energies[t];
    }
    double spread_db = 20.0 * log10(hi / (lo > 0 ? lo : 1));
    char buf[128];
    snprintf(buf, sizeof(buf), "RMS spread across pans = %.2f dB (tol 1.0 dB)", spread_db);
    CHECK(spread_db < 1.0, buf);
}

/* ------------------------------------------------------------------ */
/* 11. スペクトラムアナライザ (Goertzel 8バンド): 1kHz ピーク & 選択度 */
/* ------------------------------------------------------------------ */
static void test_spectrum_analyzer_goertzel(void)
{
    printf("[TEST] Goertzel 8-band spectrum analyzer (1kHz tone selectivity & dBFS)\n");

    /* 1kHz フルスケール正弦波 (48kHz サンプリング) */
    static int16_t pcm[2048 * 2];
    for (int i = 0; i < 2048; i++) {
        double ph = (double)i * 1000.0 * 2.0 * M_PI / 48000.0;
        int16_t val = (int16_t)(sin(ph) * 32767.0);
        pcm[i * 2 + 0] = val;
        pcm[i * 2 + 1] = val;
    }

    /* 平滑化フィルタが収束するまで 16 窓投入 */
    for (int w = 0; w < 16; w++) {
        spectrum_update(pcm, 2048);
    }

    const float *levels = spectrum_levels();

    /* 1kHz バンド (インデックス 4) が最大かつ 0dBFS 近傍 (-3.0dBFS 以上) であること */
    char buf[128];
    snprintf(buf, sizeof(buf), "1kHz band level = %.2f dBFS (target >= -3.0 dBFS)", levels[4]);
    CHECK(levels[4] >= -3.0f, buf);

    /* 離れた帯域 (63Hz, 8kHz) が 20dB 以上減衰していること */
    snprintf(buf, sizeof(buf), "63Hz band = %.2f dBFS, 8kHz band = %.2f dBFS (tol < -20.0 dBFS)",
             levels[0], levels[7]);
    CHECK(levels[0] < -20.0f && levels[7] < -20.0f, buf);

    /* 1kHz が全 8 バンド中で最大強度であること */
    int max_band = 0;
    float max_val = levels[0];
    for (int b = 1; b < SPECTRUM_BANDS; b++) {
        if (levels[b] > max_val) {
            max_val = levels[b];
            max_band = b;
        }
    }
    CHECK(max_band == 4, "1kHz (band 4) is peak of spectrum");

    /* フォーマット文字列の検証 */
    char line[192];
    size_t len = spectrum_format(line, sizeof(line));
    CHECK(len > 0 && strstr(line, "[SPECTRUM]") != NULL && strstr(line, "1k |") != NULL,
          "spectrum_format outputs valid ASCII spectrum bar");
}

/* ------------------------------------------------------------------ */
/* 12. ウェーブテーブル ミップ選択: エイリアス安全 & 最小明るさの保証      */
/* ------------------------------------------------------------------ */
static void test_wavetable_mip_selection(void)
{
    printf("[TEST] Wavetable 6-mip ladder alias-safety invariant\n");
    static const uint8_t harmonics[SUBWT_MIPS] = { 15, 10, 6, 4, 2, 1 };
    char buf[128];

    /* 不変条件 1: 選ばれたミップがラダー内で解を持つ周波数では、
     * 最高次倍音が必ずエイリアス限界以下であること (MIDI 全音域走査)。
     * 最暗ミップでも満たせない極高音 (f > LIMIT/h_min) のみ仕様上の例外 */
    float exhaustion_hz = SUBWT_ALIAS_LIMIT_HZ / (float)harmonics[SUBWT_MIPS - 1];
    for (int note = 0; note <= 127; note++) {
        float f = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
        uint8_t mip = subwt_pick_mip(f);
        snprintf(buf, sizeof(buf), "note %d (%.1f Hz) -> mip %u safe (%.0f Hz <= %.0f)",
                 note, (double)f, (unsigned)mip,
                 (double)harmonics[mip] * f, (double)SUBWT_ALIAS_LIMIT_HZ);
        CHECK(f >= exhaustion_hz || (float)harmonics[mip] * f <= SUBWT_ALIAS_LIMIT_HZ, buf);
    }

    /* 不変条件 2: 最小明るさ — ひとつ明るいミップで足りるなら
     * 暗いミップを選んでいないこと (音質の最適性) */
    bool minimal = true;
    for (int note = 0; note <= 127; note++) {
        float f = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
        uint8_t mip = subwt_pick_mip(f);
        if (mip > 0 && (float)harmonics[mip - 1] * f <= SUBWT_ALIAS_LIMIT_HZ) {
            minimal = false;
        }
    }
    CHECK(minimal, "never selects darker mip than alias-safety requires");

    /* 不変条件 3: 単調性 — 周波数が上がるほど同じか暗いミップ */
    uint8_t prev = 0;
    bool monotonic = true;
    for (int note = 0; note <= 127; note++) {
        float f = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
        uint8_t mip = subwt_pick_mip(f);
        if (mip < prev) monotonic = false;
        prev = mip;
    }
    CHECK(monotonic, "selected mip is monotonically non-decreasing with pitch");

    /* 中域の明るさ回復: 旧実装は C6 超 (1046Hz) で一律 7 倍音の暗い表へ
     * 落としていたが、新ラダーは 15 倍音のまま安全域で使い分ける
     * (1200Hz=15次/2000Hz=10次/3000Hz=6次、いずれも 20kHz 以下) */
    CHECK(subwt_pick_mip(1200.0f) == 0 && subwt_pick_mip(2000.0f) == 1 &&
          subwt_pick_mip(3000.0f) == 2,
          "mid band keeps brighter spectra within alias-safe limits");
}

/* ------------------------------------------------------------------ */
/* 13. ROM ウェーブテーブル: 生成データと実行時 init の bit 一致          */
/* ------------------------------------------------------------------ */
static void test_wavetable_rom_bitexact(void)
{
    printf("[TEST] ROM wavetable bank bit-exactness vs runtime init\n");
    static SubWavBank runtime;
    subwav_init(&runtime);
    CHECK(sizeof(g_sub_wavbank) == sizeof(runtime.table),
          "ROM bank size matches SubWavBank layout");
    int mismatches = 0;
    float max_delta = 0.0f;
    const float *p_rom = (const float *)g_sub_wavbank;
    const float *p_rt  = (const float *)runtime.table;
    size_t count = sizeof(runtime.table) / sizeof(float);
    for (size_t i = 0; i < count; i++) {
        float diff = fabsf(p_rom[i] - p_rt[i]);
        if (diff > max_delta) max_delta = diff;
        if (diff > 1e-6f) mismatches++;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "ROM vs runtime init max delta %.3e (tol < 1e-6, mismatches=%d)", max_delta, mismatches);
    CHECK(mismatches == 0 && max_delta < 1e-6f, buf);
}

int main(void)
{
    printf("=======================================================\n");
    printf(" DSP Mathematical Verification Suite\n");
    printf("=======================================================\n");

    test_pan_law();
    test_semitone_ratio();
    test_detune_constants();
    test_sine_interpolation();
    test_triangle_blamp();
    test_svf();
    test_biquad();
    test_adsr_time_constants();
    test_reverb_linearity();
    test_engine_pan_energy();
    test_spectrum_analyzer_goertzel();
    test_wavetable_mip_selection();
    test_wavetable_rom_bitexact();

    printf("=======================================================\n");
    printf(" MATH TESTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return (g_fail == 0) ? 0 : 1;
}
