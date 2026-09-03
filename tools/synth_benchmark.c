/**
 * @file synth_benchmark.c
 * @brief Sony Spresense シンセサイザーエンジン ベンチマーク測定＆音響解析データ生成ハーネス
 * @details PolyBLEPエイリアシング低減度測定、Sine LUT vs sinf性能比較、
 *          ステレオリバーブ空間減衰・インパルス応答WAV生成、およびCortex-M4F 156MHz負荷率推定
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "synth_engine.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846f)
#endif

/* WAVヘッダー構造体 (44バイト標準PCM) */
#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];        /* "RIFF" */
    uint32_t riff_size;
    char     wave_id[4];        /* "WAVE" */
    char     fmt_id[4];         /* "fmt " */
    uint32_t fmt_size;          /* 16 */
    uint16_t audio_format;      /* 1 (PCM) */
    uint16_t num_channels;      /* 2 */
    uint32_t sample_rate;       /* 48000 */
    uint32_t byte_rate;         /* 48000 * 2 * 2 = 192000 */
    uint16_t block_align;       /* 4 */
    uint16_t bits_per_sample;   /* 16 */
    char     data_id[4];        /* "data" */
    uint32_t data_size;
} WavHeader;
#pragma pack(pop)

/* 高精度タイマーユーティリティ (マイクロ秒単位) */
static double get_time_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
#endif
}

/* WAVファイル書き込みヘルパー */
static bool write_wav_file(const char *filename, const int16_t *buffer, uint32_t frames)
{
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open %s for writing\n", filename);
        return false;
    }

    uint32_t data_bytes = frames * SYNTH_CHANNELS * sizeof(int16_t);
    WavHeader hdr;
    memcpy(hdr.riff_id, "RIFF", 4);
    hdr.riff_size = sizeof(WavHeader) - 8 + data_bytes;
    memcpy(hdr.wave_id, "WAVE", 4);
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = SYNTH_CHANNELS;
    hdr.sample_rate = SYNTH_SAMPLE_RATE;
    hdr.byte_rate = SYNTH_SAMPLE_RATE * SYNTH_CHANNELS * sizeof(int16_t);
    hdr.block_align = SYNTH_CHANNELS * sizeof(int16_t);
    hdr.bits_per_sample = SYNTH_BITS_PER_SAMPLE;
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_bytes;

    fwrite(&hdr, sizeof(WavHeader), 1, fp);
    fwrite(buffer, sizeof(int16_t) * SYNTH_CHANNELS, frames, fp);
    fclose(fp);
    return true;
}

/* 波形名の文字列取得 */
static const char* get_wave_name(WaveType type)
{
    switch (type) {
        case WAVE_SINE:     return "Sine (LUT)";
        case WAVE_SQUARE:   return "Square (BLEP)";
        case WAVE_SAWTOOTH: return "Saw (BLEP)";
        case WAVE_TRIANGLE: return "Triangle";
        case WAVE_NOISE:    return "Noise";
        default:            return "Unknown";
    }
}

/**
 * @brief Spresense (ARM Cortex-M4F @ 156MHz) 上でのCPU負荷率推定
 * @details 1フレームあたりの想定サイクル数から負荷率(%)を算出
 * 
 * Cortex-M4F 156MHz での想定クロックサイクル数モデル:
 * - 基本レンダリングオーバーヘッド: ~15 サイクル
 * - ボイスあたり基本処理 (ADSR状態更新, 位相更新, mix加算): ~18 サイクル
 * - オシレータ別処理:
 *   - SINE (LUT + 線形補間): ~8 サイクル (旧 sinf() は ~70 サイクル)
 *   - SQUARE (PolyBLEP): ~14 サイクル (旧 ナイーブは ~6 サイクル)
 *   - SAWTOOTH (PolyBLEP): ~10 サイクル (旧 ナイーブは ~5 サイクル)
 *   - TRIANGLE (PolyBLAMP): ~12 サイクル (旧 ナイーブは ~8 サイクル)
 *   - NOISE (LFSR): ~10 サイクル
 * - ステレオリバーブ DSP (Comb 4本 + Allpass 2本 x 2ch): ~65 サイクル (フレーム全体で1回)
 */
