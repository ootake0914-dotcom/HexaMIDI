/**
 * @file asmp_protocol.h
 * @brief Sony Spresense ASMP 6コア完全分散処理・コア間通信プロトコル定義
 * @details Main Core (Core 0) および SubCore 1〜5 間のメッセージ・共有メモリ構造体・ロックフリーキュー
 */

#ifndef ASMP_PROTOCOL_H_
#define ASMP_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* offsetof (レイアウト固定アサート用) */

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* 1. コア ID 定義                                                           */
/* ========================================================================= */
#define ASMP_CORE_MAIN      (0)  /**< Core 0: Main Core (OS, JoyStick, DMA Audio Out, 監視) */
#define ASMP_CORE_SUB1_SEQ  (1)  /**< Core 1: MIDI シーケンサー / タイムマネージャー */
#define ASMP_CORE_SUB2_LEAD (2)  /**< Core 2: メロディ / リード / ピアノ音源 (PolyBLEP 8ボイス) */
#define ASMP_CORE_SUB3_BASS (3)  /**< Core 3: ベース / ストリングス / コード音源 (8ボイス) */
#define ASMP_CORE_SUB4_DRUM (4)  /**< Core 4: GM Standard ドラムキット音源 */
#define ASMP_CORE_SUB5_DSP  (5)  /**< Core 5: 5バンド EQ & ステレオリバーブ DSP エフェクト */

#define ASMP_NUM_CORES      (6)
#define ASMP_BUFFER_FRAMES  (512)
/* キュー容量 128: 64では密集譜面で SUB1 キューが溢れ NOTE/CC ロスト。
 * 2026-08-29 全ワーカー再ビルド済みで AsmpSharedContext 35744B に拡張、
 * pcm_* オフセットも再整合済み。64に戻すと再び破損するため触らないこと */
#define ASMP_QUEUE_CAPACITY (512)

/* ========================================================================= */
/* ビルド識別タグ (実機シリアルログでリビジョン一致を確認する用)              */
/* Main / SubCore 1-5 全てが起動時に [BUILD] 行として出力する。               */
/* DSP パラメータを変更したらサフィックスを更新すること。                     */
/* 実機ログに旧タグが出た場合は romfs 埋め込み ELF が古い (=再ビルド漏れ)     */
/* ※ #define のみで構造体レイアウトには影響しない (バイナリ互換性維持)        */
/* ========================================================================= */
/* [dsp-20260903-p0c] P0-C Render Mailbox & Non-Droppable Control Plane */
#define HEXASENSE_DSP_TAG "dsp-20260903-q512-spawn"

/* ABI whiskey (S1) - 共有コンテキストのレイアウト不一致を起動時に検出 */
#define ASMP_PROTOCOL_MAGIC   0x48535836u /* "H" "S" "X" "6" */
#define ASMP_PROTOCOL_VERSION 13u /* 13: Core1 voice-spawn descriptor pool + stats (2026-09-03) / 12: render_mbox 64Bライン分離 + main_ctrlライタ分離 / 11: q512 + slot gen-guard + owner_mask原子化 / 10: P0-C Render Mailbox + Non-Droppable Control Plane */

/* Ping-Pong スロット数: エポック偶奇で PCM バッファを切り替えることで、
 * SubCore 2-4 の「次エポック合成」と SubCore 5 の「現エポック・ミキシング」を
 * オーバーラップさせる (完全同期だとフレーム時間 = max(S2..S4)+S5 になるが、
 * 非同期化により = max(全コア個別時間) まで短縮される) */
#define ASMP_NUM_SLOTS      (2u)
#define ASMP_EPOCH_SLOT(epoch) ((uint32_t)(epoch) & 1u)

/* quality_flags ビット定義 (Main Core の品質ガバナーが設定)
 * 段階的デグレード: 0 = 5osc / UNISON_3OSC = 3osc / UNISON_OFF = 1osc
 * 余裕時HQ: 余裕がある時のみワイドデチューンで音質向上 (512B ROM増のみ)
 * GOV:3: ドラムECO = Metal系を6osc->3osc + PolyBLEP省略で軽量化 (SUB4専用) */
#define ASMP_QF_UNISON_3OSC (0x02)
#define ASMP_QF_UNISON_OFF  (0x01)
#define ASMP_QF_HQ_WIDE     (0x04)
#define ASMP_QF_DRUM_ECO    (0x08)

/* MP ライブラリ オブジェクトキー (Main / Worker で厳密に同期すること) */
#define ASMP_KEY_SHM        (10)
#define ASMP_KEY_MUTEX      (11)

