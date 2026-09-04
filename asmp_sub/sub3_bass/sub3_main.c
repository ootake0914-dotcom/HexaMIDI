/**
 * @file sub3_main.c
 * @brief SubCore 3: ベース・ストリングス・コード音源 (8ボイス)
 * @details 8音ポリフォニック合成、Q32 サブオシレータ、64フレーム float タイル蓄積、3連 FMA SVF フィルター
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#define sub3_sleep_us(us) Sleep((DWORD)((us) / 1000))
#elif defined(__NuttX__)
#include <nuttx/arch.h>
#define sub3_sleep_us(us) up_udelay((useconds_t)(us))
#else
#include <unistd.h>
#define sub3_sleep_us(us) usleep(us)
#endif

#include "sub_common.h"
#include "sub_spawn.h"
#include "rt_profile.h"
#include "spatial_audio.h"
#include "sub_asm.h"
#include "sub_kick.h" /* Phase 4: Kick (note 35/36) の動的移行先ホスト用 専用ドラム */
#include "sub_metal.h" /* Phase 5: Metal (HiHat/Cymbal) の動的移行先ホスト用 専用ドラム */
#include "sub_perc.h" /* Phase 6: Perc (Snare/Clap/Tom) の動的移行先ホスト用 専用ドラム */

#define SUB3_MAX_POLYPHONY (16)
#define SUB3_FILTER_ENV_TIME (0.28f)  /* LPF エンベロープ減衰時定数 (秒) */
#define SUB3_MIX_TILE (128u)          /* float タイル蓄積バッファ長 (128フレーム: 64->128でタイル数8->4、オーバーヘッド半減。stack 512->1024B増のみ) */

typedef enum {
    WAVE3_SINE = 0,
    WAVE3_SQUARE,
    WAVE3_SAWTOOTH,
    WAVE3_TRIANGLE
} WaveType3;

/**
 * @brief Q32 整数位相から分岐レスで矩形波 (-1.0f / +1.0f) を生成 (帯域制限なし)
 * @details 算術右シフト (符号拡張) と OR 演算により、ARM 命令 2 サイクル (ASR + ORR + VCVT) で実行
 * @note 現在は未使用 (旧サブオシレータ実装。sq_q32_blep に置換済み)。
 *       互換のため残置するが、新規呼び出しは追加しないこと
 */
static inline float sq_q32(uint32_t p)
{
    int32_t s = (int32_t)p >> 31;
    return (float)(s | 1);
}

/**
 * @brief Q32 整数位相の矩形波を PolyBLEP で帯域制限して生成 (#音質改善B)
 * @details 旧 sq_q32 はメインオシレータの sub_osc_square と異なり無補正のため、
 *          -1oct/-2oct のベース サブオシレータが高域エイリアシングノイズ源に
 *          なっていた (特にベース高音域でジリジリした金属的な濁りとして可聴)。
 *          メインオシレータと同一の PolyBLEP 補正を適用し無音時と同じコスト構造
 *          (分岐 2 回) に揃える。dt は呼び出し側でオクターブに応じて渡すこと
 */
static inline float sq_q32_blep(uint32_t p, float dt)
{
    const float Q32_INV = 1.0f / 4294967296.0f;
    float naive = (p & 0x80000000u) ? 1.0f : -1.0f;
    float ph = (float)p * Q32_INV;
    float shift = (ph >= 0.5f) ? (ph - 0.5f) : (ph + 0.5f);
    return naive + sub_poly_blep(ph, dt) - sub_poly_blep(shift, dt);
}

typedef struct {
    bool        active;
    uint8_t     channel;
    uint8_t     note;
    float       velocity;
    float       frequency;
    uint32_t    phase_q;          /* メインオシレータ Q32 位相 */
    uint32_t    inc_q;            /* Q32 位相増分 */
    uint32_t    sub1_phase_q;     /* サブオシ #1 (-1 Oct) Q32 位相 */
    uint32_t    sub2_phase_q;     /* サブオシ #2 (-2 Oct) Q32 位相 */
    bool        is_bass;          /* ベース音色フラグ (サブオシ加算) */
    WaveType3   wave_type;

    SubEnvCore  env;              /* 共通 ADSR コア (sub_common.h) */
    uint32_t    age_samples;

    /* ペルボイス 共振LPF + フィルターエンベロープ */
    float       filt_cutoff_base;
    float       filt_peak_hz;
    float       filt_env;
    float       filt_env_coeff;
    float       filt_env_coeff64;  /* 64 サンプル一括減衰係数 (タイル更新用) */
    SubSvf      svf;
    float       svf_q;             /* 音色別共振 Q (再計算時も保持) */
    float       base_increment;    /* ベンド/LFO 適用前の基本位相増分 */
    float       vib_phase;         /* LFO ビブラート位相 */
    float       vib_inc;           /* LFO 位相増分 (~4.6Hz) */
    float       vib_depth_st;      /* ビブラート深さ (半音, CC#1) */
    SpatialVoiceState spatial;
} VoiceSub3;

