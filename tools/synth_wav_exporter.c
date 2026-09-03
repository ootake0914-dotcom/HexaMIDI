/**
 * @file synth_wav_exporter.c
 * @brief シンセサイザーの出力をWAVファイルに出力する検証・テスト用ツール
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "synth_engine.h"
#include "sequencer.h"

#define CHUNK_FRAMES (256)

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

int main(int argc, char *argv[])
{
    uint32_t track_count = 0;
    const Track *tracks = sequencer_get_preset_tracks(&track_count);

    uint32_t track_idx = 0;
    const char *out_filename = "output.wav";
    bool enable_reverb = true;

    if (argc > 1) {
        track_idx = (uint32_t)atoi(argv[1]);
        if (track_idx >= track_count) {
            track_idx = 0;
        }
    }
    if (argc > 2) {
        out_filename = argv[2];
    }
    if (argc > 3) {
        enable_reverb = (atoi(argv[3]) != 0);
    }

    printf("Rendering track [%u] '%s' to %s (Reverb: %s)...\n", 
           (unsigned int)track_idx, tracks[track_idx].title, out_filename,
           enable_reverb ? "Enabled" : "Disabled");

    SynthEngine engine;
    synth_engine_init(&engine);
    synth_engine_set_master_volume(&engine, 0.7f);
    if (enable_reverb) {
        synth_engine_set_reverb(&engine, true, 0.6f, 0.4f, 0.30f);
    }

    Sequencer seq;
    sequencer_init(&seq, &engine);
    sequencer_play_track(&seq, &tracks[track_idx]);

    FILE *fp = fopen(out_filename, "wb");
    if (!fp) {
        perror("Failed to open output file");
        return 1;
    }

    /* ヘッダーのプレースホルダーを書き込み */
    WavHeader hdr;
    memset(&hdr, 0, sizeof(WavHeader));
    if (fwrite(&hdr, sizeof(WavHeader), 1, fp) != 1) {
        perror("Failed to write WAV header");
        fclose(fp);
        return 1;
    }

    uint32_t total_frames = 0;
    int16_t buffer[CHUNK_FRAMES * SYNTH_CHANNELS];
    bool write_failed = false;
    const uint32_t kMaxFrames = SYNTH_SAMPLE_RATE * 30u; /* ループ曲の無限再生ガード: 30秒上限 */

    while (sequencer_tick_frames(&seq, CHUNK_FRAMES, SYNTH_SAMPLE_RATE)) {
        synth_engine_render(&engine, buffer, CHUNK_FRAMES);
        if (fwrite(buffer, sizeof(int16_t) * SYNTH_CHANNELS, CHUNK_FRAMES, fp) != CHUNK_FRAMES) {
            write_failed = true;
            break;
        }
        total_frames += CHUNK_FRAMES;
        if (total_frames >= kMaxFrames) break;
    }

    /* 残響・リリースのレンダリング (約0.5秒) */
    for (int r = 0; r < 20 && !write_failed; r++) {
        synth_engine_render(&engine, buffer, CHUNK_FRAMES);
        if (fwrite(buffer, sizeof(int16_t) * SYNTH_CHANNELS, CHUNK_FRAMES, fp) != CHUNK_FRAMES) {
            write_failed = true;
        } else {
            total_frames += CHUNK_FRAMES;
        }
    }

    /* WAVヘッダーを更新 */
    uint32_t data_bytes = total_frames * SYNTH_CHANNELS * sizeof(int16_t);
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

    bool finalize_ok = !write_failed &&
                       fseek(fp, 0, SEEK_SET) == 0 &&
                       fwrite(&hdr, sizeof(WavHeader), 1, fp) == 1 &&
                       fflush(fp) == 0 &&
                       fclose(fp) == 0;

    if (write_failed || !finalize_ok) {
        fprintf(stderr, "Error: WAV output is incomplete or corrupt (%s)\n", out_filename);
        return 1;
    }

    printf("Complete! Output saved: %s (Duration: %.2f sec)\n",
           out_filename, (float)total_frames / (float)SYNTH_SAMPLE_RATE);
    return 0;
}