/* ROMFS 埋め込みワーカー ELF 名 (asmp_sub/Makefile のビルド生成物と一致させる) */
#define ASMP_WORKER_NAME_SUB1 "synth_worker1"
#define ASMP_WORKER_NAME_SUB2 "synth_worker2"
#define ASMP_WORKER_NAME_SUB3 "synth_worker3"
#define ASMP_WORKER_NAME_SUB4 "synth_worker4"
#define ASMP_WORKER_NAME_SUB5 "synth_worker5"
#define ASMP_ROMFS_MOUNTPT    "/romfs"

/* アライメント指定マクロ (32バイト境界 / L1 キャッシュライン) */
#if defined(_MSC_VER)
#  define ASMP_ALIGN32 __declspec(align(32))
#else
#  define ASMP_ALIGN32 __attribute__((aligned(32)))
#endif

/* コア間メモリアクセス順序保証 (ARM: DMB / x86: コンパイラバリア) */
#if defined(__GNUC__) || defined(__clang__)
#  define ASMP_BARRIER() __sync_synchronize()
#elif defined(_MSC_VER)
#  define ASMP_BARRIER() _ReadWriteBarrier()
#else
#  define ASMP_BARRIER()
#endif

/* キャッシュコヒーレンス管理ヘルパー (実機: up_clean/invalidate_dcache / ホスト: no-op) */
#ifdef __NuttX__
#  include <nuttx/arch.h>
static inline void asmp_dcache_clean(const void *addr, size_t size)
{
    if (addr && size > 0) {
        uintptr_t start = (uintptr_t)addr;
        uintptr_t end = start + (uintptr_t)size;
        up_clean_dcache(start, end);
        (void)start;
        (void)end;
    }
}

static inline void asmp_dcache_invalidate(const void *addr, size_t size)
{
    if (addr && size > 0) {
        uintptr_t start = (uintptr_t)addr;
        uintptr_t end = start + (uintptr_t)size;
        up_invalidate_dcache(start, end);
        (void)start;
        (void)end;
    }
}
#else
static inline void asmp_dcache_clean(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
}

static inline void asmp_dcache_invalidate(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
}
#endif

/**
 * @brief コア core がエポック epoch の処理を完了済みか (ラップアラウンド安全)
 *        done_epoch は単調増加のため、符号付き差分で比較する。
 *        「クリア → 立て替え」型のマスク方式と異なり競合で偽完了が起きない。
 */
static inline bool asmp_epoch_done(const volatile uint32_t *done_epoch, uint32_t target)
{
    return ((int32_t)(*done_epoch - target) >= 0);
}

/* ========================================================================= */
/* 2. コア間メッセージ ID 定義                                               */
/* ========================================================================= */
typedef enum {
    ASMP_MSG_NONE           = 0x00,
    /* 0x01 旧 HEARTBEAT (廃止: 死活監視は共有メモリの heartbeats[] を直接使用) */
    ASMP_MSG_CMD_PLAY       = 0x02,  /**< 楽曲再生開始 */
    ASMP_MSG_CMD_PAUSE      = 0x03,  /**< 楽曲一時停止 */
    ASMP_MSG_CMD_NEXT       = 0x04,  /**< 次の曲へ */
    ASMP_MSG_CMD_PREV       = 0x05,  /**< 前の曲へ */
    ASMP_MSG_CMD_SELECT     = 0x06,  /**< 指定曲番号選択 (data1 = 曲番号) */
    ASMP_MSG_CMD_VOLUME     = 0x07,  /**< ボリューム変更 (param = float音量*1000) */
    /* 0x08 旧 CMD_TEMPO (廃止: テンポは共有メモリ tempo_us_per_quarter 経由で共有) */
    ASMP_MSG_CMD_STOP       = 0x09,  /**< 停止 */

    /* リアルタイム MIDI イベント */
    ASMP_MSG_NOTE_ON        = 0x10,  /**< ノートオン (channel, data1=note, data2=velocity) */
    ASMP_MSG_NOTE_OFF       = 0x11,  /**< ノートオフ (channel, data1=note) */
    ASMP_MSG_PROGRAM_CHANGE = 0x12,  /**< 音色変更 (channel, data1=program) */
    ASMP_MSG_CONTROL_CHANGE = 0x13,  /**< コントロールチェンジ (channel, data1=cc_num, data2=val) */
    ASMP_MSG_PITCH_BEND     = 0x14,  /**< ピッチベンド (channel, param=bend_raw_signed) */
    ASMP_MSG_ALL_NOTES_OFF  = 0x15,  /**< 全音消音 (channel) */

    /* 同期制御シグナル */
    ASMP_MSG_RENDER_REQ     = 0x20   /**< レンダリング要求 (param = render_epoch 値) */
} AsmpMsgType;