typedef struct {
    AsmpSharedContext *shared;
    VoiceSub3   voices[SUB3_MAX_POLYPHONY];
    SubChannel  channels[16];
    uint32_t    lfsr_state;
    /* バス集約重低音ラムブル除去 HP (L/R)。per-voice HP と同一 38Hz/Q0.7 を
     * ミックス後に 1 回だけ適用する (LTI 可換により等価) */
    SubSvf      bus_hp_l;
    SubSvf      bus_hp_r;
} Sub3BassEngine;

static Sub3BassEngine s_sub3;
static bool s_sub3_hp_bypass = false;
static SubKickEngine s_sub3_kick; /* Phase 4: C4 から移行された Kick のホスト状態 */
static SubMetalEngine s_sub3_metal; /* Phase 5: C4 から移行された Metal のホスト状態 */
static SubPercEngine s_sub3_perc; /* Phase 6: C4 から移行された Perc のホスト状態 */

/* 256->512 (+3KB)。密集譜面のNOTE/CCロストによる音痩せ・処理落ち連鎖を防止 */
#define SUB3_MAX_PENDING 512
static AsmpPacket s_sub3_pending[SUB3_MAX_PENDING];
static uint32_t s_sub3_pending_cnt = 0;

/**
 * @brief SubCore 3 エンジン初期化
 */
static void sub3_engine_init(Sub3BassEngine *eng, AsmpSharedContext *shared)
{
    memset(eng, 0, sizeof(Sub3BassEngine));
    eng->shared = shared;
    eng->lfsr_state = 0x5678DEF0u;
    /* g_sine_lut const, no init */
    sub_freq_lut_init();
    sub_exp_lut_init();
    sub_tan_lut_init((float)SUB_SAMPLE_RATE);
    /* バス集約 HP 初期化 (per-voice HP と同一特性 38Hz/Q0.7。連続動作) */
    sub_svf_reset(&eng->bus_hp_l);
    sub_svf_reset(&eng->bus_hp_r);
    sub_svf_set(&eng->bus_hp_l, 38.0f, 0.70f, (float)SUB_SAMPLE_RATE);
    sub_svf_set(&eng->bus_hp_r, 38.0f, 0.70f, (float)SUB_SAMPLE_RATE);
    sub_kick_init(&s_sub3_kick); /* Phase 4: Kick ホスト初期化 (noise/volume 独立) */
    sub_metal_init(&s_sub3_metal); /* Phase 5: Metal ホスト初期化 (noise/free_ph 独立) */
    sub_perc_init(&s_sub3_perc); /* Phase 6: Perc ホスト初期化 (noise/roll 独立) */

    for (int ch = 0; ch < 16; ch++) {
        eng->channels[ch].program = 33; /* 33: Electric Bass (Finger) */
        eng->channels[ch].volume = 0.85f;
        eng->channels[ch].expression = 1.0f;
        eng->channels[ch].pan = 0.5f;
        eng->channels[ch].pitch_bend_semitones = 0.0f;
        eng->channels[ch].mod_depth = 0.0f;
        eng->channels[ch].reverb_send = 0.3f;
        eng->channels[ch].sustain_pedal = false;
        sub_channel_update_pan_gains(&eng->channels[ch]);
    }
}

static void sub3_note_off(Sub3BassEngine *eng, uint8_t channel, uint8_t note);

/**
 * @brief ノートオン
 */
static int sub3_voice_alloc(Sub3BassEngine *eng, uint8_t channel, uint8_t note)
{
    int voice_idx = -1;
    for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
        if (eng->voices[i].active && eng->voices[i].channel == channel && eng->voices[i].note == note) {
            voice_idx = i;
            break;
        }
    }
    if (voice_idx == -1) {
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            if (!eng->voices[i].active || eng->voices[i].env.env_state == SUB_ENV_IDLE) {
                voice_idx = i;
                break;
            }
        }
    }
    if (voice_idx == -1) {
        float min_level = 999.0f;
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            if (eng->voices[i].env.env_state == SUB_ENV_RELEASE && eng->voices[i].env.current_env_level < min_level) {
                min_level = eng->voices[i].env.current_env_level;
                voice_idx = i;
            }
        }
    }
    /* 最古ボイススチール。極小音量ボイスを優先して切り、
     * アタック上昇中の新規ボイスは保護する (RELEASE 中か発音 100ms 経過のみ対象) */
    if (voice_idx == -1) {
        uint32_t max_age = 0;
        int oldest_idx = -1;
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            if (!eng->voices[i].active) continue;
            if (oldest_idx < 0) oldest_idx = i;
            if (eng->voices[i].age_samples > max_age) {
                max_age = eng->voices[i].age_samples;
                oldest_idx = i;
            }
        }
        if (oldest_idx < 0) oldest_idx = 0;

        const uint32_t kMinStealAge = SUB_SAMPLE_RATE / 10u;
        int quiet_idx = -1;
        float quiet_level = 999.0f;
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            VoiceSub3 *vo = &eng->voices[i];
            if (!vo->active) continue;
            bool eligible = (vo->env.env_state == SUB_ENV_RELEASE) || (vo->age_samples >= kMinStealAge);
            if (eligible && vo->env.current_env_level < quiet_level) {
                quiet_level = vo->env.current_env_level;
                quiet_idx = i;
            }
        }
        voice_idx = (quiet_idx >= 0) ? quiet_idx : oldest_idx;
    }
    return voice_idx;
}

