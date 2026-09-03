/**
 * @file preset_songs.h
 * @brief プリセット楽曲データ管理モジュール
 * @details 最新ヒット曲およびクラシック定番曲の高精度MIDIノートデータ定義と楽曲情報提供
 */

#ifndef PRESET_SONGS_H_
#define PRESET_SONGS_H_

#include <stdint.h>
#include <stdbool.h>
#include "synth_engine.h"
#include "sequencer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 全プリセット楽曲配列の取得
 * @param count 楽曲数を格納するポインタ（NULL指定可）
 * @return プリセット楽曲配列の先頭ポインタ
 */
const Track* preset_songs_get_all(uint32_t *count);

/**
 * @brief 指定インデックスのプリセット楽曲を取得
 * @param index 楽曲インデックス
 * @return Track構造体へのポインタ（範囲外の場合はNULL）
 */
const Track* preset_songs_get_by_index(uint32_t index);

/**
 * @brief プリセット楽曲の総数を取得
 * @return 登録されている楽曲総数
 */
uint32_t preset_songs_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PRESET_SONGS_H_ */
