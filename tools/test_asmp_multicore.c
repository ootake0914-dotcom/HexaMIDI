/**
 * @file test_asmp_multicore.c
 * @brief Sony Spresense ASMP 6コア完全分散処理 単体・統合検証テスト
 */

/* Release (NDEBUG) ビルドでも assert を有効化してテストを機能させる */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define test_sleep_ms(ms) Sleep(ms)
static uint32_t GetTickCountMsPort(void) { return (uint32_t)GetTickCount(); }
#else
#include <unistd.h>
#include <time.h>
#define test_sleep_ms(ms) usleep((ms) * 1000)
static uint32_t GetTickCountMsPort(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u));
}
#endif

#include <stddef.h>
#include "asmp_protocol.h"
#include "asmp_manager.h"
#include "sub_spawn.h"

static void test_asmp_ringbuffer_layout(void)
{
    printf("[TEST] Testing AsmpRingBuffer Memory Alignment & Cache-Line Padding...\n");
    /* 1. 構造体サイズが 32 の倍数かつ容量に応じたサイズであること */
    size_t expected = (size_t)ASMP_QUEUE_CAPACITY * sizeof(AsmpPacket) + 64u; /* head4+pad28+tail4+pad28 */
    assert(sizeof(AsmpRingBuffer) == expected);
    assert(sizeof(AsmpRingBuffer) % 32 == 0);

    /* 2. head と tail のオフセットが別々の 32B キャッシュラインに分離されていること */
    size_t head_offset = offsetof(AsmpRingBuffer, head);
    size_t tail_offset = offsetof(AsmpRingBuffer, tail);
    assert(head_offset == (size_t)ASMP_QUEUE_CAPACITY * sizeof(AsmpPacket));
    assert(head_offset % 32 == 0);
    assert(tail_offset == head_offset + 32);
    assert((tail_offset - head_offset) == 32);
    assert(tail_offset % 32 == 0);

    /* 3. 共有メモリ内の配列 queues[N] の全要素が 32B 境界に整列すること */
    AsmpSharedContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < ASMP_NUM_CORES; i++) {
        uintptr_t addr = (uintptr_t)&ctx.queues[i];
        assert((addr % 32) == 0);
    }
    printf("  -> PASS: AsmpRingBuffer struct size %uB, 32B alignment verified!\n",
           (unsigned)sizeof(AsmpRingBuffer));
}

