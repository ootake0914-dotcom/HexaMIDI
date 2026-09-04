/**
 * @file sub1_main.c
 * @brief SubCore 1: MIDI イベントルーター & タイムマネージャー
 * @details Main Core (SD MIDI レーン) から受信したノート/CC イベントを
 *          各音源サブコア (2〜4) へルーティングし、16 分ステップクロックを生成する。
 *          内蔵プリセット/内蔵 SMF の自前再生機能は撤去済み (SD 専用プレイヤー化)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define sub1_sleep_us(us) Sleep((DWORD)((us) / 1000))
#elif defined(__NuttX__)
#include <nuttx/arch.h>
#define sub1_sleep_us(us) up_udelay((useconds_t)(us))
#else
#include <unistd.h>
#define sub1_sleep_us(us) usleep(us)
#endif

#define SUB_COMMON_NO_LUT /* Sub1はLUT未使用、BSS 2KB節約 OPTIMIZATION_MEMO #4 */
#include "sub_common.h"
#include "sub_spawn.h"
#include "rt_profile.h"

/* ヒューマナイズ用 Xorshift32 (rand()置換: ロック不要・スレッドセーフ) */
static uint32_t s_hum_seed = 0x1234ABCDu;

/* P0-C ノンドロップ制御プレーン: 受信済み最新 generation */
static uint32_t s_last_all_notes_off_gen[16];
static uint32_t s_last_all_sound_off_gen[16];
static uint32_t s_last_sustain_off_gen[16];

/* ========================================================================= */
/* 動的チャンネルルーティング                                                */
/* cyd-particle-life の「実測時間差によるジョブ配分の閉ループ補正」思想を移植。 */
/* ドラム (ch9) 以外は SubCore2/3 どちらにも割当可能とし、実測エポック処理時間 */
/* (render_busy_us) の偏りに応じてアイドル中チャンネルを移す。                */
/* 制約:                                                                     */
/*  - 移動対象はフレックスプール {2,3,5,8} のみ (リード/ベースの音色性格保護)  */
/*  - アクティブノートが無いチャンネルのみ移動 (note_off 消失を構造的に防止)   */
/*  - 移動時は宛先エンジンへ PROGRAM_CHANGE を再生 (音色状態の複製)            */
/* ========================================================================= */
#define ROUTE_MOVE_COOLDOWN_EPOCHS (192u)  /* 再割当ての最短間隔 (~2 秒) */
#define ROUTE_IMBALANCE_US         (1500u) /* 発動: 実測差がこれを超えたら */
#define ROUTE_MIN_BUSY_US          (2500u) /* 双方が低負荷なら何もしない */

typedef struct {
    uint8_t core;          /**< 現在の割当先 (SUB2_LEAD / SUB3_BASS) */
    uint8_t active_notes;  /**< 未解放ノート数 (移動可否判定) */
    uint8_t program_set;   /**< program 有効 */
    uint8_t program;       /**< 最終 PROGRAM_CHANGE 値 (移動時の再送用) */
    uint8_t pedal_down;    /**< CC#64 サステイン状態 (移動可否判定) */
    /* 完全スナップショット: migration時に宛先へ復元するチャンネル状態 */
    uint8_t bank_msb;      /**< CC#0 Bank Select MSB */
    uint8_t bank_set;      /**< バンク情報あり (MSB/LSB のいずれか受信済み) */
    uint8_t bank_lsb;      /**< CC#32 Bank Select LSB (バリエーション音色用) */
    uint8_t has_bank_lsb;
    int16_t pitch_bend;    /**< Pitch Bend (-8192..+8191) 中央0 */
    uint8_t volume;        /**< CC#7 */
    uint8_t pan;           /**< CC#10 */
    uint8_t expression;    /**< CC#11 */
    uint8_t modulation;    /**< CC#1 */
    uint8_t reverb_send;   /**< CC#91 */
    uint8_t has_volume, has_pan, has_expression, has_modulation, has_reverb;
} ChRoute;

static ChRoute s_route[16];
/* メモリ活用: ノート単位の動的コア追跡マップ (16ch x 128note = 2048B RAM)。
 * NOTE_ON 時に過負荷コアから余裕コアへ逃がしたノートを正確に記憶し、
 * NOTE_OFF を発音コアへ確実に配送して迷子ノートを完全防止する */
static uint8_t s_note_core_map[16][128];
/* メモリ活用: Core1 ローカル即時ボイスカウンタ (同フレーム内12音バーストでも完璧に1音ずつ交互分散) */
static uint8_t s_live_vc2 = 0;
static uint8_t s_live_vc3 = 0;
static uint32_t s_route_cooldown = 0;

/* ABI v13 spawn プールの Core1側 producer 状態 (Sub2/Sub3 各1)。
 * route_init でリセットする (再起動時に共有メモリ memset と歩調を合わせる) */
static SubSpawnProd s_spawn_prod2;
static SubSpawnProd s_spawn_prod3;

/* チャンネル状態の宛先への復元 (migration 用)。sub1_push_bounded 定義より後で
 * 定義し、ここでは前方宣言する */
static bool route_restore_to(AsmpSharedContext *shared, uint8_t dst, uint8_t ch, bool include_pc);

/* C4 (ドラム) 監視状態 (Governor 認識フェーズ) */
#define C4_STATE_IDLE       (0u)
#define C4_STATE_ACTIVE     (1u)
#define C4_STATE_OVERLOADED (2u)
static uint32_t s_c4_state = C4_STATE_IDLE;

/* C4 migration candidate (過負荷時の逃がし先候補。実際の移行は次フェーズ) */
#define C4_CAND_NONE             (0u)    /* 候補なし (NONE) */
#define C4_CAND_SAFETY_MARGIN_US (2500u) /* 候補先に要求する予算余裕 (deadline余裕) */
#define C4_CAND_TRANSFER_RESERVE_US (6000u) /* 移行後の worst-case cost を確保する予備マージン */
static uint32_t s_c4_candidate = C4_CAND_NONE;

/* 過負荷判定のヒステリシス: 境界付近で OVERLOADED/active が毎エポック揺れ、
 * 候補が散発的に切り替わってドラム音がコア間を跳ねるのを防ぐ */
static bool s_c4_overloaded = false;

/* 移行後の最小保持エポック数 (~683ms)。密集部では打楽器の合間で C4 の
 * render_busy_us が瞬間的に下がるため、これを下回る復帰を抑止して
 * 「移行→即復帰→過負荷→…」のリミットサイクルを断つ */
