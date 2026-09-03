/**
 * @file synth_engine.c
 * @brief Sony Spresense 16ch マルチティンバー・音声合成（シンセサイザー）エンジン実装
 * @details 16音ポリフォニー、GM音色マッピング、GM Standardドラムキット、PolyBLEP/BLAMPオシレータ、ステレオリバーブDSP
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "synth_engine.h"
#include "sub_common.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846f)
#endif

/* 疑似乱数生成（Xorshift32 ホワイトノイズ・仮数ビットパック）
 * 16bit LFSR(周期65k/1.36s)から32bit Xorshift(周期4G/89s)へ置換。
 * 除算を排除しVCVTを不要化。sub4_noise_fastと同一品質/CPU。 */
static inline float generate_noise_lfsr(uint32_t *lfsr)
{
    uint32_t x = *lfsr;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *lfsr = x;
    union { uint32_t u; float f; } c;
    c.u = 0x3F800000u | (x >> 9); /* [1.0,2.0) */
    return (c.f - 1.5f) * 2.0f;   /* [-1.0,1.0) */
}
/* 互換エイリアス（旧名参照の外部コード用） */
static inline float generate_noise_xor(uint32_t *s){ return generate_noise_lfsr(s); }

/* ========================================================================= */
/* 1. 正弦波 LUT ＋ 線形補間オシレータ                                       */
/* ========================================================================= */
/* sine LUTは共有ROM g_sine_lutを使用するため初期化不要 */

static inline float lookup_sine(const float *lut, float phase)
{
    /* floorf排除版: sub_common.h:sub_lookup_sineと同一のint cast+mask方式 */
    float pos = phase * (float)SYNTH_SINE_LUT_SIZE;
    int idx = (int)pos;
    if (pos < 0.0f && idx != pos) idx--; /* floor */
    int im = (idx - 1) & (SYNTH_SINE_LUT_SIZE - 1);
    int i0 = idx & (SYNTH_SINE_LUT_SIZE - 1);
    int i1 = (idx + 1) & (SYNTH_SINE_LUT_SIZE - 1);
    int i2 = (idx + 2) & (SYNTH_SINE_LUT_SIZE - 1);
    float frac = pos - (float)idx;
    /* 3次 Hermite 補間 (sub_common.h と同一実装): 誤差 ~3.7e-9 */
    float m0 = 0.5f * (lut[i1] - lut[im]);
    float m1 = 0.5f * (lut[i2] - lut[i0]);
    float t = frac;
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0f*t3 - 3.0f*t2 + 1.0f) * lut[i0]
         + (t3 - 2.0f*t2 + t)       * m0
         + (-2.0f*t3 + 3.0f*t2)     * lut[i1]
         + (t3 - t2)                * m1;
}

/* ========================================================================= */
/* 2. PolyBLEP / PolyBLAMP 帯域制限補正                                       */
/* ========================================================================= */
static inline float poly_blep(float t, float dt)
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

static inline float poly_blamp(float t, float dt)
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

/* オシレータ単一サンプル計算 */
static inline float calculate_oscillator(SynthVoice *v)
{
    float phase = v->phase;
    float dt = v->phase_increment;

    switch (v->wave_type) {
        case WAVE_SINE:
            return lookup_sine(g_sine_lut, phase);

        case WAVE_SQUARE: {
            float naive = (phase < 0.5f) ? 1.0f : -1.0f;
            float shift_phase = (phase >= 0.5f) ? (phase - 0.5f) : (phase + 0.5f);
            return naive + poly_blep(phase, dt) - poly_blep(shift_phase, dt);
        }

        case WAVE_SAWTOOTH: {
            float naive = (2.0f * phase) - 1.0f;
            return naive - poly_blep(phase, dt);
        }

        case WAVE_TRIANGLE: {
            float naive = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
            float shift_phase = (phase >= 0.5f) ? (phase - 0.5f) : (phase + 0.5f);
            /* PolyBLAMP 補正: 傾き跳び Δ=8、係数は数値較正により C=4·dt が最適
             * (C=8 は過大。dt 乗算欠落も不可。詳細は sub_common.h 参照) */
            return naive
                 + 4.0f * dt * poly_blamp(phase, dt)
                 - 4.0f * dt * poly_blamp(shift_phase, dt);
        }

        case WAVE_NOISE:
            return generate_noise_lfsr(&v->noise_seed);

        case WAVE_DRUM_KICK: {
            /* ピッチ急降下サイン波 + サチュレーション
             * (0.8 x 1.125 = 0.9 で値連続のソフトクリップ。
             *  旧式の分岐は |s|=0.8 で 0.96 <-> 0.90 の段差ノイズを生んでいた) */
            float s = lookup_sine(g_sine_lut, phase) * 1.125f;
            return fminf(fmaxf(s, -0.9f), 0.9f);
        }

        case WAVE_DRUM_SNARE: {
            /* 180Hz トーン + HP (~1.7kHz) 締めノイズ。
             * 生ノイズの低域ゴロつきを除去しスナップ感を強調 (sub4 と同一音作り) */
            float tone = lookup_sine(g_sine_lut, phase) * 0.4f;
            float n = generate_noise_lfsr(&v->noise_seed);
            v->shim_lp += 0.22f * (n - v->shim_lp);
            float noise = (n - v->shim_lp) * 0.85f;
            return tone + noise;
        }

        case WAVE_DRUM_HIHAT: {
            /* 高域ハイパス風ノイズ + HP (~2.4kHz) 整形。
             * 無フィルタだと中域がチャプチャプして安っぽく聴こえるため、
             * チッチッとした質感へ寄せる (sub4 と同一音作り) */
            float n = generate_noise_lfsr(&v->noise_seed) * 0.9f;
            v->shim_lp += 0.32f * (n - v->shim_lp);
            return n - v->shim_lp;
        }

        case WAVE_DRUM_CYMBAL: {
            /* メタリック倍音 + ノイズ。
             * 金属音は独立連続位相 metal_phase (3.7 倍速) で生成する。
             * 旧実装の phase * 3.7f は位相折返し (1.0 -> 0.0) 毎に
             * sin 値が不連続になり、基本周期ごとのバズを混入させていた */
            float n1 = generate_noise_lfsr(&v->noise_seed);
            float s1 = lookup_sine(g_sine_lut, v->metal_phase) * 0.3f;
            return (n1 * 0.7f + s1);
        }

        default:
            return 0.0f;
    }
}

