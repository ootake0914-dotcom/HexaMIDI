/**
 * @file synth_controller.c
 * @brief JoyStick Shield シンセサイザー・SD MIDI プレイヤーコントローラー実装
 * @details デュアルモード設計:
 *          - プレイヤーモード: B/D=SD MIDI 曲送り, E/F=音量,
 *            スティック Y=音量(連続) / X=曲送りフリック
 *          - 演奏モード: A/B/C/D=音階発音(ド・レ・ミ・ソ), E=オクターブ,
 *            F=波形, K=短押しスネア/長押しアルペジオ切替
 *          - E+F 同時押しで切替。演奏モード中は SD レーンのイベント配信が
 *            停止し、プレイヤーモードへ戻ると続きから自動再開する。
 *          - 共通: K 押し込み=スネア。
 *            演奏モードのスティックはスマート・スケールゾーンナビゲーター
 *            (4 ゾーン + レガートスライド) として動作し、X/Y 全域を占有する。
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "synth_controller.h"
#include "asmp_manager.h"
#include "async_logger.h"
#include "spatial_audio.h"

static const char *const s_wave_names[] = { "Sine", "Square", "Sawtooth", "Triangle" };

/* スマート・スティック演奏マトリクス: [4ゾーン][4ボタン (A,B,C,D)]
 * - CENTER:     ド(0),   レ(2),   ミ(4),   ファ(5)  [低音域 4音]
 * - HIGH (右/上): ソ(7),   ラ(9),   シ(11),  高ド(12) [高音域 4音]
 * - ACCIDENTAL(左): ド#(1), レ#(3),  ファ#(6), ソ#(8)  [黒鍵 4音]
 * - LOW (下):    低ソ(-5),低ラ(-3), 低シ(-1), ド(0)   [ベース音域 4音]
 */
static const int8_t s_scale_matrix[4][4] = {
    {  0,  2,  4,  5 },  /* [0] PERF_ZONE_CENTER */
    {  7,  9, 11, 12 },  /* [1] PERF_ZONE_HIGH */
    {  1,  3,  6,  8 },  /* [2] PERF_ZONE_ACCIDENTAL */
    { -5, -3, -1,  0 }   /* [3] PERF_ZONE_LOW */
};

static const char *const s_scale_names[4] = {
    "Do-Re-Mi-Fa (C-D-E-F)",
    "So-La-Ti-Do+ (G-A-B-C+)",
    "Accidental (C#-D#-F#-G#)",
    "Low Bass (G--A--B--C)"
};

static const char *const s_note_names_matrix[4][4] = {
    { "Do (C)",    "Re (D)",    "Mi (E)",    "Fa (F)" },
    { "So (G)",    "La (A)",    "Ti (B)",    "Hi-Do (C+)" },
    { "Do# (C#)",  "Re# (D#)",  "Fa# (F#)",  "So# (G#)" },
    { "Lo-So (G-)", "Lo-La (A-)", "Lo-Ti (B-)", "Do (C)" }
};

/* ========================================================================= */
/* 共通ヘルパー                                                              */
/* ========================================================================= */

static void ctrl_send_asmp(SynthController *ctrl, uint8_t core, const AsmpPacket *pkt)
{
    if (!ctrl->asmp || !pkt) return;
    if (!asmp_manager_send_command((AsmpManager *)ctrl->asmp, core, pkt)) {
        /* NOTE_OFF / ALL_NOTES_OFF は音残り (ゾンビノート) を防ぐため到達保証。
         * それ以外のパケットは単発試行で諦め警告のみ */
        bool critical = (pkt->msg_type == ASMP_MSG_NOTE_OFF ||
                         pkt->msg_type == ASMP_MSG_ALL_NOTES_OFF);
        uint32_t retries = 0;
        while (critical && retries < 1000u) {
            if (asmp_manager_send_command((AsmpManager *)ctrl->asmp, core, pkt)) {
                return;
            }
            retries++;
        }
        async_logf("[CTRL] Warning: ASMP queue full (core=%u)%s\n",
                   (unsigned int)core, critical ? " CRITICAL DROP" : "");
    }
}

