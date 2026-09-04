/**
 * @file sub_common.h
 * @brief Sony Spresense ASMP サブコア共通モジュール・DSPユーティリティ
 * @details PolyBLEP/PolyBLAMP オシレータ、Biquad 5バンドEQ、ステレオリバーブDSP、LUT
 */

#ifndef SUB_COMMON_H_
#define SUB_COMMON_H_

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <float.h>
#include "asmp_protocol.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846f)
#endif

#define SUB_SAMPLE_RATE (48000)
#define SUB_SINE_LUT_SIZE (1024)

/* ビルド識別タグは asmp_protocol.h の HEXASENSE_DSP_TAG を使用する
 * (Main / 全サブコアが単一の定義を共有するため) */

/* ========================================================================= */
/* 1. Xorshift32 ホワイトノイズ生成 (sub4と同一・旧LFSRから置換)             */
/*    周期 65k(1.36s)->4G(89s)、除算排除、VCVT排除。互換名は残す。          */
/* ========================================================================= */
static inline float sub_noise_lfsr(uint32_t *seed)
{
    uint32_t x = *seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *seed = x;
    union { uint32_t u; float f; } c;
    c.u = 0x3F800000u | (x >> 9);
    return (c.f - 1.5f) * 2.0f;
}
static inline float sub_noise_xor(uint32_t *s){ return sub_noise_lfsr(s); }

/* ========================================================================= */
/* 1b. 16bit 飽和クランプ (ARM __SSAT / ホスト分岐フォールバック)             */
/* ========================================================================= */
static inline int16_t sub_ssat16(int32_t val)
{
#if defined(__arm__) || defined(__ARM_ARCH)
#  if defined(__GNUC__) || defined(__clang__)
    int32_t out;
    __asm__("ssat %0, #16, %1" : "=r"(out) : "r"(val));
    return (int16_t)out;
#  elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
    return (int16_t)__ssat(val, 16);
#  else
    return (int16_t)((val > 32767) ? 32767 : ((val < -32768) ? -32768 : val));
#  endif
#else
    return (int16_t)((val > 32767) ? 32767 : ((val < -32768) ? -32768 : val));
#endif
}

/* ========================================================================= */
/* 1b-1. TPDF ディザ付き 16bit 量子化 (最終出力段専用)                        */
/* --------------------------------------------------------------------------- */
/* 単純切り捨て/丸めは量子化誤差が信号と相関し、リバーブ尾などの
 * 低レベル減衰音で高調波歪みとして聴こえる。TPDF (三角確率密度) ディザは
 * この相関を排除し SNR を約 -93dBFS の一定ノイズフロアに置換する。
 * xorshift32 2 抽出の差分で三角分布を生成する (演算 ~20 サイクル)。       */

/* NaN/±Inf 整数ビット判定 (音声ホットループ専用)。
 * 指数部全1 (0x7F800000) をビットマスクで判定することで、
 * Cortex-M4F での VMRS や FPU 比較命令を構造的に 100% 排除する。
 * memcpy(4) はコンパイラ最適化によりレジスタ間転送に消える (#14) */
static inline bool sub_isfinite_f(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}
#ifndef SUB_ISFINITE_F
#define SUB_ISFINITE_F(x) sub_isfinite_f((x))
#endif

static inline int16_t sub_quantize_dither(float x, uint32_t *rng)
{
    /* 音質修正: 旧fabsf<0.0001(-80dB)ゲートはリバーブ尾(-80〜-96dB)を
     * 早期切断しTPDFの意味を消していた。デノーマル flush のみ(1e-20)に
     * 緩和し、真の無音(x==0)は分岐なしで自然に0+ディザ±1LSB→丸めで0へ。
     * 低レベル(-96dB)までディザで滑らかに減衰する */
    if (!sub_isfinite_f(x)) return 0;
    if (fabsf(x) < 1e-20f) return 0;
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    /* P9改善: 2回 xorshift(各3xor+2shift= ~12c)を1回に半減。
     * 32bit乱数1個を上位16bit/下位16bitに分割して三角分布(r1-r2)を生成。
     * 2つの独立xorshift差と相関は -93dB以下で知覚差なし、毎サンプル約5-7c削減。
     * 512frame*48k*2ch=49k回/frame -> 約0.3ms/frame削減期待。
     * 処理面: Sub5/Engineの最終量子化ホットループで最も頻出 */
    uint32_t s = *rng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    *rng = s;
    int32_t r1 = (int32_t)(s & 0xFFFFu);
    int32_t r2 = (int32_t)((s >> 16) & 0xFFFFu);
    /* 三角分布ディザ [-1, +1) LSB + half away from zero 丸め */
    float v = x * 32767.0f + (float)(r1 - r2) * (1.0f / 65536.0f);
    v += (v >= 0.0f) ? 0.5f : -0.5f;
    return sub_ssat16((int32_t)v);
}

/* ========================================================================= */
/* 1b-2. カオスストレステスト用 遅延インジェクション フック                    */
/* ========================================================================= */
/* 通常ビルドでは SUB_CHAOS_DELAY は完全に消える (ゼロコスト・実機影響なし)。
 * カオスストレステスト (tools/chaos_stress_test.c) のみ
 * ASMP_CHAOS_INJECT_DELAY を define してビルドし、各音源/DSP サブコアの
 * エポック処理直前にランダム遅延 (0〜5ms) を注入する。これにより
 * 「各コアの処理時間がランダムに変動してもキューやエポック同期が
 *  破綻しないこと」を構造的に検証できる。 */
#if defined(ASMP_CHAOS_INJECT_DELAY)
void sub_chaos_delay(int core_idx);            /* 実体はテスト側で定義 */
#define SUB_CHAOS_DELAY(core_idx) sub_chaos_delay((core_idx))
#else
#define SUB_CHAOS_DELAY(core_idx) ((void)0)
#endif

/* ========================================================================= */
/* 2. 正弦波 LUT & 線形補間 (共有 ROM 常数化で RAM 20KB削減)                    */
/* ========================================================================= */
extern const float g_sine_lut[SUB_SINE_LUT_SIZE+1];

static inline void sub_init_sine_lut(float *lut)
{
    if (lut == (float*)g_sine_lut) return; /* ROM常数は初期化不要 */
    for (int i = 0; i <= SUB_SINE_LUT_SIZE; i++) {
        lut[i] = sinf((float)i * 2.0f * (float)M_PI / (float)SUB_SINE_LUT_SIZE);
    }
}

static inline float sub_lookup_sine(const float *lut, float phase)
{
    if (!lut) lut = g_sine_lut;
    /* phase は周期関数として折り返し参照する (負値/1超過も許容)。
     * 旧実装の floorf() を排除し int キャスト+マスクで mod を取る。
     * 負値は floor 方向へ丸めてからマスクする (ホットパスでは非成立分岐)。 */
    float pos = phase * (float)SUB_SINE_LUT_SIZE;
    int idx = (int)pos;
    if (pos < 0.0f && idx != pos) idx--; /* floor */
    int i0 = idx & (SUB_SINE_LUT_SIZE - 1);
    int i1 = (idx + 1) & (SUB_SINE_LUT_SIZE - 1);
    float frac = pos - (float)idx;
    /* 1024要素テーブルの線形補間 (単一 FMA 2サイクル):
     * 誤差は最大 4.7e-6 (-106dB) であり、16bit オーディオ (-96dB) において
     * 完全ロスレス・高音質を維持したまま、3次エルミート多項式の重い積和を全廃 */
    return fmaf(lut[i1] - lut[i0], frac, lut[i0]);
}

