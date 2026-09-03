/**
 * @file sub5_main.c
 * @brief SubCore 5: 5バンド EQ & ステレオリバーブ DSP エフェクト
 * @details 全サブ音源 (2〜4) のミキシング、5バンド Biquad EQ、Schroeder/Freeverb ステレオリバーブ、ソフトリミッター
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#define sub5_sleep_us(us) Sleep((DWORD)((us) / 1000))
#elif defined(__NuttX__)
#include <nuttx/arch.h>
#define sub5_sleep_us(us) up_udelay((useconds_t)(us))
#else
#include <unistd.h>
#define sub5_sleep_us(us) usleep(us)
#endif
#include "rt_profile.h"

#define SUB_COMMON_NO_LUT /* Sub5はLUT未使用(Freq/Exp/Tan不要)、BSS 2KB節約 OPTIMIZATION_MEMO #4 */
#include "sub_common.h"

#define NUM_EQ_BANDS       (5)

/* ゲインステージング定数 ([SUB5][BUILD] デバッグログと値源を共有) */
/* master 0.77: C1連続ソフトクリッパー化で旧実装の内蔵メイクアップ (+0.68dB) を
 * 廃止したため、A/B 比較で同等ラウドネスになるよう +0.68dB 分をここで補償。
 * 0.77 + Lookahead Limiter 復帰で、ヘッドルームを確保しつつ豊かなラウドネスを維持 */
#define SUB5_MASTER_VOLUME       (0.77f)
#define SUB5_CHORUS_DRY          (0.90f)
#define SUB5_CHORUS_WET          (0.10f)
#define SUB5_REV_WET             (0.12f)
#define SUB5_REV_DRY             (0.90f)
#define SUB5_DELAY_SEND          (0.18f)
#define SUB5_LIMIT_CEILING       (0.90f)

/* ルックアヘッド ピークリミッターの先読み長 (48 サンプル = 1.0ms)。
 * 利得をトランジェント到達前に下げられるため天井での頭打ち歪みが消える */
#define SUB5_LOOKAHEAD           (48u)

/* リバーブ プリディレイ (30ms @48kHz)。dry/wet を時間分離し
 * リード音の輪郭を残したまま残響だけを空間へ広げる */
#define SUB5_PREDELAY            (1440u)

/* BPM 同期ディレイ反響路のダンピング係数 (one-pole LP, ~3kHz)。
 * 反復エコーが最後まで明るくバズるのを防ぎ自然に溶ける */

/* DSP 演算最適化用コンパイル時定数 (不変式ホイスト) */
#define SUB5_INV_32768           (1.0f / 32768.0f)
#define SUB5_MIX_SCALE           (0.25f)        /* 3 音源サミング正規化 (ヘッドルーム確保) */
#define SUB5_LOOKAHEAD_CAP       (64u)          /* 窓49要素を収める2のべき乗リング容量 */

/* Step9 診断: 1 にすると 128 エポック毎に DSP ステージを順次 OFF し、
 * Main 側 [EPOCH US] の S5 ビジー値差分から各ステージのコストを帰属させる。
 * 診断終了後は必ず 0 に戻す (通常動作では全ステージ有効) */
#ifndef SYNTH_SUB5_ABLATION
#define SYNTH_SUB5_ABLATION 0
#endif
#define SUB5_CHORUS_PHASE_INC    (0.8f / (float)SUB_SAMPLE_RATE)
#define SUB5_FDN_PHASE_INC       (0.35f / (float)SUB_SAMPLE_RATE)
#define SUB5_DELAY_FB_DAMP       (0.40f)

/* ソース (Sub2/3/4) 完了待ちの上限。旧50ms(5エポック)はMainの
 * done待ち2秒より前にSub5が50ms空転し、パイプライン全体の遅延として
 * 蓄積してラグ化していた。実機(NuttX)では12ms(≒1エポック+余裕)に短縮し、
 * タイムアウト後は部分スロットでも即ミックスして done を公開する。
 * ホストシミュレーションはOSスケジューリングで本来より遅いため50msを維持 */
#ifdef __NuttX__
#define SUB5_SRC_WAIT_TIMEOUT_MS (10u)
#else
#define SUB5_SRC_WAIT_TIMEOUT_MS (50u)
#endif

/**
 * @brief Cortex-M4F FPU 設定: Flush-to-Zero (FZ) および Default NaN (DN) 有効化
 * @details リバーブやディレイの減衰末尾でのデノーマル数 (非正規化数) による CPU スパイクを恒久的に防止する
 */
static inline void sub5_fpu_init(void)
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

static uint32_t sub5_now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
#endif
}

/* ---------------------------------------------------------------------------
 * FDN (Feedback Delay Network) リバーブ — Householder 行列 + 各線ダンピング
 * 6 本のディレイ線を非整合長で配置し、Modulation により密度を確保。
 * 旧 Comb+Allpass (Freeverb 系) より透明感・拡散性が高い。
 * ------------------------------------------------------------------------- */
#define FDN_LINES           (6)
#define FDN_BUF_MAX         (3000) /* 旧最大値 (互換用, 実体は per-line) */
#define FDN_LEN_0           (1439)
#define FDN_LEN_1           (1693)
#define FDN_LEN_2           (1931)
#define FDN_LEN_3           (2179)
#define FDN_LEN_4           (2411)
#define FDN_LEN_5           (2741)

/* 遅延長は互いに素な素数 (旧の偶数揃え値は共通約数でリンギングを生んだ) */
#define SUB5_DELAY_MAX 4800u /* 150ms->100ms 9.6KB節約 OPTIMIZATION_MEMO #3; 120BPM以上で十分 */

static const uint16_t fdn_len_tbl[FDN_LINES] = { FDN_LEN_0, FDN_LEN_1, FDN_LEN_2, FDN_LEN_3, FDN_LEN_4, FDN_LEN_5 };
/* per-line確保で 72KB->~48.4KB (21.9KB節約, OPTIMIZATION_MEMO #1) */
static float fdn_buf0[FDN_LEN_0];
static float fdn_buf1[FDN_LEN_1];
static float fdn_buf2[FDN_LEN_2];
static float fdn_buf3[FDN_LEN_3];
static float fdn_buf4[FDN_LEN_4];
static float fdn_buf5[FDN_LEN_5];
static float * const fdn_buf[FDN_LINES] = { fdn_buf0, fdn_buf1, fdn_buf2, fdn_buf3, fdn_buf4, fdn_buf5 };
static uint32_t     fdn_pos[FDN_LINES];
static float        fdn_lp[FDN_LINES];      /* 各線ダンプ用 one-pole */
static float        fdn_mod_phase;          /* 密度用 LFO */

/* リバーブ構造体 */
typedef struct {
    bool  enabled;
    float room_size;
    float damping;
    float wet_level;
    float dry_level;
} Sub5Reverb;

/* NOTE: 旧マルチバンドコンプ (LR4 クロスオーバー x8 + RMS エンベロープ) は
 * 演算結果を in_l/r へ書き戻しても最終出力 out_l/r が既に確定済みのため
 * 完全なデッドコードだった (毎サンプル biquad 16 回 + 包絡演算が無駄)。
 * 実測エポック 10060us の大半をこれが占めていたため廃止。
 * ダイナミクス制御は下記のピークリミッター + ソフトリミッタが担う。 */

