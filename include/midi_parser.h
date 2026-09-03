/**
 * @file midi_parser.h
 * @brief 標準 MIDI ファイル (Standard MIDI File: SMF Format 0/1) パーサー
 * @details 複数トラックのマージ、可変長デルタタイムデコード、絶対サンプル時間への変換
 */

#ifndef MIDI_PARSER_H_
#define MIDI_PARSER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI ステータスバイト定義 */
#define MIDI_STATUS_NOTE_OFF         (0x80)
#define MIDI_STATUS_NOTE_ON          (0x90)
#define MIDI_STATUS_POLY_AFTERTOUCH  (0xA0)
#define MIDI_STATUS_CONTROL_CHANGE   (0xB0)
#define MIDI_STATUS_PROGRAM_CHANGE   (0xC0)
#define MIDI_STATUS_CHANNEL_PRESSURE (0xD0)
#define MIDI_STATUS_PITCH_BEND       (0xE0)
#define MIDI_STATUS_META_OR_SYSEX    (0xF0)

/* GM コントロールチェンジ番号 */
#define MIDI_CC_BANK_SELECT_MSB      (0)
#define MIDI_CC_MODULATION           (1)
#define MIDI_CC_VOLUME               (7)
#define MIDI_CC_PAN                  (10)
#define MIDI_CC_EXPRESSION           (11)
#define MIDI_CC_SUSTAIN_PEDAL        (64)
#define MIDI_CC_REVERB_SEND          (91)
#define MIDI_CC_ALL_SOUND_OFF        (120)
#define MIDI_CC_ALL_NOTES_OFF        (123)

/* GM ドラム専用チャンネル (1-based: ch10 -> 0-based: 9) */
#define MIDI_DRUM_CHANNEL            (9)

/* 統合 MIDI イベント構造体 (12バイト: 4Bタイムスタンプ + 4Bシーケンス + 4x1Bメッセージデータ + パディング) */
typedef struct {
    uint32_t timestamp_samples; /**< 再生開始からの絶対時間 (サンプル数 @ 48kHz) */
    uint32_t sequence;          /**< 入力順序 (安定ソート用) */
    uint8_t  type;              /**< MIDIメッセージタイプ (0x80, 0x90, 0xB0, 0xC0, 0xE0 等) */
    uint8_t  channel;           /**< チャンネル番号 (0〜15) */
    uint8_t  data1;             /**< ノート番号 / CC番号 / プログラム番号 */
    uint8_t  data2;             /**< ベロシティ / CC値 / ピッチベンドMSB (data1=LSB) */
} MidiEvent;

/* MIDI 楽曲データ構造体 */
typedef struct {
    char title[64];             /**< 楽曲タイトル */
    MidiEvent *events;          /**< 時系列ソート済みイベント配列 */
    bool events_dynamic;        /**< events が malloc 動的確保か (false: 外部静的プール) */
    uint32_t event_count;       /**< 総イベント数 */
    uint32_t total_samples;     /**< 総演奏時間 (サンプル数) */
    uint16_t format;            /**< SMF フォーマット (0 または 1) */
    uint16_t num_tracks;        /**< トラック数 */
    uint16_t ticks_per_quarter; /**< 四分音符あたりの分解能 (TPQN) */
    uint32_t initial_tempo_us;  /**< 初期テンポ (マイクロ秒/四分音符, デフォルト 500000 = BPM 120) */
} MidiSong;

/**
 * @brief メモリ上の SMF (Standard MIDI File) バイナリデータをパースし MidiSong を構築
 * @param data MIDI ファイルの生バイナリデータ
 * @param size データサイズ (バイト数)
 * @param song パース結果を格納する MidiSong 構造体へのポインタ
 * @return 成功時は 0、エラー時は負のエラーコード
 */
int midi_file_load_memory(const uint8_t *data, size_t size, MidiSong *song);

/**
 * @brief 拡張パラメータ付き SMF バイナリデータパース API
 * @param data MIDI ファイルの生バイナリデータ
 * @param size データサイズ (バイト数)
 * @param song パース結果を格納する MidiSong 構造体へのポインタ
 * @param event_pool イベントを格納する静的バッファ (NULLの場合は動的確保)
 * @param max_events イベントバッファの最大容量
 * @param sample_rate ターゲットサンプリングレート (48000 Hz)
 * @return 成功時は 0、エラー時は負のエラーコード
 */
int midi_parser_load_memory(const uint8_t *data, size_t size, MidiSong *song,
                            MidiEvent *event_pool, uint32_t max_events, uint32_t sample_rate);

/**
 * @brief ファイルポインタから直接 SMF をストリーミングパース (生データの巨大mallocを完全排除)
 * @param fp オープン済みのバイナリ読み取りファイルポインタ
 * @param song パース結果を格納する MidiSong 構造体へのポインタ
 * @param event_pool イベントを格納する静的バッファ (NULLの場合は動的確保)
 * @param max_events イベントバッファの最大容量
 * @param sample_rate ターゲットサンプリングレート (48000 Hz)
 * @return 成功時は 0、エラー時は負のエラーコード
 */