/**
 * @brief ディスクリプタからの発音実体化 (ABI v13 fast-spawn)
 * @details 正準 build の出力 + live 状態 + 初期位相でボイスを初期化する。
 *          係数変換 (exp/tan LUT) のみ演奏コア側で行う。float 式は従来同一。
 */
static void sub3_spawn_from_desc(Sub3BassEngine *eng, VoiceSub3 *v, const SubSpawnDesc *d)
{
    uint8_t channel = d->channel;
    float velocity = (float)d->velocity / 127.0f;
    v->active = true;
    v->channel = channel;
    v->note = d->note;
    v->velocity = velocity;
    v->env.sustained_by_pedal = false;
    v->phase_q = 0;
    v->sub1_phase_q = 0;
    v->sub2_phase_q = 0;
    v->age_samples = 0;

    /* base_increment は必ず「ベンドなし」の値を保存する。
     * レンダーループが毎回 ch->pitch_bend_semitones を掛けるため、
     * ここでベンド込みの値を保存するとベンドが二重適用される */
    v->frequency = sub_note_to_freq(d->note);
    v->base_increment = d->base_increment;
    {
        const float Q32 = 4294967296.0f;
        v->inc_q = (uint32_t)(d->base_increment * Q32);
    }

    v->is_bass = (d->is_bass != 0u);
    v->wave_type = (WaveType3)d->wave;
    v->env.adsr.attack_time_sec = d->adsr_a;
    v->env.adsr.decay_time_sec = d->adsr_d;
    v->env.adsr.sustain_level = d->adsr_s;
    v->env.adsr.release_time_sec = d->adsr_r;
    v->env.adsr.exponential_decay = (d->exp_decay != 0u);

    /* 共通 ADSR コアで発音準備 (ATTACK 開始) */
    sub_env_prepare_attack(&v->env);

    /* ペルボイス共振LPF (正準値をそのまま使う) */
    v->filt_cutoff_base = d->filt_base;
    v->filt_peak_hz = d->filt_peak;
    v->filt_env = 1.0f;
    v->svf_q = d->filt_q;   /* タイル毎の係数再計算でも音色別 Q を維持 */
    {
        float tc_samples = SUB3_FILTER_ENV_TIME * (float)SUB_SAMPLE_RATE;
        v->filt_env_coeff = sub_exp_approx(1.0f / tc_samples);
        v->filt_env_coeff64 = sub_exp_approx((float)SUB3_MIX_TILE / tc_samples);
    }
    sub_svf_reset(&v->svf);
    sub_svf_set(&v->svf, v->filt_peak_hz, d->filt_q, (float)SUB_SAMPLE_RATE);
    /* HP はバス集約へ移行 (per-voice HP 廃止)。重低音ラムブル除去はタイル後段で実施 */
    v->vib_phase = (float)((eng->lfsr_state++ & 0xFFu)) / 256.0f;
    v->vib_inc = (4.4f + 0.6f * ((eng->lfsr_state >> 8) & 0x3u)) / (float)SUB_SAMPLE_RATE;
    v->vib_depth_st = eng->channels[channel].mod_depth * 0.35f;
    spatial_voice_init(&v->spatial);
}

/**
 * @brief ディスクリプタ経由のノートオン (Core1分解送信の受け側)
 */
static void sub3_note_on_desc(Sub3BassEngine *eng, const SubSpawnDesc *d)
{
    int voice_idx = sub3_voice_alloc(eng, d->channel, d->note);
    sub3_spawn_from_desc(eng, &eng->voices[voice_idx], d);
}

/**
 * @brief ノートオン (従来解釈 = 正準 build 関数に一本化)
 * @details Core1分解送信とbit一致させるため、解釈は sub_spawn_build_sub3 のみ。
 */
static void sub3_note_on(Sub3BassEngine *eng, uint8_t channel, uint8_t note, uint8_t vel)
{
    if (vel == 0u) {
        sub3_note_off(eng, channel, note);
        return;
    }
    int voice_idx = sub3_voice_alloc(eng, channel, note);
    SubSpawnDesc d;
    sub_spawn_build_sub3(&d, channel, note, vel, eng->channels[channel].program);
    sub3_spawn_from_desc(eng, &eng->voices[voice_idx], &d);
}

/**
 * @brief ボイスのリリース開始 (共通処理)
 */
static void sub3_begin_release(VoiceSub3 *v)
{
    sub_env_begin_release(&v->env);
}

/**
 * @brief サステインペダル (CC#64) 更新
 */
static void sub3_channel_sustain(Sub3BassEngine *eng, uint8_t channel, bool pedal_down)
{
    eng->channels[channel].sustain_pedal = pedal_down;
    if (!pedal_down) {
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            VoiceSub3 *v = &eng->voices[i];
            if (v->active && v->channel == channel && v->env.sustained_by_pedal) {
                v->env.sustained_by_pedal = false;
                if (v->env.env_state != SUB_ENV_RELEASE && v->env.env_state != SUB_ENV_IDLE) {
                    sub3_begin_release(v);
                }
            }
        }
    }
}

/**
 * @brief ノートオフ
 */
