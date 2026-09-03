/**
 * @file worker_entry.c
 * @brief SubCore 5 ワーカー ELF エントリポイント (NuttX ASMP)
 * @details mpshm 共有コンテキストにアタッチし、EQ/リバーブ マスターDSP処理へ委譲する
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <asmp/types.h>
#include <asmp/mpshm.h>
#include <asmp/mpmutex.h>

#define SUB_COMMON_NO_LUT
#include "sub_common.h"

int main(void)
{
    mpmutex_t mutex;
    mpshm_t shm;
    AsmpSharedContext *shared;

    mpmutex_init(&mutex, ASMP_KEY_MUTEX);
    mpshm_init(&shm, ASMP_KEY_SHM, sizeof(AsmpSharedContext));

    shared = (AsmpSharedContext *)mpshm_attach(&shm, 0);
    if (!shared) {
        return -1;
    }

    /* ABIゲート: Main との共有メモリレイアウト不一致を進入前に検出する。
     * 不一致のまま subcoreN_entry へ進むとキュー/PCMオフセットずれで
     * 誤動作 (音がぷつぷつ切れる) するため、タグ付きで即死させる。
     * (Makefile WORKER_DEPS の include/*.h 依存と対になる防御) */
    if (!asmp_abi_ok(shared)) {
        printf("[SUB5][FATAL] ABI mismatch magic=%08x ver=%u size=%u expected %u tag=%s\n",
               (unsigned int)shared->abi_magic, (unsigned int)shared->abi_version,
               (unsigned int)shared->abi_size, (unsigned int)sizeof(AsmpSharedContext),
               HEXASENSE_DSP_TAG);
        mpshm_detach(&shm);
        return -1;
    }

    subcore5_entry(shared);

    mpshm_detach(&shm);
    return 0;
}