/* MIDIイベント入力ソース識別 (ヒューマナイズ制御用) */
#define MIDI_SOURCE_SMF   0u /**< SMFファイル再生 (velocity厳密) */
#define MIDI_SOURCE_LIVE  1u /**< ライブ入力 (humanize許可) */

/* ========================================================================= */
/* 3. コア間メッセージパケット (12バイト: サンプルオフセット+ソース対応)       */
/* ========================================================================= */
typedef struct {
    uint8_t  msg_type;      /**< AsmpMsgType */
    uint8_t  channel;       /**< MIDI チャンネル (0〜15) */
    uint8_t  data1;         /**< ノート番号 / CC番号 / プログラム番号 / 曲インデックス */
    uint8_t  data2;         /**< ベロシティ / CC値 */
    uint16_t sample_offset; /**< チャンク先頭からのサンプルオフセット (0..511, 1サンプル精度) */
    uint8_t  source;        /**< MIDI_SOURCE_SMF / MIDI_SOURCE_LIVE */
    uint8_t  _pad;          /**< アライメントパディング */
    uint32_t param;         /**< 追加パラメータ (ピッチベンド, 音量等) */
} AsmpPacket;

/* ========================================================================= */
/* 4. ロックフリー・シングルプロデューサ・シングルコンシューマ リングバッファ  */
/* ========================================================================= */
typedef struct ASMP_ALIGN32 {
    AsmpPacket buffer[ASMP_QUEUE_CAPACITY];
    volatile uint32_t head;  /**< 書き込みインデックス (プロデューサコアのみ加算) */
    /* 偽共有 (False Sharing) 分離用パディング:
     * Cortex-M4F の L1 キャッシュラインは 32B。head と tail が同一ラインに
     * 載ると、片方の更新が他コアのラインを無効化し、キュー操作のたびに
     * コア間バス通信が発生する。pad0[28] で head/tail を別ラインへ分離する
     * (head は buffer 直後で常に 32B 境界、64容量時は 512B) */
    uint8_t pad0[28];
    volatile uint32_t tail;  /**< 読み出しインデックス (コンシューマコアのみ加算) */
    uint8_t pad1[28];        /**< 構造体サイズを 32B 倍数に丸め、配列 queues[N] の要素間アライメントを保証 (64->576B) */
} AsmpRingBuffer;

/**
 * @brief リングバッファへパケットを追加 (ノンブロッキング)
 *        実機は write-back D-cacheのため head/buffer の clean が必須
 */
static inline bool asmp_queue_push(AsmpRingBuffer *rb, const AsmpPacket *pkt)
{
    /* tail は consumer が更新、最新を観測するため invalidate */
    asmp_dcache_invalidate((const void *)&rb->tail, sizeof(rb->tail));
    ASMP_BARRIER();
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t next_head = (head + 1) & (ASMP_QUEUE_CAPACITY - 1);
    if (next_head == tail) {
        return false; /* キュー満杯 */
    }
    rb->buffer[head] = *pkt;
    asmp_dcache_clean((const void *)&rb->buffer[head], sizeof(AsmpPacket));
    ASMP_BARRIER();
    rb->head = next_head;
    asmp_dcache_clean((const void *)&rb->head, sizeof(rb->head));
    return true;
}

/**
 * @brief リングバッファからパケットを取得 (ノンブロッキング)
 * @note  ARM 弱メモリ順序では、head 更新の観測後にパケット本体を読む際も
 *        Acquire バリアが必須。これがないと古いデータを先読みするレースが起きる
 *        実機は write-backのため head/buffer の invalidate が必須
 */
static inline bool asmp_queue_pop(AsmpRingBuffer *rb, AsmpPacket *pkt)
{
    uint32_t tail = rb->tail;
    asmp_dcache_invalidate((const void *)&rb->head, sizeof(rb->head));
    ASMP_BARRIER();
    if (tail == rb->head) {
        return false; /* キュー空 */
    }
    ASMP_BARRIER(); /* head 確認 (acquire) -> パケット本体読み出しの順序保証 */
    asmp_dcache_invalidate((const void *)&rb->buffer[tail], sizeof(AsmpPacket));
    ASMP_BARRIER();
    *pkt = rb->buffer[tail];
    ASMP_BARRIER();
    rb->tail = (tail + 1) & (ASMP_QUEUE_CAPACITY - 1);
    asmp_dcache_clean((const void *)&rb->tail, sizeof(rb->tail));
    return true;
}

/* Per-core telemetry cell: 1 writer per core, 32B isolated (P0-A) */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t heartbeat;      /* each core writes its own */
    volatile uint32_t render_busy_us; /* EMA */
    volatile uint16_t voice_count;
    volatile uint16_t cpu_load;       /* 0.1% */
    uint8_t _pad[20];
} AsmpCoreCell;