/* ========================================================================= */
/* 3. ステレオリバーブ DSP エフェクト (Schroeder / Freeverb 構造)             */
/* ========================================================================= */
#if !defined(SYNTH_MULTICORE) || !SYNTH_MULTICORE
static const uint32_t s_comb_lengths_l[REVERB_NUM_COMBS] = { 1116, 1188, 1277, 1356 };
static const uint32_t s_comb_lengths_r[REVERB_NUM_COMBS] = { 1139, 1211, 1300, 1379 };
static const uint32_t s_allpass_lengths_l[REVERB_NUM_ALLPASS] = { 556, 441 };
static const uint32_t s_allpass_lengths_r[REVERB_NUM_ALLPASS] = { 579, 464 };
#endif

/* 遅延メモリは SynthEngine のメンバ (comb_mem / allpass_mem 各配列) に移管済み。
 * 旧静的配列は全インスタンスで共有され、2 インスタンス目の init が
 * 1 インスタンス目の残響を破壊するバグ源だったため廃止 */

/* ドラム (Kick/Tom) ピッチスイープの到達時間 (秒)。
 * 係数は「1 サンプルあたり」の移動量として使用する
 * (旧実装はブロック単位想定の 0.008 をサンプル毎に適用し、
 *  スイープが約 2.6ms で終わって固定音になっていた) */
#define DRUM_PITCH_SWEEP_SEC (0.045f)

static void init_reverb(SynthEngine *eng)
{
    ReverbEffect *rev = &eng->reverb;
    rev->enabled = false;
    rev->room_size = 0.75f;
    rev->damping = 0.35f;
    rev->wet_level = 0.30f;
    /* synth_engine_set_reverb と同一式 (dry = 1 - wet*0.3) */
    rev->dry_level = 1.0f - (rev->wet_level * 0.3f);

#if !defined(SYNTH_MULTICORE) || !SYNTH_MULTICORE
    /* リバーブ遅延メモリはシングルコア時のみ存在 (マルチコアでは構造体から除外) */
    float feedback = 0.70f + rev->room_size * 0.28f;
    float damp = rev->damping * 0.40f;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        rev->combs_l[i].buffer = eng->comb_mem_l[i];
        rev->combs_l[i].buf_size = s_comb_lengths_l[i];
        rev->combs_l[i].buf_idx = 0;
        rev->combs_l[i].feedback = feedback;
        rev->combs_l[i].filter_store = 0.0f;
        rev->combs_l[i].damp = damp;
        memset(eng->comb_mem_l[i], 0, sizeof(eng->comb_mem_l[i]));

        rev->combs_r[i].buffer = eng->comb_mem_r[i];
        rev->combs_r[i].buf_size = s_comb_lengths_r[i];
        rev->combs_r[i].buf_idx = 0;
        rev->combs_r[i].feedback = feedback;
        rev->combs_r[i].filter_store = 0.0f;
        rev->combs_r[i].damp = damp;
        memset(eng->comb_mem_r[i], 0, sizeof(eng->comb_mem_r[i]));
    }

    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        rev->allpass_l[i].buffer = eng->allpass_mem_l[i];
        rev->allpass_l[i].buf_size = s_allpass_lengths_l[i];
        rev->allpass_l[i].buf_idx = 0;
        rev->allpass_l[i].feedback = 0.5f;
        memset(eng->allpass_mem_l[i], 0, sizeof(eng->allpass_mem_l[i]));

        rev->allpass_r[i].buffer = eng->allpass_mem_r[i];
        rev->allpass_r[i].buf_size = s_allpass_lengths_r[i];
        rev->allpass_r[i].buf_idx = 0;
        rev->allpass_r[i].feedback = 0.5f;
        memset(eng->allpass_mem_r[i], 0, sizeof(eng->allpass_mem_r[i]));
    }
#endif
}

static inline float process_comb(ReverbComb *comb, float input)
{
    float output = comb->buffer[comb->buf_idx];
    comb->filter_store = (output * (1.0f - comb->damp)) + (comb->filter_store * comb->damp);
    /* デノーマル沈み込み防止: 残響尾部の微小値が x86 で
     * 数百サイクルの補助演算に落ちるのを防ぐ (FTZ 無効環境向け) */
    if (comb->filter_store > -1e-20f && comb->filter_store < 1e-20f) {
        comb->filter_store = 0.0f;
    }
    comb->buffer[comb->buf_idx] = input + (comb->filter_store * comb->feedback);
    if (++comb->buf_idx >= comb->buf_size) comb->buf_idx = 0;
    return output;
}

static inline float process_allpass(ReverbAllPass *ap, float input)
{
    float buf_out = ap->buffer[ap->buf_idx];
    float output = -input + buf_out;
    float stored = input + (buf_out * ap->feedback);
    if (stored > -1e-20f && stored < 1e-20f) stored = 0.0f;
    ap->buffer[ap->buf_idx] = stored;
    if (++ap->buf_idx >= ap->buf_size) ap->buf_idx = 0;
    return output;
}

static inline void process_reverb_core(ReverbEffect *rev, float in_l, float in_r, float *out_l, float *out_r)
{
    float mono_in = (in_l + in_r) * 0.5f;
    float comb_out_l = 0.0f;
    float comb_out_r = 0.0f;

    /* コムバンク入力を半減して内部 (湿音) 経路のヘッドルームを確保する。
     * 全経路が線形なため出力側の x2 で伝達特性は不変 */
    mono_in *= 0.5f;

    /* 4 基のコム出力を加算するため 1/4 に正規化 (無正規化だと +12dB で全通帯域がクリップ) */
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        comb_out_l += process_comb(&rev->combs_l[i], mono_in) * 0.25f;
        comb_out_r += process_comb(&rev->combs_r[i], mono_in) * 0.25f;
    }

    float ap_out_l = comb_out_l;
    float ap_out_r = comb_out_r;

    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        ap_out_l = process_allpass(&rev->allpass_l[i], ap_out_l);
        ap_out_r = process_allpass(&rev->allpass_r[i], ap_out_r);
    }

    *out_l = ap_out_l * rev->wet_level * 2.0f;
    *out_r = ap_out_r * rev->wet_level * 2.0f;
}

