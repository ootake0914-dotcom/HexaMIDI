/**
 * @file sequencer.c
 * @brief 音楽シーケンサー実装 (マルチレイヤー・ポリフォニック対応)
 * @details Lead/Bass/Chord/Drum の 4 レイヤーをサンプル単位で並列進行。
 *          ハーモニー同時発音、チャンネル別音色ルーティング、ジッターフリー再生に対応。
 */

#include <string.h>
#include <stdio.h>
#include "sequencer.h"
#include "preset_songs.h"

/* レイヤーインデックス → MIDI チャンネル */
static const uint32_t s_layer_channel[SEQ_NUM_LAYERS] = {
    0,  /* Layer 0: Lead   */
    1,  /* Layer 1: Bass   */
    2,  /* Layer 2: Chord  */
    9   /* Layer 3: Drums  */
};

static uint32_t ms_to_frames(uint32_t ms, uint32_t sample_rate)
{
    return (uint32_t)(((uint64_t)ms * sample_rate) / 1000);
}

static bool is_playable_note(uint8_t note)
{
    return (note != NOTE_REST && note != NOTE_NONE);
}

/**
 * @brief レイヤーの発音中ノートをすべて消音
 */
static void layer_all_notes_off(Sequencer *seq, uint32_t layer)
{
    SynthEngine *eng = seq->engine;
    if (!eng) return;

    uint32_t ch = seq->layer_channels[layer];
    for (int i = 0; i < SEQ_MAX_ACTIVE_NOTES; i++) {
        uint8_t n = seq->active_notes[layer][i];
        if (n != NOTE_NONE) {
            synth_engine_channel_note_off(eng, (uint8_t)ch, n);
            seq->active_notes[layer][i] = NOTE_NONE;
        }
    }
}

/**
 * @brief イベントのノート + ハーモニーを同時発音
 */
static void layer_trigger_event(Sequencer *seq, uint32_t layer, const NoteEvent *ev)
{
    SynthEngine *eng = seq->engine;
    if (!eng) return;

    uint32_t ch = seq->layer_channels[layer];
    uint8_t notes[SEQ_MAX_ACTIVE_NOTES];
    int count = 0;

    notes[count++] = ev->note;
    if (is_playable_note(ev->harmony1) && count < SEQ_MAX_ACTIVE_NOTES) notes[count++] = ev->harmony1;
    if (is_playable_note(ev->harmony2) && count < SEQ_MAX_ACTIVE_NOTES) notes[count++] = ev->harmony2;

    int active_idx = 0;
    for (int i = 0; i < count; i++) {
        if (!is_playable_note(notes[i])) continue;
        if (ch == 9) {
            /* ドラムレイヤー: GM ノートマッピング優先 */
            synth_engine_channel_note_on(eng, (uint8_t)ch, notes[i], ev->velocity);
        } else {
            synth_engine_channel_note_on_w(eng, (uint8_t)ch, notes[i], ev->velocity, ev->wave_type);
        }
        if (active_idx < SEQ_MAX_ACTIVE_NOTES) {
            seq->active_notes[layer][active_idx++] = notes[i];
        }
    }
    while (active_idx < SEQ_MAX_ACTIVE_NOTES) {
        seq->active_notes[layer][active_idx++] = NOTE_NONE;
    }
}

/**
 * @brief 単一レイヤーを frames サンプル分進行させる
 * @return レイヤーが完走したかどうか
 */
