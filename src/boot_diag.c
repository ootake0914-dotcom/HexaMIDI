/**
 * @file boot_diag.c
 * @brief 実機ブート診断: SD ログ + ビープコード (シリアル不可視環境用)
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#include <unistd.h>
#include <time.h>
#include <arch/board/board.h>
#include <arch/board/cxd56_audio.h>
#endif

#include "boot_diag.h"

/* 診断ビープの有効化フラグ (実機調査時のみ 1 にする)。通常は無音 */
#ifndef BOOT_DIAG_BEEP_ENABLE
#define BOOT_DIAG_BEEP_ENABLE 0
#endif

#ifdef __NuttX__
static const char *const k_diag_paths[] = {
    "/mnt/sd0/hexa_boot.log",
    "/mnt/sd1/hexa_boot.log",
};

static uint32_t diag_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u));
}
#endif

void boot_diag_log(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

#ifdef __NuttX__
    const uint32_t ms = diag_now_ms();
    for (int i = 0; i < (int)(sizeof(k_diag_paths) / sizeof(k_diag_paths[0])); i++) {
        FILE *fp = fopen(k_diag_paths[i], "a");
        if (!fp) {
            continue;
        }
        fprintf(fp, "[%10u.%03u] %s\n",
                (unsigned)(ms / 1000u), (unsigned)(ms % 1000u), line);
        fclose(fp);
    }
    printf("[DIAG] %s\n", line);
    fflush(stdout);
#else
    fprintf(stderr, "[DIAG] %s\n", line);
#endif
}

void boot_diag_beep(int count, int hz)
{
#if BOOT_DIAG_BEEP_ENABLE && defined(__NuttX__)
    for (int i = 0; i < count; i++) {
        board_audio_tone_generator(1, 0, hz);
        usleep(90000);
        board_audio_tone_generator(0, 0, 0);
        usleep(70000);
    }
#else
    (void)count;
    (void)hz;
#endif
}