static void ctrl_apply_volume(SynthController *ctrl)
{
    if (ctrl->asmp) {
        /* スティック Y 連続操作で毎フレーム (187Hz) 送信すると Sub1 キュー (128) を
         * 無駄に圧迫し、NOTE/CC バースト時の待ちを増やす。2% 量子化で間引きする
         * (体感は 2% ステップでも連続に聴こえる)。初期送信と 2% 超の変化のみ通す */
        static float s_last_sent = -1.0f;
        float cur = ctrl->volume;
        if (s_last_sent >= 0.0f && fabsf(cur - s_last_sent) < 0.015f) {
            /* ローカルエンジン側は即反映してもコストが低いため、分散時のみ間引く。
             * ただし 0.0/1.0 端点は必ず通す (ミュート/最大の確実性) */
            if (cur > 0.015f && cur < 0.985f) {
                return;
            }
        }
        s_last_sent = cur;
        AsmpPacket vol = {
            .msg_type = ASMP_MSG_CMD_VOLUME,
            .param = (uint32_t)(cur * 1000.0f)
        };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &vol); /* Sub1 経由で Sub5 へ (SPSC 単一プロデューサ維持) */
    } else if (ctrl->engine) {
        synth_engine_set_master_volume(ctrl->engine, ctrl->volume);
    }
}

/**
 * @brief スティック押し込み (K): ドラムスネアを重ねて発音 (全モード共通の演奏アクション)
 */
static void ctrl_stick_snare(SynthController *ctrl)
{
    if (ctrl->asmp) {
        AsmpPacket snare = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 9,
            .data1 = 38,   /* GM Snare Drum */
            .data2 = 120
        };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &snare); /* ch9 は Sub1 が Sub4 へルーティング */
    } else if (ctrl->engine) {
        synth_engine_channel_note_on(ctrl->engine, 9, 38, 0.95f);
    }
    async_logf("[CTRL] Snare!\n");
}

/**
 * @brief モード切替時のクリーンアップ
 * @details 演奏モードへ入ると Main ループの SD イベント配信が停止するため、
 *          ここでは発音中ノートの消音のみ行う。プレイヤーモードへ戻れば
 *          SD レーンは続きから自動再開する。
 */
static void ctrl_transition_mode(SynthController *ctrl, CtrlMode new_mode)
{
    (void)new_mode;
    if (!ctrl->engine) return;

    /* 発音中の全ボイスを消音 */
    synth_engine_all_notes_off(ctrl->engine);
    for (int i = 0; i < 4; i++) ctrl->perf_note[i] = 0;
    ctrl->perf_held_mask = 0;

    if (ctrl->asmp) {
        /* サブコア側で鳴り続ける音も確実に消音 (stuck note 防止) */
        AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF, .data1 = 0xFF };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &off);
    }
}

/* ========================================================================= */
/* 演奏モード (スマート・スティック演奏システム)                             */
/* ========================================================================= */

/**
 * @brief スティックのアナログ座標からスケールゾーンを判定 (ヒステリシス付き)
 * @details 境界 (0.35) 付近でスティックを保持すると ADC ノイズでゾーンが
 *          高速切替し、押下中ノートのレガートスライドが連発していました。
 *          進入 0.42 / 維持 0.28 の二重閾値でチャタリングを防止する。
 *          ゾーン優先度は旧実装と同一 (HIGH > ACCIDENTAL > LOW > CENTER)。
 */
