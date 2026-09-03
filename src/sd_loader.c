/**
 * @file sd_loader.c
 * @brief SD カード MIDI 非同期ローダー実装
 * @details 音声リアルタイムループからの SD ブロッキング IO (mount 再試行 最大 ~1 秒、
 *          ディレクトリ走査、最大 512KB の fread) を専用ワーカースレッドへ移管する。
 *
 * 背景: Main ループは 256/512 フレーム (=5.3/10.7ms) 周期で DMA オーディオへ
 * 書き込む必要があるが、DMA バッファは合計 8 x 512 フレーム = 85.3ms 分しかない。
 * 旧実装は SD ロード (100〜500ms+) や mount 再試行 (~1 秒) をループ内で直接呼ぶため、
 * 1) 音飛び (underrun)、2) JoyStick ポーリング凍結 (スティック/ボタン無反応)、
 * 3) WAV 録音スレッドとの SDHCI 排他で更なる悪化、が発生していた。
 *
 * スレッドモデル:
 *   Main (RT)          Worker (本モジュール)
 *   ----------------   ----------------------------------
 *   request_first/next -> コマンド受領
 *                      <- mount / scan / chunked fread + parse
 *   poll() で結果回収   -> MidiSong 所有権移転 (mutex 下でポインタ交換のみ)
 *   get_info() 10Hz    -> 表示スナップショット
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define SDLOADER_SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#include <pthread.h>
#include <unistd.h>
#define SDLOADER_SLEEP_MS(ms) usleep((ms) * 1000u)
#endif

#include "sd_loader.h"
#include "sd_midi.h"
#include "sd_player.h"

/* ストリーミング再生内部コンテキスト */
static struct {
    FILE             *fp;
    MidiStreamReader  reader;
    bool              active;
} s_stream;

/* ワーカー状態 */
typedef struct {
    /* スレッドハンドル */
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    bool thread_valid;

    /* 保護領域 (mutex) */
    struct {
        bool     busy;        /**< mount/scan/load 実行中 */
        uint32_t req;         /**< 0=なし 1=request_first 2=request_next */
        uint32_t next_start;  /**< request_next の開始インデックス */
        SdLoaderResult result;      /**< 完了結果 (poll 未回収分) */
        MidiSong song;              /**< LOADED 時の曲 (poll 未回収分を所有) */
        uint32_t loaded_index;
        uint32_t info_count;
        uint32_t info_index;
        char     info_name[SD_MIDI_NAME_LEN];
        bool     shutdown;
    } st;

    void *mutex;
} SdLoaderState;

static SdLoaderState s_loader;
static SdMidiList s_worker_list; /**< ワーカー私有 (Main からは参照されない) */

static void lock(void)
{
#ifdef _WIN32
    WaitForSingleObject((HANDLE)s_loader.mutex, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t *)s_loader.mutex);
#endif
}

static void unlock(void)
{
#ifdef _WIN32
    ReleaseMutex((HANDLE)s_loader.mutex);
#else
    pthread_mutex_unlock((pthread_mutex_t *)s_loader.mutex);
#endif
}

/* 結果発行 (worker 内)。song の所有権は構造体ごと移す */
static void publish_result(SdLoaderResult res, MidiSong *song, uint32_t index)
{
    lock();
    s_loader.st.result = res;
    if (res == SD_LOADER_RESULT_LOADED && song) {
        s_loader.st.song = *song;
        s_loader.st.loaded_index = index;
        memset(song, 0, sizeof(*song)); /* 所有権移転済み */
        s_loader.st.info_count = s_worker_list.count;
        s_loader.st.info_index = index;
        strncpy(s_loader.st.info_name, s_worker_list.files[index].name,
                sizeof(s_loader.st.info_name) - 1);
        s_loader.st.info_name[sizeof(s_loader.st.info_name) - 1] = '\0';
    }
    s_loader.st.busy = false;
    unlock();
}

/* ストリームを安全に閉じる */
static void close_active_stream(void)
{
    if (s_stream.active) {
        midi_stream_close(&s_stream.reader, NULL, 0);
        if (s_stream.fp) {
            fclose(s_stream.fp);
            s_stream.fp = NULL;
        }
        s_stream.active = false;
    }
}

void sd_loader_stop_stream(void)
{
    lock();
    close_active_stream();
    unlock();
}