/* ========================================================================= */
/* 3. PolyBLEP / PolyBLAMP 帯域制限アンチエイリアシング                       */
/* ========================================================================= */
static inline float sub_poly_blep(float t, float dt)
{
    if (dt <= 0.0f || dt >= 0.5f) return 0.0f;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* メモリ事前計算逆数 inv_dt による除算レス PolyBLEP (14cyc vdiv.f32 -> 1cyc vmul.f32) */
static inline float sub_poly_blep_fast(float t, float dt, float inv_dt)
{
    if (dt <= 0.0f || dt >= 0.5f) return 0.0f;
    if (t < dt) {
        t *= inv_dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) * inv_dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float sub_osc_saw_fast(float phase, float dt, float inv_dt)
{
    return ((2.0f * phase) - 1.0f) - sub_poly_blep_fast(phase, dt, inv_dt);
}

static inline float sub_poly_blep_q32(uint32_t phase, uint32_t increment)
{
    if (increment == 0u || increment >= 0x80000000u) return 0.0f;
    if (phase < increment) {
        float t = (float)phase / (float)increment;
        return t + t - t * t - 1.0f;
    }
    uint32_t tail = 0u - phase;
    if (tail < increment) {
        float t = -(float)tail / (float)increment;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float sub_osc_square_q32(uint32_t phase, uint32_t increment)
{
    float naive = (phase & 0x80000000u) ? 1.0f : -1.0f;
    return naive + sub_poly_blep_q32(phase, increment)
                 - sub_poly_blep_q32(phase + 0x80000000u, increment);
}

static inline float sub_poly_blamp(float t, float dt)
{
    if (dt <= 0.0f || dt >= 0.5f) return 0.0f;
    if (t < dt) {
        t = t / dt - 1.0f;
        return -1.0f / 3.0f * t * t * t;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return 1.0f / 3.0f * t * t * t;
    }
    return 0.0f;
}

/* ========================================================================= */
/* 3b. 共通 帯域制限オシレータ (矩形/ノコギリ/三角)                            */
/*     旧 sub2/sub3 の重複実装を集約したもの (実装ドリフトによる音質バグ防止) */
/* ========================================================================= */
static inline float sub_osc_square(float phase, float dt)
{
    float naive = (phase < 0.5f) ? 1.0f : -1.0f;
    float shift = (phase >= 0.5f) ? (phase - 0.5f) : (phase + 0.5f);
    return naive + sub_poly_blep(phase, dt) - sub_poly_blep(shift, dt);
}

static inline float sub_osc_saw(float phase, float dt)
{
    return ((2.0f * phase) - 1.0f) - sub_poly_blep(phase, dt);
}

static inline float sub_osc_triangle(float phase, float dt)
{
    float naive = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    float shift = (phase >= 0.5f) ? (phase - 0.5f) : (phase + 0.5f);
    /* PolyBLAMP 補正: 傾き跳び Δ=8 に対し理論・数値較正の結果
     * C=4·dt がエイリアシング最適 (C=8 は過大で無補正より悪化する)。
     * 数値検証: f=3971Hz @48kHz で折返し成分が約 1/4.6 に低減 */
    return naive
         + 4.0f * dt * sub_poly_blamp(phase, dt)
         - 4.0f * dt * sub_poly_blamp(shift, dt);
}

/* ========================================================================= */
/* 4. MIDI ノート -> 周波数 (Hz) 変換                                       */
/*    70cyc powf → 5cyc LUT。note_onバースト64音で3.9kcyc/0.025ms削減。RAMは512B ROM */
/*    OPTIMIZATION_MEMO #4: SUB_COMMON_NO_LUTでBSS削減 (Sub1/Sub5やsub_sine_lutでは未使用) */
/* ========================================================================= */
#ifndef SUB_COMMON_NO_LUT
static float s_sub_freq_lut[128];
static int   s_sub_freq_lut_ready = 0;
static inline void sub_freq_lut_init(void)
{
    if (s_sub_freq_lut_ready) return;
    for (int i = 0; i < 128; i++) {
        s_sub_freq_lut[i] = 440.0f * powf(2.0f, ((float)i - 69.0f) / 12.0f);
    }
    s_sub_freq_lut_ready = 1;
}
static inline float sub_note_to_freq(uint8_t note)
{
    if (!s_sub_freq_lut_ready) sub_freq_lut_init();
    return s_sub_freq_lut[note & 0x7F];
}
#else
static inline void sub_freq_lut_init(void) {}
static inline float sub_note_to_freq(uint8_t note)
{
    return 440.0f * powf(2.0f, ((float)(note & 0x7F) - 69.0f) / 12.0f);
}
#endif

/**
 * @brief 半音差 -> 周波数比 2^(st/12) の高速近似
 *        線形近似 (1 + 0.0578·st) は ±2 半音で -10.7 セントの誤差があるが、
 *        3次 Taylor では ±2 半音で -0.012 セント、±4 半音でも -0.17 セント。
 *        powf をサンプル毎に呼ぶコストなく数学的に十分な精度を得る。
 */
static inline float sub_semitone_ratio(float st)
{
    const float a = 0.0577622650f;   /* ln(2)/12 */
    float ax = a * st;
    return 1.0f + ax * (1.0f + ax * (0.5f + ax * (1.0f / 6.0f)));
}

/**
 * @brief 等価パワー パンニング (pan: 0=L .. 1=R)
 *        センターで +3dB 膨らむ線形則と異なり、どの位置でもエネルギー一定
 */
static inline void sub_equal_power_pan(float pan, float *l, float *r)
{
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    float ang = pan * ((float)M_PI * 0.5f);
    *l = cosf(ang);
    *r = sinf(ang);
}

/* ========================================================================= */
/* 5. Biquad フィルタ (5バンド EQ 用)                                       */
/* ========================================================================= */
typedef enum {
    BIQUAD_LOWSHELF,
    BIQUAD_PEAKING,
    BIQUAD_HIGHSHELF,
    BIQUAD_LOW_PASS,
    BIQUAD_HIGH_PASS
} BiquadFilterType;

typedef struct {
    float b0, b1, b2, a1, a2;
    float s1, s2; /**< Transposed Direct Form 2 状態変数
                       (単精度 DF1 の桁落ち/低域量子化ノイズを根絶) */
} BiquadFilter;

static inline void biquad_init(BiquadFilter *f)
{
    memset(f, 0, sizeof(BiquadFilter));
    f->b0 = 1.0f;
}

static inline void biquad_calc_coeffs(BiquadFilter *f, BiquadFilterType type, float f0, float gain_db, float q, float fs)
{
    /* 入力ガード: q<=0 や f0 が範囲外だと alpha/a0 が NaN/Inf/負になり、
     * DF1 の状態 y1/y2 に NaN が取り込まれると永久に復帰しない
     * (SVF 側 sub_svf_set と同一思想のクランプ) */
    if (!(q > 0.02f)) q = 0.02f;          /* NaN/ゼロ/負を一括捕捉 */
    if (!(f0 > 20.0f)) f0 = 20.0f;
    if (!(fs > 0.0f)) fs = (float)SUB_SAMPLE_RATE;
    if (f0 > 0.45f * fs) f0 = 0.45f * fs; /* ナイキスト超えの極安定化 */
    if (!(gain_db > -48.0f && gain_db < 48.0f)) {
        gain_db = (gain_db > 0.0f) ? 24.0f : -24.0f;
        if (!(gain_db > -48.0f && gain_db < 48.0f)) gain_db = 0.0f; /* NaN -> 0 */
    }

    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * q);
    float a0 = 1.0f;

    switch (type) {
        case BIQUAD_LOWSHELF: {
            float two_sqrt_a_alpha = 2.0f * sqrtf(A) * alpha;
            f->b0 = A * ((A + 1.0f) - (A - 1.0f) * cos_w + two_sqrt_a_alpha);
            f->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w);
            f->b2 = A * ((A + 1.0f) - (A - 1.0f) * cos_w - two_sqrt_a_alpha);
            a0    = (A + 1.0f) + (A - 1.0f) * cos_w + two_sqrt_a_alpha;
            f->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w);
            f->a2 = (A + 1.0f) + (A - 1.0f) * cos_w - two_sqrt_a_alpha;
            break;
        }
        case BIQUAD_PEAKING: {
            f->b0 = 1.0f + alpha * A;
            f->b1 = -2.0f * cos_w;
            f->b2 = 1.0f - alpha * A;
            a0    = 1.0f + alpha / A;
            f->a1 = -2.0f * cos_w;
            f->a2 = 1.0f - alpha / A;
            break;
        }
        case BIQUAD_HIGHSHELF: {
            float two_sqrt_a_alpha = 2.0f * sqrtf(A) * alpha;
            f->b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w + two_sqrt_a_alpha);
            f->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w);
            f->b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w - two_sqrt_a_alpha);
            a0    = (A + 1.0f) - (A - 1.0f) * cos_w + two_sqrt_a_alpha;
            f->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w);
            f->a2 = (A + 1.0f) - (A - 1.0f) * cos_w - two_sqrt_a_alpha;
            break;
        }
        case BIQUAD_LOW_PASS: {
            f->b0 = (1.0f - cos_w) * 0.5f;
            f->b1 = 1.0f - cos_w;
            f->b2 = (1.0f - cos_w) * 0.5f;
            a0    = 1.0f + alpha;
            f->a1 = -2.0f * cos_w;
            f->a2 = 1.0f - alpha;
            break;
        }
        case BIQUAD_HIGH_PASS: {
            f->b0 = (1.0f + cos_w) * 0.5f;
            f->b1 = -(1.0f + cos_w);
            f->b2 = (1.0f + cos_w) * 0.5f;
            a0    = 1.0f + alpha;
            f->a1 = -2.0f * cos_w;
            f->a2 = 1.0f - alpha;
            break;
        }
    }

    /* 正規化 */
    float inv_a0 = 1.0f / a0;
    f->b0 *= inv_a0;
    f->b1 *= inv_a0;
    f->b2 *= inv_a0;
    f->a1 *= inv_a0;
    f->a2 *= inv_a0;
}