static void sub3_note_off(Sub3BassEngine *eng, uint8_t channel, uint8_t note)
{
    for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
        VoiceSub3 *v = &eng->voices[i];
        if (v->active && v->channel == channel && v->note == note && v->env.env_state != SUB_ENV_RELEASE && v->env.env_state != SUB_ENV_IDLE) {
            if (eng->channels[channel].sustain_pedal) {
                /* ダンパーペダル踏み中: リリースを延期して保持 */
                v->env.sustained_by_pedal = true;
                continue;
            }
            sub3_begin_release(v);
        }
    }
}

/**
 * @brief 全音消音
 */
/**
 * @brief チャンネル指定消音 (channel >= 16 で全チャンネル)
 */
static void sub3_all_notes_off(Sub3BassEngine *eng, uint8_t channel)
{
    for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
        VoiceSub3 *v = &eng->voices[i];
        if (v->active && (channel >= 16 || v->channel == channel)) {
            v->env.sustained_by_pedal = false;
            if (v->env.env_state != SUB_ENV_IDLE && v->env.env_state != SUB_ENV_RELEASE) {
                sub_env_begin_release(&v->env);
                v->env.release_step = v->env.current_env_level / 128.0f;
                v->env.release_coeff = sub_exp_approx(6.907755f / 128.0f);
                v->env.phase_max_samples = 128;
                v->env.env_samples = 0;
            }
        }
    }
    if (channel < 16) {
        eng->channels[channel].sustain_pedal = false;
    } else {
        for (int ch = 0; ch < 16; ch++) {
            eng->channels[ch].sustain_pedal = false;
        }
    }
}

static void sub3_apply_pending_packet(const AsmpPacket *pkt)
{
    switch (pkt->msg_type) {
        case ASMP_MSG_NOTE_ON:
            if (pkt->channel == 9) {
                if (sub_drum_is_kick(pkt->data1)) sub_kick_note_on(&s_sub3_kick, (float)pkt->data2 / 127.0f);
                else if (sub_drum_is_metal(pkt->data1)) sub_metal_note_on(&s_sub3_metal, pkt->data1, (float)pkt->data2 / 127.0f);
                else sub_perc_note_on(&s_sub3_perc, pkt->data1, (float)pkt->data2 / 127.0f);
            } else if (pkt->channel < 16) {
                /* ABI v13: Core1分解送信トークンが有効なら fast-spawn。
                 * 無効なら正準 build で従来解釈する (Core1送信とbit一致) */
                SubSpawnDesc desc;
                if (pkt->param != 0u &&
                    sub_spawn_consume(s_sub3.shared->spawn_pool_sub3,
                                      &s_sub3.shared->spawn_ack_sub3.consumed,
                                      pkt->param, &desc)) {
                    s_sub3.shared->spawn_stats_sub3.fast_spawn++;
                    asmp_dcache_clean((const void *)&s_sub3.shared->spawn_stats_sub3,
                                      sizeof(s_sub3.shared->spawn_stats_sub3));
                    ASMP_BARRIER();
                    sub3_note_on_desc(&s_sub3, &desc);
                } else {
                    s_sub3.shared->spawn_stats_sub3.legacy_spawn++;
                    asmp_dcache_clean((const void *)&s_sub3.shared->spawn_stats_sub3,
                                      sizeof(s_sub3.shared->spawn_stats_sub3));
                    ASMP_BARRIER();
                    if (pkt->param != 0u) {
                        /* 陳腐トークンの解放 (スロット再利用のため ack のみ) */
                        sub_spawn_ack_only(s_sub3.shared->spawn_pool_sub3,
                                           &s_sub3.shared->spawn_ack_sub3.consumed,
                                           pkt->param);
                    }
                    sub3_note_on(&s_sub3, pkt->channel, pkt->data1, pkt->data2);
                }
            }
            break;
        case ASMP_MSG_NOTE_OFF:
            if (pkt->channel != 9 && pkt->channel < 16) sub3_note_off(&s_sub3, pkt->channel, pkt->data1);
            break;
        case ASMP_MSG_PROGRAM_CHANGE:
            if (pkt->channel < 16) s_sub3.channels[pkt->channel].program = pkt->data1;
            break;
        case ASMP_MSG_CONTROL_CHANGE:
            if (pkt->channel == 9) {
                if (pkt->data1 == 7) {
                    s_sub3_kick.volume = (float)pkt->data2 / 127.0f;
                    s_sub3_metal.volume = (float)pkt->data2 / 127.0f;
                    s_sub3_perc.volume = (float)pkt->data2 / 127.0f;
                } else if (pkt->data1 == 91) {
                    /* Ch9 CC91は無視 */
                }
            } else if (pkt->channel < 16) {
                float norm = (float)pkt->data2 / 127.0f;
                if (pkt->data1 == 7) s_sub3.channels[pkt->channel].volume = norm;
                if (pkt->data1 == 10) { s_sub3.channels[pkt->channel].pan = norm; sub_channel_update_pan_gains(&s_sub3.channels[pkt->channel]); }
                if (pkt->data1 == 11) s_sub3.channels[pkt->channel].expression = norm;
                if (pkt->data1 == 1) s_sub3.channels[pkt->channel].mod_depth = norm;
                if (pkt->data1 == 91) s_sub3.channels[pkt->channel].reverb_send = norm;
                if (pkt->data1 == 64) sub3_channel_sustain(&s_sub3, pkt->channel, pkt->data2 >= 64);
                if (pkt->data1 == 120 || pkt->data1 == 123) {
                    for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
                        VoiceSub3 *v = &s_sub3.voices[i];
                        if (v->active && v->channel == pkt->channel) {
                            if (pkt->data1 == 120) { v->active = false; v->env.env_state = SUB_ENV_IDLE; v->env.current_env_level = 0.0f; v->env.sustained_by_pedal = false; }
                            else if (v->env.env_state != SUB_ENV_IDLE && v->env.env_state != SUB_ENV_RELEASE) { sub_env_begin_release(&v->env); v->env.release_step = v->env.current_env_level / 128.0f; v->env.release_coeff = sub_exp_approx(6.907755f / 128.0f); v->env.phase_max_samples = 128; v->env.env_samples = 0; }
                        }
                    }
                    s_sub3.channels[pkt->channel].sustain_pedal = false;
                }
            }
            break;
        case ASMP_MSG_PITCH_BEND:
            if (pkt->channel < 16) {
                int16_t bend = (int16_t)((int32_t)pkt->param);
                s_sub3.channels[pkt->channel].pitch_bend_semitones = ((float)bend / 8192.0f) * 2.0f;
            }
            break;
        case ASMP_MSG_ALL_NOTES_OFF:
            {
                uint8_t target_ch = (pkt->channel >= 16 || pkt->data1 == 0xFF) ? 0xFF : pkt->channel;
                sub3_all_notes_off(&s_sub3, target_ch);
                if (target_ch == 9 || target_ch >= 16) {
                    sub_kick_all_notes_off(&s_sub3_kick);
                    sub_metal_all_notes_off(&s_sub3_metal);
                    sub_perc_all_notes_off(&s_sub3_perc);
                }
            }
            break;
        default: break;
    }
}