/* 5バンド EQ */
typedef struct {
    bool         enabled;
    BiquadFilter filters_l[NUM_EQ_BANDS];
    BiquadFilter filters_r[NUM_EQ_BANDS];
} Sub5Eq;

/* DSP エンジン全体状態 */
typedef struct {
    AsmpSharedContext *shared;
    Sub5Eq     eq;
    Sub5Reverb reverb;
    float master_volume;

    /* Ping-Pong: 今処理中のエポックスロット (ソース読み出し/マスタ書き込み先) */
    uint32_t src_slot;

    /* LFO 用正弦波 LUT (毎サンプルの sinf() を排除) */
    /* マスタリング: DC ブロッカー & ルックアヘード ピークリミッター */
    SubDcBlocker dc_l, dc_r;
    float lim_gain;                              /**< リミッター現在利得 (1.0..0) */
    struct {
        float l, r, peak;
    } la_ring[SUB5_LOOKAHEAD_CAP];               /**< 先読み遅延線 + ピーク (インターリーブ構造体配列: 同一キャッシュライン局所化) */
    uint8_t la_pos;                              /**< 遅延線書き込み位置 [0..63] */
    uint8_t dq[SUB5_LOOKAHEAD_CAP];              /**< 単調減少デック インデックス列 [0..63] */
    uint8_t dq_head;                             /**< デック先頭 (最大値) 位置 */
    uint8_t dq_len;                              /**< デック要素数 [0..63] */

    /* 最終出力 TPDF ディザ用 PRNG */
    uint32_t dither_rng;

    /* ステレオコーラス (LFO 変調ディレイ 2 系統)。
     * 低域分離: wet 経路のみ HP (~180Hz) を通しベースの輪郭を守る (#8) */
    BiquadFilter chorus_hp_l;
    BiquadFilter chorus_hp_r;
    float chorus_l[1024];
    float chorus_r[1024];
    uint32_t chorus_pos_l;    /* L/R 独立の書き込み位置 (共有すると R 側が1サンプルずれる) */
    uint32_t chorus_pos_r;
    float chorus_phase;       /* LFO 位相 0..1 */

    /* リバーブ プリディレイ (モノラル, wet 経路のみ) */
    float pred_buf[SUB5_PREDELAY];
    uint32_t pred_pos;

    /* BPM 同期ディレイ (モノライン + 幅広 R タップ) */
    float delay_line[SUB5_DELAY_MAX];   /* 最大 ~100ms @48kHz (旧7200=150msから4800へ節約) */
    uint32_t delay_pos;
    float delay_fb_lp;        /**< 反響路ダンピング one-pole LP 状態 */
    uint32_t cached_delay_len; /**< ブロック先頭で更新される遅延長 (サンプル) */
    /* FDN変調キャッシュ (8サンプル間引きで3LUT/sample->0.375LUT/sample) */
    uint8_t  fdn_mod_div;
    /* 読出しオフセット = 48 + (int)(mod*24)。mod が8区間定数のため同一境界で
     * 確定し、毎サンプル側は書込み位置との加算+上限判定のみにする
     * (float 乗算・VCVT・下限分岐を排除。旧逐次計算と bit 一致) */
    int32_t  fdn_rd_base[FDN_LINES];
    /* リバーブ入力HPF（低音濁り対策: ~150Hz以下を wet から除去） */
    float rev_hp_lp;
} Sub5DspEngine;

static Sub5DspEngine s_sub5;


/**
 * @brief 5バンド EQ 初期化
 */
static void init_eq(Sub5Eq *eq)
{
    eq->enabled = true;

    /* 5バンド定義:
     * Band 0: 100 Hz Low Shelf  (+2.0 dB, Q=0.707) - 低域の厚みとパンチ
     * Band 1: 400 Hz Peaking    (-1.5 dB, Q=1.000) - 中低域の濁り解消・クリアネス
     * Band 2: 1.0 kHz Peaking   (+1.0 dB, Q=1.200) - メロディの抜けと存在感
     * Band 3: 3.2 kHz Peaking   (+1.5 dB, Q=1.400) - アタック・ドラムスナップ感
     * Band 4: 8.0 kHz High Shelf ( 0.0 dB, Q=0.707) - 高域フラット
     */
    float fs = (float)SUB_SAMPLE_RATE;

    /* マスタリング EQ: Kick の芯 (60Hz) を強調し、200Hz の濁りをカット。
     * 低域シェルフの広がりがマスキングを起こすためピーキングへ変更 */
    biquad_calc_coeffs(&eq->filters_l[0], BIQUAD_PEAKING, 60.0f, +2.0f, 1.500f, fs);
    biquad_calc_coeffs(&eq->filters_r[0], BIQUAD_PEAKING, 60.0f, +2.0f, 1.500f, fs);

    biquad_calc_coeffs(&eq->filters_l[1], BIQUAD_PEAKING, 200.0f, -2.0f, 1.000f, fs);
    biquad_calc_coeffs(&eq->filters_r[1], BIQUAD_PEAKING, 200.0f, -2.0f, 1.000f, fs);

    biquad_calc_coeffs(&eq->filters_l[2], BIQUAD_PEAKING, 1000.0f, +1.0f, 1.200f, fs);
    biquad_calc_coeffs(&eq->filters_r[2], BIQUAD_PEAKING, 1000.0f, +1.0f, 1.200f, fs);

    biquad_calc_coeffs(&eq->filters_l[3], BIQUAD_PEAKING, 3200.0f, +1.5f, 1.400f, fs);
    biquad_calc_coeffs(&eq->filters_r[3], BIQUAD_PEAKING, 3200.0f, +1.5f, 1.400f, fs);

    biquad_calc_coeffs(&eq->filters_l[4], BIQUAD_HIGHSHELF, 8000.0f, 0.0f, 0.707f, fs);
    biquad_calc_coeffs(&eq->filters_r[4], BIQUAD_HIGHSHELF, 8000.0f, 0.0f, 0.707f, fs);
}

/**
 * @brief リバーブ初期化 (FDN 状態クリア)
 */
static void init_reverb(Sub5Reverb *rev)
{
    rev->enabled = true;
    rev->room_size = 0.75f;
    rev->damping = 0.35f;
    /* ゲインステージング: wet を下げて後段リミッターへの過大入力を防ぐ
     * (旧 0.28 は FDN タップ合計が尖りやすく、常時リミットで音が潰れていた) */
    rev->wet_level = SUB5_REV_WET;
    rev->dry_level = SUB5_REV_DRY;

    for (int i = 0; i < FDN_LINES; i++) {
        fdn_pos[i] = 0;
        fdn_lp[i] = 0.0f;
        memset(fdn_buf[i], 0, (size_t)fdn_len_tbl[i] * sizeof(float));
    }
    fdn_mod_phase = 0.0f;
}

/**
 * @brief SubCore 5 DSP エンジン初期化
 */
