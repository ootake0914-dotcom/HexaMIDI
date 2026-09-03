/**
 * @file sequencer.h
 * @brief 音楽シーケンサーモジュール
 * @details 音符シーケンスの高精度再生制御およびプリセット楽曲連携
 */

#ifndef SEQUENCER_H_
#define SEQUENCER_H_

#include <stdint.h>
#include <stdbool.h>
#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 特殊ノート定義 */
#define NOTE_REST (0xFF) /* 休符 */
#define NOTE_NONE (0x00) /* 無効ノート (harmony 拡張フィールドの未使用判定に使用) */

/* GM プログラム「未指定」センチネル
 * ※ MIDI の Program 0 = Acoustic Grand Piano は「有効な指定値」であるため、
 *   0 を未指定判定に使うとピアノ音色が選択できなくなる */
#define GM_PROGRAM_DEFAULT (0xFFu)

/**
 * @brief 1音符（ノートイベント）の定義
 */
typedef struct {
    uint8_t note;        /**< MIDIノート番号 (NOTE_RESTで休符) */
    uint16_t duration_ms;/**< 発音時間 (ミリ秒) */
    uint16_t gate_ms;    /**< ゲート時間（実発音時間、余りは休符） */
    float velocity;      /**< ベロシティ (0.0 ~ 1.0) */
    WaveType wave_type;  /**< 使用波形 */

    /* --- v2 拡張: マルチパート編成用 (末尾追加により旧データと互換) --- */
    uint8_t harmony1;    /**< 同時発音ノート1 (NOTE_NONE/NOTE_RESTで無効) */
    uint8_t harmony2;    /**< 同時発音ノート2 (NOTE_NONE/NOTE_RESTで無効) */
    uint8_t channel;     /**< MIDIチャンネル (0=リード, 1=ベース, 2=コード, 9=ドラム) */
} NoteEvent;

/**
 * @brief 楽曲トラック定義
 */
typedef struct {
    const char *title;
    const NoteEvent *events;
    uint32_t event_count;
    uint16_t bpm;        /**< テンポ (BPM) */
    bool loop;           /**< ループ再生フラグ */

    /* --- v2 拡張: 6コア分散エンジン向けマルチパートレイヤー --- */
    const NoteEvent *bass_events;   /**< ベースライン (ch1, NULL可) */
    uint32_t bass_event_count;
    const NoteEvent *chord_events;  /**< コード/パッド (ch2, NULL可) */
    uint32_t chord_event_count;
    const NoteEvent *drum_events;   /**< GM ドラム (ch9, NULL可) */
    uint32_t drum_event_count;
    uint8_t lead_program;           /**< ASMP SubCore 用 GM プログラム (ch0) */
    uint8_t bass_program;           /**< ASMP SubCore 用 GM プログラム (ch1) */
    uint8_t chord_program;          /**< ASMP SubCore 用 GM プログラム (ch2) */
} Track;

/** シーケンサー同時再生レイヤー数 (Lead / Bass / Chord / Drum) */
#define SEQ_NUM_LAYERS      (4)
/** 1レイヤーあたりの最大同時発音数 (メイン + ハーモニー2音 + 余裕) */
#define SEQ_MAX_ACTIVE_NOTES (3)

/**
 * @brief E/F 同時押し判定ウィンドウ (フレーム数)。
 *        人間の同時押しは数ms〜数十msずれるため、この期間内の単押しは
 *        個別アクションを遅延させてコンボ判定に回す。
 *        メインループ周期 ~10.7ms 前提で 4 フレーム ≒ 40ms。
 */
#define EF_COMBO_WINDOW_FRAMES (4)

/**
 * @brief シーケンサーの状態
 */
typedef struct {
    SynthEngine *engine;
    const Track *current_track;

    /* レイヤー別再生状態 */
    const NoteEvent *layer_events[SEQ_NUM_LAYERS];
    uint32_t layer_counts[SEQ_NUM_LAYERS];
    uint32_t layer_channels[SEQ_NUM_LAYERS];
    uint32_t layer_event_idx[SEQ_NUM_LAYERS];
    uint32_t layer_elapsed_samples[SEQ_NUM_LAYERS];
    bool     layer_finished[SEQ_NUM_LAYERS];
    uint8_t  active_notes[SEQ_NUM_LAYERS][SEQ_MAX_ACTIVE_NOTES];

    bool is_playing;
    bool is_paused;
    bool is_note_active;         /**< 互換フラグ (リードレイヤー発音中) */
    uint8_t current_playing_note;/**< 互換フィールド (リードレイヤーの現在音) */
} Sequencer;

/**
 * @brief シーケンサーの初期化
 * @param seq シーケンサー構造体へのポインタ
 * @param engine 連携するシンセサイザーエンジンへのポインタ
 */
void sequencer_init(Sequencer *seq, SynthEngine *engine);

/**
 * @brief 楽曲トラックのセットと再生開始（頭出し）
 * @param seq シーケンサー構造体へのポインタ
 * @param track 再生するトラックへのポインタ
 */
void sequencer_play_track(Sequencer *seq, const Track *track);

/**
 * @brief 再生の一時停止（現在位置を保持）
 * @param seq シーケンサー構造体へのポインタ
 */
void sequencer_pause(Sequencer *seq);

/**
 * @brief 一時停止からの再開
 * @param seq シーケンサー構造体へのポインタ
 */
void sequencer_resume(Sequencer *seq);

/**
 * @brief 再生の完全停止（位置リセット）
 * @param seq シーケンサー構造体へのポインタ
 */
void sequencer_stop(Sequencer *seq);

/**
 * @brief フレーム（サンプル）単位でのシーケンサー状態の更新（高精度・ジッターフリー）
 * @param seq シーケンサー構造体へのポインタ
 * @param frames 経過フレーム数（サンプル数）
 * @param sample_rate サンプリングレート (Hz)
 * @return 曲が終了した場合は false、再生中の場合は true
 */
bool sequencer_tick_frames(Sequencer *seq, uint32_t frames, uint32_t sample_rate);

/**
 * @brief 時間経過によるシーケンサー状態の更新 (ミリ秒指定API)
 * @param seq シーケンサー構造体へのポインタ
 * @param delta_ms 経過時間 (ミリ秒)
 * @return 曲が終了した場合は false、再生中の場合は true
 */
bool sequencer_tick(Sequencer *seq, uint32_t delta_ms);

/**
 * @brief プリセット楽曲一覧の取得（preset_songs モジュールと連携）
 * @param count 楽曲数を格納するポインタ
 * @return プリセット楽曲配列へのポインタ
 */
const Track* sequencer_get_preset_tracks(uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* SEQUENCER_H_ */