/**
 * @brief 単一ボイスのレンダリング (外側4サンプルLFO + 内側4サンプル完全分岐フリーDSPループ)
 */
static void sub3_render_voice(VoiceSub3 *v, const SubChannel *ch,
                               const float *lut, float *mix_l, float *mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.45f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.35f;
    const WaveType3 wt = v->wave_type;
    const bool is_bass = v->is_bass;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    uint32_t phase_q = v->phase_q;
    uint32_t inc_q = v->inc_q;
    uint32_t sub1_phase_q = v->sub1_phase_q;
    uint32_t sub2_phase_q = v->sub2_phase_q;

    const float Q32_INV = 1.0f / 4294967296.0f;
    const float Q32 = 4294967296.0f;

    /* 波形分岐ホイスト: 旧switchは毎サンプル×512回分岐予測ミス源。
     * タイル先頭で1回だけ分岐し、4種の専用ループへ振り分ける。
     * dtはinc_qが4samp一定のため外側で1回だけ算出(3回分のVCVT+VMUL削減) */
    for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
        uint32_t chunk = tile_frames - f4;
        if (chunk > 4) chunk = 4;

        /* 4 サンプル外側グループ化: LFO ビブラート / ピッチ増分更新 */
        v->vib_phase += v->vib_inc * 4.0f;
        if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
        float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
        float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi);
        inc_q = (uint32_t)(inc_f * Q32);
        float dt_blk = (float)inc_q * Q32_INV;

        /* 4 サンプル内側ループ: 完全分岐フリー DSP ループ */
        for (uint32_t k = 0; k < chunk; k++) {
            uint32_t f = f4 + k;

            /* ADSR 進行 (共通コア) */
            float env = sub_env_advance(&v->env);
            if (v->env.env_state == SUB_ENV_IDLE) {
                v->active = false;
                goto exit_voice;
            }
            v->age_samples++;

            float dt = dt_blk;
            float ph = (float)phase_q * Q32_INV;

            float osc;
            /* switch→if-chain: コンパイラがジャンプテーブル化せず
             * 分岐予測しやすい等価形へ。根本対策はタイル外ホイストだが
             * まずdtホイストと合わせ中間計測する */
            if (wt == WAVE3_SINE) {
                osc = sub_lookup_sine(g_sine_lut, ph);
            } else if (wt == WAVE3_SQUARE) {
                osc = sub_osc_square(ph, dt);
            } else if (wt == WAVE3_SAWTOOTH) {
                osc = sub_osc_saw(ph, dt);
            } else if (wt == WAVE3_TRIANGLE) {
                osc = sub_osc_triangle(ph, dt);
            } else {
                osc = 0.0f;
            }

            if (is_bass && !s_sub3_hp_bypass) {
                /* サブオシレータ: PolyBLEP 帯域制限 -1 Oct / -2 Oct 矩形波ブレンド (#音質改善B)
                 * GOV時はis_bass自体は保持しつつサブオシのみ無効化 (S6 fix, 永続破壊防止) */
                float sub1 = sub_osc_square_q32(sub1_phase_q, inc_q >> 1);
                float sub2 = sub_osc_square_q32(sub2_phase_q, inc_q >> 2);
                osc = osc * 0.60f + sub1 * 0.25f + sub2 * 0.15f;
            }

            /* 直列 LP フィルター (HP はバス集約へ移行。GOV2時はサブオシ無効のみ) */
            float filtered = sub_svf_lp(&v->svf, osc);

            float sample = filtered * env * ch_gain;
            if (spatial_on) {
                float l, r;
                spatial_process_sample(&v->spatial, sample, spatial_az, spatial_itd,
                                       spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                mix_l[f] += l;
                mix_r[f] += r;
            } else {
                mix_l[f] += sample * pan_l;
                mix_r[f] += sample * pan_r;
            }

            /* 位相更新 (サブオシはビットシフト) */
            phase_q += inc_q;
            sub1_phase_q += (inc_q >> 1);
            sub2_phase_q += (inc_q >> 2);
        }
    }

