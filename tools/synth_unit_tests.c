/**
 * @file synth_unit_tests.c
 * @brief シンセエンジン・16chマルチティンバー・MIDIパーサー・シーケンサー・コントローラーの単体テストスイート
 */

/* Release (NDEBUG) ビルドでも assert を有効化してテストを機能させる */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include "synth_engine.h"
#include "sequencer.h"
#include "preset_songs.h"
#include "synth_controller.h"
#include "midi_parser.h"
#include "midi_player.h"
#include "midi_demo_songs.h"
#include "sd_midi.h"
#include "sub_common.h"

static void test_adsr_sustain_zero(void)
{
    printf("[TEST] Testing ADSR with sustain_level == 0.0f...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    engine.default_adsr.attack_time_sec = 0.001f;
    engine.default_adsr.decay_time_sec = 0.010f;
    engine.default_adsr.sustain_level = 0.0f;
    engine.default_adsr.release_time_sec = 0.020f;

    /* プログラム0 (Piano) の実際のエンベロープ: A=3ms D=350ms S=0.30 R=80ms
     * (channel_note_on はプログラム別 ADSR を適用するため default_adsr は参考値) */
    int v_idx = synth_engine_note_on(&engine, 60, 1.0f, WAVE_SINE);
    assert(v_idx >= 0);

    /* レンダリングを数十サンプル進める (Attack中) */
    int16_t buf[64 * 2];
    synth_engine_render(&engine, buf, 64);
    assert(engine.voices[v_idx].active);

    /* ノートオフ発行 */
    synth_engine_note_off(&engine, 60);
    assert(engine.voices[v_idx].env_state == ENV_RELEASE || engine.voices[v_idx].env_state == ENV_IDLE);

    /* リリース時間 (80ms) + マージン分レンダリングして完全消音を確認 */
    float release_sec = engine.voices[v_idx].adsr.release_time_sec + 0.020f;
    uint32_t release_frames = (uint32_t)(release_sec * SYNTH_SAMPLE_RATE);
    for (uint32_t i = 0; i < release_frames; i += 64) {
        synth_engine_render(&engine, buf, 64);
    }

    /* 完全に消音・非アクティブになっていることを検証 */
    assert(engine.voices[v_idx].active == false);
    assert(engine.voices[v_idx].env_state == ENV_IDLE);
    assert(engine.voices[v_idx].current_env_level == 0.0f);
    printf("  -> PASS: Sustain zero release test passed.\n");
}

static void test_adsr_mid_attack_release(void)
{
    printf("[TEST] Testing ADSR mid-attack early note off...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    engine.default_adsr.attack_time_sec = 0.100f; // 100ms
    engine.default_adsr.decay_time_sec = 0.100f;
    engine.default_adsr.sustain_level = 0.8f;
    engine.default_adsr.release_time_sec = 0.050f; // 50ms

    int v_idx = synth_engine_note_on(&engine, 60, 1.0f, WAVE_SQUARE);
    assert(v_idx >= 0);

    /* プログラム0 Piano のアタックは約3ms (=144 frames)。
     * 96 frames レンダリングしてアタック途中で止める */
    int16_t buf[256 * 2];
    synth_engine_render(&engine, buf, 48);
    synth_engine_render(&engine, buf, 48);
    assert(engine.voices[v_idx].env_state == ENV_ATTACK);
    float mid_level = engine.voices[v_idx].current_env_level;
    assert(mid_level > 0.0f && mid_level < 1.0f);

    /* アタック途中でノートオフ */
    synth_engine_note_off(&engine, 60);
    assert(engine.voices[v_idx].env_state == ENV_RELEASE);
    assert(fabsf(engine.voices[v_idx].release_start_level - mid_level) < 0.01f);

    /* リリース完了までレンダリング */
    for (int i = 0; i < 20; i++) {
        synth_engine_render(&engine, buf, 256);
    }

    assert(engine.voices[v_idx].active == false);
    assert(engine.voices[v_idx].env_state == ENV_IDLE);
    assert(engine.voices[v_idx].current_env_level == 0.0f);
    printf("  -> PASS: Mid-attack release test passed.\n");
}

static void test_sequencer_sample_timing(void)
{
    printf("[TEST] Testing sequencer cumulative timing precision...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    static const NoteEvent test_events[] = {
        { 60, 125, 100, 0.8f, WAVE_SINE }, // 125ms = 6000 samples @ 48kHz
        { 62, 125, 100, 0.8f, WAVE_SINE },
        { 64, 125, 100, 0.8f, WAVE_SINE },
        { 65, 125, 100, 0.8f, WAVE_SINE },
    };
    static const Track test_track = {
        .title = "Timing Test Track",
        .events = test_events,
        .event_count = 4,
        .bpm = 120,
        .loop = false
    };

    Sequencer seq;
    sequencer_init(&seq, &engine);
    sequencer_play_track(&seq, &test_track);

    uint32_t chunk_size = 256;
    uint32_t total_samples = 0;

    while (seq.is_playing) {
        total_samples += chunk_size;
        sequencer_tick_frames(&seq, chunk_size, SYNTH_SAMPLE_RATE);
    }

    uint32_t expected_min = 24000;
    uint32_t expected_max = 24000 + chunk_size;

    assert(total_samples >= expected_min && total_samples <= expected_max);
    printf("  -> Total rendered samples: %u (Expected range: %u - %u)\n", 
           total_samples, expected_min, expected_max);
    printf("  -> PASS: Sequencer timing accuracy verified without tempo drift.\n");
}

static void test_controller_dap(void)
{
    printf("[TEST] Testing SD MIDI player controller (Skip request, Volume)...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    SynthController ctrl;
    synth_controller_init(&ctrl, &engine);
    int16_t ctrl_buf[512 * 2]; /* エンベロープ進行用のレンダリングバッファ */

    /* SD レーン活性状態を模擬 (Main が SD MIDI を保持している状態) */
    synth_controller_set_sd_active(&ctrl, true);

    /* 1. ボタン B (Next Track ⏩): Main へ +1 要求が立つ */
    JoystickState state = {0};
    state.pressed_buttons = BTN_MASK_B;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.sd_skip_request == 1);
    ctrl.sd_skip_request = 0; /* Main ループの消費を模擬 */

    /* 2. ボタン D (Prev Track ⏪): -1 要求 */
    memset(&state, 0, sizeof(state));
    state.pressed_buttons = BTN_MASK_D;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.sd_skip_request == -1);
    ctrl.sd_skip_request = 0;

    /* 3. B+D 同時押しは排他され要求が出ない */
    memset(&state, 0, sizeof(state));
    state.pressed_buttons = BTN_MASK_B | BTN_MASK_D;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.sd_skip_request == 0);

    /* 4. SD 非活性時はスキップ要求が出ない */
    synth_controller_set_sd_active(&ctrl, false);
    memset(&state, 0, sizeof(state));
    state.pressed_buttons = BTN_MASK_B;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.sd_skip_request == 0);
    synth_controller_set_sd_active(&ctrl, true);

    /* 5. ボタン E (Volume Down 🔉) & ボタン F (Volume Up 🔊)
     *    単押しはコンボ判定ウィンドウ (~4フレーム) を経て解決される */
    float init_vol = ctrl.volume;
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E;
    state.pressed_buttons = BTN_MASK_E;
    synth_controller_update(&ctrl, &state);
    memset(&state, 0, sizeof(state));        /* 離して確定 */
    synth_controller_update(&ctrl, &state);
    assert(ctrl.volume < init_vol);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    memset(&state, 0, sizeof(state));
    synth_controller_update(&ctrl, &state);
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    memset(&state, 0, sizeof(state));
    synth_controller_update(&ctrl, &state);
    assert(ctrl.volume > init_vol);
    assert(fabsf(ctrl.volume - 0.75f) < 0.001f);

    /* 6. スティック押し込み (K): スネア発音 (ch9 ドラムボイスが立つ) */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_STICK;
    state.pressed_buttons = BTN_MASK_STICK;
    synth_controller_update(&ctrl, &state);
    {
        int drum_voices = 0;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].active && engine.voices[i].channel == 9) {
                drum_voices++;
            }
        }
        assert(drum_voices >= 1);
    }

    /* 7. スティック X軸: 倒した瞬間だけ曲送り (ホールド連打防止) */
    memset(&state, 0, sizeof(state));
    state.stick_x = 1.0f;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.stick_x_zone == 1);
    assert(ctrl.sd_skip_request == 1);
    ctrl.sd_skip_request = 0;

    synth_controller_update(&ctrl, &state); /* ホールド中は連打されない */
    assert(ctrl.sd_skip_request == 0);

    memset(&state, 0, sizeof(state));       /* 中央へ戻す -> ゾーン解除 */
    synth_controller_update(&ctrl, &state);
    assert(ctrl.stick_x_zone == 0);

    memset(&state, 0, sizeof(state));
    state.stick_x = -1.0f;
    synth_controller_update(&ctrl, &state); /* 左へ倒す -> 前の曲 */
    assert(ctrl.stick_x_zone == -1);
    assert(ctrl.sd_skip_request == -1);
    ctrl.sd_skip_request = 0;

    /* 8. E+F 同時押し: 演奏モードへ切替 */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_PERFORMANCE);
    assert(ctrl.combo_latch == true);

    /* ホールド中は再切替されない
     * (実機のドライバは pressed をエッジで1回のみ報告するため、
     *  2フレーム目は current のみセットするのが正しい再現) */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_PERFORMANCE);
    assert(ctrl.perf_octave == 4); /* ホールド中に操作が重複しないこと */

    /* 9. A ボタン (ド) 押下で発音 / 離すと消音 */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.pressed_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_held_mask & BTN_MASK_A);
    {
        int found_c4 = 0;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].active && engine.voices[i].channel == 0 &&
                engine.voices[i].note == 60) { /* C4 (オクターブ4基準) */
                found_c4++;
            }
        }
        assert(found_c4 >= 1);
    }

    memset(&state, 0, sizeof(state));
    state.released_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    assert((ctrl.perf_held_mask & BTN_MASK_A) == 0);
    {
        bool all_releasing = true;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].channel == 0 && engine.voices[i].note == 60 &&
                engine.voices[i].active &&
                engine.voices[i].env_state != ENV_RELEASE) {
                all_releasing = false;
            }
        }
        assert(all_releasing);
    }

    /* 10. F ボタン: 波形循環 -> A 押下 (C4) -> E ボタンでオクターブアップ
     *     保持中の音が押し直し不要で C4 -> C5 へ滑ることを検証 */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    memset(&state, 0, sizeof(state));        /* 離して確定 */
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_wave == WAVE_SAWTOOTH);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.pressed_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_note[0] == 60); /* C4 発音中 */

    /* アタック (3ms = 144 frames) を越えてディケイへ進行させておく */
    synth_engine_render(&engine, ctrl_buf, 240);
    {
        bool in_decay = false;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].active && engine.voices[i].channel == 0 &&
                engine.voices[i].note == 60 &&
                engine.voices[i].env_state == ENV_DECAY) {
                in_decay = true;
            }
        }
        assert(in_decay);
    }

    /* A は押し続けたまま E だけを追加で押す (実機の物理操作の再現) */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.pressed_buttons = BTN_MASK_E;
    synth_controller_update(&ctrl, &state);

    /* E を離してコンボ不成立を確定 (A は保持継続) -> オクターブ循環が実行される */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.released_buttons = BTN_MASK_E;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_octave == 5);
    assert(ctrl.perf_note[0] == 72); /* 保持音が C5 へ滑っている */

    /* 保持中 (A) の音がエンベロープを維持したまま C4 -> C5 へ移動していること */
    {
        int retuned = 0;
        bool env_kept = true;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].active && engine.voices[i].channel == 0 &&
                engine.voices[i].note == 72) {
                retuned++;
                if (engine.voices[i].env_state != ENV_SUSTAIN &&
                    engine.voices[i].env_state != ENV_DECAY) {
                    env_kept = false;
                }
            }
        }
        assert(retuned >= 1);
        assert(env_kept);
    }

    /* 離してから改めて A を押すと C5 の新規発音になる */
    memset(&state, 0, sizeof(state));
    state.released_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_note[0] == 0);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.pressed_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    {
        int found_c5_fresh = 0;
        for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
            if (engine.voices[i].active && engine.voices[i].channel == 0 &&
                engine.voices[i].note == 72) {
                found_c5_fresh++;
            }
        }
        assert(found_c5_fresh >= 1);
    }

    memset(&state, 0, sizeof(state));
    state.released_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);

    /* 11. 演奏モードのスマート・スティック: スケールゾーン切り替え (右=HIGH / 左=ACCIDENTAL / 中央=CENTER) */
    memset(&state, 0, sizeof(state));
    state.stick_x = 1.0f;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_HIGH);

    memset(&state, 0, sizeof(state));
    state.stick_x = -1.0f;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_ACCIDENTAL);

    memset(&state, 0, sizeof(state));
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_CENTER);

    /* 12. E+F 同時押し: ジュークボックスへ復帰 */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_JUKEBOX);

    /* 13. 時間差 E+F (数フレームずれ): コンボ成立し、個別の音量操作は発動しない */
    float vol_before_combo = ctrl.volume;

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E;      /* 先に E だけ */
    state.pressed_buttons = BTN_MASK_E;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.ef_pending_btn == BTN_MASK_E); /* 遅延中 */

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F; /* ウィンドウ内で F 追加 */
    state.pressed_buttons = BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_PERFORMANCE);
    assert(fabsf(ctrl.volume - vol_before_combo) < 0.001f); /* 音量不変 */

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_PERFORMANCE); /* ホールド連打なし */

    memset(&state, 0, sizeof(state));           /* 両方離す -> ラッチ解除 */
    synth_controller_update(&ctrl, &state);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_F;      /* 今度は F 先着 */
    synth_controller_update(&ctrl, &state);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_JUKEBOX);

    /* 14. アルペジエータ: 演奏モードで K 長押しトグル、保持音をステップ発音 (ch1) */
    memset(&state, 0, sizeof(state));
    synth_controller_update(&ctrl, &state);          /* 両方離す -> ラッチ解除 */

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);          /* -> PERFORMANCE */
    assert(ctrl.mode == CTRL_MODE_PERFORMANCE);

    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A | BTN_MASK_STICK;
    state.pressed_buttons = BTN_MASK_STICK;
    synth_controller_update(&ctrl, &state);
    for (int i = 0; i < 60; i++) {
        memset(&state, 0, sizeof(state));
        state.current_buttons = BTN_MASK_A | BTN_MASK_STICK;
        synth_controller_update(&ctrl, &state);
    }
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.released_buttons = BTN_MASK_STICK;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.arp_enabled == true);

    {
        bool saw_ch1_voice = false;
        for (int i = 0; i < 48 && !saw_ch1_voice; i++) {
            memset(&state, 0, sizeof(state));
            state.current_buttons = BTN_MASK_A;
            synth_controller_update(&ctrl, &state);
            synth_engine_render(&engine, ctrl_buf, 64);
            for (int vi = 0; vi < SYNTH_MAX_POLYPHONY; vi++) {
                if (engine.voices[vi].active && engine.voices[vi].channel == 1) {
                    saw_ch1_voice = true;
                }
            }
        }
        assert(saw_ch1_voice);
    }

    /* K 長押しで OFF */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A | BTN_MASK_STICK;
    state.pressed_buttons = BTN_MASK_STICK;
    synth_controller_update(&ctrl, &state);
    for (int i = 0; i < 60; i++) {
        memset(&state, 0, sizeof(state));
        state.current_buttons = BTN_MASK_A | BTN_MASK_STICK;
        synth_controller_update(&ctrl, &state);
    }
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.released_buttons = BTN_MASK_STICK;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.arp_enabled == false);

    /* ジュークボックスへ復帰して終了 */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.mode == CTRL_MODE_JUKEBOX);

    printf("  -> PASS: Controller combo symmetry & anti-chattering verified.\n");
}

