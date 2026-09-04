/**
 * @file sub_spawn.h
 * @brief Core1 voice-spawn descriptor: MIDI解釈のCore1集約 (ABI v13)
 * @details 生MIDI -> 発音パラメータの解釈 (program->音色/ADSR/フィルタ、
 *          note->周波数/Q32増分) を Core1 (Sub1) で1回だけ行い、演奏コアは
 *          結果の受け取り+発音のみ行う。従来は Sub2/Sub3 が各々解釈しており、
 *          同一 program でもコア毎・到達順序毎に音色/ADSR/フィルタが異なって
 *          いた (移動前後の音変 = 音痴の構造原因)。本ヘッダの正準 build 関数が
 *          唯一の解釈であり、Core1 (分解送信) と演奏コア (フォールバック解釈)
 *          の双方が同一関数を呼ぶため、経路によらず音は一致する。
 *          発音中に live 追従するもの (bend/vol/pan/expr/mod) は扱わない。
 *          Sub1 (SUB_COMMON_NO_LUT) でも動作するよう libm 直接呼び
 *          (powf/sqrtf, note-on レートのみ) で LUT に依存しない。
 */

#ifndef SUB_SPAWN_H_
#define SUB_SPAWN_H_

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "asmp_protocol.h"
#include "sub_common.h"

/* ========================================================================= */
/* トークン (NOTE_ON.param): [31:24]magic [23:8]gen [7:0]slot                 */
/* ========================================================================= */
static inline uint32_t sub_spawn_token(uint16_t gen, uint8_t idx)
{
    return ((uint32_t)SUB_SPAWN_TOKEN_MAGIC << 24) |
           ((uint32_t)gen << 8) | (uint32_t)idx;
}

static inline bool sub_spawn_token_parse(uint32_t param, uint16_t *gen_out, uint8_t *idx_out)
{
    if ((param >> 24) != (uint32_t)SUB_SPAWN_TOKEN_MAGIC) return false;
    uint32_t gen = (param >> 8) & 0xFFFFu;
    uint32_t idx = param & 0xFFu;
    if (gen == 0u || idx >= SUB_SPAWN_POOL_SLOTS) return false;
    *gen_out = (uint16_t)gen;
    *idx_out = (uint8_t)idx;
    return true;
}

/* ========================================================================= */
/* 正準マッピング: bend-free 基本周波数                                       */
/* sub_common.h の s_sub_freq_lut 初期式と逐語同一 (bit一致保証)              */
/* ========================================================================= */
static inline float sub_spawn_base_freq(uint8_t note)
{
    return 440.0f * powf(2.0f, ((float)(note & 0x7F) - 69.0f) / 12.0f);
}

/**
 * @brief Sub2用 spawn パラメータ構築 (sub2_note_on の解釈部と同一)
 * @param gov_unison_off quality_flags の UNISON_OFF (Core1が送信時に読んだ値)
 * @param thin 飽和時の強制薄化 (unison を落として1osc化。音痴ではなく意図的間引き)
 */
