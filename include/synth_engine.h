/**
 * @file synth_engine.h
 * @brief Sony Spresense 16ch マルチティンバー・音声合成（シンセサイザー）エンジン
 * @details 16音ポリフォニー、GM音色マッピング、GM Standardドラムキット、ステレオリバーブDSP
 */

#ifndef SYNTH_ENGINE_H_
#define SYNTH_ENGINE_H_

/* SYNTH_MULTICORE はビルド系 (Makefile/CMake) が必ず 0 か 1 に定義する。
 * 0/1 のどちらか, 未定義でのフォールバックは廃止:
 * 構造体レイアウトがファイルごとに変わる危険のため、
 * ビルド系での定義を必須とする */
#if !defined(SYNTH_MULTICORE)
#error "SYNTH_MULTICORE must be defined via build system (-DSYNTH_MULTICORE=0 or 1)"
#endif

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* オーディオサンプリングパラメータ */
#define SYNTH_SAMPLE_RATE       (48000)  /* 48kHz */
#define SYNTH_CHANNELS          (2)      /* ステレオ (2ch) */
#define SYNTH_BITS_PER_SAMPLE   (16)     /* 16bit PCM */
#define SYNTH_MAX_POLYPHONY     (64)     /* 最大同時発音数 (64音ポリフォニック) */
#define SYNTH_NUM_CHANNELS      (16)     /* MIDI 16チャンネル */

/* 正弦波ルックアップテーブル (LUT) サイズ (1024 + 1 ガードポイント) */
#define SYNTH_SINE_LUT_SIZE     (1024)

/* ステレオリバーブ DSP 定数 */
#define REVERB_NUM_COMBS        (4)
#define REVERB_NUM_ALLPASS      (2)

/**
 * @brief オシレータ波形の種類
 */
typedef enum {
    WAVE_SINE = 0,     /**< サイン波 (1024エントリLUT + 線形補間) */
    WAVE_SQUARE,       /**< 矩形波 (PolyBLEP 帯域制限) */
    WAVE_SAWTOOTH,     /**< ノコギリ波 (PolyBLEP 帯域制限) */
    WAVE_TRIANGLE,     /**< 三角波 (PolyBLAMP 帯域制限) */
    WAVE_NOISE,        /**< ホワイトノイズ (LFSR) */
    WAVE_DRUM_KICK,    /**< ドラム: ピッチ急降下サイン波 */
    WAVE_DRUM_SNARE,   /**< ドラム: トーン + ノイズスネア */
    WAVE_DRUM_HIHAT,   /**< ドラム: ショート高域ノイズ */
    WAVE_DRUM_CYMBAL   /**< ドラム: ロングメタリックノイズ */
} WaveType;

/**
 * @brief ADSRエンベロープパラメータ
 */
typedef struct {
    float attack_time_sec;   /**< アタック時間 (秒) */
    float decay_time_sec;    /**< ディケイ時間 (秒) */
    float sustain_level;     /**< サステインレベル (0.0 ~ 1.0) */
    float release_time_sec;  /**< リリース時間 (秒) */
    bool  exponential_decay; /**< 指数関数的減衰カーブを使用するか */
} AdsrParams;

/**
 * @brief エンベロープの状態
 */
typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} EnvState;

/**
 * @brief 単一ボイス構造体
 */
typedef struct {
    bool     active;             /**< 発音中フラグ */
    uint8_t  channel;            /**< 発音元 MIDI チャンネル (0〜15) */
    uint8_t  note;               /**< MIDIノート番号 */
    float    velocity;           /**< ベロシティ (0.0 ~ 1.0) */
    float    frequency;          /**< 発音周波数 (Hz) */
    float    start_frequency;    /**< ドラム用開始周波数 (Hz) */
    float    target_frequency;   /**< ドラム用目標周波数 (Hz) */
    float    phase;              /**< オシレータ位相 (0.0 ~ 1.0) */
    float    phase_increment;    /**< 位相加算量 (dt) */
    float    metal_phase;        /**< シンバル金属音用の独立連続位相 (0.0 ~ 1.0)。
                                       位相に非整数倍率 (3.7x) を掛けていた旧実装は
                                       折返し毎に不連続になり周期バズを生んでいた */
    float    shim_lp;            /**< ドラムノイズ系 (ハット/スネア) の HP 用 one-pole LP 状態 */
    WaveType wave_type;          /**< 波形タイプ */
    
    /* ADSR エンベロープ状態 */
    AdsrParams adsr;
    EnvState env_state;
    float    current_env_level;
    float    release_start_level;
    float    attack_step;
    float    decay_step;
    float    release_step;
    float    decay_coeff;
    float    release_coeff;
    uint32_t env_samples;
    uint32_t phase_max_samples;
    uint32_t age_samples;        /**< ボイススチール用の発音経過サンプル数 */
    bool     sustained_by_pedal; /**< CC#64 ダンパーペダルによりリリース延期中 */
    uint32_t noise_seed;         /**< per-voice Xorshift seed (WAVE_NOISE/Drum decorrelation) */
} SynthVoice;