/* ストリームを開いて初期 Prefill を行う */
static int open_stream_for_index(uint32_t index, MidiSong *out_song)
{
    close_active_stream();

    FILE *fp = sd_midi_open_file(&s_worker_list, index);
    if (!fp) return -2;

    int ret = midi_stream_open(&s_stream.reader, fp, 48000);
    if (ret != 0) {
        fclose(fp);
        return ret;
    }

    s_stream.fp = fp;
    s_stream.active = true;

    /* リングバッファ初期化は Main consumer との排他のため lock 下で即時実行。
     * 以降の Prefill ファイル I/O (数百ms級) は lock 外で行う:
     * 旧実装は prefill 全体を lock 保持したまま fread していたため、
     * Main の poll (10ms周期) が最大 400ms ブロックされ DMA アンダーラン=
     * 曲頭音飛びを起こしていた。open 中は Main 側 dispatch が loaded=false で
     * 停止中であり、同一ワーカー内の refill とも逐次 (request処理中は refill
     * しない) のため、prefill 読み出し自体は排他不要で安全 */
    lock();
    sd_ring_reset(&g_sd_player_ring);
    unlock();

    MidiEvent chunk[256];
    while (sd_ring_free_space(&g_sd_player_ring) > 0 && !s_stream.reader.is_eof) {
        uint32_t free_sp = sd_ring_free_space(&g_sd_player_ring);
        uint32_t to_read = (free_sp > 256) ? 256 : free_sp;
        uint32_t n = midi_stream_read(&s_stream.reader, chunk, to_read);
        if (n == 0) break;
        sd_ring_push_batch(&g_sd_player_ring, chunk, n);
    }
    if (s_stream.reader.is_eof) {
        SD_RING_BARRIER();
        g_sd_player_ring.is_eof = true;
        SD_RING_BARRIER();
    }

    memset(out_song, 0, sizeof(*out_song));
    out_song->format = s_stream.reader.format;
    out_song->num_tracks = s_stream.reader.num_tracks;
    out_song->ticks_per_quarter = s_stream.reader.ticks_per_quarter;
    out_song->initial_tempo_us = s_stream.reader.initial_tempo_us;
    out_song->total_samples = s_stream.reader.total_samples;
    strncpy(out_song->title, s_stream.reader.title, sizeof(out_song->title) - 1);
    out_song->event_count = sd_ring_available(&g_sd_player_ring);
    out_song->events = g_sd_player_ring.events;
    out_song->events_dynamic = false;

    printf("[SD] Stream Opened [%u] %s (Prefill %u events)\n",
           (unsigned int)index, s_worker_list.files[index].name,
           (unsigned int)out_song->event_count);
    return 0;
}

/* request_first: 先頭から演奏可能曲を探す */
static void do_request_first(void)
{
    sd_midi_ensure_mount(); /* ワーカー内なのでブロッキング可 (mount 再試行含む) */
    if (sd_midi_scan(&s_worker_list) == 0u) {
        publish_result(SD_LOADER_RESULT_NO_FILES, NULL, 0);
        return;
    }
    for (uint32_t i = 0; i < s_worker_list.count; i++) {
        MidiSong song;
        memset(&song, 0, sizeof(song));
        int rc = open_stream_for_index(i, &song);
        if (rc == 0) {
            publish_result(SD_LOADER_RESULT_LOADED, &song, i);
            return;
        }
        if (rc == -10) {
            printf("[SDLOADER] SKIP overflow [%u] %s rc=%d\n", (unsigned)i, s_worker_list.files[i].fs_name, rc);
        } else if (rc != 0) {
            printf("[SDLOADER] SKIP [%u] %s rc=%d\n", (unsigned)i, s_worker_list.files[i].fs_name, rc);
        }
        midi_parser_free_song(&song);
        SDLOADER_SLEEP_MS(2);
    }
    publish_result(SD_LOADER_RESULT_NO_FILES, NULL, 0);
}