void synth_engine_set_reverb(SynthEngine *engine, bool enabled, float room_size, float damping, float wet_level)
{
    if (!engine) return;
    ReverbEffect *rev = &engine->reverb;
    rev->enabled = enabled;
    /* NaN は比較がすべて偽となるため素通しする。feedback/damp へ混入すると
     * コムフィルタ状態が恒久汚染されるため、範囲外値は飽和させて防ぐ */
    if (!(room_size >= 0.0f && room_size <= 1.0f)) room_size = (room_size > 1.0f) ? 1.0f : 0.0f;
    if (!(damping   >= 0.0f && damping   <= 1.0f)) damping   = (damping   > 1.0f) ? 1.0f : 0.0f;
    if (!(wet_level >= 0.0f && wet_level <= 1.0f)) wet_level = (wet_level > 1.0f) ? 1.0f : 0.0f;
    rev->room_size = room_size;
    rev->damping = damping;
    rev->wet_level = wet_level;
    rev->dry_level = 1.0f - (rev->wet_level * 0.3f);

    float feedback = 0.70f + rev->room_size * 0.28f;
    float damp = rev->damping * 0.40f;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        rev->combs_l[i].feedback = feedback;
        rev->combs_l[i].damp = damp;
        rev->combs_r[i].feedback = feedback;
        rev->combs_r[i].damp = damp;
    }
}

void synth_engine_set_reverb_enabled(SynthEngine *engine, bool enabled)
{
    if (!engine) return;
    engine->reverb.enabled = enabled;
}

/* ========================================================================= */
/* 4. MIDI周波数・エンベロープ・音色制御                                     */
/* ========================================================================= */
static float s_synth_freq_lut[128];
static int   s_synth_freq_lut_ready = 0;
static inline void synth_freq_lut_init(void)
{
    if (s_synth_freq_lut_ready) return;
    for (int i = 0; i < 128; i++) {
        s_synth_freq_lut[i] = 440.0f * powf(2.0f, ((float)i - 69.0f) / 12.0f);
    }
    s_synth_freq_lut_ready = 1;
}
float synth_note_to_freq(uint8_t note)
{
    if (!s_synth_freq_lut_ready) synth_freq_lut_init();
    return s_synth_freq_lut[note & 0x7F];
}

/**
 * @brief チャンネルのパン ゲイン表 (等価パワー) を再計算
 *        render 内の per-sample cosf/sinf を排除するための前計算。
 *        init / CC#10 変更時のみ呼ぶ
 */
static void engine_update_pan_gains(SynthEngine *engine, uint8_t channel)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return;
    float pan = engine->channels[channel].pan;
    if (!(pan >= 0.0f)) pan = 0.0f;   /* NaN/負値ガード */
    if (pan > 1.0f) pan = 1.0f;
    float ang = pan * ((float)M_PI * 0.5f);
    engine->ch_pan_cos[channel] = cosf(ang);
    engine->ch_pan_sin[channel] = sinf(ang);
}

void synth_engine_init(SynthEngine *engine)
{    if (!engine) return;
    synth_freq_lut_init();
    memset(engine, 0, sizeof(SynthEngine));
    engine->master_volume = 0.65f;
    engine->default_wave = WAVE_SQUARE;
    engine->lfsr_state = 0xACE1u;
    engine->dither_rng = 0x51ED2701u;

    /* デフォルトADSR */
    engine->default_adsr.attack_time_sec   = 0.005f;
    engine->default_adsr.decay_time_sec    = 0.080f;
    engine->default_adsr.sustain_level     = 0.600f;
    engine->default_adsr.release_time_sec  = 0.050f;
    engine->default_adsr.exponential_decay = true;

    /* 16チャンネルの初期化 */
    for (int ch = 0; ch < SYNTH_NUM_CHANNELS; ch++) {
        engine->channels[ch].program = 0; /* 0: Acoustic Piano */
        engine->channels[ch].volume = 0.8f;
        engine->channels[ch].expression = 1.0f;
        engine->channels[ch].pan = 0.5f; /* Center */
        engine->channels[ch].reverb_send = 0.3f;
        engine->channels[ch].pitch_bend_semitones = 0.0f;
        engine->channels[ch].sustain_pedal = false;
        engine_update_pan_gains(engine, (uint8_t)ch);
    }
    /* ドラムチャンネル (ch 9: 1-based Ch 10) */
    engine->channels[9].program = 0;
    engine->channels[9].volume = 0.9f;

    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        engine->voices[i].active = false;
        engine->voices[i].env_state = ENV_IDLE;
        engine->voices[i].current_env_level = 0.0f;
        engine->voices[i].age_samples = 0;
        engine->voices[i].noise_seed = 0xACE1u ^ (0x9E3779B9u * (uint32_t)(i + 1));
        engine->active_pos[i] = -1; /* 発音中リストも空に初期化 */
    }
    engine->num_active = 0;

    /* g_sine_lutはROM常数のため初期化不要 */
    init_reverb(engine);
}

void synth_engine_set_master_volume(SynthEngine *engine, float volume)
{
    if (!engine) return;
    if (!(volume >= 0.0f && volume <= 1.0f)) { /* NaN/Inf を含む異常値は飽和 */
        volume = (volume > 1.0f) ? 1.0f : 0.0f;
    }
    engine->master_volume = volume;
}

void synth_engine_program_change(SynthEngine *engine, uint8_t channel, uint8_t program)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return;
    engine->channels[channel].program = program;
}

static void begin_voice_release(SynthEngine *engine, SynthVoice *v);

/* 発音中リスト操作 (active-list)。render の走査対象を発音中のみに限定する。
 * 追加・削除とも O(1)。多重追加・多重削除はガードで無視する */
static inline void engine_voice_activate(SynthEngine *engine, int voice_idx)
{
    if (engine->active_pos[voice_idx] < 0) {
        engine->active_list[engine->num_active] = (uint8_t)voice_idx;
        engine->active_pos[voice_idx] = (int8_t)engine->num_active;
        engine->num_active++;
    }
}

static inline void engine_voice_deactivate(SynthEngine *engine, int voice_idx)
{
    int8_t pos = engine->active_pos[voice_idx];
    if (pos >= 0) {
        uint8_t last = engine->active_list[--engine->num_active];
        engine->active_list[pos] = last;
        engine->active_pos[last] = pos;
        engine->active_pos[voice_idx] = -1;
    }
}