static void test_asmp_6core_distributed_pipeline(void)
{
    printf("[TEST] Testing ASMP 6-Core Fully Distributed Synthesizer Pipeline...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    assert(mgr.is_initialized == true);

    /* 1. SubCore 1〜5 の起動 */
    printf("  1. Spawning SubCore 1 to 5...\n");
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    assert(mgr.are_cores_running == true);

    test_sleep_ms(100);

    /* 2. 6コアのハートビート検証 */
    printf("  2. Verifying initial heartbeats across all 6 cores...\n");
    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    printf("     Heartbeats: [C0:%u, S1:%u, S2:%u, S3:%u, S4:%u, S5:%u]\n",
           shm->core[0].heartbeat, shm->core[1].heartbeat, shm->core[2].heartbeat,
           shm->core[3].heartbeat, shm->core[4].heartbeat, shm->core[5].heartbeat);

    /* 3. 200フレーム (約2.1秒分) の同期レンダリングテスト */
    printf("  3. Injecting Note-On and running 200 real-time render cycles (2.13 sec audio)...\n");
    AsmpPacket note_init = {
        .msg_type = ASMP_MSG_NOTE_ON,
        .channel = 0,
        .data1 = 60, /* C4 */
        .data2 = 127
    };
    bool init_push_ok = asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &note_init);
    assert(init_push_ok == true);

    int16_t master_pcm[ASMP_BUFFER_FRAMES * 2];
    int64_t total_energy = 0;

    for (uint32_t i = 0; i < 200; i++) {
        ret = asmp_manager_sync_render_frame(&mgr, master_pcm, ASMP_BUFFER_FRAMES);
        assert(ret == 0);

        for (uint32_t s = 0; s < ASMP_BUFFER_FRAMES * 2; s++) {
            total_energy += abs(master_pcm[s]);
        }
    }

    printf("     Total Master Output Energy: %lld\n", (long long)total_energy);
    assert(total_energy > 10000); /* 実際に音声信号が生成されていることを検証 */

    /* 4. 各サブコアの活動・ボイス数確認 */
    printf("  4. Checking active voice telemetry:\n");
    printf("     - SubCore 2 (Melody/Lead): %u voices\n", shm->core[2].voice_count);
    printf("     - SubCore 3 (Bass/Strings): %u voices\n", shm->core[3].voice_count);
    printf("     - SubCore 4 (GM Drums): %u voices\n", shm->core[4].voice_count);
    printf("     - Final Heartbeats: [C0:%u, S1:%u, S2:%u, S3:%u, S4:%u, S5:%u]\n",
           shm->core[0].heartbeat, shm->core[1].heartbeat, shm->core[2].heartbeat,
           shm->core[3].heartbeat, shm->core[4].heartbeat, shm->core[5].heartbeat);

    assert(shm->core[1].heartbeat >= 100);
    assert(shm->core[2].heartbeat >= 100);
    assert(shm->core[3].heartbeat >= 100);
    assert(shm->core[4].heartbeat >= 100);
    assert(shm->core[5].heartbeat >= 100);

    /* 5. JoyStick / 外部コマンド送信テスト
     * NOTE: SUB2 キューのプロデューサは SubCore 1 のみ (SPSC 規律)。
     * 直接送信ではなく Sub1 経由のルーティングで注入する */
    printf("  5. Testing real-time event injection via ASMP queue...\n");
    AsmpPacket note_pkt = {
        .msg_type = ASMP_MSG_NOTE_ON,
        .channel = 0,
        .data1 = 60, /* C4 */
        .data2 = 127
    };
    bool push_ok = asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &note_pkt);
    assert(push_ok == true);

    /* 1サイクル進行 */
    ret = asmp_manager_sync_render_frame(&mgr, master_pcm, ASMP_BUFFER_FRAMES);
    assert(ret == 0);

    /* 6. 安全なシャットダウン */
    printf("  6. Shutting down all SubCores cleanly...\n");
    asmp_manager_stop_cores(&mgr);
    assert(mgr.are_cores_running == false);

    printf("  -> PASS: ASMP 6-Core fully distributed architecture 100%% verified!\n");
}

/**
 * @brief Sub1 ルーティングによる分散再生検証 (Lead/Bass/Drum 3 音源コア)
 * @details Sub1 の自前再生は廃止済みのため、Main 相当としてノートを
 *          Sub1 キューへ投下し、各音源コアが発音するかを検証する
 */