static void sub5_engine_init(Sub5DspEngine *eng, AsmpSharedContext *shared)
{
    sub5_fpu_init();
    memset(eng, 0, sizeof(Sub5DspEngine));
    eng->shared = shared;
    /* マスター出力の基準レベル。後段ピークリミッター (ceiling 0.90) と
     * ソフトリミッタのヘッドルームを活かす値に整理 */
    eng->master_volume = SUB5_MASTER_VOLUME;
    eng->lim_gain = 1.0f;
    eng->la_pos = 0;
    eng->dq_head = 0;
    eng->dq_len = 0;
    eng->fdn_mod_div = 0;
    eng->rev_hp_lp = 0.0f;
    /* ディザ PRNG: ゼロ列を避けるため非ゼロ種 */
    eng->dither_rng = 0x51ED2701u;
    sub_dc_reset(&eng->dc_l, 0.995f);
    sub_dc_reset(&eng->dc_r, 0.995f);
    /* g_sine_lut const, no init */
    init_eq(&eng->eq);
    init_reverb(&eng->reverb);
    /* 低域分離コーラス用 HP (wet 経路のみ ~180Hz 以上を変調)
     * biquad 5係数(10FMA) → one-pole 1係数(2FMA)に軽量化: 1024 biquad/epoch → 0.12ms削減
     * fc180HzのHPは one-poleで十分、聴感差 -80dB以下。状態は s1 に保持(SRAM増0) */
    eng->chorus_hp_l.s1 = 0.0f; eng->chorus_hp_l.s2 = 0.0f;
    eng->chorus_hp_r.s1 = 0.0f; eng->chorus_hp_r.s2 = 0.0f;
}

/* ---- コーラス: 変調ディレイライン (1024 サンプル環状, 線形補間読み) ----
 * 遅延 D = floor(D) + fr に対し x[n-floor(D)](1-fr) + x[n-floor(D)-1]·fr を返す
 * (旧実装は新しい側のサンプルを参照する逆方向補間で、実効遅延が D-1〜D に反転していた) */
static inline float chorus_line_process(float *buf, uint32_t *pos, float x, float delay_samp)
{
    if (delay_samp < 1.0f) delay_samp = 1.0f;
    uint32_t d = (uint32_t)delay_samp;
    float fr = delay_samp - (float)d;
    uint32_t idx = (*pos + 1024u - (d & 1023u)) & 1023u;   /* x[n-d] (2のべき乗マスク化) */
    uint32_t prv = (idx + 1023u) & 1023u;                 /* x[n-d-1] */
    float out = buf[idx] * (1.0f - fr) + buf[prv] * fr;
    buf[*pos] = x;
    *pos = (*pos + 1u) & 1023u;
    return out;
}

/**
 * @brief BPM 同期ディレイの長さ (サンプル) を算出
 *        8 分音符 = tempo_us/2、上限 100ms (SUB5_DELAY_MAX サンプル)、下限 20ms
 */
static uint32_t delay_len_samples(const AsmpSharedContext *shared)
{
    uint32_t tus = shared->main_ctrl.tempo_us_per_quarter;
    if (tus == 0) tus = 500000u;
    uint64_t eighth = ((uint64_t)tus / 2ull) * (uint64_t)SUB_SAMPLE_RATE / 1000000ull;
    if (eighth < 960ull) eighth = 960ull;       /* ~20ms */
    if (eighth > SUB5_DELAY_MAX) eighth = SUB5_DELAY_MAX;     /* ~100ms */
    return (uint32_t)eighth;
}


/**
 * @brief リバーブコア処理
 */
/**
 * @brief FDN リバーブ コア処理 (Householder 反饋 + 各線ダンピング + モデレーション)
 * @details 変調 LFO は 3 系統の位相グループ (0°/120°/240°) に分け、
 *          各ディレイ線 2 本ずつへ割り当てる。旧実装は全 6 線が同一位相の
 *          単一 LFO で変調されていたためモード縮退し密度が伸びなかった。
 *          入力はプリディレイ 30ms 経由で wet 側のみ時間シフトする。
 */