static PerfStickZone perf_determine_zone(float x, float y, PerfStickZone prev)
{
    const float enter = 0.42f;
    const float exit_ = 0.28f;

    /* 1. 現ゾーンの維持判定 (緩い閾値) */
    switch (prev) {
        case PERF_ZONE_HIGH:
            if (x > exit_ || y > exit_) return PERF_ZONE_HIGH;
            break;
        case PERF_ZONE_ACCIDENTAL:
            if (x < -exit_) return PERF_ZONE_ACCIDENTAL;
            break;
        case PERF_ZONE_LOW:
            if (y < -exit_) return PERF_ZONE_LOW;
            break;
        default:
            break;
    }

    /* 2. 新規進入判定 (厳しい閾値、優先度は従来どおり) */
    if (x > enter || y > enter) return PERF_ZONE_HIGH;      /* 右 or 上 */
    if (x < -enter)             return PERF_ZONE_ACCIDENTAL; /* 左: 黒鍵 */
    if (y < -enter)             return PERF_ZONE_LOW;        /* 下: 低音 */
    return PERF_ZONE_CENTER;
}

/**
 * @brief 保持中の音を新しいノートへ滑らかに移行する (レガート/ピッチ追従)
 */
static void perf_retune_held(SynthController *ctrl, uint8_t old_note, uint8_t new_note, float velocity)
{
    if (old_note == new_note || old_note == 0) return;

    if (ctrl->asmp) {
        AsmpPacket off = {
            .msg_type = ASMP_MSG_NOTE_OFF,
            .channel = 0,
            .data1 = old_note
        };
        AsmpPacket on = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 0,
            .data1 = new_note,
            .data2 = (uint8_t)(velocity * 127.0f)
        };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &off); /* ch0 は Sub1 が Sub2 へルーティング */
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &on);
    } else if (ctrl->engine) {
        synth_engine_retune_voice(ctrl->engine, 0, old_note, new_note);
    }
}

/**
 * @brief 音階ボタン (A/B/C/D) の押下・解放処理 (押している間発音)
 */
static void perf_handle_scale_buttons(SynthController *ctrl, const JoystickState *state)
{
    static const uint8_t masks[4] = { BTN_MASK_A, BTN_MASK_B, BTN_MASK_C, BTN_MASK_D };

    for (int i = 0; i < 4; i++) {
        bool held_now = (state->current_buttons & masks[i]) != 0;
        bool held_prev = (ctrl->perf_held_mask & masks[i]) != 0;

        if (held_now == held_prev) continue;

        int16_t raw_note = (int16_t)(12 * (ctrl->perf_octave + 1) + s_scale_matrix[ctrl->perf_zone][i]);
        if (raw_note < 0) raw_note = 0;
        if (raw_note > 127) raw_note = 127;
        uint8_t note = (uint8_t)raw_note;

        if (held_now) {
            ctrl->perf_note[i] = note;
            if (ctrl->asmp) {
                AsmpPacket on = {
                    .msg_type = ASMP_MSG_NOTE_ON,
                    .channel = 0,
                    .data1 = note,
                    .data2 = 115
                };
                ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &on); /* ch0 は Sub1 が Sub2 へルーティング */
            } else if (ctrl->engine) {
                synth_engine_channel_note_on_w(ctrl->engine, 0, note, 0.9f, ctrl->perf_wave);
            }
            async_logf("[PERF] %s -> Note %u (%s, Oct%d, Zone: %s)\n",
                   s_note_names_matrix[ctrl->perf_zone][i], (unsigned int)note,
                   s_wave_names[ctrl->perf_wave], (int)ctrl->perf_octave,
                   s_scale_names[ctrl->perf_zone]);
        } else {
            note = ctrl->perf_note[i];
            if (note != 0) {
                if (ctrl->asmp) {
                    AsmpPacket off = {
                        .msg_type = ASMP_MSG_NOTE_OFF,
                        .channel = 0,
                        .data1 = note
                    };
                    ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &off);
                } else if (ctrl->engine) {
                    synth_engine_channel_note_off(ctrl->engine, 0, note);
                }
            }
            ctrl->perf_note[i] = 0;
        }

        if (held_now) {
            ctrl->perf_held_mask |= masks[i];
        } else {
            ctrl->perf_held_mask &= (uint8_t)~masks[i];
        }
    }
}

/**
 * @brief スティックゾーン変化に伴う保持中ノートのリアルタイム移行
 */