#define C4_MIGRATE_HOLD_EPOCHS (64u)
static uint32_t s_c4_migrate_hold = 0;

/* Kick / Metal 動的移行 (Phase 4/5): 移行先が変化した時のみログするための保持値 */

/* Commit3: 予測型3コア分散用コスト定義 (実測p99に置換予定、現段階は推定) */
typedef enum {
    DRUM_COST_KICK   = 450,
    DRUM_COST_SNARE  = 900,
    DRUM_COST_HAT    = 1400,
    DRUM_COST_CYMBAL = 2100,
    DRUM_COST_TOM    = 500,
    DRUM_COST_CLAP   = 700
} DrumCostUs;
static uint32_t s_reserved_us[ASMP_NUM_CORES] = {0};
static uint32_t s_current_budget_us = 10666u; /* 512fr default, epoch毎に更新 */

static inline uint32_t drum_cost_for_note(uint8_t note)
{
    if (note == 35 || note == 36) return DRUM_COST_KICK;
    if (note == 38 || note == 40) return DRUM_COST_SNARE;
    if (note == 42 || note == 44 || note == 46) return DRUM_COST_HAT;
    if (note == 39) return DRUM_COST_CLAP;
    if (note >= 41 && note <= 50 && note != 49) return DRUM_COST_TOM;
    return DRUM_COST_CYMBAL;
}

static inline uint8_t route_default_core(uint8_t ch)
{
    /* デフォルトのフォールバック (PC未受信時用):
     * ch1(Bass), ch2(Chord), ch3, ch5 を Sub3(Bass/Strings) へ。
     * ※ PC受信時は音色ベース動的ルーティングにより自動判定される */
    return (ch == 1 || ch == 2 || ch == 3 || ch == 5)
               ? (uint8_t)ASMP_CORE_SUB3_BASS
               : (uint8_t)ASMP_CORE_SUB2_LEAD;
}

static inline bool route_is_flex(uint8_t ch)
{
    return (ch == 2 || ch == 3 || ch == 5 || ch == 8);
}

static void route_init(void)
{
    for (int ch = 0; ch < 16; ch++) {
        s_route[ch].core = route_default_core((uint8_t)ch);
        s_route[ch].active_notes = 0;
        s_route[ch].program_set = 0;
        s_route[ch].program = 0;
        s_route[ch].pedal_down = 0;
        s_route[ch].bank_msb = 0; s_route[ch].bank_set = 0;
        s_route[ch].bank_lsb = 0; s_route[ch].has_bank_lsb = 0;
        s_route[ch].pitch_bend = 0;
        s_route[ch].volume = 100; s_route[ch].has_volume = 0;
        s_route[ch].pan = 64; s_route[ch].has_pan = 0;
        s_route[ch].expression = 127; s_route[ch].has_expression = 0;
        s_route[ch].modulation = 0; s_route[ch].has_modulation = 0;
        s_route[ch].reverb_send = 40; s_route[ch].has_reverb = 0;
    }
    s_route_cooldown = 0;
    memset(s_note_core_map, 0, sizeof(s_note_core_map));
    s_live_vc2 = 0;
    s_live_vc3 = 0;
    sub_spawn_prod_reset(&s_spawn_prod2);
    sub_spawn_prod_reset(&s_spawn_prod3);
}

/**
 * @brief 全ノート消音時に未解放カウンタをリセット (曲替え等)
 *        ペダル状態も同時に落とす。旧実装は active_notes のみで pedal_down を
 *        残していたため、サステイン踏み中のスキップ後に Sub 側 (ペダル解除済み) と
 *        Sub1 側 (踏み中と誤認) が乖離し、当該 ch が以後の移動候補から永久除外
 *        されていた (負荷分散の片効き→dense部で片コア過負荷→音色劣化の連鎖)。
 */
static void route_reset_active_notes(void)
{
    for (int ch = 0; ch < 16; ch++) {
        s_route[ch].active_notes = 0;
        s_route[ch].pedal_down = 0;
    }
}

/**
 * @brief 実測負荷に基づくチャンネル再割当て (エポック毎に呼ぶ、内部で間引き)
 *
 * 発動条件: 重い側と軽い側の差が ROUTE_IMBALANCE_US 超、かつ重い側が
 * 十分に負荷高いこと。移動は 1 回に 1 チャンネル、既定経路への回帰を優先。
 */
static void route_rebalance(AsmpSharedContext *shared)
{
    if (s_route_cooldown != 0u) {
        s_route_cooldown--;
        return;
    }

    const uint32_t b2 = shared->core[ASMP_CORE_SUB2_LEAD].render_busy_us;
    const uint32_t b3 = shared->core[ASMP_CORE_SUB3_BASS].render_busy_us;
    if (b2 == 0u && b3 == 0u) {
        return; /* 計測未開始 */
    }

    int dir = 0; /* +1: S2→S3 / -1: S3→S2 */
    if (b2 > b3 && (b2 - b3) > ROUTE_IMBALANCE_US && b2 > ROUTE_MIN_BUSY_US) {
        dir = +1;
    } else if (b3 > b2 && (b3 - b2) > ROUTE_IMBALANCE_US && b3 > ROUTE_MIN_BUSY_US) {
        dir = -1;
    }
    if (dir == 0) return;

    const uint8_t src = (dir > 0) ? (uint8_t)ASMP_CORE_SUB2_LEAD
                                  : (uint8_t)ASMP_CORE_SUB3_BASS;
    const uint8_t dst = (dir > 0) ? (uint8_t)ASMP_CORE_SUB3_BASS
                                  : (uint8_t)ASMP_CORE_SUB2_LEAD;

    /* 移動候補: フレックスプール & src 割当中 & アクティブノート無し &
     * サステイン非踏み。ペダル保持中は Note Off 済みでも音が鳴り続けるため、
     * 移動すると CC64 離しが旧コアに取り残され永続スタックノートになる */
    int pick = -1;
    int fallback = -1;
    for (int ch = 0; ch < 16; ch++) {
        const uint8_t uch = (uint8_t)ch;
        if (!route_is_flex(uch)) continue;
        ChRoute *r = &s_route[uch];
        if (r->core != src || r->active_notes != 0u || r->pedal_down != 0u) continue;
        if (route_default_core(uch) == dst) {
            pick = ch;
            break;
        }
        if (fallback < 0) fallback = ch;
    }
    if (pick < 0) pick = fallback;
    if (pick < 0) {
        /* 移動候補なしでもクールダウンを置き、頻繁な再評価を避ける */
        s_route_cooldown = ROUTE_MOVE_COOLDOWN_EPOCHS;
        return;
    }

    ChRoute *r = &s_route[pick];
    r->core = dst;

    /* 宛先エンジンへチャンネル状態の完全スナップショットを複製。
     * Pitch Bend/Mod/Vol/Pan等を再現しないと移動後に音程・定位が飛ぶ。
     * 投入に失敗した場合は移動を取り消す (状態不明のまま放置しない) */
    bool ok = route_restore_to(shared, dst, (uint8_t)pick, true);
    if (!ok) {
        r->core = src;
        s_route_cooldown = ROUTE_MOVE_COOLDOWN_EPOCHS;
        return;
    }

    shared->route_moves++;
    shared->route_last_ch = (uint32_t)pick;
    shared->route_last_core = dst;
    s_route_cooldown = ROUTE_MOVE_COOLDOWN_EPOCHS;
}

