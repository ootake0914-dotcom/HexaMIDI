/**
 * @file sub_perc.h
 * @brief GM ドラム打楽器系 (Snare / Clap / Tom) 専用の共有音源モジュール
 * @details SubCore 4 のドラム音源から打楽器系 (Snare/Clap/Tom) を SubCore 2/3 へ
 *          動的移行するための専用 DSP (Phase 6 = 全ドラム分散)。
 *          sub4_main.c の sub4_render_snare() / sub4_render_clap() /
 *          sub4_render_tom() と同一アルゴリズムを移植したもの。
 *          各 Core が独立にこのモジュールを持ち、noise_seed / volume /
 *          rendered_total / last_snare_at も Core 毎に所有する。
 *
 *          音質を変えないため、sub4 の打楽器ロジックの数値・定数・スネアロール
 *          判定 (rendered_total/last_snare_at) を一切変更していない。
 */
#ifndef SUB_PERC_H_
#define SUB_PERC_H_

#include "sub_common.h"

#define SUB_PERC_MAX_VOICES (6u)
#define SUB_PERC_DEFAULT_VOL (0.90f)
#define SUB_PERC_SNARE_NONE  (0xFFFFFFFFu) /* 「直前ヒットなし」センチネル */
#define SUB_PERC_SWEEP_EXP   (-11.0f)      /* Tom ピッチスイープ (sub4 と同値) */

typedef enum {
    SUB_PERC_SNARE = 0,
    SUB_PERC_CLAP,
    SUB_PERC_TOM
} SubPercType;

typedef struct {
    bool        active;
    SubPercType type;
    float       velocity;
    float       phase;
    float       phase_increment;
    float       start_frequency;
    float       target_frequency;
    float       env_level;
    float       decay_coeff;
    uint32_t    samples_rendered;
    uint32_t    total_samples;
    float       prog;               /* Snare 進捗率 (ノイズゲイン用) */
    float       prog_inc;
    float       pitch_env;          /* Tom ピッチスイープ包絡 */
    float       pitch_decay_coeff;
    float       shim_lp;            /* Snare HP / Clap BPF 状態 */
    bool        buzz;               /* Snare ロール/ゴースト判定 */
    uint32_t    tap_pos;            /* Clap マルチタップ位相 (剰余カウンタ。%384 置換) */
} SubPercVoice;

typedef struct {
    SubPercVoice voices[SUB_PERC_MAX_VOICES];
    uint32_t     noise_seed;        /* 本 Core 専用 PRNG (C4 と非共有) */
    uint32_t     rendered_total;    /* 累計レンダリングサンプル (ロール判定用) */
    uint32_t     last_snare_at;     /* 直前スネア トリガ位置 */
    float        volume;            /* CC#7 (ch10) 反映値 */
} SubPercEngine;

/** @brief 打楽器系エンジン初期化 (各 Core の engine_init から 1 回呼ぶ) */
static inline void sub_perc_init(SubPercEngine *m)
{
    memset(m, 0, sizeof(SubPercEngine));
    m->noise_seed = 0x3C6EF372u; /* C4 (0x9ABCDEF1) と独立な種 */
    m->last_snare_at = SUB_PERC_SNARE_NONE;
    m->volume = SUB_PERC_DEFAULT_VOL;
}

/**
 * @brief 打楽器系 note-on (sub4_note_on の Snare/Clap/Tom 分岐と同一ロジック)
 */
static inline void sub_perc_note_on(SubPercEngine *m, uint8_t note, float velocity)
{
    if (velocity <= 0.0f) {
        return;
    }

    int voice_idx = -1;
    for (int i = 0; i < (int)SUB_PERC_MAX_VOICES; i++) {
        if (!m->voices[i].active) {
            voice_idx = i;
            break;
        }
    }
    if (voice_idx == -1) {
        float min_env = 999.0f;
        for (int i = 0; i < (int)SUB_PERC_MAX_VOICES; i++) {
            if (m->voices[i].env_level < min_env) {
                min_env = m->voices[i].env_level;
                voice_idx = i;
            }
        }
    }

    SubPercVoice *v = &m->voices[voice_idx];
    v->active = true;
    v->velocity = velocity;
    v->phase = 0.0f;
    v->env_level = 1.0f;
    v->samples_rendered = 0;
    v->shim_lp = 0.0f;
    v->buzz = false;
    v->tap_pos = 0;

    float decay_sec;
    if (note == 38 || note == 40) {
        /* Snare Drum: FM ボディ + ノイズ。ロール/ゴースト判定付き */
        v->type = SUB_PERC_SNARE;
        v->start_frequency = 180.0f;
        v->target_frequency = 180.0f;
        decay_sec = (velocity < 0.40f) ? 0.085f : ((velocity >= 0.85f) ? 0.135f : 0.120f);
        v->buzz = (m->last_snare_at != SUB_PERC_SNARE_NONE) &&
                  ((m->rendered_total - m->last_snare_at) < 2880u);
        m->last_snare_at = m->rendered_total;
    } else if (note == 39) {
        /* Hand Clap: 808 風マルチインパルス + バンドパスノイズ */
        v->type = SUB_PERC_CLAP;
        v->start_frequency = 0.0f;
        v->target_frequency = 0.0f;
        decay_sec = 0.180f;
    } else {
        /* Toms (Low/Mid/High): トーン急降下サイン波 */
        v->type = SUB_PERC_TOM;
        float tom_f = 90.0f + (float)(note - 41) * 15.0f;
        v->start_frequency = tom_f * 1.5f;
        v->target_frequency = tom_f;
        decay_sec = 0.160f;
    }

    uint32_t decay_samples = (uint32_t)(decay_sec * (float)SUB_SAMPLE_RATE);
    v->total_samples = decay_samples;
    v->decay_coeff = (decay_samples > 0)
        ? sub_exp_approx(6.907755f / (float)decay_samples) : 0.0f;
    v->phase_increment = v->start_frequency / (float)SUB_SAMPLE_RATE;
    v->prog = 0.0f;
    v->prog_inc = (decay_samples > 0) ? (1.0f / (float)decay_samples) : 1.0f;
    v->pitch_env = 1.0f;
    v->pitch_decay_coeff = (decay_samples > 0)
        ? sub_exp_approx(-SUB_PERC_SWEEP_EXP / (float)decay_samples) : 0.0f;
}