/* Render control: Main single writer, 32B isolated */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t render_epoch;
    volatile uint32_t epoch_frames[ASMP_NUM_SLOTS];
    volatile uint32_t slot_epoch[ASMP_NUM_SLOTS];
    uint8_t _pad[12];
} AsmpRenderCtrl;

/* Done epoch per-core: 1 writer per core, 32B each */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t val;
    uint8_t _pad[28];
} AsmpDoneCell;

/* Main control: line0 = Main writer only, line1 = Sub5/Sub1 signaling only.
 * 旧レイアウトは Main 書き (shutdown等) と Sub5/Sub1 書き (force_clear_req) が
 * 同一32Bラインに同居し、ライトバック環境では相互の更新を消し合う
 * (lost update → epoch skip/消音要求消失)。64B2ラインに分離する */
typedef struct ASMP_ALIGN32 {
    /* --- line 0: Main Core のみ書き込み --- */
    volatile uint32_t tempo_us_per_quarter;
    volatile uint32_t seq_step16_dummy; /* placeholder, real seq is Sub1 */
    volatile uint8_t quality_flags;
    volatile uint8_t spatial_enable;
    volatile bool shutdown_requested;
    uint8_t _pad_main[21]; /* line0 合計 4+4+1+1+1+21 = 32B (下記static_assertで保証) */
    /* --- line 1: Sub5 が要求セット / Sub1 がクリア --- */
    volatile uint8_t sub5_force_clear_req;
    uint8_t _pad_sub[31];
} AsmpMainCtrl;

/* P0-B Slot generation guard: separate array, 32B per slot, no PCM offset change (案B) */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t epoch;        /* this slot belongs to epoch */
    volatile uint32_t generation;   /* increment per reuse */
    volatile uint32_t owner_mask;   /* bitmask of completed workers */
    uint8_t _pad[20];
} AlignedSlotHeader;

/* ========================================================================= */
/* P0-C: Render Mailbox — line0 = Main writer, line1 = Sub1 writer (計64B)    */
/* 旧32Bレイアウトは Main 書き (render_epoch) と Sub1 書き (ack_epoch) が      */
/* 同一キャッシュラインに同居し、ライトバック環境では相互更新を消し合って      */
/* epoch skip (音飛び) を起こす。2ライン分離で構造的に根絶する。               */
/* ========================================================================= */
typedef struct ASMP_ALIGN32 {
    /* --- line 0: Main Core のみ書き込み --- */
    volatile uint32_t render_epoch;  /**< Main Core がフレーム開始時に更新 */
    volatile uint32_t generation;    /**< 世代一致確認用 (Main のみ) */
    uint8_t _pad_main[24];           /**< line0 残 (4+4+24=32B) */
    /* --- line 1: Sub1 のみ書き込み --- */
    volatile uint32_t ack_epoch;     /**< Sub1 が処理完了後に更新 */
    uint8_t _pad_sub[28];            /**< line1 残 (4+28=32B) */
} AsmpRenderMailbox; /* 合計64B */

/* ========================================================================= */
/* P0-C: Non-Droppable Control Plane (16ch sticky state, 208B)               */
/* ========================================================================= */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t all_notes_off_gen[16];  /**< 全消音 generation */
    volatile uint32_t all_sound_off_gen[16];  /**< 即時消音 generation */
    volatile uint32_t sustain_off_gen[16];    /**< ペダル解除 generation */
    uint8_t _pad[16];                         /**< 32B 倍数パディング (16*4*3=192 + 16=208B) */
} ChannelControlState;

/* ========================================================================= */
/* ABI v13: Core1 voice-spawn descriptor (MIDI分解のCore1集約)                */
/* 生MIDI解釈 (program->音色/ADSR/フィルタ、note->Q32増分/周波数) を Core1    */
/* (Sub1) へ集約し、演奏コア (Sub2/Sub3) は spawn-frozen パラメータの受領と   */
/* 発音のみ行う。同一 program はどのコア・どの順序でも同一正準マッピングで   */
/* 発音されるため、移動前後やPC到達順序による音色・音程の不一致 (音痴) を     */
/* 構造的に根絶する。発音中に live 追従すべきもの (bend/vol/pan/expr/mod/CC)  */
/* は含まず、従来通り演奏コア側チャンネル状態を毎タイル読む (MIDI意味論維持)。*/
/* 輸送: NOTE_ON.param にトークン [31:24]magic [23:8]gen [7:0]slot を添付。   */
/* プール満杯・世代不一致時は param=0 の従来解釈経路へ自動フォールバックする  */
/* (フォールバックも同一正準関数で解釈するため音は一致する)。                */
/* ========================================================================= */
#define SUB_SPAWN_POOL_SLOTS (16u)
#define SUB_SPAWN_TOKEN_MAGIC (0x5Au)

