/**
 * @file synth_main.c
 * @brief Sony Spresense ASMP 6コア完全分散処理 MIDI & シンセサイザープレイヤー
 * @details 16ch MIDI、GMドラムキット、PolyBLEPオシレータ、JoyStick Shield
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#ifdef _WIN32
#include <windows.h>
#define usleep(us) Sleep((DWORD)((us) / 1000))
#else
#include <unistd.h>
#include <time.h>
#endif

/**
 * @brief Cortex-M4F FPU デノーマル対策 (FZ/DN)。詳細は sub_common.h 参照
 */
#if defined(__NuttX__) && (defined(__arm__) || defined(__ARM_ARCH))
static void main_fpu_denormal_init(void)
{
#  if defined(__GNUC__) || defined(__clang__)
    uint32_t fpscr;
    __asm__ volatile(
        "vmrs %0, fpscr\n"
        "orr  %0, %0, #(3 << 24)\n" /* bit 24 (FZ) | bit 25 (DN) */
        "vmsr fpscr, %0\n"
        : "=r"(fpscr)
        :
        : "memory"
    );
#  endif
}
#endif

/**
 * @brief ポータブルなミリ秒タイマ (未使用: GetMonotonicUsへ移行済み、ホスト互換のため残置)
 */
#if defined(__GNUC__) || defined(__clang__)
static uint32_t GetTickCountMs(void) __attribute__((unused));
#else
static uint32_t GetTickCountMs(void);
#endif
static uint32_t GetTickCountMs(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u));
#endif
}