static void perf_update_stick_zone(SynthController *ctrl, const JoystickState *state)
{
    PerfStickZone new_zone = perf_determine_zone(state->stick_x, state->stick_y, ctrl->perf_zone);
    if (new_zone == ctrl->perf_zone) return;

    ctrl->perf_zone = new_zone;
    async_logf("[PERF] Scale Zone Switched: %s\n", s_scale_names[ctrl->perf_zone]);

    /* すでに押されているボタンがあれば、その音を新ゾーンの音程へ滑らかにスライド */
    for (int i = 0; i < 4; i++) {
        uint8_t old_note = ctrl->perf_note[i];
        if (old_note == 0) continue;

        int16_t raw_note = (int16_t)(12 * (ctrl->perf_octave + 1) + s_scale_matrix[ctrl->perf_zone][i]);
        if (raw_note < 0) raw_note = 0;
        if (raw_note > 127) raw_note = 127;
        uint8_t new_note = (uint8_t)raw_note;

        perf_retune_held(ctrl, old_note, new_note, 0.9f);
        ctrl->perf_note[i] = new_note;
        async_logf("  -> Legato Slide: %s (Note %u)\n",
               s_note_names_matrix[ctrl->perf_zone][i], (unsigned int)new_note);
    }
}

/**
 * @brief E ボタン: オクターブ循環 (C3 → C4 → C5 → C3)
 */
static void perf_cycle_octave(SynthController *ctrl)
{
    int8_t next = (int8_t)((ctrl->perf_octave >= 5) ? 3 : ctrl->perf_octave + 1);

    for (int i = 0; i < 4; i++) {
        uint8_t old_note = ctrl->perf_note[i];
        if (old_note == 0) continue;

        int16_t raw_note = (int16_t)(12 * (next + 1) + s_scale_matrix[ctrl->perf_zone][i]);
        if (raw_note < 0) raw_note = 0;
        if (raw_note > 127) raw_note = 127;
        uint8_t new_note = (uint8_t)raw_note;

        perf_retune_held(ctrl, old_note, new_note, 0.9f);
        ctrl->perf_note[i] = new_note;
    }

    ctrl->perf_octave = next;
    async_logf("[PERF] Octave: C%d (%s)\n", (int)next, s_scale_names[ctrl->perf_zone]);
}

/**
 * @brief F ボタン: 波形循環 (Square → Saw → Sine → Triangle)
 */
static void perf_cycle_wave(SynthController *ctrl)
{
    switch (ctrl->perf_wave) {
        case WAVE_SQUARE:    ctrl->perf_wave = WAVE_SAWTOOTH; break;
        case WAVE_SAWTOOTH:  ctrl->perf_wave = WAVE_SINE;     break;
        case WAVE_SINE:      ctrl->perf_wave = WAVE_TRIANGLE; break;
        default:             ctrl->perf_wave = WAVE_SQUARE;   break;
    }
    async_logf("[PERF] Wave: %s\n", s_wave_names[ctrl->perf_wave]);
}

/**
 * @brief アルペジオ ステップ処理
 *        保持中の音階 (perf_note[]) を昇順に上下バウンドで鳴らす。
 *        発音は ch1 (ローカル: ベースチャンネル / ASMP: SubCore 3 ルート)。
 */
