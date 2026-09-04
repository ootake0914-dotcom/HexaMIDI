/**
 * @file asmp_manager.c
 * @brief Sony Spresense ASMP 6コア完全分散処理 マネージャー実装
 * @details
 *  実機 (__NuttX__): MP ライブラリ (mptask/mpshm/mpmutex) により SubCore 1〜5 の
 *                    ワーカー ELF を起動し、共有メモリ経由で同期レンダリングを行う。
 *  ホスト (_WIN32/Linux): OS スレッドで subcoreN_entry を起動する統合シミュレーション。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asmp_manager.h"
#include "rt_profile.h"

#ifdef __NuttX__

#include <asmp/asmp.h>
#include <asmp/mptask.h>
#include <asmp/mpshm.h>
#include <asmp/mpmutex.h>
#include <unistd.h>

/* sync_render_frame の待機ループで使用 (ホスト分岐でのみ定義されていた) */
#define asmp_sleep_us(us) usleep(us)

static mpshm_t   s_asmp_shm;
static mpmutex_t s_asmp_mutex;
static mptask_t  s_asmp_tasks[ASMP_NUM_CORES];

static const char *const s_worker_names[ASMP_NUM_CORES] = {
    NULL,
    ASMP_WORKER_NAME_SUB1,
    ASMP_WORKER_NAME_SUB2,
    ASMP_WORKER_NAME_SUB3,
    ASMP_WORKER_NAME_SUB4,
    ASMP_WORKER_NAME_SUB5
};

#else /* __NuttX__ */

#include "sub_common.h"

#ifdef _WIN32
#include <windows.h>
#define asmp_sleep_us(us) Sleep((DWORD)((us) / 1000))
#define ASMP_THREAD_RET   DWORD WINAPI
#define ASMP_THREAD_ARG   LPVOID
#else
#include <unistd.h>
#include <pthread.h>
#define asmp_sleep_us(us) usleep(us)
#define ASMP_THREAD_RET   void *
#define ASMP_THREAD_ARG   void *
#ifndef __NuttX__
#define _GNU_SOURCE
#endif
#endif

typedef struct {
    int core_id;
    AsmpSharedContext *shared;
} HostThreadArg;

static HostThreadArg s_host_args[ASMP_NUM_CORES];

static ASMP_THREAD_RET host_thread_entry(ASMP_THREAD_ARG arg)
{
    HostThreadArg *ta = (HostThreadArg *)arg;
    if (!ta || !ta->shared) {
        return 0;
    }

    switch (ta->core_id) {
        case ASMP_CORE_SUB1_SEQ:  subcore1_entry(ta->shared); break;
        case ASMP_CORE_SUB2_LEAD: subcore2_entry(ta->shared); break;
        case ASMP_CORE_SUB3_BASS: subcore3_entry(ta->shared); break;
        case ASMP_CORE_SUB4_DRUM: subcore4_entry(ta->shared); break;
        case ASMP_CORE_SUB5_DSP:  subcore5_entry(ta->shared); break;
        default:
            break;
    }
    return 0;
}

#endif /* __NuttX__ */

#define ASMP_SYNC_TIMEOUT_MS (2000)

/* 再起動後の状態再同期コールバック (音量/テンポ/選曲を SubCore へ再送する) */
typedef void (*AsmpResyncCb)(void *user);

static uint32_t monotonic_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
#endif
}

/**
 * @brief ASMP マネージャーの初期化
 */
int asmp_manager_init(AsmpManager *mgr)
{
    if (!mgr) return -1;

    memset(mgr, 0, sizeof(AsmpManager));
#ifndef __NuttX__
    /* ホストは init 直後から ctx を公開するため、ここでゼロ初期化しておく。
     * (旧実装は start_cores まで未初期化メモリを露出させていた) */
    mgr->ctx = &mgr->ctx_storage;
    memset(&mgr->ctx_storage, 0, sizeof(mgr->ctx_storage));
    mgr->ctx_storage.abi_magic = ASMP_PROTOCOL_MAGIC;
    mgr->ctx_storage.abi_version = ASMP_PROTOCOL_VERSION;
    mgr->ctx_storage.abi_size = sizeof(AsmpSharedContext);
#endif
    mgr->is_initialized = true;
    mgr->are_cores_running = false;
    mgr->published_epoch = 0;

    printf("[ASMP] Initialized 6-Core ASMP Architecture Context.\n");
    return 0;
}