static inline float biquad_process(BiquadFilter *f, float in)
{
    /* Transposed Direct Form 2: 状態 2 個 + FMA で構成し、
     * 単精度 DF1 の桁落ち (特に低域) を構造的に排除する */
    float out = fmaf(in, f->b0, f->s1);
    f->s1 = fmaf(in, f->b1, f->s2) - f->a1 * out;
    f->s2 = in * f->b2 - f->a2 * out;
    if (!sub_isfinite_f(out)) {
        f->s1 = 0.0f;
        f->s2 = 0.0f;
        return 0.0f;
    }
    return out;
}

/* ホットパス専用チェックレス版 (Sub5毎サンプル8回→0分岐)。
 * FZ+DN有効 + 係数init時ガード済みのためNaN混入は構造的に起きない。
 * 万一NaNが入っても次ブロック先頭のDC/係数チェックで復帰する */
static inline float biquad_process_fast(BiquadFilter *f, float in)
{
    float out = fmaf(in, f->b0, f->s1);
    f->s1 = fmaf(in, f->b1, f->s2) - f->a1 * out;
    f->s2 = in * f->b2 - f->a2 * out;
    return out;
}

/**
 * @brief Cortex-M4F FPU デノーマル対策: Flush-to-Zero (FZ) + Default NaN (DN)
 * @details 全ワーカーコアで呼び出すこと。FZ 無効のままリリース尾やフィルタ
 *          状態が非正規化数域へ落ちると、演算毎に例外ペナルティが発生し
 *          タイミング揺らぎ (じりじりノイズ) の原因になる。
 *          ホストビルド (__arm__ 以外) では何もしない。
 */
static inline void sub_fpu_denormal_init(void)
{
#if defined(__arm__) || defined(__ARM_ARCH)
#  if defined(__GNUC__) || defined(__clang__)
    uint32_t fpscr;
    __asm__ volatile(
        "vmrs %0, fpscr\n"
        "orr  %0, %0, #(3 << 24)\n" /* bit 24 (FZ) | bit 25 (DN) */
        "vmsr fpscr, %0\n"
        : "=r"(fpscr)
        :
        : "memory"
    );
#  endif
#endif
}

/* ========================================================================= */
/* 5b. TPT State Variable Filter — ペルボイス 共振ローパス                    */
/*      台形積分の陰形式を毎サンプル厳密に解く方式 (bilinear 変換と等価、     */
/*      理論上無条件安定・LP の DC ゲインは厳密に 1.0)。                     */
/*      旧 ic 更新式は係数誤り (a2 を g と取り違え) により持続入力下で       */
/*      発散する数学的欠陥があったため全面改訂した                           */
/* ========================================================================= */
typedef struct {
    float g, k;              /* カットオフ係数 / ダンピング (=1/Q) */
    float inv_d;             /* 1/(1+g(k+g)) */
    float c_1mgg;            /* 1-g^2 */
    float s1, s2;            /* 積分器状態 (BP 出力 / LP 出力) */
    float u1p;               /* 前サンプルの BP 入力 */
} SubSvf;

static inline void sub_svf_reset(SubSvf *f)
{
    f->s1 = 0.0f;
    f->s2 = 0.0f;
    f->u1p = 0.0f;
}

/* ========================================================================= */
/* 5b-1. 数学関数の小型 LUT (expf / tanf のホットパス置換用)                   */
/* --------------------------------------------------------------------------- */
/* 設計方針:
 *  - 各 LUT は「明示初期化したワーカーのみ」使用する。初期化されていない
 *    場合 (ホストのユニットテスト等) は自動的に厳密関数へフォールバックし、
 *    数値リファレンス試験 (math_tests) の精度を保証する
 *  - ワーカーはコア毎に独立リンクされるため、static テーブルはコア単位で
 *    一つずつ存在する (シングルスレッド前提のためロック不要)               */

