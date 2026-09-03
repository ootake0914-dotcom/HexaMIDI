/**
 * @file preset_songs.c
 * @brief プリセット楽曲データ定義（超軽量版・テスト用1曲のみ）
 * @details ASMP 6コアメモリタイル節約のため、大容量譜面を排除し
 *          最小限のテストパターンのみを保持する。
 *          本格的な楽曲演奏は SD カード MIDI を使用する。
 */

#include <stddef.h>
#include "preset_songs.h"

/* 4小節のシンプルなテストパターン (BPM 120) */
static const NoteEvent s_test_lead_events[] = {
    { 60, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* C4 */
    { 64, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* E4 */
    { 67, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* G4 */
    { 72, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* C5 */
    { 71, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* B4 */
    { 67, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* G4 */
    { 65, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* F4 */
    { 64, 500, 450, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* E4 */
};

static const NoteEvent s_test_bass_events[] = {
    { 36, 1000, 900, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* C2 */
    { 43, 1000, 900, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* G2 */
    { 41, 1000, 900, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* F2 */
    { 36, 1000, 900, 0.90f, WAVE_SAWTOOTH, NOTE_REST, NOTE_REST, 0 }, /* C2 */
};

static const NoteEvent s_test_drum_events[] = {
    { 36, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Kick */
    { 38, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Snare */
    { 36, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Kick */
    { 38, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Snare */
    { 36, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Kick */
    { 38, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Snare */
    { 36, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Kick */
    { 38, 500, 200, 0.90f, WAVE_SINE, NOTE_REST, NOTE_REST, 0 }, /* Snare */
};

static const Track s_preset_tracks[] = {
    {
        .title = "HexaMIDI Test Tone (C-Major)",
        .events = s_test_lead_events,
        .event_count = sizeof(s_test_lead_events) / sizeof(NoteEvent),
        .bpm = 120,
        .loop = true,
        .bass_events = s_test_bass_events,
        .bass_event_count = sizeof(s_test_bass_events) / sizeof(NoteEvent),
        .chord_events = NULL,
        .chord_event_count = 0,
        .drum_events = s_test_drum_events,
        .drum_event_count = sizeof(s_test_drum_events) / sizeof(NoteEvent),
        .lead_program = 80,
        .bass_program = 38,
        .chord_program = 0
    }
};

const Track* preset_songs_get_all(uint32_t *count)
{
    if (count) {
        *count = sizeof(s_preset_tracks) / sizeof(Track);
    }
    return s_preset_tracks;
}

const Track* preset_songs_get_by_index(uint32_t index)
{
    uint32_t count = 0;
    preset_songs_get_all(&count);
    if (index >= count) {
        return NULL;
    }
    return &s_preset_tracks[index];
}

uint32_t preset_songs_get_count(void)
{
    uint32_t count = 0;
    preset_songs_get_all(&count);
    return count;
}