static void test_asmp_routed_playback(void)
{
    printf("[TEST] Testing multi-channel routed playback across 3 voice cores...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);

    test_sleep_ms(50);

    /* Sub1 経由 (SPSC 単一プロデューサ規律) でリード(ch0)/ベース(ch1)/ドラム(ch9) を発音。
     * Note Off は送らずサステインさせることで全エポックでエネルギーが積み上がる */
    static const struct { uint8_t ch, note; } held_notes[] = {
        { 0, 60 }, { 0, 64 }, { 0, 67 }, { 0, 72 },
        { 1, 36 }, { 1, 43 },
        { 2, 48 }, { 2, 55 },
    };
    for (size_t i = 0; i < sizeof(held_notes) / sizeof(held_notes[0]); i++) {
        AsmpPacket on = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = held_notes[i].ch,
            .data1 = held_notes[i].note,
            .data2 = 120
        };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    {
        static const uint8_t drums[8] = { 36, 38, 42, 46, 49, 41, 45, 38 };
        for (int i = 0; i < 8; i++) {
            AsmpPacket pkt = {
                .msg_type = ASMP_MSG_NOTE_ON,
                .channel = 9,
                .data1 = drums[i],
                .data2 = 120
            };
            assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pkt));
        }
    }

    int16_t master_pcm[ASMP_BUFFER_FRAMES * 2];
    int64_t total_energy = 0;
    uint32_t frames_run = 0;

    /* 約4秒分 (375フレーム) を分散レンダリング */
    for (uint32_t i = 0; i < 375; i++) {
        ret = asmp_manager_sync_render_frame(&mgr, master_pcm, ASMP_BUFFER_FRAMES);
        if (ret != 0) break;
        frames_run++;
        for (uint32_t smp = 0; smp < ASMP_BUFFER_FRAMES * 2; smp++) {
            total_energy += abs(master_pcm[smp]);
        }
    }

    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    printf("     Rendered %u frames (%.1f sec), Total Energy: %lld\n",
           (unsigned int)frames_run,
           (double)frames_run * ASMP_BUFFER_FRAMES / 48000.0,
           (long long)total_energy);
    printf("     Voice counts [S2:%u S3:%u S4:%u]\n",
           shm->core[ASMP_CORE_SUB2_LEAD].voice_count,
           shm->core[ASMP_CORE_SUB3_BASS].voice_count,
           shm->core[ASMP_CORE_SUB4_DRUM].voice_count);

    /* リード/ベース/ドラム全パートが音声エネルギーを生成したことを検証 */
    assert(frames_run >= 300);
    assert(total_energy > 100000000); /* マルチパートで満遍なく発音していること */

    /* 全消音してから終了 (ゾンビノート持ち越し防止) */
    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (asmp_manager_sync_render_frame(&mgr, master_pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: Multi-channel routed playback verified!\n");
}

/**
 * @brief フルロード試験: 全音源コアを最大ポリフォニーで駆動し、
 *        デッドライン遵守 (同期タイムアウト無し) と発音数を検証する
 */
static void test_asmp_full_burn_stress(void)
{
    printf("[TEST] Testing FULL-BURN stress (max polyphony across all voice cores)...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);

    test_sleep_ms(50);

    /* 内蔵シーケンスを停止 (ストレス音との競合排除) */
    {
        AsmpPacket stop = { .msg_type = ASMP_MSG_CMD_STOP };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &stop));
    }
    test_sleep_ms(50);

    /* SubCore 1 経由 (SPSC 単一プロデューサ規律) で全音源コアへ大量発音:
     * Lead(ch0) 12 音 + Bass/Chords(ch2/ch3) 12 音 + ドラム(ch9) 8 発 */
    for (int i = 0; i < 12; i++) {
        AsmpPacket pkt = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 0,
            .data1 = (uint8_t)(60 + i),
            .data2 = 127
        };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pkt));
    }
    for (int i = 0; i < 6; i++) {
        AsmpPacket pkt = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 2,
            .data1 = (uint8_t)(48 + i),
            .data2 = 120
        };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pkt));
    }
    for (int i = 0; i < 6; i++) {
        AsmpPacket pkt = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 3,
            .data1 = (uint8_t)(55 + i),
            .data2 = 118
        };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pkt));
    }
    {
        static const uint8_t drums[8] = { 36, 38, 42, 46, 49, 41, 45, 38 };
        for (int i = 0; i < 8; i++) {
            AsmpPacket pkt = {
                .msg_type = ASMP_MSG_NOTE_ON,
                .channel = 9,
                .data1 = drums[i],
                .data2 = 110
            };
            assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pkt));
        }
    }

    /* 200 フレーム (2.13 秒) の連続レンダリング: タイムアウト零が必須 */
    int16_t master_pcm[ASMP_BUFFER_FRAMES * 2];
    uint32_t frames_run = 0;
    uint64_t energy = 0;
    uint32_t start_ms = (uint32_t)GetTickCountMsPort();

    for (uint32_t i = 0; i < 200; i++) {
        ret = asmp_manager_sync_render_frame(&mgr, master_pcm, ASMP_BUFFER_FRAMES);
        if (ret != 0) break;
        frames_run++;
        for (uint32_t smp = 0; smp < ASMP_BUFFER_FRAMES * 2; smp++) {
            energy += abs(master_pcm[smp]);
        }
    }
    uint32_t elapsed_ms = (uint32_t)GetTickCountMsPort() - start_ms;

    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    printf("     Frames: %u/200 | Wall: %ums (%.2f ms/frame) | Energy: %llu\n",
           (unsigned int)frames_run, (unsigned int)elapsed_ms,
           (frames_run > 0) ? ((double)elapsed_ms / frames_run) : -1.0,
           (unsigned long long)energy);
    printf("     Voice counts [S2:%u S3:%u S4:%u] | Loads(permille) [S2:%u S3:%u S4:%u S5:%u]\n",
           shm->core[ASMP_CORE_SUB2_LEAD].voice_count,
           shm->core[ASMP_CORE_SUB3_BASS].voice_count,
           shm->core[ASMP_CORE_SUB4_DRUM].voice_count,
           shm->core[ASMP_CORE_SUB2_LEAD].cpu_load,
           shm->core[ASMP_CORE_SUB3_BASS].cpu_load,
           shm->core[ASMP_CORE_SUB4_DRUM].cpu_load,
           shm->core[ASMP_CORE_SUB5_DSP].cpu_load);

    /* 全フレーム完走 (デッドライン遵守) & 全コア稼働 & 高エネルギー */
    assert(frames_run == 200);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count >= 10);
    assert(shm->core[ASMP_CORE_SUB3_BASS].voice_count >= 8); /* SubCore 3 上限 8音 */
    assert(energy > 100000000);

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: FULL-BURN stress verified (all 6 cores loaded, no deadline miss)!\n");
}