static void perf_run_arp(SynthController *ctrl)
{
    if (!ctrl->arp_enabled || ctrl->perf_held_mask == 0) {
        if (ctrl->arp_last_note != 0) {
            uint8_t last = ctrl->arp_last_note;
            if (ctrl->asmp) {
                AsmpPacket off = { .msg_type = ASMP_MSG_NOTE_OFF, .channel = 1, .data1 = last };
                ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &off);
            } else if (ctrl->engine) {
                synth_engine_channel_note_off(ctrl->engine, 1, last);
            }
            ctrl->arp_last_note = 0;
        }
        ctrl->arp_step_idx = 0;
        ctrl->arp_dir = 1;
        ctrl->arp_frame_cnt = 0;
        return;
    }

    /* クロック: ASMP 時は Sub1 の 16 分クロックに同期 / ローカル時はフレーム分周 */
    bool tick = false;
    if (ctrl->asmp) {
        AsmpSharedContext *sc =
            asmp_manager_context((AsmpManager *)ctrl->asmp);
        if (sc != NULL) {
            uint32_t step = sc->seq_step16;
            if (step != ctrl->arp_last_seen) {
                ctrl->arp_last_seen = step;
                tick = true;
            }
        }
    } else {
        if (++ctrl->arp_frame_cnt >= ARP_STEP_FRAMES) {
            ctrl->arp_frame_cnt = 0;
            tick = true;
        }
    }
    if (!tick) return;

    /* 保持中ノートを昇順に収集 */
    uint8_t notes[4];
    uint8_t count = 0;
    for (int i = 0; i < 4; i++) {
        if (ctrl->perf_note[i] == 0) continue;
        int j = count++;
        while (j > 0 && notes[j - 1] > ctrl->perf_note[i]) {
            notes[j] = notes[j - 1];
            j--;
        }
        notes[j] = ctrl->perf_note[i];
    }
    if (count == 0) return;
    if (ctrl->arp_step_idx >= count) ctrl->arp_step_idx = (count > 1) ? (uint16_t)(count - 1) : 0;

    /* 前ステップを消して次のステップを発音 */
    if (ctrl->arp_last_note != 0) {
        if (ctrl->asmp) {
            AsmpPacket off = { .msg_type = ASMP_MSG_NOTE_OFF, .channel = 1, .data1 = ctrl->arp_last_note };
            ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &off);
        } else if (ctrl->engine) {
            synth_engine_channel_note_off(ctrl->engine, 1, ctrl->arp_last_note);
        }
    }

    uint8_t note = notes[ctrl->arp_step_idx];
    if (ctrl->asmp) {
        AsmpPacket on = {
            .msg_type = ASMP_MSG_NOTE_ON,
            .channel = 1,
            .data1 = note,
            .data2 = 100
        };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &on);
    } else if (ctrl->engine) {
        synth_engine_channel_note_on_w(ctrl->engine, 1, note, 0.80f, ctrl->perf_wave);
    }
    ctrl->arp_last_note = note;

    /* 上下バウンド進行 */
    int16_t nx = (int16_t)ctrl->arp_step_idx + ctrl->arp_dir;
    if (nx >= count)      { nx = (count > 1) ? (int16_t)(count - 2) : 0; ctrl->arp_dir = -1; }
    else if (nx < 0)      { nx = (count > 1) ? 1 : 0;                    ctrl->arp_dir = 1; }
    ctrl->arp_step_idx = (uint16_t)nx;
}

static void performance_mode_update(SynthController *ctrl, const JoystickState *state,
                                    bool e_pressed, bool f_pressed)
{
    /* 1. スティックの傾きによるスケールゾーン更新 (押下中ノートのスライド追従含む) */
    perf_update_stick_zone(ctrl, state);

    /* 2. ボタン押下・解放処理 (現在ゾーンの音階を発音) */
    perf_handle_scale_buttons(ctrl, state);

    if (e_pressed) perf_cycle_octave(ctrl);
    if (f_pressed) perf_cycle_wave(ctrl);

    /* K 押し込み: 短押し=スネア / 長押し (~0.6s)=アルペジオ切替
     * 閾値到達のその場で発動し、latch (0xFFFF) により離すまで再発火しない */
    if (state->current_buttons & BTN_MASK_STICK) {
        if (ctrl->k_hold_frames < 0xFFFFu) ctrl->k_hold_frames++;
        if (ctrl->k_hold_frames == ARP_K_HOLD_FRAMES) {
            ctrl->arp_enabled = !ctrl->arp_enabled;
            async_logf("[PERF] Arpeggio: %s\n", ctrl->arp_enabled ? "ON" : "OFF");
            ctrl->arp_step_idx = 0;
            ctrl->arp_dir = 1;
            ctrl->k_hold_frames = 0xFFFFu; /* latch */
        }
    } else {
        ctrl->k_hold_frames = 0;
    }

    if (state->pressed_buttons & BTN_MASK_STICK) {
        ctrl_stick_snare(ctrl);
    }

    perf_run_arp(ctrl);
}