exit_voice:
    v->phase_q = phase_q;
    v->inc_q = inc_q;
    v->sub1_phase_q = sub1_phase_q;
    v->sub2_phase_q = sub2_phase_q;

    /* タイル完了後の SVF カットオフ追従 (tile_frames サンプル一括減衰) */
    if (v->active && v->env.env_state != SUB_ENV_IDLE) {
        if (tile_frames == SUB3_MIX_TILE) {
            v->filt_env *= v->filt_env_coeff64;
        } else if (tile_frames > 0) {
            v->filt_env *= sub_exp_approx((float)tile_frames / (SUB3_FILTER_ENV_TIME * (float)SUB_SAMPLE_RATE));
        }
        float cut = v->filt_cutoff_base +
                    (v->filt_peak_hz - v->filt_cutoff_base) * v->filt_env;
        sub_svf_set(&v->svf, cut, v->svf_q, (float)SUB_SAMPLE_RATE);
    }
}

/**
 * @brief PCM レンダリング (64 フレーム float タイル蓄積 + float バス書き出し)
 *        最終 16bit 量子化は Sub5 マスター出力段で TPDF ディザ付き 1 回のみ
 */
static bool sub3_render(Sub3BassEngine *eng, float *buffer, uint32_t frames, uint32_t slot, uint32_t req_epoch)
{
    uint16_t active_count = 0;
    uint8_t qf3 = eng->shared->main_ctrl.quality_flags;
    /* GOV2時はspatial OFF+HP bypass+サブオシ無効で戦術落とし。Marisa 8voice10k→6voice7kへ */
    bool spatial_on = (eng->shared->main_ctrl.spatial_enable != 0) && !(qf3 & ASMP_QF_UNISON_OFF);
    s_sub3_hp_bypass = (qf3 & ASMP_QF_UNISON_OFF) != 0;
    /* 事前culling: GOV2で6voice超は最弱を緊急リリース(2.7ms)でフェード。クリック防止で即死IDLE廃止 */
    if (qf3 & ASMP_QF_UNISON_OFF) {
        int active_tmp = 0;
        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            if (eng->voices[i].active) active_tmp++;
        }
        while (active_tmp > 6) {
            float min_lvl = 999.0f;
            int min_idx = -1;
            for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
                if (eng->voices[i].active && eng->voices[i].env.current_env_level < min_lvl &&
                    eng->voices[i].env.env_state != SUB_ENV_IDLE && eng->voices[i].env.env_state != SUB_ENV_RELEASE) {
                    min_lvl = eng->voices[i].env.current_env_level;
                    min_idx = i;
                }
            }
            if (min_idx < 0) {
                break;
            }
            VoiceSub3 *v = &eng->voices[min_idx];
            sub_env_begin_release(&v->env);
            v->env.release_step = v->env.current_env_level / 128.0f;
            v->env.release_coeff = sub_exp_approx(6.907755f / 128.0f);
            v->env.phase_max_samples = 128;
            v->env.env_samples = 0;
            active_tmp--;
        }
        /* is_bassは永続変更せず、描画時にQFで判定 (S6 fix) */
    }

    for (uint32_t t_start = 0; t_start < frames; t_start += SUB3_MIX_TILE) {
        /* 所有権検証 (Early Abort): 遅延してMainがスロットを再割り当てしていたら直ちに中断 */
        if (!asmp_slot_validate(eng->shared, slot, req_epoch)) {
            return false;
        }

        uint32_t tile_frames = frames - t_start;
        if (tile_frames > SUB3_MIX_TILE) tile_frames = SUB3_MIX_TILE;

        float mix_l[SUB3_MIX_TILE];
        float mix_r[SUB3_MIX_TILE];
        memset(mix_l, 0, tile_frames * sizeof(float));
        memset(mix_r, 0, tile_frames * sizeof(float));

        for (int i = 0; i < SUB3_MAX_POLYPHONY; i++) {
            VoiceSub3 *v = &eng->voices[i];
            if (!v->active || v->env.env_state == SUB_ENV_IDLE) continue;

            if (t_start == 0) {
                active_count++;
            }

            SubChannel *ch = &eng->channels[v->channel];
            sub3_render_voice(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
        }

        /* 重低音ラムブル除去 HP をバス後段へ集約 (LTI 可換により per-voice と等価)。
         * GOV2 時は従来通り bypass。ドラムは HP 対象外だったため HP 後に加算 */
        if (!s_sub3_hp_bypass) {
            for (uint32_t f = 0; f < tile_frames; f++) {
                mix_l[f] = sub_svf_hp(&eng->bus_hp_l, mix_l[f]);
                mix_r[f] = sub_svf_hp(&eng->bus_hp_r, mix_r[f]);
            }
        }

        /* Phase 4/5/6: 移行された Kick/Metal/Perc をモノラルでタイルへ加算 (L=R) */
        sub_kick_render(&s_sub3_kick, mix_l, mix_r, tile_frames);
        sub_metal_render(&s_sub3_metal, mix_l, mix_r, tile_frames);
        sub_perc_render(&s_sub3_perc, mix_l, mix_r, tile_frames);

        /* float のまま PCM バスへ書き出し (中間量子化なし) */
        float *dst = &buffer[t_start * 2];
        for (uint32_t f = 0; f < tile_frames; f++) {
            dst[f * 2 + 0] = mix_l[f];
            dst[f * 2 + 1] = mix_r[f];
        }
    }

    eng->shared->core[ASMP_CORE_SUB3_BASS].voice_count = active_count;
    /* Core1入場整理の飽和判定用に公開 (busy_us と同一規律で clean) */
    asmp_dcache_clean((const void *)&eng->shared->core[ASMP_CORE_SUB3_BASS].voice_count,
                      sizeof(eng->shared->core[ASMP_CORE_SUB3_BASS].voice_count));
    return true;
}