static double estimate_m4f_cpu_load(int polyphony, WaveType wave_type, bool reverb_on)
{
    double cycles_base = 15.0;
    double voice_base = 18.0;
    double osc_cycles = 0.0;

    switch (wave_type) {
        case WAVE_SINE:     osc_cycles = 8.0;  break;
        case WAVE_SQUARE:   osc_cycles = 14.0; break;
        case WAVE_SAWTOOTH: osc_cycles = 10.0; break;
        case WAVE_TRIANGLE: osc_cycles = 12.0; break;
        case WAVE_NOISE:    osc_cycles = 10.0; break;
        default:            osc_cycles = 15.0; break;
    }

    double reverb_cycles = reverb_on ? 65.0 : 0.0;
    double cycles_per_frame = cycles_base + reverb_cycles + (double)polyphony * (voice_base + osc_cycles);
    double total_cycles_per_sec = cycles_per_frame * (double)SYNTH_SAMPLE_RATE;
    double m4f_clock_freq = 156000000.0; /* 156 MHz */

    double cpu_load_pct = (total_cycles_per_sec / m4f_clock_freq) * 100.0;
    return cpu_load_pct;
}

/* ========================================================================= */
/* 比較用 ナイーブ波形レンダラ (旧エンジン動作のエミュレーション)             */
/* ========================================================================= */

static inline float naive_oscillator(WaveType type, float phase)
{
    switch (type) {
        case WAVE_SINE:
            return sinf(phase * 2.0f * (float)M_PI);
        case WAVE_SQUARE:
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case WAVE_SAWTOOTH:
            return (2.0f * phase) - 1.0f;
        case WAVE_TRIANGLE:
            return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
        default:
            return 0.0f;
    }
}

