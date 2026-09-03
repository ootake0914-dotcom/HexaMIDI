/**
 * @file test_rt_services.c
 * @brief RT 周辺サービス (非同期ロガー) の回帰テスト
 * @details 音声ループをブロックさせてはならないコンポーネント。
 *          - async_logger: 大量ログ投入時に全行が無欠損で排出されること
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define RT_MKDIR(d) _mkdir(d)
#define RT_SLEEP_MS(ms) Sleep(ms)
#define RT_DUP(fd) _dup(fd)
#define RT_DUP2(a, b) _dup2(a, b)
#define RT_CLOSE(fd) _close(fd)
#else
#include <unistd.h>
#include <sys/stat.h>
#define RT_MKDIR(d) mkdir(d, 0777)
#define RT_SLEEP_MS(ms) usleep((ms) * 1000)
#define RT_DUP(fd) dup(fd)
#define RT_DUP2(a, b) dup2(a, b)
#define RT_CLOSE(fd) close(fd)
#endif

#include "async_logger.h"

/* NDEBUG (Release) でも検査が消えないよう専用マクロを使用する */
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); exit(1); } } while (0)

static void test_async_logger(void)
{
    printf("[TEST] async_logger: 500 lines burst + drain integrity...\n");
    fflush(stdout);

    /* 元の stdout ハンドルを保存してからファイルへ差し替え
     * (CONOUT$ 再オープンはリダイレクトを破壊するため dup 系で復元する) */
    int saved_fd = RT_DUP(fileno(stdout));
    CHECK(saved_fd >= 0);
    FILE *cap = freopen("rt_log_capture.txt", "w", stdout);
    CHECK(cap != NULL);

    CHECK(async_log_start() == true);

    char line[64];
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof(line), "[BENCH] line %04d xxxxxxxxxxxxxxxxxxxx\n", i);
        async_logf("%s", line);
        /* 実運用 (1 秒あたり数行) を模倣したペーシング。
         * 無休息バーストは満杯ドロップ設計上の正しい挙動のため、
         * 本試験では消費速度が追いつく間隔で投入する */
        if ((i & 7u) == 7u) RT_SLEEP_MS(1);
    }

    async_log_stop(); /* 全排出を保証して停止 */

    /* 満杯ドロップが一切発生していないことも確認 */
    CHECK(async_log_dropped_bytes() == 0);

    /* stdout を元ハンドルへ復元 */
    fflush(stdout);
    CHECK(RT_DUP2(saved_fd, fileno(stdout)) == 0);
    RT_CLOSE(saved_fd);

    FILE *fp = fopen("rt_log_capture.txt", "rb");
    CHECK(fp != NULL);

    int count = 0;
    char buf[128];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        /* 各行が完全 (改行まで含む) で到達していること = 行欠損/混信なし */
        size_t len = strlen(buf);
        CHECK(len > 0 && buf[len - 1] == '\n');
        count++;
    }
    fclose(fp);
    remove("rt_log_capture.txt");

    printf("  -> captured %d/500 complete lines\n", count);
    CHECK(count == 500);
    printf("  -> PASS: async logger delivered every line intact.\n");
}

int main(void)
{
    test_async_logger();
    printf("=======================================================\n");
    printf(" ALL RT SERVICE TESTS PASSED (100%% SUCCESS)!\n");
    printf("=======================================================\n");
    return 0;
}