/** Core1 が事前解決する note-on パラメータ (52B)。
 *  WaveType2 (sub2) / WaveType3 (sub3) と同順 (SINE=0/SQUARE=1/SAW=2/TRI=3) の
 *  波形番号を wave に格納する。enum 順序を変えたら本定義と同時更新すること */
#define SUB_SPAWN_WAVE_SINE     (0u)
#define SUB_SPAWN_WAVE_SQUARE   (1u)
#define SUB_SPAWN_WAVE_SAWTOOTH (2u)
#define SUB_SPAWN_WAVE_TRIANGLE (3u)

typedef struct {
    uint8_t  channel;
    uint8_t  note;
    uint8_t  velocity;      /**< humanize済み 1..127 */
    uint8_t  program;       /**< 解決に使った program (記録・診断用) */
    float    frequency;     /**< bend-free 基本周波数 (freq LUT初期式とbit一致) */
    float    base_increment;/**< frequency / 48000 (従来式と同一) */
    float    adsr_a, adsr_d, adsr_s, adsr_r;
    uint8_t  exp_decay;
    uint8_t  wave;          /**< SUB_SPAWN_WAVE_* */
    uint8_t  is_bass;       /**< Sub3 サブオシ判定 (prog 32..39) */
    uint8_t  unison;        /**< Sub2 スーパーソウ判定 (Core1で間引き時は強制薄化) */
    uint8_t  wt_active;     /**< Sub2 ウェーブテーブル発音 (prog < 80) */
    uint8_t  morph_a, morph_b;
    uint8_t  mip;
    float    filt_base, filt_peak, filt_q;
    float    morph_w;
} SubSpawnDesc; /* 4 + 8 + 16 + 8 + 16 = 52B */

/** SPSC ディスクリプタスロット (96B = 32B×3ライン)。
 *  line0: gen のみ (Core1 writer)。desc (line1-2) と同居させないことで
 *  世代公開とペイロード公開の順序をキャッシュライン単位で分離する */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t gen;   /**< 0=無効, 1..単調増加 (Core1のみ書き込み) */
    uint8_t _pad0[28];       /**< line0 残 */
    SubSpawnDesc desc;        /**< line1-2 (Core1のみ書き込み、52B) */
    uint8_t _pad1[12];        /**< 4+28+52+12 = 96B */
} SubSpawnSlot;

/** spawn 消費カウンタ (演奏コア single writer、コア毎に32B分離) */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t consumed; /**< 消費 (spawn/ack) 回数 */
    uint8_t _pad[28];
} SubSpawnAck;

/** spawn 経路統計 (各演奏コア single writer、コア毎に32B分離。
 *  テスト・現調用。fast+legacy の合計が受信 NOTE_ON 数に一致する) */
typedef struct ASMP_ALIGN32 {
    volatile uint32_t fast_spawn;   /**< ディスクリプタ fast-spawn 回数 */
    volatile uint32_t legacy_spawn; /**< 従来解釈フォールバック回数 */
    uint8_t _pad[24];
} SubSpawnStats;

/* ========================================================================= */
/* レイアウト固定アサート (C99互換 typedef トリック): マルチライタ分離のための */
/* 32Bライン境界をコンパイル時に保証する。いずれか失格したらパディング見直し */
/* ========================================================================= */
typedef char asmp_assert_mbox_size[(sizeof(AsmpRenderMailbox) == 64) ? 1 : -1];
typedef char asmp_assert_mbox_ack_line[(offsetof(AsmpRenderMailbox, ack_epoch) == 32) ? 1 : -1];
typedef char asmp_assert_mainctrl_size[(sizeof(AsmpMainCtrl) == 64) ? 1 : -1];
typedef char asmp_assert_mainctrl_sig_line[(offsetof(AsmpMainCtrl, sub5_force_clear_req) == 32) ? 1 : -1];
typedef char asmp_assert_corecell[(sizeof(AsmpCoreCell) == 32) ? 1 : -1];
typedef char asmp_assert_renderctrl[(sizeof(AsmpRenderCtrl) == 32) ? 1 : -1];
typedef char asmp_assert_donecell[(sizeof(AsmpDoneCell) == 32) ? 1 : -1];
typedef char asmp_assert_slotheader[(sizeof(AlignedSlotHeader) == 32) ? 1 : -1];
typedef char asmp_assert_spawndesc[(sizeof(SubSpawnDesc) == 52) ? 1 : -1];
typedef char asmp_assert_spawnslot[(sizeof(SubSpawnSlot) == 96) ? 1 : -1];
typedef char asmp_assert_spawndesc_line[(offsetof(SubSpawnSlot, desc) == 32) ? 1 : -1];
typedef char asmp_assert_spawnack[(sizeof(SubSpawnAck) == 32) ? 1 : -1];
typedef char asmp_assert_spawnstats[(sizeof(SubSpawnStats) == 32) ? 1 : -1];
typedef char asmp_assert_ringbuffer[(sizeof(AsmpRingBuffer) == (size_t)ASMP_QUEUE_CAPACITY * sizeof(AsmpPacket) + 64u) ? 1 : -1];
typedef char asmp_assert_ringhead[(offsetof(AsmpRingBuffer, head) == (size_t)ASMP_QUEUE_CAPACITY * sizeof(AsmpPacket)) ? 1 : -1];
typedef char asmp_assert_ringtail[(offsetof(AsmpRingBuffer, tail) == (size_t)ASMP_QUEUE_CAPACITY * sizeof(AsmpPacket) + 32u) ? 1 : -1];

