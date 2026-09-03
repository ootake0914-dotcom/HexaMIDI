/**
 * @file worker_compat.c
 * @brief SubCore ワーカー ELF 用 最小限 POSIX 互換レイヤー
 * @details libasmpw (CONFIG_ASMP_WORKER_LIBC) は string/libm のみを提供するため、
 *          ワーカー側で必要になる rand/usleep/clock_gettime/qsort/malloc/free を
 *          ワーカー環境で利用可能なプリミティブ (clock_getcpubaseclock / wk_udelay)
 *          の上に実装する。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* libasmpw 提供のプリミティブ */
extern uint32_t clock_getcpubaseclock(void);
extern void wk_udelay(uint32_t us);

#define WC_CPU_HZ 156000000ull

/* ---------------------------------------------------------------- */
/* rand / srand                                                      */
/* ---------------------------------------------------------------- */

static uint32_t s_wc_seed = 1;

long rand(void)
{
    /* LCG (ANSI C 準拠のパラメータ, 下位16bitを返す) */
    s_wc_seed = s_wc_seed * 1103515245u + 12345u;
    return (long)((s_wc_seed >> 16) & 0x7fffu);
}

void srand(unsigned int seed)
{
    s_wc_seed = (uint32_t)seed ? (uint32_t)seed : 1u;
}

/* ---------------------------------------------------------------- */
/* usleep (ビジーウェイト: ワーカーには OS タイマが無い)              */
/* ---------------------------------------------------------------- */

int usleep(useconds_t usec)
{
    wk_udelay((uint32_t)usec);
    return 0;
}

int fflush(void *stream)
{
    (void)stream;
    return 0;
}

/* ---------------------------------------------------------------- */
/* clock_gettime(CLOCK_MONOTONIC) — ARM Cortex-M4 DWT サイクルから導出 */
/* ---------------------------------------------------------------- */

#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004u)
#define DEMCR       (*(volatile uint32_t *)0xE000EDFCu)

static inline uint32_t get_dwt_cycles(void)
{
    static bool s_dwt_inited = false;
    if (!s_dwt_inited) {
        DEMCR |= 0x01000000u;      /* TRCENA 有効化 */
        DWT_CTRL |= 0x00000001u;   /* CYCCNTENA 有効化 */
        s_dwt_inited = true;
    }
    return DWT_CYCCNT;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    if (!tp) return -1;
    if (clk_id != CLOCK_MONOTONIC && clk_id != CLOCK_REALTIME) {
        return -1;
    }
    /* DWT CYCCNT は 32bit CPU サイクル (156MHz で ~27.5 秒毎にラップ)。
     * 前回値より減ったら 2^32 を加算した仮想 64bit 時系列を構成し、
     * 負荷計測等の差分演算がラップで破壊されないようにする */
    static uint64_t s_ext_cycles = 0;
    static uint32_t s_prev_raw = 0;
    uint32_t raw = get_dwt_cycles();
    if (raw < s_prev_raw) {
        s_ext_cycles += (uint64_t)1 << 32;
    }
    s_prev_raw = raw;
    uint64_t cycles = s_ext_cycles + (uint64_t)raw;

    tp->tv_sec  = (time_t)(cycles / WC_CPU_HZ);
    tp->tv_nsec = (long)(((cycles % WC_CPU_HZ) * 1000000000ull) / WC_CPU_HZ);
    return 0;
}

/* ---------------------------------------------------------------- */
/* qsort (シェルソート: SMF イベント整列 ~4096 件を想定)              */
/* ---------------------------------------------------------------- */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    char *a = (char *)base;
    if (!a || nmemb < 2 || size == 0) return;

    /* Knuth のギャップ列 (1,4,13,40,...) */
    size_t gap = 1;
    while (gap < nmemb / 3u) gap = gap * 3u + 1u;

    /* スワップは 1 バイト毎の交換のため任意の要素サイズで動作する
     * (旧実装は size > 64 で黙って何もせず、ソートされない地雷だった) */
    for (; gap > 0; gap /= 3u) {
        for (size_t i = gap; i < nmemb; i++) {
            for (size_t j = i; j >= gap; j -= gap) {
                char *pj = a + (j - gap) * size;
                char *pjj = a + j * size;
                if (compar(pj, pjj) <= 0) break;
                for (size_t k = 0; k < size; k++) {
                    char t = pj[k]; pj[k] = pjj[k]; pjj[k] = t;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* malloc / free (静的バンプアロケータ)                               */
/* ワーカーでは通常スタティックプールを使用するため、リンカ解決用の   */
/* フォールバックとして最小限の実装を提供する                        */
/* ---------------------------------------------------------------- */

#define WC_HEAP_SIZE (16u * 1024u) /* ゴールデン復帰: 8Kは音飛びリスク (ヒープ枯渇でqsort失敗) */
static uint8_t  s_wc_heap[WC_HEAP_SIZE] __attribute__((aligned(8)));
static uint32_t s_wc_heap_pos = 0;

void *malloc(size_t size)
{
    if (size == 0) size = 1;
    size = (size + 7u) & ~(size_t)7u;
    if (size > (size_t)(WC_HEAP_SIZE - s_wc_heap_pos)) {
        return NULL; /* ワーカー内ヒープ枯渇 */
    }
    void *ptr = &s_wc_heap[s_wc_heap_pos];
    s_wc_heap_pos += (uint32_t)size;
    return ptr;
}

void free(void *ptr)
{
    /* バンプアロケータのため解放なし (ワーカーは長期稼働しない前提) */
    (void)ptr;
}

void *calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > SIZE_MAX / size) return NULL; /* オーバーフローガード */
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}