AsmpSharedContext *asmp_manager_context(AsmpManager *mgr)
{
    return (mgr && mgr->is_initialized) ? mgr->ctx : NULL;
}

/**
 * @brief SubCore 1〜5 の起動
 */
int asmp_manager_start_cores(AsmpManager *mgr)
{
    if (!mgr || !mgr->is_initialized || mgr->are_cores_running) return -1;

#ifdef __NuttX__

    /* 共有コンテキスト サイズの自己診断:
     * 構造体レイアウト変更 (PCM バス float 化等) 後に MP 共有メモリ プール
     * を溢れていないかを起動ログで即判定できるようにする */
    printf("[ASMP] shm request: %lu B (AsmpSharedContext)\n",
           (unsigned long)sizeof(AsmpSharedContext));

    int ret = mpshm_init(&s_asmp_shm, ASMP_KEY_SHM, sizeof(AsmpSharedContext));
    if (ret < 0) {
        printf("[ASMP] Error: mpshm_init failed: %d\n", ret);
        return -1;
    }

    ret = mpmutex_init(&s_asmp_mutex, ASMP_KEY_MUTEX);
    if (ret < 0) {
        printf("[ASMP] Error: mpmutex_init failed: %d\n", ret);
        mpshm_destroy(&s_asmp_shm);
        return -1;
    }

    void *buf = mpshm_attach(&s_asmp_shm, 0);
    if (!buf) {
        printf("[ASMP] Error: mpshm_attach failed.\n");
        mpmutex_destroy(&s_asmp_mutex);
        mpshm_destroy(&s_asmp_shm);
        return -1;
    }
    memset(buf, 0, sizeof(AsmpSharedContext));
    /* S1: ABI header初期化 - ワーカー起動時のレイアウト検証用 */
    {
        AsmpSharedContext *sc = (AsmpSharedContext *)buf;
        sc->abi_magic = ASMP_PROTOCOL_MAGIC;
        sc->abi_version = ASMP_PROTOCOL_VERSION;
        sc->abi_size = sizeof(AsmpSharedContext);
    }
    mgr->ctx = (AsmpSharedContext *)buf;

    int failed_task = -1;
    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", ASMP_ROMFS_MOUNTPT, s_worker_names[i]);

        mptask_t *task = &s_asmp_tasks[i];
        ret = mptask_init(task, path);
        if (ret != 0) {
            printf("[ASMP] Error: mptask_init(%s) failed: %d\n", path, ret);
            goto nuttx_fail;
        }
        failed_task = i; /* 以降の失敗時はこのタスクの解放も必要 */

        ret = mptask_assign(task);
        if (ret != 0) {
            printf("[ASMP] Error: mptask_assign(%s) failed: %d\n", path, ret);
            goto nuttx_fail;
        }

        ret = mptask_bindobj(task, &s_asmp_mutex);
        if (ret < 0) {
            printf("[ASMP] Error: mptask_bindobj(mutex) failed: %d\n", ret);
            goto nuttx_fail;
        }

        ret = mptask_bindobj(task, &s_asmp_shm);
        if (ret < 0) {
            printf("[ASMP] Error: mptask_bindobj(shm) failed: %d\n", ret);
            goto nuttx_fail;
        }

        ret = mptask_exec(task);
        if (ret < 0) {
            printf("[ASMP] Error: mptask_exec(%s) failed: %d\n", path, ret);
            goto nuttx_fail;
        }

        mgr->mp_tasks[i] = task;
        printf("[ASMP] Starting SubCore %d (%s) on CPU#%d...\n",
               i, s_worker_names[i], mptask_getcpuid(task));
    }

    mgr->are_cores_running = true;
    printf("[ASMP] All 6 cores are up (Main + SubCore 1-5).\n");
    return 0;