/* ========================================================================= */
/* プレイヤーモード (SD MIDI)                                                */
/* ========================================================================= */

static void jukebox_handle_e_f_volume(SynthController *ctrl, bool e_pressed, bool f_pressed)
{
    if (e_pressed) {
        ctrl->volume -= 0.05f;
        if (ctrl->volume < 0.0f) ctrl->volume = 0.0f;
        ctrl_apply_volume(ctrl);
        async_logf("[PLAYER] Volume Down: %.0f%%\n", (double)(ctrl->volume * 100.0f));
    }

    if (f_pressed) {
        ctrl->volume += 0.05f;
        if (ctrl->volume > 1.0f) ctrl->volume = 1.0f;
        ctrl_apply_volume(ctrl);
        async_logf("[PLAYER] Volume Up: %.0f%%\n", (double)(ctrl->volume * 100.0f));
    }
}

static void jukebox_update(SynthController *ctrl, const JoystickState *state,
                           bool x_edge, int8_t zone, bool e_pressed, bool f_pressed)
{
    jukebox_handle_e_f_volume(ctrl, e_pressed, f_pressed);

    /* C ボタン: 立体音響 (Woodworth球モデル ITD/ILD) トグル */
    if (state->pressed_buttons & BTN_MASK_C) {
        if (ctrl->asmp) {
            AsmpSharedContext *sc = asmp_manager_context((AsmpManager *)ctrl->asmp);
            if (sc) {
                sc->main_ctrl.spatial_enable ^= 1u;
                async_logf("[SPATIAL] %s (Woodworth ITD %.2fms max)\n", sc->main_ctrl.spatial_enable ? "ON" : "OFF",
                           (double)SPATIAL_HEAD_RADIUS / (double)SPATIAL_SPEED_OF_SOUND * (M_PI/2 + 1) * 1000.0);
            }
        } else {
            async_logf("[SPATIAL] toggle ignored (single-core)\n");
        }
    }

    /* スティック押し込み (K): 演奏アクセントとしてスネア */
    if (state->pressed_buttons & BTN_MASK_STICK) { /* 押下エッジで 1 回のみ発音 */
        ctrl_stick_snare(ctrl);
    }

    /* A ボタン: 再生 / 一時停止 (Play / Pause) トグル */
    if (state->pressed_buttons & BTN_MASK_A) {
        if (!ctrl->sd_active) {
            async_logf("[PLAYER] No SD MIDI loaded: Play/Pause ignored.\n");
        } else {
            ctrl->sd_play_pause_request = true;
            async_logf("[PLAYER] Play/Pause toggle requested.\n");
        }
    }

    /* SD MIDI 曲送り: B / スティック右 = 次曲、D / スティック左 = 前曲。
     * 実際の切替 (消音 + ロード要求) は Main ループがフラグを消費して
     * 完奏時と同一の手順で行う */
    bool next_req = ((state->pressed_buttons & BTN_MASK_B) || (x_edge && zone > 0));
    bool prev_req = ((state->pressed_buttons & BTN_MASK_D) || (x_edge && zone < 0));

    if (next_req != prev_req) { /* B+D 同時押しは無視 */
        if (!ctrl->sd_active) {
            async_logf("[PLAYER] No SD MIDI loaded: track skip ignored.\n");
        } else {
            ctrl->sd_skip_request = next_req ? 1 : -1;
            async_logf("[PLAYER] %s Track requested.\n", next_req ? "Next" : "Prev");
        }
    }
}

/* ========================================================================= */
/* 公開 API                                                                  */
/* ========================================================================= */

