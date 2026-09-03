/**
 * @file audio_player.c
 * @brief Sony Spresense オーディオ出力デバイス制御実装 (超堅牢・高音質版)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#if defined(_WIN32) || defined(_MSC_VER)
#include <windows.h>
#include <io.h>
#define usleep(us) Sleep((DWORD)((us) / 1000))
#define close _close
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#ifdef __NuttX__
#include <nuttx/config.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#include <arch/board/board.h>
#include <arch/board/cxd56_audio.h>
#include <arch/chip/audio.h>
#endif

#include "audio_player.h"
#include "boot_diag.h"

#define SPRESENSE_AUDIO_DEV_PATH "/dev/audio/pcm0"
#define SPRESENSE_AUDIO_DEV_ALT  "/dev/pcm0"

/* 一時診断ログ (原因調査用): 通常は 0。ioctl 失敗系の調査時は 1 にする */
#define SYNTH_AUDIO_DEBUG 0

#ifdef __NuttX__
/* 起動時のハードウェア テストトーン (1000Hz)。通常運用では無効 */
#define AUDIO_BOOT_TONE_ENABLE 0

/* DMAバッファのアライメント (16バイト境界) */
static struct ap_buffer_s s_apbs[AUDIO_PLAYER_NUM_BUFFERS] __attribute__((aligned(4)));
static uint8_t s_buff_mem[AUDIO_PLAYER_NUM_BUFFERS][AUDIO_PLAYER_BUFFER_SIZE] __attribute__((aligned(16)));

static volatile uint32_t s_dequeue_count = 0;
static volatile uint32_t s_enqueue_count = 0;
static volatile uint32_t s_underrun_count = 0;
static volatile uint32_t s_ioerror_count = 0;
static volatile uint32_t s_starve_drops = 0; /* buffer starvation チャンク放棄数 */

/* APB lifecycle プロファイリング変数 */
static volatile uint32_t s_drain_blocked = 0;   /* drain後もF=0,L=0でwrite=0を返した回数 */
static volatile uint32_t s_enq_retry = 0;        /* ENQUEUEBUFFER 1回目失敗→再試行回数 */
static volatile uint32_t s_max_hold_us = 0;      /* acquire→submit の最大保持時間 (us) */
static uint64_t s_fill_apb_acquire_us = 0;       /* 現在Filling中APBのacquire時刻 */
static uint64_t s_last_restart_us = 0;           /* 最終STOP時刻 (二重発火抑止用) */

/* 空きバッファ管理:
 * DEQUEUE メッセージ到着を待たずとも、アプリは自前の apb をすべて把握している。
 * メッセージは「非ブロッキングで排出する通知」としてのみ扱い、空きプールを真実源とする。
 * (ブロッキング mq_receive 依存は、ドライバがアイドル状態になると
 *  「DEQUEUE 待ち ← 再生再開にはエンキューが必要」との循環待ちで永久停止する) */
static struct ap_buffer_s *s_free_apbs[AUDIO_PLAYER_NUM_BUFFERS];

/* 積み上げ中の APB (可変チャンク: 256fr x 2 回で 1 APB を満たす)。
 * stop/deinit 時は free pool へ返却してリークさせない */
static struct ap_buffer_s *s_fill_apb = NULL;
static size_t s_fill_off = 0;
static int s_free_top = 0;
static volatile bool s_restart_needed = false; /* UNDERRUN 後の DMA 再始動要求 */
static volatile bool s_restart_hard = false; /* true=IOERROR hard, false=UNDERRUN soft */

static struct ap_buffer_s *free_pool_pop(void)
{
    if (s_free_top <= 0) return NULL;
    return s_free_apbs[--s_free_top];
}

static void free_pool_push(struct ap_buffer_s *apb)
{
    if (s_free_top >= AUDIO_PLAYER_NUM_BUFFERS) return;
    for (int i = 0; i < s_free_top; i++) {
        if (s_free_apbs[i] == apb) return; /* 二重返却防止 */
    }
    s_free_apbs[s_free_top++] = apb;
}

