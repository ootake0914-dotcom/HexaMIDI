/**
 * @file sub_metal.h
 * @brief GM ドラム金属系 (HiHat / Cymbal) 専用の共有音源モジュール
 * @details SubCore 4 のドラム音源から金属系 (HiHat/Cymbal) を SubCore 2/3 へ
 *          動的移行するための専用 DSP (Phase 5)。sub4_main.c の
 *          sub4_render_hihat() / sub4_render_cymbal() と同一アルゴリズム
 *          (6 系統金属音 Q32 位相 + PolyBLEP 帯域制限 + ノイズ + one-pole HP/BPF)
 *          を移植したもの。各 Core が独立にこのモジュールを持ち、
 *          noise_seed / metal_free_ph[] / volume も Core 毎に所有する。
 *
 *          音質を変えないため、sub4 の金属音ロジックの数値・定数・位相継承
 *          (発音毎の固定リセットを廃しフリーランニング位相を引き継ぐ) を
 *          一切変更していない。
 */
#ifndef SUB_METAL_H_
#define SUB_METAL_H_

#include "sub_common.h"

/* 同時発音数上限 (HiHat は短い・Cymbal は長い one-shot) */
#define SUB_METAL_MAX_VOICES (6u)
#define SUB_METAL_DEFAULT_VOL (0.90f)

typedef enum {
    SUB_METAL_HIHAT = 0,
    SUB_METAL_CYMBAL
} SubMetalType;

typedef struct {
    bool        active;
    SubMetalType type;
    float       velocity;
    float       env_level;
    float       decay_coeff;
    uint32_t    samples_rendered;
    uint32_t    total_samples;
    float       shim_lp;          /* one-pole HP/BPF 状態 (sub4 の shim_lp 相当) */
    uint32_t    metal_ph[6];      /* 発音時に free_ph から継承 */
} SubMetalVoice;

typedef struct {
    SubMetalVoice voices[SUB_METAL_MAX_VOICES];
    uint32_t      noise_seed;      /* 本 Core 専用 PRNG (C4 と非共有) */
    uint32_t      metal_free_ph[6];/* フリーランニング位相 (ボイス有無に非依存) */
    float         volume;          /* CC#7 (ch10) 反映値 */
} SubMetalEngine;