nuttx_fail:
    for (int j = 1; j < ASMP_NUM_CORES; j++) {
        if (mgr->mp_tasks[j]) {
            int wret = -1;
            mptask_destroy((mptask_t *)mgr->mp_tasks[j], true, &wret);
            mgr->mp_tasks[j] = NULL;
        }
    }
    /* 登録前に失敗した実行中タスクも解放 (CPU 割当/ELF のリーク防止) */
    if (failed_task > 0 && failed_task < ASMP_NUM_CORES && !mgr->mp_tasks[failed_task]) {
        int wret = -1;
        mptask_destroy(&s_asmp_tasks[failed_task], true, &wret);
    }
    mpmutex_destroy(&s_asmp_mutex);
    if (mgr->ctx) {
        mpshm_detach(&s_asmp_shm);
        mgr->ctx = NULL;
    }
    mpshm_destroy(&s_asmp_shm);
    return -1;

#else /* __NuttX__ */

    AsmpSharedContext *shm = &mgr->ctx_storage;

#ifdef _WIN32
    /* 前回停止失敗で生き残ったワーカースレッドがある場合は再利用しない
     * (memset が生存スレッドの足元で行われメモリ破壊するのを防ぐ) */
    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        if (mgr->thread_handles[i] != NULL) {
            printf("[ASMP] Error: SubCore %d worker thread still alive. Restart aborted.\n", i);
            return -1;
        }
    }
#else
    /* POSIX 版: join 済み (id == 0) のときだけ再起動を許可する
     * (生存スレッドの pthread_t を上書きして join 不能化するのを防ぐ) */
    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        if (mgr->thread_ids[i] != 0) {
            printf("[ASMP] Error: SubCore %d worker thread still alive. Restart aborted.\n", i);
            return -1;
        }
    }
#endif

    memset(shm, 0, sizeof(AsmpSharedContext));
    /* S1: ABI header */
    shm->abi_magic = ASMP_PROTOCOL_MAGIC;
    shm->abi_version = ASMP_PROTOCOL_VERSION;
    shm->abi_size = sizeof(AsmpSharedContext);
    mgr->are_cores_running = true;

    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        s_host_args[i].core_id = i;
        s_host_args[i].shared = shm;

#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, host_thread_entry, &s_host_args[i], 0, NULL);
        if (th == NULL) {
            printf("[ASMP] Error: CreateThread(SubCore %d) failed.\n", i);
            asmp_manager_stop_cores(mgr);
            return -1;
        }
        mgr->thread_handles[i] = (void *)th;
        printf("[ASMP] Starting SubCore %d (Host Thread %p)...\n", i, (void *)th);
#else
        pthread_t tid;
        if (pthread_create(&tid, NULL, host_thread_entry, &s_host_args[i]) != 0) {
            printf("[ASMP] Error: pthread_create(SubCore %d) failed.\n", i);
            asmp_manager_stop_cores(mgr);
            return -1;
        }
        mgr->thread_ids[i] = (unsigned long)tid;
        printf("[ASMP] Starting SubCore %d (Host Thread id=%lu)...\n", i, (unsigned long)tid);
#endif
    }

    mgr->are_cores_running = true;
    printf("[ASMP] All 6 cores are simulated on host threads.\n");
    return 0;

#endif /* __NuttX__ */
}

/**
 * @brief 特定のサブコアへのメッセージ送信 (共有メモリキュー経由)
 */
