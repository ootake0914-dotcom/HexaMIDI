/**
 * @file audio_player.h
 * @brief Sony Spresense オーディオ出力デバイス制御モジュール
 * @details CXD5602 / CXD5247 DMA オーディオドライバおよびNuttX PCMデバイス制御
 */

#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PLAYER_NUM_BUFFERS (16)
#define AUDIO_PLAYER_BUFFER_SIZE (2048) /* [f] 10 x 2048B = 20KB (+4KB) - 8でund 0でも密集で一時枯渇するため2枚増で107msへ
                                           * 1APB=512fr=10.67ms。dの4096はbudget超過でrevert済み
                                           * 気絶対策: FDN per-line(21KB)+delay(9KB)回収済みで headroom十分 */

/**
 * @brief オーディオプレイヤー設定
 */
typedef struct {
    uint32_t sample_rate;    /**< サンプリングレート (例: 48000) */
    uint8_t channels;        /**< チャンネル数 (1: モノラル, 2: ステレオ) */
    uint8_t bits_per_sample; /**< ビット深度 (16) */
    int volume_db;           /**< ボリューム (0dB ~ -102dB, 例: -10) */
} AudioPlayerConfig;

/**
 * @brief オーディオプレイヤー構造体
 */
typedef struct {
    int dev_fd;              /**< オーディオデバイスファイルディスクリプタ */
    void *mq_handle;         /**< メッセージキューハンドル (mqd_t) */
    char mq_name[32];        /**< メッセージキューパス */
    bool is_initialized;
    bool is_playing;
    AudioPlayerConfig config;
} AudioPlayer;

/**
 * @brief オーディオプレイヤーの初期化 (CXD5247 電源オン・アンプミュート解除・DMA設定)
 * @param player プレイヤー構造体へのポインタ
 * @param config 設定構造体へのポインタ
 * @return 成功時は 0, 失敗時は負のエラーコード
 */
int audio_player_init(AudioPlayer *player, const AudioPlayerConfig *config);

/**
 * @brief オーディオ再生の開始（DMA 開始）
 * @param player プレイヤー構造体へのポインタ
 * @return 成功時は 0, 失敗時は負のエラーコード
 */
int audio_player_start(AudioPlayer *player);

/**
 * @brief PCMデータの送信・再生 (DMAキューへのバッファ投入)
 * @param player プレイヤー構造体へのポインタ
 * @param buffer PCMデータバッファ (16bit ステレオ)
 * @param frames フレーム数 (サンプル数 = frames * channels)
 * @return 実際に書き込まれたフレーム数、またはエラーコード
 */
int audio_player_write(AudioPlayer *player, const int16_t *buffer, uint32_t frames);

/**
 * @brief 音量設定
 * @param player プレイヤー構造体へのポインタ
 * @param volume_db 音量 (dB: 0dB 〜 -102dB)
 * @return 成功時は 0, 失敗時は負のエラーコード
 */
/**
 * @brief 起動からの累積音抜けイベント数 (ドライバ UNDERRUN + starvation 放棄)
 * @note MAINSTAGE テレメトリへの露出用。ホストビルドでは常時 0
 */
uint32_t audio_player_get_underruns(void);
uint32_t audio_player_get_free_apb(void);
void audio_player_get_buffer_states(uint32_t *free_cnt, uint32_t *fill_cnt, uint32_t *queued_cnt);
void audio_player_get_apb_stats(uint32_t *drain_blocked, uint32_t *enq_retry,
                                  uint32_t *max_hold_us);

int audio_player_set_volume(AudioPlayer *player, int volume_db);

/**
 * @brief オーディオ再生の停止（DMA 停止）
 * @param player プレイヤー構造体へのポインタ
 * @return 成功時は 0, 失敗時は負のエラーコード
 */
int audio_player_stop(AudioPlayer *player);

/**
 * @brief オーディオプレイヤーの解放 (アンプミュート・電源遮断)
 * @param player プレイヤー構造体へのポインタ
 */
void audio_player_deinit(AudioPlayer *player);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H_ */
