/**
 * @file midi_parser.c
 * @brief 標準 MIDI ファイル (SMF Format 0/1) 高速バイナリパーサー実装
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midi_parser.h"
#include "async_logger.h"

/* 16bit / 32bit ビッグエンディアン読み取りユーティリティ */
static inline uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ========================================================================= */
/* 単一トラックのストリーム抽象化 (ファイル / メモリ 共通)                      */
/* ========================================================================= */
#define TRACK_STREAM_BUF_SIZE (512u)
#define MAX_PARSE_TRACKS      (32)

typedef struct {
    long           file_start_pos;
    const uint8_t *mem_data;
    size_t         track_size;
} TrackInfo;

typedef struct {
    FILE           *fp;
    long            file_start_pos;
    const uint8_t  *mem_data;
    size_t          track_size;
    size_t          bytes_read;
    uint8_t         buf[TRACK_STREAM_BUF_SIZE];
    size_t          buf_pos;
    size_t          buf_len;
} TrackStream;

static void stream_init_mem(TrackStream *ts, const uint8_t *data, size_t size)
{
    memset(ts, 0, sizeof(*ts));
    ts->mem_data = data;
    ts->track_size = size;
}

static void stream_init_file(TrackStream *ts, FILE *fp, long file_pos, size_t size)
{
    memset(ts, 0, sizeof(*ts));
    ts->fp = fp;
    ts->file_start_pos = file_pos;
    ts->track_size = size;
    if (fp) {
        fseek(fp, file_pos, SEEK_SET);
    }
}

#if defined(__GNUC__) || defined(__clang__)
static void stream_reset(TrackStream *ts) __attribute__((unused));
#else
static void stream_reset(TrackStream *ts);
#endif
static void stream_reset(TrackStream *ts)
{
    ts->bytes_read = 0;
    ts->buf_pos = 0;
    ts->buf_len = 0;
    if (ts->fp) {
        fseek(ts->fp, ts->file_start_pos, SEEK_SET);
    }
}

static inline bool stream_get_byte(TrackStream *ts, uint8_t *b)
{
    if (ts->bytes_read >= ts->track_size) return false;
    if (ts->mem_data) {
        *b = ts->mem_data[ts->bytes_read++];
        return true;
    }
    if (ts->buf_pos >= ts->buf_len) {
        size_t rem = ts->track_size - ts->bytes_read;
        size_t want = (rem < sizeof(ts->buf)) ? rem : sizeof(ts->buf);
        if (want == 0) return false;
        size_t n = fread(ts->buf, 1, want, ts->fp);
        if (n == 0) return false;
        ts->buf_len = n;
        ts->buf_pos = 0;
    }
    *b = ts->buf[ts->buf_pos++];
    ts->bytes_read++;
    return true;
}

static void stream_skip(TrackStream *ts, size_t n)
{
    size_t rem = ts->track_size - ts->bytes_read;
    if (n > rem) n = rem;
    if (n == 0) return;

    ts->bytes_read += n;
    if (ts->mem_data) return;

    /* ファイルモード: バッファ内スキップまたはfseek */
    if (ts->buf_pos + n <= ts->buf_len) {
        ts->buf_pos += n;
    } else {
        size_t in_buf = ts->buf_len - ts->buf_pos;
        size_t in_file = n - in_buf;
        fseek(ts->fp, (long)in_file, SEEK_CUR);
        ts->buf_pos = 0;
        ts->buf_len = 0;
    }
}

/* 可変長数値 (Variable-Length Quantity: VLQ) 高速ストリームデコード
 * 4バイト超の継続ビットは SMF 不正 (C3)。途中値を返して1バイトずらして
 * 以降全イベントが崩れるのを防ぐため、トラックを強制終了させる */
static uint32_t stream_read_vlq(TrackStream *ts)
{
    uint32_t value = 0;
    int bytes_read = 0;
    uint8_t b = 0;
    while (bytes_read < 4 && stream_get_byte(ts, &b)) {
        bytes_read++;
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            break;
        }
        if (bytes_read == 4 && (b & 0x80)) {
            async_logf("[MIDI] Warning: malformed VLQ (4 bytes with continuation), truncating track\n");
            /* 不正VLQ: 残りの継続バイトを消費しても復帰できないためトラック終端扱い */
            ts->bytes_read = ts->track_size;
            ts->buf_pos = ts->buf_len;
            return value;
        }
    }
    return value;
}

/* テンポ変換構造体 (12B/entry 圧縮: tick と sample_offset を 32bit クランプ保持)。
 * 旧実装は 24B x 64 エントリ (1.5KB スタック) 上限で、クラシック系 SMF の
 * ルバートテンポマップ (数百〜数千件の SetTempo) が溢れていた。
 * dedup 処理で冗長テンポを除去し、256 エントリで十分カバーする。
 * 12B x 256 = 3KB を静的領域 (BSS) に確保し、スタック溢れも回避する。 */
#define MAX_TEMPO_NODES (256)
typedef struct {
    uint32_t tick;          /* u32 clamp: u64 cur_tick が 2^32 を超えたら飽和 */
    uint32_t tempo_us;
    uint32_t sample_offset; /* u32 clamp: 48kHz で 約24.8時間まで収まる */
} TempoPoint;

static TempoPoint s_tempo_map[MAX_TEMPO_NODES];
static uint8_t s_last_cc[16][10]; /* CC重複除去用: 10種のみ 0xFF=未設定 */
static inline int cc_to_idx(uint8_t cc) {
    switch (cc) {
        case 0: return 0; case 1: return 1; case 7: return 2; case 10: return 3; case 11: return 4;
        case 32: return 5; case 64: return 6; case 91: return 7; case 120: return 8; case 123: return 9;
        default: return -1;
    }
}

/* pool full 警告抑制は撤廃: 各曲で明示的にログし拒否を可視化する */

/* u64 累積 tick の格納用クランプ */
static inline uint32_t clamp_tick_u32(uint64_t t)
{
    return (t > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)t;
}

