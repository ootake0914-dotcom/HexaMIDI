/**
 * @file sub2_main.c
 * @brief SubCore 2: メロディ・リード・ピアノ音源 (PolyBLEP 8ボイス)
 * @details 8音ポリフォニック合成、PolyBLEP/PolyBLAMP帯域制限オシレータ、GM音色マッピング
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#define sub2_sleep_us(us) Sleep((DWORD)((us) / 1000))
#elif defined(__NuttX__)
#include <nuttx/arch.h>
#define sub2_sleep_us(us) up_udelay((useconds_t)(us))
#else
#include <unistd.h>
#define sub2_sleep_us(us) usleep(us)
#endif

#include "sub_common.h"
#include "sub_spawn.h"
#include "rt_profile.h"
#include "spatial_audio.h"
#include "sub_asm.h" /* M4F手書き UBFX/VFMA カーネル。hostはCフォールバック */
#include "sub_kick.h" /* Phase 4: Kick (note 35/36) の動的移行先ホスト用 専用ドラム */
#include "sub_metal.h" /* Phase 5: Metal (HiHat/Cymbal) の動的移行先ホスト用 専用ドラム */
#include "sub_perc.h" /* Phase 6: Perc (Snare/Clap/Tom) の動的移行先ホスト用 専用ドラム */

/* ポリフォニー上限。
 * 実測では S2 エポック時間 = ~0.53ms x 発音数 + ~1.6ms で線形増加し、
 * 16 声の密集譜面で 12ms == RT 予算 (512fr@48k) を突破して
 * オーディオバッファ枯渇 -> アンダーラン連鎖 (体感ラグ) を起こしていた (#13)。
 * 12 声上限で最大 ~8ms に抑え、超過分は最古ボイス スチールで吸収する */
#define SUB2_MAX_POLYPHONY (16)
#define SUB2_FILTER_ENV_TIME (0.22f)  /* LPF エンベロープ減衰時定数 (秒) */
#define SUB2_MIX_TILE (128u)          /* float タイル蓄積バッファ長 (128フレーム: 64->128でタイル数8->4、ループ/係数更新半減。stack 512->1024B増のみ、共有SRAM増0) */

/* ユニソン・デチューン量 (厳密値 2^(±cent/1200))
 * Roland JP-8000 風スーパーソウの黄金比: 内側ペア ±0.12 半音 (=±12 cent)、
 * 外側ペア ±0.28 半音 (=±28 cent)。
 * 旧 ±7/+14 cent は広がりが控えめで「極太感」が足りなかった
 * HQ時は外側を±35 centにワイド化 (余裕時のみ、512B増と合わせても破綻なし) */
#define SUB2_DETUNE_RATIO_LO  (0.99309250f)   /* -12 cent (-0.12 半音) */
#define SUB2_DETUNE_RATIO_HI  (1.00695555f)   /* +12 cent (+0.12 半音) */
#define SUB2_DETUNE_RATIO_LO2 (0.98395666f)   /* -28 cent (-0.28 半音) */
#define SUB2_DETUNE_RATIO_HI2 (1.01630493f)   /* +28 cent (+0.28 半音) */
#define SUB2_DETUNE_RATIO_LO2_HQ (0.9799f)   /* HQ -35 cent */
#define SUB2_DETUNE_RATIO_HI2_HQ (1.0204f)   /* HQ +35 cent */

typedef enum {
    WAVE2_SINE = 0,
    WAVE2_SQUARE,
    WAVE2_SAWTOOTH,
    WAVE2_TRIANGLE
} WaveType2;

typedef struct {
    bool        active;
    uint8_t     channel;
    uint8_t     note;
    float       velocity;
    float       frequency;
    float       phase;           /* オシ #1 (中央) */
    float       phase2;          /* オシ #2 (+12 cent) */
    float       phase3;          /* オシ #3 (-12 cent) */
    float       phase4;          /* オシ #4 (+28 cent) */
    float       phase5;          /* オシ #5 (-28 cent) */
    bool        unison;          /* 5 オシレータ スーパーソウ */
    float       phase_increment;
    WaveType2   wave_type;
    bool        wt_active;       /* ウェーブテーブル発音 */
    float       wt_morph;        /* テーブル モルフ位置 */
    uint8_t     wt_mip;          /* 0=通常 / 1=高音域 */

    SubEnvCore  env;             /* 共通 ADSR コア (sub_common.h) */
    uint32_t    age_samples;

    /* Q32 位相系 (ウェーブテーブル発音専用)。
     * 32bit 自然オーバーフロー = 1 周期ラップのため毎サンプル分岐が不要。
     * クラシック発音 (PolyBLEP) 系は float phase 系をそのまま使用する */
    uint32_t    qph[5];          /* オシ #1..#5 の位相 */
    uint32_t    qinc[5];         /* 位相増分 (ビブラート/ベンド反映は 4 サンプル毎) */
    uint8_t     morph_a;         /* モルフ先テーブル A (note-on 時に確定) */
    uint8_t     morph_b;         /* モルフ先テーブル B (単表時は A と同じ) */
    float       morph_w;         /* B 側ミックス比 (0 = 単表 fast path) */

    /* モーフ済み単一テーブル (note-on 時に A/B をブレンドして生成)。
     * カーネルは常に単表 fast path で読め、毎サンプルの 2 表読みが消える (#13) */
    float       premix[SUBWT_SIZE + 1];
    bool        use_premix;

    /* ペルボイス 共振LPF + フィルターエンベロープ (減算合成) */
    float       filt_cutoff_base;   /* エンベロープ末端のカットオフ (Hz) */
    float       filt_peak_hz;       /* トリガ直後のピークカットオフ (Hz) */
    float       filt_env;           /* 正規化エンベロープ 1 -> 0 */
    float       filt_env_coeff;     /* サンプル毎の減衰係数 */
    float       filt_env_coeff64;   /* 64 サンプル一括減衰係数 (タイル更新用) */
    SubSvf      svf;
    float       svf_q;            /* 音色別共振 Q (再計算時も保持) */
    float       base_increment;   /* ベンド/LFO 適用前の基本位相増分 */
    float       vib_phase;        /* LFO ビブラート位相 */
    float       vib_inc;          /* LFO 位相増分 (~5.2Hz) */
    float       vib_depth_st;     /* ビブラート深さ (半音, CC#1) */
    float       drift_phase;      /* アナログVCOドリフト位相 (0.12-0.22Hz) */
    float       drift_inc;        /* ドリフト増分 */
    SpatialVoiceState spatial;    /* 立体音響 ITD/ILD 状態 */
} VoiceSub2;

typedef struct {
    AsmpSharedContext *shared;
    VoiceSub2   voices[SUB2_MAX_POLYPHONY];
    SubChannel  channels[16];
    uint32_t    lfsr_state;
    /* バス集約ランブル除去 HP (L/R)。per-voice HP と同一 45Hz/Q0.7 を
     * ミックス後に 1 回だけ適用する (LTI 可換により等価、16voice 分→2ch 分へ) */
    SubSvf      bus_hp_l;
    SubSvf      bus_hp_r;
} Sub2LeadEngine;

static Sub2LeadEngine s_sub2;
static bool s_hq_wide = false; /* 余裕時HQワイドデチューン (512B増分と同様に破綻なし) */
static bool s_sub2_hp_bypass = false; /* GOV2(1-osc)時バスHPラムブル除去をbypass */
static SubKickEngine s_sub2_kick; /* Phase 4: C4 から移行された Kick のホスト状態 */
static SubMetalEngine s_sub2_metal; /* Phase 5: C4 から移行された Metal のホスト状態 */
static SubPercEngine s_sub2_perc; /* Phase 6: C4 から移行された Perc のホスト状態 */

/* サンプルオフセット対応: エポック内MIDIイベントを一時蓄積 */
#define SUB2_MAX_PENDING 256
static AsmpPacket s_sub2_pending[SUB2_MAX_PENDING];
static uint32_t s_sub2_pending_cnt = 0;

