/**
 * @file midi_player.h
 * @brief リアルタイム MIDI シーケンス・プレイヤー
 * @details サンプル単位の高精度タイムスタンプ同期、16chマルチトラックMIDI再生制御
 */

#ifndef MIDI_PLAYER_H_
#define MIDI_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>
#include "synth_engine.h"
#include "midi_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SynthEngine *engine;
    const MidiSong *current_song;
    uint32_t current_event_idx;
    uint32_t current_sample_time;
    bool is_playing;
    bool is_paused;
} MidiPlayer;

/**
 * @brief MIDIプレイヤーの初期化
 */
void midi_player_init(MidiPlayer *player, SynthEngine *engine);

/**
 * @brief MIDIソングのセットと再生開始（頭出し）
 */
void midi_player_play(MidiPlayer *player, const MidiSong *song);

/**
 * @brief 再生の一時停止
 */
void midi_player_pause(MidiPlayer *player);

/**
 * @brief 一時停止からの再開
 */
void midi_player_resume(MidiPlayer *player);

/**
 * @brief 再生の完全停止
 */
void midi_player_stop(MidiPlayer *player);

/**
 * @brief サンプル数単位でのMIDIイベント処理・更新
 * @param player プレイヤー構造体へのポインタ
 * @param frames 進めるフレーム数（サンプル数）
 * @return 楽曲が終了した場合は false、再生中の場合は true
 */
bool midi_player_tick_frames(MidiPlayer *player, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_PLAYER_H_ */
