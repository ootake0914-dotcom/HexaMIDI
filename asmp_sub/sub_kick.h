/**
 * @file sub_kick.h
 * @brief GM ドラム Kick (note 35/36) 専用の共有音源モジュール
 * @details SubCore 4 のドラム音源から Kick だけを SubCore 2/3 へ動的移行する
 *          ための専用 DSP (Phase 4)。sub4_main.c の sub4_render_kick() と
 *          同一アルゴリズム (sine LUT + pitch sweep + exp エンベロープ +
 *          72 サンプル ビータークリック + ±0.95 飽和) を移植したもの。
 *          各 Core が独立にこのモジュールを持ち、noise_seed / volume も
 *          Core 毎に所有する (C4 の noise_seed とは共有しない)。
 *
 *          音質を変えないため、sub4 の Kick ロジックの数値・定数は
 *          一切変更していない (サイン頂点開始 phase=0.25, 130/120/105Hz,
 *          target 48Hz, sweep exp(-11), click 72 サンプル)。
 */
#ifndef SUB_KICK_H_
#define SUB_KICK_H_

#include "sub_common.h"

/* 同時発音数上限 (Kick は短い one-shot のため 4 音で十分) */
#define SUB_KICK_MAX_VOICES (4u)

/* Kick チューニング定数 (sub4_main.c の SUB4_KICK_* と同一値) */
#define SUB_KICK_START_HARD   (130.0f)   /* velocity >= 0.85 */
#define SUB_KICK_START_NORMAL (120.0f)   /* velocity >= 0.50 */
#define SUB_KICK_START_GHOST  (105.0f)   /* それ以下 */
#define SUB_KICK_TARGET       (48.0f)
#define SUB_KICK_SWEEP_EXP    (-11.0f)
#define SUB_KICK_CLICK_LEN    (72u)      /* ビータークリック長 (~1.5ms) */
#define SUB_KICK_DEFAULT_VOL  (0.90f)

typedef struct {
    bool     active;
    float    velocity;
    float    phase;             /* サイン位相 (0.25 開始) */
    float    frequency;         /* 現在周波数 (表示用) */
    float    start_frequency;   /* 開始周波数 (ベロシティ 3 レイヤー) */
    float    target_frequency;  /* 着地周波数 48Hz */
    float    env_level;         /* 指数減衰エンベロープ */
    float    decay_coeff;       /* note-on 時に算出 */
    uint32_t samples_rendered;
    uint32_t total_samples;
    float    pitch_env;         /* ピッチスイープ包絡 (1.0 -> 0.0) */
    float    pitch_decay_coeff; /* note-on 時に算出 */
    float    click_lp;          /* ビータークリック平滑化 (shim_lp 相当) */
} SubKickVoice;

typedef struct {
    SubKickVoice voices[SUB_KICK_MAX_VOICES];
    uint32_t     noise_seed;    /* 本 Core 専用 PRNG (C4 と非共有) */
    float        volume;        /* CC#7 (ch10) 反映値 */
} SubKickEngine;

/* OPTIMIZATION_MEMO #5: ヘッダinline(コード重複) -> 外部リンクで1KB節約 (Sub2/3共有) */
void sub_kick_init(SubKickEngine *k);
void sub_kick_note_on(SubKickEngine *k, float velocity);
void sub_kick_all_notes_off(SubKickEngine *k);
void sub_kick_render(SubKickEngine *k, float *acc_l, float *acc_r, uint32_t frames);

#endif /* SUB_KICK_H_ */