/**
 * @brief MIDI チャンネル個別状態
 */
typedef struct {
    uint8_t program;             /**< 音色プログラム番号 (0〜127) */
    float   volume;              /**< チャンネル音量 (CC#7: 0.0〜1.0) */
    float   expression;          /**< エクスプレッション (CC#11: 0.0〜1.0) */
    float   pan;                 /**< パン (CC#10: 0.0=Left, 0.5=Center, 1.0=Right) */
    float   reverb_send;         /**< リバーブセンド量 (CC#91: 0.0〜1.0) */
    float   pitch_bend_semitones;/**< ピッチベンド (-2.0〜+2.0 半音) */
    bool    sustain_pedal;       /**< サステインペダル (CC#64) */
} MidiChannelState;

/**
 * @brief Comb フィルタ構造体
 */
typedef struct {
    float *buffer;
    uint32_t buf_size;
    uint32_t buf_idx;
    float feedback;
    float filter_store;
    float damp;
} ReverbComb;

/**
 * @brief All-Pass フィルタ構造体
 */
typedef struct {
    float *buffer;
    uint32_t buf_size;
    uint32_t buf_idx;
    float feedback;
} ReverbAllPass;

/**
 * @brief ステレオリバーブ DSP エフェクト状態
 */
typedef struct {
    bool  enabled;           /**< エフェクト有効フラグ */
    float room_size;         /**< 空間の広さ (0.0 ~ 1.0) */
    float damping;           /**< 高音域ダンピング (0.0 ~ 1.0) */
    float wet_level;         /**< エフェクト音量 (0.0 ~ 1.0) */
    float dry_level;         /**< 原音音量 (0.0 ~ 1.0) */

    ReverbComb    combs_l[REVERB_NUM_COMBS];
    ReverbComb    combs_r[REVERB_NUM_COMBS];
    ReverbAllPass allpass_l[REVERB_NUM_ALLPASS];
    ReverbAllPass allpass_r[REVERB_NUM_ALLPASS];
} ReverbEffect;

/**
 * @brief シンセサイザーエンジン本体
 * @note  リバーブ遅延メモリをインスタンス内に所有するため、
 *        複数インスタンス生成しても相互干渉しない (~54KB/インスタンス)
 * @note  マルチコア (SYNTH_MULTICORE) では Main 上のローカルエンジンは
 *        実レンダリングに使われず (SubCore 側が担当)、54KB のリバーブ遅延
 *        メモリは不要のため構造体から除外する (BSS 54KB 削減)。
 *        reverb 状態構造体は API 互換のため残す (buffer=NULL で安全側退避)。
 */
typedef struct {
    SynthVoice voices[SYNTH_MAX_POLYPHONY];
    MidiChannelState channels[SYNTH_NUM_CHANNELS];
    float master_volume;     /**< マスター音量 (0.0 ~ 1.0) */
    WaveType default_wave;   /**< デフォルト波形 */
    AdsrParams default_adsr; /**< デフォルトADSR */
    uint32_t lfsr_state;     /**< LFSR 乱数シード */
    uint32_t dither_rng;     /**< TPDF ディザ量子化用 PRNG シード */

    /* ミックスバス DC ブロッカー状態 (L/R 各 x1/y1)。Sub5 と同一特性 r=0.995。
     * 低域うねり蓄積を防ぐ出力段フィルタ。reset_effects でクリアする */
    float dc_x1_l, dc_y1_l;
    float dc_x1_r, dc_y1_r;

    /* 正弦波LUTは共有ROM g_sine_lutを使用 (RAM 4KB削減) */

    /* チャンネル別パン ゲイン表 (等価パワー)。
     * render 内の per-sample cosf/sinf を排除するため CC#10 変更時に更新 */
    float ch_pan_cos[SYNTH_NUM_CHANNELS];
    float ch_pan_sin[SYNTH_NUM_CHANNELS];

    /* 発音中ボイスの詰め配列 (render 高速化用)。
     * render 内の 64 ボイス線形走査を発音中ボイスのみの走査へ短縮する。
     * active_list[0..num_active) に発音中ボイス番号を詰めて保持し、
     * active_pos[v] は active_list 内の位置 (非発音中は -1) を指す。
     * 追加・削除とも O(1) (削除は末尾スワップ) */
    uint8_t active_list[SYNTH_MAX_POLYPHONY];
    int8_t  active_pos[SYNTH_MAX_POLYPHONY];
    uint8_t num_active;

    /* ステレオリバーブエフェクト */
    ReverbEffect reverb;

#if !defined(SYNTH_MULTICORE) || !SYNTH_MULTICORE
    /* リバーブ遅延メモリ (インスタンス所有。reverb 内の buffer がここを指す)
     * シングルコア (ホスト/フォールバック) でのみ必要 */
    float comb_mem_l[REVERB_NUM_COMBS][1400];
    float comb_mem_r[REVERB_NUM_COMBS][1400];
    float allpass_mem_l[REVERB_NUM_ALLPASS][600];
    float allpass_mem_r[REVERB_NUM_ALLPASS][600];
#endif
} SynthEngine;