bool asmp_manager_send_command(AsmpManager *mgr, uint8_t target_core, const AsmpPacket *pkt)
{
    if (!mgr || !mgr->ctx || !pkt || target_core >= ASMP_NUM_CORES) return false;

    /* P0-C Control Plane: 消音系コマンドは sticky generation を更新してドロップ防止 */
    if (pkt->msg_type == ASMP_MSG_ALL_NOTES_OFF) {
        uint8_t ch = pkt->channel;
        if (ch == 0 && target_core != ASMP_CORE_SUB2_LEAD) {
            ch = 0xFF; /* 未指定ブロードキャスト時は全チャンネル消音 */
        }
        asmp_control_plane_all_notes_off(mgr->ctx, ch);
    } else if (pkt->msg_type == ASMP_MSG_CONTROL_CHANGE && pkt->data1 == 64 && pkt->data2 < 64) {
        asmp_control_plane_sustain_off(mgr->ctx, pkt->channel < 16 ? pkt->channel : 0xFF);
    } else if (pkt->msg_type == ASMP_MSG_CONTROL_CHANGE && pkt->data1 == 120) {
        asmp_control_plane_all_sound_off(mgr->ctx, pkt->channel < 16 ? pkt->channel : 0xFF);
    }

    return asmp_queue_push(&mgr->ctx->queues[target_core], pkt);
}

void asmp_manager_render(AsmpManager *mgr, const int16_t *pcm_buffer, uint32_t frames)
{
    if (!mgr || !pcm_buffer || frames == 0 || !mgr->ctx) return;
    if (mgr->are_cores_running) return; /* Sub5 の Ping-Pong スロット保護 (テレメトリ用は begin_frame が更新) */

    /* ハートビートは「各コアが自分の進捗を自ら申告する」もの。
     * Main が代わりに全コア分を加算すると死活監視が永久に正しく機能しなくなるため
     * 自分 (Main) の分だけを更新する */
    mgr->ctx->core[ASMP_CORE_MAIN].heartbeat++;

    AsmpSharedContext *shm = mgr->ctx;
    uint32_t copy_bytes = frames * sizeof(int16_t) * 2;
    if (copy_bytes > sizeof(shm->pcm_sub5_master[0])) {
        copy_bytes = sizeof(shm->pcm_sub5_master[0]);
    }
    memcpy((void *)shm->pcm_sub5_master[0], pcm_buffer, copy_bytes);
}

/**
 * @brief 1フレーム分のレンダリング同期実行 (互換ラッパ)
 *        begin_frame + end_frame の連続呼び出し。
 *        ホスト統合テストツール (tools/test_asmp_multicore.c) から使用する。
 */
int asmp_manager_sync_render_frame(AsmpManager *mgr, int16_t *master_out, uint32_t frames)
{
    if (!mgr || !mgr->is_initialized || !master_out || frames == 0) return -1;
    if (!mgr->are_cores_running || !mgr->ctx) return -1;

    (void)asmp_manager_begin_frame(mgr, frames);
    return asmp_manager_end_frame(mgr, master_out, frames);
}

/**
 * @brief フレーム前半: 新エポックの先行公開
 */
