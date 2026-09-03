/**
 * @file sd_player.h
 * @brief SD MIDI レーン: 状態管理とイベント配信 (Main/ASMP 経路の吸収)
 * @details synth_main.c から分離。SD レーンの曲構と ASMP/ローカルエンジンへの
 *          MIDI イベント変換をカプセル化する。Main は本モジュール経由で
 *          状態 (loaded/active/index/time/event_idx) を操作する。
 */

#ifndef SD_PLAYER_H_
#define SD_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>
#include "midi_parser.h"
#include "sd_midi.h"
#include "synth_engine.h"

#if !defined(SYNTH_MULTICORE) || !SYNTH_MULTICORE
#define SD_PLAYER_ASMP 0
#else
#define SD_PLAYER_ASMP 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SD_STREAM_RING_SIZE (2048u)
#define SD_STREAM_RING_MASK (SD_STREAM_RING_SIZE - 1u)

#if defined(__GNUC__) || defined(__clang__)
#  define SD_RING_BARRIER() __sync_synchronize()
#elif defined(_MSC_VER)
#  include <intrin.h>
#  define SD_RING_BARRIER() _ReadWriteBarrier()
#else
#  define SD_RING_BARRIER()
#endif

/**
 * @brief SD MIDI ストリーミング用 ロックフリー SPSC リングバッファ
 *        Producer (sd_loader) のみ head を更新
 *        Consumer (Main RT ループ) のみ tail を更新
 *        count を廃止し head/tail のモジュロ差分で競合ゼロのオンライン計算を行う
 */
typedef struct {
    MidiEvent         events[SD_STREAM_RING_SIZE];
    volatile uint32_t head;     /**< 書込位置 (Producer: sd_loader のみ書き込み) */
    volatile uint32_t tail;     /**< 読取位置 (Consumer: Main RT ループのみ書き込み) */
    volatile bool     is_eof;   /**< SMF 終端到達フラグ (Producer が設定) */
} SdMidiRingBuffer;

void     sd_ring_init(SdMidiRingBuffer *rb);
void     sd_ring_reset(SdMidiRingBuffer *rb);
uint32_t sd_ring_available(const SdMidiRingBuffer *rb);
uint32_t sd_ring_free_space(const SdMidiRingBuffer *rb);
bool     sd_ring_push(SdMidiRingBuffer *rb, const MidiEvent *ev);
uint32_t sd_ring_push_batch(SdMidiRingBuffer *rb, const MidiEvent *evs, uint32_t num);
bool     sd_ring_peek(const SdMidiRingBuffer *rb, MidiEvent *ev);
bool     sd_ring_peek_at(const SdMidiRingBuffer *rb, uint32_t offset, MidiEvent *ev);
bool     sd_ring_pop(SdMidiRingBuffer *rb, MidiEvent *ev);
uint32_t sd_ring_pop_batch(SdMidiRingBuffer *rb, uint32_t num);

/**
 * @brief SD レーンの状態 (nontextual: Main がライタ、Sub1 からの再同期フックのみ参照)
 */
typedef struct {
    MidiSong    song;           /**< ロード済み曲メタデータ */
    uint64_t    time;           /**< 曲内経過サンプル */
    uint32_t    event_idx;      /**< 互換用: 累計消費イベント数 */
    uint32_t    index;          /**< 現在再生中のファイルインデックス */
    bool        loaded;
    bool        active;
    bool        req_inflight;   /**< ローダーへ要求中 (結果未回収) */
    uint32_t    rescan_acc_us;  /**< リスキャン用累積時間 */
    uint32_t    last_ts;        /**< 配信済み最終イベント ts (完奏テール判定用。
                                 * ストリーミングでは reader.total が生 tick 最大
                                 * (除外CC含む) で過大になるため、実配信の最終 ts
                                 * +1sec で完奏を判定する。バッチと同式) */
} SdLaneState;

/* Main 側ストリーミングリングバッファ (1024 events = 12KB) */
extern SdMidiRingBuffer g_sd_player_ring;

#define SD_PLAYER_POOL_SIZE  SD_STREAM_RING_SIZE
extern MidiEvent g_sd_player_pool[SD_PLAYER_POOL_SIZE];

/* 楽曲の初期テンポを共有メモリへ公開 (Sub5 の BPM 同期ディレイ用) */
void sd_publish_tempo(void *mgr, const MidiSong *song);

/* MIDI イベント 1 件を分散/ローカルの適切な経路へ配信する。
 * @param sample_offset チャンク先頭からのサンプルオフセット (0..511)
 * @param source MIDI_SOURCE_SMF / MIDI_SOURCE_LIVE
 * @return true: 配信完了 (または対応外で消費扱い) / false: キュー満杯で未配信 */
bool sd_deliver_event(void *mgr, SynthEngine *engine, bool use_asmp, const MidiEvent *ev,
                      uint16_t sample_offset, uint8_t source);

/* 完奏/手動スキップ共通: 現行 SD 曲を解放しレーンを空載状態へ戻す */
void sd_lane_release_current(SdLaneState *lane, SynthEngine *engine, bool send_asmp_all_off,
                             void *mgr);

#ifdef __cplusplus
}
#endif

#endif /* SD_PLAYER_H_ */
