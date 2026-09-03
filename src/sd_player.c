/**
 * @file sd_player.c
 * @brief SD MIDI レーン: 状態管理とイベント配信実装
 * @details synth_main.c から分離。SD のイベントを ASMP (Sub1 キュー) または
 *          ローカルエンジンへ変換・投入する。状態は呼び出し側が持つ
 *          SdLaneState 構造体を更新する。
 */

#include <string.h>
#include "sd_player.h"
#include "sd_loader.h"
#include "asmp_manager.h"
#include "midi_parser.h"

/* Main 側ストリーミングリングバッファ */
SdMidiRingBuffer g_sd_player_ring;
MidiEvent g_sd_player_pool[SD_PLAYER_POOL_SIZE];

void sd_ring_init(SdMidiRingBuffer *rb)
{
    if (!rb) return;
    memset(rb, 0, sizeof(*rb));
}

void sd_ring_reset(SdMidiRingBuffer *rb)
{
    if (!rb) return;
    rb->is_eof = false;
    SD_RING_BARRIER();
    rb->head = 0;
    rb->tail = 0;
    SD_RING_BARRIER();
}

uint32_t sd_ring_available(const SdMidiRingBuffer *rb)
{
    if (!rb) return 0;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    return (head - tail) & SD_STREAM_RING_MASK;
}

uint32_t sd_ring_free_space(const SdMidiRingBuffer *rb)
{
    if (!rb) return 0;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    return (tail - head - 1u) & SD_STREAM_RING_MASK;
}

bool sd_ring_push(SdMidiRingBuffer *rb, const MidiEvent *ev)
{
    if (!rb || !ev) return false;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t next_head = (head + 1u) & SD_STREAM_RING_MASK;
    if (next_head == tail) return false;

    rb->events[head] = *ev;
    SD_RING_BARRIER();
    rb->head = next_head;
    return true;
}

uint32_t sd_ring_push_batch(SdMidiRingBuffer *rb, const MidiEvent *evs, uint32_t num)
{
    if (!rb || !evs || num == 0) return 0;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t free_sp = (tail - head - 1u) & SD_STREAM_RING_MASK;
    uint32_t to_push = (num < free_sp) ? num : free_sp;
    if (to_push == 0) return 0;

    for (uint32_t i = 0; i < to_push; i++) {
        rb->events[(head + i) & SD_STREAM_RING_MASK] = evs[i];
    }
    SD_RING_BARRIER();
    rb->head = (head + to_push) & SD_STREAM_RING_MASK;
    return to_push;
}

bool sd_ring_peek(const SdMidiRingBuffer *rb, MidiEvent *ev)
{
    if (!rb) return false;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    if (head == tail) return false;
    SD_RING_BARRIER();
    if (ev) *ev = rb->events[tail];
    return true;
}

bool sd_ring_peek_at(const SdMidiRingBuffer *rb, uint32_t offset, MidiEvent *ev)
{
    if (!rb) return false;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t avail = (head - tail) & SD_STREAM_RING_MASK;
    if (offset >= avail) return false;
    SD_RING_BARRIER();
    if (ev) {
        uint32_t idx = (tail + offset) & SD_STREAM_RING_MASK;
        *ev = rb->events[idx];
    }
    return true;
}

bool sd_ring_pop(SdMidiRingBuffer *rb, MidiEvent *ev)
{
    if (!rb) return false;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    if (head == tail) return false;
    SD_RING_BARRIER();
    if (ev) *ev = rb->events[tail];
    SD_RING_BARRIER();
    rb->tail = (tail + 1u) & SD_STREAM_RING_MASK;
    return true;
}

uint32_t sd_ring_pop_batch(SdMidiRingBuffer *rb, uint32_t num)
{
    if (!rb || num == 0) return 0;
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t avail = (head - tail) & SD_STREAM_RING_MASK;
    uint32_t to_pop = (num < avail) ? num : avail;
    if (to_pop == 0) return 0;

    SD_RING_BARRIER();
    rb->tail = (tail + to_pop) & SD_STREAM_RING_MASK;
    return to_pop;
}

/* ========================================================================= */
/* イベント変換                                                                */
/* ========================================================================= */

/* 14bit ピッチベンド (LSB/MSB) -> 中央原点 (-8192..+8191) へ変換 */
static int16_t midi_pitch_bend_centered(uint8_t lsb, uint8_t msb)
{
    return (int16_t)(((uint16_t)msb << 7) | (uint16_t)lsb) - 8192;
}