/**
 * @brief シンセサイザーエンジンの初期化
 */
void synth_engine_init(SynthEngine *engine);

/**
 * @brief マスター音量の設定 (0.0 ~ 1.0)
 */
void synth_engine_set_master_volume(SynthEngine *engine, float volume);

/**
 * @brief ステレオリバーブの設定 (有効フラグ, room_size, damping, wet_level)
 */
void synth_engine_set_reverb(SynthEngine *engine, bool enabled, float room_size, float damping, float wet_level);

/**
 * @brief リバーブの有効/無効切り替え
 */
void synth_engine_set_reverb_enabled(SynthEngine *engine, bool enabled);

/**
 * @brief MIDI チャンネル別プログラム (音色) 変更 (Program Change)
 */
void synth_engine_program_change(SynthEngine *engine, uint8_t channel, uint8_t program);

/**
 * @brief MIDI チャンネル別コントロールチェンジ (CC) 処理
 */
void synth_engine_control_change(SynthEngine *engine, uint8_t channel, uint8_t control, uint8_t value);

/**
 * @brief MIDI チャンネル別ピッチベンド処理
 */
void synth_engine_pitch_bend(SynthEngine *engine, uint8_t channel, int16_t bend_value);

/**
 * @brief 発音中ボイスのノート番号を差し替える (エンベロープを維持したまま音程のみ移動)
 * @details 演奏モードでオクターブ切替した際、保持中の音を押し直さずに上げ下げするために使用。
 *          周波数は現在のチャンネル・ピッチベンド設定も反映する。
 */
void synth_engine_retune_voice(SynthEngine *engine, uint8_t channel, uint8_t old_note, uint8_t new_note);

/**
 * @brief チャンネル指定ノートオン（発音開始）
 * @param engine シンセサイザーエンジンへのポインタ
 * @param channel MIDIチャンネル (0〜15, 9=ドラム)
 * @param note MIDIノート番号 (0〜127)
 * @param velocity ベロシティ (0.0 ~ 1.0)
 * @return 割り当てられたボイスインデックス (失敗時は -1)
 */
int synth_engine_channel_note_on(SynthEngine *engine, uint8_t channel, uint8_t note, float velocity);

/**
 * @brief 互換ノートオン（デフォルトチャンネル 0 で発音）
 */
int synth_engine_note_on(SynthEngine *engine, uint8_t note, float velocity, WaveType wave_type);

/**
 * @brief チャンネル指定 + 波形指定ノートオン (プリセット楽曲のマルチパート再生用)
 * @note  channel == 9 (ドラム) の場合は wave_type を無視し GM ノートマッピングを優先
 */
int synth_engine_channel_note_on_w(SynthEngine *engine, uint8_t channel, uint8_t note, float velocity, WaveType wave_type);

/**
 * @brief チャンネル指定ノートオフ（離鍵）
 */
void synth_engine_channel_note_off(SynthEngine *engine, uint8_t channel, uint8_t note);

/**
 * @brief 互換ノートオフ（全チャンネルから該当ノートを検索して離鍵）
 */
void synth_engine_note_off(SynthEngine *engine, uint8_t note);

/**
 * @brief 全ボイスの完全消音
 */
void synth_engine_all_notes_off(SynthEngine *engine);

/**
 * @brief エフェクト内部バッファのゼロクリア
 */
void synth_engine_reset_effects(SynthEngine *engine);

/**
 * @brief PCM サンプルバッファのレンダリング (16bit ステレオインターリーブ)
 */
void synth_engine_render(SynthEngine *engine, int16_t *buffer, uint32_t frames);

/**
 * @brief リバーブDSPインパルス応答のレンダリング (テスト・音響解析用)
 */
void synth_engine_render_impulse_response(SynthEngine *engine, int16_t *buffer, uint32_t frames);

/**
 * @brief MIDIノート番号から周波数(Hz)への変換
 */
float synth_note_to_freq(uint8_t note);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_ENGINE_H_ */