uint32_t asmp_manager_begin_frame(AsmpManager *mgr, uint32_t frames)
{
    if (!mgr || !mgr->ctx) return 0;
    AsmpSharedContext *shm = mgr->ctx;

    /* Credit制御の撤廃:
     * 旧Credit制御は usleep(1ms) が NuttXで10msに丸められ budget超過で音飛びの直接原因だったため撤廃。
     * スロット上書きは Sub側の slot_epoch ミスマッチ検出で安全にスキップされる (ゴールデン同様の無待機方式) */
    mgr->published_epoch = mgr->published_epoch + 1u;
    uint32_t epoch = mgr->published_epoch;
    uint32_t slot = ASMP_EPOCH_SLOT(epoch);
#ifdef PROFILE_ENABLE
    profile_epoch_start(0, epoch);
#endif
    shm->render_ctrl.epoch_frames[slot] = frames;
    shm->render_ctrl.slot_epoch[slot] = epoch;
    asmp_dcache_clean((const void *)&shm->render_ctrl.epoch_frames[slot], sizeof(shm->render_ctrl.epoch_frames[slot]));
    asmp_dcache_clean((const void *)&shm->render_ctrl.slot_epoch[slot], sizeof(shm->render_ctrl.slot_epoch[slot]));
    /* P0-B: allocate slot header with new generation (Main single writer) */
    asmp_slot_allocate(shm, slot, epoch);
    ASMP_BARRIER();
    shm->render_ctrl.render_epoch = epoch;
    asmp_dcache_clean((const void *)&shm->render_ctrl.render_epoch, sizeof(shm->render_ctrl.render_epoch));
    /* P0-C: Render Mailbox 更新 (Main single writer) */
    asmp_render_mbox_begin(shm, epoch);
    ASMP_BARRIER(); /* エポック公開 -> Sub 側の処理開始を順序付ける */
    shm->core[ASMP_CORE_MAIN].heartbeat++;
    asmp_dcache_clean((const void *)&shm->core[ASMP_CORE_MAIN].heartbeat, sizeof(shm->core[ASMP_CORE_MAIN].heartbeat));

    return shm->render_ctrl.render_epoch;
}

/**
 * @brief フレーム後半: 直前エポックのマスターPCM取得
 *
 * 非同期 Ping-Pong: 待ち時間中に SubCore 2-4 は次エポックを合成中であり、
 * SubCore 5 もソース完了次第ミックスへ進む。Main はここでだけブロックする。
 */
int asmp_manager_end_frame(AsmpManager *mgr, int16_t *master_out, uint32_t frames)
{
    if (!mgr || !mgr->is_initialized || !master_out || frames == 0) return -1;
    if (!mgr->are_cores_running || !mgr->ctx) return -1;
    if (frames > ASMP_BUFFER_FRAMES) frames = ASMP_BUFFER_FRAMES;

    AsmpSharedContext *shm = mgr->ctx;

    /* begin_frame 済みエポック (Main 管理値) の 1 つ前が今回の出力対象。
     * 共有メモリの render_epoch は読まない: WD 再起動で共有メモリのみ
     * リセットされた場合でも target のラップが起きない。
     * 初回フレームは target=0 (=初期ゼロ埋めバッファ) が即座に確定する */
    const uint32_t target = mgr->published_epoch - 1u;
    const volatile uint32_t *d5 = &shm->done_epoch[ASMP_CORE_SUB5_DSP].val;

    uint32_t start_ms = monotonic_ms();
    for (;;) {
        asmp_dcache_invalidate((const void *)d5, sizeof(*d5));
        ASMP_BARRIER(); /* done_epoch 読み込み acquire */
        if (asmp_epoch_done(d5, target)) {
            break;
        }
        if ((monotonic_ms() - start_ms) > ASMP_SYNC_TIMEOUT_MS) {
            printf("[ASMP] Error: render sync timeout (epoch=%u done5=%u)\n",
                   (unsigned int)target, (unsigned int)*d5);
            return -2;
        }
#ifdef _WIN32
        Sleep(0); /* ワーカースレッドへ実行を譲る (完全ビジーループは CPU を浪費し
                     シミュレート済みワーカーの進行まで遅らせていた) */
#elif defined(__NuttX__)
        {
            /* 実機: 短期待ちのためビジースピン。usleep はシステムティック
             * (既定 10ms) へ丸められ、Ping-Pong の重畳が崩れて実スループット
             * がリアルタイム割れする原因だった (専用コアなのでスピン可) */
            for (volatile int spin = 0; spin < 512; spin++) {
                ASMP_BARRIER();
            }
        }
#else
        asmp_sleep_us(25);
#endif
    }

    /* S3: per-slot化 - targetスロットのepoch_framesでコピーサイズ決定 (単一framesでは切替時破壊) */
    uint32_t slot_t = ASMP_EPOCH_SLOT(target);
    asmp_dcache_invalidate((const void *)&shm->render_ctrl.epoch_frames[slot_t], sizeof(shm->render_ctrl.epoch_frames[slot_t]));
    asmp_dcache_invalidate((const void *)&shm->render_ctrl.slot_epoch[slot_t], sizeof(shm->render_ctrl.slot_epoch[slot_t]));
    ASMP_BARRIER();
    uint32_t ef_target = shm->render_ctrl.epoch_frames[slot_t];
    uint32_t got_epoch = shm->render_ctrl.slot_epoch[slot_t];
    if (ef_target == 0u || ef_target > ASMP_BUFFER_FRAMES) ef_target = frames;
    if (got_epoch != target) {
        printf("[ASMP][SLOT MISMATCH] end_frame slot %u exp %u got %u\n", (unsigned)slot_t, (unsigned)target, (unsigned)got_epoch);
    }
    ASMP_BARRIER();
    asmp_dcache_invalidate(shm->pcm_sub5_master[slot_t],
                           ((size_t)ef_target * sizeof(int16_t) * 2 + 31u) & ~31u);
    memcpy(master_out, shm->pcm_sub5_master[slot_t],
           (size_t)ef_target * sizeof(int16_t) * 2);
#ifdef PROFILE_ENABLE
    profile_epoch_end(0, target);
#endif

    return 0;
}