void synth_engine_control_change(SynthEngine *engine, uint8_t channel, uint8_t control, uint8_t value){
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return;
    float norm = (float)value / 127.0f;

    switch (control) {
        case 7:  /* Volume (CC#7) */
            engine->channels[channel].volume = norm;
            break;
        case 10: /* Pan (CC#10) */
            engine->channels[channel].pan = norm;
            engine_update_pan_gains(engine, channel);
            break;
        case 11: /* Expression (CC#11) */
            engine->channels[channel].expression = norm;
            break;
        case 64: /* Sustain Pedal (CC#64) */
        {
            bool pedal_down = (value >= 64);
            engine->channels[channel].sustain_pedal = pedal_down;

            if (!pedal_down) {
                /* ペダル離解放: 延期されていたリリースをここで開始 */
                for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
                    SynthVoice *v = &engine->voices[i];
                    if (v->active && v->channel == channel && v->sustained_by_pedal) {
                        v->sustained_by_pedal = false;
                        if (v->env_state != ENV_IDLE) {
                            begin_voice_release(engine, v);
                        }
                    }
                }
            }
            break;
        }
        case 91: /* Reverb Send / Wet (CC#91) */
            engine->channels[channel].reverb_send = norm;
            break;
        case 120: /* All Sound Off (ハードミュート: ペダル無視・即時消音) */
            for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
                SynthVoice *v = &engine->voices[i];
                if (v->channel == channel && v->active) {
                    v->current_env_level = 0.0f;
                    v->env_state = ENV_IDLE;
                    v->active = false;
                    v->sustained_by_pedal = false;
                    engine_voice_deactivate(engine, i);
                }
            }
            break;
        case 123: /* All Notes Off (ペダル保持も強制リリース) */
            for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
                SynthVoice *v = &engine->voices[i];
                if (v->channel == channel && v->active &&
                    v->env_state != ENV_IDLE && v->env_state != ENV_RELEASE) {
                    v->sustained_by_pedal = false;
                    begin_voice_release(engine, v);
                }
            }
            break;
        default:
            break;
    }
}

void synth_engine_pitch_bend(SynthEngine *engine, uint8_t channel, int16_t bend_value)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return;
    /* -8192〜+8191 -> -2.0〜+2.0 半音 */
    engine->channels[channel].pitch_bend_semitones = ((float)bend_value / 8192.0f) * 2.0f;

    /* 発音中ボイスの周波数を即時更新 */
    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        if (engine->voices[i].active && engine->voices[i].channel == channel) {
            float base_freq = synth_note_to_freq(engine->voices[i].note);
            float bent_freq = base_freq * sub_semitone_ratio(engine->channels[channel].pitch_bend_semitones);
            engine->voices[i].frequency = bent_freq;
            engine->voices[i].phase_increment = bent_freq / (float)SYNTH_SAMPLE_RATE;
        }
    }
}

void synth_engine_retune_voice(SynthEngine *engine, uint8_t channel, uint8_t old_note, uint8_t new_note)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS || old_note == new_note) return;

    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        SynthVoice *v = &engine->voices[i];
        if (!v->active || v->channel != channel || v->note != old_note) continue;

        v->note = new_note;
        float base_freq = synth_note_to_freq(new_note);
        float bent_freq = base_freq * sub_semitone_ratio(engine->channels[channel].pitch_bend_semitones);
        v->frequency = bent_freq;
        v->phase_increment = bent_freq / (float)SYNTH_SAMPLE_RATE;
        /* エンベロープ状態・レベルはそのまま維持 (押し直し不要の滑かな音程移動) */
    }
}

/**
 * @brief GM プログラム群 -> 基本音色 (波形 + ADSR) マッピング表
 *        先頭から順に判定し、最初に一致した [prog_start, prog_end) を適用する。
 *        ※ 意図的な穴: 8-15 / 48-55 は最終の Default (Lead 系) へ落ちる
 *          (旧 if-else ラダーと同じ挙動を保持)
 */
typedef struct {
    uint8_t  prog_start;    /**< 適用開始プログラム (含む) */
    uint8_t  prog_end;      /**< 適用終了プログラム (含まない) */
    WaveType wave_type;
    float    attack_sec;
    float    decay_sec;
    float    sustain_level;
    float    release_sec;
} GmTimbreEntry;

static const GmTimbreEntry s_gm_timbre_table[] = {
    /*  範囲      波形              A       D       S       R   */
    {   0,   8, WAVE_TRIANGLE, 0.003f, 0.350f, 0.300f, 0.080f }, /* Piano: パーカッシブなディケイ */
    {  16,  24, WAVE_SINE,     0.010f, 0.050f, 0.900f, 0.040f }, /* Organ: サイン波 + 高サステイン */
    {  24,  32, WAVE_SAWTOOTH, 0.002f, 0.200f, 0.400f, 0.060f }, /* Guitar: ノコギリ + プラック */
    {  32,  40, WAVE_SQUARE,   0.003f, 0.150f, 0.700f, 0.050f }, /* Bass: 矩形波 + ファットなアタック */
    {  40,  48, WAVE_SAWTOOTH, 0.050f, 0.100f, 0.850f, 0.120f }, /* Strings: スロー攻撃 & 豊潤サステイン */
    {  56,  64, WAVE_SAWTOOTH, 0.020f, 0.080f, 0.800f, 0.060f }, /* Brass: ブラスアタック */
    {   0, 128, WAVE_SQUARE,   0.005f, 0.080f, 0.600f, 0.050f }, /* Default: Lead/Synth/Other */
};

static const GmTimbreEntry *gm_timbre_lookup(uint8_t prog)
{
    for (size_t i = 0; i < sizeof(s_gm_timbre_table) / sizeof(s_gm_timbre_table[0]); i++) {
        if (prog >= s_gm_timbre_table[i].prog_start && prog < s_gm_timbre_table[i].prog_end) {
            return &s_gm_timbre_table[i];
        }
    }
    return &s_gm_timbre_table[sizeof(s_gm_timbre_table) / sizeof(s_gm_timbre_table[0]) - 1];
}

