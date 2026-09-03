/**
 * @file asmp_manager.h
 * @brief Sony Spresense ASMP 6コア完全分散処理 マネージャー
 * @details Main Core からの SubCore 1〜5 起動・監視・同期・コマンド送受信
 */

#ifndef ASMP_MANAGER_H_
#define ASMP_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include "asmp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsmpManager_s AsmpManager;

/**
 * @brief サブコア再起動後の状態再同期コールバック
 *        (音量/テンポ/選曲などを SubCore へ再送する処理を Main が登録する)
 */
typedef void (*AsmpResyncCb)(void *user);

struct AsmpManager_s {
    /* 実機 (__NuttX__): mpshm アタッチ領域へのポインタ
     * ホスト (_WIN32/Linux): プロセス内シミュレーション領域 */
    AsmpSharedContext *ctx;
#ifndef __NuttX__
    AsmpSharedContext ctx_storage;
#endif
    bool is_initialized;
    bool are_cores_running;
#ifdef _WIN32
    void *thread_handles[ASMP_NUM_CORES];
#elif defined(__NuttX__)
    void *mp_tasks[ASMP_NUM_CORES];   /**< struct mptask_s (実体は asmp_manager.c 内) */
#else
    unsigned long thread_ids[ASMP_NUM_CORES];
#endif

    /* Watchdog (ハートビート停滞検出: コア毎に連続停滞を判定) */
    uint32_t wd_prev_hb[ASMP_NUM_CORES];
    uint8_t  wd_stall[ASMP_NUM_CORES];   /**< コア毎の連続停滞回数 */

    /* 非同期パイプライン: Main 側で管理する公開エポック。
     * 共有メモリの render_epoch を直接読まない (WD 再起動で共有メモリが
     * リセットされた際のエポック巻き戻し -> target ラップを構造的に防止) */
    uint32_t published_epoch;

    /* 再起動後の状態再同期 (省略可) */
    AsmpResyncCb resync_cb;
    void *resync_user;
} ;

/**
 * @brief ASMP マネージャーの初期化
 */
int asmp_manager_init(AsmpManager *mgr);

/**
 * @brief 共有コンテキスト (AsmpSharedContext) の取得
 */
AsmpSharedContext *asmp_manager_context(AsmpManager *mgr);

/**
 * @brief SubCore 1〜5 の起動
 */
int asmp_manager_start_cores(AsmpManager *mgr);

/**
 * @brief 特定のサブコアへのメッセージ送信
 */
bool asmp_manager_send_command(AsmpManager *mgr, uint8_t target_core, const AsmpPacket *pkt);

/**
 * @brief シングルコアモード用テレメトリ更新 (PCM ミラーリング & ハートビート)
 * @note  マルチコアモードでは asmp_manager_sync_render_frame を使用すること
 */
void asmp_manager_render(AsmpManager *mgr, const int16_t *pcm_buffer, uint32_t frames);

/**
 * @brief 1フレーム分のレンダリング同期実行 (SubCore 1〜5 を駆動し、マスターPCMを取得)
 * @note  非同期パイプラインの互換ラッパ。内部で begin_frame + end_frame を
 *        連続呼び出しするため、オーバーラップは発生しない (1エポック遅延のみ)。
 *        ホスト統合テストツール向けに残置。
 */
int asmp_manager_sync_render_frame(AsmpManager *mgr, int16_t *master_out, uint32_t frames);

/**
 * @brief フレーム前半: 新エポックを先行公開し SubCore 2-4 の合成を開始させる
 * @return 公開したエポック番号
 *
 * Ping-Pong 非同期パイプラインの前半処理。この直後に Main 側で
 * MIDI/SD イベントを投入すると、SubCore が次エポックを合成している間も
 * メッセージがキューへ流し込まれる。
 */
/**
 * @brief フレーム前半: 新エポックの先行公開
 * @param frames このエポックでレンダリングするフレーム数
 *              (0 = ASMP_BUFFER_FRAMES。共有メモリへ公開され全 SubCore が統一参照)
 */
uint32_t asmp_manager_begin_frame(AsmpManager *mgr, uint32_t frames);

/**
 * @brief フレーム後半: 直前エポックのマスターPCM完了を待ち取得する
 * @param master_out マスターPCMの出力先
 * @param frames フレーム数 (通常 512)
 * @return 成功時 0, タイムアウト/エラー時 負値
 *
 * begin_frame で公開したエポックの **1つ前** の完成を待つ。
 * 待ち時間中に SubCore 2-4 は次エポックを並列合成するため、
 * スループット = max(全コア個別処理時間) となる。
 */
int asmp_manager_end_frame(AsmpManager *mgr, int16_t *master_out, uint32_t frames);

/**
 * @brief 全サブコアの停止とリソース解放
 */
void asmp_manager_stop_cores(AsmpManager *mgr);

/**
 * @brief 各コアの CPU 負荷 (0.1% 単位, 1000 = 100%) の取得
 */
void asmp_manager_get_loads(const AsmpManager *mgr, uint16_t loads[ASMP_NUM_CORES]);

/**
 * @brief Main Core 自身の負荷 (0.1% 単位) の登録
 */
void asmp_manager_set_main_load(AsmpManager *mgr, uint16_t load_permille);

/**
 * @brief サブコア再起動成功後に呼ばれる状態再同期コールバックを登録
 *        (音量・テンポ・選曲状態の再送など。未登録なら何もしない)
 */
void asmp_manager_set_resync_callback(AsmpManager *mgr, AsmpResyncCb cb, void *user);

/**
 * @brief サブコア死活監視 (1 秒周期で呼び出し)
 * @return true: 全コア正常 / false: 再起動に失敗した (呼び出し側はフォールバックへ)
 *
 * ハートビートが 3 回連続で停滞した SubCore があると、全コアを再起動して
 * パイプラインを復旧させる。復旧失敗時は呼び出し元がシングルコアへ倒す。
 */
bool asmp_manager_health_check_and_recover(AsmpManager *mgr);

#ifdef __cplusplus
}
#endif

#endif /* ASMP_MANAGER_H_ */
