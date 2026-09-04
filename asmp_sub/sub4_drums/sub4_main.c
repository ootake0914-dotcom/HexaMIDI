/**
 * @file sub4_main.c
 * @brief SubCore 4: GM Standard ドラムキット音源
 * @details Kick, Snare, Hi-Hat, Cymbal, Toms, Clap 等のアナログ＆FMモデリング・パーカッション合成 (極限最適化版)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
/* Sleep(0) は即再スケジュールのビジーループになるため最低 1ms は休止 */
static void sub4_sleep_us_impl(unsigned int us)
{
    DWORD ms = us / 1000u;
    Sleep((ms > 0) ? ms : 1);
}
#define sub4_sleep_us(us) sub4_sleep_us_impl((unsigned int)(us))
#elif defined(__NuttX__)
#include <nuttx/arch.h>
#define sub4_sleep_us(us) up_udelay((useconds_t)(us))
#else
#include <unistd.h>
#define sub4_sleep_us(us) usleep(us)
#endif

#include "sub_common.h"
#include "rt_profile.h"
#include "sub_asm.h"

#define SUB4_MAX_VOICES (8)
#define SUB4_SNARE_NONE (0xFFFFFFFFu)  /* 「直前ヒットなし」センチネル */
#define SUB4_TILE_FRAMES (32u) /* 共通金属キャリア tile (Commit2) */

/* キック チューニング定数 ([SUB4][BUILD] デバッグログと値源を共有)
 * 低域の芯 (48Hz 着地) + 高速スイープ exp(-11) でタイトなパンチ */
#define SUB4_KICK_START_HARD   (130.0f)   /* velocity >= 0.85 */
#define SUB4_KICK_START_NORMAL (120.0f)   /* velocity >= 0.50 */
#define SUB4_KICK_START_GHOST  (105.0f)   /* それ以下 */
#define SUB4_KICK_TARGET       (48.0f)
#define SUB4_KICK_SWEEP_EXP    (-11.0f)
#define SUB4_KICK_CLICK_LEN    (64u)      /* ベータークリック長 (~1.3ms: hot master 下でも耳に痛くない短さ) */

/* シンバル / ハット 6系統金属音の Q32 位相増分定数テーブル (TR-808 非整数比 6発振器準拠)
 * 263Hz, 400Hz, 421Hz, 474Hz, 587Hz, 845Hz @ 48kHz */