static void render_naive_tone(int16_t *buffer, uint32_t frames, float freq, WaveType wave_type, float master_vol)
{
    float phase = 0.0f;
    float dt = freq / (float)SYNTH_SAMPLE_RATE;

    for (uint32_t i = 0; i < frames; i++) {
        float s = naive_oscillator(wave_type, phase) * master_vol;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t pcm = (int16_t)(s * 32767.0f);
        buffer[i * 2 + 0] = pcm;
        buffer[i * 2 + 1] = pcm;

        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

/**
 * @brief ベンチマーク1: ポリフォニー数 (1, 2, 4, 8) と波形タイプ別のレンダリング性能測定
 */
static void run_rendering_benchmark(void)
{
    printf("\n======================================================================\n");
    printf(" 1. Synth Engine Rendering Benchmark (PolyBLEP & Sine LUT / Cortex-M4F)\n");
    printf("======================================================================\n");
    printf("Audio Settings: %d Hz, %d Channels, %d-bit PCM\n", 
           SYNTH_SAMPLE_RATE, SYNTH_CHANNELS, SYNTH_BITS_PER_SAMPLE);

    const uint32_t test_frames = SYNTH_SAMPLE_RATE * 10; /* 10秒分 */
    const uint32_t chunk_size = 256;
    int16_t *buffer = (int16_t*)malloc(sizeof(int16_t) * chunk_size * SYNTH_CHANNELS);
    if (!buffer) return;

    SynthEngine engine;
    uint8_t test_notes[8] = { 60, 64, 67, 71, 72, 76, 79, 83 }; /* Cmaj7 + テンション */

    WaveType wave_types[] = { WAVE_SINE, WAVE_SQUARE, WAVE_SAWTOOTH, WAVE_TRIANGLE, WAVE_NOISE };
    int polyphony_list[] = { 1, 2, 4, 8 };

    printf("\n%-14s | %-6s | %-16s | %-12s | %-10s | %-18s\n",
           "Waveform", "Voices", "1s Render Time", "Per Frame", "Speed (xRT)", "Est. M4F 156MHz Load");
    printf("---------------+--------+------------------+--------------+------------+---------------------\n");

    for (int w = 0; w < 5; w++) {
        WaveType wave = wave_types[w];
        for (int p = 0; p < 4; p++) {
            int poly = polyphony_list[p];

            synth_engine_init(&engine);
            engine.default_adsr.attack_time_sec = 0.001f;
            engine.default_adsr.decay_time_sec = 0.001f;
            engine.default_adsr.sustain_level = 1.0f;
            engine.default_adsr.release_time_sec = 0.5f;

            for (int v = 0; v < poly; v++) {
                synth_engine_note_on(&engine, test_notes[v], 0.8f, wave);
            }

            synth_engine_render(&engine, buffer, chunk_size);
            double start_t = get_time_us();
            uint32_t frames_rendered = 0;
            while (frames_rendered < test_frames) {
                synth_engine_render(&engine, buffer, chunk_size);
                frames_rendered += chunk_size;
            }
            double end_t = get_time_us();

            double total_time_us = end_t - start_t;
            double time_per_1s_us = total_time_us / 10.0;
            double time_per_frame_ns = (total_time_us * 1000.0) / (double)test_frames;
            double xrt = (10.0 * 1000000.0) / total_time_us;
            double m4f_load = estimate_m4f_cpu_load(poly, wave, false);

            printf("%-14s | %-6d | %11.2f us  | %9.2f ns | %8.1fx   | %16.2f %%\n",
                   get_wave_name(wave), poly, time_per_1s_us, time_per_frame_ns, xrt, m4f_load);
        }
        printf("---------------+--------+------------------+--------------+------------+---------------------\n");
    }

    /* 8ボイス + リバーブ有効時の総合性能測定 */
    {
        synth_engine_init(&engine);
        synth_engine_set_reverb(&engine, true, 0.8f, 0.2f, 0.35f);
        engine.default_adsr.sustain_level = 1.0f;
        for (int v = 0; v < 8; v++) {
            synth_engine_note_on(&engine, test_notes[v], 0.8f, WAVE_SAWTOOTH);
        }
        synth_engine_render(&engine, buffer, chunk_size);

        double start_t = get_time_us();
        uint32_t frames_rendered = 0;
        while (frames_rendered < test_frames) {
            synth_engine_render(&engine, buffer, chunk_size);
            frames_rendered += chunk_size;
        }
        double end_t = get_time_us();

        double total_time_us = end_t - start_t;
        double time_per_1s_us = total_time_us / 10.0;
        double time_per_frame_ns = (total_time_us * 1000.0) / (double)test_frames;
        double xrt = (10.0 * 1000000.0) / total_time_us;
        double m4f_load = estimate_m4f_cpu_load(8, WAVE_SAWTOOTH, true);

        printf("%-14s | %-6d | %11.2f us  | %9.2f ns | %8.1fx   | %16.2f %%\n",
               "Saw8 + Reverb", 8, time_per_1s_us, time_per_frame_ns, xrt, m4f_load);
        printf("---------------+--------+------------------+--------------+------------+---------------------\n");
    }

    free(buffer);
}

/**
 * @brief ベンチマーク2: Sine LUT vs sinf() 浮動小数点計算の8ボイス比較性能測定
 */
static void run_sine_lut_vs_sinf_benchmark(void)
{
    printf("\n======================================================================\n");
    printf(" 2. Sine Wave Performance: 1024-Entry LUT vs math sinf() (8 Voices)\n");
    printf("======================================================================\n");

    const uint32_t test_frames = SYNTH_SAMPLE_RATE * 10; /* 10秒分 */
    const uint32_t chunk_size = 256;
    int16_t *buffer = (int16_t*)malloc(sizeof(int16_t) * chunk_size * SYNTH_CHANNELS);
    if (!buffer) return;

    uint8_t test_notes[8] = { 60, 64, 67, 71, 72, 76, 79, 83 };

    /* 1. 新エンジン: Sine LUT + 線形補間 */
    SynthEngine engine_lut;
    synth_engine_init(&engine_lut);
    engine_lut.default_adsr.sustain_level = 1.0f;
    for (int v = 0; v < 8; v++) {
        synth_engine_note_on(&engine_lut, test_notes[v], 0.8f, WAVE_SINE);
    }
    synth_engine_render(&engine_lut, buffer, chunk_size); /* ウォームアップ */

    double start_lut = get_time_us();
    uint32_t frames = 0;
    while (frames < test_frames) {
        synth_engine_render(&engine_lut, buffer, chunk_size);
        frames += chunk_size;
    }
    double end_lut = get_time_us();
    double time_lut = end_lut - start_lut;

    /* 2. 旧エンジン方式: sinf() 直接呼び出し (8ボイス) */
    double start_sinf = get_time_us();
    frames = 0;
    float phases[8] = {0};
    float dts[8];
    for (int v = 0; v < 8; v++) {
        dts[v] = synth_note_to_freq(test_notes[v]) / (float)SYNTH_SAMPLE_RATE;
    }

    while (frames < test_frames) {
        for (uint32_t i = 0; i < chunk_size; i++) {
            float mix = 0.0f;
            for (int v = 0; v < 8; v++) {
                mix += sinf(phases[v] * 2.0f * (float)M_PI) * 0.8f;
                phases[v] += dts[v];
                if (phases[v] >= 1.0f) phases[v] -= 1.0f;
            }
            mix *= 0.5f;
            if (mix > 1.0f) mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
            int16_t pcm = (int16_t)(mix * 32767.0f);
            buffer[i * 2 + 0] = pcm;
            buffer[i * 2 + 1] = pcm;
        }
        frames += chunk_size;
    }
    double end_sinf = get_time_us();
    double time_sinf = end_sinf - start_sinf;

    double m4f_sinf_load = ((15.0 + 8.0 * (18.0 + 70.0)) * (double)SYNTH_SAMPLE_RATE / 156000000.0) * 100.0;
    double m4f_lut_load  = ((15.0 + 8.0 * (18.0 + 8.0))  * (double)SYNTH_SAMPLE_RATE / 156000000.0) * 100.0;
    double speedup_host  = time_sinf / time_lut;
    double m4f_reduction = (m4f_sinf_load - m4f_lut_load) / m4f_sinf_load * 100.0;

    printf("\n%-24s | %-16s | %-12s | %-10s | %-18s\n",
           "Method", "1s Render Time", "Per Frame", "Speed (xRT)", "Est. M4F 156MHz Load");
    printf("-------------------------+------------------+--------------+------------+---------------------\n");
    printf("%-24s | %11.2f us  | %9.2f ns | %8.1fx   | %16.2f %%\n",
           "Legacy math sinf() (8V)", time_sinf / 10.0, (time_sinf * 1000.0) / test_frames, (10.0 * 1e6) / time_sinf, m4f_sinf_load);
    printf("%-24s | %11.2f us  | %9.2f ns | %8.1fx   | %16.2f %%\n",
           "New 1024-Sine LUT (8V)",   time_lut / 10.0,  (time_lut * 1000.0) / test_frames,  (10.0 * 1e6) / time_lut,  m4f_lut_load);
    printf("-------------------------+------------------+--------------+------------+---------------------\n");
    printf(">>> Host PC Speedup Ratio       : %.2fx Faster\n", speedup_host);
    printf(">>> Spresense M4F CPU Load Drop : %.2f%% -> %.2f%% (%.1f%% CPU Load Reduction)\n",
           m4f_sinf_load, m4f_lut_load, m4f_reduction);

    free(buffer);
}

/**
 * @brief ベンチマーク3: 音質・スペクトル・PolyBLEP比較用波形データの出力 (WAV)
 */
static void generate_spectral_test_waves(void)
{
    printf("\n======================================================================\n");
    printf(" 3. Generating Audio Files for Spectral, FFT & PolyBLEP Analysis\n");
    printf("======================================================================\n");

    const uint32_t duration_sec = 2;
    const uint32_t total_frames = SYNTH_SAMPLE_RATE * duration_sec;
    int16_t *buffer = (int16_t*)malloc(sizeof(int16_t) * total_frames * SYNTH_CHANNELS);
    if (!buffer) return;

    SynthEngine engine;

    /* 1. 標準 440Hz 波形 (Sine, Square, Sawtooth, Triangle, Noise) */
    WaveType wave_types[] = { WAVE_SINE, WAVE_SQUARE, WAVE_SAWTOOTH, WAVE_TRIANGLE, WAVE_NOISE };
    const char *wave_files[] = {
        "test_sine_440.wav",
        "test_square_440.wav",
        "test_sawtooth_440.wav",
        "test_triangle_440.wav",
        "test_noise.wav"
    };

    for (int w = 0; w < 5; w++) {
        synth_engine_init(&engine);
        engine.master_volume = 0.8f;
        engine.default_adsr.attack_time_sec = 0.01f;
        engine.default_adsr.decay_time_sec = 0.01f;
        engine.default_adsr.sustain_level = 1.0f;
        engine.default_adsr.release_time_sec = 0.05f;

        synth_engine_note_on(&engine, 69, 1.0f, wave_types[w]); /* A4 = 440Hz */
        synth_engine_render(&engine, buffer, total_frames);

        write_wav_file(wave_files[w], buffer, total_frames);
        printf("Saved: %-24s (New Engine, 440Hz / %d frames)\n", wave_files[w], total_frames);
    }

    /* 2. PolyBLEP 有効 vs ナイーブ (無効) 比較用 WAV 出力 (440Hz, 880Hz, 1046.5Hz) */
    render_naive_tone(buffer, total_frames, 440.0f, WAVE_SQUARE, 0.8f);
    write_wav_file("test_square_440_naive.wav", buffer, total_frames);
    printf("Saved: %-24s (Legacy Naive Square @ 440Hz)\n", "test_square_440_naive.wav");

    render_naive_tone(buffer, total_frames, 440.0f, WAVE_SAWTOOTH, 0.8f);
    write_wav_file("test_sawtooth_440_naive.wav", buffer, total_frames);
    printf("Saved: %-24s (Legacy Naive Sawtooth @ 440Hz)\n", "test_sawtooth_440_naive.wav");

    /* 高音域 A5 = 880Hz (Note 81) */
    synth_engine_init(&engine);
    engine.master_volume = 0.8f;
    engine.default_adsr.sustain_level = 1.0f;
    synth_engine_note_on(&engine, 81, 1.0f, WAVE_SAWTOOTH);
    synth_engine_render(&engine, buffer, total_frames);
    write_wav_file("test_sawtooth_880_polyblep.wav", buffer, total_frames);
    printf("Saved: %-24s (PolyBLEP Sawtooth @ 880Hz)\n", "test_sawtooth_880_polyblep.wav");

    render_naive_tone(buffer, total_frames, 880.0f, WAVE_SAWTOOTH, 0.8f);
    write_wav_file("test_sawtooth_880_naive.wav", buffer, total_frames);
    printf("Saved: %-24s (Legacy Naive Sawtooth @ 880Hz)\n", "test_sawtooth_880_naive.wav");

    /* 8ボイス最大ポリフォニー時のコード波形 */
    {
        synth_engine_init(&engine);
        synth_engine_set_master_volume(&engine, 0.8f);
        engine.default_adsr.attack_time_sec = 0.05f;
        engine.default_adsr.decay_time_sec = 0.05f;
        engine.default_adsr.sustain_level = 0.8f;
        engine.default_adsr.release_time_sec = 0.2f;

        uint8_t chord[8] = { 48, 55, 60, 64, 67, 71, 74, 79 }; /* Cmaj9 */
        for (int i = 0; i < 8; i++) {
            synth_engine_note_on(&engine, chord[i], 0.8f, WAVE_SAWTOOTH);
        }

        synth_engine_render(&engine, buffer, total_frames);
        write_wav_file("test_poly8_chord.wav", buffer, total_frames);
        printf("Saved: %-24s (8-Voice Polyphony Cmaj9, Sawtooth)\n", "test_poly8_chord.wav");
    }

    free(buffer);
}

/**
 * @brief ベンチマーク4: ステレオリバーブ DSP の空間減衰・インパルス応答WAV生成
 */
static void generate_reverb_test_waves(void)
{
    printf("\n======================================================================\n");
    printf(" 4. Generating Stereo Reverb Audio Files (Impulse Response & Wet/Dry)\n");
    printf("======================================================================\n");

    const uint32_t duration_sec = 3; /* 3秒の残響減衰 */
    const uint32_t total_frames = SYNTH_SAMPLE_RATE * duration_sec;
    int16_t *buffer = (int16_t*)malloc(sizeof(int16_t) * total_frames * SYNTH_CHANNELS);
    if (!buffer) return;

    SynthEngine engine;

    /* 1. リバーブ インパルス応答 (Impulse Response) 測定用 WAV */
    {
        synth_engine_init(&engine);
        synth_engine_set_master_volume(&engine, 1.0f);
        synth_engine_set_reverb(&engine, true, 0.85f, 0.20f, 0.60f); /* Room=0.85, Damp=0.20, Wet=0.60 */

        synth_engine_render_impulse_response(&engine, buffer, total_frames);

        write_wav_file("test_reverb_impulse.wav", buffer, total_frames);
        printf("Saved: %-24s (Stereo Reverb Impulse Response, 3.0s)\n", "test_reverb_impulse.wav");
    }

    /* 2. リバーブ OFF vs ON 比較用 音楽フレーズ WAV (スタッカート和音) */
    uint8_t chord[4] = { 60, 64, 67, 72 }; /* C Major */
    uint32_t chord_dur = SYNTH_SAMPLE_RATE * 2; /* 2秒 */

    /* 2-A: Reverb OFF */
    {
        synth_engine_init(&engine);
        synth_engine_set_master_volume(&engine, 0.7f);
        synth_engine_set_reverb_enabled(&engine, false);
        engine.default_adsr.attack_time_sec = 0.01f;
        engine.default_adsr.decay_time_sec = 0.05f;
        engine.default_adsr.sustain_level = 0.0f; /* 歯切れの良いスタッカート */
        engine.default_adsr.release_time_sec = 0.05f;

        for (int i = 0; i < 4; i++) {
            synth_engine_note_on(&engine, chord[i], 0.8f, WAVE_SAWTOOTH);
        }
        synth_engine_render(&engine, buffer, chord_dur);

        write_wav_file("test_reverb_off.wav", buffer, chord_dur);
        printf("Saved: %-24s (Staccato Chord with Reverb OFF)\n", "test_reverb_off.wav");
    }

    /* 2-B: Reverb ON */
    {
        synth_engine_init(&engine);
        synth_engine_set_master_volume(&engine, 0.7f);
        synth_engine_set_reverb(&engine, true, 0.80f, 0.25f, 0.40f);
        engine.default_adsr.attack_time_sec = 0.01f;
        engine.default_adsr.decay_time_sec = 0.05f;
        engine.default_adsr.sustain_level = 0.0f;
        engine.default_adsr.release_time_sec = 0.05f;

        for (int i = 0; i < 4; i++) {
            synth_engine_note_on(&engine, chord[i], 0.8f, WAVE_SAWTOOTH);
        }
        synth_engine_render(&engine, buffer, chord_dur);

        write_wav_file("test_reverb_on.wav", buffer, chord_dur);
        printf("Saved: %-24s (Staccato Chord with Reverb ON)\n", "test_reverb_on.wav");
    }

    free(buffer);
}

/**
 * @brief ベンチマーク5: ADSR減衰プロファイルの数値検証データ生成
 */
static void generate_adsr_profile_data(void)
{
    printf("\n======================================================================\n");
    printf(" 5. ADSR Envelope Numerical Verification & Trace Export\n");
    printf("======================================================================\n");

    SynthEngine engine;
    synth_engine_init(&engine);
    synth_engine_set_master_volume(&engine, 1.0f);

    engine.default_adsr.attack_time_sec   = 0.010f; /* 10ms */
    engine.default_adsr.decay_time_sec    = 0.050f; /* 50ms */
    engine.default_adsr.sustain_level     = 0.600f; /* 0.6 */
    engine.default_adsr.release_time_sec  = 0.080f; /* 80ms */
    engine.default_adsr.exponential_decay = true;

    int note = 69; /* A4 */
    int v_idx = synth_engine_note_on(&engine, (uint8_t)note, 1.0f, WAVE_SINE);

    FILE *fp = fopen("adsr_trace.csv", "w");
    if (!fp) {
        fprintf(stderr, "Cannot open adsr_trace.csv\n");
        return;
    }
    fprintf(fp, "sample_idx,time_ms,pcm_l,env_level,env_state,voice_active\n");

    uint32_t note_on_frames = 4800;
    uint32_t note_off_frames = 5760;
    int16_t buf[2];
    uint32_t total_samples = note_on_frames + note_off_frames;

    bool release_started = false;
    bool fully_zero_reached = false;

    for (uint32_t i = 0; i < total_samples; i++) {
        if (i == note_on_frames) {
            synth_engine_note_off(&engine, (uint8_t)note);
            release_started = true;
        }

        SynthVoice *v = &engine.voices[v_idx];
        bool active_before = v->active;

        synth_engine_render(&engine, buf, 1);

        float env_after = v->current_env_level;
        EnvState state_after = v->env_state;
        bool active_after = v->active;

        if (release_started && active_before && !active_after) {
            fully_zero_reached = true;
            printf("  - Release Phase Complete at Sample %u (%.2f ms), active=%d, level=%.6f\n", 
                   i, (double)i * 1000.0 / 48000.0, active_after ? 1 : 0, env_after);
        }

        double time_ms = (double)i * 1000.0 / 48000.0;
        fprintf(fp, "%u,%.4f,%d,%.6f,%d,%d\n", 
                i, time_ms, buf[0], env_after, state_after, active_after ? 1 : 0);
    }

    fclose(fp);

    /* Test Case D: 全消音 (all_notes_off) の即時性検証 */
    printf("[Test D] All Notes Off Immediate Cut Verification:\n");
    for (int i = 0; i < 8; i++) {
        synth_engine_note_on(&engine, 60 + i, 0.8f, WAVE_SINE);
    }
    synth_engine_all_notes_off(&engine);
    bool all_freed = true;
    for (int i = 0; i < 8; i++) {
        if (engine.voices[i].active || engine.voices[i].env_state != ENV_IDLE || engine.voices[i].current_env_level != 0.0f) {
            all_freed = false;
        }
    }
    printf("  - All notes off check: %s\n", all_freed ? "PASS (All 8 voices instantly IDLE)" : "FAIL");
}

int main(int argc, char *argv[])
{
    printf("======================================================================\n");
    printf(" Sony Spresense Synthesizer Engine Benchmark & Quality Analysis Suite \n");
    printf("======================================================================\n");

    run_rendering_benchmark();
    run_sine_lut_vs_sinf_benchmark();
    generate_spectral_test_waves();
    generate_reverb_test_waves();
    generate_adsr_profile_data();

    printf("\nAll benchmark and audio generation tasks completed successfully.\n");
    return 0;
}