/* Tick -> Sample 変換ヘルパー (テンポマップ追従)
 * P1最適化: double 排除 (M4F ソフトfloat ~500cyc/event -> 単精度 ~15cyc)。
 * d_tick はテンポ区間内差分 (小値) のため float で十分。sample_offset の
 * 累積は uint64 のまま (長時間曲の精度維持)。旧 double 版との差は最大 1 sample。
 * hint_idx: テンポインデックスの単調ヒント (トラック内は時刻単調のため償却O(1)。
 *           NULL 可。rewind 時は呼び出し側で 0 リセットすること) */
static inline uint32_t tick_to_sample(uint64_t tick, const TempoPoint *tempo_map, uint32_t num_tempos,
                                      uint16_t tpqn, uint32_t sample_rate, uint32_t *hint_idx)
{
    uint32_t tick32 = clamp_tick_u32(tick);
    if (num_tempos == 0) {
        /* 到達不能ガード (tempo_map は build 時に既定120BPMの1点を必ず持つ)。
         * 整数演算で既定テンポ換算: tick*sr/(2*tpqn)。double排除 */
        uint32_t div = (uint32_t)2u * (uint32_t)tpqn;
        if (div == 0u) return 0u;
        return (uint32_t)(((uint64_t)tick32 * (uint64_t)sample_rate) / (uint64_t)div);
    }
    uint32_t tp_idx = (hint_idx && *hint_idx < num_tempos) ? *hint_idx : 0u;
    while (tp_idx + 1u < num_tempos && tick32 >= tempo_map[tp_idx + 1u].tick) tp_idx++;
    while (tp_idx > 0u && tick32 < tempo_map[tp_idx].tick) tp_idx--;
    if (hint_idx) *hint_idx = tp_idx;
    uint32_t d_tick = tick32 - tempo_map[tp_idx].tick;
    float d_sec = (float)d_tick * (float)tempo_map[tp_idx].tempo_us / (1000000.0f * (float)tpqn);
    uint64_t s_ret = tempo_map[tp_idx].sample_offset + (uint64_t)(d_sec * (float)sample_rate);
    return (s_ret > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)s_ret;
}

/* メッセージ種別の同一時刻優先度 (0が最高優先: Note Off先行でボイス重複防止)
 * 正しいGM順序: CC0/CC32 Bank -> Program -> NoteOn
 * 旧実装は Program(2) < CC(3) で Bank Select が Program より後になっていた */
static inline int midi_msg_priority(uint8_t type, uint8_t data1)
{
    if (type == MIDI_STATUS_NOTE_OFF) return 0;
    if (type == MIDI_STATUS_CONTROL_CHANGE && (data1 == 120 || data1 == 123)) return 1;
    if (type == MIDI_STATUS_CONTROL_CHANGE && (data1 == 0 || data1 == 32)) return 2; /* Bank MSB/LSB */
    if (type == MIDI_STATUS_PROGRAM_CHANGE) return 3;
    if (type == MIDI_STATUS_CONTROL_CHANGE) return 4;
    if (type == MIDI_STATUS_PITCH_BEND) return 5;
    return 6; /* MIDI_STATUS_NOTE_ON */
}

/* qsort用イベント比較関数 (絶対サンプル時間昇順、同一時刻のメッセージ順序制御)
 * 安定ソート代替: 同一timestamp/同一priorityは入力順序(sequence)で確定
 * (qsortは非安定のため、Bank->Program等の入力順が崩れるのを防止) */
static int compare_midi_events(const void *a, const void *b)
{
    const MidiEvent *ea = (const MidiEvent *)a;
    const MidiEvent *eb = (const MidiEvent *)b;
    if (ea->timestamp_samples < eb->timestamp_samples) return -1;
    if (ea->timestamp_samples > eb->timestamp_samples) return 1;

    /* 同一時刻はメッセージ優先度 (Note Off > AllOff > Bank > PC > CC > PitchBend > Note On) で順序を確定 */
    int pa = midi_msg_priority(ea->type, ea->data1);
    int pb = midi_msg_priority(eb->type, eb->data1);
    if (pa < pb) return -1;
    if (pa > pb) return 1;

    /* 同一priorityは入力順序(sequence)で安定化 */
    if (ea->sequence < eb->sequence) return -1;
    if (ea->sequence > eb->sequence) return 1;
    return 0;
}

/* テンポマップ構築共通関数 (第1パス)
 * out_max_tick != NULL の場合、全トラックの最終 tick (EndOfTrack 含むデルタ累積) の
 * 最大値を格納する。ストリーミング open 時の total_samples 算出用
 * (バッチパーサはイベント最終 ts+1sec を使うが、ストリーミングは事前に
 *  全長を知る必要があるため、テンポ走査のついでに無追加 I/O で求める) */
