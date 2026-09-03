/**
 * @file midi_demo_songs.h
 * @brief 内蔵 SMF (Standard MIDI File) デモ楽曲データヘッダー
 */

#ifndef MIDI_DEMO_SONGS_H_
#define MIDI_DEMO_SONGS_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;
    const uint8_t *smf_data;
    size_t smf_size;
} MidiDemoTrack;

/**
 * @brief プリセット MIDI 楽曲一覧の取得
 * @param count 楽曲数を格納するポインタ
 * @return 楽曲配列へのポインタ
 */
const MidiDemoTrack* midi_demo_songs_get_all(uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_DEMO_SONGS_H_ */