/**
 * @brief SubCore 2 エンジン初期化
 */
static void sub2_engine_init(Sub2LeadEngine *eng, AsmpSharedContext *shared)
{
    memset(eng, 0, sizeof(Sub2LeadEngine));
    eng->shared = shared;
    eng->lfsr_state = 0x1234ABCDu;
    /* g_sine_lut const, no init */
    sub_freq_lut_init();
    /* ウェーブテーブルは ROM 常数 g_sub_wavbank を使用 (RAM 24.8KB 削減) */
    /* 数学 LUT 初期化: 以降の SVF 係数算出 / エンベロープ係数が高速経路を通る */
    sub_exp_lut_init();
    sub_tan_lut_init((float)SUB_SAMPLE_RATE);
    /* バス集約 HP 初期化 (per-voice HP と同一特性 45Hz/Q0.7。連続動作) */
    sub_svf_reset(&eng->bus_hp_l);
    sub_svf_reset(&eng->bus_hp_r);
    sub_svf_set(&eng->bus_hp_l, 45.0f, 0.70f, (float)SUB_SAMPLE_RATE);
    sub_svf_set(&eng->bus_hp_r, 45.0f, 0.70f, (float)SUB_SAMPLE_RATE);
    sub_kick_init(&s_sub2_kick); /* Phase 4: Kick ホスト初期化 (noise/volume 独立) */
    sub_metal_init(&s_sub2_metal); /* Phase 5: Metal ホスト初期化 (noise/free_ph 独立) */
    sub_perc_init(&s_sub2_perc); /* Phase 6: Perc ホスト初期化 (noise/roll 独立) */

    for (int ch = 0; ch < 16; ch++) {
        eng->channels[ch].program = 0; /* 0: Acoustic Piano */
        eng->channels[ch].volume = 0.85f;
        eng->channels[ch].expression = 1.0f;
        eng->channels[ch].pan = 0.5f; /* Center */
        eng->channels[ch].pitch_bend_semitones = 0.0f;
        eng->channels[ch].mod_depth = 0.0f;
        eng->channels[ch].reverb_send = 0.3f;
        eng->channels[ch].sustain_pedal = false;
        sub_channel_update_pan_gains(&eng->channels[ch]);
    }
}

static void sub2_note_off(Sub2LeadEngine *eng, uint8_t channel, uint8_t note);

/**
 * @brief ノートオン
 */
static int sub2_voice_alloc(Sub2LeadEngine *eng, uint8_t channel, uint8_t note)
{
    int voice_idx = -1;
    /* 1. 同一ノートの再利用 */
    for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
        if (eng->voices[i].active && eng->voices[i].channel == channel && eng->voices[i].note == note) {
            voice_idx = i;
            break;
        }
    }
    /* 2. 空きボイス探索 */
    if (voice_idx == -1) {
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
            if (!eng->voices[i].active || eng->voices[i].env.env_state == SUB_ENV_IDLE) {
                voice_idx = i;
                break;
            }
        }
    }
    /* 3. リリース中最小音量ボイス */
    if (voice_idx == -1) {
        float min_level = 999.0f;
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
            if (eng->voices[i].env.env_state == SUB_ENV_RELEASE && eng->voices[i].env.current_env_level < min_level) {
                min_level = eng->voices[i].env.current_env_level;
                voice_idx = i;
            }
        }
    }
    /* 4. GOV2(1-osc)時 11→8 culling: 最弱音量voiceを優先killしてbudget内に収める */
    if (voice_idx == -1 && (eng->shared->main_ctrl.quality_flags & ASMP_QF_UNISON_OFF)) {
        int active_cnt = 0;
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) if (eng->voices[i].active) active_cnt++;
        if (active_cnt >= 8) {
            float min_lvl = 999.0f; int min_idx = -1;
            for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
                if (eng->voices[i].active && eng->voices[i].env.current_env_level < min_lvl) {
                    min_lvl = eng->voices[i].env.current_env_level;
                    min_idx = i;
                }
            }
            if (min_idx != -1) voice_idx = min_idx;
        }
    }
    /* 5. 最古ボイススチール。極小音量ボイスを優先して切り、
     * アタック上昇中の新規ボイスは保護する (RELEASE 中か発音 100ms 経過のみ対象) */
    if (voice_idx == -1) {
        uint32_t max_age = 0;
        int oldest_idx = -1;
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
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
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
            VoiceSub2 *vo = &eng->voices[i];
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
 * @details 正準 build の出力 + live 状態 (mod/bendは毎タイル参照) + 乱数位相で
 *          ボイスを初期化する。係数変換 (exp/tan LUT、premix) のみ演奏コア側で
 *          行う (LUT は演奏コアにしかないため)。float 式は従来 note-on と同一。
 */
static void sub2_spawn_from_desc(Sub2LeadEngine *eng, VoiceSub2 *v, const SubSpawnDesc *d)
{
    uint8_t channel = d->channel;
    float velocity = (float)d->velocity / 127.0f;
    v->active = true;
    v->channel = channel;
    v->note = d->note;
    v->velocity = velocity;
    v->env.sustained_by_pedal = false;
    v->phase = 0.0f;
    v->age_samples = 0;

    /* base_increment は必ず「ベンドなし」の値を保存する。
     * レンダーループが毎回 ch->pitch_bend_semitones を掛けるため、
     * ここでベンド込みの値を保存するとベンドが二重適用される */
    v->frequency = sub_note_to_freq(d->note);
    v->phase_increment = d->base_increment;
    v->base_increment = d->base_increment;

    v->wave_type = (WaveType2)d->wave;
    v->env.adsr.attack_time_sec = d->adsr_a;
    v->env.adsr.decay_time_sec = d->adsr_d;
    v->env.adsr.sustain_level = d->adsr_s;
    v->env.adsr.release_time_sec = d->adsr_r;
    v->env.adsr.exponential_decay = (d->exp_decay != 0u);
    v->unison = (d->unison != 0u);
    v->wt_active = (d->wt_active != 0u);
    /* wt_morph は note-on 時の morph 解決専用で以降読まないため保持しない */
    if (v->wt_active) {
        v->morph_a = d->morph_a;
        v->morph_b = d->morph_b;
        v->morph_w = d->morph_w;
    }
    v->wt_mip = d->mip;

    /* モーフ済み単一テーブルを 1 回だけ生成 (従来と同一。カーネルは単表 fast path) */
    if (v->wt_active) {
        const float (*T)[SUBWT_SIZE + 1] = g_sub_wavbank[v->wt_mip];
        const float *Ta = T[v->morph_a];
        if (v->morph_w == 0.0f) {
            memcpy(v->premix, Ta, sizeof(v->premix));
        } else {
            const float *Tb = T[v->morph_b];
            for (int i = 0; i <= SUBWT_SIZE; i++) {
                v->premix[i] = Ta[i] + (Tb[i] - Ta[i]) * v->morph_w;
            }
        }
        v->use_premix = true;
    } else {
        v->use_premix = false;
    }

    /* Q32 位相/増分の初期化 (従来と同一式。plain mult のみで libm 呼びなし) */
    {
        const float Q32 = 4294967296.0f;
        float inc0 = v->phase_increment;
        bool hq = (eng->shared->main_ctrl.quality_flags & ASMP_QF_HQ_WIDE) != 0;
        v->qinc[0] = (uint32_t)(inc0 * Q32);
        v->qinc[1] = (uint32_t)(inc0 * SUB2_DETUNE_RATIO_HI  * Q32);
        v->qinc[2] = (uint32_t)(inc0 * SUB2_DETUNE_RATIO_LO  * Q32);
        v->qinc[3] = (uint32_t)(inc0 * (hq ? SUB2_DETUNE_RATIO_HI2_HQ : SUB2_DETUNE_RATIO_HI2) * Q32);
        v->qinc[4] = (uint32_t)(inc0 * (hq ? SUB2_DETUNE_RATIO_LO2_HQ : SUB2_DETUNE_RATIO_LO2) * Q32);
        /* 初期位相を LFSR で分散 (全位相 0 の同時スタートは位相干渉で
         * アタックが痩せるため。各オシに独立した乱数位相を与える) */
        uint32_t r = eng->lfsr_state;
        for (int k = 0; k < 5; k++) {
            r = (r >> 1) ^ (-(r & 1u) & 0xD0000001u);
            v->qph[k] = r;
        }
        eng->lfsr_state = r;
    }

    v->phase2 = v->phase3 = v->phase4 = v->phase5 = 0.0f;

    /* 共通 ADSR コアで発音準備 (ATTACK 開始) */
    sub_env_prepare_attack(&v->env);

    /* ペルボイス共振LPF + フィルターエンベロープ (正準値をそのまま使う) */
    v->filt_cutoff_base = d->filt_base;
    v->filt_peak_hz = d->filt_peak;
    v->filt_env = 1.0f;
    v->svf_q = d->filt_q;   /* タイル毎の係数再計算でも音色別 Q を維持 */
    {
        float tc_samples = SUB2_FILTER_ENV_TIME * (float)SUB_SAMPLE_RATE;
        v->filt_env_coeff = sub_exp_approx(1.0f / tc_samples);
        v->filt_env_coeff64 = sub_exp_approx((float)SUB2_MIX_TILE / tc_samples);
    }
    sub_svf_reset(&v->svf);
    sub_svf_set(&v->svf, v->filt_peak_hz, d->filt_q, (float)SUB_SAMPLE_RATE);
    /* HP はバス集約へ移行 (per-voice HP 廃止)。ラムブル除去はタイル後段で実施 */
    v->vib_phase = (float)((eng->lfsr_state++ & 0xFFu)) / 256.0f; /* 位相分散 */
    v->vib_inc = (4.6f + 0.8f * ((eng->lfsr_state >> 8) & 0x3u)) / (float)SUB_SAMPLE_RATE;
    v->vib_depth_st = eng->channels[channel].mod_depth * 0.45f;   /* CC#1 最大 ~0.5 半音 */
    v->drift_phase = (float)((eng->lfsr_state++ & 0xFFu)) / 256.0f;
    v->drift_inc = (0.12f + 0.10f * ((eng->lfsr_state >> 4) & 0x3u)) / (float)SUB_SAMPLE_RATE; /* 0.12-0.42Hz */
    spatial_voice_init(&v->spatial);
}

/**
 * @brief ディスクリプタ経由のノートオン (Core1分解送信の受け側)
 */
static void sub2_note_on_desc(Sub2LeadEngine *eng, const SubSpawnDesc *d)
{
    int voice_idx = sub2_voice_alloc(eng, d->channel, d->note);
    sub2_spawn_from_desc(eng, &eng->voices[voice_idx], d);
}

/**
 * @brief ノートオン (従来解釈 = 正準 build 関数に一本化)
 * @details Core1分解送信とbit一致させるため、解釈は sub_spawn_build_sub2 のみ。
 *          経路 (desc/従来) によらず同一 program は同一音になる。
 */
static void sub2_note_on(Sub2LeadEngine *eng, uint8_t channel, uint8_t note, uint8_t vel)
{
    if (vel == 0u) {
        sub2_note_off(eng, channel, note);
        return;
    }
    int voice_idx = sub2_voice_alloc(eng, channel, note);
    SubSpawnDesc d;
    bool gov_off = (eng->shared->main_ctrl.quality_flags & ASMP_QF_UNISON_OFF) != 0;
    sub_spawn_build_sub2(&d, channel, note, vel, eng->channels[channel].program, gov_off, false);
    sub2_spawn_from_desc(eng, &eng->voices[voice_idx], &d);
}

/**
 * @brief ボイスのリリース開始 (共通コアへ委譲)
 */
static void sub2_begin_release(VoiceSub2 *v)
{
    sub_env_begin_release(&v->env);
}

/**
 * @brief サステインペダル (CC#64) 更新
 *        踏み: 以降の Note Off をリリース延期にする / 離し: 延期分を一括解放
 */
static void sub2_channel_sustain(Sub2LeadEngine *eng, uint8_t channel, bool pedal_down)
{
    eng->channels[channel].sustain_pedal = pedal_down;
    if (!pedal_down) {
        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
            VoiceSub2 *v = &eng->voices[i];
            if (v->active && v->channel == channel && v->env.sustained_by_pedal) {
                v->env.sustained_by_pedal = false;
                if (v->env.env_state != SUB_ENV_RELEASE && v->env.env_state != SUB_ENV_IDLE) {
                    sub2_begin_release(v);
                }
            }
        }
    }
}