static uint32_t build_tempo_map_from_tracks(FILE *fp, const TrackInfo *tracks, uint32_t found_tracks,
                                            TempoPoint *tempo_map, uint32_t max_tempos,
                                            uint16_t ticks_per_quarter, uint32_t sample_rate,
                                            uint32_t *initial_tempo_us, char *out_title, size_t title_size,
                                            uint64_t *out_max_tick)
{
    uint32_t num_tempos = 0;
    bool     tempo_overflow_warned = false;
    tempo_map[0].tick = 0;
    tempo_map[0].tempo_us = 500000; /* デフォルト 120 BPM */
    tempo_map[0].sample_offset = 0;
    num_tempos = 1;
    if (initial_tempo_us) *initial_tempo_us = 500000;
    if (out_title && title_size > 0) out_title[0] = '\0';
    uint64_t max_tick_all = 0;
    uint64_t max_event_tick = 0; /* 最終音楽イベント (ch系) の tick。EndOfTrack 後の無音は含めない */

    TrackStream ts;

    for (uint32_t t = 0; t < found_tracks; t++) {
        if (tracks[t].mem_data) {
            stream_init_mem(&ts, tracks[t].mem_data, tracks[t].track_size);
        } else {
            stream_init_file(&ts, fp, tracks[t].file_start_pos, tracks[t].track_size);
        }

        uint64_t cur_tick = 0;
        uint8_t run_status = 0;

        while (ts.bytes_read < ts.track_size) {
            uint32_t dt = stream_read_vlq(&ts);
            cur_tick += dt;
            if (ts.bytes_read >= ts.track_size) break;

            uint8_t status = 0;
            if (!stream_get_byte(&ts, &status)) break;

            if (status >= 0xF8 && status <= 0xFE) {
                continue;
            } else if (status & 0x80) {
                if (status < 0xF0) {
                    run_status = status;
                } else {
                    run_status = 0;
                }
            } else {
                if (run_status == 0) continue;
                status = run_status;
                /* ランニングステータス ch系も音楽イベントとして最終 tick に計上 */
                if ((status & 0xF0) != 0xF0 && cur_tick > max_event_tick) {
                    max_event_tick = cur_tick;
                }
                uint8_t msg_type = status & 0xF0;
                if (msg_type == 0xC0 || msg_type == 0xD0) {
                } else {
                    stream_skip(&ts, 1);
                }
                continue;
            }

            if (status == 0xFF) {
                uint8_t meta_type = 0;
                if (!stream_get_byte(&ts, &meta_type)) break;
                uint32_t meta_len = stream_read_vlq(&ts);

                if (meta_type == 0x51 && meta_len == 3) {
                    uint8_t tbuf[3];
                    if (stream_get_byte(&ts, &tbuf[0]) &&
                        stream_get_byte(&ts, &tbuf[1]) &&
                        stream_get_byte(&ts, &tbuf[2])) {
                        uint32_t tempo = ((uint32_t)tbuf[0] << 16) |
                                         ((uint32_t)tbuf[1] << 8) |
                                         (uint32_t)tbuf[2];
                        uint32_t tempo_store = (tempo > 0) ? tempo : 500000u;
                        uint32_t tick_store = clamp_tick_u32(cur_tick);
                        if (num_tempos > 0 && tempo_map[num_tempos - 1].tick == tick_store) {
                            tempo_map[num_tempos - 1].tempo_us = tempo_store;
                        } else if (num_tempos > 0 && tempo_map[num_tempos - 1].tempo_us == tempo_store) {
                        } else if (num_tempos < max_tempos) {
                            tempo_map[num_tempos].tick = tick_store;
                            tempo_map[num_tempos].tempo_us = tempo_store;
                            num_tempos++;
                        } else {
                            if (!tempo_overflow_warned) {
                                async_logf("[MIDI] Warning: tempo_map overflow (%d), discarding tempo at tick %llu\n",
                                           (int)max_tempos, (unsigned long long)cur_tick);
                                tempo_overflow_warned = true;
                            }
                        }
                        if (cur_tick == 0 && initial_tempo_us) {
                            *initial_tempo_us = tempo;
                        }
                    }
                } else if (meta_type == 0x03 && meta_len > 0 && out_title && title_size > 0 && out_title[0] == '\0') {
                    size_t cpy_len = (meta_len < title_size - 1) ? meta_len : title_size - 1;
                    for (size_t c = 0; c < cpy_len; c++) {
                        uint8_t ch = 0;
                        stream_get_byte(&ts, &ch);
                        out_title[c] = (char)ch;
                    }
                    out_title[cpy_len] = '\0';
                    if (meta_len > cpy_len) stream_skip(&ts, meta_len - cpy_len);
                } else {
                    stream_skip(&ts, meta_len);
                }
                run_status = 0;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sysex_len = stream_read_vlq(&ts);
                stream_skip(&ts, sysex_len);
                run_status = 0;
            } else if (status >= 0xF1 && status <= 0xF6) {
                static const uint8_t sys_common_len[6] = { 1, 2, 1, 0, 0, 0 };
                stream_skip(&ts, sys_common_len[status - 0xF1]);
            } else if (status >= 0xF8 && status <= 0xFE) {
            } else {
                /* ch系 (0x80-0xEF) のみ最終イベント tick に計上。meta/sysex の
                 * EndOfTrack 後無音は total に含めない (バッチと同式にする) */
                if (status < 0xF0 && cur_tick > max_event_tick) {
                    max_event_tick = cur_tick;
                }
                uint8_t msg_type = status & 0xF0;
                if (msg_type == 0xC0 || msg_type == 0xD0) {
                    stream_skip(&ts, 1);
                } else {
                    stream_skip(&ts, 2);
                }
            }
        }
        if (cur_tick > max_tick_all) max_tick_all = cur_tick;
    }
    /* total 用は音楽イベント最終 tick を優先。ch系が1件も無い空曲のみ
     * トラック末尾 (EndOfTrack) にフォールバックする */
    if (out_max_tick) *out_max_tick = (max_event_tick > 0) ? max_event_tick : max_tick_all;

    if (num_tempos > 1) {
        for (uint32_t i = 0; i < num_tempos - 1; i++) {
            for (uint32_t j = i + 1; j < num_tempos; j++) {
                if (tempo_map[i].tick > tempo_map[j].tick) {
                    TempoPoint tmp = tempo_map[i];
                    tempo_map[i] = tempo_map[j];
                    tempo_map[j] = tmp;
                }
            }
        }
    }

    tempo_map[0].sample_offset = 0;
    for (uint32_t i = 1; i < num_tempos; i++) {
        uint32_t tick_diff = (tempo_map[i].tick >= tempo_map[i - 1].tick)
                             ? (tempo_map[i].tick - tempo_map[i - 1].tick) : 0;
        double sec = ((double)tick_diff * (double)tempo_map[i - 1].tempo_us) / (1000000.0 * (double)ticks_per_quarter);
        uint64_t off = tempo_map[i - 1].sample_offset + (uint64_t)(sec * (double)sample_rate + 0.5);
        tempo_map[i].sample_offset = (off > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)off;
    }

    return num_tempos;
}