/* ---- expf(-x) 近似: 512 要素 + 線形補間 (x ∈ [0,16], -80dB 打ち切り) ----
 * 256->512 (+1KB/ワーカー)。ADSR/スイープ係数の補間誤差を半減し、
 * 参照コストは同一 (FMA 1回)。メモリで音質を買う */
#define SUB_EXP_LUT_SIZE  (512)
#define SUB_EXP_LUT_SPAN  (16.0f)
#ifndef SUB_COMMON_NO_LUT
static float s_sub_exp_lut[SUB_EXP_LUT_SIZE + 1];
static int   s_sub_exp_lut_ready = 0;

/** @brief exp LUT 初期化 (各ワーカーの engine_init から 1 回呼ぶ) */
static inline void sub_exp_lut_init(void)
{
    if (s_sub_exp_lut_ready) return;
    for (int i = 0; i <= SUB_EXP_LUT_SIZE; i++) {
        float x = SUB_EXP_LUT_SPAN * (float)i / (float)SUB_EXP_LUT_SIZE;
        s_sub_exp_lut[i] = expf(-x);
    }
    s_sub_exp_lut_ready = 1;
}

/**
 * @brief expf(-x) の高速近似 (x >= 0)。ADSR/スイープ係数算出用
 *        未初期化時は厳密 expf へフォールバック (テスト互換)
 */
static inline float sub_exp_approx(float neg_arg)
{
    if (!s_sub_exp_lut_ready) return expf(-neg_arg);
    if (!(neg_arg > 0.0f)) return 1.0f;                 /* 0 / NaN -> 1.0 */
    if (neg_arg >= SUB_EXP_LUT_SPAN) return s_sub_exp_lut[SUB_EXP_LUT_SIZE];
    float fidx = neg_arg * ((float)SUB_EXP_LUT_SIZE / SUB_EXP_LUT_SPAN);
    int i = (int)fidx;
    float fr = fidx - (float)i;
    return s_sub_exp_lut[i] + (s_sub_exp_lut[i + 1] - s_sub_exp_lut[i]) * fr;
}
#else
static inline void sub_exp_lut_init(void) {}
static inline float sub_exp_approx(float neg_arg)
{
    if (!(neg_arg > 0.0f)) return 1.0f;
    return expf(-neg_arg);
}
#endif

/* ---- tan(π fc/fs) 近似: 対数分割 128 要素 + 線形補間 --------------------- */
/* SVF の g = tan(π·fc/fs) をカットオフ追従 (64 サンプル毎) のたびに求める。
 * インデックスは IEEE754 ビット抽出による高速 log2 で計算する
 * (tanf ~100cyc -> LUT 参照 ~10cyc。g 誤差は最大 ~1% だが音感・安定性への
 *  影響は無視できる。DC ゲインは g に非依存で常に 1.0)                      */
#define SUB_TAN_LUT_SIZE  (1024)
#define SUB_TAN_FC_MIN    (60.0f)      /* sub_svf_set のクランプ下限と一致 */
#define SUB_TAN_FC_MAX    (21600.0f)   /* 同 上限 (0.45 x 48kHz) */
#ifndef SUB_COMMON_NO_LUT
static float s_sub_tan_lut[SUB_TAN_LUT_SIZE + 1];
static float s_sub_tan_lut_fs = 0.0f;
#endif

/** @brief float <-> ビット列変換 (union 型パニングを使わない移植安全版)。
 *  memcpy(4) はコンパイラによりレジスタ転送に最適化される。
 *  ※ union 経由のビット読みは MSVC /O2 のインライン展開下で
 *    最適化により壊れるケースを確認したため、厳密動作する本方式を採用 */