static inline void process_reverb_sample(Sub5DspEngine *eng, Sub5Reverb *rev, float in_l, float in_r,
                                         float *out_l, float *out_r)
{
    /* プリディレイ: mono 入力を 1440 サンプル (30ms) 遅延して反響網へ送る */
    float mono_in = eng->pred_buf[eng->pred_pos];
    eng->pred_buf[eng->pred_pos] = (in_l + in_r) * 0.5f;
    if (++eng->pred_pos >= SUB5_PREDELAY) eng->pred_pos = 0;

    /* 低音濁り対策: リバーブ入力から ~120Hz 以下を除去（Bass ch7 A1 55Hzの濁り根絶）。
     * one-pole HPF: HPF = in - LPF, LPF係数 0.0156 = 1 - exp(-2*pi*120/48000) */
    {
        float lp = eng->rev_hp_lp;
        lp += 0.0156f * (mono_in - lp); // 120Hz HPF (1-exp(-2*pi*120/48000))
        eng->rev_hp_lp = lp;
        mono_in = mono_in - lp;
    }

    fdn_mod_phase += SUB5_FDN_PHASE_INC;
    if (fdn_mod_phase >= 1.0f) fdn_mod_phase -= 1.0f;
    /* FDN変調 8間引き: 0.35Hz LFOは8サンプル(0.16ms)で位相0.00006しか進まず聴感差なし。
     * 1536LUT/epoch ->192LUTで0.13ms削減。RAM増8Bのみ */
    if ((eng->fdn_mod_div & 7u) == 0u) {
        float mod_g0 = sub_lookup_sine(g_sine_lut, fdn_mod_phase);
        float ph1 = fdn_mod_phase + 0.3333333f; if (ph1 >= 1.0f) ph1 -= 1.0f;
        float ph2 = fdn_mod_phase + 0.6666667f; if (ph2 >= 1.0f) ph2 -= 1.0f;
        float mod_g1 = sub_lookup_sine(g_sine_lut, ph1);
        float mod_g2 = sub_lookup_sine(g_sine_lut, ph2);
        /* 読出しオフセット確定 (C = 48 + (int)(mod*24) ∈ [24,72] で非負保証) */
        eng->fdn_rd_base[0] = 48 + (int32_t)(mod_g0 * 24.0f);
        eng->fdn_rd_base[1] = 48 + (int32_t)(mod_g0 * 24.0f);
        eng->fdn_rd_base[2] = 48 + (int32_t)(mod_g1 * 24.0f);
        eng->fdn_rd_base[3] = 48 + (int32_t)(mod_g1 * 24.0f);
        eng->fdn_rd_base[4] = 48 + (int32_t)(mod_g2 * 24.0f);
        eng->fdn_rd_base[5] = 48 + (int32_t)(mod_g2 * 24.0f);
    }
    eng->fdn_mod_div++;

    float feedback_gain = 0.70f + rev->room_size * 0.28f;

    /* FDN 6ライン読出し: 書込み位置 + 8区間定数オフセット (+上限折返しのみ。
     * 下限は C>=24 で非負保証のため判定不要。旧計算と bit 一致) */
    int32_t rd0 = (int32_t)fdn_pos[0] + eng->fdn_rd_base[0];
    int32_t rd1 = (int32_t)fdn_pos[1] + eng->fdn_rd_base[1];
    int32_t rd2 = (int32_t)fdn_pos[2] + eng->fdn_rd_base[2];
    int32_t rd3 = (int32_t)fdn_pos[3] + eng->fdn_rd_base[3];
    int32_t rd4 = (int32_t)fdn_pos[4] + eng->fdn_rd_base[4];
    int32_t rd5 = (int32_t)fdn_pos[5] + eng->fdn_rd_base[5];

    if (rd0 >= (int32_t)FDN_LEN_0) rd0 -= (int32_t)FDN_LEN_0;
    if (rd1 >= (int32_t)FDN_LEN_1) rd1 -= (int32_t)FDN_LEN_1;
    if (rd2 >= (int32_t)FDN_LEN_2) rd2 -= (int32_t)FDN_LEN_2;
    if (rd3 >= (int32_t)FDN_LEN_3) rd3 -= (int32_t)FDN_LEN_3;
    if (rd4 >= (int32_t)FDN_LEN_4) rd4 -= (int32_t)FDN_LEN_4;
    if (rd5 >= (int32_t)FDN_LEN_5) rd5 -= (int32_t)FDN_LEN_5;

    float s0 = fdn_buf[0][rd0];
    float s1 = fdn_buf[1][rd1];
    float s2 = fdn_buf[2][rd2];
    float s3 = fdn_buf[3][rd3];
    float s4 = fdn_buf[4][rd4];
    float s5 = fdn_buf[5][rd5];

    float sum = s0 + s1 + s2 + s3 + s4 + s5;

    float tap_l = (s0 * 1.0f - s1 * 0.7f + s2 * 0.8f - s3 * 1.0f + s4 * 0.6f - s5 * 0.8f);
    float tap_r = (-s0 * 0.8f + s1 * 1.0f - s2 * 0.6f + s3 * 0.7f - s4 * 1.0f + s5 * 0.65f);
    *out_l = tap_l * rev->wet_level;
    *out_r = tap_r * rev->wet_level;

    const float damping = rev->damping;
    const float fb_scale = 2.0f / 6.0f;
    #define FDN_STEP_WRITE(i, s_val) do { \
        float fb = (s_val) - fb_scale * sum; \
        if (fb > 0.9f) fb = 0.9f; \
        if (fb < -0.9f) fb = -0.9f; \
        float write_in = mono_in * 0.6f + fb * feedback_gain; \
        fdn_lp[i] += damping * (write_in - fdn_lp[i]); \
        fdn_buf[i][fdn_pos[i]] = fdn_lp[i]; \
        if (++fdn_pos[i] >= fdn_len_tbl[i]) fdn_pos[i] = 0; \
    } while(0)

    FDN_STEP_WRITE(0, s0);
    FDN_STEP_WRITE(1, s1);
    FDN_STEP_WRITE(2, s2);
    FDN_STEP_WRITE(3, s3);
    FDN_STEP_WRITE(4, s4);
    FDN_STEP_WRITE(5, s5);
    #undef FDN_STEP_WRITE
}

/**
 * @brief ソフトサチュレーション / クリッパー (C1 連続: 値・傾きとも連続)
 *        線形域 (|x| <= 0.85) -> 3 次ショルダー -> フラット天井 (|x| >= 1.30)。
 *        y = a + u - u²/w + u³/(3w²), u = |x| - a, w = 1.30 - 0.85
 *        旧実装は x=knee で傾きが不連続 (0.24 -> 0) になり、クリップ動作時に
 *        高調波の角が立って濁りの原因になっていた
 */
static inline float soft_limit(float x)
{
    /* NaN/Inf は比較分岐を素通しし、後段の float→int 変換で UB になるため遮断
     * (VMRS を踏む isfinite ではなく SUB_ISFINITE_F を使用) */
    if (!SUB_ISFINITE_F(x)) return 0.0f;
    const float a = 0.85f;
    const float ax = fabsf(x);
    if (ax <= a) return x;
    if (ax >= 1.30f) return (x > 0.0f) ? 1.0f : -1.0f;
    const float w_inv  = 2.2222222f;   /* 1 / w       (w = 0.45) */
    const float w3_inv = 1.6460905f;   /* 1 / (3w^2)            */
    const float u = ax - a;
    const float m = u - u * u * w_inv + u * u * u * w3_inv;
    return (x > 0.0f) ? (a + m) : -(a + m);
}

/**
 * @brief DSP ミキシング ＆ エフェクト処理実行
 *        構成: 3音源ミックス -> DCブロック -> コーラス -> 5バンドEQ
 *              -> BPMディレイ -> FDNリバーブ -> ピークリミッター
 *              -> ソフトサチュレーション
 *        ゲインプラン: 各段の最大合成利得を ~1.1 以下に収め、
 *        リミッターはトランジェント時のみ動作するヘッドルーム設計
 */