/* request_dir 共通本体: last の dir 側 (+1=次 / -1=前) から順に試す */
static void do_request_dir(uint32_t last, int dir)
{
    sd_midi_ensure_mount();
    if (s_worker_list.count == 0u) {
        if (sd_midi_scan(&s_worker_list) == 0u) {
            publish_result(SD_LOADER_RESULT_EXHAUSTED, NULL, 0);
            return;
        }
    }
    for (uint32_t off = 1u; off <= s_worker_list.count; off++) {
        uint32_t try_i = (last + s_worker_list.count + dir * (int)off) %
                         s_worker_list.count;
        MidiSong song;
        memset(&song, 0, sizeof(song));
        int rc = open_stream_for_index(try_i, &song);
        if (rc == 0) {
            publish_result(SD_LOADER_RESULT_LOADED, &song, try_i);
            return;
        }
        if (rc == -10) {
            printf("[SDLOADER] SKIP overflow [%u] %s rc=%d\n", (unsigned)try_i, s_worker_list.files[try_i].fs_name, rc);
        } else if (rc != 0) {
            printf("[SDLOADER] SKIP [%u] %s rc=%d\n", (unsigned)try_i, s_worker_list.files[try_i].fs_name, rc);
        }
        midi_parser_free_song(&song);
        SDLOADER_SLEEP_MS(2);
    }
    /* 全滅時の再スキャン再試行 */
    {
        SdMidiList retry_list;
        memset(&retry_list, 0, sizeof(retry_list));
        retry_list.dir_index = -1;
        if (sd_midi_scan(&retry_list) != 0u) {
            s_worker_list = retry_list;
            uint32_t retry_cnt = s_worker_list.count;
            for (uint32_t i = 0; i < retry_cnt; i++) {
                uint32_t try_i = (dir > 0) ? i : (retry_cnt - 1 - i);
                MidiSong song;
                memset(&song, 0, sizeof(song));
                int rc = open_stream_for_index(try_i, &song);
                if (rc == 0) {
                    publish_result(SD_LOADER_RESULT_LOADED, &song, try_i);
                    return;
                }
                if (rc == -10) {
                    printf("[SDLOADER] RETRY SKIP overflow [%u] %s rc=%d\n", (unsigned)try_i, s_worker_list.files[try_i].fs_name, rc);
                } else if (rc != 0) {
                    printf("[SDLOADER] RETRY SKIP [%u] %s rc=%d\n", (unsigned)try_i, s_worker_list.files[try_i].fs_name, rc);
                }
                midi_parser_free_song(&song);
                SDLOADER_SLEEP_MS(2);
            }
        } else {
            printf("[SDLOADER] RETRY scan found 0, keeping cached %u files\n", (unsigned)s_worker_list.count);
        }
    }
    publish_result(SD_LOADER_RESULT_EXHAUSTED, NULL, 0);
}

/* request_next: last+1 以降を順に試す (末尾の次は先頭へ巻き戻す) */
static void do_request_next(uint32_t last)
{
    do_request_dir(last, +1);
}

/* request_prev: last-1 から逆方向に順に試す (先頭の前は末尾へ巻き戻す) */
static void do_request_prev(uint32_t last)
{
    do_request_dir(last, -1);
}