static inline void sub_spawn_build_sub2(SubSpawnDesc *d, uint8_t channel, uint8_t note,
                                        uint8_t vel, uint8_t program,
                                        bool gov_unison_off, bool thin)
{
    uint8_t prog = program;
    d->channel = channel;
    d->note = (uint8_t)(note & 0x7F);
    d->velocity = vel;
    d->program = prog;
    float base_freq = sub_spawn_base_freq(note);
    d->frequency = base_freq;
    d->base_increment = base_freq / (float)SUB_SAMPLE_RATE;
    float velocity = (float)vel / 127.0f;

    if (prog < 8) {
        /* Acoustic Piano (0-7): 1ms鋭角ハンマー打弦アタック + クリスタル倍音開口 */
        d->wave = SUB_SPAWN_WAVE_TRIANGLE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.350f; d->adsr_s = 0.250f; d->adsr_r = 0.080f;
    } else if (prog >= 8 && prog < 16) {
        /* Chromatic Perc (8-15): 鐘系パーカッシブ (host GM 表と統一) */
        d->wave = SUB_SPAWN_WAVE_TRIANGLE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.300f; d->adsr_s = 0.250f; d->adsr_r = 0.080f;
    } else if (prog >= 16 && prog < 24) {
        /* Organ (16-23): 2msキークリック立ち上がり + 豊潤サステイン */
        d->wave = SUB_SPAWN_WAVE_SINE;
        d->adsr_a = 0.0020f; d->adsr_d = 0.050f; d->adsr_s = 0.900f; d->adsr_r = 0.040f;
    } else if (prog >= 24 && prog < 32) {
        /* Guitar (24-31): 0.8ms超鋭角ピックプラック */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0008f; d->adsr_d = 0.220f; d->adsr_s = 0.350f; d->adsr_r = 0.060f;
    } else if (prog >= 32 && prog < 40) {
        /* Bass (32-39): ファットな矩形波 (host GM 表と統一。Sub3 へ回るのが本筋) */
        d->wave = SUB_SPAWN_WAVE_SQUARE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.180f; d->adsr_s = 0.650f; d->adsr_r = 0.050f;
    } else if (prog >= 40 && prog < 48) {
        /* Strings (40-47): 8msボウイング初動アタック (もたつき排除) */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0080f; d->adsr_d = 0.120f; d->adsr_s = 0.850f; d->adsr_r = 0.140f;
    } else if (prog >= 48 && prog < 56) {
        /* Ensemble (48-55): ストリングス寄り (host GM 表と統一) */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0040f; d->adsr_d = 0.120f; d->adsr_s = 0.800f; d->adsr_r = 0.120f;
    } else if (prog >= 56 && prog < 64) {
        /* Brass (56-63): 2.5msリップアタック「パァン！」 */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0025f; d->adsr_d = 0.080f; d->adsr_s = 0.800f; d->adsr_r = 0.060f;
    } else {
        /* Lead (80-87) / Synth / Other: 1ms超高速アタック・高エネルギー立ち上がり */
        d->wave = SUB_SPAWN_WAVE_SQUARE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.080f; d->adsr_s = 0.650f; d->adsr_r = 0.050f;
    }
    d->exp_decay = 1u;
    d->unison = ((!gov_unison_off &&
                  ((prog >= 56 && prog < 72) || (prog >= 80 && prog < 96)) &&
                  !thin) ? 1u : 0u);
    d->is_bass = 0u;

    /* ウェーブテーブル発音: Lead クラシック 4 波 (80-87) 以外は GM マップ */
    /* 全音色ウェーブテーブル発音 (Q32超高速・バンドリミット高音質):
     * Lead/Synth (80〜) も Analog Saw テーブルを活用し、PolyBLEP の分岐・除算負荷を完全根絶 */
    d->wt_active = 1u;
    if (d->wt_active) {
        float wm = subwt_program_to_morph(prog);
        uint8_t ma = (uint8_t)wm;
        uint8_t mb;
        float mw;
        if (ma > SUBWT_TABLES - 1u) ma = SUBWT_TABLES - 1u;
        mw = wm - (float)ma;
        if (mw < 0.0f || ma >= SUBWT_TABLES - 1u) {
            mw = 0.0f;
            mb = ma;
        } else {
            mb = (uint8_t)(ma + 1u);
        }
        d->morph_a = ma;
        d->morph_b = mb;
        d->morph_w = mw;
    } else {
        d->morph_a = 0u;
        d->morph_b = 0u;
        d->morph_w = 0.0f;
    }
    d->mip = subwt_pick_mip(base_freq);

    /* ペルボイス共振LPF + フィルターエンベロープ (初動倍音開口ブースト) */
    {
        float base_hz, peak_add, q;
        if (prog < 8) {            /* Piano: 抜ける打鍵倍音 (4.4kHz開口) */
            base_hz = 850.0f;  peak_add = 3600.0f; q = 1.05f;
        } else if (prog >= 8 && prog < 16) { /* Chromatic: ピアノ寄り鐘系 */
            base_hz = 850.0f;  peak_add = 3600.0f; q = 1.05f;
        } else if (prog >= 16 && prog < 24) { /* Organ: 明るいトーンホイール抜け */
            base_hz = 1500.0f; peak_add = 1200.0f; q = 0.75f;
        } else if (prog >= 24 && prog < 32) { /* Guitar: 強烈なアタックエッジ (5kHz開口) */
            base_hz = 1000.0f; peak_add = 4200.0f; q = 1.20f;
        } else if (prog >= 32 && prog < 40) { /* Bass: タイトな重低音 (Sub3 寄り) */
            base_hz = 300.0f;  peak_add = 2200.0f; q = 1.30f;
        } else if (prog >= 40 && prog < 48) { /* Strings: 打頭オープン (3.3kHz開口) */
            base_hz = 750.0f;  peak_add = 2600.0f; q = 0.90f;
        } else if (prog >= 48 && prog < 56) { /* Ensemble: ストリングス寄り */
            base_hz = 750.0f;  peak_add = 2600.0f; q = 0.90f;
        } else if (prog >= 56 && prog < 64) { /* Brass: 鋭いリップ開口 (5.1kHz開口) */
            base_hz = 1100.0f; peak_add = 4000.0f; q = 1.25f;
        } else {                   /* Lead/Synth: アグレッシブなハイパースイープ (6kHz開口) */
            base_hz = 1200.0f; peak_add = 4800.0f; q = 1.35f;
        }
        float kt_in = base_freq * (1.0f / 261.6256f);
        float keytrack = sqrtf(sqrtf(kt_in));
        if (!(keytrack > 0.55f)) keytrack = 0.55f;   /* NaN/下限ガード */
        if (keytrack > 2.60f) keytrack = 2.60f;
        /* ベロシティ -> 基準カットオフ追従 (弱弾きは暗く、強弾きは明るく開く) */
        float vel_open = 0.70f + 0.55f * velocity;
        d->filt_base = base_hz * vel_open * keytrack;
        d->filt_peak = (base_hz + peak_add * (0.35f + 0.65f * velocity)) * keytrack;
        d->filt_q = q;
    }
}