/**
 * @brief C4 (ドラム) 負荷・状態監視 (Governor 認識フェーズ、エポック毎に呼ぶ)
 *
 * render_busy_us[SUB4] (EMA 平滑済み実測値) を「このエポックの予算」
 * (ef フレーム分 = 512fr で 10666us) と比較し、C4 を
 * idle / active / overloaded に分類して shared->c4_state へ公開する。
 * C4 が overloaded の場合は C2/C3 の余裕 (render_busy_us と予算の差) から
 * migration candidate を決め shared->c4_candidate へ公開する。
 * CPU 使用率ではなく「予算超過/予算余裕」を優先する (音声デッドライン保護)。
 * 現段階では DSP 移行は行わない (認識・状態管理のみ)。
 */
static void route_c4_monitor(AsmpSharedContext *shared, uint32_t ef)
{
    const uint32_t b4 = shared->core[ASMP_CORE_SUB4_DRUM].render_busy_us;
    const uint32_t vc4 = (uint32_t)shared->core[ASMP_CORE_SUB4_DRUM].voice_count;

    /* エポック予算 (µs) = フレーム数 / サンプルレート。512fr -> 10666us */
    uint32_t budget_us = (ef * 1000000u) / SUB_SAMPLE_RATE;
    if (budget_us == 0u) budget_us = 1u;

    /* 過負荷判定のヒステリシス: 進入は予算超、復帰は予算の 1/4 未満まで待つ。
     * Snare/Clap/Tom の残存 (~4-5ms) では復帰させず、metal を移行先に留める。
     * さらに移行後は C4_MIGRATE_HOLD_EPOCHS の間、render_busy_us が瞬間的に
     * 下がっても復帰を抑止し、密集部の合間で発生するリミットサイクルを断つ */
    const bool was_overloaded = s_c4_overloaded;
    if (!s_c4_overloaded && b4 > budget_us) {
        s_c4_overloaded = true;
        s_c4_migrate_hold = C4_MIGRATE_HOLD_EPOCHS;
    } else if (s_c4_overloaded) {
        if (s_c4_migrate_hold > 0u) {
            s_c4_migrate_hold--;
        } else if (b4 < (budget_us / 4u)) {
            s_c4_overloaded = false;
        }
    }

    uint32_t state;
    if (s_c4_overloaded) {
        state = C4_STATE_OVERLOADED;   /* 予算超過: デッドライン破綻リスク */
    } else if (vc4 == 0u) {
        state = C4_STATE_IDLE;         /* 発音なし */
    } else {
        state = C4_STATE_ACTIVE;       /* 発音中かつ予算内 */
    }
    shared->c4_state = state;

    /* C4 過負荷時のみ migration candidate を判定する。
     * 候補先は「予算に対して safety margin 以上の余裕がある」Core に限る
     * (ぎりぎりの Core へ逃がすと移行先ごとデッドライン破綻するため)。
     * 候補は「過負荷に突入した瞬間」だけ決定し、以後復帰するまで保持する。
     * これにより C2⇔C3 の頻繁な反転が根絶され、metal の位相連続性も保たれる。
     * 既存 C2⇔C3 routing は変更しない */
    const uint32_t b2 = shared->core[ASMP_CORE_SUB2_LEAD].render_busy_us;
    const uint32_t b3 = shared->core[ASMP_CORE_SUB3_BASS].render_busy_us;

    uint32_t candidate = s_c4_candidate; /* 既定: 現在の候補を保持 */
    if (!s_c4_overloaded) {
        candidate = C4_CAND_NONE;
    } else if (!was_overloaded) {
        /* 過負荷突入時のみ再決定 */
        candidate = C4_CAND_NONE;
        const uint32_t head2 = (b2 < budget_us) ? (budget_us - b2) : 0u;
        const uint32_t head3 = (b3 < budget_us) ? (budget_us - b3) : 0u;
        const uint32_t post2 = (head2 > C4_CAND_TRANSFER_RESERVE_US)
                             ? (head2 - C4_CAND_TRANSFER_RESERVE_US) : 0u;
        const uint32_t post3 = (head3 > C4_CAND_TRANSFER_RESERVE_US)
                             ? (head3 - C4_CAND_TRANSFER_RESERVE_US) : 0u;
        /* 直前 busy だけでなく、移行後の予約ヘッドルームも見る。
         * 金属系ドラムの最悪ケース (12-14ms 級) を見積もらないと、
         * C2/C3 が 3ms 余裕に見えても直後にデッドラインを破る。 */
        const bool avail2 = (post2 >= C4_CAND_SAFETY_MARGIN_US);
        const bool avail3 = (post3 >= C4_CAND_SAFETY_MARGIN_US);
        if (avail2 && avail3) {
            candidate = (post2 >= post3) ? (uint32_t)ASMP_CORE_SUB2_LEAD
                                         : (uint32_t)ASMP_CORE_SUB3_BASS;
        } else if (avail2) {
            candidate = (uint32_t)ASMP_CORE_SUB2_LEAD;
        } else if (avail3) {
            candidate = (uint32_t)ASMP_CORE_SUB3_BASS;
        }
    }
    shared->c4_candidate = candidate;
    const bool cand_changed = (candidate != s_c4_candidate);
    s_c4_candidate = candidate;

    /* 状態遷移はカウンタのみ (Commit1: 同期printf撤廃) */
    if (state != s_c4_state) {
        if (state == C4_STATE_ACTIVE) shared->diag_c4_active_edges++;
        else if (state == C4_STATE_IDLE) shared->diag_c4_idle_edges++;
        else if (state == C4_STATE_OVERLOADED) shared->diag_c4_overload_edges++;
        s_c4_state = state;
    }

    /* 候補遷移もカウンタのみ */
    if (cand_changed || (s_c4_overloaded && !was_overloaded)) {
        shared->diag_c4_candidate_changes++;
    }

    /* ゴールデン復帰: DRUM_ECOは無効 (Main GOVのみで制御していたゴールデンへ) */
}