static const uint32_t s_metal_inc[6] = {
    (uint32_t)(263.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(400.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(421.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(474.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(587.0f / 48000.0f * 4294967296.0f + 0.5f),
    (uint32_t)(845.0f / 48000.0f * 4294967296.0f + 0.5f)
};

/**
 * @brief Q32 位相の金属音パーシャルを PolyBLEP 帯域制限矩形波として評価
 *        (ph はサンプル評価「前」の位相を渡すこと。加算はループ側で行う)
 */
static inline float sub4_metal_partial(uint32_t ph, uint32_t inc)
{
    return sub_osc_square_q32(ph, inc);
}

/* Drum ECO 時に PolyBLEP を省略した簡易矩形波 (符号ビットのみ) */
static inline float sub4_metal_partial_eco(uint32_t ph)
{
    return (ph & 0x80000000u) ? 1.0f : -1.0f;
}

typedef enum {
    DRUM_TYPE_KICK = 0,
    DRUM_TYPE_SNARE,
    DRUM_TYPE_HIHAT,
    DRUM_TYPE_CYMBAL,
    DRUM_TYPE_TOM,
    DRUM_TYPE_CLAP
} DrumType;

typedef struct {
    bool     active;
    uint8_t  note;
    float    velocity;
    DrumType type;
    float    phase;
    float    phase_increment;
    float    frequency;
    float    start_frequency;
    float    target_frequency;

    float    env_level;
    float    decay_coeff;
    uint32_t samples_rendered;
    uint32_t total_samples;
    float    prog;               /**< 進行率 [0.0, 1.0) (加算更新) */
    float    prog_inc;           /**< 進行率増分 (1サイクル加算用) */
    float    pitch_env;          /**< Kick/Tom ピッチスイープ包絡 (1.0->0.0 の乗算型指数減衰) */
    float    pitch_decay_coeff;  /**< note-on 時に1回だけ算出 */
    float    shim_lp;            /**< シンバル/クラップ BPF 用 one-pole LP 状態 */
    uint32_t tap_pos;             /**< Clap マルチタップ位相 (剰余カウンタ。%384 置換) */
    uint32_t metal_ph[6];        /**< シンバル/ハット 6系統金属音 Q32 位相 */
    bool     buzz;               /**< スネア ロール/ゴースト判定 */
} DrumVoice;

typedef struct {
    AsmpSharedContext *shared;
    DrumVoice voices[SUB4_MAX_VOICES];
    uint32_t  noise_seed;         /**< Xorshift32 高速乱数シード */
    uint32_t  metal_free_ph[6];   /**< フリーランニング金属音位相 (発音毎リセットしない) */
    float     volume;
    uint32_t  rendered_total;     /**< 累計レンダリングサンプル (ロール判定用) */
    uint32_t  last_snare_at;      /**< 直前スネア トリガ位置 */
} Sub4DrumEngine;

static Sub4DrumEngine s_sub4;

/* 256->512 (+3KB)。密集譜面のNOTEロスト(特に消音系の欠落→鳴りっぱなし)を防止 */
#define SUB4_MAX_PENDING 512
static AsmpPacket s_sub4_pending[SUB4_MAX_PENDING];
static uint32_t s_sub4_pending_cnt = 0;

/* Commit2: 共通金属キャリア tile ビルダ (6発振器をコア共有, 24k -> 3k polyblep) */
static inline void sub4_build_metal_tile(Sub4DrumEngine *eng, float *sum6, float *sum3, uint32_t n)
{
    uint32_t p0 = eng->metal_free_ph[0];
    uint32_t p1 = eng->metal_free_ph[1];
    uint32_t p2 = eng->metal_free_ph[2];
    uint32_t p3 = eng->metal_free_ph[3];
    uint32_t p4 = eng->metal_free_ph[4];
    uint32_t p5 = eng->metal_free_ph[5];
    const uint32_t i0 = s_metal_inc[0], i1 = s_metal_inc[1], i2 = s_metal_inc[2];
    const uint32_t i3 = s_metal_inc[3], i4 = s_metal_inc[4], i5 = s_metal_inc[5];
    for (uint32_t i = 0; i < n; i++) {
        float o0 = sub4_metal_partial(p0, i0);
        float o1 = sub4_metal_partial(p1, i1);
        float o2 = sub4_metal_partial(p2, i2);
        float o3 = sub4_metal_partial(p3, i3);
        float o4 = sub4_metal_partial(p4, i4);
        float o5 = sub4_metal_partial(p5, i5);
        sum6[i] = o0 + o1 + o2 + o3 + o4 + o5;
        sum3[i] = o0 + o2 + o4;
        p0 += i0; p1 += i1; p2 += i2; p3 += i3; p4 += i4; p5 += i5;
    }
    eng->metal_free_ph[0] = p0; eng->metal_free_ph[1] = p1; eng->metal_free_ph[2] = p2;
    eng->metal_free_ph[3] = p3; eng->metal_free_ph[4] = p4; eng->metal_free_ph[5] = p5;
}

/**
 * @brief Xorshift32 の仮数部ビットパックによる超高速一様乱数 [-1.0, 1.0)
 * @details IEEE 754 浮動小数点のビット構造を活用し、VCVT 型変換命令を完全排除
 */
static inline float sub4_noise_fast(uint32_t *seed)
{
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    union { uint32_t u; float f; } c;
    c.u = 0x3F800000u | (x >> 9); /* [1.0, 2.0) */
    return (c.f - 1.5f) * 2.0f;   /* [-1.0, 1.0) */
}

/**
 * @brief ドラムエンジン初期化
 */
static void sub4_engine_init(Sub4DrumEngine *eng, AsmpSharedContext *shared)
{
    memset(eng, 0, sizeof(Sub4DrumEngine));
    eng->shared = shared;
    eng->noise_seed = 0x9ABCDEF1u;
    /* 金属音フリーランニング位相の初期種 (互いに離散した値) */
    eng->metal_free_ph[0] = 0x2A000000u;
    eng->metal_free_ph[1] = 0x7C000000u;
    eng->metal_free_ph[2] = 0xB1000000u;
    eng->metal_free_ph[3] = 0xE6000000u;
    eng->metal_free_ph[4] = 0x19000000u;
    eng->metal_free_ph[5] = 0x5F000000u;
    eng->volume = 0.90f;
    eng->last_snare_at = SUB4_SNARE_NONE; /* 「直前ヒットなし」のセンチネル */
    /* g_sine_lut const, no init */
    sub_freq_lut_init();
    sub_exp_lut_init();
}

/**
 * @brief ドラムノートオン
 *        Drum ECO 時は Hi-Hat(42/44/46)の同一ノートを再利用して重複ボイスを防ぐ (優先2)
 *        + 動的に 8->4 へ上限を絞る
 */
static void sub4_note_on(Sub4DrumEngine *eng, uint8_t note, float velocity)
{
    if (velocity <= 0.0f) return;

    bool drum_eco = eng->shared && (eng->shared->main_ctrl.quality_flags & ASMP_QF_DRUM_ECO);
    /* 優先2: Hi-Hat 同一ノート再利用 (eco時のみ) — Open/Closedのチョークを自然に再現 */
    if (drum_eco && (note == 42 || note == 44 || note == 46)) {
        for (int i = 0; i < SUB4_MAX_VOICES; i++) {
            if (eng->voices[i].active && eng->voices[i].note == note) {
                /* 既存Hi-Hatを即座に再トリガ — 位相・envをリセットして再利用 */
                eng->voices[i].active = false;
                eng->voices[i].env_level = 0.0f;
                break;
            }
        }
    }

    int voice_idx = -1;
    for (int i = 0; i < SUB4_MAX_VOICES; i++) {
        if (!eng->voices[i].active) {
            voice_idx = i;
            break;
        }
    }
    if (voice_idx == -1) {
        float min_env = 999.0f;
        for (int i = 0; i < SUB4_MAX_VOICES; i++) {
            if (eng->voices[i].env_level < min_env) {
                min_env = eng->voices[i].env_level;
                voice_idx = i;
            }
        }
    }
    /* 優先1/2: Drum ECO 時は上限4を超えたらここで即時フェード間引きはせず
     * render入口で128sampフェードを行うため、ここでは従来通りstealのみ */
    (void)drum_eco;

    DrumVoice *v = &eng->voices[voice_idx];
    v->active = true;
    v->note = note;
    v->velocity = velocity;
    v->phase = 0.0f;
    v->samples_rendered = 0;
    v->env_level = 1.0f;
    v->shim_lp = 0.0f;
    v->tap_pos = 0;
    v->buzz = false;

    float decay_sec = 0.15f;

    /* Kick: 初期位相 0.0 (ゼロクロス) から立ち上がり、打頭のアタック・パンチを最大化 */
    if (note == 35 || note == 36) {
        v->phase = 0.0f;
    }

    if (note == 35 || note == 36) {
        /* Bass Drum / Kick: ベロシティ 3 レイヤー (ハード/ノーマル/ゴースト)
         * 初期ピッチを下げ (120Hz 系) + 目標 48Hz + 高速スイープで
         * 「低域の芯がある・輪郭のタイトな」パンチに調整 */
        v->type = DRUM_TYPE_KICK;
        if (velocity >= 0.85f) {          /* ハード: 明るく長い */
            v->start_frequency = SUB4_KICK_START_HARD; decay_sec = 0.150f;
        } else if (velocity >= 0.50f) {   /* ノーマル */
            v->start_frequency = SUB4_KICK_START_NORMAL; decay_sec = 0.130f;
        } else {                          /* ゴーストノート: 短く抑える */
            v->start_frequency = SUB4_KICK_START_GHOST; decay_sec = 0.095f;
        }
        v->target_frequency = SUB4_KICK_TARGET;
        v->frequency = v->start_frequency;
    } else if (note == 38 || note == 40) {
        /* Snare Drum: FM ボディ + ノイズ。ロール/ゴースト判定付き */
        v->type = DRUM_TYPE_SNARE;
        v->start_frequency = 180.0f;
        v->target_frequency = 180.0f;
        v->frequency = 180.0f;
        decay_sec = (velocity < 0.40f) ? 0.085f : ((velocity >= 0.85f) ? 0.135f : 0.120f);

        /* 60ms 以内の再トリガはロール/ゴースト扱い。
         * last_snare_at は初期化時 SUB4_SNARE_NONE (センチネル)。
         * センチネルとの減算は mod 2^32 でラップし、起動直後 (~60ms 以内)
         * の初回ヒットが誤ってロール扱いになるため、センチネルは明示比較する */
        v->buzz = (eng->last_snare_at != SUB4_SNARE_NONE) &&
                  ((eng->rendered_total - eng->last_snare_at) < 2880u);
        eng->last_snare_at = eng->rendered_total;
    } else if (note == 39) {
        /* Hand Clap: 808 風マルチインパルス + バンドパスノイズ */
        v->type = DRUM_TYPE_CLAP;
        v->start_frequency = 0.0f;
        v->target_frequency = 0.0f;
        v->frequency = 0.0f;
        decay_sec = 0.180f;
    } else if (note == 42 || note == 44) {
        /* Closed / Pedal Hi-Hat: 6系統金属音 + 高域ノイズ (超短時間ディケイ) */
        v->type = DRUM_TYPE_HIHAT;
        v->start_frequency = 0.0f;
        v->target_frequency = 0.0f;
        v->frequency = 0.0f;
        decay_sec = 0.035f;
    } else if (note == 46) {
        /* Open Hi-Hat: 6系統金属音 + 高域ノイズ (220ms ディケイ) */
        v->type = DRUM_TYPE_HIHAT;
        v->start_frequency = 0.0f;
        v->target_frequency = 0.0f;
        v->frequency = 0.0f;
        decay_sec = 0.220f;
    } else if (note >= 41 && note <= 50 && note != 49) {
        /* Toms (Low/Mid/High): トーン急降下サイン波 */
        v->type = DRUM_TYPE_TOM;
        float tom_f = 90.0f + (float)(note - 41) * 15.0f;
        v->start_frequency = tom_f * 1.5f;
        v->target_frequency = tom_f;
        v->frequency = v->start_frequency;
        decay_sec = 0.160f;
    } else {
        /* Crash Cymbal (49, 57) / Ride / Other: 6系統金属音 + 倍音リング + BPF シマー */
        v->type = DRUM_TYPE_CYMBAL;
        v->start_frequency = 0.0f;
        v->target_frequency = 0.0f;
        v->frequency = 0.0f;
        decay_sec = 0.650f;
    }

    uint32_t decay_samples = (uint32_t)(decay_sec * (float)SUB_SAMPLE_RATE);
    v->total_samples = decay_samples;
    v->decay_coeff = (decay_samples > 0) ? sub_exp_approx(6.907755f / (float)decay_samples) : 0.0f;
    v->phase_increment = v->frequency / (float)SUB_SAMPLE_RATE;

    /* 進捗率の事前計算 (レンダリングループ内の 14 サイクル除算 vdiv を完全排除) */
    v->prog = 0.0f;
    v->prog_inc = (decay_samples > 0) ? (1.0f / (float)decay_samples) : 1.0f;

    /* Kick/Tom ピッチスイープ包絡の初期化 (note-on 時に1回だけ算出) */
    v->pitch_env = 1.0f;
    v->pitch_decay_coeff = (decay_samples > 0) ? sub_exp_approx(-SUB4_KICK_SWEEP_EXP / (float)decay_samples) : 0.0f;

    /* 6系統金属音 Q32 位相: フリーランニング値を引き継ぐ
     * (発音毎の固定リセットは位相打ち消しで音痩せするため廃止) */
    for (int k = 0; k < 6; k++) {
        v->metal_ph[k] = eng->metal_free_ph[k];
    }
}

/**
 * @brief 全ドラム消音
 */
static void sub4_all_notes_off(Sub4DrumEngine *eng)
{
    for (int i = 0; i < SUB4_MAX_VOICES; i++) {
        eng->voices[i].active = false;
        eng->voices[i].env_level = 0.0f;
    }
}

static void sub4_apply_pending_packet(const AsmpPacket *pkt)
{
    switch (pkt->msg_type) {
        case ASMP_MSG_NOTE_ON:
            sub4_note_on(&s_sub4, pkt->data1, (float)pkt->data2 / 127.0f);
            break;
        case ASMP_MSG_ALL_NOTES_OFF:
            if (pkt->channel == 9 || pkt->channel >= 16 || pkt->data1 == 0xFF) {
                sub4_all_notes_off(&s_sub4);
            }
            break;
        case ASMP_MSG_CONTROL_CHANGE:
            if (pkt->data1 == 7) {
                s_sub4.volume = (float)pkt->data2 / 127.0f;
            } else if (pkt->data1 == 91) {
                /* Ch9 CC91は無視 (ドラムにreverb_sendなし) */
            }
            break;
        default: break;
    }
}

/* ========================================================================= */
/* ドラム種別ごとの専用レンダリング関数 (スタックスピル根絶・レジスタ直結)    */
/* ========================================================================= */

/**
 * @brief キック専用レンダリング (高速ピッチスイープ + 連続ソフトサチュレーション)
 */
static void sub4_render_kick(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t frames, float gain)
{
    float pitch_env = v->pitch_env;
    float pitch_decay = v->pitch_decay_coeff;
    float phase = v->phase;
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    float start_f = v->start_frequency;
    float target_f = v->target_frequency;
    /* shim_lp はベータークリックのノイズ平滑化に転用 (note-on 時に 0 リセット済み) */
    float click_lp = v->shim_lp;

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
        float d = s * 1.25f;
        if (d > 0.95f)       d = 0.95f;
        else if (d < -0.95f) d = -0.95f;

        /* ベータークリック: 発音頭 ~2.0ms の HP ノイズバーストで
         * 強烈なビーター アタック感を付与 (減衰しつつ自然に消える) */
        if (samples_rendered <= SUB4_KICK_CLICK_LEN) {
            float cn = sub4_noise_fast(&eng->noise_seed);
            click_lp += 0.60f * (cn - click_lp);
            d += (cn - click_lp) * 0.65f *
                 (1.0f - (float)samples_rendered * (1.0f / (float)SUB4_KICK_CLICK_LEN));
        }

        acc[f] += d * env_level * gain;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    v->pitch_env = pitch_env;
    v->phase = phase;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
    v->shim_lp = click_lp;
}

/**
 * @brief スネア専用レンダリング (FM ボディ + 1サイクル加算進捗率ノイズ)
 */
static void sub4_render_snare(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t frames, float gain)
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
    uint32_t *noise_seed = &eng->noise_seed;
    /* shim_lp をスネアノイズの HP 状態に転用 (note-on 時 0 リセット済み) */
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

        float noise_gain = buzz ? 0.95f : (0.70f + 0.35f * (1.0f - prog));
        prog += prog_inc;

        /* ノイズを ~1.7kHz HP で締め「パシッ」とするスナップ感を付与
         * (生ノイズのままだと低域がゴロつき、ボディと混濁する) */
        float noise_raw = sub4_noise_fast(noise_seed) * noise_gain;
        snare_hp += 0.22f * (noise_raw - snare_hp);
        float noise = (noise_raw - snare_hp) * 1.35f;
        float out = tone + fm_tone + noise;

        acc[f] += out * env_level * gain;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    v->phase = phase;
    v->prog = prog;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
    v->shim_lp = snare_hp;
}

/* Tile版: 共通金属キャリアを使用 (Commit2) */
static void sub4_render_hihat_tile(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t n, const float *sum6, const float *sum3, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    uint32_t *noise_seed = &eng->noise_seed;
    float hat_lp = v->shim_lp;
    bool drum_eco = eng->shared && (eng->shared->main_ctrl.quality_flags & ASMP_QF_DRUM_ECO);
    for (uint32_t f = 0; f < n; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            /* 残りサンプルは無音のまま break */
            /* 呼び出し側は samples_rendered/envを保存するため break前に状態反映 */
            break;
        }
        float metal = drum_eco ? sum3[f] * 0.20f : sum6[f] * 0.12f;
        float noise = sub4_noise_fast(noise_seed) * 0.50f;
        float raw = metal + noise;
        hat_lp += 0.32f * (raw - hat_lp);
        float out = raw - hat_lp;
        acc[f] += out * env_level * gain;
    }
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
    v->shim_lp = hat_lp;
}

static void sub4_render_cymbal_tile(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t n, const float *sum6, const float *sum3, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    float shim_lp = v->shim_lp;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    uint32_t *noise_seed = &eng->noise_seed;
    bool drum_eco = eng->shared && (eng->shared->main_ctrl.quality_flags & ASMP_QF_DRUM_ECO);
    /* 注: 旧 s1/s2 (phase*3.7/7.3 サイン) は削除。Cymbal は frequency=0 のため
     * phase が常時 0 で両項は恒等的に +0.0 (LUT[0]=0)。毎サンプルの Hermite
     * 2 回分 (~30flop) を削減し、将来周波数を持たせた際の不連続バグも根絶 */
    for (uint32_t f = 0; f < n; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }
        float metal = drum_eco ? sum3[f] * 0.13f : sum6[f] * 0.08f;
        float n1 = sub4_noise_fast(noise_seed);
        float raw = n1 * 0.40f + metal;
        shim_lp += 0.55f * (raw - shim_lp);
        float hp = raw - shim_lp;
        float out = n1 * 0.35f + hp * 0.70f;
        acc[f] += out * env_level * gain;
    }
    v->shim_lp = shim_lp;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

 /**
  * @brief タム専用レンダリング (急降下サイン波)
  */
static void sub4_render_tom(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t frames, float gain)
{
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

        acc[f] += d * env_level * gain;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    v->pitch_env = pitch_env;
    v->phase = phase;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

/**
 * @brief ハンドクラップ専用レンダリング (マルチタップ・インパルス + バンドパスノイズ)
 */
static void sub4_render_clap(DrumVoice *v, Sub4DrumEngine *eng, float *acc, uint32_t frames, float gain)
{
    float env_level = v->env_level;
    float decay_coeff = v->decay_coeff;
    float shim_lp = v->shim_lp;
    uint32_t samples_rendered = v->samples_rendered;
    uint32_t total_samples = v->total_samples;
    uint32_t *noise_seed = &eng->noise_seed;
    /* タップ位相は加算カウンタで維持 (%384 は M4F でソフト除算のため排除) */
    uint32_t tap_pos = v->tap_pos;

    for (uint32_t f = 0; f < frames; f++) {
        env_level *= decay_coeff;
        samples_rendered++;
        if (env_level <= 0.001f || samples_rendered >= total_samples) {
            v->active = false;
            env_level = 0.0f;
            break;
        }

        float n = sub4_noise_fast(noise_seed);
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
        acc[f] += out;
    }

    v->tap_pos = tap_pos;
    v->shim_lp = shim_lp;
    v->env_level = env_level;
    v->samples_rendered = samples_rendered;
}

/* ========================================================================= */
/* モノラル 1ch float タイル蓄積 + float バス L/R 一括書き出し                */
/* (最終 16bit 量子化は Sub5 マスター出力段で TPDF ディザ付き 1 回のみ)       */
/* ========================================================================= */

/**
 * @brief ドラム PCM レンダリング
 *        Drum ECO 時は 4voice 超を128sampフェードで間引き
 */
static bool sub4_render(Sub4DrumEngine *eng, float *buffer, uint32_t frames, uint32_t slot, uint32_t req_epoch)
{
    /* 1. モノラル 1ch float アキュムレータをクリア */
    float acc[ASMP_BUFFER_FRAMES];
    memset(acc, 0, frames * sizeof(float));

    uint16_t active_count = 0;

    /* 実時間基準で進める (ボイス毎の加算だと多重発音時に速回りしロール判定が狂う) */
    eng->rendered_total += frames;

    /* 1b. Drum ECO 時の事前カリング: 4voice超は最弱から128sampフェード */
    bool drum_eco = eng->shared && (eng->shared->main_ctrl.quality_flags & ASMP_QF_DRUM_ECO);
    if (drum_eco) {
        int active_tmp = 0;
        for (int i = 0; i < SUB4_MAX_VOICES; i++) if (eng->voices[i].active) active_tmp++;
        while (active_tmp > 4) {
            float min_lvl = 999.0f;
            int min_idx = -1;
            for (int i = 0; i < SUB4_MAX_VOICES; i++) {
                if (eng->voices[i].active && eng->voices[i].env_level < min_lvl) {
                    min_lvl = eng->voices[i].env_level;
                    min_idx = i;
                }
            }
            if (min_idx < 0) break;
            DrumVoice *v = &eng->voices[min_idx];
            /* 128サンプルで -80dBへ減衰する係数: coeff = exp(-6.907755/128) */
            v->decay_coeff = sub_exp_approx(6.907755f / 128.0f);
            v->total_samples = v->samples_rendered + 128u;
            /* 即死ではなくフェードへ — 次の呼出で自然にactive=falseになる */
            active_tmp--;
            /* 同一エポックで再選択されないようenvを一時的に下げる */
            v->env_level *= 0.5f;
        }
    }

    /* 1c. アクティブ数と金属系要否を先に判定 (tile分岐用) */
    bool need_metal = false;
    for (int i = 0; i < SUB4_MAX_VOICES; i++) {
        if (!eng->voices[i].active) continue;
        active_count++;
        if (eng->voices[i].type == DRUM_TYPE_HIHAT || eng->voices[i].type == DRUM_TYPE_CYMBAL) {
            need_metal = true;
        }
    }

    /* 2. 各アクティブボイスをドラム種別ごとにディスパッチ */
    if (need_metal) {
        /* 金属系あり: 非金属は一度に、金属はtile共有で 24k->3k polyblep へ削減 */
        for (int i = 0; i < SUB4_MAX_VOICES; i++) {
            DrumVoice *v = &eng->voices[i];
            if (!v->active) continue;
            if (v->type == DRUM_TYPE_HIHAT || v->type == DRUM_TYPE_CYMBAL) continue;
            float gain = eng->volume * v->velocity * 0.50f;
            switch (v->type) {
                case DRUM_TYPE_KICK: sub4_render_kick(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_SNARE: sub4_render_snare(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_TOM: sub4_render_tom(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_CLAP: sub4_render_clap(v, eng, acc, frames, gain); break;
                default: break;
            }
        }
        /* 金属系は 32fr tile で共通キャリアを生成し共有 */
        for (uint32_t t = 0; t < frames; t += SUB4_TILE_FRAMES) {
            uint32_t n = frames - t;
            if (n > SUB4_TILE_FRAMES) n = SUB4_TILE_FRAMES;
            float sum6[SUB4_TILE_FRAMES];
            float sum3[SUB4_TILE_FRAMES];
            sub4_build_metal_tile(eng, sum6, sum3, n);
            for (int i = 0; i < SUB4_MAX_VOICES; i++) {
                DrumVoice *v = &eng->voices[i];
                if (!v->active) continue;
                float gain = eng->volume * v->velocity * 0.50f;
                if (v->type == DRUM_TYPE_HIHAT) {
                    sub4_render_hihat_tile(v, eng, &acc[t], n, sum6, sum3, gain);
                } else if (v->type == DRUM_TYPE_CYMBAL) {
                    sub4_render_cymbal_tile(v, eng, &acc[t], n, sum6, sum3, gain);
                }
            }
        }
    } else {
        /* 金属系なし: O(1)で位相を進める (512ループ -> 1 mul) */
        for (int k = 0; k < 6; k++) {
            eng->metal_free_ph[k] += s_metal_inc[k] * frames;
        }
        for (int i = 0; i < SUB4_MAX_VOICES; i++) {
            DrumVoice *v = &eng->voices[i];
            if (!v->active) continue;
            float gain = eng->volume * v->velocity * 0.50f;
            switch (v->type) {
                case DRUM_TYPE_KICK: sub4_render_kick(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_SNARE: sub4_render_snare(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_TOM: sub4_render_tom(v, eng, acc, frames, gain); break;
                case DRUM_TYPE_CLAP: sub4_render_clap(v, eng, acc, frames, gain); break;
                default: break;
            }
        }
    }

    /* 3. 最終出力段: ソフトクリッパー (過渡ピークの硬い頭打ち歪みを緩和)
     *    -> 共有バッファへの書き出し直前にスロット所有権を検証 (Early Abort) */
    if (!asmp_slot_validate(eng->shared, slot, req_epoch)) {
        return false;
    }

    for (uint32_t f = 0; f < frames; f++) {
        /* 最適化+音質: 旧 sample/(1+|s|*0.5) は毎サンプル vdiv.f32(14cyc)。
         * Sub5と同一のC1連続ソフトリミッタ(分岐+多項式、除算なし)へ統一。
         * 線形域| x|<=0.85→3次ショルダー→±1天井で値・傾き連続、クリックなし */
        float s = acc[f];
        if (!SUB_ISFINITE_F(s)) s = 0.0f;
        float ax = fabsf(s);
        float y;
        if (ax <= 0.85f) y = s;
        else if (ax >= 1.30f) y = (s > 0.0f) ? 1.0f : -1.0f;
        else {
            const float u = ax - 0.85f;
            const float m = u - u * u * 2.2222222f + u * u * u * 1.6460905f;
            y = (s > 0.0f) ? (0.85f + m) : -(0.85f + m);
        }
        buffer[f * 2 + 0] = y;
        buffer[f * 2 + 1] = y;
    }

    eng->shared->core[ASMP_CORE_SUB4_DRUM].voice_count = active_count;
    /* C4監視の飽和判定用に公開 (busy_us と同一規律で clean) */
    asmp_dcache_clean((const void *)&eng->shared->core[ASMP_CORE_SUB4_DRUM].voice_count,
                      sizeof(eng->shared->core[ASMP_CORE_SUB4_DRUM].voice_count));
    return true;
}

/**
 * @brief SubCore 4 エントリーポイント
 */
void *subcore4_entry(void *arg)
{
    AsmpSharedContext *shared = (AsmpSharedContext *)arg;
    if (!shared) return NULL;
    if (!asmp_abi_ok(shared)) {
        printf("[SUB4][FATAL] ABI mismatch at entry\n");
        return NULL;
    }
    sub_fpu_denormal_init(); /* デノーマル例外ペナルティによるじりじりノイズ防止 */

    sub4_engine_init(&s_sub4, shared);

    /* ビルド識別 + キック チューニング自己診断 (newlib-nano float非対応のため整数化) */
    printf("[SUB4][BUILD] %s | kick %d/%d/%d -> %d Hz sweep exp(%d)\n",
           HEXASENSE_DSP_TAG,
           (int)SUB4_KICK_START_HARD, (int)SUB4_KICK_START_NORMAL, (int)SUB4_KICK_START_GHOST,
           (int)SUB4_KICK_TARGET, (int)SUB4_KICK_SWEEP_EXP);

    /* CPU 負荷メーター (実測ビジー時間比率) */
    SubLoadMeter load_m; SUB_LOAD_INIT(load_m);

    uint32_t idle_loops = 0;
    uint32_t req_epoch = 0;

    while (!shared->main_ctrl.shutdown_requested) {
        AsmpPacket pkt;
        bool has_render_req = false;

        while (asmp_queue_pop(&shared->queues[ASMP_CORE_SUB4_DRUM], &pkt)) {
            if (pkt.msg_type == ASMP_MSG_RENDER_REQ) {
                has_render_req = true;
                req_epoch = pkt.param;
                break;
            }
            /* 緊急消音（ALL_NOTES_OFF / CC120）はRENDER_REQを待たずに即時反映 */
            if (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF ||
                (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 120)) {
                sub4_apply_pending_packet(&pkt);
                continue;
            }
            if (s_sub4_pending_cnt < SUB4_MAX_PENDING) {
                s_sub4_pending[s_sub4_pending_cnt++] = pkt;
            } else {
                bool is_release = (pkt.msg_type == ASMP_MSG_NOTE_OFF) ||
                                  (pkt.msg_type == ASMP_MSG_CONTROL_CHANGE && pkt.data1 == 64 && pkt.data2 < 64) ||
                                  (pkt.msg_type == ASMP_MSG_ALL_NOTES_OFF);
                if (is_release) {
                    /* 溢れても消音は必達: 最低ベロシティNOTE_ONを追い出す。
                     * 退避は diag_queue_drop で可視化 (従来は無計測の欠落) */
                    int rep = -1;
                    uint8_t rep_vel = 0xFFu;
                    for (int i = 0; i < (int)SUB4_MAX_PENDING; i++) {
                        if (s_sub4_pending[i].msg_type == ASMP_MSG_NOTE_ON &&
                            s_sub4_pending[i].data2 <= rep_vel) {
                            rep_vel = s_sub4_pending[i].data2;
                            rep = i;
                        }
                    }
                    if (rep >= 0) s_sub4_pending[rep] = pkt;
                    else s_sub4_pending[SUB4_MAX_PENDING - 1] = pkt;
                    shared->diag_queue_drop++;
                } else {
                    shared->diag_queue_drop++;
                }
            }
        }

        if (has_render_req) {
            static uint32_t s_last_epoch = 0;
            if (s_last_epoch != 0 && req_epoch != s_last_epoch + 1u) {
                shared->diag_epoch_gap[ASMP_CORE_SUB4_DRUM]++;
            }
            s_last_epoch = req_epoch;
            SUB_CHAOS_DELAY(ASMP_CORE_SUB4_DRUM);
            SUB_LOAD_BUSY_BEGIN(load_m);
            uint64_t t0_ns = sub_get_ns();
            uint32_t slot = ASMP_EPOCH_SLOT(req_epoch);
            if (!asmp_abi_ok(shared)) {
                printf("[SUB4][FATAL] ABI mismatch\n");
                return NULL;
            }
            if (shared->render_ctrl.slot_epoch[slot] != req_epoch) {
                shared->diag_slot_mismatch++;
                shared->done_epoch[ASMP_CORE_SUB4_DRUM].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB4_DRUM].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                s_sub4_pending_cnt = 0;
                continue;
            }
            uint32_t ef = shared->render_ctrl.epoch_frames[slot];
#ifdef PROFILE_ENABLE
    profile_epoch_start(4, req_epoch);
#endif
            if (ef == 0u || ef > ASMP_BUFFER_FRAMES) ef = ASMP_BUFFER_FRAMES;

            for (uint32_t i = 1; i < s_sub4_pending_cnt; i++) {
                AsmpPacket key = s_sub4_pending[i];
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && s_sub4_pending[j].sample_offset > key.sample_offset) {
                    s_sub4_pending[j+1] = s_sub4_pending[j];
                    j--;
                }
                s_sub4_pending[j+1] = key;
            }
            bool render_ok = true;
            if (s_sub4_pending_cnt == 0) {
                render_ok = sub4_render(&s_sub4, shared->pcm_sub4_drums[slot], ef, slot, req_epoch);
            } else {
                uint32_t cursor = 0;
                uint32_t pi = 0;
                memset(shared->pcm_sub4_drums[slot], 0, ef * sizeof(float) * 2);
                while (pi < s_sub4_pending_cnt) {
                    uint32_t off = s_sub4_pending[pi].sample_offset;
                    if (off > ef) off = ef;
                    if (off > cursor) {
                        if (!sub4_render(&s_sub4, shared->pcm_sub4_drums[slot] + cursor * 2, off - cursor, slot, req_epoch)) {
                            render_ok = false;
                            break;
                        }
                        cursor = off;
                    }
                    uint32_t cur_off = s_sub4_pending[pi].sample_offset;
                    while (pi < s_sub4_pending_cnt && s_sub4_pending[pi].sample_offset == cur_off) {
                        sub4_apply_pending_packet(&s_sub4_pending[pi]);
                        pi++;
                    }
                }
                if (render_ok && cursor < ef) {
                    render_ok = sub4_render(&s_sub4, shared->pcm_sub4_drums[slot] + cursor * 2, ef - cursor, slot, req_epoch);
                }
            }
            s_sub4_pending_cnt = 0;
            if (!render_ok || !asmp_slot_validate(shared, slot, req_epoch)) {
                shared->diag_slot_rejected++;
                shared->done_epoch[ASMP_CORE_SUB4_DRUM].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB4_DRUM].heartbeat++;
                SUB_LOAD_BUSY_END(load_m);
                continue;
            }
            #ifdef PROFILE_ENABLE
    profile_epoch_end(4, req_epoch);
#endif
            asmp_slot_commit(shared, slot, req_epoch, ASMP_SLOT_WORKER_BIT_SUB4);
            SUB_EPOCH_TIME_UPDATE(shared, ASMP_CORE_SUB4_DRUM, t0_ns);

            /* PCM キャッシュクリーン & Release バリア (done_epoch 公開前にペイロード確定) 32B丸めで隣接slot false sharing防止 */
            asmp_dcache_clean(shared->pcm_sub4_drums[slot],
                              ((size_t)ef * sizeof(float) * 2 + 31u) & ~31u);
            ASMP_BARRIER();

            /* 完了エポックを公開 (単調増加のためクリア競合が存在しない) */
            shared->done_epoch[ASMP_CORE_SUB4_DRUM].val = req_epoch;
            ASMP_BARRIER();
            shared->core[ASMP_CORE_SUB4_DRUM].heartbeat++;
            idle_loops = 0;
            SUB_LOAD_BUSY_END(load_m);
        } else {
            /* RENDER_REQ が来ていない待機中でも、溜まったMIDIイベントがあれば即座に適用
             * (曲停止時の消音や、フレーム間に届いた Note Off / CC を絶対に捨てない) */
            if (s_sub4_pending_cnt > 0) {
                for (uint32_t i = 0; i < s_sub4_pending_cnt; i++) {
                    sub4_apply_pending_packet(&s_sub4_pending[i]);
                }
                s_sub4_pending_cnt = 0;
            }
            /* 待機中も低レートでハートビートを申告 (生存監視用) */
            if ((++idle_loops & 0x3Fu) == 0u) {
                shared->core[ASMP_CORE_SUB4_DRUM].heartbeat++;
            }
            sub4_sleep_us(100);
        }

        SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB4_DRUM);
    }

    return NULL;
}