void asmp_manager_get_loads(const AsmpManager *mgr, uint16_t loads[ASMP_NUM_CORES])
{
    if (!mgr || !mgr->ctx || !loads) return;
    const AsmpSharedContext *shm = mgr->ctx;
    for (int i = 0; i < ASMP_NUM_CORES; i++) {
        loads[i] = shm->core[i].cpu_load;
    }
}

void asmp_manager_set_main_load(AsmpManager *mgr, uint16_t load_permille)
{
    if (!mgr || !mgr->ctx) return;
    mgr->ctx->core[ASMP_CORE_MAIN].cpu_load = load_permille;
}

void asmp_manager_set_resync_callback(AsmpManager *mgr, AsmpResyncCb cb, void *user)
{
    if (!mgr) return;
    mgr->resync_cb = cb;
    mgr->resync_user = user;
}

bool asmp_manager_health_check_and_recover(AsmpManager *mgr)
{
    if (!mgr || !mgr->is_initialized || !mgr->are_cores_running || !mgr->ctx) {
        return true;
    }

    /* コア毎に「連続停滞秒数」を判定する。全コア共有のカウンタだと
     * 異なるコアの一時的な揺らぎが累積して誤再起動されるため */
    const AsmpSharedContext *shm = mgr->ctx;
    int stalled_core = -1;
    uint8_t max_stall = 0;
    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        if (shm->core[i].heartbeat == mgr->wd_prev_hb[i]) {
            if (mgr->wd_stall[i] < 0xFFu) mgr->wd_stall[i]++;
        } else {
            mgr->wd_stall[i] = 0;
        }
        mgr->wd_prev_hb[i] = shm->core[i].heartbeat;
        if (mgr->wd_stall[i] > max_stall) {
            max_stall = mgr->wd_stall[i];
            stalled_core = i;
        }
    }

    if (max_stall < 3) {
        return true; /* 一時的な揺らぎは 3 回 (約 3 秒) までは様子見 */
    }

    printf("[ASMP][WD] Heartbeat stall on SubCore %d (%u sec consecutive). Restarting...\n",
           stalled_core, (unsigned int)max_stall);

    asmp_manager_stop_cores(mgr);
    memset(mgr->wd_prev_hb, 0, sizeof(mgr->wd_prev_hb));
    memset(mgr->wd_stall, 0, sizeof(mgr->wd_stall));
    mgr->published_epoch = 0; /* 共有側memset(0)と同期。残すとtarget 1000 vs done 0で2秒timeout確定 */

    if (asmp_manager_start_cores(mgr) == 0) {
        printf("[ASMP][WD] SubCores restarted successfully.\n");
        /* 再起動した SubCore は初期状態に戻っているため、
         * Main 側が把握している音量/テンポ/選曲を再送する */
        if (mgr->resync_cb) {
            mgr->resync_cb(mgr->resync_user);
        }
        return true;
    }

    printf("[ASMP][WD] Restart FAILED. Requesting fallback to single-core.\n");
    return false;
}