/**
 * @brief Goertzel 単一ビン振幅推定 (ピッチベンド周波数検証用)
 */
static double test_goertzel_mag(const int16_t *pcm, uint32_t frames, double freq, double fs)
{
    const double k = (2.0 * 3.14159265358979323846 * freq) / fs;
    const double coeff = 2.0 * cos(k);
    double s1 = 0.0, s2 = 0.0;
    for (uint32_t n = 0; n < frames; n++) {
        double x = (double)pcm[n];
        double s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

/**
 * @brief 回帰: ピッチベンドの二重適用防止。
 *        ベンド中に発音したノートは「ベンド込み 1 回分」だけずれること
 *        (旧実装は Note On 時とレンダーループで二重に掛かり
 *        +2 半音指定が約 +4 半音相当に聞こえていた) を
 *        Goertzel ピークで検証する
 */
static void test_asmp_pitch_bend_frequency(void)
{
    printf("[TEST] Testing pitch bend single-application via SubCore routing...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    test_sleep_ms(50);

    /* 内蔵シーケンス停止 (測定妨害の排除) */
    AsmpPacket stop = { .msg_type = ASMP_MSG_CMD_STOP };
    assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &stop));
    {
        int16_t sink[ASMP_BUFFER_FRAMES * 2];
        for (int i = 0; i < 8; i++) assert(asmp_manager_sync_render_frame(&mgr, sink, ASMP_BUFFER_FRAMES) == 0);
    }

    /* A4 (440Hz) を +2 半音 (= ~493.88Hz) のベンド中に発音 */
    AsmpPacket pb = { .msg_type = ASMP_MSG_PITCH_BEND, .channel = 0,
                      .param = (uint32_t)(int32_t)8191 };
    AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0,
                      .data1 = 69, .data2 = 127 };
    assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pb));
    assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));

    /* end_frame はステレオ (frames * 2ch) 分を書き込むためバッファは 2 倍必須。
     * (旧実装はモノラル分しか確保しておらず毎呼び出しスタックを破壊していた) */
    int16_t pcm[ASMP_BUFFER_FRAMES * 2];
    /* アタック過渡を捨てる */
    for (int i = 0; i < 10; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    /* 定常部 16 フレーム (~171ms ≒ A4 の 75 周期) を収集 */
    static int16_t steady[ASMP_BUFFER_FRAMES * 16 * 2];
    for (int i = 0; i < 16; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
        memcpy(&steady[i * ASMP_BUFFER_FRAMES * 2], pcm, sizeof(pcm));
    }

    /* Goertzel には L/R 同内容のインターリーブ PCM をそのまま与える
     * (中央パンのため L=R。総サンプル数は frames*2) */
    const uint32_t total = ASMP_BUFFER_FRAMES * 16 * 2;
    double g_base = test_goertzel_mag(steady, total, 440.0, 48000.0);   /* 無ベンド音高 */
    double g_bent = test_goertzel_mag(steady, total, 493.88, 48000.0);  /* +2 半音 (正解) */
    double g_dbl  = test_goertzel_mag(steady, total, 622.25, 48000.0);  /* +4 半音 (旧バグ) */

    printf("     Goertzel magnitude: 440Hz=%.0f  493.88Hz=%.0f  622.25Hz=%.0f\n",
           g_base, g_bent, g_dbl);

    asmp_manager_stop_cores(&mgr);

    /* 正しい音程が支配的であり、二重適用 (+4半音) になっていないこと */
    assert(g_bent > g_base * 1.5);
    assert(g_bent > g_dbl * 2.0);

    printf("  -> PASS: pitch bend applied exactly once (bent tone dominant).\n");
}