/**
 * @brief ノートオフ
 */
static void sub2_note_off(Sub2LeadEngine *eng, uint8_t channel, uint8_t note)
{
    for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
        VoiceSub2 *v = &eng->voices[i];
        if (v->active && v->channel == channel && v->note == note && v->env.env_state != SUB_ENV_RELEASE && v->env.env_state != SUB_ENV_IDLE) {
            if (eng->channels[channel].sustain_pedal) {
                /* ダンパーペダル踏み中: リリースを延期して保持 */
                v->env.sustained_by_pedal = true;
                continue;
            }
            sub2_begin_release(v);
        }
    }
}

/**
 * @brief 全音消音
 */
/**
 * @brief チャンネル指定消音 (channel >= 16 で全チャンネル)
 */
static void sub2_all_notes_off(Sub2LeadEngine *eng, uint8_t channel)
{
    for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
        VoiceSub2 *v = &eng->voices[i];
        if (v->active && (channel >= 16 || v->channel == channel)) {
            v->env.sustained_by_pedal = false;
            if (v->env.env_state != SUB_ENV_IDLE && v->env.env_state != SUB_ENV_RELEASE) {
                sub2_begin_release(v);
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

static void sub2_apply_pending_packet(const AsmpPacket *pkt)
{
    switch (pkt->msg_type) {
        case ASMP_MSG_NOTE_ON:
            if (pkt->channel == 9) {
                if (sub_drum_is_kick(pkt->data1)) sub_kick_note_on(&s_sub2_kick, (float)pkt->data2 / 127.0f);
                else if (sub_drum_is_metal(pkt->data1)) sub_metal_note_on(&s_sub2_metal, pkt->data1, (float)pkt->data2 / 127.0f);
                else sub_perc_note_on(&s_sub2_perc, pkt->data1, (float)pkt->data2 / 127.0f);
            } else if (pkt->channel < 16) {
                /* ABI v13: Core1分解送信トークンが有効なら fast-spawn。
                 * 無効 (param=0/陳腐化) なら正準 build で従来解釈する
                 * (同一関数なので Core1送信とbit一致する) */
                SubSpawnDesc desc;
                if (pkt->param != 0u &&
                    sub_spawn_consume(s_sub2.shared->spawn_pool_sub2,
                                      &s_sub2.shared->spawn_ack_sub2.consumed,
                                      pkt->param, &desc)) {
                    s_sub2.shared->spawn_stats_sub2.fast_spawn++;
                    asmp_dcache_clean((const void *)&s_sub2.shared->spawn_stats_sub2,
                                      sizeof(s_sub2.shared->spawn_stats_sub2));
                    ASMP_BARRIER();
                    sub2_note_on_desc(&s_sub2, &desc);
                } else {
                    s_sub2.shared->spawn_stats_sub2.legacy_spawn++;
                    asmp_dcache_clean((const void *)&s_sub2.shared->spawn_stats_sub2,
                                      sizeof(s_sub2.shared->spawn_stats_sub2));
                    ASMP_BARRIER();
                    if (pkt->param != 0u) {
                        /* 陳腐トークンの解放 (スロット再利用のため ack のみ) */
                        sub_spawn_ack_only(s_sub2.shared->spawn_pool_sub2,
                                           &s_sub2.shared->spawn_ack_sub2.consumed,
                                           pkt->param);
                    }
                    sub2_note_on(&s_sub2, pkt->channel, pkt->data1, pkt->data2);
                }
            }
            break;
        case ASMP_MSG_NOTE_OFF:
            if (pkt->channel != 9 && pkt->channel < 16) sub2_note_off(&s_sub2, pkt->channel, pkt->data1);
            break;
        case ASMP_MSG_PROGRAM_CHANGE:
            if (pkt->channel < 16) s_sub2.channels[pkt->channel].program = pkt->data1;
            break;
        case ASMP_MSG_CONTROL_CHANGE:
            if (pkt->channel == 9) {
                if (pkt->data1 == 7) {
                    s_sub2_kick.volume = (float)pkt->data2 / 127.0f;
                    s_sub2_metal.volume = (float)pkt->data2 / 127.0f;
                    s_sub2_perc.volume = (float)pkt->data2 / 127.0f;
                } else if (pkt->data1 == 91) {
                    /* Ch9 CC91はリバーブ送り: ドラムvolumeへ流用すると残響で音量が跳ねるので無視 */
                }
            } else if (pkt->channel < 16) {
                float norm = (float)pkt->data2 / 127.0f;
                if (pkt->data1 == 7) s_sub2.channels[pkt->channel].volume = norm;
                if (pkt->data1 == 10) { s_sub2.channels[pkt->channel].pan = norm; sub_channel_update_pan_gains(&s_sub2.channels[pkt->channel]); }
                if (pkt->data1 == 11) s_sub2.channels[pkt->channel].expression = norm;
                if (pkt->data1 == 1) s_sub2.channels[pkt->channel].mod_depth = norm;
                if (pkt->data1 == 91) s_sub2.channels[pkt->channel].reverb_send = norm;
                if (pkt->data1 == 64) sub2_channel_sustain(&s_sub2, pkt->channel, pkt->data2 >= 64);
                if (pkt->data1 == 120 || pkt->data1 == 123) {
                    for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
                        VoiceSub2 *v = &s_sub2.voices[i];
                        if (v->active && v->channel == pkt->channel) {
                            if (pkt->data1 == 120) { v->active = false; v->env.env_state = SUB_ENV_IDLE; v->env.current_env_level = 0.0f; v->env.sustained_by_pedal = false; }
                            else if (v->env.env_state != SUB_ENV_IDLE && v->env.env_state != SUB_ENV_RELEASE) sub2_begin_release(v);
                        }
                    }
                    s_sub2.channels[pkt->channel].sustain_pedal = false;
                }
            }
            break;
        case ASMP_MSG_PITCH_BEND:
            if (pkt->channel < 16) {
                int16_t bend = (int16_t)((int32_t)pkt->param);
                s_sub2.channels[pkt->channel].pitch_bend_semitones = ((float)bend / 8192.0f) * 2.0f;
            }
            break;
        case ASMP_MSG_ALL_NOTES_OFF:
            {
                uint8_t target_ch = (pkt->channel >= 16 || pkt->data1 == 0xFF) ? 0xFF : pkt->channel;
                sub2_all_notes_off(&s_sub2, target_ch);
                if (target_ch == 9 || target_ch >= 16) {
                    sub_kick_all_notes_off(&s_sub2_kick);
                    sub_metal_all_notes_off(&s_sub2_metal);
                    sub_perc_all_notes_off(&s_sub2_perc);
                }
            }
            break;
        default: break;
    }
}

/**
 * @brief 指定フェーズでの波形サンプル (ユニソンで複数回呼び出すため分離)
 */
static inline float sub2_wave_at(WaveType2 wt, float phase, float dt, const float *lut)
{
    switch (wt) {
        case WAVE2_SINE:
            return sub_lookup_sine(g_sine_lut, phase);
        case WAVE2_SQUARE:
            return sub_osc_square(phase, dt);
        case WAVE2_SAWTOOTH:
            return sub_osc_saw(phase, dt);
        case WAVE2_TRIANGLE:
            return sub_osc_triangle(phase, dt);
        default:
            return 0.0f;
    }
}

/* ========================================================================= */
/* 6 カーネル静的分割: WT / Classic x 1/3/5 oscillator                       */
/* 動的分岐・不要変数をスコープから完全排除し、Cortex-M4F のスタックスピルを根絶 */
/* ========================================================================= */

/**
 * @brief ウェーブテーブル 5 オシレータ スーパーソウ カーネル
 */
static void sub2_render_wt5(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                             const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const float *ptab = v->premix;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    uint32_t p0 = v->qph[0], p1 = v->qph[1], p2 = v->qph[2], p3 = v->qph[3], p4 = v->qph[4];
    uint32_t iq0 = v->qinc[0], iq1 = v->qinc[1], iq2 = v->qinc[2], iq3 = v->qinc[3], iq4 = v->qinc[4];
    /* アナログドリフト 64fr毎 1LUT (0.12Hz, ±2.5cent) - 厚み用、CPU 0.02ms */
    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;
    else if (v->drift_phase < 0.0f) v->drift_phase += 1.0f;

    const float detune_hi2 = s_hq_wide ? SUB2_DETUNE_RATIO_HI2_HQ : SUB2_DETUNE_RATIO_HI2;
    const float detune_lo2 = s_hq_wide ? SUB2_DETUNE_RATIO_LO2_HQ : SUB2_DETUNE_RATIO_LO2;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
            iq1 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_HI  * Q32);
            iq2 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_LO  * Q32);
            iq3 = (uint32_t)(inc_f * detune_hi2 * Q32);
            iq4 = (uint32_t)(inc_f * detune_lo2 * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt5;
                }
                v->age_samples++;
    
                float o1 = subwt_read_raw_q32(ptab, p0);
                float o2 = subwt_read_raw_q32(ptab, p1);
                float o3 = subwt_read_raw_q32(ptab, p2);
                float o4 = subwt_read_raw_q32(ptab, p3);
                float o5 = subwt_read_raw_q32(ptab, p4);
    
                float osc = fmaf(o5, 0.32f, fmaf(o4, 0.32f, fmaf(o3, 0.32f, fmaf(o2, 0.32f, o1 * 0.32f))));
                float side = (o2 + o4) - (o3 + o5);
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    float l, r;
                    spatial_process_sample(&v->spatial, filtered * env * ch_gain, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    l += spread * spatial_l_gain;
                    r -= spread * spatial_r_gain;
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                p0 += iq0; p1 += iq1; p2 += iq2; p3 += iq3; p4 += iq4;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
            iq1 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_HI  * Q32);
            iq2 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_LO  * Q32);
            iq3 = (uint32_t)(inc_f * detune_hi2 * Q32);
            iq4 = (uint32_t)(inc_f * detune_lo2 * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt5;
                }
                v->age_samples++;
    
                float o1 = subwt_read_raw_q32(ptab, p0);
                float o2 = subwt_read_raw_q32(ptab, p1);
                float o3 = subwt_read_raw_q32(ptab, p2);
                float o4 = subwt_read_raw_q32(ptab, p3);
                float o5 = subwt_read_raw_q32(ptab, p4);
    
                float osc = fmaf(o5, 0.32f, fmaf(o4, 0.32f, fmaf(o3, 0.32f, fmaf(o2, 0.32f, o1 * 0.32f))));
                float side = (o2 + o4) - (o3 + o5);
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    /* #音質改善C: spread は既に env*ch_gain 込み (下記 spatial_on 分岐と同型)。
                     * 従来コードは (filtered+spread)*env*ch_gain としており spread 側に
                     * env*ch_gain が二重適用され、アタック/リリースやベロシティに応じて
                     * ユニゾン幅が非線形に変化する (弱奏で不自然に狭まる) バグがあった */
                    float base = filtered * env * ch_gain;
                    mix_l[f] += (base + spread) * pan_l;
                    mix_r[f] += (base - spread) * pan_r;
                
    
                p0 += iq0; p1 += iq1; p2 += iq2; p3 += iq3; p4 += iq4;
            }
        }
    }

