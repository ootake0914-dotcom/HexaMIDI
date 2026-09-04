/**
 * @file gen_wavbank.c
 * @brief ホスト専用ツール: ROMウェーブテーブルバンク生成
 * @details sub_common.h の subwav_init() と同一コードでテーブルを生成し、
 *          asmp_sub/sub_wavbank.c (const ROM データ) として出力する。
 *          %a (hex float) 出力のため bit 完全一致が保証される。
 *
 * 使い方:
 *   gcc -O2 -Iinclude -Iasmp_sub tools/gen_wavbank.c -o /tmp/gen_wavbank -lm
 *   /tmp/gen_wavbank > asmp_sub/sub_wavbank.c
 */
#include <stdio.h>
#include "sub_common.h"

int main(void)
{
    static SubWavBank bank;
    subwav_init(&bank);

    printf("/* 自動生成ファイル。手編集禁止。\n");
    printf(" * 生成: tools/gen_wavbank.c (subwav_init と bit 一致、%%a 出力)\n");
    printf(" * 内容: Sub2 ウェーブテーブル 8種 x 6mip (SubWavBank 相当、ROM 配置) */\n");
    printf("#include \"sub_common.h\"\n\n");
    printf("const float g_sub_wavbank[SUBWT_MIPS][SUBWT_TABLES][SUBWT_SIZE + 1] =\n{\n");
    for (int mip = 0; mip < SUBWT_MIPS; mip++) {
        printf("    { /* mip %d */\n", mip);
        for (int t = 0; t < SUBWT_TABLES; t++) {
            printf("        {");
            for (int n = 0; n <= SUBWT_SIZE; n++) {
                if ((n % 4) == 0) {
                    printf("\n            ");
                }
                printf("%a%s", (double)bank.table[mip][t][n],
                       (n < SUBWT_SIZE) ? "," : "");
                if ((n % 4) != 3 && n < SUBWT_SIZE) {
                    printf(" ");
                }
            }
            printf("}%s /* table %d */\n", (t < SUBWT_TABLES - 1) ? "," : "", t);
        }
        printf("    }%s\n", (mip < SUBWT_MIPS - 1) ? "," : "");
    }
    printf("};\n");
    return 0;
}