/* MIDI イベント -> ASMP パケット変換 (対応外は msg_type == ASMP_MSG_NONE) */
#if SD_PLAYER_ASMP
static AsmpPacket midi_event_to_asmp_packet(const MidiEvent *ev, uint16_t sample_offset, uint8_t source)
{
    AsmpPacket p;
    memset(&p, 0, sizeof(p));
    p.sample_offset = sample_offset;
    p.source = source;
    switch (ev->type) {
        case MIDI_STATUS_NOTE_ON:
            p.msg_type = ASMP_MSG_NOTE_ON;
            p.channel = ev->channel; p.data1 = ev->data1; p.data2 = ev->data2;
            break;
        case MIDI_STATUS_NOTE_OFF:
            p.msg_type = ASMP_MSG_NOTE_OFF;
            p.channel = ev->channel; p.data1 = ev->data1;
            break;
        case MIDI_STATUS_PROGRAM_CHANGE:
            p.msg_type = ASMP_MSG_PROGRAM_CHANGE;
            p.channel = ev->channel; p.data1 = ev->data1;
            break;
        case MIDI_STATUS_CONTROL_CHANGE:
            p.msg_type = ASMP_MSG_CONTROL_CHANGE;
            p.channel = ev->channel; p.data1 = ev->data1; p.data2 = ev->data2;
            break;
        case MIDI_STATUS_PITCH_BEND:
            p.msg_type = ASMP_MSG_PITCH_BEND;
            p.channel = ev->channel;
            p.param = (uint32_t)(int32_t)midi_pitch_bend_centered(ev->data1, ev->data2);
            break;
        case MIDI_STATUS_POLY_AFTERTOUCH:
            /* Poly Aftertouch: 現状音源未対応だがパーサーは保持、将来拡張で使用 */
            p.msg_type = ASMP_MSG_NONE;
            break;
        case MIDI_STATUS_CHANNEL_PRESSURE:
            p.msg_type = ASMP_MSG_NONE;
            break;
        default:
            break;
    }
    return p;
}
#endif

/* ========================================================================= */
/* 配信                                                                        */
/* ========================================================================= */

void sd_publish_tempo(void *mgr, const MidiSong *song)
{
#if SD_PLAYER_ASMP
    AsmpSharedContext *sc = asmp_manager_context((AsmpManager *)mgr);
    if (sc) {
        sc->main_ctrl.tempo_us_per_quarter =
            song->initial_tempo_us ? song->initial_tempo_us : 500000u;
    }
#else
    (void)mgr; (void)song;
#endif
}

bool sd_deliver_event(void *mgr, SynthEngine *engine, bool use_asmp, const MidiEvent *ev,
                      uint16_t sample_offset, uint8_t source)
{
#if SD_PLAYER_ASMP
    if (use_asmp) {
        AsmpPacket p = midi_event_to_asmp_packet(ev, sample_offset, source);
        if (p.msg_type == ASMP_MSG_NONE) {
            return true;
        }
        /* 満杯時に黙って捨てるとノート/CC が永久欠落するため失敗を伝播する */
        return asmp_manager_send_command((AsmpManager *)mgr, ASMP_CORE_SUB1_SEQ, &p);
    }
    (void)sample_offset; (void)source; (void)mgr;
#endif
    /* MIDI イベントをローカルエンジンへ直接適用 (シングルコアフォールバック経路) */
    switch (ev->type) {
        case MIDI_STATUS_NOTE_ON:
            synth_engine_channel_note_on(engine, ev->channel, ev->data1, (float)ev->data2 / 127.0f);
            break;
        case MIDI_STATUS_NOTE_OFF:
            synth_engine_channel_note_off(engine, ev->channel, ev->data1);
            break;
        case MIDI_STATUS_PROGRAM_CHANGE:
            synth_engine_program_change(engine, ev->channel, ev->data1);
            break;
        case MIDI_STATUS_CONTROL_CHANGE:
            synth_engine_control_change(engine, ev->channel, ev->data1, ev->data2);
            break;
        case MIDI_STATUS_PITCH_BEND:
            synth_engine_pitch_bend(engine, ev->channel,
                                    midi_pitch_bend_centered(ev->data1, ev->data2));
            break;
        default:
            break;
    }
    return true;
}

/* ========================================================================= */
/* 解放                                                                        */
/* ========================================================================= */

void sd_lane_release_current(SdLaneState *lane, SynthEngine *engine, bool send_asmp_all_off,
                             void *mgr)
{
    midi_parser_free_song(&lane->song);
    sd_loader_stop_stream(); /* ワーカーの自動補充 Refill を確実に停止してからリセット */
    sd_ring_reset(&g_sd_player_ring);
    lane->loaded = false;
    lane->time = 0;
    lane->event_idx = 0;
    lane->last_ts = 0;

    synth_engine_all_notes_off(engine);
#if SD_PLAYER_ASMP
    if (send_asmp_all_off) {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF, .data1 = 0xFF };
        asmp_manager_send_command((AsmpManager *)mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
#else
    (void)send_asmp_all_off;
    (void)mgr;
#endif
}
