/**
 * tests/p0b_epoch_slot_race.c
 * P0-B epoch/slot generation 遅延注入再現テスト (host)
 *  - 正常 / Sub2 20ms遅延 / Sub3 50ms遅延 / 全遅延 / wraparound をシミュレート
 *  - 現行の「slot_epoch mismatchを数えるだけ」で破壊を防げないことを再現
 * Build: gcc -Iinclude -Iasmp_sub -o /tmp/p0b tests/p0b_epoch_slot_race.c src/asmp_manager.c asmp_sub/sub_common.c -lpthread -lm
 * または CMake: add_executable(p0b_epoch_slot_race tests/p0b_epoch_slot_race.c src/asmp_manager.c ...)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define TEST_SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#include <unistd.h>
#define TEST_SLEEP_MS(ms) usleep((ms)*1000)
#endif
#include "asmp_protocol.h"
#include "asmp_manager.h"

// SlotHeader: 現行には無い世代管理をテスト用に付与
typedef struct {
    uint32_t epoch;
    uint32_t generation;
    uint32_t owner_mask;
} SlotHeader;

// シミュレータ: 現行の単純 slot_epoch[mismatch++] 方式を再現
typedef struct {
    AsmpRenderCtrl render_ctrl;
    AsmpDoneCell done_epoch[ASMP_NUM_CORES];
    uint32_t generation[ASMP_NUM_SLOTS];
    int pcm_slot[ASMP_NUM_SLOTS][8]; // ダミーPCM先頭
    uint32_t diag_mismatch;
    uint32_t diag_gap;
} SimContext;

static void sim_init(SimContext *s) {
    memset(s, 0, sizeof(*s));
}

static void sim_begin_frame(SimContext *s, uint32_t frames) {
    (void)frames;
    s->render_ctrl.render_epoch++;
    uint32_t slot = ASMP_EPOCH_SLOT(s->render_ctrl.render_epoch);
    s->render_ctrl.slot_epoch[slot] = s->render_ctrl.render_epoch;
    s->generation[slot]++; // 理想では世代インクリメント
}

static void sim_worker_write(SimContext *s, int core, uint32_t epoch, int delay_ms, bool *destroyed) {
    if (delay_ms > 0) TEST_SLEEP_MS(delay_ms);
    uint32_t slot = ASMP_EPOCH_SLOT(epoch);
    // 現行の脆弱性: workerは generationを検証せず直接書き込む
    // 遅延後に slot_epoch が既に次世代に進んでいても書き換えてしまう
    if (s->render_ctrl.slot_epoch[slot] != epoch) {
        s->diag_mismatch++;
        // 現行はカウンタだけ増やして継続 -> 破壊を防げない
        // 検証: 書き込みが発生したか
        if (destroyed) *destroyed = true;
    }
    // 書き込みシミュレート
    s->pcm_slot[slot][0] = (int)epoch;
    s->done_epoch[core].val = epoch;
}

static bool sim_end_frame(SimContext *s, uint32_t target, uint32_t timeout_ms) {
    uint64_t start = 0;
#ifdef _WIN32
    start = GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    start = ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
    while (1) {
        bool ok = true;
        // Sub2,3,5 の done を待つ (Sub4は簡略化で除外)
        for (int c=2;c<=5;c++) {
            if (c==4) continue;
            if ((int32_t)(s->done_epoch[c].val - target) < 0) { ok = false; break; }
        }
        if (ok) return true;
#ifdef _WIN32
        uint64_t now = GetTickCount();
#else
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
        if (now - start > timeout_ms) return false;
        TEST_SLEEP_MS(1);
    }
}

// テストケース
static int test_normal(void) {
    SimContext s; sim_init(&s);
    printf("[P0-B] test_normal: ");
    for (uint32_t e=1;e<=10;e++) {
        sim_begin_frame(&s, 512);
        bool destroyed=false;
        sim_worker_write(&s, 2, e, 0, &destroyed);
        sim_worker_write(&s, 3, e, 0, &destroyed);
        sim_worker_write(&s, 5, e, 0, &destroyed);
        if (!sim_end_frame(&s, e, 20)) { printf("FAIL timeout at %u\n", e); return 1; }
        if (destroyed) { printf("FAIL destroyed at %u\n", e); return 1; }
    }
    printf("PASS mismatch=%u gap=%u\n", s.diag_mismatch, s.diag_gap);
    return 0;
}

static int test_sub2_delay_20ms(void) {
    SimContext s; sim_init(&s);
    printf("[P0-B] test_sub2_delay_20ms (Sub5 timeout 12ms, slot reuse): ");
    // e=1 slot1, advance to e=3 slot1 to force reuse, then Sub2 writes e=1 late
    sim_begin_frame(&s, 512); // e=1
    sim_worker_write(&s, 3, 1, 0, NULL);
    sim_worker_write(&s, 5, 1, 0, NULL);
    // Don't complete Sub2 yet
    sim_begin_frame(&s, 512); // e=2 slot0
    sim_worker_write(&s, 2, 2, 0, NULL);
    sim_worker_write(&s, 3, 2, 0, NULL);
    sim_worker_write(&s, 5, 2, 0, NULL);
    sim_end_frame(&s, 2, 20);
    sim_begin_frame(&s, 512); // e=3 slot1 (reuse of e=1 slot)
    bool destroyed=false;
    // Late Sub2 for e=1 now writes to slot1 which is now e=3
    sim_worker_write(&s, 2, 1, 20, &destroyed);
    sim_worker_write(&s, 2, 3, 0, NULL);
    sim_worker_write(&s, 3, 3, 0, NULL);
    sim_worker_write(&s, 5, 3, 0, NULL);
    sim_end_frame(&s, 3, 20);
    printf("mismatch=%u destroyed=%d %s\n", s.diag_mismatch, destroyed, destroyed?"DETECTED (現行は破壊)":"?");
    return destroyed?0:1;
}

static int test_sub3_delay_50ms(void) {
    SimContext s; sim_init(&s);
    printf("[P0-B] test_sub3_delay_50ms: ");
    sim_begin_frame(&s, 1);
    bool destroyed=false;
    sim_worker_write(&s, 2, 1, 0, NULL);
    // Sub3 50ms遅延 -> 4 epoch分先へ進む間に書き込み
    for (uint32_t e=2;e<=5;e++) {
        sim_begin_frame(&s, 512);
        sim_worker_write(&s, 2, e, 0, NULL);
        sim_worker_write(&s, 5, e, 0, NULL);
        sim_end_frame(&s, e, 12);
    }
    sim_worker_write(&s, 3, 1, 50, &destroyed);
    printf("mismatch=%u destroyed=%d %s\n", s.diag_mismatch, destroyed, destroyed?"DETECTED":"?");
    return destroyed?0:1;
}

static int test_wraparound(void) {
    SimContext s; sim_init(&s);
    s.render_ctrl.render_epoch = 0xFFFFFFFEu;
    printf("[P0-B] test_wraparound epoch 0xFFFFFFFE->0: ");
    for (int i=0;i<5;i++) {
        uint32_t e = s.render_ctrl.render_epoch+1;
        sim_begin_frame(&s, 512);
        bool d=false;
        sim_worker_write(&s, 2, e, 0, &d);
        if (d) { printf("FAIL destroyed at wraparound %u\n", e); return 1; }
        s.done_epoch[2].val=e; s.done_epoch[3].val=e; s.done_epoch[5].val=e;
        if (!sim_end_frame(&s, e, 20)) { printf("FAIL timeout %u\n", e); return 1; }
    }
    printf("PASS\n");
    return 0;
}

static int test_generation_guard(void) {
    // 理想の修正案: generationを検証してcommit拒否する方式のデモ
    printf("[P0-B] test_generation_guard (理想): ");
    SimContext s; sim_init(&s);
    sim_begin_frame(&s, 512); // e=1 slot1 gen1=1
    uint32_t e1=1, slot1=ASMP_EPOCH_SLOT(e1);
    uint32_t gen1=s.generation[slot1];
    // 2つ進めて同じslotへ再訪 (e=1 slot1 -> e=3 slot1)
    sim_begin_frame(&s, 512); // e=2 slot0
    sim_begin_frame(&s, 512); // e=3 slot1 gen2=2
    uint32_t e3=3;
    // 遅れたSub2が e1のgenerationでcommitしようとする -> 拒否されるべき
    bool should_reject = (gen1 != s.generation[slot1]);
    printf("e1 gen=%u e3 gen=%u reject=%d %s\n", gen1, s.generation[slot1], should_reject, should_reject?"PASS (guard works)":"FAIL");
    (void)e3;
    return should_reject?0:1;
}

int main(void) {
    printf("=== P0-B Epoch/Slot Race Tests (HEAD 6b90fc7) ===\n");
    int fails=0;
    fails += test_normal();
    fails += test_sub2_delay_20ms();
    fails += test_sub3_delay_50ms();
    fails += test_wraparound();
    fails += test_generation_guard();
    printf("\nSummary: %d tests failed (0 means all detections passed)\n", fails);
    printf("Note: 現行コードは mismatchを数えるだけで破壊を防げないため、delayテストは'破壊検出'をもって現行の脆弱性を証明する。\n");
    return fails;
}