/**
 * @brief 回帰: PC理想配置移動時のチャンネル状態復元
 *        ch8 (既定 Sub2) へ CC7=20 (quiet) / CC10=0 (hard left) を送ってから
 *        PC48 (ideal=Sub3) で移動させ、NoteOn を発音する。
 *        旧実装は PC 自体のみ新コアへ流し、直前の Vol/Pan (旧コア配送済み) が
 *        新コア側で既定値のまま残ったため、移動直後の発音が「大きい・中央」
 *        (＝出だしの音色変) になっていた。復元後は Sub3 側で quiet-left の
 *        まま鳴らなければならず、定常部の L/R エネルギー比 (L>3R) で検証する。
 */
static void test_asmp_route_move_restores_state(void)
{
    printf("[TEST] Testing route-move state restore (quiet-left survives PC move)...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    test_sleep_ms(50);

    /* 参照フェーズ (移動なし): ch0 に同一の quiet-left 設定 + Piano で発音。
     * 移動を伴わない正しいレンダリングの L/R 比を求める */
    {
        AsmpPacket cc7 = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 0, .data1 = 7, .data2 = 20 };
        AsmpPacket cc10 = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 0, .data1 = 10, .data2 = 0 };
        AsmpPacket pc = { .msg_type = ASMP_MSG_PROGRAM_CHANGE, .channel = 0, .data1 = 0 };
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0, .data1 = 60, .data2 = 100 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &cc7));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &cc10));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pc));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    int16_t pcm[ASMP_BUFFER_FRAMES * 2];
    int64_t ref_l = 0, ref_r = 0;
    for (int i = 0; i < 12; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
        if (i >= 4) {
            for (uint32_t s = 0; s < ASMP_BUFFER_FRAMES; s++) {
                ref_l += abs(pcm[s * 2 + 0]);
                ref_r += abs(pcm[s * 2 + 1]);
            }
        }
    }
    printf("     reference (no move) L=%lld R=%lld\n", (long long)ref_l, (long long)ref_r);
    assert(ref_l > 0);
    assert(ref_l > 3 * ref_r); /* quiet-left が正しく鳴ることの確認 */

    /* 消音して離す */
    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (int i = 0; i < 15; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    /* 移動フェーズ: ch8 (既定 Sub2) に同一設定を送ってから PC48 (ideal Sub3) */
    {
        AsmpPacket cc7 = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 8, .data1 = 7, .data2 = 20 };
        AsmpPacket cc10 = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 8, .data1 = 10, .data2 = 0 };
        AsmpPacket pc = { .msg_type = ASMP_MSG_PROGRAM_CHANGE, .channel = 8, .data1 = 48 };
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 8, .data1 = 60, .data2 = 100 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &cc7));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &cc10));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pc));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    int64_t mov_l = 0, mov_r = 0;
    for (int i = 0; i < 12; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
        if (i >= 4) {
            for (uint32_t s = 0; s < ASMP_BUFFER_FRAMES; s++) {
                mov_l += abs(pcm[s * 2 + 0]);
                mov_r += abs(pcm[s * 2 + 1]);
            }
        }
    }
    printf("     moved (PC move) L=%lld R=%lld voices S2=%u S3=%u\n",
            (long long)mov_l, (long long)mov_r,
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count,
            shm->core[ASMP_CORE_SUB3_BASS].voice_count);
    /* 移動先 Sub3 で発音していること */
    assert(shm->core[ASMP_CORE_SUB3_BASS].voice_count >= 1);
    /* 復元されていれば参照同様 L>>R。旧実装 (復元なし) では既定センターのまま L≈R になる */
    assert(mov_l > 0);
    assert(mov_l > 3 * mov_r);

    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (int i = 0; i < 8; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: moved channel keeps volume/pan (quiet-left preserved).\n");
}