/* ========================================================================= */
/* 5. 6コア共有メモリ構造体 (ゼロコピー PCM パイプライン ＆ テレメトリ)      */
/* ========================================================================= */
typedef struct ASMP_ALIGN32 {
    /* ABI ヘッダ: 構造体変更の検出用 */
    volatile uint32_t abi_magic;
    volatile uint32_t abi_version;
    volatile uint32_t abi_size;
    uint8_t _pad_abi[20]; /* pad to 32B line */

    /* Per-core telemetry: each core writes its own 32B cell, no false sharing */
    AsmpCoreCell core[ASMP_NUM_CORES];

    /* 動的チャンネルルーティングのテレメトリ (Sub1 single writer) */
    volatile uint32_t route_moves;      /**< 累積再割当て回数 */
    volatile uint32_t route_last_ch;    /**< 最後に移動したチャンネル */
    volatile uint32_t route_last_core;  /**< 移動先コア (2 or 3) */
    uint8_t _pad_route[20];

    /* C4 (ドラム) 監視テレメトリ (Sub1 single writer) */
    volatile uint32_t c4_state;
    volatile uint32_t c4_candidate;
    uint8_t _pad_c4[24];

    /* コア別メッセージ受信用キュー (各コアが自身のキューから読み出し) */
    AsmpRingBuffer queues[ASMP_NUM_CORES];

    /* 各音源サブコア PCM 出力バッファ (float ステレオインターリーブ [-1,1] 振幅,
     * Ping-Pong 2 スロット) - each 32B aligned */
    ASMP_ALIGN32 float pcm_sub2_melody[ASMP_NUM_SLOTS][ASMP_BUFFER_FRAMES * 2];
    ASMP_ALIGN32 float pcm_sub3_bass[ASMP_NUM_SLOTS][ASMP_BUFFER_FRAMES * 2];
    ASMP_ALIGN32 float pcm_sub4_drums[ASMP_NUM_SLOTS][ASMP_BUFFER_FRAMES * 2];
    ASMP_ALIGN32 int16_t pcm_sub5_master[ASMP_NUM_SLOTS][ASMP_BUFFER_FRAMES * 2];

    /* レンダリング同期制御: Main single writer, 32B isolated */
    AsmpRenderCtrl render_ctrl;

    /* P0-B Slot headers: Main writes epoch/generation at allocate, workers validate */
    AlignedSlotHeader slot_headers[ASMP_NUM_SLOTS];
    volatile uint32_t slot_generation; /* global generation counter, Main only */
    uint8_t _pad_slot_gen[28];

    /* Done epoch: per-core 32B each, no line sharing */
    AsmpDoneCell done_epoch[ASMP_NUM_CORES];

    /* Main control: line0=Main writer, line1=Sub5/Sub1 signaling (分離済み) */
    AsmpMainCtrl main_ctrl;

    /* Sub1 seq: Sub1 single writer */
    volatile uint32_t seq_step16;
    uint32_t _step_acc_samples;
    uint8_t _pad_seq[24];

    /* 診断カウンタ: per-core or Sub1 writer, padded */
    volatile uint32_t diag_c4_active_edges;
    volatile uint32_t diag_c4_idle_edges;
    volatile uint32_t diag_c4_overload_edges;
    uint8_t _pad_diag0[20];
    volatile uint32_t diag_epoch_gap[ASMP_NUM_CORES];
    uint8_t _pad_diag1[8];
    volatile uint32_t diag_slot_mismatch;
    volatile uint32_t diag_slot_rejected; /* P0-B late writer rejected */
    volatile uint32_t diag_queue_drop;
    volatile uint32_t diag_timeout;
    volatile uint32_t diag_c4_candidate_changes;
    uint8_t _pad_diag2[12];
    volatile uint16_t queue_depth[ASMP_NUM_CORES];
    uint8_t _pad_qd[20];
    volatile uint32_t queue_backpressure_hits[ASMP_NUM_CORES];
    uint8_t _pad_qb[8];

    /* P0-C: Render Mailbox (line0=Main writer, line1=Sub1 writer, 64B分離済み) */
    ASMP_ALIGN32 AsmpRenderMailbox render_mbox;

    /* P0-C: Non-Droppable Control Plane (Main/SD single writer, Sub1/Workers reader) */
    ASMP_ALIGN32 ChannelControlState ch_control;

    /* ABI v13: Core1 voice-spawn プール (各プール Core1 single writer)。
     * 末尾追加のため既存オフセットは不変。BSS ゼロ初期化で gen=0=無効から開始 */
    ASMP_ALIGN32 SubSpawnSlot spawn_pool_sub2[SUB_SPAWN_POOL_SLOTS];
    ASMP_ALIGN32 SubSpawnSlot spawn_pool_sub3[SUB_SPAWN_POOL_SLOTS];
    /* ABI v13: spawn 消費カウンタ (各演奏コア single writer) */
    ASMP_ALIGN32 SubSpawnAck spawn_ack_sub2;
    ASMP_ALIGN32 SubSpawnAck spawn_ack_sub3;
    /* ABI v13: spawn 経路統計 (各演奏コア single writer、診断用) */
    ASMP_ALIGN32 SubSpawnStats spawn_stats_sub2;
    ASMP_ALIGN32 SubSpawnStats spawn_stats_sub3;
} AsmpSharedContext;