int midi_parser_load_file(FILE *fp, MidiSong *song,
                          MidiEvent *event_pool, uint32_t max_events, uint32_t sample_rate);

/**
 * @brief MidiSong 構造体のリソース解放
 * @param song MidiSong 構造体へのポインタ
 */
void midi_parser_free_song(MidiSong *song);

/* ========================================================================= */
/* 5. ストリーミング (k-way マージ・オンデマンド先読み) API                  */
/* ========================================================================= */
#define MIDI_STREAM_MAX_TRACKS (32)
#define MIDI_STREAM_TRACK_BUF  (256)
#define MIDI_STREAM_MAX_TEMPOS (256)

typedef struct {
    uint32_t tick;
    uint32_t tempo_us;
    uint32_t sample_offset;
} MidiStreamTempoPoint;

typedef struct {
    long      file_data_pos;       /**< トラックデータ開始位置 (MTrkヘッダ直後) */
    uint32_t  track_size;          /**< トラックデータ長 (バイト) */
    uint32_t  bytes_consumed;      /**< トラック内で消費した総バイト数 */
    long      buf_file_offset;     /**< バッファ先頭のファイル位置 */
    uint16_t  buf_pos;             /**< バッファ内読取インデックス */
    uint16_t  buf_len;             /**< バッファ内有効データ長 */
    uint8_t   buf[MIDI_STREAM_TRACK_BUF]; /**< トラック個別I/Oバッファ (256B) */

    uint64_t  cur_tick;            /**< このトラックの累積tick */
    uint8_t   run_status;          /**< ランニングステータス */
    bool      is_eof;              /**< トラック終了フラグ */
    bool      has_event;           /**< pending_event が未消費か */
    MidiEvent pending_event;       /**< k-way merge 比較用の先読みイベント */
    uint32_t  tp_hint;             /**< tick_to_sample 用テンポindexヒント (単調前進。rewindで0) */
} MidiStreamTrack;

typedef struct {
    FILE           *fp;
    char            title[64];
    uint16_t        format;             /**< SMF Format 0 / 1 */
    uint16_t        num_tracks;         /**< 有効トラック数 */
    uint16_t        ticks_per_quarter;  /**< 分解能 (TPQN) */
    uint32_t        sample_rate;        /**< 48000 Hz */
    uint32_t        initial_tempo_us;   /**< 初期テンポ (通常 500000) */
    uint32_t        total_samples;      /**< 概算総サンプル数 (Conductor Track等から算出) */
    uint32_t        next_sequence;      /**< 安定ソート用シーケンス番号 */
    bool            is_eof;             /**< 全トラック終了フラグ */

    uint32_t        num_tempos;
    MidiStreamTempoPoint tempo_map[MIDI_STREAM_MAX_TEMPOS];

    /* 内部状態: CC/PC重複除去、オープンノート追跡 */
    uint8_t         last_cc[16][10];
    uint8_t         last_bank_msb[16];
    uint8_t         last_bank_lsb[16];
    uint8_t         last_program[16];
    bool            has_bank[16];
    bool            open_notes[16][128];

    MidiStreamTrack tracks[MIDI_STREAM_MAX_TRACKS];
} MidiStreamReader;

/**
 * @brief ファイルポインタからストリーミングリーダーを初期化
 * @details ヘッダ解析、Conductor Track によるテンポマップ構築、全トラックの初期イベント先読みを行う
 * @param reader リーダー構造体へのポインタ
 * @param fp オープン済みの SMF ファイルポインタ (rb モード)
 * @param sample_rate サンプリングレート (48000)
 * @return 成功時 0、エラー時 負値
 */
int midi_stream_open(MidiStreamReader *reader, FILE *fp, uint32_t sample_rate);

/**
 * @brief ストリーミングリーダーから時系列順にイベントを逐次取得 (k-way マージ)
 * @param reader リーダー構造体へのポインタ
 * @param out_events 出力先イベント配列
 * @param max_events 取得する最大イベント数
 * @return 実際に取得できたイベント数 (0 の場合は全曲終了 EOF)
 */
uint32_t midi_stream_read(MidiStreamReader *reader, MidiEvent *out_events, uint32_t max_events);

/**
 * @brief ストリーミングリーダーの曲頭への巻き戻し (ループ・リワインド用)
 */
void midi_stream_rewind(MidiStreamReader *reader);

/**
 * @brief ストリーミングリーダーの終了・保留ノート強制解放
 * @param reader リーダー構造体へのポインタ
 * @param out_events 終了時に強制NoteOffを格納する配列 (NULL可)
 * @param max_events 配列長
 * @return 生成された強制NoteOff数
 */
uint32_t midi_stream_close(MidiStreamReader *reader, MidiEvent *out_events, uint32_t max_events);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_PARSER_H_ */