void synth_controller_init(SynthController *ctrl, SynthEngine *engine)
{
    if (!ctrl) return;

    memset(ctrl, 0, sizeof(SynthController));
    ctrl->engine = engine;
    ctrl->mode = CTRL_MODE_JUKEBOX;
    ctrl->volume = 0.70f;
    ctrl->perf_wave = WAVE_SQUARE;
    ctrl->perf_octave = 4;

    ctrl_apply_volume(ctrl);
}

void synth_controller_bind_asmp(SynthController *ctrl, struct AsmpManager_s *mgr)
{
    if (!ctrl) return;
    ctrl->asmp = mgr;
    ctrl_apply_volume(ctrl);
}

void synth_controller_set_volume(SynthController *ctrl, float volume)
{
    if (!ctrl) return;
    if (!(volume > 0.0f)) volume = 0.0f;   /* NaN/負値ガード */
    if (volume > 1.0f) volume = 1.0f;
    ctrl->volume = volume;
    ctrl_apply_volume(ctrl);
}

void synth_controller_set_sd_active(SynthController *ctrl, bool active)
{
    if (!ctrl) return;
    ctrl->sd_active = active;
}

void synth_controller_resync_asmp(SynthController *ctrl)
{
    if (!ctrl || !ctrl->asmp) return;

    /* Watchdog 再起動後の SubCore は初期状態に戻っているため、
     * Main 側が把握している状態をこちらから再送する */
    AsmpPacket off = { .msg_type = ASMP_MSG_ALL_NOTES_OFF, .channel = 0xFF, .data1 = 0xFF };
    asmp_manager_send_command((AsmpManager *)ctrl->asmp, ASMP_CORE_SUB1_SEQ, &off);
    /* 再同期はスロットルをバイパスして確実に届ける (throttleで欠落すると再起動後無音) */
    {
        AsmpPacket vol = {
            .msg_type = ASMP_MSG_CMD_VOLUME,
            .param = (uint32_t)(ctrl->volume * 1000.0f)
        };
        ctrl_send_asmp(ctrl, ASMP_CORE_SUB1_SEQ, &vol);
    }
    async_logf("[CTRL] Resynced volume/state to restarted SubCores.\n");
}

CtrlMode synth_controller_mode(const SynthController *ctrl)
{
    return ctrl ? ctrl->mode : CTRL_MODE_JUKEBOX;
}

