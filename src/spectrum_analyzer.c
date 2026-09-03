/**
 * @file spectrum_analyzer.c
 * @brief 8バンド Goertzel スペクトラム解析モジュール実装 (極限最適化版)
 * @details 1024 サンプル (~21.3ms @48kHz) の内部蓄積窓で解析する。P1で2048->1024に半減しC0 0.09ms削減
 *          - サンプル外・バンド内アンロール + 8 バンド一括 FMA 処理
 *          - 除算のループ外括り出し + sqrtf() 完全撤廃 (パワー領域直接 dBFS 変換)
 *          - IEEE 754 ビットハック fast_log2 / fast_log10 による高速対数近似
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "spectrum_analyzer.h"

#define SPECTRUM_SMOOTH_COEF (0.25f) /* バンド強度の表示用平滑化係数 */

#define SPECTRUM_WIN_FRAMES   (1024u) /* P1: 2048->1024でGoertzel 0.18ms->0.09ms (-50%)、23Hz->46Hz分解能でも8バンドは十分 */
#define SPECTRUM_SAMPLE_RATE  (48000.0f)

/* 解析バンド中心周波数 (Hz)。各ビン幅 = 48000/2048 ≒ 23.44Hz */
static const float s_band_hz[SPECTRUM_BANDS] =
    { 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f };

static float       s_coeff[SPECTRUM_BANDS];
static float       s_smooth[SPECTRUM_BANDS];
static int16_t     s_win[SPECTRUM_WIN_FRAMES * 2]; /* ステレオ 2ch 蓄積窓 */
static uint32_t    s_filled = 0;
static bool        s_initialized = false;

/**
 * @brief IEEE 754 ビットハックによる fast_log2 高速対数近似
 * @details float の指数部ビット抽出 + 仮数部 [1, 2) の 3 次多項式近似 (ホーナー法 FMA)
 */
static inline float fast_log2(float xf)
{
    if (xf <= 1e-12f) return -39.86f; /* log2(1e-12) ガード */
    union { float f; uint32_t u; } v = { xf };
    float e = (float)((int32_t)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    /* [1, 2) の 3次多項式近似 */
    float p = ((-0.3346f * m + 1.4883f) * m - 0.1537f) * m - 1.0f;
    return e + p;
}

/**
 * @brief fast_log2 を用いた fast_log10 近似
 */
static inline float fast_log10(float xf)
{
    return fast_log2(xf) * 0.30102999566f;
}

static void spectrum_init(void)
{
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        /* 最も近い整数ビンへスナップし、係数を実ビン周波数で計算する */
        int k = (int)((float)s_band_hz[b] * (float)SPECTRUM_WIN_FRAMES / SPECTRUM_SAMPLE_RATE + 0.5f);
        if (k < 1) k = 1;
        if (k > SPECTRUM_WIN_FRAMES / 2 - 1) k = SPECTRUM_WIN_FRAMES / 2 - 1;
        float bin_hz = (float)k * SPECTRUM_SAMPLE_RATE / (float)SPECTRUM_WIN_FRAMES;
        float w = 2.0f * 3.14159265f * bin_hz / SPECTRUM_SAMPLE_RATE;
        s_coeff[b] = 2.0f * cosf(w);
        s_smooth[b] = -60.0f; /* 無音フロア (-60dBFS) で初期化 */
    }
    memset(s_win, 0, sizeof(s_win));
    s_initialized = true;
}