/* 1節: 優先度分離 — MIDIは4ms上限でドロップ、RENDER_REQは200ms上限で局所化 */
/* 犯人G: NOTE_OFF/CC64 offは濁り残響の原因のため50msで粘る */
#define SUB1_MIDI_PUSH_MAX_WAIT_US    (500u)
#define SUB1_RELEASE_MAX_WAIT_US      (50000u)
#define SUB1_RENDER_REQ_MAX_WAIT_US   (200000u)

static bool sub1_push_bounded(AsmpSharedContext *shared, uint8_t core,
                               const AsmpPacket *pkt, uint32_t max_wait_us)
{
    uint32_t waited = 0;
    while (!asmp_queue_push(&shared->queues[core], pkt)) {
        if (shared->main_ctrl.shutdown_requested) return false;
        sub1_sleep_us(100);
        waited += 100u;
        if (waited >= max_wait_us) {
            shared->diag_queue_drop++;
            shared->queue_backpressure_hits[core]++;
            return false;
        }
    }
    return true;
}

static bool sub1_push_render_req(AsmpSharedContext *shared, uint8_t core, const AsmpPacket *pkt)
{
    uint32_t waited = 0;
    while (!asmp_queue_push(&shared->queues[core], pkt)) {
        if (shared->main_ctrl.shutdown_requested) return false;
        sub1_sleep_us(100);
        waited += 100u;
        if (waited >= SUB1_RENDER_REQ_MAX_WAIT_US) {
            shared->diag_timeout++;
            shared->queue_backpressure_hits[core]++;
            return false;
        }
    }
    return true;
}

/* 犯人G: NOTE_OFF/CC64 off専用 — ドロップ厳禁のためNOTE_ON(4ms)より大幅に長く粘る */
static inline bool sub1_push_release(AsmpSharedContext *shared, uint8_t core, const AsmpPacket *pkt)
{
    return sub1_push_bounded(shared, core, pkt, SUB1_RELEASE_MAX_WAIT_US);
}

/**
 * @brief チャンネル状態を移動先エンジンへ復元する (migration 共通ヘルパー)
 * @details rebalance 移動と PC 理想配置移動の両方から使う。連続系 CC
 *          (Vol/Pan/Expr/Mod/Reverb) は has_* に関わらず実効値 (未受信なら
 *          route_init の GM 既定値) を必ず送り、移動先に残る旧曲・旧ch の
 *          古い値をブルドーズする。旧実装は has_* 付きのみ送っていたため、
 *          CC未受信ch の移動後に定位・音量・ビブラートが化けていた
 *          (dense部の負荷移動で発症)。Bank は MSB→LSB の順で送る。
 *          PC 自体は include_pc=true でのみ送る (PC 理想移動では契機パケット
 *          自体が後続の通常配送で届くため false)。Bend は非ゼロ時のみ。
 *          1件でも積めなければ false (呼び出し側は移動を取り消すこと)。
 */
static bool route_restore_to(AsmpSharedContext *shared, uint8_t dst, uint8_t ch, bool include_pc)
{
    if (ch >= 16) return false;
    ChRoute *r = &s_route[ch];
    bool ok = true;
    AsmpPacket pkt;
    /* Bank は未設定でも 0 で上書きして移動先の残留状態を消去 */
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = ASMP_MSG_CONTROL_CHANGE; pkt.channel = ch; pkt.data1 = 0; pkt.data2 = r->bank_set ? r->bank_msb : 0;
    ok = ok && sub1_push_bounded(shared, dst, &pkt, SUB1_MIDI_PUSH_MAX_WAIT_US);

    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = ASMP_MSG_CONTROL_CHANGE; pkt.channel = ch; pkt.data1 = 32; pkt.data2 = (r->bank_set && r->has_bank_lsb) ? r->bank_lsb : 0;
    ok = ok && sub1_push_bounded(shared, dst, &pkt, SUB1_MIDI_PUSH_MAX_WAIT_US);

    if (include_pc) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.msg_type = ASMP_MSG_PROGRAM_CHANGE; pkt.channel = ch;
        pkt.data1 = r->program_set ? r->program : 0;
        ok = ok && sub1_push_bounded(shared, dst, &pkt, SUB1_MIDI_PUSH_MAX_WAIT_US);
    }
    {
        static const uint8_t cc_num[5] = { 7, 10, 11, 1, 91 };
        uint8_t eff[5];
        eff[0] = r->has_volume ? r->volume : 100;
        eff[1] = r->has_pan ? r->pan : 64;
        eff[2] = r->has_expression ? r->expression : 127;
        eff[3] = r->has_modulation ? r->modulation : 0;
        eff[4] = r->has_reverb ? r->reverb_send : 40;
        for (int i = 0; i < 5 && ok; i++) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.msg_type = ASMP_MSG_CONTROL_CHANGE; pkt.channel = ch;
            pkt.data1 = cc_num[i]; pkt.data2 = eff[i];
            ok = ok && sub1_push_bounded(shared, dst, &pkt, SUB1_MIDI_PUSH_MAX_WAIT_US);
        }
    }
    /* Pitch Bend も 0 であっても必ず送信して残留ピッチを確実にリセット */
    if (ok) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.msg_type = ASMP_MSG_PITCH_BEND; pkt.channel = ch; pkt.param = (uint32_t)(int32_t)r->pitch_bend;
        ok = ok && sub1_push_bounded(shared, dst, &pkt, SUB1_MIDI_PUSH_MAX_WAIT_US);
    }
    return ok;
}

/**
 * @brief ノートイベントを適切な音源サブコアへルーティング
 *        (動的ルーティング: 割当は s_route[].core に従う)
 */