/* 共通コアパーサー (単一 TrackStream を再利用してスタック消費を最小化) */
static int parse_smf_tracks(FILE *fp, const TrackInfo *tracks, uint32_t found_tracks, MidiSong *song,
                            MidiEvent *event_pool, uint32_t max_events, uint32_t sample_rate)
{
    if (event_pool && max_events > 0) {
        song->events = event_pool;
        song->events_dynamic = false;   /* 静的プール: free してはならない */
    } else {
        max_events = (max_events > 0) ? max_events : 8192;
        song->events = (MidiEvent *)malloc(sizeof(MidiEvent) * max_events);
        if (!song->events) return -4;
        song->events_dynamic = true;
    }

    /* 3. 第1パス: テンポチェンジマップ (Set Tempo) の収集 */
    TempoPoint *tempo_map = s_tempo_map; /* 静的 256 エントリ (スタック不使用) */
    uint32_t num_tempos = build_tempo_map_from_tracks(fp, tracks, found_tracks,
                                                     tempo_map, MAX_TEMPO_NODES,
                                                     song->ticks_per_quarter, sample_rate,
                                                     &song->initial_tempo_us,
                                                     song->title, sizeof(song->title),
                                                     NULL);

    /* 4. 第2パス: チャンネルメッセージ抽出 & 統合イベントストリーム構築 */
    uint32_t total_events = 0;
    bool     truncated = false;
    const uint32_t pool_guard = (max_events > 128) ? (max_events - 128) : 0;
    uint32_t next_sequence = 0;
    memset(s_last_cc, 0xFF, sizeof(s_last_cc));
    bool open_notes[16][128] = {{false}};
    TrackStream ts;

    for (uint32_t t = 0; t < found_tracks; t++) {
        if (tracks[t].mem_data) {
            stream_init_mem(&ts, tracks[t].mem_data, tracks[t].track_size);
        } else {
            stream_init_file(&ts, fp, tracks[t].file_start_pos, tracks[t].track_size);
        }

        uint64_t cur_tick = 0;
        uint8_t run_status = 0;

        while (ts.bytes_read < ts.track_size && total_events < max_events) {
            uint32_t dt = stream_read_vlq(&ts);
            cur_tick += dt;
            if (ts.bytes_read >= ts.track_size) break;

            uint8_t status = 0;
            if (!stream_get_byte(&ts, &status)) break;

            uint8_t data1_preload = 0;
            bool    has_preload = false;

            if (status >= 0xF8 && status <= 0xFE) {
                continue;
            } else if (status & 0x80) {
                if (status < 0xF0) {
                    run_status = status;
                } else {
                    run_status = 0;
                }
            } else {
                if (run_status == 0) {
                    continue;
                }
                data1_preload = status;
                has_preload = true;
                status = run_status;
            }

            if (status == 0xFF) {
                uint8_t meta_type = 0;
                stream_get_byte(&ts, &meta_type);
                (void)meta_type;
                uint32_t meta_len = stream_read_vlq(&ts);
                stream_skip(&ts, meta_len);
                run_status = 0;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sysex_len = stream_read_vlq(&ts);
                stream_skip(&ts, sysex_len);
                run_status = 0;
            } else if (status >= 0xF1 && status <= 0xF6) {
                static const uint8_t sys_common_len[6] = { 1, 2, 1, 0, 0, 0 };
                stream_skip(&ts, sys_common_len[status - 0xF1]);
            } else if (status >= 0xF8 && status <= 0xFE) {
                /* リアルタイムメッセージ */
            } else {
                uint8_t msg_type = status & 0xF0;
                uint8_t ch = status & 0x0F;

                if (msg_type == 0x80 || msg_type == 0x90 || msg_type == 0xA0 ||
                    msg_type == 0xB0 || msg_type == 0xC0 || msg_type == 0xD0 ||
                    msg_type == 0xE0) {

                    uint8_t d1 = 0, d2 = 0;
                    if (has_preload) {
                        d1 = data1_preload;
                    } else {
                        stream_get_byte(&ts, &d1);
                    }

                    if (msg_type != 0xC0 && msg_type != 0xD0) {
                        stream_get_byte(&ts, &d2);
                    }

                    /* Note On でベロシティ 0 は Note Off として事前正規化 */
                    if (msg_type == MIDI_STATUS_NOTE_ON && d2 == 0) {
                        msg_type = MIDI_STATUS_NOTE_OFF;
                    }

                    /* P0: 有限プール節約フィルタ — 再生で未使用のイベントは格納しない
                     * PolyAT(0xA0)/ChPressure(0xD0)は音源未対応、空振りでプールを消費するだけ */
                    if (msg_type == MIDI_STATUS_POLY_AFTERTOUCH ||
                        msg_type == MIDI_STATUS_CHANNEL_PRESSURE) {
                        continue;
                    }
                    /* CCは音源が実装済みのもののみ保持。未対応CCをロスレス保持すると
                     * オートメーション大量のSMFで12288件を容易に超過し全曲拒否になる。
                     * 対応CC: 0/32 Bank, 1 Mod, 7 Vol, 10 Pan, 11 Expr, 64 Sustain, 91 Reverb, 120/123 AllOff */
                    if (msg_type == MIDI_STATUS_CONTROL_CHANGE) {
                        switch (d1) {
                            case 0: case 1: case 7: case 10: case 11: case 32: case 64: case 91: case 120: case 123:
                                break;
                            default:
                                continue;
                        }
                    }
                    /* P0-6 Fix: Program Change重複除去は統合後に実行 (ファイル順誤判定防止) */
                    /* CC重複除去: 同一ch/同一CC番号で値が前回と同じならスキップ (オートメーションの冗長除去) */
                    if (msg_type == MIDI_STATUS_CONTROL_CHANGE) {
                        int idx = cc_to_idx(d1);
                        if (idx >= 0) {
                            if (s_last_cc[ch][idx] == d2) {
                                continue;
                            }
                            s_last_cc[ch][idx] = d2;
                        }
                    }
                    /* プール末端ガード: 末尾128枠は強制NoteOff用に予約 */
                    if (total_events >= pool_guard) {
                        truncated = true;
                        continue;
                    }

                    MidiEvent *ev = &song->events[total_events];
                    ev->timestamp_samples = tick_to_sample(cur_tick, tempo_map, num_tempos,
                                                           song->ticks_per_quarter, sample_rate, NULL);
                    ev->sequence = next_sequence++;
                    total_events++;
                    ev->type = msg_type;
                    ev->channel = ch;
                    ev->data1 = d1;
                    ev->data2 = d2;
                    if (msg_type == MIDI_STATUS_NOTE_ON) open_notes[ch][d1 & 0x7F] = true;
                    else if (msg_type == MIDI_STATUS_NOTE_OFF) open_notes[ch][d1 & 0x7F] = false;
                } else {
                    /* 未対応メッセージはスキップ */
                    stream_skip(&ts, has_preload ? 0 : 0);
                }
            }
        }
        if (ts.bytes_read < ts.track_size) {
            truncated = true;
        }
    }
    /* 強制クローズ: プール溢れで打ち切られた場合、開いたままのノートを追跡し
     * 最後にNoteOffを強制追加してハングを防止 (末尾128枠予約で必ず収まる) */
    if (truncated) {
        uint32_t force_ts = (total_events > 0) ? song->events[total_events - 1].timestamp_samples + 480 : 0;
        uint32_t forced = 0;
        for (int ch = 0; ch < 16 && total_events < max_events; ch++) {
            for (int n = 0; n < 128 && total_events < max_events; n++) {
                if (open_notes[ch][n]) {
                    MidiEvent *ev = &song->events[total_events];
                    ev->timestamp_samples = force_ts;
                    ev->sequence = next_sequence++;
                    ev->type = MIDI_STATUS_NOTE_OFF;
                    ev->channel = (uint8_t)ch;
                    ev->data1 = (uint8_t)n;
                    ev->data2 = 0;
                    total_events++;
                    forced++;
                    open_notes[ch][n] = false;
                }
            }
        }
        if (forced) {
            async_logf("[MIDI] forced NoteOff to close %u hanging notes at %u\n",
                       (unsigned)forced, (unsigned)force_ts);
        }
    }

    song->event_count = total_events;

    /* 5. 複数トラック統合イベントの時系列ソート */
    if (song->event_count > 1) {
        qsort(song->events, song->event_count, sizeof(MidiEvent), compare_midi_events);
    }
    /* P0-6 Fix: Program Change重複除去を統合後に実行 (時系列直前で判定)
     * 旧はトラック解析中に last_pc を更新していたため、Format1で同一chが複数
     * トラックに跨る場合にファイル順で誤って消していた。Bank込みで判定 */
    {
        typedef struct { uint8_t bank_msb; uint8_t bank_lsb; uint8_t program; bool has_bank; } PatchState;
        PatchState last_patch[16] = {0};
        for (int i=0;i<16;i++) { last_patch[i].bank_msb=0xFF; last_patch[i].bank_lsb=0xFF; last_patch[i].program=0xFF; }
        uint32_t w = 0;
        for (uint32_t r=0;r<song->event_count;r++) {
            MidiEvent *ev = &song->events[r];
            bool is_dup = false;
            if (ev->type == MIDI_STATUS_PROGRAM_CHANGE) {
                PatchState *st = &last_patch[ev->channel & 0x0F];
                if (st->program == ev->data1 && st->has_bank) {
                    // 同一バンク+プログラムなら重複。バンク未設定時はプログラムのみで判定
                    is_dup = true;
                }
                if (!is_dup) {
                    st->program = ev->data1;
                }
            } else if (ev->type == MIDI_STATUS_CONTROL_CHANGE) {
                if (ev->data1 == 0) { last_patch[ev->channel & 0x0F].bank_msb = ev->data2; last_patch[ev->channel & 0x0F].has_bank = true; }
                else if (ev->data1 == 32) { last_patch[ev->channel & 0x0F].bank_lsb = ev->data2; last_patch[ev->channel & 0x0F].has_bank = true; }
                else if (ev->data1 == 0 || ev->data1 == 32) { /* Bankは常に保持 */ }
            }
            if (is_dup) continue;
            if (w != r) song->events[w] = song->events[r];
            w++;
        }
        song->event_count = w;
    }
    if (song->event_count > 1) {
        song->total_samples = song->events[song->event_count - 1].timestamp_samples + sample_rate;
    } else if (song->event_count == 1) {
        song->total_samples = song->events[0].timestamp_samples + sample_rate;
    } else {
        song->total_samples = sample_rate * 4;
    }

    if (truncated) {
        async_logf("[MIDI] Warning: event pool full (%u events), song truncated (guard kept NoteOff)\n",
                   (unsigned int)song->event_count);
        /* 拒否ではなくトランケート再生: pool_guardでNoteOffは保持済みのためハングは回避 */
    }

    return 0;
}

