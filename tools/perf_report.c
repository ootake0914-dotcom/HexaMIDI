/**
 * @file perf_report.c
 * @brief メモリフットプリント / リソース使用量レポート (ホスト・実機共通)
 * @details Spresense CXD5602 (SRAM 1.5MB, Core 156MHz x6) への収束性を
 *          構造体サイズの実測で確認する。sizeof のみ使用するため
 *          実装コードに依存しない。
 */

#include <stdio.h>
#include <stdint.h>

#include "synth_engine.h"
#include "asmp_protocol.h"
#include "asmp_manager.h"

/* サブコア共通定義 (SubWavBank / SubSvf 等) */
#include "sub_common.h"

#define KB(x) ((unsigned long)(x))

int main(void)
{
    printf("=== HexaMIDI Footprint Report ===\n\n");

    /* --- 共有メモリ (Main <-> SubCore 5 分) --- */
    unsigned long ctx = sizeof(AsmpSharedContext);
    printf("[Shared context]\n");
    printf("  AsmpSharedContext        : %8lu B (slots=%u x %u frames x %u pcm)\n",
           KB(ctx), (unsigned)ASMP_NUM_SLOTS, (unsigned)ASMP_BUFFER_FRAMES,
           (unsigned)(ASMP_NUM_CORES));
    printf("  - pcm buffers            : %8lu B (src float x3 + master int16)\n",
           KB((unsigned long)ASMP_NUM_SLOTS * 3u * ASMP_BUFFER_FRAMES * 2u * sizeof(float) +
              (unsigned long)ASMP_NUM_SLOTS * ASMP_BUFFER_FRAMES * 2u * sizeof(int16_t)));
    printf("  - queues (%u x ring %u pkt): %7lu B\n",
           (unsigned)ASMP_NUM_CORES, (unsigned)ASMP_QUEUE_CAPACITY,
           KB((unsigned long)sizeof(AsmpRingBuffer) * ASMP_NUM_CORES));
    printf("\n");

    /* --- Main Core 静的確保分 --- */
    unsigned long eng = sizeof(SynthEngine);
    unsigned long mgr = sizeof(AsmpManager);
    unsigned long wav = sizeof(SubWavBank);
    unsigned long svf = sizeof(SubSvf);

    /* MIDI ストリーミングリングバッファ: 1024 events x 12B (=12KB) / ロガーリング: 4KB */
    const unsigned long midi_ring = 1024ul * 12ul;
    const unsigned long log_ring = 4096ul;

    printf("[Main Core statics]\n");
    printf("  SynthEngine              : %8lu B (fallback 合成エンジン)\n", KB(eng));
    printf("  AsmpManager              : %8lu B\n", KB(mgr));
    printf("  MIDI streaming ring      : %8lu B (1024 events)\n", KB(midi_ring));
    printf("  Async logger ring        : %8lu B\n", KB(log_ring));
    unsigned long main_total = eng + mgr + midi_ring + log_ring;
    printf("  -> Main subtotal         : %8lu B (%.1f%% of 1536KB)\n",
           KB(main_total), main_total * 100.0 / (1536.0 * 1024.0));
    printf("\n");

    /* --- SubCore 側の目安 --- */
    printf("[SubCore reference]\n");
    /* SubWavBank は ROM 常数 g_sub_wavbank へ移行済み (RAM 消費ゼロ)。参考値として表示 */
    printf("  SubWavBank (ROM const)     : %8lu B (%u tables x %u mips x %u samples)\n",
           KB(wav), (unsigned)SUBWT_TABLES, (unsigned)SUBWT_MIPS, (unsigned)(SUBWT_SIZE + 1));
    printf("  SubSvf                   : %8lu B (x2 per voice)\n", KB(svf));
    /* ワーカー ELF + BSS は実機リンクマップで最終確認 */
    printf("\n");

    /* --- 判定 --- */
    int ok = (main_total < 512ul * 1024ul) && (ctx < 256ul * 1024ul);
    printf("Verdict: %s\n", ok ? "OK - fits comfortably in CXD5602 1.5MB SRAM"
                                  : "CHECK - approaching memory limits");
    return ok ? 0 : 1;
}