static void route_midi_packet(AsmpSharedContext *shared, const AsmpPacket *pkt)
{
    /* CC91 はチャンネル別センドとして音源コアで処理する。
     * 旧実装は Sub5 のグローバル wet へ直結し、最後のCC91チャンネルが全体残響を上書きしていたため撤廃。
     * Sub5 への中継は行わず、通常のチャンネルルーティングで各コアへ配送する。 */

    /* MIDI 規約: NOTE_ON velocity == 0 は NOTE_OFF 相当に正規化 */
    AsmpPacket routed = *pkt;
    if (routed.msg_type == ASMP_MSG_NOTE_ON && routed.data2 == 0) {
        routed.msg_type = ASMP_MSG_NOTE_OFF;
    }

    uint8_t target_core;
    bool is_drum_ch = false;
    ChRoute *r = NULL;

    if (routed.channel == 9) {
        /* MIDI Ch 10 (0-indexed 9): GM ドラム。
         * Phase 6 = 全ドラム分散: 全 NOTE_ON (Kick/Metal/Snare/Clap/Tom) を
         * candidate (C2/C3) へ動的移行する。candidate=NONE の時は従来通り C4
         * ゴールデン復帰: 負荷分散を再有効化し C4単独過負荷による音飛びを解消 */
        is_drum_ch = true;

        /* CC#7/CC91 (ch10 ドラム音量/リバーブ) は C4 と C2/C3 へ配送して移行ドラム音量を同期。
         * C2/C3 側は ch9 の CC#7/CC91 をドラム専用として扱うため melodic に影響しない */
        if (routed.msg_type == ASMP_MSG_CONTROL_CHANGE && (routed.data1 == 7u || routed.data1 == 91u)) {
            AsmpPacket cc = routed;
            /* 犯人A' 1: 無制限blockingを4ms boundedへ。CCはNOTE_ON同様、詰まればドロップでよい */
            (void)sub1_push_bounded(shared, ASMP_CORE_SUB4_DRUM, &cc, SUB1_MIDI_PUSH_MAX_WAIT_US);
            (void)sub1_push_bounded(shared, ASMP_CORE_SUB2_LEAD, &cc, SUB1_MIDI_PUSH_MAX_WAIT_US);
            (void)sub1_push_bounded(shared, ASMP_CORE_SUB3_BASS, &cc, SUB1_MIDI_PUSH_MAX_WAIT_US);
            return;
        }

        /* ドラム専用コア (Core4) 専任ルーティング:
         * 実機テレメトリによりドラム全量でも Core4 負荷は 1ms 未満 (956us) と証明。
         * メロディ/ベースコアへ一切ドラムを侵入させず、Core4 の高品質音源に集中させる */
        target_core = ASMP_CORE_SUB4_DRUM;
    } else {
        r = &s_route[routed.channel & 0x0Fu];
        target_core = r->core;

        /* メモリ活用動的バランシング (Core1即時カウンタによる完全50:50分散):
         * NOTE_OFF は発音時に記録したコアへ確実に届ける */
        if (routed.msg_type == ASMP_MSG_NOTE_OFF) {
            uint8_t ch = routed.channel & 0x0Fu;
            uint8_t note = routed.data1 & 0x7Fu;
            if (s_note_core_map[ch][note] != 0) {
                target_core = s_note_core_map[ch][note];
                s_note_core_map[ch][note] = 0;
                if (target_core == ASMP_CORE_SUB2_LEAD && s_live_vc2 > 0) s_live_vc2--;
                else if (target_core == ASMP_CORE_SUB3_BASS && s_live_vc3 > 0) s_live_vc3--;
            }
        } else if (routed.msg_type == ASMP_MSG_NOTE_ON && routed.data2 > 0) {
            uint8_t ch = routed.channel & 0x0Fu;
            uint8_t note = routed.data1 & 0x7Fu;
            s_note_core_map[ch][note] = target_core;
        }

        /* チャンネル状態スナップショット (migration復元用) */
        if (routed.msg_type == ASMP_MSG_PROGRAM_CHANGE) {
            r->program_set = 1;
            r->program = routed.data1;

            /* 音色ベース動的ルーティング (#音抜け対策):
             * GM音色分類に基づき、Bass系 (32〜39) と Ensemble/Strings系 (48〜55) は Sub3 へ、
             * Lead/Brass/Piano/Synth等のメロディ系は Sub2 へ動的に再割り当てする。
             * 発音前またはノート停止中に切り替えることで、ノートオフの迷子を防止。 */
            uint8_t prog = routed.data1;
            /* 負荷バランシング黄金律 (伴奏とリードの完全分担):
             * 伴奏・和音・低域: Piano(0〜7), Chromatic(8〜15), Organ(16〜23), Guitar(24〜31),
             *                   Bass(32〜39), Strings/Ensemble(40〜55), Pad(88〜95) を Sub3 へ。
             * 主旋律・リード: Brass(56〜63), Reed(64〜71), Pipe(72〜79), Synth Lead(80〜87) を Sub2 へ。
             * これにより和音密集伴奏がすべて高速FMAのSub3に流れ、Sub2は主旋律(2〜4音)に専念できる！ */
            bool is_sub3_role = (prog < 56) || (prog >= 88 && prog < 96);
            uint8_t ideal_core = is_sub3_role
                                     ? (uint8_t)ASMP_CORE_SUB3_BASS
                                     : (uint8_t)ASMP_CORE_SUB2_LEAD;
            /* pedal_down も条件に含める (rebalance 側と同一条件)。
             * ペダル保持中は Note Off 済みでも旧コアで鳴り続けており、ここで
             * 移動すると後続 CC64-off が新コアへ流れて旧コア保持音が stuck する
             * (dense部のペダル多用で発症)。踏み中は旧配置に留め、解放後の
             * 次回 PC で移動する (音は正しく、負荷は次善) */
            if (r->active_notes == 0 && r->pedal_down == 0 && r->core != ideal_core) {
                /* 理想配置への移動時は Bank/CC/ベンドの実効状態を先に複製する。
                 * 旧実装は PC パケット自体だけを新コアへ流し、直前の Bank MSB
                 * (同ブロック先行は旧コアへ配送済み) や旧曲由来の Vol/Pan 等が
                 * 新コア側で化けたまま先頭発音されていた (出だしの音色変)。
                 * 復元失敗時は移動を取り消し、PC は旧コアへ届ける (安全側)。 */
                uint8_t prev_core = r->core;
                r->core = ideal_core;
                if (!route_restore_to(shared, ideal_core, routed.channel & 0x0Fu, false)) {
                    r->core = prev_core;
                    target_core = prev_core;
                } else {
                    target_core = ideal_core;
                }
            }
        } else if (routed.msg_type == ASMP_MSG_CONTROL_CHANGE) {
            switch (routed.data1) {
                case 0:  r->bank_msb = routed.data2; r->bank_set = 1; break;
                case 32: r->bank_lsb = routed.data2; r->has_bank_lsb = 1; r->bank_set = 1; break;
                case 1:  r->modulation = routed.data2; r->has_modulation = 1; break;
                case 7:  r->volume = routed.data2; r->has_volume = 1; break;
                case 10: r->pan = routed.data2; r->has_pan = 1; break;
                case 11: r->expression = routed.data2; r->has_expression = 1; break;
                case 64: r->pedal_down = (routed.data2 >= 64) ? 1u : 0u; break;
                case 91: r->reverb_send = routed.data2; r->has_reverb = 1; break;
                default: break;
            }
        } else if (routed.msg_type == ASMP_MSG_PITCH_BEND) {
            r->pitch_bend = (int16_t)routed.param;
        }
    }

    if (routed.msg_type == ASMP_MSG_NOTE_ON && routed.data2 > 0) {
        /* SMF再生はヒューマナイズ無効、ライブ入力のみ適用 */
        if (routed.source == MIDI_SOURCE_LIVE) {
            s_hum_seed ^= s_hum_seed << 13; s_hum_seed ^= s_hum_seed >> 17; s_hum_seed ^= s_hum_seed << 5;
            int32_t hum = (int32_t)routed.data2 + (int32_t)(s_hum_seed % 7u) - 3;
            if (hum < 1) hum = 1;
            if (hum > 127) hum = 127;
            routed.data2 = (uint8_t)hum;
        }
    }

    /* Core1 分解送信 (ABI v13): メロディ NOTE_ON は発音パラメータを事前解決し
     * ディスクリプタで配送する。演奏コアは MIDI 解釈なしで発音できる。
     * program は Core1 の正準状態 (未受信時は宛先コアの既定値 Sub2=0/Sub3=33)
     * を使い、キュー順序 (FIFO) により演奏コア側と一致する。
     * プール満杯時は param=0 のまま従来 NOTE_ON (演奏コアが正準解釈) になる */
    if (routed.msg_type == ASMP_MSG_NOTE_ON && routed.data2 > 0 && !is_drum_ch &&
        (target_core == ASMP_CORE_SUB2_LEAD || target_core == ASMP_CORE_SUB3_BASS) &&
        r != NULL) {
        /* 入場整理: 宛先が飽和なら弱音を Core1 で音楽的に間引く。
         * キュー満杯 (4ms) のランダム落としは「遅れて届いた重要音」を殺し、
         * しかも Sub1 自体を停滞させて全コアの RENDER_REQ 配送まで遅らせる
         * 連鎖 (4ms×連打 = 数エポックのジッタ) になる。早期の弱音シェッドは
         * キューを短く保ち、強音の到達と Sub1 の定時進行を保証する。
         * vc=14-15 では落とさず thin 化 (unison off) のみで負荷を下げる */
        uint32_t vc = shared->core[target_core].voice_count;
        uint32_t busy = shared->core[target_core].render_busy_us;
        bool hot = (busy > s_current_budget_us);
        bool hard_full = (vc >= 16u);
        bool saturated = hard_full || hot;
        /* 黄金律復元: 通常演奏ノートのドロップを完全撤廃。
         * 全ノートを演奏コアへ届け、演奏コアの最古・最弱音ボイススチールに任せる。
         * 飽和時は thin (unison解除) のみで負荷を下げ、音符の欠落を防ぐ */
        bool thin = saturated || (vc >= 14u);
        uint8_t prog_eff = r->program_set ? r->program
            : ((target_core == ASMP_CORE_SUB2_LEAD) ? 0u : 33u);
        SubSpawnDesc desc;
        SubSpawnProd *prod;
        SubSpawnSlot *pool;
        volatile uint32_t *ack;
        if (target_core == ASMP_CORE_SUB2_LEAD) {
            bool gov_off = (shared->main_ctrl.quality_flags & ASMP_QF_UNISON_OFF) != 0;
            sub_spawn_build_sub2(&desc, routed.channel & 0x0Fu, routed.data1,
                                 routed.data2, prog_eff, gov_off, thin);
            prod = &s_spawn_prod2;
            pool = shared->spawn_pool_sub2;
            ack = &shared->spawn_ack_sub2.consumed;
        } else {
            sub_spawn_build_sub3(&desc, routed.channel & 0x0Fu, routed.data1,
                                 routed.data2, prog_eff);
            prod = &s_spawn_prod3;
            pool = shared->spawn_pool_sub3;
            ack = &shared->spawn_ack_sub3.consumed;
        }
        uint32_t token = 0u;
        if (sub_spawn_produce(pool, ack, prod, &desc, &token)) {
            routed.param = token;
        }
    }

    /* 犯人G: NOTE_OFF/ALL_NOTES_OFF/CC64 off/CC120/CC123 はドロップ厳禁 — 50msで粘る。落としていいのはNOTE_ONだけ(4ms) */
    bool is_release = (routed.msg_type == ASMP_MSG_NOTE_OFF) ||
                      (routed.msg_type == ASMP_MSG_ALL_NOTES_OFF) ||
                      (routed.msg_type == ASMP_MSG_CONTROL_CHANGE && routed.data1 == 64 && routed.data2 < 64) ||
                      (routed.msg_type == ASMP_MSG_CONTROL_CHANGE && (routed.data1 == 120 || routed.data1 == 123));

    bool delivered;
    if (routed.msg_type == ASMP_MSG_ALL_NOTES_OFF) {
        /* 動的チャンネル移動時の取り残し音を完全に防ぐため、メロディ(Sub2)とベース(Sub3)の双方へ消音パケットを届ける */
        bool d2 = sub1_push_release(shared, ASMP_CORE_SUB2_LEAD, &routed);
        bool d3 = sub1_push_release(shared, ASMP_CORE_SUB3_BASS, &routed);
        if (is_drum_ch || routed.channel >= 16) {
            (void)sub1_push_release(shared, ASMP_CORE_SUB4_DRUM, &routed);
        }
        delivered = d2 || d3;
    } else {
        delivered = is_release
            ? sub1_push_release(shared, target_core, &routed)
            : sub1_push_bounded(shared, target_core, &routed, SUB1_MIDI_PUSH_MAX_WAIT_US);
    }

    /* 未解放カウンタは「配信が成功した場合のみ」更新する。
     * ドロップされた NOTE_OFF を減算に使うと実態 (音が鳴り続けている) と
     * カウンタが乖離し、移動判定や後続の Note Off 追跡を誤らせる */
    if (!is_drum_ch && delivered && r != NULL) {
        if (routed.msg_type == ASMP_MSG_NOTE_ON) {
            if (r->active_notes < 255u) r->active_notes++;
        } else if (routed.msg_type == ASMP_MSG_NOTE_OFF) {
            if (r->active_notes > 0u) r->active_notes--;
        } else if (routed.msg_type == ASMP_MSG_ALL_NOTES_OFF) {
            r->active_notes = 0;
        }
    }
}