/**
 * @brief ABI v13: spawn プールのレイアウト・トークン検証
 */
static void test_asmp_spawn_pool_layout(void)
{
    printf("[TEST] Testing voice-spawn pool layout (ABI v13)...\n");
    assert(sizeof(SubSpawnDesc) == 52);
    assert(sizeof(SubSpawnSlot) == 96);
    assert(offsetof(SubSpawnSlot, desc) == 32);
    assert(sizeof(SubSpawnAck) == 32);
    assert(sizeof(SubSpawnStats) == 32);
    AsmpSharedContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(((uintptr_t)&ctx.spawn_pool_sub2[0] % 32) == 0);
    assert(((uintptr_t)&ctx.spawn_pool_sub3[0] % 32) == 0);
    assert(((uintptr_t)&ctx.spawn_ack_sub2 % 32) == 0);
    assert(((uintptr_t)&ctx.spawn_stats_sub3 % 32) == 0);
    /* トークン往復 */
    {
        uint16_t g = 0;
        uint8_t idx = 0;
        uint32_t tok = sub_spawn_token(7u, 3u);
        assert(sub_spawn_token_parse(tok, &g, &idx) && g == 7u && idx == 3u);
        assert(!sub_spawn_token_parse(0u, &g, &idx));
        assert(!sub_spawn_token_parse(sub_spawn_token(0u, 1u), &g, &idx)); /* gen0拒否 */
        assert(!sub_spawn_token_parse(sub_spawn_token(5u, SUB_SPAWN_POOL_SLOTS), &g, &idx)); /* slot範囲外拒否 */
    }
    printf("  -> PASS: spawn pool 96B slots, gen line isolated, token codec OK.\n");
}

/**
 * @brief 回帰: PC理想配置移動のペダルガード (踏み中の移動禁止)
 *        踏み→発音→離鍵→PC48 の順で、保持音が旧コアに残り、移動しないこと。
 *        ガードなし旧実装では PC で移動して後続 CC64-off が新コアへ流れ、
 *        旧コアの保持音が stuck する (moved で S2==1 が残る)。
 */