int midi_parser_load_file(FILE *fp, MidiSong *song,
                          MidiEvent *event_pool, uint32_t max_events, uint32_t sample_rate)
{
    if (!fp || !song) return -1;
    if (sample_rate == 0) sample_rate = 48000;

    memset(song, 0, sizeof(MidiSong));
    strncpy(song->title, "SMF Track", sizeof(song->title) - 1);
    song->initial_tempo_us = 500000;

    /* 1. MThd ヘッダー検証 */
    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[14];
    if (fread(hdr, 1, 14, fp) != 14) {
        return -2;
    }
    if (memcmp(hdr, "MThd", 4) != 0) {
        return -2;
    }

    uint32_t header_len = read_be32(hdr + 4);
    if (header_len < 6) {
        return -3;
    }

    song->format = read_be16(hdr + 8);
    song->num_tracks = read_be16(hdr + 10);
    song->ticks_per_quarter = read_be16(hdr + 12);
    if (song->ticks_per_quarter == 0) song->ticks_per_quarter = 480;

    /* 2. トラック (MTrk) 位置の収集 (軽量 TrackInfo: 32件で約384B) */
    fseek(fp, 8 + (long)header_len, SEEK_SET);
    TrackInfo tracks[MAX_PARSE_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    uint32_t found_tracks = 0;

    while (found_tracks < MAX_PARSE_TRACKS && found_tracks < song->num_tracks) {
        uint8_t trk_hdr[8];
        if (fread(trk_hdr, 1, 8, fp) != 8) break;

        if (memcmp(trk_hdr, "MTrk", 4) == 0) {
            uint32_t trk_len = read_be32(trk_hdr + 4);
            long cur_pos = ftell(fp);
            tracks[found_tracks].file_start_pos = cur_pos;
            tracks[found_tracks].track_size = trk_len;
            found_tracks++;
            fseek(fp, (long)trk_len, SEEK_CUR);
        } else {
            fseek(fp, -7, SEEK_CUR); /* 1バイト進んで再同期 */
        }
    }

    if (found_tracks == 0) return -5;

    return parse_smf_tracks(fp, tracks, found_tracks, song, event_pool, max_events, sample_rate);
}

int midi_parser_load_memory(const uint8_t *data, size_t size, MidiSong *song,
                            MidiEvent *event_pool, uint32_t max_events, uint32_t sample_rate)
{
    if (!data || size < 14 || !song) {
        return -1;
    }

    if (sample_rate == 0) {
        sample_rate = 48000;
    }

    memset(song, 0, sizeof(MidiSong));
    strncpy(song->title, "SMF Track", sizeof(song->title) - 1);
    song->initial_tempo_us = 500000;

    /* 1. MThd ヘッダー検証 */
    if (memcmp(data, "MThd", 4) != 0) {
        return -2;
    }

    uint32_t header_len = read_be32(data + 4);
    if (header_len < 6 || size < 8 || (size - 8) < header_len) {
        return -3;
    }

    song->format = read_be16(data + 8);
    song->num_tracks = read_be16(data + 10);
    song->ticks_per_quarter = read_be16(data + 12);
    if (song->ticks_per_quarter == 0) song->ticks_per_quarter = 480;

    /* 2. トラック (MTrk) 位置の収集 */
    size_t pos = 8 + header_len;
    TrackInfo tracks[MAX_PARSE_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    uint32_t found_tracks = 0;

    while (pos + 8 <= size && found_tracks < MAX_PARSE_TRACKS && found_tracks < song->num_tracks) {
        if (memcmp(data + pos, "MTrk", 4) == 0) {
            uint32_t trk_len = read_be32(data + pos + 4);
            size_t rem = size - (pos + 8);
            size_t actual_trk_len = (trk_len <= rem) ? (size_t)trk_len : rem;

            tracks[found_tracks].mem_data = data + pos + 8;
            tracks[found_tracks].track_size = actual_trk_len;
            found_tracks++;

            if (trk_len > rem) break;
            pos += 8 + (size_t)trk_len;
        } else {
            pos++;
        }
    }

    if (found_tracks == 0) return -5;

    return parse_smf_tracks(NULL, tracks, found_tracks, song, event_pool, max_events, sample_rate);
}

int midi_file_load_memory(const uint8_t *data, size_t size, MidiSong *song)
{
    return midi_parser_load_memory(data, size, song, NULL, 8192, 48000);
}

void midi_parser_free_song(MidiSong *song)
{
    if (!song) return;
    if (song->events && song->events_dynamic) {
        /* 動的確保時のみ解放 (静的プールへの free はヒープ破壊を起こす) */
        free(song->events);
    }
    song->events = NULL;
    song->events_dynamic = false;
    song->event_count = 0;
    song->total_samples = 0;
}

/* ========================================================================= */
/* 5. ストリーミング (k-way マージ・オンデマンド先読み) 実装                 */
/* ========================================================================= */

static bool stream_track_get_byte(FILE *fp, MidiStreamTrack *tr, uint8_t *b)
{
    if (tr->bytes_consumed >= tr->track_size) return false;

    if (tr->buf_pos >= tr->buf_len) {
        /* バッファ枯渇: ファイルから次のブロックを補充 */
        size_t rem = tr->track_size - tr->bytes_consumed;
        size_t want = (rem < sizeof(tr->buf)) ? rem : sizeof(tr->buf);
        if (want == 0) return false;

        long want_offset = tr->file_data_pos + (long)tr->bytes_consumed;
        if (fseek(fp, want_offset, SEEK_SET) != 0) {
            return false;
        }
        size_t n = fread(tr->buf, 1, want, fp);
        if (n == 0) return false;

        tr->buf_file_offset = want_offset;
        tr->buf_len = (uint16_t)n;
        tr->buf_pos = 0;
    }

    *b = tr->buf[tr->buf_pos++];
    tr->bytes_consumed++;
    return true;
}

static void stream_track_skip(FILE *fp, MidiStreamTrack *tr, size_t n)
{
    (void)fp;
    size_t rem = tr->track_size - tr->bytes_consumed;
    if (n > rem) n = rem;
    if (n == 0) return;

    if (tr->buf_pos + n <= tr->buf_len) {
        tr->buf_pos += (uint16_t)n;
        tr->bytes_consumed += (uint32_t)n;
    } else {
        tr->bytes_consumed += (uint32_t)n;
        tr->buf_pos = 0;
        tr->buf_len = 0;
    }
}

static uint32_t stream_track_read_vlq(FILE *fp, MidiStreamTrack *tr)
{
    uint32_t value = 0;
    int bytes_read = 0;
    uint8_t b = 0;
    while (bytes_read < 4 && stream_track_get_byte(fp, tr, &b)) {
        bytes_read++;
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
        if (bytes_read == 4 && (b & 0x80)) {
            tr->bytes_consumed = tr->track_size;
            tr->buf_pos = tr->buf_len;
            return value;
        }
    }
    return value;
}

static bool stream_track_fetch_next_event(MidiStreamReader *reader, uint8_t track_idx)
{
    MidiStreamTrack *tr = &reader->tracks[track_idx];
    if (tr->is_eof || tr->has_event) return tr->has_event;

    FILE *fp = reader->fp;
    while (tr->bytes_consumed < tr->track_size) {
        uint32_t delta_tick = stream_track_read_vlq(fp, tr);
        tr->cur_tick += delta_tick;

        uint8_t b = 0;
        if (!stream_track_get_byte(fp, tr, &b)) break;

        uint8_t status = b;
        uint8_t data1_preload = 0;
        bool has_preload = false;

        if (status < 0x80) {
            if (tr->run_status == 0) continue;
            status = tr->run_status;
            data1_preload = b;
            has_preload = true;
        } else {
            if (status < 0xF0) tr->run_status = status;
            else if (status < 0xF8) tr->run_status = 0;
        }

        if (status == 0xFF) {
            uint8_t meta_type = 0;
            stream_track_get_byte(fp, tr, &meta_type);
            uint32_t meta_len = stream_track_read_vlq(fp, tr);
            if (meta_type == 0x2F) {
                tr->is_eof = true;
                break;
            }
            stream_track_skip(fp, tr, meta_len);
            tr->run_status = 0;
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t sysex_len = stream_track_read_vlq(fp, tr);
            stream_track_skip(fp, tr, sysex_len);
            tr->run_status = 0;
        } else if (status >= 0xF1 && status <= 0xF6) {
            static const uint8_t sys_common_len[6] = { 1, 2, 1, 0, 0, 0 };
            stream_track_skip(fp, tr, sys_common_len[status - 0xF1]);
        } else if (status >= 0xF8 && status <= 0xFE) {
            /* リアルタイム */
        } else {
            uint8_t msg_type = status & 0xF0;
            uint8_t ch = status & 0x0F;

            if (msg_type == 0x80 || msg_type == 0x90 || msg_type == 0xA0 ||
                msg_type == 0xB0 || msg_type == 0xC0 || msg_type == 0xD0 ||
                msg_type == 0xE0) {

                uint8_t d1 = 0, d2 = 0;
                if (has_preload) d1 = data1_preload;
                else stream_track_get_byte(fp, tr, &d1);

                if (msg_type != 0xC0 && msg_type != 0xD0) {
                    stream_track_get_byte(fp, tr, &d2);
                }

                if (msg_type == MIDI_STATUS_NOTE_ON && d2 == 0) {
                    msg_type = MIDI_STATUS_NOTE_OFF;
                }

                if (msg_type == MIDI_STATUS_POLY_AFTERTOUCH ||
                    msg_type == MIDI_STATUS_CHANNEL_PRESSURE) {
                    continue;
                }

                if (msg_type == MIDI_STATUS_CONTROL_CHANGE) {
                    switch (d1) {
                        case 0: case 1: case 7: case 10: case 11:
                        case 32: case 64: case 91: case 120: case 123:
                            break;
                        default:
                            continue;
                    }
                    int cidx = cc_to_idx(d1);
                    if (cidx >= 0) {
                        if (reader->last_cc[ch][cidx] == d2) continue;
                        reader->last_cc[ch][cidx] = d2;
                    }
                }

                if (msg_type == MIDI_STATUS_PROGRAM_CHANGE) {
                    if (reader->last_program[ch] == d1 && reader->has_bank[ch]) continue;
                    reader->last_program[ch] = d1;
                }
                if (msg_type == MIDI_STATUS_CONTROL_CHANGE && (d1 == 0 || d1 == 32)) {
                    if (d1 == 0) reader->last_bank_msb[ch] = d2;
                    if (d1 == 32) reader->last_bank_lsb[ch] = d2;
                    reader->has_bank[ch] = true;
                }

                MidiEvent *ev = &tr->pending_event;
                ev->timestamp_samples = tick_to_sample(tr->cur_tick,
                                                       (const TempoPoint *)reader->tempo_map,
                                                       reader->num_tempos,
                                                       reader->ticks_per_quarter,
                                                       reader->sample_rate,
                                                       &tr->tp_hint);
                ev->sequence = tr->bytes_consumed;
                ev->type = msg_type;
                ev->channel = ch;
                ev->data1 = d1;
                ev->data2 = d2;

                if (msg_type == MIDI_STATUS_NOTE_ON) reader->open_notes[ch][d1 & 0x7F] = true;
                else if (msg_type == MIDI_STATUS_NOTE_OFF) reader->open_notes[ch][d1 & 0x7F] = false;

                tr->has_event = true;
                return true;
            }
        }
    }

    tr->is_eof = true;
    tr->has_event = false;
    return false;
}

static inline int compare_stream_events_inline(const MidiEvent *a, uint16_t track_a,
                                              const MidiEvent *b, uint16_t track_b)
{
    if (a->timestamp_samples < b->timestamp_samples) return -1;
    if (a->timestamp_samples > b->timestamp_samples) return 1;

    /* 同一時刻: メッセージ優先度 (Note Off > AllOff > Bank > PC > CC > PitchBend > Note On) */
    int pa = midi_msg_priority(a->type, a->data1);
    int pb = midi_msg_priority(b->type, b->data1);
    if (pa < pb) return -1;
    if (pa > pb) return 1;

    /* 同一時刻・同一優先度: トラック番号昇順 (バッチパースと完全等価) */
    if (track_a < track_b) return -1;
    if (track_a > track_b) return 1;

    /* 同一トラック内: シーケンス昇順 */
    if (a->sequence < b->sequence) return -1;
    if (a->sequence > b->sequence) return 1;
    return 0;
}

int midi_stream_open(MidiStreamReader *reader, FILE *fp, uint32_t sample_rate)
{
    if (!reader || !fp) return -1;

    memset(reader, 0, sizeof(*reader));
    reader->fp = fp;
    reader->sample_rate = sample_rate ? sample_rate : 48000;
    reader->initial_tempo_us = 500000;

    for (int ch = 0; ch < 16; ch++) {
        for (int i = 0; i < 10; i++) reader->last_cc[ch][i] = 0xFF;
        reader->last_bank_msb[ch] = 0xFF;
        reader->last_bank_lsb[ch] = 0xFF;
        reader->last_program[ch] = 0xFF;
    }

    /* 1. MThd ヘッダ解析 */
    fseek(fp, 0, SEEK_SET);
    uint8_t hbuf[14];
    if (fread(hbuf, 1, 14, fp) != 14 || memcmp(hbuf, "MThd", 4) != 0) {
        return -2;
    }
    uint32_t hdr_len = read_be32(hbuf + 4);
    reader->format = read_be16(hbuf + 8);
    reader->num_tracks = read_be16(hbuf + 10);
    reader->ticks_per_quarter = read_be16(hbuf + 12);

    if (hdr_len > 6) {
        fseek(fp, (long)(8 + hdr_len), SEEK_SET);
    }

    if (reader->num_tracks > MIDI_STREAM_MAX_TRACKS) {
        reader->num_tracks = MIDI_STREAM_MAX_TRACKS;
    }

    /* 2. MTrk トラック探索 */
    uint32_t found_tracks = 0;
    uint8_t chunk_hdr[8];
    while (found_tracks < reader->num_tracks && fread(chunk_hdr, 1, 8, fp) == 8) {
        if (memcmp(chunk_hdr, "MTrk", 4) == 0) {
            uint32_t trk_len = read_be32(chunk_hdr + 4);
            long data_pos = ftell(fp);

            MidiStreamTrack *tr = &reader->tracks[found_tracks];
            tr->file_data_pos = data_pos;
            tr->track_size = trk_len;
            found_tracks++;

            fseek(fp, (long)trk_len, SEEK_CUR);
        } else {
            fseek(fp, -7, SEEK_CUR);
        }
    }
    reader->num_tracks = (uint16_t)found_tracks;
    if (reader->num_tracks == 0) return -3;

    /* 3. テンポマップ事前抽出 (全トラック走査で確実に Set Tempo と曲名を抽出)
     * 同時に全長 tick を取得し total_samples (末尾1secテール付き, バッチと同式) を確定する。
     * 旧実装は total_samples を 0 のまま残し、Main の完奏判定
     * (time >= total) が常に真 (=リング空即完奏) になっていた。短曲では
     * リリース/リバーブテール 1sec が切り詰められ、曲尻で突発無音化=音飛びに聞こえた */
    {
        TrackInfo ti[MIDI_STREAM_MAX_TRACKS];
        for (uint16_t t = 0; t < reader->num_tracks; t++) {
            ti[t].file_start_pos = reader->tracks[t].file_data_pos;
            ti[t].track_size = reader->tracks[t].track_size;
            ti[t].mem_data = NULL;
        }
        uint64_t max_tick = 0;
        reader->num_tempos = build_tempo_map_from_tracks(fp, ti, reader->num_tracks,
                                                         (TempoPoint *)reader->tempo_map,
                                                         MIDI_STREAM_MAX_TEMPOS,
                                                         reader->ticks_per_quarter,
                                                         reader->sample_rate,
                                                         &reader->initial_tempo_us,
                                                         reader->title, sizeof(reader->title),
                                                         &max_tick);
        /* バッチ parse_smf_tracks と同式: 最終 tick の sample 換算 + 1sec テール。
         * tick_to_sample は tempo_map 完成後でないと呼べないためここで算出する */
        {
            uint32_t tail_ts = tick_to_sample(max_tick,
                                              (const TempoPoint *)reader->tempo_map,
                                              reader->num_tempos,
                                              reader->ticks_per_quarter,
                                              reader->sample_rate, NULL);
            uint64_t total = (uint64_t)tail_ts + (uint64_t)reader->sample_rate;
            reader->total_samples = (total > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)total;
        }
    }

    /* 4. 各トラックの先頭イベント先読み */
    for (uint16_t t = 0; t < reader->num_tracks; t++) {
        stream_track_fetch_next_event(reader, (uint8_t)t);
    }

    return 0;
}

uint32_t midi_stream_read(MidiStreamReader *reader, MidiEvent *out_events, uint32_t max_events)
{
    if (!reader || !reader->fp || reader->is_eof || !out_events || max_events == 0) {
        return 0;
    }

    uint32_t count = 0;
    while (count < max_events) {
        int best_track = -1;

        for (uint16_t t = 0; t < reader->num_tracks; t++) {
            if (!reader->tracks[t].has_event && !reader->tracks[t].is_eof) {
                stream_track_fetch_next_event(reader, (uint8_t)t);
            }
            if (reader->tracks[t].has_event) {
                if (best_track < 0 ||
                    compare_stream_events_inline(&reader->tracks[t].pending_event, (uint16_t)t,
                                                 &reader->tracks[best_track].pending_event, (uint16_t)best_track) < 0) {
                    best_track = (int)t;
                }
            }
        }

        if (best_track < 0) {
            reader->is_eof = true;
            break;
        }

        MidiEvent ev = reader->tracks[best_track].pending_event;
        ev.sequence = reader->next_sequence++;
        out_events[count++] = ev;
        reader->tracks[best_track].has_event = false;
    }

    return count;
}

void midi_stream_rewind(MidiStreamReader *reader)
{
    if (!reader || !reader->fp) return;

    reader->is_eof = false;
    reader->next_sequence = 0;

    for (int ch = 0; ch < 16; ch++) {
        for (int i = 0; i < 10; i++) reader->last_cc[ch][i] = 0xFF;
        reader->last_bank_msb[ch] = 0xFF;
        reader->last_bank_lsb[ch] = 0xFF;
        reader->last_program[ch] = 0xFF;
        for (int n = 0; n < 128; n++) reader->open_notes[ch][n] = false;
    }

    for (uint16_t t = 0; t < reader->num_tracks; t++) {
        MidiStreamTrack *tr = &reader->tracks[t];
        tr->bytes_consumed = 0;
        tr->buf_pos = 0;
        tr->buf_len = 0;
        tr->cur_tick = 0;
        tr->run_status = 0;
        tr->is_eof = false;
        tr->has_event = false;
        tr->tp_hint = 0;
        stream_track_fetch_next_event(reader, (uint8_t)t);
    }
}

uint32_t midi_stream_close(MidiStreamReader *reader, MidiEvent *out_events, uint32_t max_events)
{
    if (!reader) return 0;

    uint32_t forced = 0;
    if (out_events && max_events > 0) {
        uint32_t force_ts = 0;
        for (int ch = 0; ch < 16 && forced < max_events; ch++) {
            for (int n = 0; n < 128 && forced < max_events; n++) {
                if (reader->open_notes[ch][n]) {
                    MidiEvent *ev = &out_events[forced++];
                    ev->timestamp_samples = force_ts;
                    ev->sequence = reader->next_sequence++;
                    ev->type = MIDI_STATUS_NOTE_OFF;
                    ev->channel = (uint8_t)ch;
                    ev->data1 = (uint8_t)n;
                    ev->data2 = 0;
                    reader->open_notes[ch][n] = false;
                }
            }
        }
    }

    reader->is_eof = true;
    return forced;
}