exit_wt5:
    v->qph[0] = p0; v->qph[1] = p1; v->qph[2] = p2; v->qph[3] = p3; v->qph[4] = p4;
    v->qinc[0] = iq0; v->qinc[1] = iq1; v->qinc[2] = iq2; v->qinc[3] = iq3; v->qinc[4] = iq4;
}

/**
 * @brief ウェーブテーブル 3 オシレータ カーネル
 */
static void sub2_render_wt3(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                             const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const float *ptab = v->premix;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    uint32_t p0 = v->qph[0], p1 = v->qph[1], p2 = v->qph[2];
    uint32_t iq0 = v->qinc[0], iq1 = v->qinc[1], iq2 = v->qinc[2];

    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
            iq1 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_HI * Q32);
            iq2 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_LO * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt3;
                }
                v->age_samples++;
    
                float o1 = subwt_read_raw_q32(ptab, p0);
                float o2 = subwt_read_raw_q32(ptab, p1);
                float o3 = subwt_read_raw_q32(ptab, p2);
    
                float osc = fmaf(o3, 0.42f, fmaf(o2, 0.42f, o1 * 0.42f));
                float side = o2 - o3;
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    float l, r;
                    spatial_process_sample(&v->spatial, filtered * env * ch_gain, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    l += spread * spatial_l_gain;
                    r -= spread * spatial_r_gain;
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                p0 += iq0; p1 += iq1; p2 += iq2;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
            iq1 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_HI * Q32);
            iq2 = (uint32_t)(inc_f * SUB2_DETUNE_RATIO_LO * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt3;
                }
                v->age_samples++;
    
                float o1 = subwt_read_raw_q32(ptab, p0);
                float o2 = subwt_read_raw_q32(ptab, p1);
                float o3 = subwt_read_raw_q32(ptab, p2);
    
                float osc = fmaf(o3, 0.42f, fmaf(o2, 0.42f, o1 * 0.42f));
                float side = o2 - o3;
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    /* #音質改善C: spread は既に env*ch_gain 込み (下記 spatial_on 分岐と同型)。
                     * 従来コードは (filtered+spread)*env*ch_gain としており spread 側に
                     * env*ch_gain が二重適用され、アタック/リリースやベロシティに応じて
                     * ユニゾン幅が非線形に変化する (弱奏で不自然に狭まる) バグがあった */
                    float base = filtered * env * ch_gain;
                    mix_l[f] += (base + spread) * pan_l;
                    mix_r[f] += (base - spread) * pan_r;
                
    
                p0 += iq0; p1 += iq1; p2 += iq2;
            }
        }
    }