/**
 * @brief Sub3用 spawn パラメータ構築 (sub3_note_on の解釈部と同一)
 */
static inline void sub_spawn_build_sub3(SubSpawnDesc *d, uint8_t channel, uint8_t note,
                                        uint8_t vel, uint8_t program)
{
    uint8_t prog = program;
    d->channel = channel;
    d->note = (uint8_t)(note & 0x7F);
    d->velocity = vel;
    d->program = prog;
    float base_freq = sub_spawn_base_freq(note);
    d->frequency = base_freq;
    d->base_increment = base_freq / (float)SUB_SAMPLE_RATE;
    float velocity = (float)vel / 127.0f;

    uint8_t is_bass = ((prog >= 32 && prog < 40) ? 1u : 0u);
    d->is_bass = is_bass;
    d->exp_decay = 1u;
    if (is_bass) {
        /* Bass (32-39): ファットな矩形波/ノコギリ波 + スラップスナップ打頭倍音 + サブオシ重低音 */
        d->wave = (prog == 38) ? SUB_SPAWN_WAVE_SAWTOOTH : SUB_SPAWN_WAVE_SQUARE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.180f; d->adsr_s = 0.650f; d->adsr_r = 0.050f;
    } else if (prog < 8) {
        /* Acoustic Piano (0-7): 1ms鋭角ハンマー打弦アタック (Sub2と音色整合) */
        d->wave = SUB_SPAWN_WAVE_TRIANGLE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.350f; d->adsr_s = 0.250f; d->adsr_r = 0.080f;
    } else if (prog >= 8 && prog < 16) {
        /* Chromatic Perc (8-15): 鐘系パーカッシブ (host GM 表と統一) */
        d->wave = SUB_SPAWN_WAVE_TRIANGLE;
        d->adsr_a = 0.0010f; d->adsr_d = 0.300f; d->adsr_s = 0.250f; d->adsr_r = 0.080f;
    } else if (prog >= 16 && prog < 24) {
        /* Organ (16-23): 2msキークリック立ち上がり + 高サステイン */
        d->wave = SUB_SPAWN_WAVE_SINE;
        d->adsr_a = 0.0020f; d->adsr_d = 0.050f; d->adsr_s = 0.900f; d->adsr_r = 0.040f;
    } else if (prog >= 40 && prog < 48) {
        /* Strings (40-47): 8msボウイング初動アタック & 豊潤サステイン */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0080f; d->adsr_d = 0.120f; d->adsr_s = 0.850f; d->adsr_r = 0.140f;
    } else if (prog >= 48 && prog < 56) {
        /* Ensemble (48-55): ストリングス寄り (host GM 表と統一) */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0040f; d->adsr_d = 0.120f; d->adsr_s = 0.800f; d->adsr_r = 0.120f;
    } else if (prog >= 24 && prog < 32) {
        /* Guitar (24-31): 0.8ms超鋭角ピックプラック */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0008f; d->adsr_d = 0.220f; d->adsr_s = 0.350f; d->adsr_r = 0.060f;
    } else if (prog >= 88 && prog < 96) {
        /* Pad / Warm / Choir: 15msアタック + 豊潤エンベロープ */
        d->wave = SUB_SPAWN_WAVE_TRIANGLE;
        d->adsr_a = 0.0150f; d->adsr_d = 0.200f; d->adsr_s = 0.800f; d->adsr_r = 0.200f;
    } else {
        /* Other / Default: 1.5msアタック */
        d->wave = SUB_SPAWN_WAVE_SAWTOOTH;
        d->adsr_a = 0.0015f; d->adsr_d = 0.100f; d->adsr_s = 0.700f; d->adsr_r = 0.080f;
    }
    d->unison = 0u;
    d->wt_active = 0u;
    d->morph_a = 0u;
    d->morph_b = 0u;
    d->mip = 0u;
    d->morph_w = 0.0f;

    /* ペルボイス共振LPF (初動倍音開口ブースト) */
    {
        float base_hz, peak_add, q;
        if (is_bass) {                      /* Bass: タイトな重低音 + 鋭いスラップ開口 (2.4kHz) */
            base_hz = 180.0f;  peak_add = 2200.0f; q = 1.30f;
        } else if (prog < 8) {              /* Piano: 抜けの良い自然な高域 (4.8kHz) */
            base_hz = 1200.0f; peak_add = 3600.0f; q = 0.85f;
        } else if (prog >= 8 && prog < 16) { /* Chromatic: ピアノ寄り */
            base_hz = 1200.0f; peak_add = 3600.0f; q = 0.85f;
        } else if (prog >= 40 && prog < 48) { /* Strings: 豊かなアタック開口 (3.1kHz) */
            base_hz = 700.0f;  peak_add = 2400.0f; q = 0.85f;
        } else if (prog >= 48 && prog < 56) { /* Ensemble: ストリングス寄り */
            base_hz = 700.0f;  peak_add = 2400.0f; q = 0.85f;
        } else if (prog >= 88 && prog < 96) { /* Pad: 柔らかなローパス */
            base_hz = 550.0f;  peak_add = 1400.0f; q = 0.70f;
        } else {                              /* Default */
            base_hz = 500.0f;  peak_add = 2000.0f; q = 1.10f;
        }
        /* キートラッキング: ベースは低音の太さを優先して緩い 0.15 乗則、
         * それ以外は 0.25 乗則で高音域の開きを確保する */
        float kt_exp = is_bass ? 0.15f : 0.25f;
        float kt_in = base_freq * (1.0f / 261.6256f);
        float keytrack;
        if (kt_exp == 0.25f) {
            keytrack = sqrtf(sqrtf(kt_in)); /* 70cyc->12cyc: 0.25乗はsqrt二乗で同値 */
        } else {
            keytrack = powf(kt_in, kt_exp); /* bass 0.15は稀な分岐、powf維持 */
        }
        if (!(keytrack > 0.55f)) keytrack = 0.55f;   /* NaN/下限ガード */
        if (keytrack > 2.60f) keytrack = 2.60f;
        d->filt_base = base_hz * keytrack;
        d->filt_peak = (base_hz + peak_add * (0.35f + 0.65f * velocity)) * keytrack;
        d->filt_q = q;
    }
}

