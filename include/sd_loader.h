/**
 * @file sd_loader.h
 * @brief SD カード MIDI 非同期ローダー (音声ループ外でのブロッキング IO)
 */

#ifndef SD_LOADER_H_
#define SD_LOADER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "midi_parser.h"

/**
 * @brief ローダー結果コード
 */
typedef enum {
    SD_LOADER_RESULT_NONE = 0,   /**< 結果なし (まだ処理中) */
    SD_LOADER_RESULT_LOADED,     /**< 演奏可能曲を 1 曲ロードした (out_song を所有) */
    SD_LOADER_RESULT_NO_FILES,   /**< カード未挿入 / MIDI ファイルなし */
    SD_LOADER_RESULT_EXHAUSTED   /**< 続きの演奏可能ファイルがない (レーン停止用) */
} SdLoaderResult;

/**
 * @brief ワーカースレッド起動
 * @retval 0 成功 / -1 失敗 (スレッド作成不可)
 */
int sd_loader_start(void);

/**
 * @brief ワーカースレッド停止 (未取得の曲があれば解放)
 */
void sd_loader_stop(void);
void sd_loader_stop_stream(void);

/**
 * @brief マウント/スキャン/ロード実行中か (重複要求防止用)
 */
bool sd_loader_busy(void);

/**
 * @brief 先頭曲ロード要求 (mount -> scan -> index 0 から順に試行)。
 *        結果は sd_loader_poll() で受け取る。ノンブロッキング。
 * @retval true 要求受理 / false ワーカー busy で拒否 (後で再要求すること)
 */
bool sd_loader_request_first(void);

/**
 * @brief 継続ロード要求 (last_played_index + 1 から順に試行)。
 *        全ファイル試行後も不可なら EXHAUSTED を返す。
 * @retval true 要求受理 / false ワーカー busy で拒否
 */
bool sd_loader_request_next(uint32_t last_played_index);

/**
 * @brief 前曲ロード要求 (last_played_index - 1 から逆方向に試行)。
 *        先頭の前は末尾へ巻き戻す。全ファイル試行後も不可なら EXHAUSTED。
 * @retval true 要求受理 / false ワーカー busy で拒否
 */
bool sd_loader_request_prev(uint32_t last_played_index);

/**
 * @brief 完了結果のポーリング (ノンブロッキング、1 回の結果は 1 度だけ返す)
 * @param[out] res      結果コード
 * @param[out] out_song LOADED 時のみ有効な MidiSong (呼び出し側が所有・解放)
 * @param[out] out_index ロードしたファイルインデックス (LOADED 時)
 * @retval true 結果を取得した / false なし
 */
bool sd_loader_poll(SdLoaderResult *res, MidiSong *out_song, uint32_t *out_index);

/**
 * @brief 表示用スナップショット (スレッド安全コピー)。UI/テレメトリ用 (~10Hz 想定)
 */
void sd_loader_get_info(uint32_t *count, uint32_t *index, char *name, size_t namesz);

#endif /* SD_LOADER_H_ */