static bool layer_tick(Sequencer *seq, uint32_t layer, uint32_t frames, uint32_t sample_rate)
{
    const Track *tr = seq->current_track;
    if (seq->layer_finished[layer]) return true;
    if (!seq->layer_events[layer] || seq->layer_counts[layer] == 0) {
        seq->layer_finished[layer] = true;
        return true;
    }

    seq->layer_elapsed_samples[layer] += frames;

    while (true) {
        uint32_t idx = seq->layer_event_idx[layer];
        if (idx >= seq->layer_counts[layer]) {
            if (tr->loop) {
                /* ループバック: 先頭イベントを明示的に発音しないと
                 * 2 周目以降の曲頭の音が無音スキップされる */
                layer_all_notes_off(seq, layer);
                seq->layer_event_idx[layer] = 0;
                layer_trigger_event(seq, layer, &seq->layer_events[layer][0]);
                continue;
            }
            layer_all_notes_off(seq, layer);
            seq->layer_finished[layer] = true;
            return true;
        }

        const NoteEvent *ev = &seq->layer_events[layer][idx];
        uint32_t duration_frames = ms_to_frames(ev->duration_ms, sample_rate);
        uint32_t gate_frames = ms_to_frames(ev->gate_ms, sample_rate);
        if (duration_frames == 0) {
            /* duration 0 のイベントのみで構成されたループトラックで
             * 時間が進まず while が永久周回するのを防ぐ下限ガード */
            duration_frames = 1;
        }

        /* ゲート経過: 発音中ノートを消音 (スタッカート) */
        if (seq->active_notes[layer][0] != NOTE_NONE &&
            seq->layer_elapsed_samples[layer] >= gate_frames) {
            layer_all_notes_off(seq, layer);
        }

        if (seq->layer_elapsed_samples[layer] >= duration_frames) {
            /* 余剰サンプルを引き継いで累積ジッターを防止 */
            seq->layer_elapsed_samples[layer] -= duration_frames;
            seq->layer_event_idx[layer]++;

            layer_all_notes_off(seq, layer);

            uint32_t next_idx = seq->layer_event_idx[layer];
            if (next_idx >= seq->layer_counts[layer]) {
                continue; /* 次の while ループで loop 判定/完走処理 */
            }
            layer_trigger_event(seq, layer, &seq->layer_events[layer][next_idx]);
        } else {
            break;
        }
    }

    return false;
}

void sequencer_init(Sequencer *seq, SynthEngine *engine)
{
    if (!seq) return;
    memset(seq, 0, sizeof(Sequencer));
    seq->engine = engine;
    seq->is_playing = false;
    seq->is_paused = false;
    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        for (int i = 0; i < SEQ_MAX_ACTIVE_NOTES; i++) {
            seq->active_notes[l][i] = NOTE_NONE;
        }
    }
}

void sequencer_play_track(Sequencer *seq, const Track *track)
{
    if (!seq || !track || track->event_count == 0) return;

    if (seq->engine) {
        synth_engine_all_notes_off(seq->engine);

        /* チャンネル初期プログラム設定 (ASMP サブコアの音色選択にも使用)
         * ※ Program 0 = Acoustic Grand Piano は有効な指定値 (0 を未指定指定扱いしない) */
        if (track->lead_program != GM_PROGRAM_DEFAULT) {
            synth_engine_program_change(seq->engine, 0, track->lead_program);
        } else {
            synth_engine_program_change(seq->engine, 0, 81); /* デフォルト: Lead 2 (sawtooth) */
        }
        if (track->bass_program != GM_PROGRAM_DEFAULT) {
            synth_engine_program_change(seq->engine, 1, track->bass_program);
        } else {
            synth_engine_program_change(seq->engine, 1, 36); /* デフォルト: Slap Bass */
        }
        if (track->chord_program != GM_PROGRAM_DEFAULT) {
            synth_engine_program_change(seq->engine, 2, track->chord_program);
        }
    }

    seq->current_track = track;
    seq->is_playing = true;
    seq->is_paused = false;

    const NoteEvent *events[SEQ_NUM_LAYERS];
    uint32_t counts[SEQ_NUM_LAYERS];

    events[0] = track->events;       counts[0] = track->event_count;
    events[1] = track->bass_events;  counts[1] = track->bass_event_count;
    events[2] = track->chord_events; counts[2] = track->chord_event_count;
    events[3] = track->drum_events;  counts[3] = track->drum_event_count;

    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        seq->layer_events[l] = events[l];
        seq->layer_counts[l] = counts[l];
        seq->layer_channels[l] = s_layer_channel[l];
        seq->layer_event_idx[l] = 0;
        seq->layer_elapsed_samples[l] = 0;
        seq->layer_finished[l] = false;
        for (int i = 0; i < SEQ_MAX_ACTIVE_NOTES; i++) {
            seq->active_notes[l][i] = NOTE_NONE;
        }
        if (events[l] && counts[l] > 0) {
            layer_trigger_event(seq, l, &events[l][0]);
        } else {
            seq->layer_finished[l] = true;
        }
    }

    /* 互換フィールド更新 */
    seq->is_note_active = (seq->active_notes[0][0] != NOTE_NONE);
    seq->current_playing_note = seq->is_note_active ? track->events[0].note : NOTE_REST;
}