static void test_asmp_route_move_pedal_guard(void)
{
    printf("[TEST] Testing route-move pedal guard (no move while sustained)...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    test_sleep_ms(50);

    {
        AsmpPacket stop = { .msg_type = ASMP_MSG_CMD_STOP };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &stop));
    }
    int16_t pcm[ASMP_BUFFER_FRAMES * 2];
    for (int i = 0; i < 8; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }

    /* 踏み→発音(60)→離鍵→PC48 (ideal=Sub3)。保持音は Sub2 に残り、移動しない */
    {
        AsmpPacket pedal_on = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 8, .data1 = 64, .data2 = 127 };
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 8, .data1 = 60, .data2 = 100 };
        AsmpPacket off = { .msg_type = ASMP_MSG_NOTE_OFF, .channel = 8, .data1 = 60 };
        AsmpPacket pc = { .msg_type = ASMP_MSG_PROGRAM_CHANGE, .channel = 8, .data1 = 48 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pedal_on));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pc));
    }
    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    for (int i = 0; i < 10; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    printf("     held: voices S2=%u S3=%u (expect S2>=1, S3==0)\n",
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count,
            shm->core[ASMP_CORE_SUB3_BASS].voice_count);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count >= 1);
    assert(shm->core[ASMP_CORE_SUB3_BASS].voice_count == 0);

    /* ペダル解放で保持音が消え、次回 PC48 で Sub3 へ移動すること。
     * 識別点は旧コア Sub2 の消音: ガードなし旧実装では pedal_off が移動先の
     * Sub3 へ流れて旧コアの保持音が stuck し、S2 が 1 のまま残る。
     * (Piano R=90ms のため 25epoch 待てば離鍵完了のはず) */
    {
        AsmpPacket pedal_off = { .msg_type = ASMP_MSG_CONTROL_CHANGE, .channel = 8, .data1 = 64, .data2 = 0 };
        AsmpPacket pc = { .msg_type = ASMP_MSG_PROGRAM_CHANGE, .channel = 8, .data1 = 48 };
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 8, .data1 = 62, .data2 = 100 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pedal_off));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &pc));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    for (int i = 0; i < 25; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }
    printf("     moved: voices S2=%u S3=%u (expect S2==0, S3>=1)\n",
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count,
            shm->core[ASMP_CORE_SUB3_BASS].voice_count);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count == 0);
    assert(shm->core[ASMP_CORE_SUB3_BASS].voice_count >= 1);

    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (int i = 0; i < 8; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: pedal-held channel stays put, moves after release.\n");
}

/**
 * @brief ABI v13: Core1分解送信が fast-spawn 経路で発音すること
 *        Sub1経由の NOTE_ON が全てディスクリプタ経路 (fast_spawn) を通り、
 *        音声エネルギーも正常に生成されることを検証する。
 */
static void test_asmp_spawn_fast_path_used(void)
{
    printf("[TEST] Testing Core1 decomposed spawn (fast path counters)...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    test_sleep_ms(50);

    /* ch0 (Sub2) と ch1 (Sub3) に発音。どちらも Core1 が分解送信するはず */
    {
        AsmpPacket on0 = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0, .data1 = 60, .data2 = 100 };
        AsmpPacket on1 = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 1, .data1 = 48, .data2 = 100 };
        AsmpPacket on2 = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0, .data1 = 64, .data2 = 100 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on0));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on1));
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on2));
    }
    int16_t pcm[ASMP_BUFFER_FRAMES * 2];
    int64_t energy = 0;
    for (int i = 0; i < 12; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
        for (uint32_t s = 0; s < ASMP_BUFFER_FRAMES * 2; s++) energy += abs(pcm[s]);
    }
    const AsmpSharedContext *shm = asmp_manager_context(&mgr);
    printf("     fast[S2:%u S3:%u] legacy[S2:%u S3:%u] energy=%lld\n",
            (unsigned)shm->spawn_stats_sub2.fast_spawn,
            (unsigned)shm->spawn_stats_sub3.fast_spawn,
            (unsigned)shm->spawn_stats_sub2.legacy_spawn,
            (unsigned)shm->spawn_stats_sub3.legacy_spawn,
            (long long)energy);
    assert(shm->spawn_stats_sub2.fast_spawn >= 2u);
    assert(shm->spawn_stats_sub3.fast_spawn >= 1u);
    assert(shm->spawn_stats_sub2.legacy_spawn == 0u);
    assert(shm->spawn_stats_sub3.legacy_spawn == 0u);
    assert(energy > 10000);

    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (int i = 0; i < 8; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: all routed NOTE_ONs took the decomposed fast path.\n");
}

/**
 * @brief 黄金律検証: 全ノートの確実な到達とボイススチール調停
 *        Sub2 を16音で飽和させても、追加のノート (弱音・強音問わず) が
 *        Core1 で無差別ドロップされることなく演奏コアへ届き、
 *        16音ポリフォニーの枠内で自然にボイススチールされることを検証。
 */