/* P0-B Slot generation helpers */
#define ASMP_SLOT_WORKER_BIT_SUB2 (1u << 0)
#define ASMP_SLOT_WORKER_BIT_SUB3 (1u << 1)
#define ASMP_SLOT_WORKER_BIT_SUB4 (1u << 2)
#define ASMP_SLOT_WORKER_BIT_SUB5 (1u << 3)

static inline void asmp_slot_allocate(AsmpSharedContext *shared, uint32_t slot, uint32_t epoch) {
    if (!shared || slot >= ASMP_NUM_SLOTS) return;
    AlignedSlotHeader *h = &shared->slot_headers[slot];
    uint32_t gen = shared->slot_generation + 1;
    shared->slot_generation = gen;
    h->epoch = epoch;
    h->generation = gen;
    h->owner_mask = 0;
    asmp_dcache_clean((const void *)h, sizeof(*h));
    asmp_dcache_clean((const void *)&shared->slot_generation, sizeof(shared->slot_generation));
    ASMP_BARRIER();
}

static inline bool asmp_slot_validate(const AsmpSharedContext *shared, uint32_t slot, uint32_t epoch) {
    if (!shared || slot >= ASMP_NUM_SLOTS) return false;
    const AlignedSlotHeader *h = &shared->slot_headers[slot];
    asmp_dcache_invalidate((const void *)h, sizeof(*h));
    ASMP_BARRIER();
    return h->epoch == epoch;
}

static inline bool asmp_slot_commit(AsmpSharedContext *shared, uint32_t slot, uint32_t epoch, uint32_t worker_bit) {
    if (!shared || slot >= ASMP_NUM_SLOTS) return false;
    AlignedSlotHeader *h = &shared->slot_headers[slot];
    asmp_dcache_invalidate((const void *)h, sizeof(*h));
    ASMP_BARRIER();
    if (h->epoch != epoch) {
        // late writer: increment rejected counter if available
        // Use diag_slot_rejected as shared counter (may need per-core, but single for now)
        return false;
    }
    /* owner_mask は Sub2/3/4 の3ワーカーが同一スロットへ並行 commit する
     * マルチライタ変数。素朴な read-modify-write (prev=mask; mask=prev|bit) は
     * 逆アセンブルで非アトミック (ARM: ldr/orr/str, x86: mov/or/mov, lock prefix無し)
     * と確認され、密集パートで finish が重なると lost update でビット欠落する。
     * 欠落すると Sub5 の owner 完備判定 ((mask&need)==need) が偽陰性となり、
     * 完成済み PCM をフェード代用で捨てる誤タイムアウト=音飛びを起こす。
     * __sync_fetch_and_or (ARM: ldrex/strex リトライループ, x86: lock or) で
     * アトミック OR 化し、欠落を構造的に根絶する */
#if defined(__GNUC__) || defined(__clang__)
    __sync_fetch_and_or((uint32_t *)&h->owner_mask, worker_bit);
#else
    {
        uint32_t prev = h->owner_mask;
        h->owner_mask = prev | worker_bit;
    }
#endif
    asmp_dcache_clean((const void *)&h->owner_mask, sizeof(h->owner_mask));
    ASMP_BARRIER();
    return true;
}