void sequencer_pause(Sequencer *seq)
{
    if (!seq || !seq->is_playing || seq->is_paused) return;

    if (seq->engine) {
        synth_engine_all_notes_off(seq->engine);
    }
    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        for (int i = 0; i < SEQ_MAX_ACTIVE_NOTES; i++) {
            seq->active_notes[l][i] = NOTE_NONE;
        }
    }
    seq->is_paused = true;
    seq->is_note_active = false;
}

void sequencer_resume(Sequencer *seq)
{
    if (!seq || !seq->is_playing || !seq->is_paused || !seq->current_track) return;

    seq->is_paused = false;

    /* 各レイヤーの現在イベントがゲート内なら再発音 */
    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        if (seq->layer_finished[l] || !seq->layer_events[l]) continue;
        uint32_t idx = seq->layer_event_idx[l];
        if (idx >= seq->layer_counts[l]) continue;

        const NoteEvent *ev = &seq->layer_events[l][idx];
        uint32_t gate_frames = ms_to_frames(ev->gate_ms, SYNTH_SAMPLE_RATE);
        if (seq->layer_elapsed_samples[l] < gate_frames) {
            layer_trigger_event(seq, l, ev);
        }
    }
    seq->is_note_active = (seq->active_notes[0][0] != NOTE_NONE);
}

void sequencer_stop(Sequencer *seq)
{
    if (!seq) return;

    if (seq->engine) {
        synth_engine_all_notes_off(seq->engine);
    }
    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        seq->layer_event_idx[l] = 0;
        seq->layer_elapsed_samples[l] = 0;
        seq->layer_finished[l] = false;
        for (int i = 0; i < SEQ_MAX_ACTIVE_NOTES; i++) {
            seq->active_notes[l][i] = NOTE_NONE;
        }
    }
    seq->is_playing = false;
    seq->is_paused = false;
    seq->is_note_active = false;
    seq->current_playing_note = NOTE_REST;
}

bool sequencer_tick_frames(Sequencer *seq, uint32_t frames, uint32_t sample_rate)
{
    if (!seq || !seq->is_playing || seq->is_paused || !seq->current_track || sample_rate == 0) {
        return seq ? seq->is_playing : false;
    }

    bool all_finished = true;
    for (uint32_t l = 0; l < SEQ_NUM_LAYERS; l++) {
        if (!layer_tick(seq, l, frames, sample_rate)) {
            all_finished = false;
        }
    }

    /* 互換フィールド更新 */
    seq->is_note_active = (seq->active_notes[0][0] != NOTE_NONE);
    if (seq->is_note_active && seq->layer_events[0] && seq->layer_event_idx[0] < seq->layer_counts[0]) {
        seq->current_playing_note =
            seq->layer_events[0][seq->layer_event_idx[0]].note;
    } else {
        seq->current_playing_note = NOTE_REST;
    }

    if (all_finished) {
        sequencer_stop(seq);
        return false; /* 楽曲完了 */
    }
    return true;
}

bool sequencer_tick(Sequencer *seq, uint32_t delta_ms)
{
    /* ミリ秒をサンプル数に換算して高精度tickを実行
     * (u64 中間値で delta_ms ≒ 89 秒超の乗算オーバーフローを防止) */
    uint32_t frames = (uint32_t)(((uint64_t)delta_ms * SYNTH_SAMPLE_RATE) / 1000);
    return sequencer_tick_frames(seq, frames > 0 ? frames : 1, SYNTH_SAMPLE_RATE);
}

const Track* sequencer_get_preset_tracks(uint32_t *count)
{
    return preset_songs_get_all(count);
}