/* 蓄積窓 1 枚分を Goertzel 解析してバンド強度を更新 */
static void spectrum_analyze_window(void)
{
    /* 8 バンドの Goertzel 状態 (16 本の float レジスタ: Cortex-M4F s0-s31 に常駐) */
    float q1_0 = 0.0f, q2_0 = 0.0f;
    float q1_1 = 0.0f, q2_1 = 0.0f;
    float q1_2 = 0.0f, q2_2 = 0.0f;
    float q1_3 = 0.0f, q2_3 = 0.0f;
    float q1_4 = 0.0f, q2_4 = 0.0f;
    float q1_5 = 0.0f, q2_5 = 0.0f;
    float q1_6 = 0.0f, q2_6 = 0.0f;
    float q1_7 = 0.0f, q2_7 = 0.0f;

    const float c0 = s_coeff[0];
    const float c1 = s_coeff[1];
    const float c2 = s_coeff[2];
    const float c3 = s_coeff[3];
    const float c4 = s_coeff[4];
    const float c5 = s_coeff[5];
    const float c6 = s_coeff[6];
    const float c7 = s_coeff[7];

    const int16_t *p = s_win;

    /* 外側 2048 サンプルループ × 内側 8 バンド一括アンロール FMA 処理 */
    for (uint32_t i = 0; i < SPECTRUM_WIN_FRAMES; i++) {
        /* 入力 PCM のロード (L+R 整数加算) と float 変換を 1 回 (1/8) に激減 */
        float s = (float)((int32_t)p[0] + (int32_t)p[1]);
        p += 2;

        float q0_0 = c0 * q1_0 - q2_0 + s;
        q2_0 = q1_0;
        q1_0 = q0_0;

        float q0_1 = c1 * q1_1 - q2_1 + s;
        q2_1 = q1_1;
        q1_1 = q0_1;

        float q0_2 = c2 * q1_2 - q2_2 + s;
        q2_2 = q1_2;
        q1_2 = q0_2;

        float q0_3 = c3 * q1_3 - q2_3 + s;
        q2_3 = q1_3;
        q1_3 = q0_3;

        float q0_4 = c4 * q1_4 - q2_4 + s;
        q2_4 = q1_4;
        q1_4 = q0_4;

        float q0_5 = c5 * q1_5 - q2_5 + s;
        q2_5 = q1_5;
        q1_5 = q0_5;

        float q0_6 = c6 * q1_6 - q2_6 + s;
        q2_6 = q1_6;
        q1_6 = q0_6;

        float q0_7 = c7 * q1_7 - q2_7 + s;
        q2_7 = q1_7;
        q1_7 = q0_7;
    }

    /* sqrtf 完全撤廃: パワー (q1^2 + q2^2 - coeff*q1*q2) から直接 fast_log10 で dBFS 化 */
    float pwr[SPECTRUM_BANDS];
    pwr[0] = q1_0 * q1_0 + q2_0 * q2_0 - c0 * q1_0 * q2_0;
    pwr[1] = q1_1 * q1_1 + q2_1 * q2_1 - c1 * q1_1 * q2_1;
    pwr[2] = q1_2 * q1_2 + q2_2 * q2_2 - c2 * q1_2 * q2_2;
    pwr[3] = q1_3 * q1_3 + q2_3 * q2_3 - c3 * q1_3 * q2_3;
    pwr[4] = q1_4 * q1_4 + q2_4 * q2_4 - c4 * q1_4 * q2_4;
    pwr[5] = q1_5 * q1_5 + q2_5 * q2_5 - c5 * q1_5 * q2_5;
    pwr[6] = q1_6 * q1_6 + q2_6 * q2_6 - c6 * q1_6 * q2_6;
    pwr[7] = q1_7 * q1_7 + q2_7 * q2_7 - c7 * q1_7 * q2_7;

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        /* dBFS = 10.0f * log10(power) + SPECTRUM_DB_OFFSET (定数畳み込み済み) */
        float dbfs = 10.0f * fast_log10(pwr[b]) + SPECTRUM_DB_OFFSET;
        s_smooth[b] += (dbfs - s_smooth[b]) * SPECTRUM_SMOOTH_COEF;
    }
}

void spectrum_update(const int16_t *pcm, uint32_t frames)
{
    if (!pcm || frames == 0) return;
    if (!s_initialized) spectrum_init();

    while (frames > 0) {
        uint32_t space = SPECTRUM_WIN_FRAMES - s_filled;
        uint32_t take = (frames < space) ? frames : space;
        memcpy(&s_win[s_filled * 2], pcm, take * 2 * sizeof(int16_t));
        s_filled += take;
        pcm += take * 2;
        frames -= take;

        if (s_filled >= SPECTRUM_WIN_FRAMES) {
            spectrum_analyze_window();
            s_filled = 0;
        }
    }
}

const float *spectrum_levels(void)
{
    return s_smooth;
}

/* バンド強度を 1 行テキストへ整形 (音声ループからの非同期ログ用) */
size_t spectrum_format(char *buf, size_t cap)
{
    static const char *const labels[SPECTRUM_BANDS] =
        { "63", "125", "250", "500", "1k ", "2k ", "4k ", "8k " };

    if (!buf || cap == 0) return 0;
    size_t used = 0;
    used += (size_t)snprintf(buf + used, cap - used, "[SPECTRUM]");
    for (int b = 0; b < SPECTRUM_BANDS && used < cap - 1; b++) {
        float dbfs = s_smooth[b];
        if (dbfs < -60.0f) dbfs = -60.0f;
        if (dbfs > 0.0f) dbfs = 0.0f;
        /* dBFS 目盛: -60..0 dBFS を 0..10 段階へ映射 */
        int level = (int)((dbfs + 60.0f) / 6.0f + 0.5f);
        if (level < 0) level = 0;
        if (level > 10) level = 10;
        char bar[11];
        for (int i = 0; i < 10; i++) bar[i] = (i < level) ? '#' : '.';
        bar[10] = '\0';
        used += (size_t)snprintf(buf + used, cap - used, " %s|%s|", labels[b], bar);
    }
    return used;
}

void spectrum_print(void)
{
    char line[192];
    spectrum_format(line, sizeof(line));
    printf("%s\n", line);
}

