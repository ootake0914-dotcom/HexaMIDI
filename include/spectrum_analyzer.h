/**
 * @file spectrum_analyzer.h
 * @brief 8バンド Goertzel スペクトラム解析モジュール
 */

#ifndef SPECTRUM_ANALYZER_H_
#define SPECTRUM_ANALYZER_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_BANDS (8)

/* スケーリング定数:
 * フルスケールステレオ振幅 65534.0f (32767*2), Goertzel ピーク利得 N/2 = 512.0f (1024窓)
 * SPECTRUM_DB_OFFSET = -20 * log10(65534.0 * 512.0) ≒ -150.5147f
 */
#define SPECTRUM_DB_OFFSET (-150.5147f)

/**
 * @brief PCM チャンクを解析してバンド強度を更新 (呼び出し側で平滑化済み)
 */
void spectrum_update(const int16_t *pcm, uint32_t frames);

/**
 * @brief 平滑化済みバンド強度配列 (dBFS, 通常 -60.0f .. 0.0f) への参照 (8 要素)
 */
const float *spectrum_levels(void);

/**
 * @brief 現在のスペクトラムを ASCII バーで 1 行表示
 */
void spectrum_print(void);

/**
 * @brief 現在のスペクトラムを ASCII バー文字列へ整形 (printf 不要のログ経路用)
 * @return 書き込んだバイト数
 */
size_t spectrum_format(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SPECTRUM_ANALYZER_H_ */