int synth_engine_channel_note_on(SynthEngine *engine, uint8_t channel, uint8_t note, float velocity)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return -1;
    if (!(velocity > 0.0f)) { /* NaN/Inf/0/負 -> Note Off 扱い */
        synth_engine_channel_note_off(engine, channel, note);
        return -1;
    }
    if (velocity > 1.0f) velocity = 1.0f;

    /* 1. ボイス割り当て (ボイススチール優先度: 同音再利用 > 空き > リリース最小 > 最古/最小レベル) */
    int voice_idx = -1;
    bool same_note_reuse = false;

    /* A. 同一チャンネル・同一ノートの再利用 */
    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        if (engine->voices[i].active && engine->voices[i].channel == channel && engine->voices[i].note == note) {
            voice_idx = i;
            same_note_reuse = true;
            break;
        }
    }

    /* B. 空きボイス (IDLE) の探索 */
    if (voice_idx == -1) {
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (!engine->voices[i].active || engine->voices[i].env_state == ENV_IDLE) {
                voice_idx = i;
                break;
            }
        }
    }

    /* C. リリース中 (ENV_RELEASE) ボイスの中で最小レベルのものを探索 */
    if (voice_idx == -1) {
        float min_rel_level = 999.0f;
        int min_rel_idx = -1;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine->voices[i].env_state == ENV_RELEASE) {
                if (engine->voices[i].current_env_level < min_rel_level) {
                    min_rel_level = engine->voices[i].current_env_level;
                    min_rel_idx = i;
                }
            }
        }
        if (min_rel_idx != -1) {
            voice_idx = min_rel_idx;
        }
    }

    /* D. 全ボイス発音中の場合、最古ボイス (age_samples 最大) または最小レベルボイスをスチール */
    if (voice_idx == -1) {
        uint32_t max_age = 0;
        int oldest_idx = -1;

        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (!engine->voices[i].active) continue;
            /* 全ボイス age==0 の稀なケースで voice 0 固定にならないよう
             * 先頭のアクティブボイスを初期候補にする */
            if (oldest_idx == -1) oldest_idx = i;
            if (engine->voices[i].age_samples > max_age) {
                max_age = engine->voices[i].age_samples;
                oldest_idx = i;
            }
        }
        if (oldest_idx == -1) oldest_idx = 0;

        /* 極小音量ボイスを優先して切る。ただしアタック上昇中の新規ボイスを
         * 誤誤殺しないよう、RELEASE 中か発音から 100ms 経過した候補に限定 */
        const uint32_t kMinStealAge = SYNTH_SAMPLE_RATE / 10u;
        int quiet_idx = -1;
        float quiet_level = 999.0f;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            SynthVoice *vo = &engine->voices[i];
            bool eligible = (vo->env_state == ENV_RELEASE) || (vo->age_samples >= kMinStealAge);
            if (eligible && vo->current_env_level < quiet_level) {
                quiet_level = vo->current_env_level;
                quiet_idx = i;
            }
        }
        voice_idx = (quiet_idx != -1) ? quiet_idx : oldest_idx;
    }

    SynthVoice *v = &engine->voices[voice_idx];
    v->active = true;
    engine_voice_activate(engine, voice_idx);
    v->channel = channel;
    v->note = note;
    v->velocity = (velocity > 1.0f) ? 1.0f : velocity;
    /* 書き込み詰まり解消後に顕在化した低音鳴りっぱなし対策:
     * 同音連打で位相/レベルを引き継ぐと55Hz周期が途切れずドローン化する。
     * 低音(<48)で同音再利用時はハードリセットしてアタックを復活させる。 */
    bool low_hard = (note < 48) && same_note_reuse &&
                    (v->env_state == ENV_SUSTAIN || v->env_state == ENV_DECAY || v->env_state == ENV_ATTACK) &&
                    (v->current_env_level > 0.20f);
    /* 同音再トリガ時は位相も引き継ぎ、波形レベルで完全連続にする。
     * 別音程へのスチール時は位相をリセット (振幅は下記のレベル引継ぎで連続化) */
    if (!same_note_reuse || low_hard) {
        v->phase = 0.0f;
        v->metal_phase = 0.0f;
        v->shim_lp = 0.0f;
        /* per-voice decorrelated noise seed (Xorshift, 0除去) */
        uint32_t ns = engine->lfsr_state ^ (0x9E3779B9u * (uint32_t)(voice_idx + 1)) ^ ((uint32_t)note * 0x85EBCA6Bu);
        /* グローバルも1ステップ進めて次回と衝突させない */
        { uint32_t x = engine->lfsr_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; engine->lfsr_state = x ? x : 0xACE1u; ns ^= x; }
        v->noise_seed = ns ? ns : 0x12345678u;
    }
    v->age_samples = 0;
    v->sustained_by_pedal = false;

    /* 2. GM プログラム / ドラムに応じた音色とADSRの設定 */
    float base_freq = synth_note_to_freq(note);
    float bent_freq = base_freq * sub_semitone_ratio(engine->channels[channel].pitch_bend_semitones);
    v->frequency = bent_freq;
    v->phase_increment = bent_freq / (float)SYNTH_SAMPLE_RATE;
    v->start_frequency = 0.0f;
    v->target_frequency = 0.0f;

    if (channel == 9) {
        /* ================= GM Standard Drum Kit ================= */
        v->adsr.exponential_decay = true;
        if (note == 35 || note == 36) {
            /* Bass Drum / Kick: ピッチ急降下サイン波 (120Hz -> 48Hz, 約45ms掃引)
             * タイトで輪郭のあるパンチを狙う (sub4_drums と同一チューニング) */
            v->wave_type = WAVE_DRUM_KICK;
            v->start_frequency = 120.0f;
            v->target_frequency = 48.0f;
            v->frequency = 160.0f;
            v->phase_increment = 160.0f / (float)SYNTH_SAMPLE_RATE;
            v->adsr.attack_time_sec  = 0.002f;
            v->adsr.decay_time_sec   = 0.140f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.040f;
        } else if (note == 38 || note == 40) {
            /* Snare Drum: 180Hz サイン波トーン + ノイズスネア */
            v->wave_type = WAVE_DRUM_SNARE;
            v->frequency = 180.0f;
            v->phase_increment = 180.0f / (float)SYNTH_SAMPLE_RATE;
            v->adsr.attack_time_sec  = 0.002f;
            v->adsr.decay_time_sec   = 0.120f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.030f;
        } else if (note == 42 || note == 44) {
            /* Closed / Pedal Hi-Hat: 超短時間ハイパスノイズ */
            v->wave_type = WAVE_DRUM_HIHAT;
            v->adsr.attack_time_sec  = 0.001f;
            v->adsr.decay_time_sec   = 0.035f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.020f;
        } else if (note == 46) {
            /* Open Hi-Hat: 220ms ハイパスノイズ */
            v->wave_type = WAVE_DRUM_HIHAT;
            v->adsr.attack_time_sec  = 0.001f;
            v->adsr.decay_time_sec   = 0.220f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.050f;
        } else if (note >= 41 && note <= 50 && note != 49) {
            /* Toms (Low/Mid/High): トーン急降下サイン波
             * ※ GM 49 (Crash Cymbal) はトム範囲から除外してシンバル合成へ */
            v->wave_type = WAVE_DRUM_KICK;
            float tom_f = 90.0f + (float)(note - 41) * 15.0f;
            v->start_frequency = tom_f * 1.5f;
            v->target_frequency = tom_f;
            v->frequency = v->start_frequency;
            v->phase_increment = v->frequency / (float)SYNTH_SAMPLE_RATE;
            v->adsr.attack_time_sec  = 0.002f;
            v->adsr.decay_time_sec   = 0.160f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.040f;
        } else {
            /* Crash Cymbal (49, 57) / Ride / Other: ロングメタリックノイズ */
            v->wave_type = WAVE_DRUM_CYMBAL;
            v->adsr.attack_time_sec  = 0.002f;
            v->adsr.decay_time_sec   = 0.650f;
            v->adsr.sustain_level    = 0.0f;
            v->adsr.release_time_sec = 0.120f;
        }
    } else {
        /* ================= GM メロディ楽器 ================= */
        uint8_t prog = engine->channels[channel].program;
        v->adsr = engine->default_adsr;

        const GmTimbreEntry *timbre = gm_timbre_lookup(prog);
        v->wave_type             = timbre->wave_type;
        v->adsr.attack_time_sec  = timbre->attack_sec;
        v->adsr.decay_time_sec   = timbre->decay_sec;
        v->adsr.sustain_level    = timbre->sustain_level;
        v->adsr.release_time_sec = timbre->release_sec;
    }

    /* 3. エンベロープステップ事前計算 */
    float sample_rate = (float)SYNTH_SAMPLE_RATE;
    uint32_t attack_samples  = (uint32_t)(v->adsr.attack_time_sec * sample_rate);
    uint32_t decay_samples   = (uint32_t)(v->adsr.decay_time_sec * sample_rate);

    v->attack_step = (attack_samples > 0) ? (1.0f / (float)attack_samples) : 1.0f;
    float decay_diff = 1.0f - v->adsr.sustain_level;
    v->decay_step = (decay_samples > 0) ? (decay_diff / (float)decay_samples) : decay_diff;

    if (v->adsr.exponential_decay && decay_samples > 0) {
        v->decay_coeff = expf(-6.907755f / (float)decay_samples);
    } else {
        v->decay_coeff = 0.0f;
    }

    v->release_start_level = 0.0f;
    v->release_step = 0.0f;
    v->release_coeff = 0.0f;
    /* スチール/再トリガ元のエンベロープレベルを引き継ぐ。
     * 旧実装のハードリセット (=0) は直前サンプルとのフルスケール段差を
     * 生み出しクリックの原因だった。振幅を連続させたままアタックへ移行する
     * ただし低音の同音連打ではドローン化するため low_hard時は0リセット */
    if (low_hard) {
        v->current_env_level = 0.0f;
    } else if (!(v->current_env_level >= 0.0f) || v->current_env_level > 1.0f) {
        v->current_env_level = 0.0f; /* NaN/異常値ガード */
    }
    v->env_samples = 0;
    v->phase_max_samples = attack_samples;

    if (attack_samples > 0) {
        v->env_state = ENV_ATTACK;
    } else if (decay_samples > 0) {
        v->current_env_level = 1.0f;
        v->env_state = ENV_DECAY;
        v->phase_max_samples = decay_samples;
    } else {
        v->current_env_level = v->adsr.sustain_level;
        v->env_state = ENV_SUSTAIN;
        v->phase_max_samples = 0;
    }

    return voice_idx;
}