exit_wt3:
    v->qph[0] = p0; v->qph[1] = p1; v->qph[2] = p2;
    v->qinc[0] = iq0; v->qinc[1] = iq1; v->qinc[2] = iq2;
}

/**
 * @brief ウェーブテーブル 1 オシレータ カーネル
 */
static void sub2_render_wt1(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                             const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const float *ptab = v->premix;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    uint32_t p0 = v->qph[0];
    uint32_t iq0 = v->qinc[0];

    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt1;
                }
                v->age_samples++;
    
                float osc = subwt_read_raw_q32(ptab, p0);
    
                float filtered = sub_svf_lp(&v->svf, osc);
                float sample = filtered * env * ch_gain;
    
                    float l, r;
                    spatial_process_sample(&v->spatial, sample, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                p0 += iq0;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            float inc_f = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = inc_f;
            const float Q32 = 4294967296.0f;
            iq0 = (uint32_t)(inc_f * Q32);
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_wt1;
                }
                v->age_samples++;
    
                float osc = subwt_read_raw_q32(ptab, p0);
    
                float filtered = sub_svf_lp(&v->svf, osc);
                float sample = filtered * env * ch_gain;
    
                    mix_l[f] += sample * pan_l;
                    mix_r[f] += sample * pan_r;
                
    
                p0 += iq0;
            }
        }
    }

exit_wt1:
    v->qph[0] = p0;
    v->qinc[0] = iq0;
}

/**
 * @brief Classic (PolyBLEP) 5 オシレータ スーパーソウ カーネル
 */
static void sub2_render_classic5(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                                  const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const WaveType2 wt = v->wave_type;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    float ph0 = v->phase, ph1 = v->phase2, ph2 = v->phase3, ph3 = v->phase4, ph4 = v->phase5;
    float dt = v->phase_increment;
    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;

    const float detune_hi2 = s_hq_wide ? SUB2_DETUNE_RATIO_HI2_HQ : SUB2_DETUNE_RATIO_HI2;
    const float detune_lo2 = s_hq_wide ? SUB2_DETUNE_RATIO_LO2_HQ : SUB2_DETUNE_RATIO_LO2;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic5;
                }
                v->age_samples++;
    
                float o1 = sub2_wave_at(wt, ph0, dt, g_sine_lut);
                float o2 = sub2_wave_at(wt, ph1, dt * SUB2_DETUNE_RATIO_HI, g_sine_lut);
                float o3 = sub2_wave_at(wt, ph2, dt * SUB2_DETUNE_RATIO_LO, g_sine_lut);
                float o4 = sub2_wave_at(wt, ph3, dt * detune_hi2, g_sine_lut);
                float o5 = sub2_wave_at(wt, ph4, dt * detune_lo2, g_sine_lut);
    
                float osc = fmaf(o5, 0.32f, fmaf(o4, 0.32f, fmaf(o3, 0.32f, fmaf(o2, 0.32f, o1 * 0.32f))));
                float side = (o2 + o4) - (o3 + o5);
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    float l, r;
                    spatial_process_sample(&v->spatial, filtered * env * ch_gain, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    l += spread * spatial_l_gain;
                    r -= spread * spatial_r_gain;
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
                ph1 += dt * SUB2_DETUNE_RATIO_HI; if (ph1 >= 1.0f) ph1 -= 1.0f;
                ph2 += dt * SUB2_DETUNE_RATIO_LO; if (ph2 >= 1.0f) ph2 -= 1.0f;
                ph3 += dt * detune_hi2; if (ph3 >= 1.0f) ph3 -= 1.0f;
                ph4 += dt * detune_lo2; if (ph4 >= 1.0f) ph4 -= 1.0f;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic5;
                }
                v->age_samples++;
    
                float o1 = sub2_wave_at(wt, ph0, dt, g_sine_lut);
                float o2 = sub2_wave_at(wt, ph1, dt * SUB2_DETUNE_RATIO_HI, g_sine_lut);
                float o3 = sub2_wave_at(wt, ph2, dt * SUB2_DETUNE_RATIO_LO, g_sine_lut);
                float o4 = sub2_wave_at(wt, ph3, dt * detune_hi2, g_sine_lut);
                float o5 = sub2_wave_at(wt, ph4, dt * detune_lo2, g_sine_lut);
    
                float osc = fmaf(o5, 0.32f, fmaf(o4, 0.32f, fmaf(o3, 0.32f, fmaf(o2, 0.32f, o1 * 0.32f))));
                float side = (o2 + o4) - (o3 + o5);
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    /* #音質改善C: spread は既に env*ch_gain 込み (下記 spatial_on 分岐と同型)。
                     * 従来コードは (filtered+spread)*env*ch_gain としており spread 側に
                     * env*ch_gain が二重適用され、アタック/リリースやベロシティに応じて
                     * ユニゾン幅が非線形に変化する (弱奏で不自然に狭まる) バグがあった */
                    float base = filtered * env * ch_gain;
                    mix_l[f] += (base + spread) * pan_l;
                    mix_r[f] += (base - spread) * pan_r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
                ph1 += dt * SUB2_DETUNE_RATIO_HI; if (ph1 >= 1.0f) ph1 -= 1.0f;
                ph2 += dt * SUB2_DETUNE_RATIO_LO; if (ph2 >= 1.0f) ph2 -= 1.0f;
                ph3 += dt * detune_hi2; if (ph3 >= 1.0f) ph3 -= 1.0f;
                ph4 += dt * detune_lo2; if (ph4 >= 1.0f) ph4 -= 1.0f;
            }
        }
    }