/* ドライバからの通知メッセージを非ブロッキングで全て排出する */
static void drain_audio_msgs(mqd_t mq)
{
    struct audio_msg_s msg;
    for (;;) {
        ssize_t size = mq_receive(mq, (char *)&msg, sizeof(msg), NULL);
        if (size != sizeof(msg)) {
            break; /* EAGAIN: メッセージ無し */
        }
        switch (msg.msg_id) {
            case AUDIO_MSG_DEQUEUE:
                s_dequeue_count++;
                if (msg.u.ptr) {
                    free_pool_push((struct ap_buffer_s *)msg.u.ptr);
                }
                break;
            case AUDIO_MSG_UNDERRUN:
                s_underrun_count++;
                s_restart_needed = true;
                s_restart_hard = false; /* soft: STOP不要 */
                break;
            case AUDIO_MSG_IOERROR:
                s_ioerror_count++;
                s_restart_needed = true;
                s_restart_hard = true; /* hard: STOP必要 */
                break;
            default:
                break;
        }
    }
}
#endif

int audio_player_init(AudioPlayer *player, const AudioPlayerConfig *config)
{
    if (!player || !config) {
        return -EINVAL;
    }

    /* 二重 init ガード: 前回の fd/mq/REGISTERMQ を必ず解放してから初期化する
     * (memset 先行だと以前のハンドルが失われリークする) */
    if (player->is_initialized) {
        audio_player_deinit(player);
    }

    memset(player, 0, sizeof(AudioPlayer));
    player->config = *config;
    player->dev_fd = -1;
    player->mq_handle = NULL;

#ifdef __NuttX__
    /* 1. CXD5247 オーディオサブシステム電源投入 */
    printf("[AUDIO] Powering on CXD5247 codec...\n");
    bool pwr_ret = board_audio_power_control(true);
    if (!pwr_ret) {
        printf("[AUDIO] Warning: board_audio_power_control returned false\n");
    }
    boot_diag_log("audio power=%d amp_unmuted", (int)pwr_ret);

    /* 2. 再生専用動作時: CXD5247 のアナログマイク回路・ADC・バイアス電源を
     *    完全にシャットダウンし、3極イヤホン接続時のマイク端子からの
     *    ホワイトノイズ混入・アナロググランド回り込みを根絶する */
    cxd56_audio_dis_input();

    /* 3. 外部ヘッドホンアンプのミュート解除 */
    board_external_amp_mute_control(false);

#if AUDIO_BOOT_TONE_ENABLE
    /* 3. ハードウェアトーンジェネレータで起動音鳴動 (1000Hz) */
    board_audio_tone_generator(1, 0, 1000);
    usleep(150000); /* 150ms */
    board_audio_tone_generator(0, 0, 0);
    usleep(20000);
#endif

    /* 4. オーディオデバイスオープン */
    player->dev_fd = open(SPRESENSE_AUDIO_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (player->dev_fd < 0) {
#if SYNTH_AUDIO_DEBUG
        printf("[AUDIO][DBG] open(%s) failed errno=%d\n",
               SPRESENSE_AUDIO_DEV_PATH, errno);
#endif
        player->dev_fd = open(SPRESENSE_AUDIO_DEV_ALT, O_RDWR | O_CLOEXEC);
    }

    /* デバイスが開けない場合は明確に失敗させる (無音のまま
     * busy-loop に突入する事故を防ぐ) */
    if (player->dev_fd < 0) {
        printf("[AUDIO] Error: cannot open %s (errno=%d). "
               "Is CONFIG_AUDIO / CONFIG_AUDIO_CXD56 enabled?\n",
               SPRESENSE_AUDIO_DEV_PATH, errno);
        return -ENODEV;
    }
#if SYNTH_AUDIO_DEBUG
    printf("[AUDIO][DBG] dev_fd=%d\n", player->dev_fd);
#endif

    {
        /* 5. メッセージキュー作成 (非ブロッキング POSIX メッセージキュー) */
        snprintf(player->mq_name, sizeof(player->mq_name), "/mq_synth_%d", (int)getpid());
        
        struct mq_attr attr;
        attr.mq_maxmsg  = 16;
        attr.mq_msgsize = sizeof(struct audio_msg_s);
        attr.mq_curmsgs = 0;
        attr.mq_flags   = O_NONBLOCK;

        mqd_t mq = mq_open(player->mq_name, O_RDWR | O_CREAT | O_NONBLOCK, 0644, &attr);
        if (mq != (mqd_t)-1) {
            player->mq_handle = (void *)mq;

            /* 受信を確実に非ブロッキング化する (mq_getattr -> mq_setattr) */
            struct mq_attr nattr;
            if (mq_getattr(mq, &nattr) != 0) {
                nattr = attr;
            }
            nattr.mq_flags = O_NONBLOCK;
            if (mq_setattr(mq, &nattr, NULL) != 0) {
                printf("[AUDIO] Warning: mq_setattr(O_NONBLOCK) failed, continuing.\n");
            }

            /* メッセージキューをドライバへ登録 */
            if (ioctl(player->dev_fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq) < 0) {
                printf("[AUDIO] Error: REGISTERMQ failed.\n");
                mq_close(mq);
                player->mq_handle = NULL;
                close(player->dev_fd);
                player->dev_fd = -1;
                return -EIO;
            }
        } else {
            printf("[AUDIO] Error: mq_open failed.\n");
            close(player->dev_fd);
            player->dev_fd = -1;
            return -EIO;
        }

        /* 6. PCMフォーマット設定 */
        struct audio_caps_desc_s cap_desc;
        memset(&cap_desc, 0, sizeof(cap_desc));
        cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
        cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
        cap_desc.caps.ac_channels = config->channels;
        cap_desc.caps.ac_chmap = 0;
        cap_desc.caps.ac_controls.hw[0] = (uint16_t)(config->sample_rate & 0xffff);
        cap_desc.caps.ac_controls.b[2] = config->bits_per_sample;
        cap_desc.caps.ac_controls.b[3] = (uint8_t)((config->sample_rate >> 16) & 0xff);

        int cfg_ret = ioctl(player->dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap_desc);
        (void)cfg_ret;
#if SYNTH_AUDIO_DEBUG
        printf("[AUDIO][DBG] CONFIGURE ret=%d (rate=%u ch=%u bits=%u)\n",
               cfg_ret, (unsigned)config->sample_rate,
               config->channels, config->bits_per_sample);
#endif

        /* 7. 出力ボリューム設定 (最大 1000 / 0dB) */
        audio_player_set_volume(player, 0);

        /* 8. オーディオバッファ初期化と初回DMAキュー投入 */
        s_dequeue_count = 0;
        s_enqueue_count = 0;
        s_underrun_count = 0;
        s_ioerror_count = 0;
        s_restart_needed = false;
        s_free_top = 0; /* 初期状態は全バッファをドライバへ預けるため空 */

        int enq_failures = 0;
        for (int i = 0; i < AUDIO_PLAYER_NUM_BUFFERS; i++) {
            s_apbs[i].nmaxbytes = AUDIO_PLAYER_BUFFER_SIZE;
            s_apbs[i].nbytes = AUDIO_PLAYER_BUFFER_SIZE;
            s_apbs[i].curbyte = 0;
            s_apbs[i].flags = 0;
            s_apbs[i].samp = &s_buff_mem[i][0];
            memset(s_apbs[i].samp, 0, AUDIO_PLAYER_BUFFER_SIZE);
            nxmutex_init(&s_apbs[i].lock);

            struct audio_buf_desc_s buf_desc;
            buf_desc.numbytes = AUDIO_PLAYER_BUFFER_SIZE;
            buf_desc.u.buffer = &s_apbs[i];
            int enq_ret = ioctl(player->dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&buf_desc);
#if SYNTH_AUDIO_DEBUG
            printf("[AUDIO][DBG] ENQUEUE[%d] ret=%d\n", i, enq_ret);
#endif
            if (enq_ret < 0) {
                enq_failures++;
                free_pool_push(&s_apbs[i]); /* 投入失敗分のみアプリ側空きプールへ格納 */
            } else {
                s_enqueue_count++;
            }
        }
        if (enq_failures > 0) {
            printf("[AUDIO] Warning: %d/%d initial enqueue(s) failed (%s%d)\n",
                   enq_failures, AUDIO_PLAYER_NUM_BUFFERS,
                   "", enq_failures);
        }
        boot_diag_log("audio prefill ok=%u fail=%d",
                      (unsigned int)s_enqueue_count, enq_failures);
    }
#else
    printf("[AUDIO] Running in host simulation mode.\n");
#endif

    player->is_initialized = true;
    return 0;
}

int audio_player_start(AudioPlayer *player)
{
    if (!player || !player->is_initialized) {
        return -EINVAL;
    }

#ifdef __NuttX__
    if (player->dev_fd >= 0) {
        int start_ret = ioctl(player->dev_fd, AUDIOIOC_START, 0);
        (void)start_ret;
#if SYNTH_AUDIO_DEBUG
        printf("[AUDIO][DBG] START ret=%d\n", start_ret);
#endif
    }
#endif

    player->is_playing = true;
    return 0;
}

int audio_player_write(AudioPlayer *player, const int16_t *buffer, uint32_t frames)
{
    if (!player || !player->is_initialized || !buffer || frames == 0) {
        return -EINVAL;
    }

#ifdef __NuttX__
    if (player->dev_fd >= 0 && player->mq_handle) {
        mqd_t mq = (mqd_t)player->mq_handle;

        /* フェーズ別時間計測 (リアルタイムログ非出力: 変数のみ更新し上位が読む) */
        uint64_t t0 = 0;
        struct timespec _ts0, _ts1;
        clock_gettime(CLOCK_MONOTONIC, &_ts0);
        t0 = (uint64_t)_ts0.tv_sec * 1000000ull + (uint64_t)_ts0.tv_nsec / 1000ull;

        /* 1. 通知メッセージを排出し、空きバッファプールを更新 */
        drain_audio_msgs(mq);

        /* 2. アンダーラン/IOエラー復帰:
         *    CXD56 ドライバはアンダーラン時に DMA を自己停止する。
         *    このとき上位層の upper->started は true のままであるため、
         *    AUDIOIOC_START を送っても上位層に握りつぶされ二度と再生が
         *    始まらない (旧実装の永久無音デッドロックの直接原因)。
         *    正規の手順は STOP (started=false へ戻し、未処理バッファ返却) ->
         *    再エンキュー -> START である
         *    慢性ループ対策: 100ms以内の連続STOPは抑止 (前回STOP直後のUNDERRUNは残骸) 
         *    e: dの1000msは音飛び長期化のため100msへrevert (cの安定値) */
        uint64_t t_restart_us = 0;
        if (s_restart_needed) {
            if (!s_restart_hard) {
                /* Commit6: ソフトリフィル (STOP連鎖断ち) - UNDERRUNはSTOPせずゼロ詰めAPBで復帰 */
                s_restart_needed = false;
                drain_audio_msgs(mq);
                /* 最低1個、可能なら3個までゼロ埋めAPBを即時投入 */
                int soft_cnt = 0;
                for (int i = 0; i < 3 && s_free_top > 0; i++) {
                    struct ap_buffer_s *apb = free_pool_pop();
                    if (!apb) break;
                    memset(apb->samp, 0, apb->nmaxbytes);
                    apb->nbytes = apb->nmaxbytes;
                    apb->curbyte = 0;
                    struct audio_buf_desc_s bd; bd.numbytes = apb->nmaxbytes; bd.u.buffer = apb;
                    if (ioctl(player->dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&bd) == 0) {
                        s_enqueue_count++; soft_cnt++;
                    } else {
                        free_pool_push(apb); break;
                    }
                }
                if (soft_cnt > 0) {
                    ioctl(player->dev_fd, AUDIOIOC_START, 0);
                }
                /* ハードフラグはクリア済み、softはSTOP時刻を更新しない */
            } else {
                uint64_t now_us;
                clock_gettime(CLOCK_MONOTONIC, &_ts0);
                now_us = (uint64_t)_ts0.tv_sec * 1000000ull + (uint64_t)_ts0.tv_nsec / 1000ull;
                if (now_us - s_last_restart_us < 100000u) {
                    s_restart_needed = false;
                    s_restart_hard = false;
                } else {
                    uint64_t tr0 = now_us;
                    ioctl(player->dev_fd, AUDIOIOC_STOP, 0);
                    drain_audio_msgs(mq);
                    s_restart_needed = false;
                    s_restart_hard = false;
                    s_last_restart_us = tr0;
                    clock_gettime(CLOCK_MONOTONIC, &_ts1);
                    t_restart_us = (uint64_t)_ts1.tv_sec * 1000000ull + (uint64_t)_ts1.tv_nsec / 1000ull - tr0;
                    if (t_restart_us > 5000u) {
                        printf("[APB] STOP-recovery took %u us, free=%d\n",
                               (unsigned int)t_restart_us, s_free_top);
                    }
                }
            }
        }

        /* 3. 空きバッファが無い場合は即リターン (RT 予算 5.33ms 厳守)。
         *    旧 48*500us=24ms / 現行 6*500us=3ms でも FAST 予算を超過し
         *    pipe 20ms スパイクの主因。drain のみで即委譲し外側 500us
         *    リトライ (synth_main.c) に任せる。usleep排除でMainブロック0.1ms未満 */
        drain_audio_msgs(mq);
        if (s_free_top == 0 && s_fill_apb == NULL) {
            /* Q全満 = 正常な DMA 先行投入済み状態。
             * drain しても空きが来なかった = DMA が512fr再生完了前に次のwriteが来た。
             * s_drain_blocked++ で計測し、上位のusleep待機回数と照合する */
            s_drain_blocked++;
            return 0;
        }
        /* 計測バグ修正: 持ち越しAPBの hold が前回waitを含まないように、
         * 今回write入口で時刻をリセット (enq_rtry と hold_max の因果を分離) */
        if (s_fill_apb != NULL) {
            clock_gettime(CLOCK_MONOTONIC, &_ts1);
            s_fill_apb_acquire_us = (uint64_t)_ts1.tv_sec * 1000000ull + (uint64_t)_ts1.tv_nsec / 1000ull;
        }

        /* 4. 空きバッファへ PCM を積み上げ、満杯になった APB を DMA キューへ投入。
         *    1 APB = 512fr = 10.67ms (AUDIO_PLAYER_BUFFER_SIZE=2048B, eでrevert)。
         *    512frチャンクと同サイズのため1チャンクで1APBが満杯になる。
         *    APB を部分書きのまま出さないことで DMA バッファ深度 (~85ms=4096fr) と
         *    割り込みレートを維持する。
         *    APBサイズ(512fr) == chunk(512fr) のため、DMA 再生完了を
         *    待たずに次チャンクを積み上げられ C0 の write ブロックが解消される */
        {
            size_t src_off = 0;
            size_t remain = (size_t)frames * player->config.channels * sizeof(int16_t);
            bool enqueued_any = false;

            while (remain > 0) {
                if (s_fill_apb == NULL) {
                    if (s_free_top == 0) {
                        /* 全バッファ消化不能: 本チャンクの残りを放棄して
                         * Main ループを優先する (長いスタール後の自己復帰) */
                        s_starve_drops++;
                        if ((s_starve_drops & (s_starve_drops - 1)) == 0) {
                            printf("[AUDIO] Warning: buffer starvation, chunk dropped (x%u)\n",
                                   (unsigned int)s_starve_drops);
                        }
                        break;
                    }
                    s_fill_apb = free_pool_pop();
                    s_fill_off = 0;
                    /* 取得タイムスタンプを記録 */
                    clock_gettime(CLOCK_MONOTONIC, &_ts1);
                    s_fill_apb_acquire_us = (uint64_t)_ts1.tv_sec * 1000000ull + (uint64_t)_ts1.tv_nsec / 1000ull;
                }

                size_t space = (size_t)s_fill_apb->nmaxbytes - s_fill_off;
                size_t take = (remain < space) ? remain : space;
                memcpy(s_fill_apb->samp + s_fill_off,
                       (const uint8_t *)buffer + src_off, take);
                s_fill_off += take;
                src_off += take;
                remain -= take;

                if (s_fill_off >= (size_t)s_fill_apb->nmaxbytes) {
                    s_fill_apb->nbytes = s_fill_off;
                    s_fill_apb->curbyte = 0;

                    struct audio_buf_desc_s buf_desc;
                    buf_desc.numbytes = s_fill_apb->nmaxbytes;
                    buf_desc.u.buffer = s_fill_apb;

                    /* SUBMIT タイムスタンプ記録 — 計測は「今回memcpy直後のENQUEUE試行」のみに限定。
                     * 従来は s_fill_apb_acquire_us が前回失敗時の古い時刻のまま残り、次回リトライ時に
                     * holdが前回wait(10ms)を含んで 14ms 異常値として膨らんでいた (計測バグ)。
                     * 失敗時は次回計測が前回waitを含まないよう acquire時刻を更新する */
                    uint64_t t_submit;
                    clock_gettime(CLOCK_MONOTONIC, &_ts0);
                    t_submit = (uint64_t)_ts0.tv_sec * 1000000ull + (uint64_t)_ts0.tv_nsec / 1000ull;
                    uint32_t hold_us = (t_submit > s_fill_apb_acquire_us)
                                     ? (uint32_t)(t_submit - s_fill_apb_acquire_us) : 0u;
                    if (hold_us > s_max_hold_us) s_max_hold_us = hold_us;

                    int ret = ioctl(player->dev_fd, AUDIOIOC_ENQUEUEBUFFER,
                                    (unsigned long)(uintptr_t)&buf_desc);
                    if (ret < 0) {
                        /* 一時拒否はusleepせず即時1回だけ再試行、仍失敗なら持ち越し。
                         * 6*1ms=6msでもFAST 5.33ms超過でpipe 20msの第2熱源。
                         * s_fill_apbは未開放のまま次回writeへ持ち越す。
                         * 計測バグ修正: 持ち越し時は次回holdが前回waitを含まないよう時刻を更新 */
                        drain_audio_msgs(mq);
                        s_enq_retry++;
                        /* 再試行前に時刻を更新 — holdが前回waitを含まないように */
                        clock_gettime(CLOCK_MONOTONIC, &_ts0);
                        s_fill_apb_acquire_us = (uint64_t)_ts0.tv_sec * 1000000ull + (uint64_t)_ts0.tv_nsec / 1000ull;
                        if (ioctl(player->dev_fd, AUDIOIOC_ENQUEUEBUFFER,
                                  (unsigned long)(uintptr_t)&buf_desc) != 0) {
                            /* 仍失敗 — 次回リトライまで持ち越し。acquire時刻は既に更新済み */
                            return (int)(src_off / (player->config.channels * sizeof(int16_t)));
                        } else {
                            /* 再試行成功 — holdは再試行時の時刻で再計測 (小) */
                            clock_gettime(CLOCK_MONOTONIC, &_ts0);
                            t_submit = (uint64_t)_ts0.tv_sec * 1000000ull + (uint64_t)_ts0.tv_nsec / 1000ull;
                            hold_us = (t_submit > s_fill_apb_acquire_us)
                                    ? (uint32_t)(t_submit - s_fill_apb_acquire_us) : 0u;
                            if (hold_us > s_max_hold_us) s_max_hold_us = hold_us;
                        }
                    }
                    s_enqueue_count++;
                    enqueued_any = true;
                    s_fill_apb = NULL;
                    s_fill_off = 0;
                }
            }

            /* 5. 始動要求。
             *    - 初回: init の pre-enqueue 後に audio_player_start() 済みなので通常不要
             *    - STOP 復帰後: 上位 started=false なのでこの START が下位へ届き、
             *      キュー 3 個 (START_ENQUEUE_APB) 到達で HW が自動始動する
             *    - 通常再生中: 上位層が握りつぶすため実質ノーコスト */
            if (enqueued_any) {
                ioctl(player->dev_fd, AUDIOIOC_START, 0);
            }

            clock_gettime(CLOCK_MONOTONIC, &_ts1);
            (void)t0; (void)t_restart_us;
            return (int)(src_off / (player->config.channels * sizeof(int16_t)));
        }
    }
#endif

    return (int)frames;
}

/* 起動からの累積音抜けイベント数 (ドライバ UNDERRUN + starvation 放棄)。
 * MAINSTAGE テレメトリへの露出用 (#13)。ホストビルドでは常時 0 */
#ifdef __NuttX__
uint32_t audio_player_get_underruns(void)
{
    return s_underrun_count + s_starve_drops;
}

uint32_t audio_player_get_free_apb(void)
{
    return (uint32_t)s_free_top;
}

void audio_player_get_buffer_states(uint32_t *free_cnt, uint32_t *fill_cnt, uint32_t *queued_cnt)
{
    uint32_t f = (uint32_t)s_free_top;
    uint32_t l = (s_fill_apb != NULL) ? 1u : 0u;
    uint32_t q = (AUDIO_PLAYER_NUM_BUFFERS >= (f + l)) ? (AUDIO_PLAYER_NUM_BUFFERS - f - l) : 0u;
    if (free_cnt) *free_cnt = f;
    if (fill_cnt) *fill_cnt = l;
    if (queued_cnt) *queued_cnt = q;
}

/* APB プロファイリング統計取得 (毎秒テレメトリ用) */
void audio_player_get_apb_stats(uint32_t *drain_blocked, uint32_t *enq_retry,
                                  uint32_t *max_hold_us)
{
    if (drain_blocked) { *drain_blocked = s_drain_blocked; s_drain_blocked = 0; }
    if (enq_retry)     { *enq_retry     = s_enq_retry;     s_enq_retry = 0; }
    if (max_hold_us)   { *max_hold_us   = s_max_hold_us;   s_max_hold_us = 0; }
}
#else
uint32_t audio_player_get_underruns(void)
{
    return 0;
}

uint32_t audio_player_get_free_apb(void)
{
    return AUDIO_PLAYER_NUM_BUFFERS;
}

void audio_player_get_buffer_states(uint32_t *free_cnt, uint32_t *fill_cnt, uint32_t *queued_cnt)
{
    if (free_cnt) *free_cnt = AUDIO_PLAYER_NUM_BUFFERS;
    if (fill_cnt) *fill_cnt = 0;
    if (queued_cnt) *queued_cnt = 0;
}

void audio_player_get_apb_stats(uint32_t *drain_blocked, uint32_t *enq_retry,
                                  uint32_t *max_hold_us)
{
    if (drain_blocked) *drain_blocked = 0;
    if (enq_retry)     *enq_retry     = 0;
    if (max_hold_us)   *max_hold_us   = 0;
}
#endif

int audio_player_set_volume(AudioPlayer *player, int volume_db)
{
    if (!player || !player->is_initialized) {
        return -EINVAL;
    }
    /* ドライバの実効下限は 300 (= -35dB)。それ未満を要求されると
     * クランプで「要求より大きい音量」になっていたため、入力側で
     * レンジ外を明示的に飽和させる */
    if (volume_db > 0) volume_db = 0;
    if (volume_db < -35) volume_db = -35;

    player->config.volume_db = volume_db;

#ifdef __NuttX__
    board_external_amp_mute_control(false);

    if (player->dev_fd >= 0) {
        /* CXD5247 AUDIO_FU_VOLUME は 0.1dB 単位 (-1020=-102.0dB 〜 +120=+12.0dB)。
         * 旧式 (1000 + dB*20) は範囲外で -EINVAL 拒絶され、
         * コーデックが初期値の完全ミュートに固定される */
        int16_t vol_val = (int16_t)(volume_db * 10);
        if (vol_val > 120) vol_val = 120;
        if (vol_val < -1020) vol_val = -1020;

        struct audio_caps_desc_s cap_desc;
        memset(&cap_desc, 0, sizeof(cap_desc));
        cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
        cap_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
        cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
        cap_desc.caps.ac_controls.hw[0] = (uint16_t)vol_val;

        int ret = ioctl(player->dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap_desc);
        if (ret < 0) {
            printf("[AUDIO] Volume set failed: %d\n", ret);
        }
#if SYNTH_AUDIO_DEBUG
        else {
            printf("[AUDIO][DBG] volume %.1fdB (raw=%d)\n",
                   (double)(vol_val / 10.0), (int)vol_val);
        }
#endif
    }
#endif

    return 0;
}

int audio_player_stop(AudioPlayer *player)
{
    if (!player || !player->is_initialized) {
        return -EINVAL;
    }

    /* 積み上げ中の未満 APB は free pool へ返却 (残り ~5ms 分は破棄) */
#ifdef __NuttX__
    if (s_fill_apb != NULL) {
        free_pool_push(s_fill_apb);
        s_fill_apb = NULL;
        s_fill_off = 0;
    }
#endif

#ifdef __NuttX__
    if (player->dev_fd >= 0) {
        ioctl(player->dev_fd, AUDIOIOC_STOP, 0);
    }
#endif

    player->is_playing = false;
    return 0;
}

void audio_player_deinit(AudioPlayer *player)
{
    if (!player) return;

    if (player->is_playing) {
        audio_player_stop(player);
    }

#ifdef __NuttX__
    if (player->dev_fd >= 0) {
        if (player->mq_handle) {
            /* メッセージキュー登録解除 (カーネルパニック防止) */
            ioctl(player->dev_fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)player->mq_handle);
        }
        close(player->dev_fd);
        player->dev_fd = -1;
    }

    if (player->mq_handle) {
        mq_close((mqd_t)player->mq_handle);
        player->mq_handle = NULL;
        mq_unlink(player->mq_name);
    }

    /* ミューテックスの破棄 (初期化成功時のみ — open 失敗で未初期化 mutex を
     * 破壊すると UB になるため is_initialized を条件にする) */
    if (player->is_initialized) {
        for (int i = 0; i < AUDIO_PLAYER_NUM_BUFFERS; i++) {
            nxmutex_destroy(&s_apbs[i].lock);
        }
    }

    /* ポップノイズ防止シーケンス (ミュート後に20ms放電待機) */
    board_external_amp_mute_control(true);
    usleep(20000);
    board_audio_power_control(false);
#endif

    player->is_initialized = false;
}
