/**
 * @file synth_controller.h
 * @brief JoyStick Shield を用いたシンセサイザー・SD MIDI プレイヤーコントローラー
 * @details SD MIDI 再生操作 (曲送り/音量) および演奏モードの管理
 */

#ifndef SYNTH_CONTROLLER_H_
#define SYNTH_CONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>
#include "synth_engine.h"
#include "joystick_shield.h"

struct AsmpManager_s;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 動作モード定義
 */
typedef enum {
    CTRL_MODE_JUKEBOX = 0,   /**< プレイヤーモード (SD MIDI 曲送り/音量操作) */
    CTRL_MODE_PERFORMANCE  /**< 演奏モード (十字ボタンで音階発音) */
} CtrlMode;

/**
 * @brief 演奏モード時のスマート・スティック・ゾーン
 */
typedef enum {
    PERF_ZONE_CENTER = 0,   /**< ニュートラル: ド・レ・ミ・ファ (C, D, E, F) */
    PERF_ZONE_HIGH,         /**< 右 or 上:    ソ・ラ・シ・高ド (G, A, B, C+) */
    PERF_ZONE_ACCIDENTAL,   /**< 左 (黒鍵):   ド#・レ#・ファ#・ソ# (C#, D#, F#, G#) */
    PERF_ZONE_LOW           /**< 下 (低音):   低ソ・低ラ・低シ・ド (G-, A-, B-, C) */
} PerfStickZone;

#define EF_COMBO_WINDOW_FRAMES (4)

/** アルペジオ: K 長押し判定フレーム数 (~0.6s @93fps) */
#define ARP_K_HOLD_FRAMES (56)
/** アルペジオ 1 ステップのフレーム数 (≈130ms ≒ 16 分音符 @115BPM) */
#define ARP_STEP_FRAMES   (12)
 /**< E+F 同時押し検出猶予フレーム数 (約40ms) */

/**
 * @brief シンセ・SD MIDI プレイヤーコントローラー構造体
 */
typedef struct {
    SynthEngine *engine;
    struct AsmpManager_s *asmp;  /**< 非NULL: 6コア分散モード (SubCore へコマンド転送) */

    CtrlMode mode;               /**< 現在の操作モード */

    /* プレイヤー管理パラメータ */
    float volume;            /**< マスター音量 (0.0f 〜 1.0f) */
    int8_t stick_x_zone;     /**< 前回のスティック X軸ゾーン (-1/0/+1, エッジ検出用) */
    bool combo_latch;        /**< E+F 同時押しのホールド中フラグ (連続切替防止) */
    uint8_t ef_pending_btn;  /**< コンボ判定待ちの単押しボタン (BTN_MASK_E/F, 0=なし) */
    uint8_t ef_pending_ticks;/**< コンボ判定待ち経過フレーム数 */
    bool sd_active;          /**< Main が SD MIDI を保持中 (曲送り操作の可否判定用) */
    int8_t sd_skip_request;  /**< SD 曲送り要求 (+1=次曲 / -1=前曲 / 0=なし)。Main が消費して 0 へ戻す */
    bool sd_play_pause_request; /**< SD 再生/一時停止トグル要求。Main が消費して false へ戻す */

    /* 演奏モード状態 (スマート・スティック演奏システム) */
    WaveType perf_wave;      /**< 発音波形 (F ボタンで循環) */
    int8_t perf_octave;      /**< 基準オクターブ (E ボタンで 3→4→5 循環) */
    PerfStickZone perf_zone; /**< 現在のスティックスケールゾーン (CENTER/HIGH/ACCIDENTAL/LOW) */
    uint8_t perf_held_mask;  /**< 保持中の音階ボタン (bit0=A, 1=B, 2=C, 3=D) */
    uint8_t perf_note[4];    /**< 各ボタンが発音中の MIDI ノート番号 */

    /* アルペジエータ (K 長押し ~0.6s でトグル。ch1/SubCore3 で発音) */
    bool     arp_enabled;    /**< アルペジオ ON/OFF */
    int8_t   arp_dir;        /**< 走査方向 (+1/-1, 上下バウンド) */
    uint16_t arp_step_idx;   /**< 現在ステップ */
    uint8_t  arp_last_note;  /**< 直前ステップの発音中ノート (0=なし) */
    uint16_t arp_frame_cnt;  /**< ステップ用フレームカウンタ (ローカルモード) */
    uint32_t arp_last_seen;  /**< ASMP モード: 最後に処理した seq_step16 */
    uint16_t k_hold_frames;  /**< K 押し込み継続フレーム数 */
} SynthController;

/**
 * @brief コントローラーの初期化
 * @param ctrl コントローラー構造体ポインタ
 * @param engine シンセサイザーエンジンへのポインタ
 */
void synth_controller_init(SynthController *ctrl, SynthEngine *engine);

/**
 * @brief ASMP 6コア分散モードへの切り替え (操作コマンドを SubCore へ転送)
 * @param ctrl コントローラー構造体ポインタ
 * @param mgr ASMP マネージャー (NULL でローカルエンジン制御へ戻す)
 */
void synth_controller_bind_asmp(SynthController *ctrl, struct AsmpManager_s *mgr);

/**
 * @brief マスター音量を設定 (0.0〜1.0、CLI 引数やストick Y からの反映用)
 */
void synth_controller_set_volume(SynthController *ctrl, float volume);

/**
 * @brief SD MIDI レーンの状態を Main 側から通知する
 *        (true の間のみプレイヤーモードでの曲送り操作が有効)
 */
void synth_controller_set_sd_active(SynthController *ctrl, bool active);

/**
 * @brief サブコア再起動後の状態再同期
 *        (全消音 + Main 側が保持するマスター音量を SubCore 5 へ再適用)
 */
void synth_controller_resync_asmp(SynthController *ctrl);

/**
 * @brief 現在の動作モード取得 (UI 表示等から参照)
 */
CtrlMode synth_controller_mode(const SynthController *ctrl);

/**
 * @brief ジョイスティック入力イベントの処理と反映 (プレイヤー/演奏操作)
 * @param ctrl コントローラー構造体ポインタ
 * @param state ジョイスティック・ボタンの状態
 */
void synth_controller_update(SynthController *ctrl, const JoystickState *state);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_CONTROLLER_H_ */
