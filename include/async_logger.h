/**
 * @file async_logger.h
 * @brief 非同期ログ: 音声ループから stdout (UART) へのブロッキング書き込みを分離
 * @details リアルタイムスレッドは vsnprintf + リングバッファ投入のみ行い、
 *          低優先度ワーカースレッドが実際の UART 出力を担う。
 *          Spresense の 115200bps UART では 1 行 ~130 文字の printf に
 *          約 11ms かかり、10.7ms の音声バジェットを超過してプチノイズに
 *          なっていた問題の対策。
 *
 * 規約:
 *  - プロデューサは 1 スレッドのみ (SPSC)。MainCore 音声ループを想定。
 *  - バッファ満杯時は破棄してカウントする (ブロックしない)。
 *  - async_log_start() 以降、他スレッドからの直接 printf は出力が
 *    interleave し得るため避けること。
 */

#ifndef ASYNC_LOGGER_H_
#define ASYNC_LOGGER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ワーカースレッド起動。失敗時は false (呼び出し元は同期 printf にフォールバック) */
bool async_log_start(void);

/** 排出待ち -> スレッド停止。プロセス終了時に呼ぶ */
void async_log_stop(void);

/** キュー排出完了を待つだけ (停止はしない) */
void async_log_flush(void);

/** printf 形式で非同期出力 (音声ループから呼んでよい) */
void async_logf(const char *fmt, ...);

/** テスト/診断用: 開始以来の破棄バイト数 */
uint32_t async_log_dropped_bytes(void);

/** テスト/診断用: 未出力バイト数 */
size_t async_log_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* ASYNC_LOGGER_H_ */