int synth_engine_note_on(SynthEngine *engine, uint8_t note, float velocity, WaveType wave_type)
{
    int v_idx = synth_engine_channel_note_on(engine, 0, note, velocity);
    if (v_idx >= 0 && wave_type <= WAVE_NOISE) {
        SynthVoice *vv = &engine->voices[v_idx];
        if (vv->wave_type != wave_type) {
            vv->wave_type = wave_type;
            if (wave_type == WAVE_NOISE) {
                /* GM Piano ADSR(0.3 sustain)で測定用ノイズが痩せるのを防ぐ。
                 * default_adsrを尊重してベンチマークの sustain=1 を維持 */
                vv->adsr = engine->default_adsr;
                float sr = (float)SYNTH_SAMPLE_RATE;
                uint32_t as = (uint32_t)(vv->adsr.attack_time_sec * sr);
                uint32_t ds = (uint32_t)(vv->adsr.decay_time_sec * sr);
                vv->attack_step = as ? 1.0f/(float)as : 1.0f;
                float dd = 1.0f - vv->adsr.sustain_level;
                vv->decay_step = ds ? dd/(float)ds : dd;
                vv->decay_coeff = (ds && vv->adsr.exponential_decay) ? expf(-6.907755f/(float)ds) : 0.0f;
                vv->phase_max_samples = as;
                vv->env_state = as ? ENV_ATTACK : (ds ? ENV_DECAY : ENV_SUSTAIN);
                if (!as && ds) vv->current_env_level = 1.0f;
                else if (!as && !ds) vv->current_env_level = vv->adsr.sustain_level;
                vv->env_samples = 0;
            }
        }
    }
    return v_idx;
}

int synth_engine_channel_note_on_w(SynthEngine *engine, uint8_t channel, uint8_t note, float velocity, WaveType wave_type)
{
    if (channel == 9) {
        /* ドラムチャンネル: GM ノートマッピングを優先 (波形上書きしない) */
        return synth_engine_channel_note_on(engine, channel, note, velocity);
    }
    int v_idx = synth_engine_channel_note_on(engine, channel, note, velocity);
    if (v_idx >= 0 && wave_type <= WAVE_NOISE) {
        SynthVoice *vv = &engine->voices[v_idx];
        if (vv->wave_type != wave_type) {
            vv->wave_type = wave_type;
            if (wave_type == WAVE_NOISE) {
                vv->adsr = engine->default_adsr;
                float sr = (float)SYNTH_SAMPLE_RATE;
                uint32_t as = (uint32_t)(vv->adsr.attack_time_sec * sr);
                uint32_t ds = (uint32_t)(vv->adsr.decay_time_sec * sr);
                vv->attack_step = as ? 1.0f/(float)as : 1.0f;
                float dd = 1.0f - vv->adsr.sustain_level;
                vv->decay_step = ds ? dd/(float)ds : dd;
                vv->decay_coeff = (ds && vv->adsr.exponential_decay) ? expf(-6.907755f/(float)ds) : 0.0f;
                vv->phase_max_samples = as;
                vv->env_state = as ? ENV_ATTACK : (ds ? ENV_DECAY : ENV_SUSTAIN);
                if (!as && ds) vv->current_env_level = 1.0f;
                else if (!as && !ds) vv->current_env_level = vv->adsr.sustain_level;
                vv->env_samples = 0;
            }
        }
    }
    return v_idx;
}

/**
 * @brief ボイスのリリースフェーズ開始 (内部ヘルパー)
 */