/**
 * @brief 全サブコアの停止とリソース解放
 */
void asmp_manager_stop_cores(AsmpManager *mgr)
{
    if (!mgr) return;

    if (mgr->ctx && mgr->are_cores_running) {
        mgr->ctx->main_ctrl.shutdown_requested = true;
        asmp_dcache_clean((const void *)&mgr->ctx->main_ctrl.shutdown_requested, sizeof(mgr->ctx->main_ctrl.shutdown_requested));
        ASMP_BARRIER();
    }

#ifndef __NuttX__
    if (mgr->are_cores_running) {
#ifdef _WIN32
        for (int i = 1; i < ASMP_NUM_CORES; i++) {
            HANDLE th = (HANDLE)mgr->thread_handles[i];
            if (th) {
                /* 待機結果を必ず確認する。タイムアウト時はハンドルを開放せず
                 * 残置し (生存スレッドの目印)、start_cores 側の再開防止に使う。
                 * CloseHandle 後に start_cores が memset すると ghost スレッドが
                 * 解放済みメモリへ書く競合を起こしていた */
                DWORD w = WaitForSingleObject(th, 3000);
                if (w == WAIT_OBJECT_0) {
                    CloseHandle(th);
                    mgr->thread_handles[i] = NULL;
                } else {
                    printf("[ASMP] Warning: SubCore %d thread did not exit in 3s. Handle kept.\n", i);
                }
            }
        }
#else
        for (int i = 1; i < ASMP_NUM_CORES; i++) {
            if (mgr->thread_ids[i]) {
#if defined(__linux__) && defined(__GLIBC__)
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 3;
                int jret = pthread_timedjoin_np((pthread_t)mgr->thread_ids[i], NULL, &ts);
                if (jret == 0) {
                    mgr->thread_ids[i] = 0;
                } else {
                    printf("[ASMP] Warning: SubCore %d thread did not exit in 3s. Handle kept.\n", i);
                    /* タイムアウト時はthread_idsを残置しstart_cores側の再開防止に使う (Winと同等) */
                }
#else
                /* timedjoin無し環境は従来通り無限待ち。H2のshutdownガードで通常は3秒以内に抜ける */
                pthread_join((pthread_t)mgr->thread_ids[i], NULL);
                mgr->thread_ids[i] = 0;
#endif
            }
        }
#endif
    }
#else
    for (int i = 1; i < ASMP_NUM_CORES; i++) {
        mptask_t *task = (mptask_t *)mgr->mp_tasks[i];
        if (task) {
            int wret = -1;
            /* WD 発火時はワーカーが停止している可能性が高いため、
             * graceful 失敗時は kill で確実に回収してから mp オブジェクトを
             * 解体する (ゾンビタスクが mutex/shm を掴んだまま残るのを防ぐ) */
            if (mptask_destroy(task, false, &wret) < 0) {
                printf("[ASMP] Warning: graceful destroy failed (SubCore %d), killing...\n", i);
                if (mptask_destroy(task, true, &wret) < 0) {
                    printf("[ASMP] Error: mptask_destroy(kill) failed (SubCore %d).\n", i);
                }
            }
            mgr->mp_tasks[i] = NULL;
        }
    }

    /* 二重 stop 呼び出しで mutex/shm を再破壊しないよう running 中のみ実施 */
    if (mgr->are_cores_running) {
        mpmutex_destroy(&s_asmp_mutex);
        if (mgr->ctx) {
            mpshm_detach(&s_asmp_shm);
            mgr->ctx = NULL;
        }
        mpshm_destroy(&s_asmp_shm);
    }
#endif

    mgr->are_cores_running = false;
    printf("[ASMP] All SubCores stopped.\n");
}
