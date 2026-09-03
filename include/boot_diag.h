#ifndef BOOT_DIAG_H
#define BOOT_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 実機診断: シリアルが見えない環境でも SD (/mnt/sd0/hexa_boot.log) と
 * ビープで起動状況を追跡できるようにするユーティリティ */

void boot_diag_log(const char *fmt, ...);
void boot_diag_beep(int count, int hz);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_DIAG_H */