static uint64_t GetMonotonicUs(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000ull / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* レンダリングチャンク: 最大 512fr (10.67ms)。
 * 実際のエポック幅は s_chunk_frames (256/512 可変) を用い、
 * 低レイテンシ運用と安全側運用を実行時に切り替える */
#define RENDER_CHUNK_FRAMES   (512)
#define CHUNK_FAST_FRAMES     (256u)  /* 5.33ms: 256フレーム (超低レイテンシ・既定) */
#define CHUNK_SAFE_FRAMES     (512u)  /* 10.67ms: 512フレーム (過負荷退避用) */

/* 毎秒テレメトリ ([RENDER]/[SPECTRUM]/[6-CORE LOADS]/[EPOCH US]) の有効化。
 * 0 = 通常運用 (書式化コストゼロ・最軽量)。
 * 1 = 実機チューニング時のみ。ビルドし直して S2 のエポック時間を計測する */
#ifndef SYNTH_TELEMETRY_ENABLE
#define SYNTH_TELEMETRY_ENABLE 0
#endif

#ifndef SYNTH_RELEASE
#define SYNTH_RELEASE 1 /* 1 = プロダクション・リリース (ホットパスのログを全廃) */
#endif


#if SYNTH_MULTICORE
#include <sys/stat.h>
#include <sys/mount.h>
#include <nuttx/drivers/ramdisk.h>
#include "romfs.h"
#define SYNTH_ROMFS_SECTOR    512
#endif

#include "synth_engine.h"
#include "midi_parser.h"
#include "asmp_protocol.h"
#include "asmp_manager.h"
#include "audio_player.h"
#include "joystick_shield.h"
#include "synth_controller.h"
#include "sd_midi.h"
#include "sd_loader.h"
#include "sd_player.h"
#include "async_logger.h"
#include "boot_diag.h"
#ifdef PROFILE_ENABLE
#include "rt_profile.h"
#endif




static volatile sig_atomic_t s_running = 1;

static void sigint_handler(int signo)
{
    (void)signo;
    s_running = 0;
}

/* ========================================================================= */
/* SD MIDI レーン状態 (WD 再同期フックからも参照するためファイルスコープ)      */
/* ========================================================================= */
static AsmpManager s_asmp_mgr;
static SdLaneState s_sd = {0};

#if SYNTH_MULTICORE
/* WD 再起動後の状態再同期 (音量/消音/テンポを SubCore へ再送) */
static void asmp_resync_from_main(void *user)
{
    SynthController *ctrl = (SynthController *)user;
    if (ctrl) {
        synth_controller_resync_asmp(ctrl);
    }
    /* SD レーン再生中ならテンポも再公開する。
     * 共有メモリは再起動時にゼロ初期化されるため、再送しないと
     * Sub5 の BPM 同期ディレイが既定 500000µs (120BPM 扱い) のまま
     * 次の曲ロードまでズレ続ける */
    if (s_sd.active && s_sd.loaded) {
        sd_publish_tempo(&s_asmp_mgr, &s_sd.song);
    }
}
#endif

#ifdef __NuttX__
int synth_main(int argc, FAR char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
#if defined(__NuttX__) && (defined(__arm__) || defined(__ARM_ARCH))
    main_fpu_denormal_init();
#endif
    /* 診断ビーコン: ここを通ったことだけでも音で判別する
     * (3回の高音 = カーネル起動 & アプリ進入成功) */
    boot_diag_log("synth_main entered argc=%d", argc);
    boot_diag_beep(3, 2000);

    float volume = 0.85f;
    if (argc > 1) {
        volume = (float)atof(argv[1]);
    }

    printf("=================================================================\n");
    printf("  HexaMIDI: Sony Spresense ASMP 6-Core Parallel MIDI Synthesizer\n");
    printf("=================================================================\n");
    printf("[MAIN] Volume: %.2f\n", volume);
    printf("[MAIN][BUILD] DSP revision: " HEXASENSE_DSP_TAG "\n");
    fflush(stdout);

    /* 1. ASMP 6コア共有コンテキストの初期化 */
    printf("[MAIN] 1. Initializing ASMP 6-Core Context...\n");
    fflush(stdout);
    asmp_manager_init(&s_asmp_mgr);

#if SYNTH_MULTICORE
    {
        struct stat st;
        if (stat(ASMP_ROMFS_MOUNTPT, &st) < 0) {
            printf("[MAIN] Registering worker ROMFS at " ASMP_ROMFS_MOUNTPT "...\n");
            fflush(stdout);
            int rd_ret = romdisk_register(0, (FAR uint8_t *)romfs_img,
                                          (romfs_img_len + SYNTH_ROMFS_SECTOR - 1) / SYNTH_ROMFS_SECTOR,
                                          SYNTH_ROMFS_SECTOR);
            if (rd_ret < 0) {
                printf("[MAIN] Error: romdisk_register failed: %d\n", rd_ret);
                return -1;
            }
            if (mount("/dev/ram0", ASMP_ROMFS_MOUNTPT, "romfs", MS_RDONLY, NULL) < 0) {
                printf("[MAIN] Error: mount(" ASMP_ROMFS_MOUNTPT ") failed\n");
                return -1;
            }
        }
    }
#endif

#if SYNTH_MULTICORE
    bool multicore_active = (asmp_manager_start_cores(&s_asmp_mgr) == 0);
    if (!multicore_active) {
        /* ワーカー ELF 読み込み失敗などでも無音のまま放置しない:
         * ローカル合成 + シーケンサーへフォールバックする */
        printf("[MAIN] ASMP start failed -> falling back to single-core synthesis\n");
        fflush(stdout);
    }
    boot_diag_log("asmp multicore=%d", (int)multicore_active);
    boot_diag_beep(multicore_active ? 2 : 1, multicore_active ? 880 : 440);
#else
    bool multicore_active = false;
#endif

    /* 2. 音声合成エンジンの初期化 */
    printf("[MAIN] 2. Initializing 16ch Multi-Timbral DSP Engine...\n");
    fflush(stdout);
    static SynthEngine s_engine;
    synth_engine_init(&s_engine);
    synth_engine_set_master_volume(&s_engine, volume);

    /* 3. JoyStick Shield の初期化 */
    printf("[MAIN] 3. Initializing JoyStick GPIO (D2-D8)...\n");
    fflush(stdout);
    joystick_shield_init();

    /* 4. コントローラーの初期化 */
    static SynthController s_controller;
    synth_controller_init(&s_controller, &s_engine);
    /* CLI 引数の音量を反映 (controller_init の既定値 0.70 を上書き) */
    synth_controller_set_volume(&s_controller, volume);
#if SYNTH_MULTICORE
    if (multicore_active) {
        synth_controller_bind_asmp(&s_controller, &s_asmp_mgr);
        /* WD 再起動後に音量等を SubCore へ再送するフック */
        asmp_manager_set_resync_callback(&s_asmp_mgr, asmp_resync_from_main, &s_controller);
    }
#endif

    /* 5. オーディオ出力 (CXD5247 DMA / /dev/audio/pcm0) の初期化 */
    printf("[MAIN] 4. Initializing Hardware DMA Audio Player...\n");
    fflush(stdout);
    static AudioPlayer s_player;
    AudioPlayerConfig audio_cfg = {
        .sample_rate = SYNTH_SAMPLE_RATE,
        .channels = SYNTH_CHANNELS,
        .bits_per_sample = SYNTH_BITS_PER_SAMPLE,
        .volume_db = 0
    };

    if (audio_player_init(&s_player, &audio_cfg) < 0) {
        printf("[MAIN] Error: Audio Player initialization failed!\n");
        return -1;
    }
    boot_diag_log("audio init ok");

    /* 7a. SD カード MIDI 自動読み込み */
    sd_midi_ensure_mount(); /* /mnt/sd0 マウント保証 (録音・診断ログも利用) */

    /* SD 非同期ローダー起動: RT ループ内では mount/scan/fread を一切行わず
     * 結果ポーリングのみ行う (音飛び & JoyStick ポーリング凍結の根絶)。
     * 起動直後はオーディオ未始動のため完了を同期待ちしても安全 (上限 15 秒) */
    if (sd_loader_start() == 0) {
        if (sd_loader_request_first()) {
            s_sd.req_inflight = true;
        }
        for (int wait_ms = 0;
             wait_ms < 15000 && s_sd.req_inflight && !s_sd.active;
             wait_ms += 10) {
            SdLoaderResult r;
            MidiSong loaded;
            uint32_t lidx = 0;
            memset(&loaded, 0, sizeof(loaded));
            if (sd_loader_poll(&r, &loaded, &lidx)) {
                s_sd.req_inflight = false;
                if (r == SD_LOADER_RESULT_LOADED) {
                    s_sd.song = loaded;
                    s_sd.loaded = true;
                    s_sd.active = true;
                    s_sd.index = lidx;
                }
                break;
            }
            usleep(10000);
        }
    } else {
        printf("[MAIN] Warning: SD loader worker unavailable. SD lane disabled.\n");
    }

    if (s_sd.active) {
        uint32_t cnt = 0;
        char nm[SD_MIDI_NAME_LEN];
        sd_loader_get_info(&cnt, NULL, nm, sizeof(nm));
        printf("[MAIN] SD MIDI player engaged (%u files). First: %s\n",
               (unsigned int)cnt, nm);

        sd_publish_tempo(&s_asmp_mgr, &s_sd.song);
    }
    /* コントローラへ SD レーンの状態を通知 (曲送り操作の可否判定用) */
    synth_controller_set_sd_active(&s_controller, s_sd.active);

    /* 5. Audio DMA 始動は全ての重い初期化 (SD) の後へ遅らせる:
     *    START 直前のプレフィル (4 APB × 1024fr ≈ 85ms) が低速初期化中に
     *    消化されて起動直後アンダーラン → 恒久無音化するのを防ぐ */

    printf("\n*** JOYSTICK SHIELD CONTROLS ACTIVE (SD MIDI Player / Performance) ***\n");
    printf("  Player:  [A]=Play/Pause ⏯   [B]/Stick-R=Next Track ⏩   [C]=3D Spatial Toggle 🎧   [D]/Stick-L=Prev Track ⏪\n");
    printf("           [E]=Volume Down 🔉  [F]=Volume Up 🔊  [Stick Y]=Master Volume\n");
    printf("  Perform: [A-D]=Scale Notes  [Stick]=Zone Nav  [E]=Octave  [F]=Wave  [K]=Arp/Snare\n");
    printf("  [E]+[F] together = Mode Switch\n\n");
    fflush(stdout);

    s_running = 1;
    signal(SIGINT, sigint_handler);

    static int16_t s_pcm_buffer[RENDER_CHUNK_FRAMES * SYNTH_CHANNELS];
    JoystickState joy_state;
    uint64_t total_rendered_frames = 0; /* uint64: 経過時間表示の 24.9h wrap 防止 */
    uint32_t loop_iteration = 0;
    int32_t main_load_avg = 0; /* Main Core 負荷の移動平均 (0.1% 単位 <<3) */
    static uint8_t s_gov_stage = 0; /* 品質ガバー段階 0=5osc / 1=3osc / 2=1osc (テレメトリ表示用) */

    /* --- 適応チャンク制御 -------------------------------------------------
     * 既定は低レイテンシの 256fr (5.33ms, 発音遅延 半減)。
     * 過負荷シグナル (同期タイムアウト / Main 負荷持続) で安全側の
     * 512fr へ退避し、余裕が 60 秒継続したら FAST へ復帰する。
     * ヒステリシスで頻繁な切替 (音質/タイミングへの悪影響) を防ぐ */
    uint32_t s_chunk_frames = CHUNK_SAFE_FRAMES; /* 256既定はM4F 156MHzではbudget 5333でS3/S4 5kとmargin 9%のみ。Boogi_Marabiで起動直後pipe21を必ず叩くため既定を512(10666)に倒し、60s健全でFASTへ自動復帰。レイテンシ10msは人耳閾値以下 */
    uint32_t iter_per_sec = SYNTH_SAMPLE_RATE / s_chunk_frames;   /* 94 / 187 */
    uint32_t chunk_budget_us = (uint32_t)((uint64_t)s_chunk_frames * 1000000ull / SYNTH_SAMPLE_RATE);
    uint32_t safe_hold_until_ms = 0;   /* SAFE 強制中: この時刻まで復帰しない */
    uint32_t overload_ms = 0;          /* Main 過負荷の連続累積 */
    uint32_t healthy_ms = 0;           /* 余裕状態の連続累積 */

    printf("[MAIN] Entering real-time audio synthesis loop...\n");
    fflush(stdout);

    /* 非同期ロガー起動: 以降、音声ループ内のログは UART ブロッキングを
     * 回避するため async_logf 経由で出力される */
    async_log_start();

    printf("[MAIN] 5. Starting Audio DMA Stream...\n");
    fflush(stdout);
    audio_player_start(&s_player);
    boot_diag_log("audio dma started");

    /* --- セクション時間計測 (音飛び原因特定用・毎秒集計) --- */
    uint32_t mx_joy = 0, mx_pipe = 0, mx_write = 0;

    while (s_running) {
        uint64_t chunk_start_us = GetMonotonicUs();
        uint32_t chunk_start_ms = (uint32_t)(chunk_start_us / 1000ull);
        (void)chunk_start_ms; /* monotonic ms は各所で直接 GetMonotonicUs()/1000 を使用 */

        /* A0. パイプライン公開は MIDI確定後に行う (下記 B0後) */
        uint32_t us_begin = 0;
        (void)us_begin;

        uint32_t sec_joy = 0, sec_pipe = 0, sec_write = 0;

        /* A. JoyStick 入力のポーリングとコントローラー更新
         * 計測は 1ms 粒度 GetTickCountMs では FAST 5.33ms に対し 18% 誤差のため
         * 単調 us クロックで測る (負荷ヒステリシス誤発動の根絶, C5 と同方式) */
        uint64_t tsec_us = GetMonotonicUs();
        joystick_shield_poll(&joy_state);
        synth_controller_update(&s_controller, &joy_state);
        uint32_t us_joy = (uint32_t)(GetMonotonicUs() - tsec_us);
        sec_joy = us_joy / 1000u;

        /* A2. SD 再生/一時停止トグル要求 (Aボタン) */
        static bool s_sd_paused = false;
        if (s_controller.sd_play_pause_request) {
            s_controller.sd_play_pause_request = false;
            if (s_sd.active && s_sd.loaded) {
                s_sd_paused = !s_sd_paused;
                if (s_sd_paused) {
                    /* 一時停止: 発音中ノートを即座に消音 */
                    if (multicore_active && s_controller.asmp != NULL) {
                        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF };
                        asmp_manager_send_command(&s_asmp_mgr, ASMP_CORE_SUB1_SEQ, &off);
                    } else {
                        synth_engine_all_notes_off(&s_engine);
                    }
                    async_logf("[MAIN] SD Playback PAUSED.\n");
                } else {
                    async_logf("[MAIN] SD Playback RESUMED.\n");
                }
            }
        }

        /* A2. SD 曲送り要求 (プレイヤーモード B/D/スティック左右フリック):
         * 完奏時と同一の手順で現行曲を解放してから、非同期ローダーへ
         * 次/前曲を要求する。ロード要求中は連打分を捨てる */
        if (s_controller.sd_skip_request != 0) {
            const int8_t dir = s_controller.sd_skip_request;
            s_controller.sd_skip_request = 0;
            if (s_sd.active && s_sd.loaded && !sd_loader_busy()) {
                s_sd_paused = false;
                sd_lane_release_current(&s_sd, &s_engine, multicore_active && s_controller.asmp != NULL, &s_asmp_mgr);
                bool accepted = (dir > 0) ? sd_loader_request_next(s_sd.index)
                                          : sd_loader_request_prev(s_sd.index);
                if (accepted) {
                    s_sd.req_inflight = true;
                    async_logf("[MAIN] SD skip %s requested.\n",
                               (dir > 0) ? "next" : "prev");
                }
            }
        }

        /* A1. SD カード遅延リスキャン (5 秒毎・非同期):
         * ローダー ワーカーへ要求するだけ。mount/scan が RT ループを
         * ブロックすることはない (旧実装は最大 ~1 秒凍結していた)
         * 累積は us 精度で正確に 5 秒を刻む (旧ms切捨ては 5.33->5msで遅延) */
        if (!s_sd.active && !s_sd.req_inflight) {
            s_sd.rescan_acc_us += (uint32_t)((uint64_t)s_chunk_frames * 1000000ull / SYNTH_SAMPLE_RATE);
            if (s_sd.rescan_acc_us >= 5000000u) {
                s_sd.rescan_acc_us -= 5000000u; /* 余りを保持してドリフト0 */
                if (sd_loader_request_first()) {
                    s_sd.req_inflight = true;
                }
            }
        }

        /* B-1. SD ローダー結果の回収 (ノンブロッキング)。
         * ロード完了曲はここでアクティブ化し、次フレームから配信する */
        {
            SdLoaderResult r;
            MidiSong loaded;
            uint32_t lidx = 0;
            if (sd_loader_poll(&r, &loaded, &lidx)) {
                s_sd.req_inflight = false;
                if (r == SD_LOADER_RESULT_LOADED) {
                    s_sd.song = loaded;
                    s_sd.song.events = g_sd_player_ring.events;
                    s_sd.loaded = true;
                    s_sd.active = true;
                    s_sd.index = lidx;
                    s_sd.event_idx = 0; /* 新曲の先頭からイベント配信 */
                    s_sd.time = 0;      /* タイムスタンプ同期リセット */
                    s_sd.last_ts = 0;   /* 完奏テール判定用もリセット */
                    s_sd_paused = false;
                    sd_publish_tempo(&s_asmp_mgr, &s_sd.song);
                    synth_controller_set_sd_active(&s_controller, true);
                    async_logf("[MAIN] SD streaming started [%u] (prefill %u events)\n",
                               (unsigned int)lidx,
                               (unsigned int)s_sd.song.event_count);
                } else if (r == SD_LOADER_RESULT_EXHAUSTED) {
                    s_sd.active = false;
                    s_sd.loaded = false;
                    synth_controller_set_sd_active(&s_controller, false);
                    async_logf("[MAIN] No playable SD MIDI remains. Lane disabled.\n");
                }
                /* NO_FILES: 未挿入/空カード -> 5 秒毎の再スキャンが継続 */
            }
        }

        /* B0. SD MIDI レーン: イベント配信 (Sub1 キュー or ローカルエンジン)
         * ストリーミング方式: SPSC リングバッファ (g_sd_player_ring) から逐次消費 */
        uint64_t tsd_start = GetMonotonicUs();
        const bool sd_dispatch = s_sd.active && s_sd.loaded && !s_sd_paused &&
                                 (synth_controller_mode(&s_controller) != CTRL_MODE_PERFORMANCE);
        uint32_t block_events = 0;
        if (sd_dispatch) {
            uint64_t block_start = s_sd.time;
            uint64_t block_end   = block_start + s_chunk_frames;
            uint32_t delivered = 0;
            MidiEvent ev;

            while (sd_ring_peek(&g_sd_player_ring, &ev)) {
                if ((uint64_t)ev.timestamp_samples >= block_end) break;

                /* 同一タイムスタンプの和音原子性:
                 * Note On の実音和音 (<=16音) がブロック境界で分裂し「ジャラン」と遅れるのを防ぐ。
                 * CC/PC 等の制御メッセージや 16音超の初期化バーストは遅延させず、
                 * キュー容量 (126件) まで一気に連続配信する */
                if (ev.type == MIDI_STATUS_NOTE_ON) {
                    uint32_t grp = 1;
                    MidiEvent next_ev;
                    while (grp <= 16 && sd_ring_peek_at(&g_sd_player_ring, grp, &next_ev) &&
                           next_ev.timestamp_samples == ev.timestamp_samples &&
                           next_ev.type == MIDI_STATUS_NOTE_ON) {
                        grp++;
                    }
                    if (grp <= 16 && delivered + grp > 126 && delivered > 0) {
                        break;
                    }
                }

                uint64_t ts = ev.timestamp_samples;
                uint16_t offset = (ts <= block_start) ? 0u : (uint16_t)(ts - block_start);
                if (offset >= s_chunk_frames) offset = 0; /* ガード */

                bool ok = sd_deliver_event(&s_asmp_mgr, &s_engine,
                                      multicore_active && s_controller.asmp != NULL, &ev,
                                      offset, MIDI_SOURCE_SMF);
                if (!ok) {
                    /* キュー満杯: 今回は中断し次チャンク先頭で再試行。
                     * 時間を進めてしまうと残存イベントが過去に置き去りになり音飛びするため
                     * s_sd.time を進めず次のフレームへ持ち越す */
                    break;
                }

                /* 配信成功: リングバッファから取り出して消費 */
                sd_ring_pop(&g_sd_player_ring, NULL);
                s_sd.event_idx++;
                /* 時系列ソート済みのため最終配信 ts が単調最大。完奏テール用に保持 */
                if (ev.timestamp_samples > s_sd.last_ts) {
                    s_sd.last_ts = ev.timestamp_samples;
                }
                delivered++;
                if (delivered >= 126) {
                    async_logf("[MIDI] block overflow %u events at time %llu (cap 126)\n",
                               (unsigned)delivered, (unsigned long long)block_start);
                    break;
                }
            }
            block_events = delivered;
            /* キュー満杯・cap 超過でブロック内イベントを残した場合は時刻を凍結し、
             * 次フレームで同一イベントから再試行する (バックプレッシャー)。
             * 505行コメントの通り、ここで block_end まで進めてしまうと残存イベントが
             * 過去に置き去りになり、後続チャンクで offset=0 の遅延バースト
             * (和音ダンゴ・フラム) として噴出するため、進めない。
             * なお巻き戻しはしない (rem_ts > block_start の場合のみ進める):
             * 過去への巻き戻しは二重配送・無限停滞を招くため取り残し時刻で止める。
             * 持続圧迫時はテンポが一時的にもたつくが、音の欠落よりはまし。
             * Sub1 死等の異常時は WD (3秒) が全コア再起動で復旧する */
            {
                MidiEvent rem_ev;
                if (sd_ring_peek(&g_sd_player_ring, &rem_ev) &&
                    (uint64_t)rem_ev.timestamp_samples < block_end) {
                    /* 未送信イベントが残っている: その時刻までしか進めない */
                    s_sd.time = (rem_ev.timestamp_samples > block_start)
                                    ? rem_ev.timestamp_samples : block_start;
                } else {
                    s_sd.time = block_end;
                }
            }

            /* 完奏判定 -> 次の SD 曲へ
             * ストリーミング方式: リング空 かつ ファイル終端 (is_eof) 到達 かつ
             * 最終イベント+1secテール経過。reader.total は生 tick 最大
             * (除外CC含む) で過大になり曲尻6秒無音を生むため、実配信の
             * last_ts+48000 (バッチと同式) で判定する */
            {
                uint64_t tail_done = (uint64_t)s_sd.last_ts + (uint64_t)SYNTH_SAMPLE_RATE;
                if (sd_ring_available(&g_sd_player_ring) == 0 && g_sd_player_ring.is_eof &&
                    s_sd.time >= tail_done) {
                    sd_lane_release_current(&s_sd, &s_engine, multicore_active && s_controller.asmp != NULL, &s_asmp_mgr);
                    if (sd_loader_request_next(s_sd.index)) {
                        s_sd.req_inflight = true;
                    }
                }
            }
        }
        uint32_t us_sd = (uint32_t)(GetMonotonicUs() - tsd_start);
        (void)us_sd;
        (void)block_events;

        /* A0. エポック公開 (MIDIブロック確定後) */
        uint64_t t_begin_start = GetMonotonicUs();
#if SYNTH_MULTICORE
        if (multicore_active) {
            asmp_manager_begin_frame(&s_asmp_mgr, s_chunk_frames);
        }
#endif
        us_begin = (uint32_t)(GetMonotonicUs() - t_begin_start);

#if SYNTH_MULTICORE
        /* C. PCM 音声合成レンダリング */
        uint64_t tpipe_us = GetMonotonicUs();
        if (multicore_active) {
            /* 非同期 Ping-Pong: 直前エポックの完成を待ち、その間 Sub2-4 は
             * 次エポックを並列合成中。スループット = max(全コア個別時間) */
            if (asmp_manager_end_frame(&s_asmp_mgr, s_pcm_buffer, s_chunk_frames) != 0) {
                memset(s_pcm_buffer, 0, sizeof(s_pcm_buffer));
                boot_diag_log("render sync timeout chunk=%u", (unsigned)s_chunk_frames);
                /* 同期タイムアウト = パイプライン過負荷の決定的シグナル:
                 * 安全側チャンクへ即退避し、30 秒間は FAST へ復帰しない */
                if (s_chunk_frames != CHUNK_SAFE_FRAMES) {
                    s_chunk_frames = CHUNK_SAFE_FRAMES;
                    iter_per_sec = SYNTH_SAMPLE_RATE / s_chunk_frames;
                    chunk_budget_us = (uint32_t)((uint64_t)s_chunk_frames * 1000000ull / SYNTH_SAMPLE_RATE);
                    async_logf("[CHUNK] sync timeout -> SAFE %ums (%u frames)\n",
                               chunk_budget_us / 1000u, s_chunk_frames);
                }
                safe_hold_until_ms = (uint32_t)(GetMonotonicUs() / 1000ull) + 30000u;
                overload_ms = 0;
            }
            uint32_t us_pipe = (uint32_t)(GetMonotonicUs() - tpipe_us);
            sec_pipe = us_pipe / 1000u;
        } else
#endif
        {
            /* シングルコア: 直接生成して確実に発音 */
#if defined(__NuttX__)
            {
                static uint32_t r_max_us = 0, r_sum_us = 0, r_cnt = 0;
                struct timespec ts0, ts1;
                clock_gettime(CLOCK_MONOTONIC, &ts0);
                synth_engine_render(&s_engine, s_pcm_buffer, s_chunk_frames);
                clock_gettime(CLOCK_MONOTONIC, &ts1);
                uint32_t us = (uint32_t)((ts1.tv_sec - ts0.tv_sec) * 1000000u +
                                         (ts1.tv_nsec - ts0.tv_nsec) / 1000u);
                if (us > r_max_us) r_max_us = us;
                r_sum_us += us;
                if (++r_cnt >= iter_per_sec) {
#if SYNTH_TELEMETRY_ENABLE
                    async_logf("[RENDER] avg=%u.%ums max=%u.%ums (budget=%u.%ums)\n",
                               (unsigned int)(r_sum_us / r_cnt / 1000u), (unsigned int)((r_sum_us / r_cnt % 1000u) / 100u),
                               (unsigned int)(r_max_us / 1000u), (unsigned int)((r_max_us % 1000u) / 100u),
                               (unsigned int)(chunk_budget_us / 1000u), (unsigned int)((chunk_budget_us % 1000u) / 100u));
#endif
                    r_max_us = 0; r_sum_us = 0; r_cnt = 0;
                }
            }
#else
            synth_engine_render(&s_engine, s_pcm_buffer, s_chunk_frames);
#endif

            /* D. 6コア共有メモリへのミラーリング (ASMP並列テレメトリ用) */
            asmp_manager_render(&s_asmp_mgr, s_pcm_buffer, s_chunk_frames);
        }

        /* E. Spresense DMA オーディオキューへの書き込み (部分書き込み再送ループ)
         *    P0-1 Fix: APBは 512fr (AUDIO_PLAYER_BUFFER_SIZE 2048B) で chunk と同サイズ。
         *    旧コメント「1024fr > 512fr」は誤り。n==0時は usleep(500)が NuttXで10msに
         *    丸められ 5回で50msスパイクを自作していた。audio_player_writeは非ブロッキング
         *    なので sleepせず即リトライする。sw_wait カウンタが毎秒 0 に近ければ正常 */
        uint64_t twrite_us = GetMonotonicUs();
        uint32_t written_frames = 0;
        static uint32_t s_sw_wait = 0;   /* APB 空き待ち回数 (秒集計用) */
        static uint32_t s_mx_sw_wait = 0;
        uint32_t sw_wait_this = 0;
        while (written_frames < s_chunk_frames && s_running) {
            int n = audio_player_write(&s_player,
                                       &s_pcm_buffer[written_frames * SYNTH_CHANNELS],
                                       s_chunk_frames - written_frames);
            if (n < 0) {
                printf("[MAIN] Audio write error: %d\n", n);
                s_running = 0;
                break;
            }
            if (n == 0) {
                sw_wait_this++;
                /* P0-1 Fix: usleep(500)はNuttXで10msに丸められ write 52msスパイクの現行犯。
                 * 専用Audio Feederが無い現状ではブロッキングせず即リトライ。
                 * 必要なら up_udelay(50) 程度のビジーウェイトに留める */
                continue;
            }
            {
                static bool s_first_write_logged = false;
                if (!s_first_write_logged && n > 0) {
                    s_first_write_logged = true;
                    boot_diag_log("audio first write (%d frames)", n);
                    boot_diag_beep(2, 1200);
                }
            }
            written_frames += (uint32_t)n;
        }
        s_sw_wait += sw_wait_this;
        if (sw_wait_this > s_mx_sw_wait) s_mx_sw_wait = sw_wait_this;
        uint32_t us_write = (uint32_t)(GetMonotonicUs() - twrite_us);
        sec_write = us_write / 1000u;
        if (sec_joy > mx_joy) mx_joy = sec_joy;
        if (sec_pipe > mx_pipe) mx_pipe = sec_pipe;
        if (sec_write > mx_write) mx_write = sec_write;

#if !SYNTH_RELEASE
        /* スパイク検知時のタイムライン出力 (write >= 15ms or pipe >= 15ms or underrun 発生時) */
        static uint32_t s_prev_und = 0;
        uint32_t cur_und = audio_player_get_underruns();
        if (sec_write >= 15u || sec_pipe >= 15u || cur_und > s_prev_und) {
            async_logf("[SPIKE] it=%u chunk=%ums [begin=%uu joy=%uu sd=%uu pipe=%uu write=%uu(wait=%u)] apb=%u und=%u\n",
                       (unsigned int)loop_iteration,
                       (unsigned int)((GetMonotonicUs() - chunk_start_us) / 1000ull),
                       (unsigned int)us_begin, (unsigned int)us_joy, (unsigned int)us_sd,
                       (unsigned int)sec_pipe * 1000u, (unsigned int)us_write,
                       (unsigned int)sw_wait_this,
                       (unsigned int)audio_player_get_free_apb(),
                       (unsigned int)cur_und);
        }
        s_prev_und = cur_und;
#endif

        if (!s_running && written_frames < s_chunk_frames) {
            break;
        }

        total_rendered_frames += written_frames;
        loop_iteration++;

#if !SYNTH_RELEASE
        /* SD 診断ログへの生存ハートビート (10秒毎) */
        if ((loop_iteration % (iter_per_sec * 10u)) == 0u) {
            boot_diag_log("hb frames=%llu chunk=%u",
                          (unsigned long long)total_rendered_frames,
                          (unsigned int)s_chunk_frames);
        }
#endif

        /* Step4 計装: ここから MAINSTAGE 集計までの「未計測区間」
         * (ガバナー/テレメトリ/負荷集計) を測る。
         * chunk_ms 平均がフレーム周期超過の際、write 以外の隠れコストを暴く
         * 1ms 粒度では FAST 5ms に対し誤差大のため monotonic us を使用 */
        (void)GetMonotonicUs();

        /* 品質ガバー: エポック処理時間ベースの段階的デグレード
           * 閾値はチャンク幅に比例して自動調整する
           * (budget の 70% 超を 0.2 秒継続で一段階デグレード 5osc -> 3osc -> 1osc、
           *  budget の 47% 未満を 5 秒継続で一段階復帰)
           * P1改善: 84%ではFAST 5.33ms時に8v(5.84ms)が300ms遅れて退避し
           * その間アンダーラン連鎖。75% (FAST時4000us) に下げると8vは即検出し
           * 早期に3oscへデグレード、体感ラグ-30%かつ音痩せは一時的。
           * Step6: 旧実装は毎秒評価 x 3 カウント = 反応 ~3 秒で、その間の
           * レンダ超過がオーディオバッファを枯渇させ「ラグ」の直接原因と
           * なっていたため、毎イテレーション評価 + ms 積算に変更した
           * Step8: さらに 75%->70% (FAST時3733us) / 300ms->200ms に短縮し、
           * 密集譜面の立ち上がりで即座に3oscへ縮退してアンダーラン連鎖を
           * 未然に防ぐ。ホストテストでは影響なし、実機でラグ体感 -15% */
        {
            const uint32_t gov_thr_hi = chunk_budget_us * 75u / 100u;
            const uint32_t gov_thr_lo = chunk_budget_us * 47u / 100u;
            /* 攻撃/復帰の継続時間しきい値 (ms)
             * 150→400msへ延長: 3-oscを1秒保持しトン(5→1の-4dB急痩せ)を緩和。
             * 75%へ緩和: 8757usでstage1に留まる範囲を広げ痩せを緩和
             * 1-oscは13k連続時のみ。pipeは7→9に微増だがund1維持 */
            const uint32_t GOV_ATTACK_MS = 80u;
            const uint32_t GOV_RECOVER_MS = 5000u;
            static uint32_t gov_hi_ms = 0, gov_lo_ms = 0;
            AsmpSharedContext *sc_gov = asmp_manager_context(&s_asmp_mgr);
            if (sc_gov) {
                /* Commit4: ガバナー分離 (melodic vs drum) - C4過負荷でユニゾンを落とさない */
                uint32_t melodic_busy = 0;
                for (int i = ASMP_CORE_SUB2_LEAD; i <= ASMP_CORE_SUB3_BASS; i++) {
                    if (sc_gov->core[i].render_busy_us > melodic_busy) {
                        melodic_busy = sc_gov->core[i].render_busy_us;
                    }
                }
                uint32_t drum_busy = sc_gov->core[ASMP_CORE_SUB4_DRUM].render_busy_us;
                /* 犯人E/F: Sub5タイムアウト/実underrunを一次情報で即時フィードバック（busy推定より速い） */
                static uint32_t prev_diag_timeout = 0;
                static uint32_t prev_underruns = 0;
                uint32_t cur_diag_timeout = sc_gov->diag_timeout;
                uint32_t cur_underruns = audio_player_get_underruns();
                bool hint_spike = (cur_diag_timeout != prev_diag_timeout) || (cur_underruns != prev_underruns);
                prev_diag_timeout = cur_diag_timeout;
                prev_underruns = cur_underruns;
                /* 2節: drum_busyは独立ヒステリシスでDRUM_ECOを駆動 (書き出し遅延残し最適化) */
                uint32_t busy_max = melodic_busy;

                uint32_t dt_ms = (uint32_t)((s_chunk_frames * 1000u) / SYNTH_SAMPLE_RATE);
                if (hint_spike) {
                    /* 実害が既に出たなら推定を待たず即座に1段階デグレード */
                    gov_hi_ms = GOV_ATTACK_MS;
#if !SYNTH_RELEASE
                    async_logf("[GOV][HINT] timeout=%u und=%u busy=%u drum=%u -> force GOV\n",
                               (unsigned)cur_diag_timeout, (unsigned)cur_underruns,
                               (unsigned)busy_max, (unsigned)drum_busy);
#endif
                }
                if (busy_max > gov_thr_hi) {
                    gov_hi_ms += dt_ms;
                    gov_lo_ms = 0;
                } else if (busy_max < gov_thr_lo) {
                    gov_lo_ms += dt_ms;
                    gov_hi_ms = 0;
                } else {
                    /* 中間域: 現状維持 (両カウンタ凍結) */
                }

                if (gov_hi_ms >= GOV_ATTACK_MS && s_gov_stage < 2u) {
                    s_gov_stage++;
#if !SYNTH_RELEASE
                    async_logf("[GOV] epoch busy=%uus -> stage %u (%s)\n",
                               (unsigned int)busy_max, (unsigned int)s_gov_stage,
                               (s_gov_stage == 1u) ? "unison 3-osc" : "unison OFF");
#endif
                    gov_hi_ms = 0;
                } else if (gov_lo_ms >= GOV_RECOVER_MS && s_gov_stage > 0u) {
                    s_gov_stage--;
#if !SYNTH_RELEASE
                    async_logf("[GOV] epoch busy=%uus -> stage %u (%s)\n",
                               (unsigned int)busy_max, (unsigned int)s_gov_stage,
                               (s_gov_stage == 0u) ? "unison 5-osc" : "unison 3-osc");
#endif
                    gov_lo_ms = 0;
                }
                /* 2節: DRUM_ECO独立ヒステリシス (melodic 400ms/5000msより早め攻撃・早め復帰) */
                static uint32_t drum_hi_ms = 0, drum_lo_ms = 0;
                static bool drum_eco_on = false;
                const uint32_t drum_thr_hi = chunk_budget_us * 80u / 100u;
                const uint32_t drum_thr_lo = chunk_budget_us * 50u / 100u;
                const uint32_t DRUM_ATTACK_MS = 200u;
                const uint32_t DRUM_RECOVER_MS = 3000u;
                uint32_t dt_ms_drum = dt_ms;
                if (hint_spike) {
                    drum_hi_ms = DRUM_ATTACK_MS;
                }
                if (drum_busy > drum_thr_hi) { drum_hi_ms += dt_ms_drum; drum_lo_ms = 0; }
                else if (drum_busy < drum_thr_lo) { drum_lo_ms += dt_ms_drum; drum_hi_ms = 0; }
                if (!drum_eco_on && drum_hi_ms >= DRUM_ATTACK_MS) {
                    drum_eco_on = true;
#if !SYNTH_RELEASE
                    async_logf("[GOV][DRUM] busy=%uus -> ECO ON\n", (unsigned)drum_busy);
#endif
                    drum_hi_ms = 0;
                } else if (drum_eco_on && drum_lo_ms >= DRUM_RECOVER_MS) {
                    drum_eco_on = false;
#if !SYNTH_RELEASE
                    async_logf("[GOV][DRUM] busy=%uus -> ECO OFF\n", (unsigned)drum_busy);
#endif
                    drum_lo_ms = 0;
                }

                /* フラグ反映: Main Core が唯一のライタなので直接代入でよい */
                uint8_t qf = 0;
                if (s_gov_stage >= 1u) qf |= ASMP_QF_UNISON_3OSC;
                if (s_gov_stage >= 2u) qf |= ASMP_QF_UNISON_OFF;
                if (drum_eco_on) qf |= ASMP_QF_DRUM_ECO;
                /* 余裕時HQ: 35%未満が5秒継続でワイドデチューン (512B増の前回freq LUTと合わせても破綻なし) */
                const uint32_t gov_hq_thr = chunk_budget_us * 35u / 100u;
                static uint32_t hq_lo_ms = 0;
                if (busy_max < gov_hq_thr && s_gov_stage==0u) {
                    hq_lo_ms += (uint32_t)((s_chunk_frames * 1000u) / SYNTH_SAMPLE_RATE);
                    if (hq_lo_ms >= 5000u) qf |= ASMP_QF_HQ_WIDE;
                } else {
                    hq_lo_ms = 0;
                }
                sc_gov->main_ctrl.quality_flags = qf;
            }
        }

        /* 死活監視はテレメトリ無効時も必ず実行する */
        if (loop_iteration % iter_per_sec == 0) {
#if SYNTH_MULTICORE
            if (multicore_active && !asmp_manager_health_check_and_recover(&s_asmp_mgr)) {
                /* 再起動失敗: 以降はシングルコア合成へ自動フォールバック */
                multicore_active = false;
                s_controller.asmp = NULL;   /* ローカルエンジン操作へ戻す */
            }
#endif
#if SYNTH_TELEMETRY_ENABLE
            {
                uint32_t elapsed_sec = (uint32_t)(total_rendered_frames / SYNTH_SAMPLE_RATE);
                uint16_t loads[ASMP_NUM_CORES];
                asmp_manager_get_loads(&s_asmp_mgr, loads);
                AsmpSharedContext *sc_tel = asmp_manager_context(&s_asmp_mgr);

                char sd_nm_tel[SD_MIDI_NAME_LEN];
                sd_nm_tel[0] = '\0';
                sd_loader_get_info(NULL, NULL, sd_nm_tel, sizeof(sd_nm_tel));
                const char *now_title = (s_sd.active && s_sd.loaded)
                                        ? sd_nm_tel
                                        : "(no media)";
                async_logf("[6-CORE LOADS] T+%us | %s%s | CPU%% [C0:%3u.%u S1:%3u.%u S2:%3u.%u S3:%3u.%u S4:%3u.%u S5:%3u.%u]\n",
                           (unsigned int)elapsed_sec,
                           s_sd.active ? "[SD] " : "",
                           now_title,
                           (unsigned int)(loads[0] / 10u), (unsigned int)(loads[0] % 10u),
                           (unsigned int)(loads[1] / 10u), (unsigned int)(loads[1] % 10u),
                           (unsigned int)(loads[2] / 10u), (unsigned int)(loads[2] % 10u),
                           (unsigned int)(loads[3] / 10u), (unsigned int)(loads[3] % 10u),
                           (unsigned int)(loads[4] / 10u), (unsigned int)(loads[4] % 10u),
                           (unsigned int)(loads[5] / 10u), (unsigned int)(loads[5] % 10u));
                if (sc_tel) {
                    /* エポック処理時間 (µs): max がフレームレートのボトルネック。
                     * リアルタイム予算は s_chunk_frames x 1e6 / 48kHz
                     * RT: 動的チャンネルルーティングの累積再割当て回数
                     * QF/GOV: Main が共有メモリへ公開した品質フラグと段階。
                     *   GOV:0 = Stage 0 (5-osc SuperSaw フル)。1 以上が続くと音が痩せる
                     * VC: 音源コアの瞬時発音数 [S2/S3/S4]。
                     *   S2 のビジー時間と相関させボイス数起因かカーネル起因かを判別 */
                    async_logf("[EPOCH US] chunk=%u budget=%uu | C0:%5u S1:%5u S2:%5u S3:%5u S4:%5u S5:%5u | RT:%u | QF:%u(GOV:%u %s) | VC:%u/%u/%u\n",
                               (unsigned int)s_chunk_frames,
                               (unsigned int)chunk_budget_us,
                               (unsigned int)sc_tel->core[0].render_busy_us,
                               (unsigned int)sc_tel->core[1].render_busy_us,
                               (unsigned int)sc_tel->core[2].render_busy_us,
                               (unsigned int)sc_tel->core[3].render_busy_us,
                               (unsigned int)sc_tel->core[4].render_busy_us,
                               (unsigned int)sc_tel->core[5].render_busy_us,
                               (unsigned int)sc_tel->route_moves,
                               (unsigned int)sc_tel->main_ctrl.quality_flags,
                               (unsigned int)s_gov_stage,
                                (s_gov_stage == 0u) ? "5-osc FULL" :
                                ((s_gov_stage == 1u) ? "3-osc" : "1-osc"),
                               (unsigned int)sc_tel->core[ASMP_CORE_SUB2_LEAD].voice_count,
                               (unsigned int)sc_tel->core[ASMP_CORE_SUB3_BASS].voice_count,
                               (unsigned int)sc_tel->core[ASMP_CORE_SUB4_DRUM].voice_count);
                    /* Commit1: 診断カウンタ集約 (1秒毎) + 5節 Q占有率 */
                     async_logf("[DIAG] C4 edges A:%u I:%u O:%u cand:%u | gap S2:%u S3:%u S4:%u S5:%u slot_mis:%u rej:%u qdrop:%u to:%u | Q:%u/%u/%u/%u BP:%u/%u/%u/%u\n",
                               (unsigned)sc_tel->diag_c4_active_edges,
                                (unsigned)sc_tel->diag_c4_idle_edges,
                                (unsigned)sc_tel->diag_c4_overload_edges,
                                (unsigned)sc_tel->diag_c4_candidate_changes,
                                (unsigned)sc_tel->diag_epoch_gap[ASMP_CORE_SUB2_LEAD],
                                (unsigned)sc_tel->diag_epoch_gap[ASMP_CORE_SUB3_BASS],
                                (unsigned)sc_tel->diag_epoch_gap[ASMP_CORE_SUB4_DRUM],
                                (unsigned)sc_tel->diag_epoch_gap[ASMP_CORE_SUB5_DSP],
                                (unsigned)sc_tel->diag_slot_mismatch,
                                (unsigned)sc_tel->diag_slot_rejected,
                                (unsigned)sc_tel->diag_queue_drop,
                                (unsigned)sc_tel->diag_timeout,
                               (unsigned)sc_tel->queue_depth[ASMP_CORE_SUB2_LEAD],
                               (unsigned)sc_tel->queue_depth[ASMP_CORE_SUB3_BASS],
                               (unsigned)sc_tel->queue_depth[ASMP_CORE_SUB4_DRUM],
                               (unsigned)sc_tel->queue_depth[ASMP_CORE_SUB5_DSP],
                               (unsigned)sc_tel->queue_backpressure_hits[ASMP_CORE_SUB2_LEAD],
                               (unsigned)sc_tel->queue_backpressure_hits[ASMP_CORE_SUB3_BASS],
                               (unsigned)sc_tel->queue_backpressure_hits[ASMP_CORE_SUB4_DRUM],
                               (unsigned)sc_tel->queue_backpressure_hits[ASMP_CORE_SUB5_DSP]);
                }
            }
#endif /* SYNTH_TELEMETRY_ENABLE */
        }

        /* Main Core 自身の負荷計測 (チャンク処理時間 / 現バジェット) */
        {
            uint32_t chunk_us = (uint32_t)(GetMonotonicUs() - chunk_start_us);
            if (chunk_us > 20000u) chunk_us = 20000u; /* 異常値クランプ */
            (void)chunk_us;
            /* 正確なバジェット (256fr=5333µs / 512fr=10667µs) に対する千分率 */
            uint32_t load_permille =
                (uint32_t)((uint64_t)chunk_us * 1000ull / chunk_budget_us);
            if (load_permille > 1000u) load_permille = 1000u;
            main_load_avg += (int32_t)load_permille - (main_load_avg >> 3); /* 移動平均 */
            asmp_manager_set_main_load(&s_asmp_mgr, (uint16_t)(main_load_avg >> 3));

            /* セクション時間の毎秒レポート (音飛び原因特定用):
             * chunk_ms の平均に対し joy/pipe/write の max が突出していれば
             * そのセクションが周期的ブロックの犯人 */
#if SYNTH_TELEMETRY_ENABLE
            if ((loop_iteration % iter_per_sec) == 0u && loop_iteration > 0u) {
                uint32_t misc_now = (uint32_t)((GetMonotonicUs() - t_misc_us) / 1000ull);
                (void)misc_now;
                uint32_t apb_f = 0, apb_l = 0, apb_q = 0;
                audio_player_get_buffer_states(&apb_f, &apb_l, &apb_q);
                uint32_t apb_drain = 0, apb_retry = 0, apb_hold = 0;
                audio_player_get_apb_stats(&apb_drain, &apb_retry, &apb_hold);

                /* MIDI pool 使用率 (SD演奏中の場合のみ有効) */
                uint32_t midi_used = s_sd.active ? s_sd.song.event_count : 0u;

                /* RT = route_moves (Sub1 チャンネル再割当て累積回数):
                 *   - 0   : 初期化直後または割当て変更なし
                 *   - 増加: 負荷バランサが音源コア間でチャンネルを移動した累積数
                 *   - 単調増加が正常。急増は「コア負荷が閾値を頻繁に超えている」証拠
                 *   ※ 秒あたり増分を出すことで「現在の再割当て活発度」が分かる */
                uint32_t rt_now = 0;
#if SYNTH_MULTICORE
                {
                    const AsmpSharedContext *sc_rt =
                        asmp_manager_context(&s_asmp_mgr);
                    if (sc_rt) rt_now = sc_rt->route_moves;
                }
#endif
                static uint32_t s_rt_prev = 0;
                uint32_t rt_delta = (rt_now >= s_rt_prev) ? (rt_now - s_rt_prev) : rt_now;
                s_rt_prev = rt_now;

                /* 優先5: voice_counts[SUB4] 露出 — ドラム同時発音ピークのモニタ用 */
                uint32_t vc2=0, vc3=0, vc4=0;
#if SYNTH_MULTICORE
                {
                    const AsmpSharedContext *sc_vc = asmp_manager_context(&s_asmp_mgr);
                    if (sc_vc) { vc2=sc_vc->core[ASMP_CORE_SUB2_LEAD].voice_count; vc3=sc_vc->core[ASMP_CORE_SUB3_BASS].voice_count; vc4=sc_vc->core[ASMP_CORE_SUB4_DRUM].voice_count; }
                }
#endif
                async_logf("[MAINSTAGE] chunk_ms=%u joy_max=%u pipe_max=%u write_max=%u"
                           " apb[F:%u L:%u Q:%u] drain_blk=%u enq_rtry=%u hold_max=%uus"
                           " und=%u sw_wait=%u"
                           " RT_cum=%u RT_delta=%u midi_pool=%u/%u VC:%u/%u/%u\n",
                           (unsigned int)chunk_ms, (unsigned int)mx_joy,
                           (unsigned int)mx_pipe, (unsigned int)mx_write,
                           (unsigned int)apb_f, (unsigned int)apb_l, (unsigned int)apb_q,
                           (unsigned int)apb_drain, (unsigned int)apb_retry,
                           (unsigned int)apb_hold,
                           (unsigned int)audio_player_get_underruns(),
                           (unsigned int)s_sw_wait,
                           (unsigned int)rt_now, (unsigned int)rt_delta,
                           (unsigned int)midi_used,
                           (unsigned int)SD_MIDI_MAX_EVENTS,
                           (unsigned int)vc2, (unsigned int)vc3, (unsigned int)vc4);
                mx_joy = mx_pipe = mx_write = mx_misc = 0;
                s_sw_wait = 0; s_mx_sw_wait = 0;
            }
#endif

            /* P0-2 Fix: 適応チャンク切替を無効化、512固定 (波形不連続・スロット長誤読防止)
             * 256<->512切替は epoch_frames のper-slot化だけでは不十分で、end_frameで
             * 前エポックの後半を古い残骸として読む事故が起きる。10ms固定の方が安全 */
#if 0 // DISABLED: Adaptive chunk switching - fixed to 512 (CHUNK_SAFE_FRAMES)
            uint32_t now_ms = (uint32_t)(GetMonotonicUs() / 1000ull);
            int32_t load_now = main_load_avg >> 3;

            if (s_chunk_frames == CHUNK_SAFE_FRAMES &&
                now_ms >= safe_hold_until_ms) {
                if (load_now < 700) {
                    healthy_ms += s_chunk_frames * 1000u / SYNTH_SAMPLE_RATE;
                    if (healthy_ms >= 60000u) {
                        s_chunk_frames = CHUNK_FAST_FRAMES;
                        iter_per_sec = SYNTH_SAMPLE_RATE / s_chunk_frames;
                        chunk_budget_us = (uint32_t)((uint64_t)s_chunk_frames * 1000000ull / SYNTH_SAMPLE_RATE);
                        healthy_ms = 0; overload_ms = 0;
                        async_logf("[CHUNK] recovered -> FAST %ums (latency halved)\n",
                                   chunk_budget_us / 1000u);
                    }
                } else {
                    healthy_ms = 0;
                }
            } else if (s_chunk_frames == CHUNK_FAST_FRAMES) {
                if (load_now > 750) {
                    overload_ms += s_chunk_frames * 1000u / SYNTH_SAMPLE_RATE;
                    if (overload_ms >= 400u) {
                        s_chunk_frames = CHUNK_SAFE_FRAMES;
                        iter_per_sec = SYNTH_SAMPLE_RATE / s_chunk_frames;
                        chunk_budget_us = (uint32_t)((uint64_t)s_chunk_frames * 1000000ull / SYNTH_SAMPLE_RATE);
                        safe_hold_until_ms = now_ms + 30000u;
                        overload_ms = 0; healthy_ms = 0;
                        async_logf("[CHUNK] main overload -> SAFE %ums\n",
                                   chunk_budget_us / 1000u);
                    }
                } else {
                    overload_ms = 0;
                }
            }
#endif
            (void)healthy_ms; (void)overload_ms; (void)safe_hold_until_ms;
        }

#ifndef __NuttX__
        usleep(10000);
#endif
    }

    /* 終了処理 */
    printf("\n[MAIN] Stopping synth...\n");
    fflush(stdout);
    sd_loader_stop(); /* 未取得ロード結果があれば解放してワーカー停止 */
    asmp_manager_stop_cores(&s_asmp_mgr);
    audio_player_stop(&s_player);
    audio_player_deinit(&s_player);
    joystick_shield_deinit();

    async_log_stop(); /* 残ログを排出してロガー停止 */

    printf("[MAIN] Done.\n");
    return 0;
}

#ifdef __NuttX__
int spresense_main(int argc, char *argv[])
{
    return synth_main(argc, argv);
}
#endif