void synth_controller_update(SynthController *ctrl, const JoystickState *state)
{
    if (!ctrl || !state || !ctrl->engine) return;

    bool e_raw_held = (state->current_buttons & BTN_MASK_E) != 0;
    bool f_raw_held = (state->current_buttons & BTN_MASK_F) != 0;
    bool e_pressed  = (state->pressed_buttons & BTN_MASK_E) != 0;
    bool f_pressed  = (state->pressed_buttons & BTN_MASK_F) != 0;
    bool e_act = false, f_act = false;   /* 遅延解決後の個別アクション */
    bool combo_edge = false;
    bool was_latched = ctrl->combo_latch; /* フレーム更新前のラッチ値 */

    /* ------------------------------------------------------------- */
    /* 0. E+F 同時押し判定 (時間差チャタリング対策ウィンドウ付き)      */
    /*    人間の同時押しは数ms〜数十msずれるため、先着側を最大         */
    /*    EF_COMBO_WINDOW_FRAMES フレームだけ遅延させて相方を待つ      */
    /* ------------------------------------------------------------- */
    if (e_raw_held && f_raw_held) {
        ctrl->combo_latch = true;
        if (ctrl->ef_pending_btn != 0) {
            /* 待機中の単押しが相方到達でコンボ成立 */
            combo_edge = true;
            ctrl->ef_pending_btn = 0;
            ctrl->ef_pending_ticks = 0;
        } else if ((e_pressed || f_pressed) && !was_latched) {
            /* 本当に同時 (同一フレーム押下)
             * ※ ラッチにより「両方押し続けているだけ」の状態では
             *  押下エッジが立たず再発火しない (トグル暴走防止)。
             *  片方を離して再押しした場合は意図的操作として再度
             *  トグルを許容する */
            combo_edge = true;
        }
    } else if (ctrl->ef_pending_btn == 0) {
        if (e_pressed && !f_raw_held) {
            ctrl->ef_pending_btn = BTN_MASK_E;
            ctrl->ef_pending_ticks = 0;
        } else if (f_pressed && !e_raw_held) {
            ctrl->ef_pending_btn = BTN_MASK_F;
            ctrl->ef_pending_ticks = 0;
        } else {
            if (e_pressed) e_act = true;
            if (f_pressed) f_act = true;
            ctrl->combo_latch = false;
        }
    } else {
        /* 単押しの遅延判定中 */
        uint8_t pend = ctrl->ef_pending_btn;
        bool partner_now = (pend == BTN_MASK_E) ? f_pressed : e_pressed;
        bool released    = !(state->current_buttons & pend);

        if (partner_now) {
            combo_edge = true;
            ctrl->combo_latch = true;
            ctrl->ef_pending_btn = 0;
            ctrl->ef_pending_ticks = 0;
        } else if (released ||
                   ++ctrl->ef_pending_ticks >= EF_COMBO_WINDOW_FRAMES) {
            if (pend == BTN_MASK_E) e_act = true; else f_act = true;
            ctrl->ef_pending_btn = 0;
            ctrl->ef_pending_ticks = 0;
        }
    }

    /* E/F 両方離したらラッチ解除 (次のコンボを受け付けられるように) */
    if (!e_raw_held && !f_raw_held) {
        ctrl->combo_latch = false;
    }

    if (combo_edge) {
        CtrlMode new_mode = (ctrl->mode == CTRL_MODE_JUKEBOX)
                            ? CTRL_MODE_PERFORMANCE : CTRL_MODE_JUKEBOX;
        ctrl_transition_mode(ctrl, new_mode);
        ctrl->mode = new_mode;
        async_logf("\n*** MODE: %s ***\n",
               (ctrl->mode == CTRL_MODE_PERFORMANCE) ? "PERFORMANCE (Smart Stick: Full Octave / Accidental Scale)"
                                                     : "PLAYER (SD MIDI)");
        return; /* 切替フレームでは個別操作を無視 */
    }

    /* ------------------------------------------------------------- */
    /* 1. モード別処理 (演奏モード vs プレイヤーモード)              */
    /* ------------------------------------------------------------- */
    if (ctrl->mode == CTRL_MODE_PERFORMANCE) {
        /* 演奏モード: スティックはスマート・スケールナビゲーターとして稼働 */
        performance_mode_update(ctrl, state, e_act, f_act);
    } else {
        /* プレイヤーモード: スティック Y軸はマスター音量、X軸は曲送りフリック。
         * デッドゾーンは ±0.5: センターズレの微小ドリフトで音量が
         * 知らず 0% (ミュート) まで減衰するのを防ぐ。
         * 連続送信でSub1キューを圧迫しないよう 0.007*stick へ微減速し、
         * ctrl_apply_volume 側の 1.5% 量子化で間引く (最大 ~45Hz) */
        if (state->stick_y > 0.5f || state->stick_y < -0.5f) {
            ctrl->volume += state->stick_y * 0.007f;
            if (ctrl->volume > 1.0f) ctrl->volume = 1.0f;
            if (ctrl->volume < 0.0f) ctrl->volume = 0.0f;
            ctrl_apply_volume(ctrl);
        }

        /* X軸フリック: 進入 0.75 / 復帰 0.55 のヒステリシス付き 2 値判定。
         * 単一閾値だと境界でノイズにより zone が高速切替し、
         * 曲のスキップが連発するため (エッジ = zone 変化) */
        int8_t zone = ctrl->stick_x_zone;
        if      (state->stick_x > 0.75f) zone = 1;
        else if (state->stick_x < -0.75f) zone = -1;
        else if (state->stick_x > -0.55f && state->stick_x < 0.55f) zone = 0;
        bool x_edge = (zone != ctrl->stick_x_zone);
        ctrl->stick_x_zone = zone;

        jukebox_update(ctrl, state, x_edge, zone, e_act, f_act);
    }
}