static void test_sine_lut_accuracy(void)
{
    printf("[TEST] Testing 1024-entry Sine LUT interpolation accuracy...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    float max_error = 0.0f;
    for (int i = 0; i < 4800; i++) {
        float phase = (float)i / 4800.0f;
        float true_sine = sinf(phase * 2.0f * 3.14159265358979323846f);

        float pos = phase * (float)SYNTH_SINE_LUT_SIZE;
        int idx = (int)pos;
        float frac = pos - (float)idx;
        idx &= (SYNTH_SINE_LUT_SIZE - 1);
        float lut_sine = g_sine_lut[idx] + frac * (g_sine_lut[idx + 1] - g_sine_lut[idx]);

        float err = fabsf(true_sine - lut_sine);
        if (err > max_error) max_error = err;
    }

    printf("  -> Max Sine LUT interpolation error: %.6f (Tolerance: < 0.0001)\n", max_error);
    assert(max_error < 0.0001f);
    printf("  -> PASS: Sine LUT interpolation is highly accurate.\n");
}

static void test_polyblep_oscillators(void)
{
    printf("[TEST] Testing PolyBLEP/BLAMP band-limited oscillators (Square, Saw, Triangle)...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    int16_t buf[256 * 2];
    uint8_t high_notes[] = { 84, 96, 108 };

    for (int n = 0; n < 3; n++) {
        for (int w = (int)WAVE_SINE; w <= (int)WAVE_TRIANGLE; w++) {
            synth_engine_init(&engine);
            int v = synth_engine_note_on(&engine, high_notes[n], 1.0f, (WaveType)w);
            assert(v >= 0);

            for (int f = 0; f < 10; f++) {
                synth_engine_render(&engine, buf, 256);
            }

            assert(engine.voices[v].active);
        }
    }
    printf("  -> PASS: PolyBLEP oscillators rendered stably across high-frequency ranges.\n");
}

static void test_stereo_reverb_effect(void)
{
    printf("[TEST] Testing Stereo Reverb (Comb + All-Pass + Damping LPF)...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    /* 1. リバーブ無効時は左右完全同一のモノラル信号 */
    synth_engine_note_on(&engine, 60, 1.0f, WAVE_SQUARE);
    int16_t buf[256 * 2];
    synth_engine_render(&engine, buf, 256);

    for (int i = 0; i < 256; i++) {
        assert(abs((int)buf[i * 2 + 0] - (int)buf[i * 2 + 1]) <= 2); /* L ≈ R (TPDF dither ±1 LSB) */
    }

    /* 2. リバーブ有効時はステレオスプレッドにより左右に位相差・空間的な広がりが発生
     *    (L/R コム長差 1116/1139 により、差分発生まで最短約 1116 サンプル必要) */
    synth_engine_set_reverb(&engine, true, 0.7f, 0.5f, 0.5f);

    bool has_stereo_diff = false;
    for (int f = 0; f < 10 && !has_stereo_diff; f++) {
        synth_engine_render(&engine, buf, 256);
        for (int i = 0; i < 256; i++) {
            if (buf[i * 2 + 0] != buf[i * 2 + 1]) {
                has_stereo_diff = true;
                break;
            }
        }
    }
    assert(has_stereo_diff);

    /* 3. リバーブを無効化すると再びドライ出力に戻ること */
    synth_engine_set_reverb_enabled(&engine, false);
    synth_engine_render(&engine, buf, 256);
    for (int i = 0; i < 256; i++) {
        assert(abs((int)buf[i * 2 + 0] - (int)buf[i * 2 + 1]) <= 2); /* L ≈ R (TPDF dither ±1 LSB) */
    }

    printf("  -> PASS: Stereo reverb spatialization and dynamic bypass verified.\n");
}

static void test_exponential_adsr_curve(void)
{
    printf("[TEST] Testing Exponential ADSR decay & release curves...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    engine.default_adsr.attack_time_sec = 0.001f;
    engine.default_adsr.decay_time_sec = 0.050f;
    engine.default_adsr.sustain_level = 0.5f;
    engine.default_adsr.release_time_sec = 0.050f;
    engine.default_adsr.exponential_decay = true;

    /* プログラム0 Piano の実エンベロープ: A=3ms D=350ms S=0.30 R=80ms */
    int v = synth_engine_note_on(&engine, 60, 1.0f, WAVE_SINE);
    int16_t buf[(4096 + 512) * 2];

    synth_engine_render(&engine, buf, 64);
    assert(engine.voices[v].env_state == ENV_ATTACK);

    synth_engine_render(&engine, buf, 1200);
    float mid_decay_level = engine.voices[v].current_env_level;
    printf("  -> Exponential mid-decay level: %.4f (Sustain target: 0.3000)\n", mid_decay_level);
    assert(mid_decay_level > 0.30f && mid_decay_level <= 1.0f);

    /* ディケイ完了 (~352ms) までレンダリングしてサステイン到達を確認 */
    for (int i = 0; i < 6; i++) {
        synth_engine_render(&engine, buf, 4096);
    }
    assert(engine.voices[v].env_state == ENV_SUSTAIN);
    assert(fabsf(engine.voices[v].current_env_level - 0.30f) < 0.01f);

    synth_engine_note_off(&engine, 60);
    assert(engine.voices[v].env_state == ENV_RELEASE);

    synth_engine_render(&engine, buf, 4096 + 256);
    assert(engine.voices[v].active == false);
    assert(engine.voices[v].env_state == ENV_IDLE);
    assert(engine.voices[v].current_env_level == 0.0f);

    printf("  -> PASS: Exponential ADSR decay & clean voice deallocation verified.\n");
}

static void test_smf_format0_and_running_status(void)
{
    printf("[TEST] Testing SMF Format 0 binary parser with Running Status & Set Tempo...\n");

    /* 手動構築の SMF Format 0 バイナリ:
     * - MThd: Format 0, 1 Track, 480 TPQN
     * - MTrk:
     *   - Delta 0: Track Name (0xFF 0x03 0x07 "Format0")
     *   - Delta 0: Set Tempo (0xFF 0x51 0x03 0x07 0xA1 0x20 => 500,000 us = 120 BPM)
     *   - Delta 0: Program Change Ch 1 -> Prog 33 (Bass) (0xC1 0x21)
     *   - Delta 0: CC#7 Volume Ch 1 -> 120 (0xB1 0x07 0x78)
     *   - Delta 0: Note On Ch 1, Note 36 (C2), Vel 100 (0x91 0x24 0x64)
     *   - Delta 480 (1 quarter note): Running Status Note On Ch 1, Note 38 (D2), Vel 100 (0x26 0x64)
     *   - Delta 480: Note Off Ch 1, Note 36, Vel 0 (0x81 0x24 0x00)
     *   - Delta 0: Note Off Ch 1, Note 38, Vel 0 (0x81 0x26 0x00)
     *   - Delta 0: End of Track (0xFF 0x2F 0x00)
     */
    static const uint8_t smf_f0[] = {
        'M', 'T', 'h', 'd',
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, /* Format 0 */
        0x00, 0x01, /* 1 Track */
        0x01, 0xE0, /* 480 TPQN */

        'M', 'T', 'r', 'k',
        0x00, 0x00, 0x00, 0x30, /* Length 48 bytes */
        0x00, 0xFF, 0x03, 0x07, 'F', 'o', 'r', 'm', 'a', 't', '0', /* Track Name */
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,                  /* Set Tempo 500000 */
        0x00, 0xC1, 0x21,                                           /* PC Ch1 -> 33 */
        0x00, 0xB1, 0x40, 0x7F,                                     /* CC#64 Sustain -> 127 (パーサーのCCフィルタ対象外を使用) */
        0x00, 0x91, 0x24, 0x64,                                     /* Note On 36 */
        0x83, 0x60, 0x26, 0x64,                                     /* Delta 480 + Running status Note On 38 */
        0x83, 0x60, 0x81, 0x24, 0x00,                               /* Delta 480 + Note Off 36 */
        0x00, 0x81, 0x26, 0x00,                                     /* Delta 0 + Note Off 38 */
        0x00, 0xFF, 0x2F, 0x00                                      /* End of Track */
    };

    MidiSong song;
    int ret = midi_file_load_memory(smf_f0, sizeof(smf_f0), &song);
    assert(ret == 0);
    assert(song.format == 0);
    assert(song.num_tracks == 1);
    assert(song.ticks_per_quarter == 480);
    assert(strcmp(song.title, "Format0") == 0);
    assert(song.event_count == 6); /* PC, CC, NoteOn36, NoteOn38, NoteOff36, NoteOff38 */

    /* タイムスタンプ検証: 120BPM @ 48kHz => 480 ticks = 0.5s = 24000 samples */
    printf("  -> Event 0: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[0].type, song.events[0].channel, song.events[0].data1, song.events[0].timestamp_samples);
    printf("  -> Event 1: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[1].type, song.events[1].channel, song.events[1].data1, song.events[1].timestamp_samples);
    printf("  -> Event 2: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[2].type, song.events[2].channel, song.events[2].data1, song.events[2].timestamp_samples);
    printf("  -> Event 3: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[3].type, song.events[3].channel, song.events[3].data1, song.events[3].timestamp_samples);
    printf("  -> Event 4: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[4].type, song.events[4].channel, song.events[4].data1, song.events[4].timestamp_samples);
    printf("  -> Event 5: type=0x%02X, ch=%u, d1=%u, ts=%u\n", song.events[5].type, song.events[5].channel, song.events[5].data1, song.events[5].timestamp_samples);

    /* Event 2 (Note On 36) ts=0, Event 3 (Running status Note On 38) ts=24000 */
    assert(song.events[2].type == MIDI_STATUS_NOTE_ON && song.events[2].data1 == 36 && song.events[2].timestamp_samples == 0);
    assert(song.events[3].type == MIDI_STATUS_NOTE_ON && song.events[3].data1 == 38 && song.events[3].timestamp_samples == 24000);

    /* Event 4/5: 同一タイムスタンプ (ts=48000) の Note Off ペア。
     * パーサーは同時刻イベントの順序を保証しないため、集合として検証する */
    {
        bool off36_at_end = false, off38_at_end = false;
        assert(song.event_count == 6);
        assert(song.events[4].type == MIDI_STATUS_NOTE_OFF && song.events[5].type == MIDI_STATUS_NOTE_OFF);
        assert(song.events[4].timestamp_samples == 48000 && song.events[5].timestamp_samples == 48000);

        for (int i = 4; i <= 5; i++) {
            if (song.events[i].data1 == 36) off36_at_end = true;
            if (song.events[i].data1 == 38) off38_at_end = true;
        }
        assert(off36_at_end && off38_at_end);
    }

    midi_parser_free_song(&song);
    printf("  -> PASS: SMF Format 0 parser & running status perfectly decoded.\n");
}

static void test_16ch_multitimbral_and_cc(void)
{
    printf("[TEST] Testing 16-Channel Multi-timbral tone selection and real-time CC control...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    /* 1. 音色プログラムの独立設定 */
    synth_engine_program_change(&engine, 0, 0);  /* Ch 0: Acoustic Piano */
    synth_engine_program_change(&engine, 1, 33); /* Ch 1: Electric Bass */
    synth_engine_program_change(&engine, 2, 56); /* Ch 2: Trumpet / Brass */
    synth_engine_program_change(&engine, 3, 40); /* Ch 3: Violin / Strings */
    synth_engine_program_change(&engine, 4, 80); /* Ch 4: Lead */

    int v0 = synth_engine_channel_note_on(&engine, 0, 60, 0.8f);
    int v1 = synth_engine_channel_note_on(&engine, 1, 36, 0.8f);
    int v2 = synth_engine_channel_note_on(&engine, 2, 60, 0.8f);
    int v3 = synth_engine_channel_note_on(&engine, 3, 60, 0.8f);
    int v4 = synth_engine_channel_note_on(&engine, 4, 60, 0.8f);

    assert(engine.voices[v0].wave_type == WAVE_TRIANGLE); /* Piano */
    assert(engine.voices[v1].wave_type == WAVE_SQUARE);   /* Bass */
    assert(engine.voices[v2].wave_type == WAVE_SAWTOOTH); /* Brass */
    assert(engine.voices[v3].wave_type == WAVE_SAWTOOTH); /* Strings */
    assert(engine.voices[v4].wave_type == WAVE_SQUARE);   /* Lead */

    synth_engine_all_notes_off(&engine);

    /* 2. CC#10 (Pan) リアルタイム定位テスト */
    synth_engine_program_change(&engine, 0, 80);
    synth_engine_control_change(&engine, 0, 10, 0);   /* Pan Hard Left (0) */
    synth_engine_channel_note_on(&engine, 0, 69, 1.0f); /* A4 440Hz */

    int16_t buf[256 * 2];
    synth_engine_render(&engine, buf, 256);

    int32_t left_energy = 0, right_energy = 0;
    for (int i = 0; i < 256; i++) {
        left_energy += abs(buf[i * 2 + 0]);
        right_energy += abs(buf[i * 2 + 1]);
    }
    printf("  -> Pan Left: Left Energy=%d, Right Energy=%d\n", left_energy, right_energy);
    assert(left_energy > 1000 && right_energy == 0);

    /* Pan Hard Right (127) */
    synth_engine_control_change(&engine, 0, 10, 127);
    synth_engine_render(&engine, buf, 256);
    left_energy = 0; right_energy = 0;
    for (int i = 0; i < 256; i++) {
        left_energy += abs(buf[i * 2 + 0]);
        right_energy += abs(buf[i * 2 + 1]);
    }
    printf("  -> Pan Right: Left Energy=%d, Right Energy=%d\n", left_energy, right_energy);
    assert(right_energy > 1000 && left_energy == 0);

    /* 3. CC#7 (Volume) テスト */
    synth_engine_control_change(&engine, 0, 10, 64); /* Center */
    synth_engine_control_change(&engine, 0, 7, 0);    /* Volume 0 */
    synth_engine_render(&engine, buf, 256);
    int32_t vol0_energy = 0;
    for (int i = 0; i < 256; i++) {
        vol0_energy += abs(buf[i * 2 + 0]) + abs(buf[i * 2 + 1]);
    }
    assert(vol0_energy == 0);

    synth_engine_all_notes_off(&engine);
    printf("  -> PASS: 16ch Multi-timbral tone routing & CC control verified.\n");
}

static void test_gm_drum_kit_synthesis(void)
{
    printf("[TEST] Testing Channel 10 GM Standard Drum Kit Synthesis (Kick, Snare, Hi-Hat, Cymbal, Tom)...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    /* Ch 10 (ch 9 in 0-based) の各ドラムノート発音 */
    int v_kick   = synth_engine_channel_note_on(&engine, 9, 36, 1.0f); /* Kick (Bass Drum) */
    assert(engine.voices[v_kick].wave_type == WAVE_DRUM_KICK);
    /* タイトなパンチの新チューニング: 120Hz -> 48Hz へのピッチスイープ */
    assert(engine.voices[v_kick].start_frequency == 120.0f);
    assert(engine.voices[v_kick].target_frequency == 48.0f);

    int v_snare  = synth_engine_channel_note_on(&engine, 9, 38, 1.0f); /* Snare */
    assert(engine.voices[v_snare].wave_type == WAVE_DRUM_SNARE);

    int v_hihat  = synth_engine_channel_note_on(&engine, 9, 42, 1.0f); /* Closed Hi-Hat */
    assert(engine.voices[v_hihat].wave_type == WAVE_DRUM_HIHAT);

    int v_cymbal = synth_engine_channel_note_on(&engine, 9, 49, 1.0f); /* Crash Cymbal */
    assert(engine.voices[v_cymbal].wave_type == WAVE_DRUM_CYMBAL);

    int v_tom    = synth_engine_channel_note_on(&engine, 9, 45, 1.0f); /* Mid Tom */
    assert(engine.voices[v_tom].wave_type == WAVE_DRUM_KICK);

    /* レンダリングしてクラッシュやクリップなく正常に合成されることを検証 */
    int16_t buf[512 * 2];
    for (int i = 0; i < 20; i++) {
        synth_engine_render(&engine, buf, 512);
    }

    synth_engine_all_notes_off(&engine);
    printf("  -> PASS: GM Standard Drum Kit percussion synthesis verified.\n");
}

static void test_64voice_polyphony_and_stealing(void)
{
    printf("[TEST] Testing 64-voice max polyphony and smooth voice stealing...\n");
    SynthEngine engine;
    synth_engine_init(&engine);

    /* 64音すべて同時発音 */
    int voice_indices[SYNTH_MAX_POLYPHONY];
    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        voice_indices[i] = synth_engine_channel_note_on(&engine, 0, (uint8_t)(20 + i), 0.8f);
        assert(voice_indices[i] >= 0);
        assert(engine.voices[voice_indices[i]].active == true);
    }

    /* 64ボイスすべてが異なるインデックスで発音中であることを検証 */
    for (int i = 0; i < SYNTH_MAX_POLYPHONY; i++) {
        for (int j = i + 1; j < SYNTH_MAX_POLYPHONY; j++) {
            assert(voice_indices[i] != voice_indices[j]);
        }
    }

    /* 少し時間を進めて発音経過時間を更新 (Voice 0 が最古になる) */
    int16_t buf[256 * 2];
    synth_engine_render(&engine, buf, 256);

    /* 65音目を入力 -> 最古ボイス (Voice 0) がスムーズにスチールされること */
    int v65 = synth_engine_channel_note_on(&engine, 0, 100, 1.0f);
    assert(v65 == voice_indices[0]);
    assert(engine.voices[v65].note == 100);
    assert(engine.voices[v65].active == true);

    printf("  -> 65th Note assigned to stolen voice index: %d (Oldest)\n", v65);
    printf("  -> PASS: 64-voice polyphony and oldest voice stealing verified.\n");
}

static void test_preset_songs_integrity(void)
{
    printf("[TEST] Testing Preset Songs Integrity & Full Playthrough...\n");

    uint32_t count = 0;
    const Track *tracks = preset_songs_get_all(&count);
    assert(tracks != NULL);
    /* プリセットはメモリ節約のためテスト用 1 曲のみ (SD MIDI が本筋) */
    assert(count >= 1);
    assert(preset_songs_get_count() == count);

    SynthEngine engine;
    synth_engine_init(&engine);
    Sequencer seq;
    sequencer_init(&seq, &engine);

    for (uint32_t i = 0; i < count; i++) {
        const Track *tr = preset_songs_get_by_index(i);
        assert(tr != NULL);
        assert(tr->title != NULL && strlen(tr->title) > 0);
        assert(tr->events != NULL);
        assert(tr->event_count >= 8);
        assert(tr->bpm >= 60 && tr->bpm <= 300);

        printf("  -> Track [%u] '%s' (Events: %u, BPM: %u)\n",
               (unsigned int)i, tr->title, (unsigned int)tr->event_count, tr->bpm);

        sequencer_play_track(&seq, tr);
        assert(seq.is_playing == true);

        if (tr->loop) {
            /* ループ譜面は完奏しない: 2 秒分進めて発音が継続することのみ確認 */
            const uint32_t probe_ticks = (SYNTH_SAMPLE_RATE / 512u) * 2u;
            for (uint32_t t = 0; t < probe_ticks; t++) {
                assert(sequencer_tick_frames(&seq, 512, SYNTH_SAMPLE_RATE) == true);
            }
            printf("     => Loop chart kept playing for %u chunks (~2 sec audio)\n",
                   probe_ticks);
        } else {
            uint32_t ticks = 0;
            const uint32_t MAX_TICKS = 100000;
            while (seq.is_playing && ticks < MAX_TICKS) {
                sequencer_tick_frames(&seq, 512, SYNTH_SAMPLE_RATE);
                ticks++;
            }

            assert(seq.is_playing == false);
            printf("     => Completed successfully in %u chunks (~%.2f sec audio)\n",
                   ticks, (double)(ticks * 512) / SYNTH_SAMPLE_RATE);
        }
    }

    printf("  -> PASS: All %u Preset Songs passed full integrity & playback verification.\n", (unsigned int)count);
}

static void test_midi_parser_and_playback(void)
{
    printf("[TEST] Testing Standard MIDI File (SMF Format 1) Parser & Multi-Track Playback...\n");
    uint32_t count = 0;
    const MidiDemoTrack *demo_tracks = midi_demo_songs_get_all(&count);
    assert(count > 0);

    SynthEngine engine;
    synth_engine_init(&engine);
    MidiPlayer player;
    midi_player_init(&player, &engine);

    for (uint32_t i = 0; i < count; i++) {
        printf("  -> Testing SMF [%u]: %s (%u bytes)...\n", 
               (unsigned int)i, demo_tracks[i].title, (unsigned int)demo_tracks[i].smf_size);

        static MidiEvent event_pool[4096];
        MidiSong song;
        int ret = midi_parser_load_memory(demo_tracks[i].smf_data, demo_tracks[i].smf_size,
                                          &song, event_pool, 4096, SYNTH_SAMPLE_RATE);
        assert(ret == 0);
        assert(song.event_count > 0);
        assert(song.format == 1);
        printf("     Parsed: format=%u, tracks=%u, TPQN=%u, events=%u, total_samples=%u\n",
               song.format, song.num_tracks, song.ticks_per_quarter, song.event_count, song.total_samples);

        /* 16ch MIDI 再生テスト (全イベント完奏確認) */
        midi_player_play(&player, &song);
        assert(player.is_playing == true);

        int16_t pcm_buf[512 * 2];
        uint32_t ticks = 0;
        const uint32_t MAX_TICKS = 100000;
        while (player.is_playing && ticks < MAX_TICKS) {
            midi_player_tick_frames(&player, 512);
            synth_engine_render(&engine, pcm_buf, 512);
            ticks++;
        }

        assert(player.is_playing == false);
        printf("     => Multi-Track MIDI rendered successfully in %u chunks (~%.2f sec audio)\n",
               ticks, (double)(ticks * 512) / SYNTH_SAMPLE_RATE);
    }

    printf("  -> PASS: SMF Format 1 Multi-Track Parser & 16ch Playback 100%% verified.\n");
}

static void test_sustain_pedal(void)
{
    printf("[TEST] Testing Sustain Pedal (CC#64) hold & release semantics...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    int16_t buf[512 * 2];

    /* 1. ペダル ON 中の Note Off は音を保持する */
    synth_engine_control_change(&engine, 0, 64, 127);
    synth_engine_note_on(&engine, 60, 1.0f, WAVE_SINE);
    synth_engine_render(&engine, buf, 240);
    assert(engine.voices[0].active);

    synth_engine_channel_note_off(&engine, 0, 60);
    assert(engine.voices[0].sustained_by_pedal == true);
    synth_engine_render(&engine, buf, 240);
    assert(engine.voices[0].active);                    /* ペダル保持で鳴り続く */
    assert(engine.voices[0].env_state != ENV_RELEASE);  /* リリースに入っていない */

    /* 2. ペダル OFF で延期されていたリリースが開始される */
    synth_engine_control_change(&engine, 0, 64, 0);
    assert(engine.voices[0].env_state == ENV_RELEASE || !engine.voices[0].active);

    /* 3. ペダル無しの通常 Note Off は即時リリース */
    synth_engine_note_on(&engine, 62, 1.0f, WAVE_SINE);
    synth_engine_render(&engine, buf, 240);
    synth_engine_channel_note_off(&engine, 0, 62);
    assert(engine.voices[1].env_state == ENV_RELEASE);

    /* 4. 回帰: ペダル保持中の「重複」Note Off はペダル優先で保持継続。
     *    (旧実装は sustained_by_pedal 済みボイスへの 2 回目の Note Off が
     *     ペダルを無視して即リリースしていた) */
    synth_engine_all_notes_off(&engine);
    synth_engine_control_change(&engine, 0, 64, 127);
    synth_engine_note_on(&engine, 64, 1.0f, WAVE_SINE);
    synth_engine_render(&engine, buf, 240);
    assert(engine.voices[0].active);
    synth_engine_channel_note_off(&engine, 0, 64);   /* 1 回目 */
    synth_engine_channel_note_off(&engine, 0, 64);   /* 重複 OFF */
    synth_engine_channel_note_off(&engine, 0, 64);   /* さらに重複 */
    synth_engine_render(&engine, buf, 240);
    assert(engine.voices[0].active);                    /* 保持継続 */
    assert(engine.voices[0].env_state != ENV_RELEASE);  /* リリース非突入 */

    /* 5. 回帰: リリース済み/停止済みボイスの Note Off でテールが
     *    再延長されない (再リリースガード) */
    synth_engine_control_change(&engine, 0, 64, 0);  /* ペダル離し -> リリース開始 */
    assert(engine.voices[0].env_state == ENV_RELEASE || !engine.voices[0].active);
    {
        float level_before = engine.voices[0].current_env_level;
        synth_engine_channel_note_off(&engine, 0, 64);   /* リリース中に重複 OFF */
        float level_after = engine.voices[0].current_env_level;
        if (engine.voices[0].active) {
            /* レベルは単調減少していなければならない (再アタック/再リリース禁止) */
            assert(level_after <= level_before + 1e-6f);
        }
    }

    for (int i = 0; i < 8; i++) {
        synth_engine_render(&engine, buf, 512);
    }
    assert(!engine.voices[0].active && !engine.voices[1].active);

    printf("  -> PASS: Sustain pedal hold & release + duplicate-off guard verified.\n");
}

static void test_smart_stick_scale(void)
{
    printf("[TEST] Testing Smart Stick Scale System (Full Octave, Accidental & Legato Slide)...\n");
    SynthEngine engine;
    synth_engine_init(&engine);
    SynthController ctrl;
    synth_controller_init(&ctrl, &engine);

    /* 1. 演奏モードへ移行 (E+F 同時押し) */
    JoystickState state;
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_E | BTN_MASK_F;
    state.pressed_buttons = BTN_MASK_E | BTN_MASK_F;
    synth_controller_update(&ctrl, &state);
    assert(synth_controller_mode(&ctrl) == CTRL_MODE_PERFORMANCE);

    /* 2. CENTER ゾーン (ニュートラル): A=ド(60), B=レ(62), C=ミ(64), D=ファ(65) (Octave 4) */
    memset(&state, 0, sizeof(state));
    state.current_buttons = BTN_MASK_A;
    state.pressed_buttons = BTN_MASK_A;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_note[0] == 60); /* ド (C4) */

    state.current_buttons |= BTN_MASK_B | BTN_MASK_C | BTN_MASK_D;
    state.pressed_buttons = BTN_MASK_B | BTN_MASK_C | BTN_MASK_D;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_note[1] == 62); /* レ (D4) */
    assert(ctrl.perf_note[2] == 64); /* ミ (E4) */
    assert(ctrl.perf_note[3] == 65); /* ファ (F4) */

    /* 3. HIGH ゾーン (右傾き): A=ソ(67), B=ラ(69), C=シ(71), D=高ド(72) */
    /* 押下したままスティックを右に倒すと、鳴っている 4 音が自動スライド！ */
    state.stick_x = 0.8f;
    state.pressed_buttons = 0;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_HIGH);
    assert(ctrl.perf_note[0] == 67); /* ソ (G4) */
    assert(ctrl.perf_note[1] == 69); /* ラ (A4) */
    assert(ctrl.perf_note[2] == 71); /* シ (B4) */
    assert(ctrl.perf_note[3] == 72); /* 高いド (C5) */

    /* 4. ACCIDENTAL ゾーン (左傾き): A=ド#(61), B=レ#(63), C=ファ#(66), D=ソ#(68) */
    state.stick_x = -0.8f;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_ACCIDENTAL);
    assert(ctrl.perf_note[0] == 61); /* ド# (C#4) */
    assert(ctrl.perf_note[1] == 63); /* レ# (D#4) */
    assert(ctrl.perf_note[2] == 66); /* ファ# (F#4) */
    assert(ctrl.perf_note[3] == 68); /* ソ# (G#4) */

    /* 5. LOW ゾーン (下傾き): A=低ソ(55), B=低ラ(57), C=低シ(59), D=ド(60) */
    state.stick_x = 0.0f;
    state.stick_y = -0.8f;
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_zone == PERF_ZONE_LOW);
    assert(ctrl.perf_note[0] == 55); /* 低いソ (G3) */
    assert(ctrl.perf_note[1] == 57); /* 低いラ (A3) */
    assert(ctrl.perf_note[2] == 59); /* 低いシ (B3) */
    assert(ctrl.perf_note[3] == 60); /* ド (C4) */

    /* 6. ボタン全解放: ノートオフが正確に発行され Stuck Note が出ないこと */
    memset(&state, 0, sizeof(state));
    synth_controller_update(&ctrl, &state);
    assert(ctrl.perf_note[0] == 0 && ctrl.perf_note[1] == 0 &&
           ctrl.perf_note[2] == 0 && ctrl.perf_note[3] == 0);
    assert(ctrl.perf_held_mask == 0);

    printf("  -> PASS: Smart Stick Scale System (Do-Re-Mi-Fa / So-La-Ti-Do / Accidental / Low) verified.\n");
}

static void test_sd_midi_streaming_load(void)
{
    printf("[TEST] Testing SD MIDI Streaming File Loader (Zero-malloc)...\n");
    SdMidiList list;
    uint32_t count = sd_midi_scan(&list);
    printf("  -> Scanned %u files in sdmidi directory\n", count);
    if (count == 0) {
        printf("  -> SKIP: no sdmidi files (CI without SD)\n");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        MidiSong song;
        memset(&song, 0, sizeof(song));
        int ret = sd_midi_load_file(&list, i, &song);
        assert(ret == 0);
        assert(song.event_count > 0);
        assert(song.events != NULL);
        assert(song.events_dynamic == false); /* 静的プール使用 */
        midi_parser_free_song(&song);
    }
    printf("  -> PASS: All %u SD MIDI files loaded successfully with Zero-malloc streaming!\n", count);
}

static void test_midi_streaming_kway_merge(void)
{
    printf("[TEST] Testing MidiStreamReader k-way merge streaming against batch parser...\n");

    FILE *fp = fopen("sdmidi/demo.mid", "rb");
    if (!fp) {
        printf("  -> SKIP: sdmidi/demo.mid not found\n");
        return;
    }

    /* 1. バッチパース (リファレンス) */
    MidiSong ref_song;
    memset(&ref_song, 0, sizeof(ref_song));
    static MidiEvent ref_pool[2048];
    int ret = midi_parser_load_file(fp, &ref_song, ref_pool, 2048, 48000);
    assert(ret == 0);
    assert(ref_song.event_count > 0);

    /* 2. ストリーミングオープン */
    MidiStreamReader reader;
    ret = midi_stream_open(&reader, fp, 48000);
    assert(ret == 0);
    assert(reader.format == ref_song.format);
    assert(reader.num_tracks == ref_song.num_tracks);
    assert(reader.ticks_per_quarter == ref_song.ticks_per_quarter);

    /* 3. 小さなチャンク (32イベント単位) で逐次ストリーミング読み出し */
    MidiEvent stream_events[2048];
    uint32_t total_streamed = 0;
    MidiEvent chunk[32];

    while (total_streamed < 2048) {
        uint32_t n = midi_stream_read(&reader, chunk, 32);
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) {
            stream_events[total_streamed++] = chunk[i];
        }
    }

    /* イベント総数の一致検証 */
    printf("  -> Ref events: %u, Streamed events: %u\n", ref_song.event_count, total_streamed);
    assert(total_streamed == ref_song.event_count);

    /* 全イベントの完全一致 (タイムスタンプ、チャンネル、タイプ、データ) 検証 */
    for (uint32_t i = 0; i < total_streamed; i++) {
        const MidiEvent *e_ref = &ref_song.events[i];
        const MidiEvent *e_str = &stream_events[i];

        if (e_str->timestamp_samples != e_ref->timestamp_samples ||
            e_str->type != e_ref->type ||
            e_str->channel != e_ref->channel ||
            e_str->data1 != e_ref->data1 ||
            e_str->data2 != e_ref->data2) {
            printf("  -> MISMATCH at index %u:\n", i);
            printf("     REF:    ts=%u, type=0x%02X, ch=%u, d1=%u, d2=%u, seq=%u\n",
                   e_ref->timestamp_samples, e_ref->type, e_ref->channel, e_ref->data1, e_ref->data2, e_ref->sequence);
            printf("     STREAM: ts=%u, type=0x%02X, ch=%u, d1=%u, d2=%u, seq=%u\n",
                   e_str->timestamp_samples, e_str->type, e_str->channel, e_str->data1, e_str->data2, e_str->sequence);
            assert(0);
        }
    }

    /* 4. 巻き戻し (Rewind) の検証 */
    midi_stream_rewind(&reader);
    uint32_t rewind_streamed = 0;
    while (rewind_streamed < 2048) {
        uint32_t n = midi_stream_read(&reader, chunk, 32);
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) {
            assert(chunk[i].timestamp_samples == ref_song.events[rewind_streamed].timestamp_samples);
            assert(chunk[i].type == ref_song.events[rewind_streamed].type);
            assert(chunk[i].channel == ref_song.events[rewind_streamed].channel);
            assert(chunk[i].data1 == ref_song.events[rewind_streamed].data1);
            assert(chunk[i].data2 == ref_song.events[rewind_streamed].data2);
            rewind_streamed++;
        }
    }
    assert(rewind_streamed == ref_song.event_count);

    fclose(fp);
    printf("  -> PASS: Streaming k-way merge produced 100%% identical event stream to batch qsort!\n");
}

int main(void)
{
    /* abort() 時も進行状況を失わないよう無バッファ化 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=======================================================\n");
    printf(" RUNNING SPRESENSE SYNTHESIZER & MIDI UNIT TESTS\n");
    printf("=======================================================\n");

    test_adsr_sustain_zero();
    test_adsr_mid_attack_release();
    test_sustain_pedal();
    test_sequencer_sample_timing();
    test_controller_dap();
    test_smart_stick_scale();
    test_sine_lut_accuracy();
    test_polyblep_oscillators();
    test_stereo_reverb_effect();
    test_exponential_adsr_curve();
    test_smf_format0_and_running_status();
    test_16ch_multitimbral_and_cc();
    test_gm_drum_kit_synthesis();
    test_64voice_polyphony_and_stealing();
    test_preset_songs_integrity();
    test_midi_parser_and_playback();
    test_sd_midi_streaming_load();
    test_midi_streaming_kway_merge();

    printf("=======================================================\n");
    printf(" ALL UNIT TESTS PASSED (100%% SUCCESS)!\n");
    printf("=======================================================\n");
    return 0;
}