static void sub5_process_block(Sub5DspEngine *eng, uint32_t frames)
{
    AsmpSharedContext *shared = eng->shared;
    const uint32_t slot = eng->src_slot;
    const float * __restrict sub2_buf = shared->pcm_sub2_melody[slot];
    const float * __restrict sub3_buf = shared->pcm_sub3_bass[slot];
    const float * __restrict sub4_buf = shared->pcm_sub4_drums[slot];
    int16_t * __restrict master_buf = shared->pcm_sub5_master[slot];

    float vol = eng->master_volume;
    uint32_t dither_rng = eng->dither_rng;
    eng->cached_delay_len = delay_len_samples(shared);

#if SYNTH_SUB5_ABLATION
    /* 256 エポック (~2.7s) 毎に遮断ステージを回転:
     * ph0=全ON / ph1=-chorus / ph2=-EQ / ph3=-delay / ph4=-reverb /
     * ph5=-srcMix(共有メモリ読みをゼロへ) / ph6=-limiter / ph7=-TPDF /
     * ph8=フロア(DSP 全停止、量子化とバス書き込みのみ)
     * 切替時に Console へマーカーを出力し Main 側ログと同期する */
    static uint32_t s_abl_tick = 0;
    static uint32_t s_abl_last = 0xFFFFFFFFu;
    const uint32_t abl = (s_abl_tick++ / 256u) % 9u;
    if (abl != s_abl_last) {
        s_abl_last = abl;
        printf("[SUB5][ABL] phase=%u\n", (unsigned int)abl);
    }
#else
    const uint32_t abl = 0;
#endif
    const bool en_chorus = (abl != 1u && abl != 8u);
    const bool en_eq     = (abl != 2u && abl != 8u);
    const bool en_delay  = (abl != 3u && abl != 8u);
    const bool en_rev    = (abl != 4u && abl != 8u);
#if SYNTH_SUB5_ABLATION
    const bool en_srcmix = (abl != 5u && abl != 8u);
#endif
    const bool en_dc     = (abl != 8u);
    const bool en_limiter= (abl != 6u && abl != 8u);
    const bool en_tpdf   = (abl != 7u);

    for (uint32_t f = 0; f < frames; f++) {
        /* 1. 3つのサブ音源からサミング (float バス直結: 中間量子化なし。
         *     ソースは振幅域 [-1,1] で出力するため 1/3 スケールのみ適用) */
#if SYNTH_SUB5_ABLATION
        /* ph5: 共有メモリ読みコスト単離のためゼロ入力 (バス読みをスキップ) */
        float in_l = en_srcmix ? (sub2_buf[f * 2 + 0] + sub3_buf[f * 2 + 0] + sub4_buf[f * 2 + 0]) * SUB5_MIX_SCALE : 0.0f;
        float in_r = en_srcmix ? (sub2_buf[f * 2 + 1] + sub3_buf[f * 2 + 1] + sub4_buf[f * 2 + 1]) * SUB5_MIX_SCALE : 0.0f;
#else
        float in_l = (sub2_buf[f * 2 + 0] + sub3_buf[f * 2 + 0] + sub4_buf[f * 2 + 0]) * SUB5_MIX_SCALE;
        float in_r = (sub2_buf[f * 2 + 1] + sub3_buf[f * 2 + 1] + sub4_buf[f * 2 + 1]) * SUB5_MIX_SCALE;
#endif

        /* 2. DC オフセット除去 */
        if (en_dc) {
            in_l = sub_dc_process(&eng->dc_l, in_l);
            in_r = sub_dc_process(&eng->dc_r, in_r);
        }

        /* 3. ステレオコーラス (0.8Hz LFO) — 低域分離型
         * wet 経路のみ ~180Hz HP を通して変調するため、ベース/キックの輪郭が
         * コーラスのドップラー揺れで濁らない (dry はフルバンドのまま)。
         * R 側を +90 度直交位相で変調する (モノラル再生時のコムフィルタ防止) */
        if (en_chorus)
        {
            eng->chorus_phase += SUB5_CHORUS_PHASE_INC;
            if (eng->chorus_phase >= 1.0f) eng->chorus_phase -= 1.0f;
            float ph_r = eng->chorus_phase + 0.25f;   /* 90 度直交位相 */
            if (ph_r >= 1.0f) ph_r -= 1.0f;
            /* 毎サンプルの sinf() を LUT 参照に置換 */
            float lfo_l = sub_lookup_sine(g_sine_lut, eng->chorus_phase);
            float lfo_r = sub_lookup_sine(g_sine_lut, ph_r);
            /* one-pole HP 180Hz: y = x - lp, lp += 0.0233*(x-lp) . biquad比 -8FMA/sample */
            float ch_in_l, ch_in_r;
            { float x=in_l; float lp=eng->chorus_hp_l.s1; lp += 0.0233f*(x - lp); eng->chorus_hp_l.s1=lp; ch_in_l = x - lp; }
            { float x=in_r; float lp=eng->chorus_hp_r.s1; lp += 0.0233f*(x - lp); eng->chorus_hp_r.s1=lp; ch_in_r = x - lp; }
            float dl = 480.0f + 96.0f * lfo_l;   /* 10ms ± 2ms */
            float wl = chorus_line_process(eng->chorus_l, &eng->chorus_pos_l, ch_in_l, dl);
            float wr_delay = 528.0f + 96.0f * lfo_r;
            float wr = chorus_line_process(eng->chorus_r, &eng->chorus_pos_r, ch_in_r, wr_delay);
            in_l = in_l * SUB5_CHORUS_DRY + wl * SUB5_CHORUS_WET;
            in_r = in_r * SUB5_CHORUS_DRY + wr * SUB5_CHORUS_WET;
        }

/* 4. 5バンド EQ 処理 (Band4 8kHz 0dBはflatのためバイパスし4バンドで処理。
         *    60Hz +2dB / 200Hz -2dB / 1kHz +1dB / 3.2kHz +1.5dB で
         *    低域パンチ・中域クリアネス・メロディの抜け・アタックを全部活かす) */
        if (en_eq && eng->eq.enabled) {
            for (int b = 0; b < 4; b++) {
                in_l = biquad_process(&eng->eq.filters_l[b], in_l);
                in_r = biquad_process(&eng->eq.filters_r[b], in_r);
            }
        }

        /* 4b. BPM 同期ディレイ (8 分音符, フィードバック 0.34, 反響ダンピング付き)
         * 反響路の one-pole LP (~3kHz) が繰り返しごとに高域を吸収し、
         * エコーが最後まで明るくバズる代わりに自然に暗く溶ける */
        if (en_delay)
        {
            uint32_t len = eng->cached_delay_len;   /* ブロック毎に算出 (サンプル毎の除算を回避) */
            /* タップは新規サンプル書き込みの「前」に読む。
             * len が最大 SUB5_DELAY_MAX にクランプされると tap == delay_pos になり、
             * 書き込み後に読むと今書いたばかりの値 (=ディレイ 0) を拾うため */
            /* delay_pos, len ∈ [0,SUB5_DELAY_MAX) なので (delay_pos + MAX - len) ∈ [0, 2*MAX)。
             * よって % MAX (M4F ではソフト除算 __aeabi_uidivmod、上の FDN ループと
             * 同じ理由で高コスト) の代わりに条件減算 1 回で剰余相当になる */
            uint32_t raw_l = eng->delay_pos + SUB5_DELAY_MAX - len;
            uint32_t tap_l = (raw_l >= SUB5_DELAY_MAX) ? (raw_l - SUB5_DELAY_MAX) : raw_l;
            uint32_t raw_r = eng->delay_pos + SUB5_DELAY_MAX - (len * 7u) / 8u;
            uint32_t tap_r = (raw_r >= SUB5_DELAY_MAX) ? (raw_r - SUB5_DELAY_MAX) : raw_r;
            float d_l2 = eng->delay_line[tap_l];
            float d_r2 = eng->delay_line[tap_r];

            float d_out = eng->delay_line[eng->delay_pos];
            eng->delay_fb_lp += SUB5_DELAY_FB_DAMP * (d_out - eng->delay_fb_lp);
            eng->delay_line[eng->delay_pos] = (in_l + in_r) * 0.5f + eng->delay_fb_lp * 0.34f;

            /* エフェクト送信量: 旧 0.24 はコーラス合成後への上乗せで
             * ピークを必要以上に押し上げていた */
            in_l += d_l2 * SUB5_DELAY_SEND;
            in_r += d_r2 * SUB5_DELAY_SEND;

            if (++eng->delay_pos >= SUB5_DELAY_MAX) eng->delay_pos = 0u;
        }

        /* 5. ステレオリバーブ DSP 処理 (プリディレイ 30ms + FDN 変調デコリレート) */
        float wet_l = 0.0f, wet_r = 0.0f;
        if (en_rev && eng->reverb.enabled) {
            process_reverb_sample(eng, &eng->reverb, in_l, in_r, &wet_l, &wet_r);
        }

        float out_l = (in_l * eng->reverb.dry_level + wet_l) * vol;
        float out_r = (in_r * eng->reverb.dry_level + wet_r) * vol;

        /* 5. ルックアヘード ピークリミッター (真の 64要素リング モノトニック・デック O(1) 償却)
         * 48 サンプル (1ms) 遅延線に対し、窓幅 49 サンプル [n-48, n] の最大ピークを
         * 単調減少デックで常時 O(1) 参照。インターリーブ構造体配列 la_ring でキャッシュミスを極小化。
         * 天井保証のため利得下降は即時 (アタック即時)、復帰のみ平滑化する (Fugu 査読指摘) */
        if (en_limiter)
        {
            const float ceiling = SUB5_LIMIT_CEILING;

            /* a) 遅延線から 48 サンプル前の信号を取り出す */
            uint8_t rd_pos = (uint8_t)((eng->la_pos + SUB5_LOOKAHEAD_CAP - SUB5_LOOKAHEAD) & (SUB5_LOOKAHEAD_CAP - 1u));
            float dl = eng->la_ring[rd_pos].l;
            float dr = eng->la_ring[rd_pos].r;

            /* b) 現在サンプルのステレオリンク ピーク */
            float in_peak = fabsf(out_l);
            float p_r = fabsf(out_r);
            if (p_r > in_peak) in_peak = p_r;

            /* c) 現在サンプルとピークを遅延線構造体へ一括格納 (同一キャッシュライン局所化) */
            eng->la_ring[eng->la_pos].l = out_l;
            eng->la_ring[eng->la_pos].r = out_r;
            eng->la_ring[eng->la_pos].peak = in_peak;

            /* d) 窓外 (49 サンプルより古い) 先頭要素をデックから除去
             *    窓は [la_pos-48, la_pos] の49サンプル。出力サンプル (la_pos-48)
             *    のピークが利得算出 (g) より前に消えるとアタック頭がクリップ
             *    するため、expire は la_pos-49 (出力サンプルの1つ手前) にする */
            uint8_t expire_pos = (uint8_t)((eng->la_pos + SUB5_LOOKAHEAD_CAP - SUB5_LOOKAHEAD - 1u) & (SUB5_LOOKAHEAD_CAP - 1u));
            if (eng->dq_len > 0u && eng->dq[eng->dq_head] == expire_pos) {
                eng->dq_head = (uint8_t)((eng->dq_head + 1u) & (SUB5_LOOKAHEAD_CAP - 1u));
                eng->dq_len--;
            }

            /* e) 尾側の新ピーク値以下の要素を除去 (単調降順を維持) */
            while (eng->dq_len > 0u) {
                uint8_t tail_idx = (uint8_t)((eng->dq_head + eng->dq_len - 1u) & (SUB5_LOOKAHEAD_CAP - 1u));
                if (eng->la_ring[eng->dq[tail_idx]].peak <= in_peak) {
                    eng->dq_len--;
                } else {
                    break;
                }
            }

            /* f) 現インデックスを尾へ追加 */
            uint8_t insert_idx = (uint8_t)((eng->dq_head + eng->dq_len) & (SUB5_LOOKAHEAD_CAP - 1u));
            eng->dq[insert_idx] = eng->la_pos;
            eng->dq_len++;

            /* g) 最大ピークから目標利得を決定 */
            float look_peak = eng->la_ring[eng->dq[eng->dq_head]].peak;
            float target_gain = (look_peak > ceiling) ? (ceiling / look_peak) : 1.0f;

            /* h) 利得下降は即時 (天井突破阻止)、復帰のみ平滑 (時定数 ~100ms) */
            if (target_gain < eng->lim_gain) {
                eng->lim_gain = target_gain; /* 即時アタック */
            } else {
                eng->lim_gain += (target_gain - eng->lim_gain) * 0.0005f; /* 復帰平滑 */
            }
            if (!sub_isfinite_f(eng->lim_gain) || eng->lim_gain <= 0.0f) eng->lim_gain = 1.0f;
            if (eng->lim_gain > 1.0f) eng->lim_gain = 1.0f;

            /* i) 48 サンプル遅延信号へ利得を適用 */
            out_l = dl * eng->lim_gain;
            out_r = dr * eng->lim_gain;

            eng->la_pos = (uint8_t)((eng->la_pos + 1u) & (SUB5_LOOKAHEAD_CAP - 1u));
        }

        /* 6. 安全クリッパーとしてのソフトサチュレーション (リミッター後に配置) */
        {
            out_l = soft_limit(out_l);
            out_r = soft_limit(out_r);
        }

        /* 7. TPDF ディザ付き 16bit 量子化
         * 切り捨て/丸め由来の相関歪みを排除し、リバーブ尾がクリアに伸びる */
        if (en_tpdf)
        {
            master_buf[f * 2 + 0] = sub_quantize_dither(out_l, &dither_rng);
            master_buf[f * 2 + 1] = sub_quantize_dither(out_r, &dither_rng);
        }
        else
        {
            /* アブレーション ph6: TPDF なしの単純丸め (コスト比較用) */
            float q_l = out_l * 32767.0f;
            q_l += (q_l >= 0.0f) ? 0.5f : -0.5f;
            float q_r = out_r * 32767.0f;
            q_r += (q_r >= 0.0f) ? 0.5f : -0.5f;
            master_buf[f * 2 + 0] = sub_ssat16((int32_t)q_l);
            master_buf[f * 2 + 1] = sub_ssat16((int32_t)q_r);
        }
    }

    eng->dither_rng = dither_rng;
}