/* ========================================================================= */
/* SPSC プール送受信 (Core1 producer / 演奏コア consumer)                     */
/* 公開順序: (1) desc 書き込み+clean (2) gen 書き込み+clean。consumer は      */
/* gen 一致を確認してから desc を読むため、陳腐 desc を掴むことはない。       */
/* ========================================================================= */

/** Core1側 producer 状態 (コア毎に1個。route_init でリセットすること) */
typedef struct {
    uint32_t sent;      /**< 発行トークン数 (単調増加) */
    uint16_t next_gen;  /**< 次回世代 (1開始、0を飛ばす) */
    uint8_t  cursor;    /**< 次回スロット */
    uint8_t  _pad;
} SubSpawnProd;

static inline void sub_spawn_prod_reset(SubSpawnProd *st)
{
    st->sent = 0u;
    st->next_gen = 1u;
    st->cursor = 0u;
    st->_pad = 0u;
}

/**
 * @brief ディスクリプタ発行。満杯 (未消費16件) なら false。
 *        false時は呼び出し側が param=0 の従来 NOTE_ON を送ること。
 */
static inline bool sub_spawn_produce(SubSpawnSlot *pool, volatile uint32_t *consumed_reg,
                                     SubSpawnProd *st, const SubSpawnDesc *d,
                                     uint32_t *token_out)
{
    asmp_dcache_invalidate((const void *)consumed_reg, sizeof(uint32_t));
    ASMP_BARRIER();
    uint32_t consumed = *consumed_reg;
    if ((st->sent - consumed) >= SUB_SPAWN_POOL_SLOTS) return false; /* 未消費で満杯 */
    uint8_t idx = st->cursor;
    SubSpawnSlot *slot = &pool[idx];
    /* 1. ペイロード公開 */
    slot->desc = *d;
    asmp_dcache_clean((const void *)&slot->desc, sizeof(slot->desc));
    ASMP_BARRIER();
    /* 2. 世代公開 (desc と別ラインのため順序が保証される) */
    uint16_t gen = st->next_gen;
    slot->gen = (uint32_t)gen;
    asmp_dcache_clean((const void *)&slot->gen, sizeof(slot->gen));
    ASMP_BARRIER();
    *token_out = sub_spawn_token(gen, idx);
    st->sent++;
    st->cursor = (uint8_t)(((uint32_t)idx + 1u) & (SUB_SPAWN_POOL_SLOTS - 1u));
    uint32_t ng = (uint32_t)gen + 1u;
    if (ng > 0xFFFFu) ng = 1u;
    st->next_gen = (uint16_t)ng;
    return true;
}