/**
 * @brief SubCore 3 エントリーポイント
 */
void *subcore3_entry(void *arg)
{
    AsmpSharedContext *shared = (AsmpSharedContext *)arg;
    if (!shared) return NULL;
    if (!asmp_abi_ok(shared)) {
        printf("[SUB3][FATAL] ABI mismatch at entry\n");
        return NULL;
    }
    sub_fpu_denormal_init(); /* デノーマル例外ペナルティによるじりじりノイズ防止 */

    sub3_engine_init(&s_sub3, shared);

    /* ビルド識別ログ出力 */
    printf("[SUB3][BUILD] %s | bass & strings extreme opt (Q32 sub-osc + 3x FMA SVF + tile mix)\n", HEXASENSE_DSP_TAG);

    /* CPU 負荷メーター (実測ビジー時間比率) */
    SubLoadMeter load_m; SUB_LOAD_INIT(load_m);

    uint32_t idle_loops = 0;
    uint32_t req_epoch = 0;

    while (!shared->main_ctrl.shutdown_requested) {
        AsmpPacket pkt;
        bool has_render_req = false;

        while (asmp_queue_pop(&shared->queues[ASMP_CORE_SUB3_BASS], &pkt)) {
            if (pkt.msg_type == ASMP_MSG_RENDER_REQ) {
                has_render_req = true;
                req_epoch = pkt.param;
                break;
            }
            /* 緊急消音（ALL_NOTES_OFF / CC120）はRENDER_REQを待たずに即時反映 */
            if (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF ||
                (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 120)) {
                sub3_apply_pending_packet(&pkt);
                continue;
            }
            if (s_sub3_pending_cnt < SUB3_MAX_PENDING) {
                s_sub3_pending[s_sub3_pending_cnt++] = pkt;
            } else {
                bool is_release = (pkt.msg_type == ASMP_MSG_NOTE_OFF) ||
                                  (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 64 && pkt.data2 < 64) ||
                                  (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF);
                if (is_release) {
                    /* 溢れても消音は必達: 最低ベロシティNOTE_ONを追い出す。
                     * 退避は diag_queue_drop で可視化 (従来は無計測の欠落) */
                    int rep = -1;
                    uint8_t rep_vel = 0xFFu;
                    for (int i = 0; i < (int)SUB3_MAX_PENDING; i++) {
                        if (s_sub3_pending[i].msg_type == ASMP_MSG_NOTE_ON &&
                            s_sub3_pending[i].data2 <= rep_vel) {
                            rep_vel = s_sub3_pending[i].data2;
                            rep = i;
                        }
                    }
                    if (rep >= 0) {
                        /* 追い出される NOTE_ON のトークンがあれば解放する。
                         * [MAX-1] 上書き側は NOTE_ON 不在確定のため不要 */
                        if (s_sub3_pending[rep].msg_type == ASMP_MSG_NOTE_ON &&
                            s_sub3_pending[rep].param != 0u) {
                            sub_spawn_ack_only(shared->spawn_pool_sub3,
                                               &shared->spawn_ack_sub3.consumed,
                                               s_sub3_pending[rep].param);
                        }
                        s_sub3_pending[rep] = pkt;
                    }
                    else s_sub3_pending[SUB3_MAX_PENDING - 1] = pkt;
                    shared->diag_queue_drop++;
                } else {
                    /* 溢れで捨てる NOTE_ON のトークンも解放する */
                    if (pkt.msg_type == ASMP_MSG_NOTE_ON && pkt.param != 0u) {
                        sub_spawn_ack_only(shared->spawn_pool_sub3,
                                           &shared->spawn_ack_sub3.consumed,
                                           pkt.param);
                    }
                    shared->diag_queue_drop++;
                }
            }
        }

        if (has_render_req) {
            static uint32_t s_last_epoch = 0;
            if (s_last_epoch != 0 && req_epoch != s_last_epoch + 1u) {
                shared->diag_epoch_gap[ASMP_CORE_SUB3_BASS]++;
            }
            s_last_epoch = req_epoch;
            SUB_CHAOS_DELAY(ASMP_CORE_SUB3_BASS);
            SUB_LOAD_BUSY_BEGIN(load_m);
            uint64_t t0_ns = sub_get_ns();
            uint32_t slot = ASMP_EPOCH_SLOT(req_epoch);
            if (!asmp_abi_ok(shared)) {
                printf("[SUB3][FATAL] ABI mismatch\n");
                return NULL;
            }
            if (shared->render_ctrl.slot_epoch[slot] != req_epoch) {
                shared->diag_slot_mismatch++;
                shared->done_epoch[ASMP_CORE_SUB3_BASS].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB3_BASS].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                s_sub3_pending_cnt = 0;
                continue;
            }
            uint32_t ef = shared->render_ctrl.epoch_frames[slot];
#ifdef PROFILE_ENABLE
    profile_epoch_start(3, req_epoch);
#endif
            if (ef == 0u || ef > ASMP_BUFFER_FRAMES) ef = ASMP_BUFFER_FRAMES;

            for (uint32_t i = 1; i < s_sub3_pending_cnt; i++) {
                AsmpPacket key = s_sub3_pending[i];
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && s_sub3_pending[j].sample_offset > key.sample_offset) {
                    s_sub3_pending[j+1] = s_sub3_pending[j];
                    j--;
                }
                s_sub3_pending[j+1] = key;
            }
            bool render_ok = true;
            if (s_sub3_pending_cnt == 0) {
                render_ok = sub3_render(&s_sub3, shared->pcm_sub3_bass[slot], ef, slot, req_epoch);
            } else {
                uint32_t cursor = 0;
                uint32_t pi = 0;
                memset(shared->pcm_sub3_bass[slot], 0, ef * sizeof(float) * 2);
                while (pi < s_sub3_pending_cnt) {
                    uint32_t off = s_sub3_pending[pi].sample_offset;
                    if (off > ef) off = ef;
                    if (off > cursor) {
                        if (!sub3_render(&s_sub3, shared->pcm_sub3_bass[slot] + cursor * 2, off - cursor, slot, req_epoch)) {
                            render_ok = false;
                            break;
                        }
                        cursor = off;
                    }
                    uint32_t cur_off = s_sub3_pending[pi].sample_offset;
                    while (pi < s_sub3_pending_cnt && s_sub3_pending[pi].sample_offset == cur_off) {
                        sub3_apply_pending_packet(&s_sub3_pending[pi]);
                        pi++;
                    }
                }
                if (render_ok && cursor < ef) {
                    render_ok = sub3_render(&s_sub3, shared->pcm_sub3_bass[slot] + cursor * 2, ef - cursor, slot, req_epoch);
                }
            }
            s_sub3_pending_cnt = 0;
            /* P0-B: commit前にslot世代を検証 */
            if (!render_ok || !asmp_slot_validate(shared, slot, req_epoch)) {
                shared->diag_slot_rejected++;
                shared->done_epoch[ASMP_CORE_SUB3_BASS].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB3_BASS].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                continue;
            }
            #ifdef PROFILE_ENABLE
    profile_epoch_end(3, req_epoch);
