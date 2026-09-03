/**
 * @file midi_player.c
 * @brief リアルタイム MIDI シーケンス・プレイヤー実装
 */

#include <string.h>
#include <stdio.h>
#include "midi_player.h"

void midi_player_init(MidiPlayer *player, SynthEngine *engine)
{
    if (!player) return;

    memset(player, 0, sizeof(MidiPlayer));
    player->engine = engine;
    player->current_song = NULL;
    player->current_event_idx = 0;
    player->current_sample_time = 0;
    player->is_playing = false;
    player->is_paused = false;
}

void midi_player_play(MidiPlayer *player, const MidiSong *song)
{
    if (!player || !song || song->event_count == 0) return;

    if (player->engine) {
        synth_engine_all_notes_off(player->engine);
        synth_engine_reset_effects(player->engine);
    }

    player->current_song = song;
    player->current_event_idx = 0;
    player->current_sample_time = 0;
    player->is_playing = true;
    player->is_paused = false;
}

void midi_player_pause(MidiPlayer *player)
{
    if (!player || !player->is_playing) return;

    if (player->engine) {
        synth_engine_all_notes_off(player->engine);
    }
    player->is_playing = false;
    player->is_paused = true;
}

void midi_player_resume(MidiPlayer *player)
{
    if (!player || !player->is_paused || !player->current_song) return;

    player->is_playing = true;
    player->is_paused = false;
}

void midi_player_stop(MidiPlayer *player)
{
    if (!player) return;

    if (player->engine) {
        synth_engine_all_notes_off(player->engine);
        synth_engine_reset_effects(player->engine);
    }
    player->is_playing = false;
    player->is_paused = false;
    player->current_event_idx = 0;
    player->current_sample_time = 0;
}

bool midi_player_tick_frames(MidiPlayer *player, uint32_t frames)
{
    if (!player || !player->is_playing || !player->current_song || !player->engine) {
        return false;
    }

    const MidiSong *song = player->current_song;
    uint32_t target_sample = player->current_sample_time + frames;

    /* 現在のフレーム期間内の全MIDIイベントを発行 */
    while (player->current_event_idx < song->event_count) {
        const MidiEvent *ev = &song->events[player->current_event_idx];
        if (ev->timestamp_samples > target_sample) {
            break; /* まだ発音時刻に達していない */
        }

        /* MIDI メッセージのディスパッチ */
        switch (ev->type) {
            case MIDI_STATUS_NOTE_ON:
                synth_engine_channel_note_on(player->engine, ev->channel, ev->data1, (float)ev->data2 / 127.0f);
                break;

            case MIDI_STATUS_NOTE_OFF:
                synth_engine_channel_note_off(player->engine, ev->channel, ev->data1);
                break;

            case MIDI_STATUS_PROGRAM_CHANGE:
                synth_engine_program_change(player->engine, ev->channel, ev->data1);
                break;

            case MIDI_STATUS_CONTROL_CHANGE:
                synth_engine_control_change(player->engine, ev->channel, ev->data1, ev->data2);
                break;

            case MIDI_STATUS_PITCH_BEND: {
                int16_t bend = (int16_t)(((uint16_t)ev->data2 << 7) | (uint16_t)ev->data1) - 8192;
                synth_engine_pitch_bend(player->engine, ev->channel, bend);
                break;
            }

            default:
                break;
        }

        player->current_event_idx++;
    }

    player->current_sample_time = target_sample;

    /* 全イベント終了判定 (+1秒の残響余韻) */
    if (player->current_event_idx >= song->event_count && player->current_sample_time >= song->total_samples) {
        midi_player_stop(player);
        return false; /* 完奏 */
    }

    return true;
}