exit_classic5:
    v->phase = ph0; v->phase2 = ph1; v->phase3 = ph2; v->phase4 = ph3; v->phase5 = ph4;
}

/**
 * @brief Classic (PolyBLEP) 3 オシレータ カーネル
 */
static void sub2_render_classic3(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                                 const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const WaveType2 wt = v->wave_type;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    float ph0 = v->phase, ph1 = v->phase2, ph2 = v->phase3;
    float dt = v->phase_increment;
    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic3;
                }
                v->age_samples++;
    
                float o1 = sub2_wave_at(wt, ph0, dt, g_sine_lut);
                float o2 = sub2_wave_at(wt, ph1, dt * SUB2_DETUNE_RATIO_HI, g_sine_lut);
                float o3 = sub2_wave_at(wt, ph2, dt * SUB2_DETUNE_RATIO_LO, g_sine_lut);
    
                float osc = fmaf(o3, 0.42f, fmaf(o2, 0.42f, o1 * 0.42f));
                float side = o2 - o3;
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    float l, r;
                    spatial_process_sample(&v->spatial, filtered * env * ch_gain, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    l += spread * spatial_l_gain;
                    r -= spread * spatial_r_gain;
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
                ph1 += dt * SUB2_DETUNE_RATIO_HI; if (ph1 >= 1.0f) ph1 -= 1.0f;
                ph2 += dt * SUB2_DETUNE_RATIO_LO; if (ph2 >= 1.0f) ph2 -= 1.0f;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic3;
                }
                v->age_samples++;
    
                float o1 = sub2_wave_at(wt, ph0, dt, g_sine_lut);
                float o2 = sub2_wave_at(wt, ph1, dt * SUB2_DETUNE_RATIO_HI, g_sine_lut);
                float o3 = sub2_wave_at(wt, ph2, dt * SUB2_DETUNE_RATIO_LO, g_sine_lut);
    
                float osc = fmaf(o3, 0.42f, fmaf(o2, 0.42f, o1 * 0.42f));
                float side = o2 - o3;
    
                float filtered = sub_svf_lp(&v->svf, osc);
    
                float spread = side * env * ch_gain * 0.35f;
                    /* #音質改善C: spread は既に env*ch_gain 込み (下記 spatial_on 分岐と同型)。
                     * 従来コードは (filtered+spread)*env*ch_gain としており spread 側に
                     * env*ch_gain が二重適用され、アタック/リリースやベロシティに応じて
                     * ユニゾン幅が非線形に変化する (弱奏で不自然に狭まる) バグがあった */
                    float base = filtered * env * ch_gain;
                    mix_l[f] += (base + spread) * pan_l;
                    mix_r[f] += (base - spread) * pan_r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
                ph1 += dt * SUB2_DETUNE_RATIO_HI; if (ph1 >= 1.0f) ph1 -= 1.0f;
                ph2 += dt * SUB2_DETUNE_RATIO_LO; if (ph2 >= 1.0f) ph2 -= 1.0f;
            }
        }
    }

exit_classic3:
    v->phase = ph0; v->phase2 = ph1; v->phase3 = ph2;
}

/**
 * @brief Classic (PolyBLEP) 1 オシレータ カーネル
 */
static void sub2_render_classic1(VoiceSub2 * __restrict v, const SubChannel * __restrict ch,
                                 const float *lut, float *__restrict mix_l, float *__restrict mix_r, uint32_t tile_frames, bool spatial_on)
{
    const float ch_gain = ch->volume * ch->expression * v->velocity * 0.40f;
    const float pan_l = ch->pan_gain_l;
    const float pan_r = ch->pan_gain_r;
    const float ch_bend = ch->pitch_bend_semitones;
    const float vib_depth = ch->mod_depth * 0.45f;
    const WaveType2 wt = v->wave_type;
    float spatial_az = 0, spatial_itd = 0, spatial_l_gain = 0, spatial_r_gain = 0, spatial_lp = 1.0f;
    if (spatial_on) {
        spatial_az = spatial_stage_azimuth(ch->pan, v->channel, v->note);
        spatial_itd = spatial_itd_samples(spatial_az);
        spatial_ild_gains(spatial_az, &spatial_l_gain, &spatial_r_gain, &spatial_lp);
    }

    float ph0 = v->phase;
    float dt = v->phase_increment;
    float drift_semi = 0.025f * sub_lookup_sine(g_sine_lut, v->drift_phase);
    v->drift_phase += v->drift_inc * (float)tile_frames;
    if (v->drift_phase >= 1.0f) v->drift_phase -= 1.0f;

    if (spatial_on) {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic1;
                }
                v->age_samples++;
    
                float osc = sub2_wave_at(wt, ph0, dt, g_sine_lut);
    
                float filtered = sub_svf_lp(&v->svf, osc);
                float sample = filtered * env * ch_gain;
    
                    float l, r;
                    spatial_process_sample(&v->spatial, sample, spatial_az, spatial_itd,
                                           spatial_l_gain, spatial_r_gain, spatial_lp, &l, &r);
                    mix_l[f] += l;
                    mix_r[f] += r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
            }
        }
    } else {
        for (uint32_t f4 = 0; f4 < tile_frames; f4 += 4) {
            uint32_t chunk = tile_frames - f4;
            if (chunk > 4) chunk = 4;
    
            /* 4 サンプル毎の LFO ビブラート / ピッチ増分更新 */
            v->vib_phase += v->vib_inc * 4.0f;
            if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
            float vib_semi = vib_depth * sub_lookup_sine(g_sine_lut, v->vib_phase);
            dt = v->base_increment * sub_semitone_ratio(ch_bend + vib_semi + drift_semi);
            v->phase_increment = dt;
    
            for (uint32_t k = 0; k < chunk; k++) {
                uint32_t f = f4 + k;
                float env = sub_env_advance(&v->env);
                if (v->env.env_state == SUB_ENV_IDLE) {
                    v->active = false;
                    goto exit_classic1;
                }
                v->age_samples++;
    
                float osc = sub2_wave_at(wt, ph0, dt, g_sine_lut);
    
                float filtered = sub_svf_lp(&v->svf, osc);
                float sample = filtered * env * ch_gain;
    
                    mix_l[f] += sample * pan_l;
                    mix_r[f] += sample * pan_r;
                
    
                ph0 += dt; if (ph0 >= 1.0f) ph0 -= 1.0f;
            }
        }
    }

exit_classic1:
    v->phase = ph0;
}

/**
 * @brief PCM レンダリング (float タイル蓄積 + 静的カーネルディスパッチ)
 *
 * 負荷・音質最適化:
 *  - SUB2_MIX_TILE (64フレーム) float タイルバッファに各ボイス出力を蓄積し、
 *    **float のまま**コア間 PCM バスへ書き出す (中間量子化を完全排除)。
 *    最終 16bit への量子化は Sub5 マスター出力段で TPDF ディザ付き 1 回のみ
 *  - WT / Classic x 1/3/5 oscillator の 6 カーネルに静的分割し、ループ内動的分岐を全廃
 *  - 4 サンプル外側グループ化で LFO / ベンド / 位相増分を更新
 *  - フィルターエンベロープは 64 サンプル一括減衰 (filt_env_coeff64)
 *  - ガバナー (quality_flags) によりユニソンを 5 -> 3 -> 1 オシレータへ段階削減
 */