/**
 * @brief 全音源サブコアへブロードキャスト送信 (消音パケットは到達保証) - B10: Sub5 FXもリセット
 */
static void broadcast_packet(AsmpSharedContext *shared, const AsmpPacket *pkt)
{
    /* 犯人A' 2: ALL_NOTES_OFFはドロップ厳禁だが無制限待ちは単一障害点。再試行は50msで打切り */
    (void)sub1_push_release(shared, ASMP_CORE_SUB2_LEAD, pkt);
    (void)sub1_push_release(shared, ASMP_CORE_SUB3_BASS, pkt);
    (void)sub1_push_release(shared, ASMP_CORE_SUB4_DRUM, pkt);
    (void)sub1_push_release(shared, ASMP_CORE_SUB5_DSP, pkt);
}

/**
 * @brief SubCore 1 エントリーポイント
 */
void *subcore1_entry(void *arg)
{
    AsmpSharedContext *shared = (AsmpSharedContext *)arg;
    if (!shared) return NULL;
    if (!asmp_abi_ok(shared)) {
        printf("[SUB1][FATAL] ABI mismatch magic=%08x ver=%u size=%u expected %u\n",
               (unsigned)shared->abi_magic, (unsigned)shared->abi_version,
               (unsigned)shared->abi_size, (unsigned)sizeof(AsmpSharedContext));
        return NULL;
    }
    sub_fpu_denormal_init(); /* デノーマル例外ペナルティによるじりじりノイズ防止 */

    route_init();

    /* ビルド識別 (全ワーカーが新リビジョンで起動したかの確認用) */
    printf("[SUB1][BUILD] %s | MIDI event router ready\n", HEXASENSE_DSP_TAG);

    uint32_t last_epoch = 0;
    uint32_t s_idle_loops = 0;

    /* CPU 負荷メーター (実測ビジー時間比率) */
    SubLoadMeter load_m; SUB_LOAD_INIT(load_m);

    while (!shared->main_ctrl.shutdown_requested) {
        asmp_dcache_invalidate((const void *)&shared->main_ctrl.shutdown_requested, sizeof(shared->main_ctrl.shutdown_requested));
        asmp_dcache_invalidate((const void *)&shared->main_ctrl.sub5_force_clear_req, sizeof(shared->main_ctrl.sub5_force_clear_req));
        asmp_dcache_invalidate((const void *)&shared->render_ctrl.render_epoch, sizeof(shared->render_ctrl.render_epoch));
        ASMP_BARRIER();
        /* 1. Main Core または 他コアからの受信メッセージを処理 */
        AsmpPacket cmd;
        while (asmp_queue_pop(&shared->queues[ASMP_CORE_SUB1_SEQ], &cmd)) {
            switch (cmd.msg_type) {
                case ASMP_MSG_CMD_VOLUME:
                    /* Main Core 由来の音量指示を SubCore 5 (マスターDSP) へ中継
                     * (queues[SUB5] の単一プロデューサは Sub1 に限定する) */
                    if (!asmp_queue_push(&shared->queues[ASMP_CORE_SUB5_DSP], &cmd)) {
                        shared->diag_queue_drop++;
                    }
                    break;

                case ASMP_MSG_ALL_NOTES_OFF:
                    /* Main Core (SD レーン切替/モード切替等) 由来の全消音要求を配信 */
                    {
                        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF };
                        broadcast_packet(shared, &off);
                        route_reset_active_notes();
                    }
                    break;

                case ASMP_MSG_CMD_STOP:
                    /* 互換用: 自前再生の廃止に伴い全消音として処理する */
                    {
                        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF };
                        broadcast_packet(shared, &off);
                        route_reset_active_notes();
                    }
                    break;

                case ASMP_MSG_NOTE_ON:
                case ASMP_MSG_NOTE_OFF:
                case ASMP_MSG_CONTROL_CHANGE:
                case ASMP_MSG_PITCH_BEND:
                case ASMP_MSG_PROGRAM_CHANGE: /* SD レーン由来の音色変更を転送する */
                    route_midi_packet(shared, &cmd);
                    break;

                default:
                    break;
            }
        }

        /* 優先4: SUB5からの強制クリア要求を処理 (10連続タイムアウト時)
         * 犯人A' 3: SUB4が詰まっている確率が最も高い瞬間に無制限blockingは再発のためboundedへ */
        if (shared->main_ctrl.sub5_force_clear_req) {
            shared->main_ctrl.sub5_force_clear_req = 0;
            ASMP_BARRIER();
            AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF };
            (void)sub1_push_bounded(shared, ASMP_CORE_SUB4_DRUM, &off, SUB1_MIDI_PUSH_MAX_WAIT_US);
            printf("[SUB1][FORCE CLEAR] S4 ALL_NOTES_OFF from SUB5 req\n");
        }

        /* P0-C Non-Droppable Control Plane: キューを介さない消音要求の処理 */
        asmp_dcache_invalidate((const void *)&shared->ch_control, sizeof(shared->ch_control));
        ASMP_BARRIER();
        for (int ch = 0; ch < 16; ch++) {
            if (shared->ch_control.all_notes_off_gen[ch] != s_last_all_notes_off_gen[ch]) {
                s_last_all_notes_off_gen[ch] = shared->ch_control.all_notes_off_gen[ch];
                AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = (uint8_t)ch };
                route_midi_packet(shared, &off);
                s_route[ch].active_notes = 0;
            }
            if (shared->ch_control.sustain_off_gen[ch] != s_last_sustain_off_gen[ch]) {
                s_last_sustain_off_gen[ch] = shared->ch_control.sustain_off_gen[ch];
                AsmpPacket cc = {
                    .msg_type = ASMP_MSG_CONTROL_CHANGE,
                    .channel = (uint8_t)ch,
                    .data1 = 64,
                    .data2 = 0
                };
                route_midi_packet(shared, &cc);
            }
            if (shared->ch_control.all_sound_off_gen[ch] != s_last_all_sound_off_gen[ch]) {
                s_last_all_sound_off_gen[ch] = shared->ch_control.all_sound_off_gen[ch];
                AsmpPacket cc = {
                    .msg_type = ASMP_MSG_CONTROL_CHANGE,
                    .channel = (uint8_t)ch,
                    .data1 = 120,
                    .data2 = 0
                };
                route_midi_packet(shared, &cc);
                s_route[ch].active_notes = 0;
            }
        }

        /* 2. 新しいレンダリングエポックの同期待機 (P0-C Render Mailbox & render_ctrl) */
        uint32_t mbox_ep = 0;
        bool has_new_epoch = asmp_render_mbox_poll(shared, &mbox_ep) || (shared->render_ctrl.render_epoch != last_epoch);
        if (!has_new_epoch) {
            /* 待機中も低レートでハートビートを申告する。
             * 「仕事が無い」ことと「死んでいる」ことを死活監視が混同しないため */
            if ((++s_idle_loops & 0x3Fu) == 0u) {
                shared->core[ASMP_CORE_SUB1_SEQ].heartbeat++;
            }
            sub1_sleep_us(100);
            SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB1_SEQ);
            continue;
        }
        /* Acquire バリア: render_epoch 変化検知以降の共有メモリ
         * (epoch_frames 等) の読み出し順序を保証し投機的先読みを防ぐ */
        ASMP_BARRIER();
        last_epoch = shared->render_ctrl.render_epoch;
        uint32_t slot_tmp = ASMP_EPOCH_SLOT(last_epoch);
        asmp_dcache_invalidate((const void *)&shared->render_ctrl.slot_epoch[slot_tmp], sizeof(shared->render_ctrl.slot_epoch[slot_tmp]));
        asmp_dcache_invalidate((const void *)&shared->render_ctrl.epoch_frames[slot_tmp], sizeof(shared->render_ctrl.epoch_frames[slot_tmp]));
        ASMP_BARRIER();
        if (!asmp_abi_ok(shared) || shared->render_ctrl.slot_epoch[slot_tmp] != last_epoch) {
            shared->diag_slot_mismatch++;
            /* 次エポックでリカバリ - 今回はスキップせず進行 (S3) */
        }