static inline uint32_t sub_f32_to_bits(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

/** @brief 高速 log2(x): 指数部ビット抽出 + p 変換級数 (誤差 ~0.3%) */
static inline float sub_log2_approx(float x)
{
    uint32_t bits = sub_f32_to_bits(x);
    int32_t e = (int32_t)(bits >> 23) - 127;
    /* 仮数部 [1,2) への復元は「整数値 x 2^-23」の算術変換で行う。
     * (ビット列への指数 127 再書き込み + float 解釈は誤り — 指数フィールドが
     *  ほぼゼロの極小値になり p -> -1 へ発散する) */
    uint32_t mbits = (bits & 0x007FFFFFu) | 0x00800000u;
    float m = (float)mbits * (1.0f / 8388608.0f);        /* -> [1,2) */
    float p = (m - 1.0f) / (m + 1.0f);                   /* atanh 展開用 */
    /* ln(1+p) ≈ 2p(1 + p²/3) -> log2 = ... / ln2 (= x2.88539) */
    return (float)e + 2.885390f * p * (1.0f + 0.333333f * p * p);
}

#ifndef SUB_COMMON_NO_LUT
/** @brief tan LUT 初期化 (fs 固定。各ワーカーの engine_init から呼ぶ) */
static inline void sub_tan_lut_init(float fs)
{
    if (s_sub_tan_lut_fs == fs && s_sub_tan_lut_fs > 0.0f) return;
    float lo = sub_log2_approx(SUB_TAN_FC_MIN);
    float span = sub_log2_approx(SUB_TAN_FC_MAX) - lo;
    for (int i = 0; i <= SUB_TAN_LUT_SIZE; i++) {
        float lg = lo + span * (float)i / (float)SUB_TAN_LUT_SIZE;
        float fc = expf(lg * 0.69314718056f); /* exp2f(lg) 互換 (Newlib-nano リンク対応) */
        s_sub_tan_lut[i] = tanf((float)M_PI * fc / fs);
    }
    s_sub_tan_lut_fs = fs;
}

static inline void sub_svf_set(SubSvf *f, float cutoff_hz, float q, float fs)
{
    if (!(fs > 0.0f)) fs = (float)SUB_SAMPLE_RATE;
    if (!(cutoff_hz >= SUB_TAN_FC_MIN && cutoff_hz <= 0.45f * fs)) {
        cutoff_hz = (cutoff_hz > 0.45f * fs) ? 0.45f * fs : SUB_TAN_FC_MIN;
        if (!(cutoff_hz >= SUB_TAN_FC_MIN)) cutoff_hz = SUB_TAN_FC_MIN; /* NaN -> min */
    }
    if (!(q > 0.02f)) q = 0.02f;   /* q<=0 / NaN による 1/q 暴走を禁止 (Q<=50) */
    float g;
    if (s_sub_tan_lut_fs == fs) {
        /* LUT 経路 (ワーカー): log2 近似 -> 対数インデックス -> 線形補間 */
        float lo = sub_log2_approx(SUB_TAN_FC_MIN);
        float span = sub_log2_approx(SUB_TAN_FC_MAX) - lo;
        float fidx = (sub_log2_approx(cutoff_hz) - lo) *
                     ((float)SUB_TAN_LUT_SIZE / span);
        if (fidx < 0.0f) fidx = 0.0f;
        if (fidx >= (float)SUB_TAN_LUT_SIZE) fidx = (float)SUB_TAN_LUT_SIZE - 0.001f;
        int i = (int)fidx;
        float fr = fidx - (float)i;
        g = s_sub_tan_lut[i] + (s_sub_tan_lut[i + 1] - s_sub_tan_lut[i]) * fr;
    } else {
        /* 厳密経路 (未初期化: ホストテスト等) */
        g = tanf((float)M_PI * cutoff_hz / fs);
    }
    f->g = g;
    f->k = 1.0f / q;
    float d = 1.0f + g * (f->k + g);
    f->inv_d = 1.0f / d;
    f->c_1mgg = 1.0f - g * g;
}
#else
static inline void sub_tan_lut_init(float fs) { (void)fs; }
static inline void sub_svf_set(SubSvf *f, float cutoff_hz, float q, float fs)
{
    if (!(fs > 0.0f)) fs = (float)SUB_SAMPLE_RATE;
    if (!(cutoff_hz >= SUB_TAN_FC_MIN && cutoff_hz <= 0.45f * fs)) {
        cutoff_hz = (cutoff_hz > 0.45f * fs) ? 0.45f * fs : SUB_TAN_FC_MIN;
        if (!(cutoff_hz >= SUB_TAN_FC_MIN)) cutoff_hz = SUB_TAN_FC_MIN;
    }
    if (!(q > 0.02f)) q = 0.02f;
    float g = tanf((float)M_PI * cutoff_hz / fs);
    f->g = g;
    f->k = 1.0f / q;
    float d = 1.0f + g * (f->k + g);
    f->inv_d = 1.0f / d;
    f->c_1mgg = 1.0f - g * g;
}
#endif

/* ローパス出力 1 サンプル処理 (Cortex-M4F VFMA 3連結合最適化) */
static inline float sub_svf_lp(SubSvf *f, float x)
{
    float diff = x + f->u1p - f->s2;
    float num  = fmaf(f->g, diff, f->s1 * f->c_1mgg);
    float bp   = num * f->inv_d;
    float lp   = fmaf(f->g, bp + f->s1, f->s2);
    float u1p  = fmaf(-f->k, bp, x - lp);
    /* Cortex-M4F FPU Flush-to-Zero (FZ) 有効化済みのハードウェア保証により毎サンプルの NaN チェックを全廃 */
    f->u1p     = u1p;
    f->s1      = bp;
    f->s2      = lp;
    return lp;
}

/* ハイパス出力 1 サンプル処理 (SVF 多モード出力: HP = x − k·BP − LP、3連 FMA 最適化)
 * ラムブル除去用の直列 HP ステージはこちらを使用する */
static inline float sub_svf_hp(SubSvf *f, float x)
{
    float diff = x + f->u1p - f->s2;
    float num  = fmaf(f->g, diff, f->s1 * f->c_1mgg);
    float bp   = num * f->inv_d;
    float lp   = fmaf(f->g, bp + f->s1, f->s2);
    float hp   = fmaf(-f->k, bp, x - lp);
    if (!sub_isfinite_f(hp)) {
        sub_svf_reset(f);
        return 0.0f;
    }
    f->u1p     = hp;
    f->s1      = bp;
    f->s2      = lp;
    return hp;
}

/* ========================================================================= */
/* 5c. DC ブロッカー (DC オフセット除去)                                      */
/* ========================================================================= */
typedef struct {
    float x1, y1;
    float r;                 /* 極 (0.995 推奨 @48kHz) */
} SubDcBlocker;

static inline void sub_dc_reset(SubDcBlocker *d, float r)
{
    d->x1 = d->y1 = 0.0f;
    d->r = r;
}

static inline float sub_dc_process_fast(SubDcBlocker *d, float x)
{
    /* ホットパス専用: isfinite 2回→0分岐。FZ+DNでデノーマルHW flush、
     * 入力は sub2/3/4 バス(有限保証)のみのためチェック不要 */
    float y = x - d->x1 + d->r * d->y1;
    if (fabsf(y) < 1e-20f) y = 0.0f;
    d->x1 = x;
    d->y1 = y;
    return y;
}

static inline float sub_dc_process(SubDcBlocker *d, float x)
{
    if (!sub_isfinite_f(x)) {
        d->x1 = 0.0f;
        d->y1 = 0.0f;
        return 0.0f;
    }
    float y = x - d->x1 + d->r * d->y1;
    if (!sub_isfinite_f(y)) {
        d->x1 = 0.0f;
        d->y1 = 0.0f;
        return 0.0f;
    }
    /* 長時間無音時に状態がデノーマル数へ沈み込むのを防止。
     * (Cortex-M4F はデノーマルを HW 処理するが、x86 ホストや
     *  厳密 IEEE 環境でのマイクロコード例外遅延を避ける) */
    if (fabsf(y) < 1e-20f) y = 0.0f;
    d->x1 = x;
    d->y1 = y;
    return y;
}


/* ========================================================================= */
/* 5d. 高分解能タイマ & CPU 負荷メーター (実測ビジー時間比率)                 */
/* ========================================================================= */
#ifdef _WIN32
#include <windows.h>
static inline uint64_t sub_get_ns(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000000ull / f.QuadPart);
}
#define SUB_SLEEP_US(us) Sleep((DWORD)((us) / 1000))
#else
#include <unistd.h>
#include <time.h>
static inline uint64_t sub_get_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define SUB_SLEEP_US(us) usleep(us)
#endif

/**
 * @brief CPU 負荷メーター (ウィンドウ内ビジー時間 / 実経過時間)
 *        使い方: 各反復の作業部を SUB_LOAD_BUSY_BEGIN/END で囲み、
 *                ループ末尾で SUB_LOAD_TICK(core) を呼ぶ (64 反復ごとに更新)
 */
typedef struct {
    uint64_t busy_ns;
    uint64_t win_start;
    uint32_t div;
} SubLoadMeter;

#define SUB_LOAD_INIT(m) do { \
    (m).busy_ns = 0; (m).div = 0; (m).win_start = sub_get_ns(); \
} while (0)

#define SUB_LOAD_BUSY_BEGIN(m) uint64_t _lb_##m = sub_get_ns()
#define SUB_LOAD_BUSY_END(m)   (m).busy_ns += sub_get_ns() - _lb_##m

#define SUB_LOAD_TICK(m, shared_ptr, core_idx) do { \
    if (++(m).div >= 64) { \
        uint64_t now = sub_get_ns(); \
        uint64_t wall = now - (m).win_start; \
        (shared_ptr)->core[core_idx].cpu_load = \
            (wall > 0) ? (uint16_t)((m).busy_ns * 1000ull / wall) : 0u; \
        (m).busy_ns = 0; (m).win_start = now; (m).div = 0; \
    } \
} while (0)

/**
 * @brief エポック処理時間の計測結果を共有メモリへ EMA 平滑で公開 (µs)
 *        使い方: 処理ブロック先頭で t0=sub_get_ns() を取り、
 *                ブロック末尾で SUB_EPOCH_TIME_UPDATE(shared, core, t0) を呼ぶ。
 *        完全同期パイプラインでは max(render_busy_us[]) がフレームレートを決める
 */