static bool sub2_render(Sub2LeadEngine *eng, float *buffer, uint32_t frames, uint32_t slot, uint32_t req_epoch)
{
    uint16_t active_count = 0;

    /* ガバナー段階の取得 (Main Core が単一ライタ) */
    uint8_t qf = eng->shared->main_ctrl.quality_flags;
    int gov_osc = 5;
    if (qf & ASMP_QF_UNISON_3OSC) gov_osc = 3;
    if (qf & ASMP_QF_UNISON_OFF)  gov_osc = 1;
    /* GOV2(1-osc)時はspatialとHPをbypass、GOV1(3-osc)時はHQ_WIDEで広がり補償してトン緩和 */
    bool spatial_on = (eng->shared->main_ctrl.spatial_enable != 0) && !(qf & ASMP_QF_UNISON_OFF);
    s_hq_wide = ((qf & ASMP_QF_HQ_WIDE) != 0) || ((qf & ASMP_QF_UNISON_3OSC) && !(qf & ASMP_QF_UNISON_OFF));
    s_sub2_hp_bypass = (qf & ASMP_QF_UNISON_OFF) != 0;

    /* 黄金律復元: 8音強制カリングを撤廃し、16音完全ポリフォニーを維持する。
     * 単一オシレータ動作時は 16 音でもデッドラインに余裕があるため、
     * 強制消音によるブツ切りクリック音や和音欠落を完全に排除する */

    for (uint32_t t_start = 0; t_start < frames; t_start += SUB2_MIX_TILE) {
        /* 所有権検証 (Early Abort): 遅延してMainがスロットを再割り当てしていたら直ちに中断 */
        if (!asmp_slot_validate(eng->shared, slot, req_epoch)) {
            return false;
        }

        uint32_t tile_frames = frames - t_start;
        if (tile_frames > SUB2_MIX_TILE) tile_frames = SUB2_MIX_TILE;

        float mix_l[SUB2_MIX_TILE];
        float mix_r[SUB2_MIX_TILE];
        memset(mix_l, 0, tile_frames * sizeof(float));
        memset(mix_r, 0, tile_frames * sizeof(float));

        for (int i = 0; i < SUB2_MAX_POLYPHONY; i++) {
            VoiceSub2 *v = &eng->voices[i];
            if (!v->active || v->env.env_state == SUB_ENV_IDLE) continue;

            if (t_start == 0) {
                active_count++;
            }

            SubChannel *ch = &eng->channels[v->channel];
            int osc_n = v->unison ? gov_osc : 1;

            if (v->wt_active) {
                if (osc_n == 5) {
                    sub2_render_wt5(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                } else if (osc_n == 3) {
                    sub2_render_wt3(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                } else {
                    sub2_render_wt1(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                }
            } else {
                if (osc_n == 5) {
                    sub2_render_classic5(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                } else if (osc_n == 3) {
                    sub2_render_classic3(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                } else {
                    sub2_render_classic1(v, ch, g_sine_lut, mix_l, mix_r, tile_frames, spatial_on);
                }
            }

            /* タイル完了後の SVF 係数一括更新 (tile_frames サンプル分) */
            if (v->active && v->env.env_state != SUB_ENV_IDLE) {
                if (tile_frames == SUB2_MIX_TILE) {
                    v->filt_env *= v->filt_env_coeff64;
                } else if (tile_frames > 0) {
                    v->filt_env *= sub_exp_approx((float)tile_frames / (SUB2_FILTER_ENV_TIME * (float)SUB_SAMPLE_RATE));
                }
                float cut = v->filt_cutoff_base +
                            (v->filt_peak_hz - v->filt_cutoff_base) * v->filt_env;
                sub_svf_set(&v->svf, cut, v->svf_q, (float)SUB_SAMPLE_RATE);
            }
        }

        /* ランブル除去 HP をバス後段へ集約 (全ボイス合算後に L/R 各 1 回)。
         * HP・LP・パン・Spatial は全て LTI のため per-voice HP と等価。
         * 16voice×毎サンプル → 2ch×毎サンプルへ削減。
         * GOV2 時は従来通り bypass (s_sub2_hp_bypass)。
         * ドラム (Kick/Metal/Perc) は HP 対象外だったため HP 後に加算し bit 完全一致を保つ */
        if (!s_sub2_hp_bypass) {
            for (uint32_t f = 0; f < tile_frames; f++) {
                mix_l[f] = sub_svf_hp(&eng->bus_hp_l, mix_l[f]);
                mix_r[f] = sub_svf_hp(&eng->bus_hp_r, mix_r[f]);
            }
        }

        /* Phase 4/5/6: 移行された Kick/Metal/Perc をモノラルでタイルへ加算 (L=R) */
        sub_kick_render(&s_sub2_kick, mix_l, mix_r, tile_frames);
        sub_metal_render(&s_sub2_metal, mix_l, mix_r, tile_frames);
        sub_perc_render(&s_sub2_perc, mix_l, mix_r, tile_frames);

        /* float のまま PCM バスへ書き出し (振幅域 [-1,1] 超過は Sub5 側の
         * ゲインステージングとリミッターで吸収するためクランプ不要) */
        float *dst = &buffer[t_start * 2];
        for (uint32_t f = 0; f < tile_frames; f++) {
            dst[f * 2 + 0] = mix_l[f];
            dst[f * 2 + 1] = mix_r[f];
        }
    }

    eng->shared->core[ASMP_CORE_SUB2_LEAD].voice_count = active_count;
    /* Core1入場整理の飽和判定用に公開 (busy_us と同一規律で clean) */
    asmp_dcache_clean((const void *)&eng->shared->core[ASMP_CORE_SUB2_LEAD].voice_count,
                      sizeof(eng->shared->core[ASMP_CORE_SUB2_LEAD].voice_count));
    return true;
}

/**
 * @brief SubCore 2 エントリーポイント
 */
void *subcore2_entry(void *arg)
{
    AsmpSharedContext *shared = (AsmpSharedContext *)arg;
    if (!shared) return NULL;
    if (!asmp_abi_ok(shared)) {
        printf("[SUB2][FATAL] ABI mismatch at entry magic=%08x ver=%u size=%u exp %u\n",
               (unsigned)shared->abi_magic, (unsigned)shared->abi_version,
               (unsigned)shared->abi_size, (unsigned)sizeof(AsmpSharedContext));
        return NULL;
    }
    sub_fpu_denormal_init(); /* デノーマル例外ペナルティによるじりじりノイズ防止 */

    sub2_engine_init(&s_sub2, shared);

    /* ビルド識別 + デチューン比の自己診断。
     * マクロ値を実セント換算して出力するので、±12.00/±28.00 が出れば
     * JP-8000 比のバイナリで動作していることが証明される */
    printf("[SUB2][BUILD] %s | unison detune = %+.2f / %+.2f cent (JP8000 0.12/0.28 st)\n",
           HEXASENSE_DSP_TAG,
           (double)(1200.0f * logf(SUB2_DETUNE_RATIO_LO) / logf(2.0f)),
           (double)(1200.0f * logf(SUB2_DETUNE_RATIO_HI2) / logf(2.0f)));

    /* CPU 負荷メーター (実測ビジー時間比率) */
    SubLoadMeter load_m; SUB_LOAD_INIT(load_m);

    uint32_t idle_loops = 0;
    uint32_t req_epoch = 0;

    while (!shared->main_ctrl.shutdown_requested) {
        /* 1. キューからメッセージを処理 (RENDER_REQで必ずbreak - エポックスキップ防止) */
        AsmpPacket pkt;
        bool has_render_req = false;

        /* 1. キューからMIDIイベントを蓄積 (RENDER_REQまで) - sample_offset保持 */
        while (asmp_queue_pop(&shared->queues[ASMP_CORE_SUB2_LEAD], &pkt)) {
            if (pkt.msg_type == ASMP_MSG_RENDER_REQ) {
                has_render_req = true;
                req_epoch = pkt.param;
                break;
            }
            /* 緊急消音（ALL_NOTES_OFF / CC120）はRENDER_REQを待たずに即時反映 */
            if (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF ||
                (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 120)) {
                sub2_apply_pending_packet(&pkt);
                continue;
            }
            if (s_sub2_pending_cnt < SUB2_MAX_PENDING) {
                s_sub2_pending[s_sub2_pending_cnt++] = pkt;
            } else {
                bool is_release = (pkt.msg_type == ASMP_MSG_NOTE_OFF) ||
                                  (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 64 && pkt.data2 < 64) ||
                                  (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF);
                if (is_release) {
                    /* 溢れても消音は必達: NOTE_ONを1件追い出してでも確保。
                     * 追い出し対象は最低ベロシティ (マスキングで最も聴こえない音)。
                     * 退避は diag_queue_drop で可視化する (従来は無計測の欠落だった) */
                    int rep = -1;
                    uint8_t rep_vel = 0xFFu;
                    for (int i = 0; i < (int)SUB2_MAX_PENDING; i++) {
                        if (s_sub2_pending[i].msg_type == ASMP_MSG_NOTE_ON &&
                            s_sub2_pending[i].data2 <= rep_vel) {
                            rep_vel = s_sub2_pending[i].data2;
                            rep = i;
                        }
                    }
                    if (rep >= 0) {
                        /* 追い出される NOTE_ON のトークンがあれば解放する
                         * (descriptor リーク = producer  wedging の防止)。
                         * [MAX-1] 上書き側は NOTE_ON 不在確定のため不要 */
                        if (s_sub2_pending[rep].msg_type == ASMP_MSG_NOTE_ON &&
                            s_sub2_pending[rep].param != 0u) {
                            sub_spawn_ack_only(shared->spawn_pool_sub2,
                                               &shared->spawn_ack_sub2.consumed,
                                               s_sub2_pending[rep].param);
                        }
                        s_sub2_pending[rep] = pkt;
                    }
                    else s_sub2_pending[SUB2_MAX_PENDING - 1] = pkt;
                    shared->diag_queue_drop++;
                } else {
                    /* 溢れで捨てる NOTE_ON のトークンも解放する */
                    if (pkt.msg_type == ASMP_MSG_NOTE_ON && pkt.param != 0u) {
                        sub_spawn_ack_only(shared->spawn_pool_sub2,
                                           &shared->spawn_ack_sub2.consumed,
                                           pkt.param);
                    }
                    shared->diag_queue_drop++;
                }
            }
        }

        if (has_render_req) {
            /* エポック連続性検証 (Commit1: カウンタのみ) */
            static uint32_t s_last_epoch = 0;
            if (s_last_epoch != 0 && req_epoch != s_last_epoch + 1u) {
                shared->diag_epoch_gap[ASMP_CORE_SUB2_LEAD]++;
            }
            s_last_epoch = req_epoch;
            /* カオスストレステスト専用: 外部割込み相当のランダム遅延 (通常ビルドは無効) */
            SUB_CHAOS_DELAY(ASMP_CORE_SUB2_LEAD);
            SUB_LOAD_BUSY_BEGIN(load_m);
            uint64_t t0_ns = sub_get_ns();
            /* S3: per-slot化 - スロット別フレーム数とepochで競合根絶 */
            uint32_t slot = ASMP_EPOCH_SLOT(req_epoch);
            if (!asmp_abi_ok(shared)) {
                printf("[SUB2][FATAL] ABI mismatch magic=%08x ver=%u size=%u expected %u\n",
                       (unsigned)shared->abi_magic, (unsigned)shared->abi_version,
                       (unsigned)shared->abi_size, (unsigned)sizeof(AsmpSharedContext));
                return NULL;
            }
            if (shared->render_ctrl.slot_epoch[slot] != req_epoch) {
                shared->diag_slot_mismatch++;
                /* stale PCMを描かない - 次エポックでリカバリ */
                shared->done_epoch[ASMP_CORE_SUB2_LEAD].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB2_LEAD].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                s_sub2_pending_cnt = 0;
                continue;
            }
            uint32_t ef = shared->render_ctrl.epoch_frames[slot];
#ifdef PROFILE_ENABLE
    profile_epoch_start(2, req_epoch);
#endif
            if (ef == 0u || ef > ASMP_BUFFER_FRAMES) ef = ASMP_BUFFER_FRAMES;

            /* sample_offsetで安定ソート (挿入ソート: 128件以下で十分高速) */
            for (uint32_t i = 1; i < s_sub2_pending_cnt; i++) {
                AsmpPacket key = s_sub2_pending[i];
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && s_sub2_pending[j].sample_offset > key.sample_offset) {
                    s_sub2_pending[j+1] = s_sub2_pending[j];
                    j--;
                }
                s_sub2_pending[j+1] = key;
            }
            /* 2. サンプルオフセット分割レンダー: P0-B scratchで破壊防止 */
            bool render_ok = true;
            if (s_sub2_pending_cnt == 0) {
                render_ok = sub2_render(&s_sub2, shared->pcm_sub2_melody[slot], ef, slot, req_epoch);
            } else {
                uint32_t cursor = 0;
                uint32_t pi = 0;
                memset(shared->pcm_sub2_melody[slot], 0, ef * sizeof(float) * 2);
                while (pi < s_sub2_pending_cnt) {
                    uint32_t off = s_sub2_pending[pi].sample_offset;
                    if (off > ef) off = ef;
                    if (off > cursor) {
                        if (!sub2_render(&s_sub2, shared->pcm_sub2_melody[slot] + cursor * 2, off - cursor, slot, req_epoch)) {
                            render_ok = false;
                            break;
                        }
                        cursor = off;
                    }
                    uint32_t cur_off = s_sub2_pending[pi].sample_offset;
                    while (pi < s_sub2_pending_cnt && s_sub2_pending[pi].sample_offset == cur_off) {
                        sub2_apply_pending_packet(&s_sub2_pending[pi]);
                        pi++;
                    }
                }
                if (render_ok && cursor < ef) {
                    render_ok = sub2_render(&s_sub2, shared->pcm_sub2_melody[slot] + cursor * 2, ef - cursor, slot, req_epoch);
                }
            }
            s_sub2_pending_cnt = 0;
            SUB_EPOCH_TIME_UPDATE(shared, ASMP_CORE_SUB2_LEAD, t0_ns);

            /* P0-B: commit前にslot世代を検証、late writerは破棄
             * (注意: 既に次世代に再割り当てされている可能性があるため、スロットのゼロクリアは行わない) */
            if (!render_ok || !asmp_slot_validate(shared, slot, req_epoch)) {
                shared->diag_slot_rejected++;
                shared->done_epoch[ASMP_CORE_SUB2_LEAD].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB2_LEAD].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                continue;
            }
            /* PCM キャッシュクリーン & Release バリア (done_epoch 公開前にペイロード確定) 32B丸めで隣接slot false sharing防止 */
            asmp_dcache_clean(shared->pcm_sub2_melody[slot],
                              ((size_t)ef * sizeof(float) * 2 + 31u) & ~31u);
            ASMP_BARRIER();
            /* owner_maskを更新 (P0-B) */
            #ifdef PROFILE_ENABLE
    profile_epoch_end(2, req_epoch);
#endif
            asmp_slot_commit(shared, slot, req_epoch, ASMP_SLOT_WORKER_BIT_SUB2);
            /* 3. 完了エポックを公開 (単調増加のためクリア競合が存在しない) */
            shared->done_epoch[ASMP_CORE_SUB2_LEAD].val = req_epoch;
            ASMP_BARRIER();
            shared->core[ASMP_CORE_SUB2_LEAD].heartbeat++;
            idle_loops = 0;
            SUB_LOAD_BUSY_END(load_m);
        } else {
            /* RENDER_REQ が来ていない待機中でも、溜まったMIDIイベントがあれば即座に適用
             * (曲停止時の消音や、フレーム間に届いた Note Off / CC を絶対に捨てない) */
            if (s_sub2_pending_cnt > 0) {
                for (uint32_t i = 0; i < s_sub2_pending_cnt; i++) {
                    sub2_apply_pending_packet(&s_sub2_pending[i]);
                }
                s_sub2_pending_cnt = 0;
            }
            /* 待機中も低レートでハートビートを申告 (生存監視用) */
            if ((++idle_loops & 0x3Fu) == 0u) {
                shared->core[ASMP_CORE_SUB2_LEAD].heartbeat++;
            }
            sub2_sleep_us(100);
        }

        SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB2_LEAD);
    }

    return NULL;
}