/**
 * @brief SubCore 5 エントリーポイント
 *
 * 非同期 Ping-Pong パイプライン:
 *  - レンダリング要求は Sub1 経由のキュー (RENDER_REQ) で受け、
 *    エポックを**逐次**処理する (スキップなし → Main の待機対象が常に書き込まれる)
 *  - ソース (Sub2/3/4) の完了 (done_epoch >= 自エポック) を待ってからミックス。
 *    待ち間にソースは既に次エポック (逆スロット) の合成へ進めるため、
 *    フレーム時間 = max(全コア個別時間) になる
 */
void *subcore5_entry(void *arg)
{
    AsmpSharedContext *shared = (AsmpSharedContext *)arg;
    if (!shared) return NULL;
    if (!asmp_abi_ok(shared)) {
        printf("[SUB5][FATAL] ABI mismatch at entry\n");
        return NULL;
    }

    sub5_engine_init(&s_sub5, shared);

     /* ビルド識別 + 実行時パラメータ自己診断 (float非対応対策で整数スケール) */
    printf("[SUB5][BUILD] %s | master=%d.%02d chorus=%d.%02d/%d.%02d delay=%d.%02d "
           "reverb=%d.%02d/%d.%02d ceiling=%d.%02d | MBComp=REMOVED\n",
           HEXASENSE_DSP_TAG,
           (int)s_sub5.master_volume, (int)(s_sub5.master_volume*100+0.5f)%100,
           (int)SUB5_CHORUS_DRY, (int)(SUB5_CHORUS_DRY*100+0.5f)%100,
           (int)SUB5_CHORUS_WET, (int)(SUB5_CHORUS_WET*100+0.5f)%100,
           (int)SUB5_DELAY_SEND, (int)(SUB5_DELAY_SEND*100+0.5f)%100,
           (int)s_sub5.reverb.wet_level, (int)(s_sub5.reverb.wet_level*100+0.5f)%100,
           (int)s_sub5.reverb.dry_level, (int)(s_sub5.reverb.dry_level*100+0.5f)%100,
           (int)SUB5_LIMIT_CEILING, (int)(SUB5_LIMIT_CEILING*100+0.5f)%100);

    /* CPU 負荷メーター (実測ビジー時間比率) */
    SubLoadMeter load_m; SUB_LOAD_INIT(load_m);

    uint32_t idle_loops = 0;

    while (!shared->main_ctrl.shutdown_requested) {
        /* コマンド処理 */
        AsmpPacket pkt;
        bool has_render_req = false;
        uint32_t req_epoch = 0;

        while (asmp_queue_pop(&shared->queues[ASMP_CORE_SUB5_DSP], &pkt)) {
            if (pkt.msg_type == ASMP_MSG_RENDER_REQ) {
                has_render_req = true;
                req_epoch = pkt.param;
                break;
            }
            switch (pkt.msg_type) {
                case ASMP_MSG_CMD_VOLUME:
                    s_sub5.master_volume = (float)pkt.param / 1000.0f;
                    break;
                case ASMP_MSG_CONTROL_CHANGE:
                    if (pkt.data1 == 91) {
                        /* Reverb Wet Level (CC#91)。上限を 0.45 に抑え
                         * 後段リミッターの常時作動を防ぐ (ゲインステージング整合) */
                        float wet = (float)pkt.data2 / 127.0f * 0.45f;
                        if (!(wet >= 0.0f && wet <= 0.45f)) wet = 0.22f; /* NaN/異常値ガード */
                        s_sub5.reverb.wet_level = wet;
                        s_sub5.reverb.dry_level = 1.0f - (wet * 0.35f);
                    }
                    break;
                case ASMP_MSG_ALL_NOTES_OFF:
                    /* B10: Sub5 FXリセット - 前曲残響を次曲へ持ち越さない */
                    {
                        /* ディレイ/リバーブ/コーラス/リミッターをクリア */
                        memset(s_sub5.delay_line, 0, sizeof(s_sub5.delay_line));
                        s_sub5.delay_pos = 0; s_sub5.delay_fb_lp = 0.0f;
                        for (int i=0;i<FDN_LINES;i++) { fdn_pos[i]=0; fdn_lp[i]=0.0f; memset(fdn_buf[i],0,(size_t)fdn_len_tbl[i]*sizeof(float)); }
                        fdn_mod_phase = 0.0f; s_sub5.fdn_mod_div=0;
                        memset(s_sub5.pred_buf,0,sizeof(s_sub5.pred_buf)); s_sub5.pred_pos=0;
                        memset(s_sub5.chorus_l,0,sizeof(s_sub5.chorus_l)); memset(s_sub5.chorus_r,0,sizeof(s_sub5.chorus_r));
                        s_sub5.chorus_pos_l=0; s_sub5.chorus_pos_r=0; s_sub5.chorus_phase=0.0f;
                        s_sub5.rev_hp_lp=0.0f;
                        s_sub5.lim_gain=1.0f; s_sub5.la_pos=0; s_sub5.dq_head=0; s_sub5.dq_len=0;
                        memset(s_sub5.la_ring,0,sizeof(s_sub5.la_ring));
                    }
                    break;
                default:
                    break;
            }
        }

        if (has_render_req) {
            static uint32_t s_last_epoch = 0;
            if (s_last_epoch != 0 && req_epoch != s_last_epoch + 1u) {
                shared->diag_epoch_gap[ASMP_CORE_SUB5_DSP]++;
            }
            s_last_epoch = req_epoch;
            /* カオスストレステスト専用: 外部割込み相当のランダム遅延 (通常ビルドは無効) */
            SUB_CHAOS_DELAY(ASMP_CORE_SUB5_DSP);
            uint32_t slot = ASMP_EPOCH_SLOT(req_epoch);
            const volatile uint32_t *d2 = &shared->done_epoch[ASMP_CORE_SUB2_LEAD].val;
            const volatile uint32_t *d3 = &shared->done_epoch[ASMP_CORE_SUB3_BASS].val;
            const volatile uint32_t *d4 = &shared->done_epoch[ASMP_CORE_SUB4_DRUM].val;

            /* ソース 3 コアの同エポック完了待ち。
             * キューが FIFO でエポック抜けが無いため、この待ちが解消次第
             * 直ちに次エポック処理へ移行できる (完全同期型と異なり Main を介さない)。
             * 待ちには上限を設ける: ソース側の RENDER_REQ がキュー満杯で
             * 落とされた場合、ここで永久待ちするとパイプライン全体が凍結し、
             * ハートビートも止まって巻き添え再起動になる。 */
            /* 連続 timeout カウント (wait前に参照するため前方宣言) */
            static uint32_t s_consec_timeout = 0u;
            uint32_t wait_start_ms = sub5_now_ms();
            bool src_timeout = false;
#ifdef PROFILE_ENABLE
            profile_epoch_start(5, req_epoch);
#endif
            for (;;) {
                ASMP_BARRIER();
                if (asmp_epoch_done(d2, req_epoch) &&
                    asmp_epoch_done(d3, req_epoch) &&
                    asmp_epoch_done(d4, req_epoch)) {
                    break;
                }
                if (shared->main_ctrl.shutdown_requested) return NULL;
                /* 待機中もハートビートを申告 (生存監視が待ちを死と誤認しないように) */
                if ((++idle_loops & 0x3Fu) == 0u) {
                    shared->core[ASMP_CORE_SUB5_DSP].heartbeat++;
                }
                /* 修正E: 連続タイムアウトで短縮しない。12ms固定で粘る。
                 * 旧実装の 12ms->8ms->4ms 短縮は「負荷が上がるほど諦めが早くなる」
                 * 正フィードバックでフェード代用ブロック連鎖を加速させていたため削除。
                 * 何回連続で諦めたかだけをパニック判定に使う */
                {
                    uint32_t dyn_to = SUB5_SRC_WAIT_TIMEOUT_MS;
                    if ((sub5_now_ms() - wait_start_ms) > dyn_to) {
                        src_timeout = true;
                        break;
                    }
                }
#if defined(__NuttX__)
                /* 実機: ソース完了待ちもビジースピン (usleep のティック丸め
                 * 10ms がエポック遅延に直結するため。専用コアなので可) */
                for (volatile int spin = 0; spin < 512; spin++) {
                    ASMP_BARRIER();
                }
#else
                sub5_sleep_us(25);
#endif
            }
            /* P0-B片手落ち修正: 読み側でも世代ガードを検証。done_epoch>=req_epochは
             * 「完了申告」でしかなく、ASMP_NUM_SLOTS=2の再利用でtorn readを防げない。
             * owner_mask全ビット揃いを必須にし、不一致なら既存のfade経路へ合流 */
            if (!src_timeout) {
                const AlignedSlotHeader *h = &shared->slot_headers[slot];
                asmp_dcache_invalidate((const void *)h, sizeof(*h));
                ASMP_BARRIER();
                const uint32_t need = ASMP_SLOT_WORKER_BIT_SUB2 |
                                       ASMP_SLOT_WORKER_BIT_SUB3 |
                                       ASMP_SLOT_WORKER_BIT_SUB4;
                uint32_t mask = (h->epoch == req_epoch) ? h->owner_mask : 0u;
                if ((mask & need) != need) {
                    shared->diag_slot_rejected++;
                    if (mask == 0u) {
                        /* 3コアとも未着: 全滅時のみフェードアウト */
                        src_timeout = true;
                    } else {
                        /* 一部コアのみ完了: 未着コアのPCMのみゼロクリアし、完了コアの音は出力する (道連れ音飛び防止) */
                        uint32_t ef_clr = shared->render_ctrl.epoch_frames[slot];
                        if (ef_clr == 0u || ef_clr > ASMP_BUFFER_FRAMES) ef_clr = ASMP_BUFFER_FRAMES;
                        if (!(mask & ASMP_SLOT_WORKER_BIT_SUB2)) {
                            memset(shared->pcm_sub2_melody[slot], 0, ef_clr * sizeof(float) * 2);
                        }
                        if (!(mask & ASMP_SLOT_WORKER_BIT_SUB3)) {
                            memset(shared->pcm_sub3_bass[slot], 0, ef_clr * sizeof(float) * 2);
                        }
                        if (!(mask & ASMP_SLOT_WORKER_BIT_SUB4)) {
                            memset(shared->pcm_sub4_drums[slot], 0, ef_clr * sizeof(float) * 2);
                        }
                    }
                }
            }
            if (src_timeout) {
                ASMP_BARRIER();
                (void)asmp_epoch_done(d2, req_epoch);
                (void)asmp_epoch_done(d3, req_epoch);
                (void)asmp_epoch_done(d4, req_epoch);
                s_consec_timeout++;
                shared->diag_timeout++;
                /* Commit1: カウンタのみ (同期printf撤廃) */
                /* 優先4: consec 10でSub4強制クリア要求 (Sub1経由でALL_NOTES_OFF) */
                if (s_consec_timeout == 10u) {
                    shared->main_ctrl.sub5_force_clear_req = 1;
                }

                /* timeout 時は前ブロック反復ではなく末尾からゼロへフェードでバズ防止 (B追加) */
                uint32_t prev_slot = slot ^ 1u;
                uint32_t ef_copy = shared->render_ctrl.epoch_frames[slot];
                if (ef_copy == 0u || ef_copy > ASMP_BUFFER_FRAMES) ef_copy = ASMP_BUFFER_FRAMES;
                {
                    int16_t last_l = shared->pcm_sub5_master[prev_slot][(ef_copy >0 ? ef_copy-1:0)*2];
                    int16_t last_r = shared->pcm_sub5_master[prev_slot][(ef_copy >0 ? ef_copy-1:0)*2+1];
                    for (uint32_t i=0;i<ef_copy;i++) {
                        float g = 1.0f - (float)i / (float)ef_copy;
                        shared->pcm_sub5_master[slot][i*2] = (int16_t)(last_l * g);
                        shared->pcm_sub5_master[slot][i*2+1] = (int16_t)(last_r * g);
                    }
                }
                /* キャッシュクリーン & Release バリア → done_epoch 公開 */
                asmp_dcache_clean(shared->pcm_sub5_master[slot],
                                  ((size_t)ef_copy * sizeof(int16_t) * 2 + 31u) & ~31u);
                ASMP_BARRIER();
                shared->done_epoch[ASMP_CORE_SUB5_DSP].val = req_epoch;
                ASMP_BARRIER();
                shared->core[ASMP_CORE_SUB5_DSP].heartbeat++;
                idle_loops = 0;
                SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB5_DSP);
#ifdef PROFILE_ENABLE
                profile_epoch_end(5, req_epoch);
#endif
                continue; /* 通常処理 (sub5_process_block) をスキップ */
            } else {
                s_consec_timeout = 0u;
            }

            if (!asmp_abi_ok(shared)) {
                printf("[SUB5][FATAL] ABI mismatch\n");
                return NULL;
            }
            if (shared->render_ctrl.slot_epoch[slot] != req_epoch) {
                shared->diag_slot_mismatch++;
            }
#ifdef PROFILE_ENABLE
            profile_epoch_start_pure(5, req_epoch);
#endif
            SUB_LOAD_BUSY_BEGIN(load_m);
            uint64_t t0_ns = sub_get_ns();
            s_sub5.src_slot = slot;
            /* S3: per-slot化 */
            uint32_t ef = shared->render_ctrl.epoch_frames[slot];
            if (ef == 0u || ef > ASMP_BUFFER_FRAMES) ef = ASMP_BUFFER_FRAMES;

            /* ソースコアの done_epoch 確定後、Acquire バリアとキャッシュ無効化を行ってからミキシング。
             * サイズは 32B キャッシュライン単位へ切り上げ、隣接スロット/変数の
             * ライン共有による意図しない無効化 (False Sharing) を防止する
             * (バス float 化に伴い 1 フレームあたり 8 バイト) */
            size_t inv_size = ((size_t)ef * sizeof(float) * 2 + 31u) & ~31u;
            ASMP_BARRIER();
            asmp_dcache_invalidate(shared->pcm_sub2_melody[slot], inv_size);
            asmp_dcache_invalidate(shared->pcm_sub3_bass[slot],   inv_size);
            asmp_dcache_invalidate(shared->pcm_sub4_drums[slot],  inv_size);

            sub5_process_block(&s_sub5, ef);

#if defined(SUB5_INJECT_SPIN_COUNT) && (SUB5_INJECT_SPIN_COUNT > 0)
            /* 実機限界特性テスト用: 意図的な負荷肥大スピン */
            for (volatile int spin = 0; spin < SUB5_INJECT_SPIN_COUNT; spin++) {
                ASMP_BARRIER();
            }
#endif

            SUB_EPOCH_TIME_UPDATE(shared, ASMP_CORE_SUB5_DSP, t0_ns);
#ifdef PROFILE_ENABLE
            profile_epoch_end_pure(5, req_epoch);
#endif

            /* マスター PCM キャッシュクリーン & Release バリア (done_epoch 公開前にペイロード確定) 32B丸め */
            asmp_dcache_clean(shared->pcm_sub5_master[slot], ((size_t)ef * sizeof(int16_t) * 2 + 31u) & ~31u);
            ASMP_BARRIER();

            shared->done_epoch[ASMP_CORE_SUB5_DSP].val = req_epoch;
            ASMP_BARRIER();
            shared->core[ASMP_CORE_SUB5_DSP].heartbeat++;
#ifdef PROFILE_ENABLE
            profile_epoch_end(5, req_epoch);
#endif
            idle_loops = 0;
            SUB_LOAD_BUSY_END(load_m);
        } else {
            /* 待機中も低レートでハートビートを申告 (生存監視用) */
            if ((++idle_loops & 0x3Fu) == 0u) {
                shared->core[ASMP_CORE_SUB5_DSP].heartbeat++;
            }
            sub5_sleep_us(50);
        }
        SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB5_DSP);
    }

    return NULL;
}