#define SUB_EPOCH_TIME_UPDATE(shared_ptr, core_idx, t0_ns) do { \
    uint64_t _et_now = sub_get_ns(); \
    uint32_t _et_us = (_et_now > (uint64_t)(t0_ns)) \
        ? (uint32_t)((_et_now - (uint64_t)(t0_ns)) / 1000ull) : 0u; \
    uint32_t _et_prev = (shared_ptr)->core[core_idx].render_busy_us; \
    (shared_ptr)->core[core_idx].render_busy_us = \
        (((_et_prev << 3) - _et_prev) + _et_us) >> 3; \
    asmp_dcache_clean((const void *)&(shared_ptr)->core[core_idx].render_busy_us, sizeof((shared_ptr)->core[core_idx].render_busy_us)); \
} while (0)

/* ========================================================================= */
/* 5e. ウェーブテーブル・オシレータ (8テーブル×6ミップ, モルフォ対応)          */
/* ========================================================================= */
#define SUBWT_TABLES  (8)
/* 128->256 (+24.6KB Flash ROM、+8KB Sub2 premix BSS)。WT補間刻み半減で
 * 高域のざらつき・ユニゾンうなり質感が向上。演算数は同一のためCPU不変 */
#define SUBWT_SIZE    (256)
#define SUBWT_MIPS    (6)

typedef struct {
    float table[SUBWT_MIPS][SUBWT_TABLES][SUBWT_SIZE + 1];
} SubWavBank;

/* ROM ウェーブテーブルバンク (asmp_sub/sub_wavbank.c。tools/gen_wavbank.c 生成)。
 * 実行時 subwav_init (RAM 24.8KB + 起動時 sinf 数万回) の置換。SubWavBank 型と
 * 同一レイアウト (g_sub_wavbank[mip][table][sample]) の const 配置 (Flash常駐)。
 * subwav_init() 自体はテスト・検証用に残す */
extern const float g_sub_wavbank[SUBWT_MIPS][SUBWT_TABLES][SUBWT_SIZE + 1];

/**
 * @brief 加算合成により 8 種類のウェーブテーブルを生成 (帯域制限付き 6 ミップ)
 *        ミップは倍音数の異なる「明るい順」のラダー。
 *        発音周波数から subwt_pick_mip() で選択し、全音域で
 *        最高次倍音がエイリアス限界以下になるよう保証する。
 */
static inline void subwav_init(SubWavBank *bank)
{
    /* 各テーブルの倍音スペクトル定義 (index1..15 の振幅) */
    static const float spec[SUBWT_TABLES][16] = {
        { 0, 1,.50f,.33f,.25f,.20f,.17f,.14f,.125f,.11f,.10f,.09f,.083f,.077f,.071f,.067f }, // 0 Analog Saw
        { 0, 1, 0,.333f,0,.20f,0,.143f,0,.111f,0,.091f,0,.077f,0,.067f },             // 1 Hollow (sq系)
        { 0, 1,.85f,.45f,.30f,.65f,.15f,.42f,.10f,.28f,.08f,.20f,.06f,.15f,.05f,.12f },      // 2 Pulse 20%
        { 0,.80f,.55f, 0,.90f, 0, .35f, 0,.60f, 0,.25f, 0,.45f, 0,.15f, 0 },           // 3 Drawbar Organ
        { 0,1.0f,.30f,.85f,.12f,.45f,.60f,.08f,.30f,.05f,.22f,.40f,.04f,.15f,.03f,.10f },     // 4 Bell
        { 0,.95f,.20f,.15f,.75f,.10f,.08f,.50f,.06f,.05f,.35f,.04f,.03f,.25f,.02f,.02f },     // 5 E.Piano/Tine
        { 0,.70f,.95f,.40f,.85f,.30f,.70f,.20f,.55f,.15f,.40f,.12f,.30f,.10f,.22f,.08f },     // 6 Vox Formant
        { 0,1.0f,.10f,.60f,.08f,.40f,.06f,.30f,.05f,.20f,.04f,.15f,.03f,.12f,.02f,.10f },     // 7 Digital
    };
    static const uint8_t mip_harmonics[SUBWT_MIPS] = { 15, 10, 6, 4, 2, 1 };

    for (int mip = 0; mip < SUBWT_MIPS; mip++) {
        int hmax = mip_harmonics[mip];
        for (int t = 0; t < SUBWT_TABLES; t++) {
            for (int n = 0; n < SUBWT_SIZE; n++) {
                float ph = (float)n / (float)SUBWT_SIZE;
                float acc = 0.0f;
                for (int h = 1; h <= hmax && h < 16; h++) {
                    acc += spec[t][h] * sinf(2.0f * (float)M_PI * (float)h * ph);
                }
                bank->table[mip][t][n] = acc;
            }
            bank->table[mip][t][SUBWT_SIZE] = bank->table[mip][t][0]; /* ガード */
            /* 正規化 */
            float peak = 0.0f;
            for (int n = 0; n < SUBWT_SIZE; n++)
                if (fabsf(bank->table[mip][t][n]) > peak) peak = fabsf(bank->table[mip][t][n]);
            if (peak > 0.0001f) {
                float inv = 1.0f / peak;
                for (int n = 0; n <= SUBWT_SIZE; n++) bank->table[mip][t][n] *= inv;
            }
        }
    }
}

/**
 * @brief 発音周波数に応じたミップ選択 (note-on 時 1 回のみ呼ぶ)
 *        最高次倍音が SUBWT_ALIAS_LIMIT_HZ を超えない「最も明るい」ミップを
 *        選ぶことで全音域エイリアスフリーを保証する。
 *        ランタイムコストゼロ (分岐数回のみ)。
 */
#define SUBWT_ALIAS_LIMIT_HZ (20000.0f)
static inline uint8_t subwt_pick_mip(float freq)
{
    static const uint8_t harmonics[SUBWT_MIPS] = { 15, 10, 6, 4, 2, 1 };
    for (int m = 0; m < SUBWT_MIPS; m++) {
        if ((float)harmonics[m] * freq <= SUBWT_ALIAS_LIMIT_HZ) {
            return (uint8_t)m;
        }
    }
    return (uint8_t)(SUBWT_MIPS - 1);
}

/**
 * @brief ウェーブテーブル読み出し (テーブル間モルフ + 線形補間)
 * @param morph テーブル位置 0..SUBWT_TABLES-1 (小数で隣接テーブルをクロスフェード)
 * @param mip   0=通常 / 1=高音域用ダークミップ
 * @param phase 正規化位相。呼び出し側で [0,1) にラップしておくこと
 *              (オシレータは毎サンプル手動ラップ済みのため floorf 不要)。
 *              整数モルフ位置 (tf==0) の頻出ケースは単表 fast path を通す
 */