static void begin_voice_release(SynthEngine *engine, SynthVoice *v)
{
    (void)engine; /* 将来のエンジン依存パラメータ拡張用プレースホルダ */
    v->release_start_level = v->current_env_level;

    if (v->release_start_level <= 0.0005f) {
        v->current_env_level = 0.0f;
        v->env_state = ENV_IDLE;
        v->active = false;
        engine_voice_deactivate(engine, (int)(v - engine->voices));
        return;
    }

    uint32_t release_samples = (uint32_t)(v->adsr.release_time_sec * (float)SYNTH_SAMPLE_RATE);
    if (release_samples == 0) {
        v->current_env_level = 0.0f;
        v->env_state = ENV_IDLE;
        v->active = false;
        engine_voice_deactivate(engine, (int)(v - engine->voices));
        return;
    }

    v->release_step = v->release_start_level / (float)release_samples;
    if (v->adsr.exponential_decay) {
        v->release_coeff = expf(-7.6009f / (float)release_samples);
    } else {
        v->release_coeff = 0.0f;
    }
    v->env_state = ENV_RELEASE;
    v->env_samples = 0;
    v->phase_max_samples = release_samples;
}

void synth_engine_channel_note_off(SynthEngine *engine, uint8_t channel, uint8_t note)
{
    if (!engine || channel >= SYNTH_NUM_CHANNELS) return;

    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        if (engine->voices[i].active && engine->voices[i].channel == channel && engine->voices[i].note == note) {
            SynthVoice *v = &engine->voices[i];

            /* リリース済み/停止済みボイスの再リリースでテールが
             * 伸びるのを防ぐ (sub コア実装と同一のガード)。
             * ※ このガードをペダル分岐より先に評価しないと、リリース中ボイスが
             *   sustained_by_pedal 化してペダル離脱時に再リリースされていた */
            if (v->env_state == ENV_RELEASE || v->env_state == ENV_IDLE) {
                continue;
            }
            /* ダンパーペダル (CC#64) 押下中はリリースを延期して保持する。
             * 重複 Note Off が来てもペダル優先 */
            if (engine->channels[channel].sustain_pedal) {
                v->sustained_by_pedal = true;
                continue;
            }
            begin_voice_release(engine, v);
        }
    }
}

void synth_engine_note_off(SynthEngine *engine, uint8_t note)
{
    if (!engine) return;
    for (int ch = 0; ch < SYNTH_NUM_CHANNELS; ch++) {
        synth_engine_channel_note_off(engine, (uint8_t)ch, note);
    }
}

void synth_engine_all_notes_off(SynthEngine *engine)
{
    if (!engine) return;

    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        engine->voices[i].active = false;
        engine->voices[i].env_state = ENV_IDLE;
        engine->voices[i].current_env_level = 0.0f;
        engine->voices[i].release_start_level = 0.0f;
        engine->voices[i].release_step = 0.0f;
        engine->voices[i].release_coeff = 0.0f;
        engine->voices[i].age_samples = 0;
        engine->voices[i].sustained_by_pedal = false;
        engine->active_pos[i] = -1;
    }
    engine->num_active = 0;
    /* ペダル状態も解除しないと曲替え後にノートが解放されない */
    for (int ch = 0; ch < SYNTH_NUM_CHANNELS; ch++) {
        engine->channels[ch].sustain_pedal = false;
    }
}

void synth_engine_reset_effects(SynthEngine *engine)
{
    if (!engine) return;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        if (engine->reverb.combs_l[i].buffer) {
            memset(engine->reverb.combs_l[i].buffer, 0, sizeof(float) * engine->reverb.combs_l[i].buf_size);
            engine->reverb.combs_l[i].buf_idx = 0;
            engine->reverb.combs_l[i].filter_store = 0.0f;
        }
        if (engine->reverb.combs_r[i].buffer) {
            memset(engine->reverb.combs_r[i].buffer, 0, sizeof(float) * engine->reverb.combs_r[i].buf_size);
            engine->reverb.combs_r[i].buf_idx = 0;
            engine->reverb.combs_r[i].filter_store = 0.0f;
        }
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        if (engine->reverb.allpass_l[i].buffer) {
            memset(engine->reverb.allpass_l[i].buffer, 0, sizeof(float) * engine->reverb.allpass_l[i].buf_size);
            engine->reverb.allpass_l[i].buf_idx = 0;
        }
        if (engine->reverb.allpass_r[i].buffer) {
            memset(engine->reverb.allpass_r[i].buffer, 0, sizeof(float) * engine->reverb.allpass_r[i].buf_size);
            engine->reverb.allpass_r[i].buf_idx = 0;
        }
    }
}

static inline float update_envelope(SynthVoice *v)
{
    if (v->env_state == ENV_IDLE) {
        return 0.0f;
    }

    switch (v->env_state) {
        case ENV_ATTACK:
            v->current_env_level += v->attack_step;
            v->env_samples++;
            if (v->current_env_level >= 1.0f || v->env_samples >= v->phase_max_samples) {
                v->current_env_level = 1.0f;
                uint32_t decay_samples = (uint32_t)(v->adsr.decay_time_sec * (float)SYNTH_SAMPLE_RATE);
                if (decay_samples > 0) {
                    v->env_state = ENV_DECAY;
                    v->env_samples = 0;
                    v->phase_max_samples = decay_samples;
                } else {
                    v->current_env_level = v->adsr.sustain_level;
                    v->env_state = ENV_SUSTAIN;
                    v->phase_max_samples = 0;
                }
            }
            break;

        case ENV_DECAY:
            v->env_samples++;
            if (v->adsr.exponential_decay) {
                float diff = v->current_env_level - v->adsr.sustain_level;
                v->current_env_level = v->adsr.sustain_level + diff * v->decay_coeff;
            } else {
                v->current_env_level -= v->decay_step;
            }

            if (v->env_samples >= v->phase_max_samples || v->current_env_level <= v->adsr.sustain_level + 0.0005f) {
                v->current_env_level = v->adsr.sustain_level;
                v->env_state = ENV_SUSTAIN;
                v->phase_max_samples = 0;
            }
            break;

        case ENV_SUSTAIN:
            v->current_env_level = v->adsr.sustain_level;
            /* サステイン 0 (ドラム等) のボイスが SUSTAIN に滞留して
             * レンダリングループを永久消費しないよう解放する */
            if (v->adsr.sustain_level <= 0.0005f) {
                v->current_env_level = 0.0f;
                v->env_state = ENV_IDLE;
                v->active = false;
            }
            break;

        case ENV_RELEASE:
            v->env_samples++;
            if (v->adsr.exponential_decay) {
                v->current_env_level *= v->release_coeff;
            } else {
                v->current_env_level -= v->release_step;
            }

            if (v->current_env_level <= 0.0005f || v->env_samples >= v->phase_max_samples) {
                v->current_env_level = 0.0f;
                v->env_state = ENV_IDLE;
                v->active = false;
            }
            break;

        default:
            v->current_env_level = 0.0f;
            v->env_state = ENV_IDLE;
            v->active = false;
            break;
    }

    return v->current_env_level;
}