#endif
            asmp_slot_commit(shared, slot, req_epoch, ASMP_SLOT_WORKER_BIT_SUB3);
            SUB_EPOCH_TIME_UPDATE(shared, ASMP_CORE_SUB3_BASS, t0_ns);

            /* PCM キャッシュクリーン & Release バリア (done_epoch 公開前にペイロード確定) 32B丸めで隣接slot false sharing防止 */
            asmp_dcache_clean(shared->pcm_sub3_bass[slot],
                              ((size_t)ef * sizeof(float) * 2 + 31u) & ~31u);
            ASMP_BARRIER();

            /* 完了エポックを公開 (単調増加のためクリア競合が存在しない) */
            shared->done_epoch[ASMP_CORE_SUB3_BASS].val = req_epoch;
            ASMP_BARRIER();
            shared->core[ASMP_CORE_SUB3_BASS].heartbeat++;
            idle_loops = 0;
            SUB_LOAD_BUSY_END(load_m);
        } else {
            /* RENDER_REQ が来ていない待機中でも、溜まったMIDIイベントがあれば即座に適用
             * (曲停止時の消音や、フレーム間に届いた Note Off / CC を絶対に捨てない) */
            if (s_sub3_pending_cnt > 0) {
                for (uint32_t i = 0; i < s_sub3_pending_cnt; i++) {
                    sub3_apply_pending_packet(&s_sub3_pending[i]);
                }
                s_sub3_pending_cnt = 0;
            }
            /* 待機中も低レートでハートビートを申告 (生存監視用) */
            if ((++idle_loops & 0x3Fu) == 0u) {
                shared->core[ASMP_CORE_SUB3_BASS].heartbeat++;
            }
            sub3_sleep_us(100);
        }

        SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB3_BASS);
    }

    return NULL;
}