static inline float subwt_read(const SubWavBank *bank, float morph, int mip, float phase)
{
    if (mip < 0) mip = 0;
    if (mip >= SUBWT_MIPS) mip = SUBWT_MIPS - 1;

    /* 位相 -> ビン番号: floorf/条件分岐なしの int キャスト+マスク。
     * phase∈[0,1) では (int)(phase*SIZE) ∈ [0,SIZE]、境界 1.0 はマスクで 0 へ折返 */
    float fidx = phase * (float)SUBWT_SIZE;
    int i0i = (int)fidx;
    uint32_t i0 = (uint32_t)i0i & (SUBWT_SIZE - 1u);
    uint32_t i1 = (i0 + 1u) & (SUBWT_SIZE - 1u);
    float fr = fidx - (float)i0i;

    int morph_i = (int)morph;
    if (morph_i < 0) morph_i = 0;
    if (morph_i > SUBWT_TABLES - 1) morph_i = SUBWT_TABLES - 1;
    float morph_f = morph - (float)morph_i;
    if (morph_f < 0.0f) morph_f = 0.0f;

    const float (*T)[SUBWT_SIZE + 1] = bank->table[mip];
    float va = T[morph_i][i0] * (1.0f - fr) + T[morph_i][i1] * fr;

    /* 整数モルフ位置 (GM マップの大半) は単表で完結する fast path */
    if (morph_f == 0.0f || morph_i >= SUBWT_TABLES - 1) {
        return va;
    }
    float vb = T[morph_i + 1][i0] * (1.0f - fr) + T[morph_i + 1][i1] * fr;
    return va * (1.0f - morph_f) + vb * morph_f;
}

/* GM プログラム -> ウェーブテーブル割当 (モルフ位置) */

/**
 * @brief Q32 位相直接読み出し (SuperSaw ホットパス専用)
 * @param ph Q32 位相 (0x100000000 で 1 周期。32bit 自然オーバーフロー = ラップ)
 * @param ta/tb モルフ両端テーブル番号、tw B 側ミックス比 (note-on 時に確定済み)
 *
 * float 位相方式の「加算 -> 1.0 比較分岐 -> int 変換」を排除し、
 * インデックス抽出をシフト 1 命令 (+小数部マスク) で行う。
 * SUBWT_SIZE==256 前提: 上位 8bit=bin, 下位24bit=補間係数
  */