/* ========================================================================= */
/* P0-C: Render Mailbox & Control Plane API                                  */
/* ========================================================================= */

/* mailbox 半ライン保守用オフセット (line1 = +32B)。構造体変更時は
 * 下記コンパイル時アサートが失格するため、数値直書きの腐敗を防げる */
#define ASMP_MBOX_LINE_BYTES (32u)

static inline void asmp_render_mbox_begin(AsmpSharedContext *shared, uint32_t epoch) {
    if (!shared) return;
    shared->render_mbox.generation++;
    shared->render_mbox.render_epoch = epoch;
    /* line0 (Main所有) のみ clean。line1 (Sub1のack) に触れない */
    asmp_dcache_clean((const void *)&shared->render_mbox, ASMP_MBOX_LINE_BYTES);
    ASMP_BARRIER();
}

static inline bool asmp_render_mbox_poll(AsmpSharedContext *shared, uint32_t *epoch_out) {
    if (!shared) return false;
    asmp_dcache_invalidate((const void *)&shared->render_mbox, sizeof(shared->render_mbox));
    ASMP_BARRIER();
    uint32_t ep = shared->render_mbox.render_epoch;
    if (ep != shared->render_mbox.ack_epoch && ep != 0) {
        if (epoch_out) *epoch_out = ep;
        return true;
    }
    return false;
}

static inline void asmp_render_mbox_ack(AsmpSharedContext *shared, uint32_t epoch) {
    if (!shared) return;
    shared->render_mbox.ack_epoch = epoch;
    /* line1 (Sub1所有) のみ clean。line0 (Mainのepoch) に触れない */
    asmp_dcache_clean((const void *)((uintptr_t)&shared->render_mbox + ASMP_MBOX_LINE_BYTES),
                      ASMP_MBOX_LINE_BYTES);
    ASMP_BARRIER();
}

static inline void asmp_control_plane_all_notes_off(AsmpSharedContext *shared, uint8_t channel) {
    if (!shared) return;
    if (channel < 16) {
        shared->ch_control.all_notes_off_gen[channel]++;
        asmp_dcache_clean((const void *)&shared->ch_control.all_notes_off_gen[channel], sizeof(uint32_t));
    } else {
        for (int ch = 0; ch < 16; ch++) {
            shared->ch_control.all_notes_off_gen[ch]++;
        }
        asmp_dcache_clean((const void *)shared->ch_control.all_notes_off_gen, sizeof(shared->ch_control.all_notes_off_gen));
    }
    ASMP_BARRIER();
}

static inline void asmp_control_plane_sustain_off(AsmpSharedContext *shared, uint8_t channel) {
    if (!shared) return;
    if (channel < 16) {
        shared->ch_control.sustain_off_gen[channel]++;
        asmp_dcache_clean((const void *)&shared->ch_control.sustain_off_gen[channel], sizeof(uint32_t));
    } else {
        for (int ch = 0; ch < 16; ch++) {
            shared->ch_control.sustain_off_gen[ch]++;
        }
        asmp_dcache_clean((const void *)shared->ch_control.sustain_off_gen, sizeof(shared->ch_control.sustain_off_gen));
    }
    ASMP_BARRIER();
}

static inline void asmp_control_plane_all_sound_off(AsmpSharedContext *shared, uint8_t channel) {
    if (!shared) return;
    if (channel < 16) {
        shared->ch_control.all_sound_off_gen[channel]++;
        asmp_dcache_clean((const void *)&shared->ch_control.all_sound_off_gen[channel], sizeof(uint32_t));
    } else {
        for (int ch = 0; ch < 16; ch++) {
            shared->ch_control.all_sound_off_gen[ch]++;
        }
        asmp_dcache_clean((const void *)shared->ch_control.all_sound_off_gen, sizeof(shared->ch_control.all_sound_off_gen));
    }
    ASMP_BARRIER();
}

/* S1: ABI整合性チェック - Main/Workerの構造体レイアウト不一致を即検出 */
static inline bool asmp_abi_ok(const volatile AsmpSharedContext *shared)
{
    if (!shared) return false;
    if (shared->abi_magic != ASMP_PROTOCOL_MAGIC) return false;
    if (shared->abi_version != ASMP_PROTOCOL_VERSION) return false;
    if (shared->abi_size != (uint32_t)sizeof(AsmpSharedContext)) return false;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* ASMP_PROTOCOL_H_ */
