/**
 * @file async_logger.c
 * @brief 非同期ログ実装 (SPSC リングバッファ + 低優先度出力スレッド)
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "async_logger.h"

/* ---- メモリバリア (asmp_protocol.h と同一のポータブル定義) -------------- */
#if defined(__GNUC__) || defined(__clang__)
#  define ALOG_BARRIER() __sync_synchronize()
#elif defined(_MSC_VER)
#  define ALOG_BARRIER() _ReadWriteBarrier()
#else
#  define ALOG_BARRIER()
#endif

#define ALOG_BUF_SIZE  (4096u)          /* 2の冪。1 秒分の出力 ~600B の 6 倍以上 */
#define ALOG_BUF_MASK  (ALOG_BUF_SIZE - 1u)

static char     s_ring[ALOG_BUF_SIZE];
static volatile uint32_t s_head = 0;    /* プロデューサのみ加算 */
static volatile uint32_t s_tail = 0;    /* コンシューマのみ加算 */

static volatile bool    s_running = false;
static volatile uint32_t s_dropped = 0;

#ifdef _WIN32
static HANDLE s_thread = NULL;
static volatile bool s_thread_valid = false;
#else
static pthread_t s_thread;
static bool s_thread_valid = false;
#endif

uint32_t async_log_dropped_bytes(void) { return s_dropped; }

size_t async_log_pending(void)
{
    uint32_t h = s_head;
    ALOG_BARRIER();
    uint32_t t = s_tail;
    return (size_t)((h + ALOG_BUF_SIZE - t) & ALOG_BUF_MASK);
}

void async_logf(const char *fmt, ...)
{
    if (!fmt) return;

    char linebuf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(linebuf, sizeof(linebuf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(linebuf)) n = (int)(sizeof(linebuf) - 1);

    if (!s_running) {
        /* スレッド未起動: 同期出力へフォールバック (起動前ログ用) */
        fwrite(linebuf, 1, (size_t)n, stdout);
        fflush(stdout);
        return;
    }

    /* SPSC 投入: 空きが足りなければ全て破棄 (部分書きで行を壊さない) */
    uint32_t tail = s_tail;
    ALOG_BARRIER();
    uint32_t free_space = (uint32_t)((tail + ALOG_BUF_SIZE - 1u - s_head) & ALOG_BUF_MASK);
    if ((uint32_t)n > free_space) {
        s_dropped += (uint32_t)n;
        return;
    }

    uint32_t head = s_head;
    uint32_t off  = head & ALOG_BUF_MASK;
    uint32_t first = ALOG_BUF_SIZE - off;
    if ((uint32_t)n <= first) {
        memcpy(&s_ring[off], linebuf, (size_t)n);
    } else {
        memcpy(&s_ring[off], linebuf, first);
        memcpy(&s_ring[0], &linebuf[first], (size_t)n - first);
    }
    ALOG_BARRIER();
    s_head = head + (uint32_t)n;
}

void async_log_flush(void)
{
    while (s_running && s_tail != s_head) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(500);
#endif
    }
}

static void alog_write_stdout(const char *data, size_t len)
{
    fwrite(data, 1, len, stdout);
}

#ifdef _WIN32
static DWORD WINAPI alog_thread_entry(LPVOID arg)
#else
static void *alog_thread_entry(void *arg)
#endif
{
    (void)arg;
    uint32_t reported_drop = 0;

    while (s_running) {
        uint32_t tail = s_tail;
        uint32_t head = s_head;
        if (tail == head) {
            fflush(stdout);
            /* 通常運用ではログは稀 (イベント発生時のみ)。
             * 起床頻度を落としてコンテキストスイッチを削除する
             * (ログ表示の許容遅延は数百 ms 級で問題ない) */
#ifdef _WIN32
            Sleep(8);
#else
            usleep(8000);
#endif
            continue;
        }

        ALOG_BARRIER();
        size_t avail = (size_t)((head + ALOG_BUF_SIZE - tail) & ALOG_BUF_MASK);

        /* 連続領域単位で吐き出す */
        uint32_t off  = tail & ALOG_BUF_MASK;
        size_t   cont = ALOG_BUF_SIZE - off;
        if (cont > avail) cont = avail;
        alog_write_stdout(&s_ring[off], cont);

        tail += (uint32_t)cont;
        if (avail - cont > 0) {
            size_t rest = avail - cont;
            alog_write_stdout(s_ring, rest);
            tail += (uint32_t)rest;
        }
        ALOG_BARRIER();
        s_tail = tail;

        uint32_t drop = s_dropped;
        if (drop != reported_drop) {
            printf("[LOG] dropped %u bytes (UART congestion)\n",
                   (unsigned int)(drop - reported_drop));
            reported_drop = drop;
        }
    }

    /* 残りを排出して終了 */
    for (;;) {
        uint32_t t = s_tail;
        uint32_t h = s_head;
        if (t == h) break;
        ALOG_BARRIER();
        size_t avail = (size_t)((h + ALOG_BUF_SIZE - t) & ALOG_BUF_MASK);
        uint32_t off = t & ALOG_BUF_MASK;
        size_t cont = ALOG_BUF_SIZE - off;
        if (cont > avail) cont = avail;
        alog_write_stdout(&s_ring[off], cont);
        ALOG_BARRIER();
        s_tail = t + (uint32_t)cont;
    }
    fflush(stdout);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

bool async_log_start(void)
{
    if (s_running) return true;

    s_tail = 0;
    s_head = 0;
    s_dropped = 0;
    ALOG_BARRIER();
    s_running = true;
    ALOG_BARRIER();

#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, alog_thread_entry, NULL, 0, NULL);
    if (s_thread == NULL) {
        s_running = false;
        return false;
    }
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&s_thread, &attr, alog_thread_entry, NULL) != 0) {
        pthread_attr_destroy(&attr);
        s_running = false;
        return false;
    }
    pthread_attr_destroy(&attr);
    s_thread_valid = true;
#endif
    return true;
}

void async_log_stop(void)
{
    if (!s_running) return;
    ALOG_BARRIER();
    s_running = false;
    ALOG_BARRIER();

#ifdef _WIN32
    if (s_thread != NULL) {
        WaitForSingleObject(s_thread, 3000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
#else
    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
#endif
}