/* TR-808 非整数比 6 発振器 Q32 位相増分 (sub4 の s_metal_inc と同一値) */
static const uint32_t s_sub_metal_inc[6] = {
    (uint32_t)(263.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(400.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(421.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(474.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(587.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(845.0f / 48000.0f * 4294967296.0f + 0.5f)
};

/**
 * @brief Q32 位相の金属音パーシャルを PolyBLEP 帯域制限矩形波として評価
 * @details C4 と同一経路に統一し、float 位相化・2回補正を避ける。
 *          これにより HiHat/Cymbal を C2/C3 へ移管しても最悪ケースの
 *          追加 CPU を抑え、C4 での高速化が移管先で崩れない。
 */
static inline float sub_metal_partial(uint32_t ph, uint32_t inc)
{
    return sub_osc_square_q32(ph, inc);
}

/** @brief 金属系エンジン初期化 (各 Core の engine_init から 1 回呼ぶ) */
static inline void sub_metal_init(SubMetalEngine *m)
{
    memset(m, 0, sizeof(SubMetalEngine));
    m->noise_seed = 0x7A55E55Au; /* C4 (0x9ABCDEF1) と独立な種 */
    m->metal_free_ph[0] = 0x2A000000u;
    m->metal_free_ph[1] = 0x7C000000u;
    m->metal_free_ph[2] = 0xB1000000u;
    m->metal_free_ph[3] = 0xE6000000u;
    m->metal_free_ph[4] = 0x19000000u;
    m->metal_free_ph[5] = 0x5F000000u;
    m->volume = SUB_METAL_DEFAULT_VOL;
}

/**
 * @brief 金属系 note-on (sub4_note_on の HiHat/Cymbal 分岐と同一ロジック)
 */
static inline void sub_metal_note_on(SubMetalEngine *m, uint8_t note, float velocity)
{
    if (velocity <= 0.0f) {
        return;
    }

    int voice_idx = -1;
    for (int i = 0; i < (int)SUB_METAL_MAX_VOICES; i++) {
        if (!m->voices[i].active) {
            voice_idx = i;
            break;
        }
    }
    if (voice_idx == -1) {
        float min_env = 999.0f;
        for (int i = 0; i < (int)SUB_METAL_MAX_VOICES; i++) {
            if (m->voices[i].env_level < min_env) {
                min_env = m->voices[i].env_level;
                voice_idx = i;
            }
        }
    }

    SubMetalVoice *v = &m->voices[voice_idx];
    v->active = true;
    v->velocity = velocity;
    v->env_level = 1.0f;
    v->samples_rendered = 0;
    v->shim_lp = 0.0f;

    float decay_sec;
    if (note == 42 || note == 44) {
        v->type = SUB_METAL_HIHAT; decay_sec = 0.035f; /* Closed / Pedal */
    } else if (note == 46) {
        v->type = SUB_METAL_HIHAT; decay_sec = 0.220f; /* Open */
    } else {
        v->type = SUB_METAL_CYMBAL; decay_sec = 0.650f; /* Crash/Ride/Other */
    }

    uint32_t decay_samples = (uint32_t)(decay_sec * (float)SUB_SAMPLE_RATE);
    v->total_samples = decay_samples;
    v->decay_coeff = (decay_samples > 0)
        ? sub_exp_approx(6.907755f / (float)decay_samples) : 0.0f;

    /* フリーランニング位相を引き継ぐ (発音毎の固定リセットは位相打ち消しで
     * 音痩せするため廃止、sub4 と同一) */
    for (int k = 0; k < 6; k++) {
        v->metal_ph[k] = m->metal_free_ph[k];
    }
}

/** @brief 全金属音消音 (ALL_NOTES_OFF / STOP) */
static inline void sub_metal_all_notes_off(SubMetalEngine *m)
{
    for (int i = 0; i < (int)SUB_METAL_MAX_VOICES; i++) {
        m->voices[i].active = false;
        m->voices[i].env_level = 0.0f;
    }
}

/** @brief HiHat 専用レンダリング (sub4_render_hihat と同一) */
static inline void sub_metal_render_hihat(SubMetalVoice *v, SubMetalEngine *m,
                                          float *acc_l, float *acc_r,
                                          uint32_t frames, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    float hat_lp = v->shim_lp;

    uint32_t ph0 = v->metal_ph[0];
    uint32_t ph1 = v->metal_ph[1];
    uint32_t ph2 = v->metal_ph[2];
    uint32_t ph3 = v->metal_ph[3];
    uint32_t ph4 = v->metal_ph[4];
    uint32_t ph5 = v->metal_ph[5];

    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float o0 = sub_metal_partial(ph0, s_sub_metal_inc[0]);
        float o1 = sub_metal_partial(ph1, s_sub_metal_inc[1]);
        float o2 = sub_metal_partial(ph2, s_sub_metal_inc[2]);
        float o3 = sub_metal_partial(ph3, s_sub_metal_inc[3]);
        float o4 = sub_metal_partial(ph4, s_sub_metal_inc[4]);
        float o5 = sub_metal_partial(ph5, s_sub_metal_inc[5]);

        ph0 += s_sub_metal_inc[0];
        ph1 += s_sub_metal_inc[1];
        ph2 += s_sub_metal_inc[2];
        ph3 += s_sub_metal_inc[3];
        ph4 += s_sub_metal_inc[4];
        ph5 += s_sub_metal_inc[5];

        float metal = fmaf(0.12f, o0, 0.0f);
        metal = fmaf(0.12f, o1, metal);
        metal = fmaf(0.12f, o2, metal);
        metal = fmaf(0.12f, o3, metal);
        metal = fmaf(0.12f, o4, metal);
        metal = fmaf(0.12f, o5, metal);

        float noise = sub_noise_lfsr(&m->noise_seed) * 0.50f;
        float raw = metal + noise;
        hat_lp += 0.32f * (raw - hat_lp);
        float out = raw - hat_lp;

        acc_l[f] += out * env_level * gain;
        acc_r[f] += out * env_level * gain;
    }

    v->metal_ph[0] = ph0;
    v->metal_ph[1] = ph1;
    v->metal_ph[2] = ph2;
    v->metal_ph[3] = ph3;
    v->metal_ph[4] = ph4;
    v->metal_ph[5] = ph5;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
    v->shim_lp = hat_lp;
}

/** @brief Cymbal 専用レンダリング (sub4_render_cymbal と同一) */
static inline void sub_metal_render_cymbal(SubMetalVoice *v, SubMetalEngine *m,
                                           float *acc_l, float *acc_r,
                                           uint32_t frames, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    float shim_lp = v->shim_lp;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;

    uint32_t ph0 = v->metal_ph[0];
    uint32_t ph1 = v->metal_ph[1];
    uint32_t ph2 = v->metal_ph[2];
    uint32_t ph3 = v->metal_ph[3];
    uint32_t ph4 = v->metal_ph[4];
    uint32_t ph5 = v->metal_ph[5];

    /* 注: 旧 s1/s2 (phase*3.7/7.3 サイン) は削除。phase は常時 0 のため
     * 両項は恒等的に +0.0。Hermite 2 回分/サンプルを削減 */
    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float o0 = sub_metal_partial(ph0, s_sub_metal_inc[0]);
        float o1 = sub_metal_partial(ph1, s_sub_metal_inc[1]);
        float o2 = sub_metal_partial(ph2, s_sub_metal_inc[2]);
        float o3 = sub_metal_partial(ph3, s_sub_metal_inc[3]);
        float o4 = sub_metal_partial(ph4, s_sub_metal_inc[4]);
        float o5 = sub_metal_partial(ph5, s_sub_metal_inc[5]);

        ph0 += s_sub_metal_inc[0];
        ph1 += s_sub_metal_inc[1];
        ph2 += s_sub_metal_inc[2];
        ph3 += s_sub_metal_inc[3];
        ph4 += s_sub_metal_inc[4];
        ph5 += s_sub_metal_inc[5];

        float metal = fmaf(0.08f, o0, 0.0f);
        metal = fmaf(0.08f, o1, metal);
        metal = fmaf(0.08f, o2, metal);
        metal = fmaf(0.08f, o3, metal);
        metal = fmaf(0.08f, o4, metal);
        metal = fmaf(0.08f, o5, metal);

        float n1 = sub_noise_lfsr(&m->noise_seed);

        float raw = n1 * 0.40f + metal;
        shim_lp += 0.55f * (raw - shim_lp);
        float hp = raw - shim_lp;
        float out = n1 * 0.35f + hp * 0.70f;

        acc_l[f] += out * env_level * gain;
        acc_r[f] += out * env_level * gain;
    }

    v->metal_ph[0] = ph0;
    v->metal_ph[1] = ph1;
    v->metal_ph[2] = ph2;
    v->metal_ph[3] = ph3;
    v->metal_ph[4] = ph4;
    v->metal_ph[5] = ph5;
    v->shim_lp = shim_lp;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

/**
 * @brief 金属系レンダリング (タイル毎に呼ばれ、内部状態は跨いで保持される)
 *        フリーランニング位相をボイス有無に非依存で進めてから各ボイスを合成する。
 */
static inline void sub_metal_render(SubMetalEngine *m, float *acc_l, float *acc_r,
                                    uint32_t frames)
{
    /* 金属音フリーランニング位相を全フレームで進める (ボイス有無に非依存)。
     * O(1) 加算: 旧 6加算xframesループと mod 2^32 で完全等価 */
    m->metal_free_ph[0] += s_sub_metal_inc[0] * frames;
    m->metal_free_ph[1] += s_sub_metal_inc[1] * frames;
    m->metal_free_ph[2] += s_sub_metal_inc[2] * frames;
    m->metal_free_ph[3] += s_sub_metal_inc[3] * frames;
    m->metal_free_ph[4] += s_sub_metal_inc[4] * frames;
    m->metal_free_ph[5] += s_sub_metal_inc[5] * frames;

    for (int i = 0; i < (int)SUB_METAL_MAX_VOICES; i++) {
        SubMetalVoice *v = &m->voices[i];
        if (!v->active) {
            continue;
        }
        const float gain = m->volume * v->velocity * 0.50f;
        if (v->type == SUB_METAL_HIHAT) {
            sub_metal_render_hihat(v, m, acc_l, acc_r, frames, gain);
        } else {
            sub_metal_render_cymbal(v, m, acc_l, acc_r, frames, gain);
        }
    }
}

#endif /* SUB_METAL_H_ */