/* float [-1,1] -> int16 (対称丸め)。
 * 単純切り捨ては負側に -0.5LSB の DC バイアスを作るため
 * 0 から遠ざかる方向へ 0.5 を足してから切る (half away from zero) */
static inline int16_t sat_s16_round(float x)
{
    x += (x >= 0.0f) ? 0.5f : -0.5f;
    if (x > 32767.0f)  x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    return (int16_t)x;
}

void synth_engine_render(SynthEngine *engine, int16_t *buffer, uint32_t frames)
{
    if (!engine || !buffer || frames == 0) return;

    for (uint32_t i = 0; i < frames; i++) {
        float dry_l = 0.0f;
        float dry_r = 0.0f;
        float rev_send_l = 0.0f;
        float rev_send_r = 0.0f;

        /* 発音中ボイスのみ走査 (active-list)。
         * 旧 64 ボイス線形走査では非発音スロットの分岐・メモリ参照が
         * 毎サンプル 64 回発生していた。追加・削除は O(1) */
        uint8_t k = 0;
        while (k < engine->num_active) {
            int v_idx = engine->active_list[k];
            SynthVoice *v = &engine->voices[v_idx];
            if (!v->active) {
                /* 他経路で停止済みの残留エントリを回収 (slot k は次候補) */
                engine_voice_deactivate(engine, v_idx);
                continue;
            }

            v->age_samples++;

            /* ドラムピッチスイープ (Kick / Tom): 45ms で目標周波数まで降下 */
            if (v->wave_type == WAVE_DRUM_KICK && v->target_frequency > 0.0f) {
                if (v->frequency > v->target_frequency) {
                    v->frequency -= (v->start_frequency - v->target_frequency) *
                                    (1.0f / (DRUM_PITCH_SWEEP_SEC * (float)SYNTH_SAMPLE_RATE));
                    if (v->frequency < v->target_frequency) v->frequency = v->target_frequency;
                    v->phase_increment = v->frequency / (float)SYNTH_SAMPLE_RATE;
                }
            }

            float raw_sample = calculate_oscillator(v);
            float env = update_envelope(v);

            if (env > 0.0f) {
                uint8_t ch = v->channel;
                float ch_vol = engine->channels[ch].volume * engine->channels[ch].expression;
                /* 等価パワー パン (CC#10 変更時に前計算済みのゲイン表を参照。
                 * 旧実装はボイス×サンプル毎に cosf/sinf を呼んでいた) */
                float pan_l = engine->ch_pan_cos[ch];
                float pan_r = engine->ch_pan_sin[ch];
                float ch_rev = engine->channels[ch].reverb_send;

                float voice_sample = raw_sample * env * v->velocity * ch_vol;
                float voice_l = voice_sample * pan_l;
                float voice_r = voice_sample * pan_r;

                dry_l += voice_l;
                dry_r += voice_r;
                rev_send_l += voice_l * ch_rev;
                rev_send_r += voice_r * ch_rev;
            }

            /* 位相更新: floorf排除、inc<0.5保証で単一減算で十分 */
            v->phase += v->phase_increment;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
            /* シンバル金属音の独立位相 (3.7 倍速、連続) */
            if (v->wave_type == WAVE_DRUM_CYMBAL) {
                v->metal_phase += v->phase_increment * 3.7f;
                if (v->metal_phase >= 1.0f) {
                    v->metal_phase -= 1.0f;
                    if (v->metal_phase >= 1.0f) v->metal_phase -= 1.0f;
                    if (v->metal_phase >= 1.0f) v->metal_phase -= 1.0f;
                }
            }

            if (v->active) {
                k++;
            } else {
                /* エンベロープ満了で停止: リストから除去 (slot k は次候補) */
                engine_voice_deactivate(engine, v_idx);
            }
        }

        /* マスターボリューム適用 */
        dry_l *= engine->master_volume;
        dry_r *= engine->master_volume;
        rev_send_l *= engine->master_volume;
        rev_send_r *= engine->master_volume;

        /* ステレオリバーブエフェクト処理 */
        float out_l = dry_l;
        float out_r = dry_r;
        if (engine->reverb.enabled) {
            float rev_out_l = 0.0f;
            float rev_out_r = 0.0f;
            process_reverb_core(&engine->reverb, rev_send_l, rev_send_r, &rev_out_l, &rev_out_r);
            out_l = dry_l * engine->reverb.dry_level + rev_out_l;
            out_r = dry_r * engine->reverb.dry_level + rev_out_r;
        }

        /* ソフトクリッパ: |x|<=1 は線形、|x|>1 は単調クランプ。
         * (旧 3x-x^3/2 多項式は x>1 で非単調かつ x=1.5 で 0.56->1.0 の不連続ジャンプ =
         *  クリックノイズを生成するため廃止。x=1 で値連続を保証) */
        if (out_l > 1.0f)  out_l = 1.0f;
        if (out_l < -1.0f) out_l = -1.0f;
        if (out_r > 1.0f)  out_r = 1.0f;
        if (out_r < -1.0f) out_r = -1.0f;

        /* TPDF ディザ付き 16bit 量子化 (SubCore 5 と同一の量子化ポリシー) */
        buffer[i * 2 + 0] = sub_quantize_dither(out_l, &engine->dither_rng);
        buffer[i * 2 + 1] = sub_quantize_dither(out_r, &engine->dither_rng);
    }
}

void synth_engine_render_impulse_response(SynthEngine *engine, int16_t *buffer, uint32_t frames)
{
    if (!engine || !buffer || frames == 0) return;

    for (uint32_t i = 0; i < frames; i++) {
        float input = (i == 0) ? 1.0f : 0.0f;
        float out_l = input;
        float out_r = input;

        if (engine->reverb.enabled) {
            float rev_out_l = 0.0f;
            float rev_out_r = 0.0f;
            process_reverb_core(&engine->reverb, input, input, &rev_out_l, &rev_out_r);
            out_l = input * engine->reverb.dry_level + rev_out_l;
            out_r = input * engine->reverb.dry_level + rev_out_r;
        }

        if (out_l > 1.0f) out_l = 1.0f;
        if (out_l < -1.0f) out_l = -1.0f;
        if (out_r > 1.0f) out_r = 1.0f;
        if (out_r < -1.0f) out_r = -1.0f;

        buffer[i * 2 + 0] = sub_quantize_dither(out_l, &engine->dither_rng);
        buffer[i * 2 + 1] = sub_quantize_dither(out_r, &engine->dither_rng);
    }
}