#ifdef _WIN32
static DWORD WINAPI loader_thread_entry(LPVOID arg)
#else
static void *loader_thread_entry(void *arg)
#endif
{
    (void)arg;
    printf("[SDLOADER] Worker started.\n");

    for (;;) {
        lock();
        uint32_t req = s_loader.st.req;
        uint32_t start = s_loader.st.next_start;
        bool shutdown = s_loader.st.shutdown;
        s_loader.st.req = 0;
        if (req != 0u) s_loader.st.busy = true;
        unlock();

        if (shutdown) break;

        if (req == 1u) {
            do_request_first();
        } else if (req == 2u) {
            do_request_next(start);
        } else if (req == 3u) {
            do_request_prev(start);
        } else {
            /* ストリーミング自動補充 (Refill)
             * s_stream (fp/reader/active) は Main の sd_loader_stop_stream
             * (fclose) と共有のため mutex 下で触る。旧実装は lock なしで
             * midi_stream_read (fseek/fread) しており、スキップ/完奏時の
             * Main 側 fclose と競合して use-after-close→破損イベント→音飛び
             * を起こしていた。read 中 (最大20ms級) の Main 側 stop 待ちは
             * DMA 85ms バッファで吸収可能な範囲であり、破損より遥かにまし。
             * リング自体は SPSC ロックフリーのまま (push は barrier のみ) */
            bool do_refill = false;
            uint32_t to_read = 0;
            lock();
            if (s_stream.active && !s_stream.reader.is_eof) {
                uint32_t free_sp = sd_ring_free_space(&g_sd_player_ring);
                if (free_sp >= 64) {
                    to_read = (free_sp > 256) ? 256 : free_sp;
                    do_refill = true;
                }
            }
            if (do_refill) {
                MidiEvent chunk[256];
                uint32_t n = midi_stream_read(&s_stream.reader, chunk, to_read);
                if (n > 0) {
                    sd_ring_push_batch(&g_sd_player_ring, chunk, n);
                }
                if (s_stream.reader.is_eof) {
                    SD_RING_BARRIER();
                    g_sd_player_ring.is_eof = true;
                    SD_RING_BARRIER();
                }
                bool still_room = (sd_ring_free_space(&g_sd_player_ring) >= 256) &&
                                  !s_stream.reader.is_eof;
                unlock();
                if (still_room) {
                    SDLOADER_SLEEP_MS(1);
                } else {
                    SDLOADER_SLEEP_MS(2);
                }
            } else {
                bool idle = !s_stream.active || s_stream.reader.is_eof;
                unlock();
                SDLOADER_SLEEP_MS(idle ? 20 : 2);
            }
        }
    }

    close_active_stream();

    /* 未回収の曲があれば解放 */
    lock();
    midi_parser_free_song(&s_loader.st.song);
    unlock();

    printf("[SDLOADER] Worker stopped.\n");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int sd_loader_start(void)
{
    memset(&s_loader, 0, sizeof(s_loader));
    memset(&s_worker_list, 0, sizeof(s_worker_list));
    s_worker_list.dir_index = -1;

#ifdef _WIN32
    s_loader.mutex = (void *)CreateMutexA(NULL, FALSE, NULL);
    if (!s_loader.mutex) return -1;
    s_loader.thread = CreateThread(NULL, 0, loader_thread_entry, NULL, 0, NULL);
    if (s_loader.thread == NULL) return -1;
    s_loader.thread_valid = true;
#else
    s_loader.mutex = malloc(sizeof(pthread_mutex_t));
    if (!s_loader.mutex) {
        printf("[SD] mutex malloc failed\n");
        return -1;
    }
    pthread_mutex_init((pthread_mutex_t *)s_loader.mutex, NULL);

    /* 明示スタック 14KB: 20KBではBSS 203KB+ヒープ枯渇でENOMEM(12)が発生するため縮小。
     * 実測でFAT+SMFパーサ+printfの最深スタックは~9KB、14KBで安全マージン4KB確保 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 14 * 1024u);

    int cret = pthread_create(&s_loader.thread, &attr, loader_thread_entry, NULL);
    pthread_attr_destroy(&attr);
    if (cret != 0) {
        printf("[SD] pthread_create failed: %d\n", cret);
        pthread_mutex_destroy((pthread_mutex_t *)s_loader.mutex);
        free(s_loader.mutex);
        s_loader.mutex = NULL;
        return -1;
    }
    s_loader.thread_valid = true;
#endif
    return 0;
}

void sd_loader_stop(void)
{
    if (!s_loader.thread_valid) return;

    lock();
    s_loader.st.shutdown = true;
    unlock();

#ifdef _WIN32
    WaitForSingleObject(s_loader.thread, 3000);
    CloseHandle(s_loader.thread);
#else
    pthread_join(s_loader.thread, NULL);
#endif
    s_loader.thread_valid = false;

    if (s_loader.mutex) {
#ifdef _WIN32
        CloseHandle((HANDLE)s_loader.mutex);
#else
        pthread_mutex_destroy((pthread_mutex_t *)s_loader.mutex);
        free(s_loader.mutex);
#endif
        s_loader.mutex = NULL;
    }
}

bool sd_loader_busy(void)
{
    lock();
    bool busy = s_loader.st.busy || s_loader.st.req != 0u || s_loader.st.result != SD_LOADER_RESULT_NONE;
    unlock();
    return busy;
}

bool sd_loader_request_first(void)
{
    lock();
    if (s_loader.st.busy || s_loader.st.req != 0u || s_loader.st.result != SD_LOADER_RESULT_NONE) { unlock(); return false; }
    s_loader.st.req = 1u;
    s_loader.st.busy = true;
    unlock();
    return true;
}

bool sd_loader_request_next(uint32_t last_played_index)
{
    lock();
    if (s_loader.st.busy || s_loader.st.req != 0u || s_loader.st.result != SD_LOADER_RESULT_NONE) { unlock(); return false; }
    s_loader.st.req = 2u;
    s_loader.st.next_start = last_played_index;
    s_loader.st.busy = true;
    unlock();
    return true;
}

bool sd_loader_request_prev(uint32_t last_played_index)
{
    lock();
    if (s_loader.st.busy || s_loader.st.req != 0u || s_loader.st.result != SD_LOADER_RESULT_NONE) { unlock(); return false; }
    s_loader.st.req = 3u;
    s_loader.st.next_start = last_played_index;
    s_loader.st.busy = true;
    unlock();
    return true;
}

bool sd_loader_poll(SdLoaderResult *res, MidiSong *out_song, uint32_t *out_index)
{
    if (!res) return false;
    *res = SD_LOADER_RESULT_NONE;

    lock();
    if (s_loader.st.result != SD_LOADER_RESULT_NONE && !s_loader.st.busy) {
        *res = s_loader.st.result;
        s_loader.st.result = SD_LOADER_RESULT_NONE;
        if (*res == SD_LOADER_RESULT_LOADED) {
            if (out_song) {
                *out_song = s_loader.st.song;
                memset(&s_loader.st.song, 0, sizeof(s_loader.st.song));
            } else {
                midi_parser_free_song(&s_loader.st.song);
            }
            if (out_index) *out_index = s_loader.st.loaded_index;
        }
        unlock();
        return true;
    }
    unlock();
    return false;
}

void sd_loader_get_info(uint32_t *count, uint32_t *index, char *name, size_t namesz)
{
    lock();
    if (count) *count = s_loader.st.info_count;
    if (index) *index = s_loader.st.info_index;
    if (name && namesz > 0) {
        strncpy(name, s_loader.st.info_name, namesz - 1);
        name[namesz - 1] = '\0';
    }
    unlock();
}