/**
 * @brief ディスクリプタ消費 (copy 完了後に ack を公開する)
 * @return 有効なトークンで copy できたら true (false時は従来解釈すること)
 */
static inline bool sub_spawn_consume(SubSpawnSlot *pool, volatile uint32_t *consumed_reg,
                                     uint32_t token, SubSpawnDesc *out)
{
    uint16_t gen;
    uint8_t idx;
    if (!sub_spawn_token_parse(token, &gen, &idx)) return false;
    SubSpawnSlot *slot = &pool[idx];
    asmp_dcache_invalidate((const void *)&slot->gen, sizeof(slot->gen));
    ASMP_BARRIER();
    if (slot->gen != (uint32_t)gen) return false; /* 陳腐化 (再利用済み) */
    asmp_dcache_invalidate((const void *)&slot->desc, sizeof(slot->desc));
    ASMP_BARRIER();
    *out = slot->desc;
    {
        uint32_t c = *consumed_reg;
        *consumed_reg = c + 1u;
    }
    asmp_dcache_clean((const void *)consumed_reg, sizeof(uint32_t));
    ASMP_BARRIER();
    return true;
}

/**
 * @brief 消費せず ack のみ (pending 溢れで NOTE_ON を捨てる際に解放する)。
 *        スロットが既に再利用 (世代不一致) なら何もしない。
 */
static inline void sub_spawn_ack_only(SubSpawnSlot *pool, volatile uint32_t *consumed_reg,
                                      uint32_t token)
{
    uint16_t gen;
    uint8_t idx;
    if (!sub_spawn_token_parse(token, &gen, &idx)) return;
    SubSpawnSlot *slot = &pool[idx];
    asmp_dcache_invalidate((const void *)&slot->gen, sizeof(slot->gen));
    ASMP_BARRIER();
    if (slot->gen != (uint32_t)gen) return; /* 他者の所有。ackしない */
    {
        uint32_t c = *consumed_reg;
        *consumed_reg = c + 1u;
    }
    asmp_dcache_clean((const void *)consumed_reg, sizeof(uint32_t));
    ASMP_BARRIER();
}

#endif /* SUB_SPAWN_H_ */