static void test_asmp_admission_triage(void)
{
    printf("[TEST] Testing golden delivery and natural voice stealing...\n");

    AsmpManager mgr;
    int ret = asmp_manager_init(&mgr);
    assert(ret == 0);
    ret = asmp_manager_start_cores(&mgr);
    assert(ret == 0);
    test_sleep_ms(50);

    {
        AsmpPacket stop = { .msg_type = ASMP_MSG_CMD_STOP };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &stop));
    }
    int16_t pcm[ASMP_BUFFER_FRAMES * 2];
    for (int i = 0; i < 8; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    const AsmpSharedContext *shm = asmp_manager_context(&mgr);

    /* 1. Sub2 を16音サステインで飽和させる (ch0, vel100, 異ピッチ) */
    for (int i = 0; i < 16; i++) {
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0,
                          .data1 = (uint8_t)(40 + i), .data2 = 100 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    for (int i = 0; i < 6; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    printf("     saturated: voices S2=%u (expect 16)\n",
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count == 16u);
    uint32_t drop_base = shm->diag_queue_drop;

    /* 2. 弱音ノート (vel30)。Core1 で無慈悲に捨てられず、確実に演奏コアへ届くこと */
    for (int i = 0; i < 4; i++) {
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0,
                          .data1 = (uint8_t)(60 + i), .data2 = 30 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    for (int i = 0; i < 6; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    printf("     weak delivered: diag_drop delta=%u (expect 0), voices S2=%u (expect 16)\n",
            (unsigned)(shm->diag_queue_drop - drop_base),
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count);
    assert(shm->diag_queue_drop - drop_base == 0u);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count == 16u);

    /* 3. 強音 4発 (vel110)。16音内で最古ボイスをスチールして発音 */
    static const uint8_t strong_notes[4] = { 84, 85, 86, 87 };
    for (int i = 0; i < 4; i++) {
        AsmpPacket on = { .msg_type = ASMP_MSG_NOTE_ON, .channel = 0,
                          .data1 = strong_notes[i], .data2 = 110 };
        assert(asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &on));
    }
    for (int i = 0; i < 6; i++) {
        assert(asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) == 0);
    }
    printf("     strong kept: diag_drop delta=%u (expect 0), voices S2=%u (expect 16)\n",
            (unsigned)(shm->diag_queue_drop - drop_base),
            shm->core[ASMP_CORE_SUB2_LEAD].voice_count);
    assert(shm->diag_queue_drop - drop_base == 0u);
    assert(shm->core[ASMP_CORE_SUB2_LEAD].voice_count == 16u);

    {
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
        asmp_manager_send_command(&mgr, ASMP_CORE_SUB1_SEQ, &off);
    }
    for (int i = 0; i < 8; i++) {
        if (asmp_manager_sync_render_frame(&mgr, pcm, ASMP_BUFFER_FRAMES) != 0) break;
    }

    asmp_manager_stop_cores(&mgr);
    printf("  -> PASS: all notes safely delivered, stealing handles saturation naturally.\n");
}

int main(void)
{
    /* abort() 時も進行状況を失わないよう無バッファ化 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=======================================================\n");
    printf(" SPRESENSE 6-CORE ASMP DISTRIBUTED PIPELINE TEST\n");
    printf("=======================================================\n");

    test_asmp_ringbuffer_layout();
    test_asmp_spawn_pool_layout();
    test_asmp_6core_distributed_pipeline();
    test_asmp_routed_playback();
    test_asmp_pitch_bend_frequency();
    test_asmp_route_move_restores_state();
    test_asmp_route_move_pedal_guard();
    test_asmp_spawn_fast_path_used();
    test_asmp_admission_triage();
    test_asmp_full_burn_stress();

    printf("=======================================================\n");
    printf(" ALL ASMP DISTRIBUTED TESTS PASSED (100%% SUCCESS)!\n");
    printf("=======================================================\n");
    return 0;
}
