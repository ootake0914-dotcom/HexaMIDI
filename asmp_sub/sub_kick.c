/**
 * @file sub_kick.c
 * @brief GM Drum Kick shared implementation (externalized from header for code dedup)
 * @details OPTIMIZATION_MEMO #5: sub_kick.hのstatic inline重複(各TUでコード生成)を
 *          外部関数化し、Sub2/3間でのテキスト重複1KBを削減。ロジックは従来の
 *          header inlineと完全同一 (phase 0.25, sub_exp_approx, sub_noise_lfsr等)。
 */
#include "sub_kick.h"
#include <string.h>

void sub_kick_init(SubKickEngine *k)
{
    memset(k, 0, sizeof(SubKickEngine));
    k->noise_seed = 0x5EEDC0DEu;
    k->volume = SUB_KICK_DEFAULT_VOL;
}

void sub_kick_note_on(SubKickEngine *k, float velocity)
{
    if (velocity <= 0.0f) return;
    int voice_idx = -1;
    for (int i = 0; i < (int)SUB_KICK_MAX_VOICES; i++) {
        if (!k->voices[i].active) { voice_idx = i; break; }
    }
    if (voice_idx == -1) {
        float min_env = 999.0f;
        for (int i = 0; i < (int)SUB_KICK_MAX_VOICES; i++) {
            if (k->voices[i].env_level < min_env) { min_env = k->voices[i].env_level; voice_idx = i; }
        }
    }
    SubKickVoice *v = &k->voices[voice_idx];
    v->active = true;
    v->velocity = velocity;
    v->phase = 0.25f;
    v->env_level = 1.0f;
    v->samples_rendered = 0;
    v->click_lp = 0.0f;
    float decay_sec = 0.150f;
    if (velocity >= 0.85f) { v->start_frequency = SUB_KICK_START_HARD; decay_sec = 0.150f; }
    else if (velocity >= 0.50f) { v->start_frequency = SUB_KICK_START_NORMAL; decay_sec = 0.130f; }
    else { v->start_frequency = SUB_KICK_START_GHOST; decay_sec = 0.095f; }
    v->target_frequency = SUB_KICK_TARGET;
    v->frequency = v->start_frequency;
    uint32_t decay_samples = (uint32_t)(decay_sec * (float)SUB_SAMPLE_RATE);
    v->total_samples = decay_samples;
    v->decay_coeff = (decay_samples > 0) ? sub_exp_approx(6.907755f / (float)decay_samples) : 0.0f;
    v->pitch_env = 1.0f;
    v->pitch_decay_coeff = (decay_samples > 0) ? sub_exp_approx(-SUB_KICK_SWEEP_EXP / (float)decay_samples) : 0.0f;
}

void sub_kick_all_notes_off(SubKickEngine *k)
{
    for (int i = 0; i < (int)SUB_KICK_MAX_VOICES; i++) {
        k->voices[i].active = false;
        k->voices[i].env_level = 0.0f;
    }
}

void sub_kick_render(SubKickEngine *k, float *acc_l, float *acc_r, uint32_t frames)
{
    for (int i = 0; i < (int)SUB_KICK_MAX_VOICES; i++) {
        SubKickVoice *v = &k->voices[i];
        if (!v->active) continue;
        const float gain = k->volume * v->velocity * 0.50f;
        float pitch_env = v->pitch_env;
        float pitch_decay = v->pitch_decay_coeff;
        float phase = v->phase;
        float env_level = v->env_level;
        float decay_coeff = v->decay_coeff;
        uint32_t samples_rendered = v->samples_rendered;
        uint32_t total_samples = v->total_samples;
        float start_f = v->start_frequency;
        float target_f = v->target_frequency;
        float click_lp = v->click_lp;
        for (uint32_t f = 0; f < frames; f++) {
            env_level *= decay_coeff;
            samples_rendered++;
            if (env_level <= 0.001f || samples_rendered >= total_samples) {
                v->active = false; env_level = 0.0f; break;
            }
            float freq = target_f + (start_f - target_f) * pitch_env;
            pitch_env *= pitch_decay;
            float phase_inc = freq * (1.0f / (float)SUB_SAMPLE_RATE);
            float s = sub_lookup_sine(g_sine_lut, phase);
            float d = s * 1.25f;
            if (d > 0.95f) d = 0.95f; else if (d < -0.95f) d = -0.95f;
            if (samples_rendered <= SUB_KICK_CLICK_LEN) {
                float cn = sub_noise_lfsr(&k->noise_seed);
                click_lp += 0.60f * (cn - click_lp);
                d += (cn - click_lp) * 0.40f * (1.0f - (float)samples_rendered * (1.0f / (float)SUB_KICK_CLICK_LEN));
            }
            const float out = d * env_level * gain;
            acc_l[f] += out; acc_r[f] += out;
            phase += phase_inc; if (phase >= 1.0f) phase -= 1.0f;
        }
        v->pitch_env = pitch_env; v->phase = phase; v->env_level = env_level;
        v->samples_rendered = samples_rendered; v->click_lp = click_lp;
    }
}
