/**
 * @file sd_midi.h
 * @brief SD カード内 Standard MIDI File の走査・ロード モジュール
 * @details NuttX (Spresense 拡張ボード /mnt/sd0) とホスト (./sdmidi/) 両対応。
 */

#ifndef SD_MIDI_H_
#define SD_MIDI_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "midi_parser.h"

/* Main 側イベントプール (sd_player.c で定義) を外部参照 */
extern MidiEvent g_sd_player_pool[];

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MIDI_MAX_FILES   (32)
#define SD_MIDI_MAX_BYTES   (2048u * 1024u)  /* 1 ファイル上限 2MB (ストリーミングパースによりRAM消費0) */
#define SD_MIDI_MAX_EVENTS  (10240u)         /* P0-B 39136B対応: 12288→10240で24KB節約、ENOMEM回避 (2026-09-02) */
#define SD_MIDI_NAME_LEN    (48)
#define SD_MIDI_FS_NAME_LEN (128)

typedef struct {
    char     name[SD_MIDI_NAME_LEN]; /**< 表示用ファイル名 (拡張子除く・切詰めあり) */
    char     fs_name[SD_MIDI_FS_NAME_LEN]; /**< 実ファイル名 (拡張子込み・ロード用) */
    uint32_t size;                   /**< バイト数 */
} SdMidiEntry;

typedef struct {
    SdMidiEntry files[SD_MIDI_MAX_FILES];
    uint32_t    count;
    int32_t     dir_index; /**< scan でヒットした探索ディレクトリの番号 (load で使用) */
} SdMidiList;

/**
 * @brief SD (またはホストの ./sdmidi/) 内の .mid/.smf を走査する
 * @return 見つかったファイル数 (0 なら利用可能な曲なし)
 */
uint32_t sd_midi_scan(SdMidiList *list);

/**
 * @brief 実機: microSD の /mnt/sd0 へのマウントを保証する (最大 ~1 秒リトライ)
 * @details CXD56_SDIO ドライバが有効な環境で /dev/mmcsd0 を vfat として
 *          マウントする。未挿入/既マウント等の失敗は黙って無視する。
 *          boot_diag も本関数の成果を利用するため、
 *          起動シーケンス早期に 1 回呼び出すこと。
 */
void sd_midi_ensure_mount(void);

/**
 * @brief 指定インデックスの MIDI ファイルをバイナリオープンする
 * @param list 走査結果リスト
 * @param index ファイルインデックス
 * @return 成功時はオープン済み FILE*、失敗時は NULL
 */
FILE *sd_midi_open_file(const SdMidiList *list, uint32_t index);

/**
 * @brief 指定インデックスの MIDI をロードしてパースする
 * @param out 解析結果 (呼び出し側で midi_parser_free_song / sd_midi_unload すること)
 * @return 成功時 0 / 失敗時 負値 (スキップして次候補を試すこと)
 */
int sd_midi_load_file(SdMidiList *list, uint32_t index, MidiSong *out);

#ifdef __cplusplus
}
#endif

#endif /* SD_MIDI_H_ */