#ifdef PROFILE_ENABLE
    profile_epoch_start(1, last_epoch);
#endif
        SUB_LOAD_BUSY_BEGIN(load_m);
        uint64_t t0_ns = sub_get_ns();

        /* S3: per-slot化 */
        uint32_t ef = shared->render_ctrl.epoch_frames[slot_tmp];
        if (ef == 0u || ef > ASMP_BUFFER_FRAMES) ef = ASMP_BUFFER_FRAMES;
        /* Commit3: 予測分散用 budget更新と予約クリア (毎エポック) */
        s_current_budget_us = (ef * 1000000u) / SUB_SAMPLE_RATE;
        if (s_current_budget_us == 0u) s_current_budget_us = 1u;
        memset(s_reserved_us, 0, sizeof(s_reserved_us));

        /* C4 (ドラム) 監視: render_busy_us[SUB4] と予算を比較し idle/active/overloaded を分類。
         * C2/C3 ルーティングとは独立に、発音有無に依らず毎エポック実行する */
        route_c4_monitor(shared, ef);

        /* 動的ルーティング: 実測エポック時間の偏りに応じたチャンネル再割当て
         * (内部でクールダウン管理、移動はフレックスch・アイドル時のみ)。
         * 発音が無い idle 時は render_busy_us が凍結した古い値のままなので
         * 移動させない (未解放ノートカウンタの合計で判定) */
        {
            uint32_t active_total = 0;
            for (int ch = 0; ch < 16; ch++) {
                active_total += s_route[ch].active_notes;
            }
            if (active_total > 0u) {
                route_rebalance(shared);
            }
        }

        /* 16 分音符ステップクロック生成 (演奏モード アルペジオ同期用)
         * 累積誤差を丸めではなく剰余引き継ぎで吸収し、長時間でも位相が走らない */
        {
            uint32_t tus = shared->main_ctrl.tempo_us_per_quarter;
            if (tus == 0) tus = 500000u;
            uint32_t step_samples = (uint32_t)(((uint64_t)tus * (uint64_t)SUB_SAMPLE_RATE + 2000000ull) / 4000000ull);
            if (step_samples == 0) step_samples = 1;
            uint32_t acc = shared->_step_acc_samples + ef;
            while (acc >= step_samples) {
                shared->seq_step16++;
                acc -= step_samples;
            }
            shared->_step_acc_samples = acc;
        }

        /* ルーティングされる NOTE_ON にベロシティ ヒューマナイズ (±3) を適用。
         * 楽曲進行自体は Main Core (SD レーン) が駆動するため、Sub1 の
         * エポック処理はクロック生成と RENDER_REQ 配信のみでよい */

        /* 3. 各音源サブコア (2, 3, 4, 5) へレンダリング開始要求を発行
         * param = 対象エポック。各コアは完了後 done_epoch[自core] = epoch を公開。
         * SubCore 5 もキュー駆動にすることで全コアがエポックを逐次処理し、
         * Ping-Pong バッファにより 2-4 と 5 の処理がオーバーラップする */
        AsmpPacket render_req = {
            .msg_type = ASMP_MSG_RENDER_REQ,
            .param = last_epoch
        };
        /* 1節: RENDER_REQは200ms上限で局所化。1コア死でもSUB1は生き残る */
        sub1_push_render_req(shared, ASMP_CORE_SUB2_LEAD, &render_req);
        sub1_push_render_req(shared, ASMP_CORE_SUB3_BASS, &render_req);
        sub1_push_render_req(shared, ASMP_CORE_SUB4_DRUM, &render_req);
        sub1_push_render_req(shared, ASMP_CORE_SUB5_DSP, &render_req);

        /* 5節: キュー占有率テレメトリ公開 (SPSC読み取り専用) */
        for (int c = 1; c < ASMP_NUM_CORES; c++) {
            uint32_t h = shared->queues[c].head;
            uint32_t t = shared->queues[c].tail;
            uint32_t depth = (h - t) & (ASMP_QUEUE_CAPACITY - 1);
            shared->queue_depth[c] = (uint16_t)depth;
        }

        /* P0-C: Render Mailbox 完了 ACK */
        asmp_render_mbox_ack(shared, last_epoch);

        /* 4. ハートビート更新 */
#ifdef PROFILE_ENABLE
    profile_epoch_end(1, last_epoch);
#endif
        shared->core[ASMP_CORE_SUB1_SEQ].heartbeat++;
        SUB_EPOCH_TIME_UPDATE(shared, ASMP_CORE_SUB1_SEQ, t0_ns);
        SUB_LOAD_BUSY_END(load_m);
        SUB_LOAD_TICK(load_m, shared, ASMP_CORE_SUB1_SEQ);
    }

    return NULL;
}