/** @brief 全打楽器消音 (ALL_NOTES_OFF / STOP) */
static inline void sub_perc_all_notes_off(SubPercEngine *m)
{
    for (int i = 0; i < (int)SUB_PERC_MAX_VOICES; i++) {
        m->voices[i].active = false;
        m->voices[i].env_level = 0.0f;
    }
}

/** @brief Snare 専用レンダリング (sub4_render_snare と同一) */
static inline void sub_perc_render_snare(SubPercVoice *v, SubPercEngine *m,
                                         float *acc_l, float *acc_r,
                                         uint32_t frames, float gain)
{
    float phase = v->phase;
    float phase_inc = v->phase_increment;
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    float prog = v->prog;
    float prog_inc = v->prog_inc;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    bool buzz = v->buzz;
    float snare_hp = v->shim_lp;

    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float tone = sub_lookup_sine(g_sine_lut, phase) * 0.40f;
        float fm_mod = sub_lookup_sine(g_sine_lut, phase * 2.76f) * (env_level * 0.55f);
        float fm_tone = sub_lookup_sine(g_sine_lut, phase + fm_mod * 0.25f) * 0.35f;

        float noise_gain = buzz ? 0.85f : (0.55f + 0.30f * (1.0f - prog));
        prog += prog_inc;

        float noise_raw = sub_noise_lfsr(&m->noise_seed) * noise_gain;
        snare_hp += 0.22f * (noise_raw - snare_hp);
        float noise = (noise_raw - snare_hp) * 1.15f;
        float out = tone + fm_tone + noise;

        acc_l[f] += out * env_level * gain;
        acc_r[f] += out * env_level * gain;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    v->phase = phase;
    v->prog = prog;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
    v->shim_lp = snare_hp;
}

/** @brief Clap 専用レンダリング (sub4_render_clap と同一) */
static inline void sub_perc_render_clap(SubPercVoice *v, SubPercEngine *m,
                                        float *acc_l, float *acc_r,
                                        uint32_t frames, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    float shim_lp = v->shim_lp;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    /* タップ位相は加算カウンタで維持 (%384 は M4F でソフト除算のため排除。
     * samples_rendered%384 と同一値列 1,2,...,383,0,1... を生成) */
    uint32_t tap_pos = v->tap_pos;

    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float n = sub_noise_lfsr(&m->noise_seed);
        shim_lp += 0.40f * (n - shim_lp);
        float clap_band = n - shim_lp;

        float env_mod = env_level;
        if (samples_rendered < 1200) {
            /* 4回の連続タップ (約 8ms 周期) */
            if (++tap_pos >= 384u) tap_pos = 0u;
            float tap_env = (float)(384u - tap_pos) * (1.0f / 384.0f);
            env_mod *= (0.5f + 0.5f * tap_env);
        }

        float out = clap_band * env_mod * gain;
        acc_l[f] += out;
        acc_r[f] += out;
    }

    v->tap_pos = tap_pos;
    v->shim_lp = shim_lp;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

/** @brief Tom 専用レンダリング (sub4_render_tom と同一) */
static inline void sub_perc_render_tom(SubPercVoice *v, SubPercEngine *m,
                                       float *acc_l, float *acc_r,
                                       uint32_t frames, float gain)
{
    (void)m;
    float pitch_env = v->pitch_env;
    float pitch_decay = v->pitch_decay_coeff;
    float phase = v->phase;
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    float start_f = v->start_frequency;
    float target_f = v->target_frequency;

    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float freq = target_f + (start_f - target_f) * pitch_env;
        pitch_env *= pitch_decay;
        float phase_inc = freq * (1.0f / (float)SUB_SAMPLE_RATE);

        float s = sub_lookup_sine(g_sine_lut, phase);
        float d = s * 1.15f;
        if (d > 0.95f)       d = 0.95f;
        else if (d < -0.95f) d = -0.95f;

        acc_l[f] += d * env_level * gain;
        acc_r[f] += d * env_level * gain;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    v->pitch_env = pitch_env;
    v->phase = phase;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

/**
 * @brief 打楽器系レンダリング (タイル毎に呼ばれ、内部状態は跨いで保持される)
 */
static inline void sub_perc_render(SubPercEngine *m, float *acc_l, float *acc_r,
                                   uint32_t frames)
{
    /* 実時間基準で進める (スネアロール判定用) */
    m->rendered_total += frames;

    for (int i = 0; i < (int)SUB_PERC_MAX_VOICES; i++) {
        SubPercVoice *v = &m->voices[i];
        if (!v->active) {
            continue;
        }
        const float gain = m->volume * v->velocity * 0.50f;
        switch (v->type) {
            case SUB_PERC_SNARE:
                sub_perc_render_snare(v, m, acc_l, acc_r, frames, gain);
                break;
            case SUB_PERC_CLAP:
                sub_perc_render_clap(v, m, acc_l, acc_r, frames, gain);
                break;
            case SUB_PERC_TOM:
                sub_perc_render_tom(v, m, acc_l, acc_r, frames, gain);
                break;
            default:
                break;
        }
    }
}

#endif /* SUB_PERC_H_ */