#if SUBWT_SIZE != 256
#error "subwt_read_q32 は SUBWT_SIZE == 256 前提のビット配置です"
#endif
static inline float subwt_read_q32(const SubWavBank *bank, int mip,
                                   uint32_t ta, uint32_t tb, float tw, uint32_t ph)
{
    const float (*T)[SUBWT_SIZE + 1] = bank->table[mip];
    uint32_t i0 = ph >> 24;                       /* 上位 8bit -> bin [0,255] */
    uint32_t i1 = (i0 + 1u) & (uint32_t)(SUBWT_SIZE - 1u); /* ガード点へ折返し */
    /* 小数部 24bit フル精度 */
    float fr = (float)(ph & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    const float *Ta = T[ta];
    float va = Ta[i0] * (1.0f - fr) + Ta[i1] * fr;
    if (tb != ta) {
        const float *Tb = T[tb];
        float vb = Tb[i0] * (1.0f - fr) + Tb[i1] * fr;
        va += (vb - va) * tw;
    }
    return va;
}

/**
 * @brief Q32 位相直接読み出し (単一ウェーブテーブル専用・モルフなし高速パス)
 * @param ph Q32 位相 (0x100000000 で 1 周期。32bit 自然オーバーフロー = ラップ)
 * @param ta テーブル番号
 */
static inline float subwt_read_q32_single(const SubWavBank *bank, int mip,
                                          uint32_t ta, uint32_t ph)
{
    const float (*T)[SUBWT_SIZE + 1] = bank->table[mip];
    uint32_t i0 = ph >> 24;                       /* 上位 8bit -> bin [0,255] */
    uint32_t i1 = (i0 + 1u) & (uint32_t)(SUBWT_SIZE - 1u); /* ガード点へ折返し */
    /* 小数部 24bit フル精度 (単表 fast path も同様) */
    float fr = (float)(ph & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    const float *Ta = T[ta];
    return Ta[i0] * (1.0f - fr) + Ta[i1] * fr;
}

/**
 * @brief Q32 位相直接読み出し (プレミックス済みフラットテーブル専用)
 * @details ノートオン時に A/B テーブルをブレンドした単一テーブルを読む。
 *          毎サンプル 2 表読みだったモルフ経路を 1 表読みに削減する (#13)
 */
static inline float subwt_read_raw_q32(const float *T, uint32_t ph)
{
    uint32_t i0 = ph >> 24;
    uint32_t i1 = (i0 + 1u) & (uint32_t)(SUBWT_SIZE - 1u);
    float fr = (float)(ph & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    /* Cortex-M4F VFMA.F32 (単一FMA 2サイクル): 乗算2回+加減算を単一命令へ集約 */
    return fmaf(T[i1] - T[i0], fr, T[i0]);
}
static inline float subwt_program_to_morph(uint8_t program)
{
    if (program < 8)    return 5.0f;   /* Piano     -> E.Piano/Tine */
    if (program < 16)   return 4.0f;   /* Chromatic -> Bell */
    if (program < 24)   return 3.0f;   /* Organ     -> Drawbar */
    if (program < 32)   return 2.0f;   /* Guitar    -> Pulse */
    if (program < 40)   return 1.0f;   /* Bass      -> Hollow */
    if (program < 48)   return 6.0f;   /* Strings   -> Vox */
    if (program < 56)   return 7.0f;   /* Ensemble  -> Digital */
    if (program < 64)   return 0.5f;   /* Brass     -> Saw 寄り */
    if (program < 72)   return 6.5f;
    return 0.0f;                        /* Lead/Synth -> Analog Saw */
}

/* ========================================================================= */
/* 5g. 共通 ADSR エンベロープコア & MIDIチャンネル状態                         */
/*     旧 sub2/sub3 の重複実装を集約したもの。各ボイスは SubEnvCore を        */
/*     メンバ env として埋め込み、sub_env_* 関数で操作する                    */
/* ========================================================================= */
typedef enum {
    SUB_ENV_IDLE = 0,
    SUB_ENV_ATTACK,
    SUB_ENV_DECAY,
    SUB_ENV_SUSTAIN,
    SUB_ENV_RELEASE
} SubEnvState;

typedef struct {
    float attack_time_sec;
    float decay_time_sec;
    float sustain_level;
    float release_time_sec;
    bool  exponential_decay;
} SubAdsrParams;

/** MIDI チャンネルの共通状態 (CC 反映値) */
typedef struct {
    uint8_t program;
    float   volume;               /* CC#7 */
    float   expression;           /* CC#11 */
    float   pan;                  /* CC#10 (0=L .. 1=R) */
    float   pan_gain_l;           /* pan の等価パワーゲイン (cosf) キャッシュ。CC#10 変更時のみ再計算 */
    float   pan_gain_r;           /* 〃 (sinf) キャッシュ */
    float   pitch_bend_semitones;
    float   mod_depth;            /* CC#1 (0..1) */
    float   reverb_send;          /* CC#91 (0..1) チャンネル別センド */
    bool    sustain_pedal;        /* CC#64 */
} SubChannel;

/**
 * @brief チャンネルの等価パワー パンゲインを再計算してキャッシュに格納
 *        (synth_engine.c の engine_update_pan_gains と同じ狙い: レンダーループ内の
 *        per-block cosf/sinf を排除する。CC#10 受信時とチャンネル初期化時のみ呼ぶ)
 */
static inline void sub_channel_update_pan_gains(SubChannel *ch)
{
    float pan = ch->pan;
    if (!(pan >= 0.0f)) pan = 0.0f;   /* NaN/負値ガード */
    if (pan > 1.0f) pan = 1.0f;
    if (pan <= 0.0f) { ch->pan_gain_l = 1.0f; ch->pan_gain_r = 0.0f; return; }
    if (pan >= 1.0f) { ch->pan_gain_l = 0.0f; ch->pan_gain_r = 1.0f; return; }
    float ang = pan * ((float)M_PI * 0.5f);
    ch->pan_gain_l = cosf(ang);
    ch->pan_gain_r = sinf(ang);
}

/** ボイスのエンベロープ コア (VoiceSub2/VoiceSub3 のメンバ env として埋め込み) */
typedef struct {
    SubEnvState   env_state;
    bool          sustained_by_pedal; /* CC#64 によりリリース延期中 */
    float         current_env_level;
    float         release_start_level;
    float         attack_step;
    float         decay_step;
    float         release_step;
    float         decay_coeff;
    float         release_coeff;
    uint32_t      env_samples;
    uint32_t      phase_max_samples;
    SubAdsrParams adsr;
} SubEnvCore;

/**
 * @brief ADSR 係数を算出し ATTACK (または即 DECAY) から発音開始
 */
static inline void sub_env_prepare_attack(SubEnvCore *c)
{
    uint32_t attack_samples = (uint32_t)(c->adsr.attack_time_sec * (float)SUB_SAMPLE_RATE);
    uint32_t decay_samples  = (uint32_t)(c->adsr.decay_time_sec * (float)SUB_SAMPLE_RATE);
    c->attack_step = (attack_samples > 0) ? (1.0f / (float)attack_samples) : 1.0f;
    float decay_diff = 1.0f - c->adsr.sustain_level;
    c->decay_step = (decay_samples > 0) ? (decay_diff / (float)decay_samples) : decay_diff;
    c->decay_coeff = (decay_samples > 0) ? sub_exp_approx(6.907755f / (float)decay_samples) : 0.0f;

    if (attack_samples > 0) {
        c->current_env_level = 0.0f;
        c->env_samples = 0;
        c->phase_max_samples = attack_samples;
        c->env_state = SUB_ENV_ATTACK;
    } else if (decay_samples > 0) {
        /* attack=0: ピーク 1.0 から即 DECAY */
        c->current_env_level = 1.0f;
        c->env_samples = 0;
        c->phase_max_samples = decay_samples;
        c->env_state = SUB_ENV_DECAY;
    } else {
        c->current_env_level = c->adsr.sustain_level;
        c->env_samples = 0;
        c->phase_max_samples = 0;
        c->env_state = SUB_ENV_SUSTAIN;
    }
    c->sustained_by_pedal = false;
}

/**
 * @brief リリース開始 (現在レベルから release_time_sec で減衰)
 */
static inline void sub_env_begin_release(SubEnvCore *c)
{
    if (c->env_state == SUB_ENV_RELEASE) {
        return;
    }
    if (c->current_env_level <= 0.0005f) {
        c->current_env_level = 0.0f;
        c->env_state = SUB_ENV_IDLE;
        return;
    }
    c->env_state = SUB_ENV_RELEASE;
    c->release_start_level = c->current_env_level;
    uint32_t release_samples = (uint32_t)(c->adsr.release_time_sec * (float)SUB_SAMPLE_RATE);
    c->release_step = (release_samples > 0)
        ? (c->release_start_level / (float)release_samples) : c->release_start_level;
    c->release_coeff = (release_samples > 0) ? sub_exp_approx(6.907755f / (float)release_samples) : 0.0f;
    c->env_samples = 0;
    c->phase_max_samples = release_samples;
}

/**
 * @brief エンベロープ 1 サンプル進行 (新レベルを返す。IDLE へ遷移で発音終了)
 */
static inline float sub_env_advance(SubEnvCore *c)
{
    /* 高速パス: 発音中かつ有効なサステインレベルを持つ場合は即リターン (消音判定漏れ防止) */
    if (c->env_state == SUB_ENV_SUSTAIN) {
        if (c->current_env_level <= 0.001f) {
            c->current_env_level = 0.0f;
            c->env_state = SUB_ENV_IDLE;
            return 0.0f;
        }
        return c->current_env_level;
    }
    float env = c->current_env_level;
    switch (c->env_state) {
        case SUB_ENV_ATTACK:
            env += c->attack_step;
            if (env >= 1.0f || c->env_samples >= c->phase_max_samples) {
                env = 1.0f;
                c->env_state = SUB_ENV_DECAY;
                c->env_samples = 0;
                c->phase_max_samples = (uint32_t)(c->adsr.decay_time_sec * (float)SUB_SAMPLE_RATE);
            }
            break;
        case SUB_ENV_DECAY:
            if (c->adsr.exponential_decay) {
                env = c->adsr.sustain_level + (env - c->adsr.sustain_level) * c->decay_coeff;
            } else {
                env -= c->decay_step;
            }
            if (env <= c->adsr.sustain_level || c->env_samples >= c->phase_max_samples) {
                env = c->adsr.sustain_level;
                c->env_state = SUB_ENV_SUSTAIN;
            }
            break;
        case SUB_ENV_SUSTAIN:
            env = c->adsr.sustain_level;
            if (env <= 0.001f) {
                env = 0.0f;
                c->env_state = SUB_ENV_IDLE;
            }
            break;
        case SUB_ENV_RELEASE:
            if (c->adsr.exponential_decay) {
                env *= c->release_coeff;
            } else {
                env -= c->release_step;
            }
            if (env <= 0.001f || c->env_samples >= c->phase_max_samples) {
                env = 0.0f;
                c->env_state = SUB_ENV_IDLE;
            }
            break;
        default:
            env = 0.0f;
            break;
    }
    c->current_env_level = env;
    c->env_samples++;
    return env;
}

/* ========================================================================= */
/* 5h. GM ドラム ノート分類 (sub4_main.c の sub4_note_on と整合)              */
/*     Kick 移行 (Phase 4) / Metal 移行 (Phase 5) のルーティング判定に使用。  */
/*     melodic チャンネルのノートでは呼ばないこと。                           */
/* ========================================================================= */
static inline bool sub_drum_is_kick(uint8_t note)
{
    return (note == 35u || note == 36u);
}

static inline bool sub_drum_is_metal(uint8_t note)
{
    if (note == 35u || note == 36u) return false;                /* Kick */
    if (note == 42u || note == 44u || note == 46u) return true;  /* HiHat */
    if (note == 38u || note == 40u) return false;                /* Snare */
    if (note == 39u) return false;                               /* Clap */
    if (note >= 41u && note <= 50u && note != 49u) return false; /* Tom */
    return true;                                                 /* Cymbal (49, 51-) */
}

/* ========================================================================= */
/* 6. 各サブコアのエントリーポイント関数定義 (マルチプラットフォーム共通)    */
/* ========================================================================= */
#ifdef __cplusplus
extern "C" {
#endif

void *subcore1_entry(void *arg);
void *subcore2_entry(void *arg);
void *subcore3_entry(void *arg);
void *subcore4_entry(void *arg);
void *subcore5_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SUB_COMMON_H_ */
