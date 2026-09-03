/**
 * @file sd_midi.c
 * @brief SD カード内 Standard MIDI File の走査・ロード実装
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sd_midi.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <errno.h>

#ifdef __NuttX__
#include <sys/mount.h>
#include <unistd.h>
#define SD_USLEEP(us) usleep(us)
#endif

/* NuttX: 拡張ボード microSD のマウントポイント配下を順に試す */
static const char *const s_search_dirs[] = {
    "/mnt/sd0/MID",
    "/mnt/sd0/midi",
    "/mnt/sd0",
    "sdmidi",   /* ホスト PC リハーサル用フォルダ */
    NULL
};

static bool has_mid_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return (strcmp(ext, ".mid") == 0 || strcmp(ext, ".MID") == 0 ||
            strcmp(ext, ".smf") == 0 || strcmp(ext, ".SMF") == 0);
}

/* パストラバーサル対策: ファイル名にディレクトリ区切りや親参照が含まれていないか検証
 * 攻撃者が "../secret.mid" や "a/b.mid" で MID ディレクトリ外の任意ファイルを
 * 読ませるのを防ぐ。SD カードは攻撃者が物理的に差替可能な外部入力であるため
 * ファイル名は信頼できない */
static bool is_safe_filename(const char *name)
{
    if (!name || name[0] == '\0') return false;
    /* 先頭が '.' / '/' / '\\' は拒否 (隠しファイル・絶対パス・カレントディレクトリ回避) */
    if (name[0] == '.' || name[0] == '/' || name[0] == '\\') return false;
    for (size_t i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':') return false;
        if (c == '.' && name[i+1] == '.' ) return false; /* ".." を含むものは全て拒否 */
        if ((unsigned char)c < 0x20u) return false; /* 制御文字拒否 */
    }
    if (strstr(name, "..") != NULL) return false;
    return true;
}

static void copy_display_name(char *dst, size_t dstsz, const char *fname)
{
    size_t len = strlen(fname);
    if (len >= 4) len -= 4; /* 拡張子を落とす */
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, fname, len);
    dst[len] = '\0';
}

/* ロード用に実ファイル名 (拡張子込み) を保存する。
 * 表示名は 48 バイトへ切詰められるため、これをそのままパス再構築に使うと
 * 長いファイル名の MIDI が必ずオープン失敗していた */
static void copy_fs_name(char *dst, size_t dstsz, const char *fname)
{
    size_t len = strlen(fname);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, fname, len);
    dst[len] = '\0';
}

void sd_midi_ensure_mount(void)
{
#if defined(__NuttX__)
    /* Automounter (FS_AUTOMOUNTER + CXD56_SDCARD_AUTOMOUNT) が有効な構成では
     * /mnt/sd0 は自動的にマウントされるため、手動 mount は不要かつ
     * ramfs オーバーレイが ENOTDIR(20) を引き起こす原因になる。
     * まず既存マウントを優先して確認し、必要な場合のみ手動 mount を試す */
    {
        struct stat st;
        DIR *test = NULL;
        for (int wait = 0; wait < 4; wait++) {
            if (stat("/mnt/sd0", &st) == 0 && S_ISDIR(st.st_mode)) {
                test = opendir("/mnt/sd0/MID");
                if (test) { closedir(test); return; }
                test = opendir("/mnt/sd0");
                if (test) { closedir(test); return; }
            }
            if (wait < 3) SD_USLEEP(100 * 1000);
        }
        /* SD マウントポイントがまだ無い場合のみ手動マウントへフォールバック */
        if (stat("/mnt/sd0", &st) != 0) {
            mkdir("/mnt", 0777);
            /* ramfs は既マウントなら EBUSY で無視 */
            if (mount(NULL, "/mnt", "ramfs", 0, NULL) < 0) {
                int e = errno;
                if (e != EBUSY && e != EEXIST && e != 0) {
                    /* 無視: automounter 構成では /mnt が既に有効 */
                }
            }
            mkdir("/mnt/sd0", 0777);
        }
    }

    /* カード挿入〜SDIO プローブ完了のラグを考慮し最大 4 回試行。
     * 既マウント (EBUSY) は即成功扱いで抜ける。
     * 失敗理由は最終試行時に errno で 1 行だけ診断出力する:
     *   ENOENT = /dev/mmcsd0 未登録 (CONFIG_CXD56_SDIO 欠落の典型症状)
     *   ENODEV/ENXIO = カード未挿入・認識失敗
     *   EINVAL 等 = フォーマット非対応 (exFAT カード等)
     * (呼び出しは起動時 + 未検出中の 5 秒周期リスキャンのみなので
     *  1 行/回の診断出力はログ洪水にならない) */
    int last_err = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (mount("/dev/mmcsd0", "/mnt/sd0", "vfat", 0, NULL) == 0) {
            printf("[SD] Mounted /dev/mmcsd0 -> /mnt/sd0\n");
            return;
        }
        last_err = errno;
        if (last_err == EBUSY) return; /* 既マウント (automounter含む) */
        /* Automounter が有効でも手動 mount が ENOTDIR で失敗し続ける場合は
         * 既にマウント済みの可能性が高いため、stat で再確認して抜ける */
        {
            struct stat st2;
            if (stat("/mnt/sd0/MID", &st2) == 0) return;
            if (stat("/mnt/sd0", &st2) == 0) {
                DIR *d = opendir("/mnt/sd0");
                if (d) { closedir(d); return; }
            }
        }
        SD_USLEEP(250 * 1000);
    }
    /* 最終的に SD が見えるなら失敗ログを抑止 (automounter 成功ケース) */
    {
        struct stat st3;
        if (stat("/mnt/sd0/MID", &st3) == 0) return;
        DIR *d = opendir("/mnt/sd0");
        if (d) { closedir(d); return; }
    }
    printf("[SD] mount /dev/mmcsd0 failed: errno=%d%s\n", last_err,
           (last_err == ENOENT) ? " (no block driver: CONFIG_CXD56_SDIO enabled?)" :
           (last_err == ENODEV || last_err == ENXIO) ? " (no card detected)" :
           (last_err == EINVAL) ? " (unsupported FS: exFAT card?)" : "");
#endif
}

uint32_t sd_midi_scan(SdMidiList *list)
{
    if (!list) return 0;
    memset(list, 0, sizeof(SdMidiList));
    list->dir_index = -1;

#ifdef _WIN32
    for (int d = 0; s_search_dirs[d] != NULL && list->count == 0; d++) {
        /* .mid と .smf の両方を同一ディレクトリで走査する
         * (旧実装は .mid が 1 つでも見つかると .smf を試していなかった) */
        static const char *const win_patterns[] = { "\\*.mid", "\\*.smf" };
        bool found_in_dir = false;
        for (int p = 0; p < 2; p++) {
            char pattern[256];
            snprintf(pattern, sizeof(pattern), "%s%s", s_search_dirs[d], win_patterns[p]);

            WIN32_FIND_DATAA find;
            HANDLE h = FindFirstFileA(pattern, &find);
            if (h == INVALID_HANDLE_VALUE) continue;

            do {
                if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                /* "*.mid" パターンは "song.midi" 等にも一致し得るため再検査 */
                if (!has_mid_ext(find.cFileName)) continue;
                if (!is_safe_filename(find.cFileName)) continue;
                if (find.nFileSizeHigh != 0u || find.nFileSizeLow > SD_MIDI_MAX_BYTES) continue;
                if (list->count >= SD_MIDI_MAX_FILES) break;
                SdMidiEntry *e = &list->files[list->count];
                copy_display_name(e->name, SD_MIDI_NAME_LEN, find.cFileName);
                copy_fs_name(e->fs_name, sizeof(e->fs_name), find.cFileName);
                e->size = find.nFileSizeLow;
                list->count++;
                found_in_dir = true;
            } while (FindNextFileA(h, &find));
            FindClose(h);
        }
        if (found_in_dir) list->dir_index = d;
    }
#else
    for (int d = 0; s_search_dirs[d] != NULL && list->count == 0; d++) {
        DIR *dir = opendir(s_search_dirs[d]);
        if (!dir) {
            /* 診断用: なぜ開けなかったか errno を残す (マウント失敗の区別) */
            // printf("[SD] opendir %s failed errno=%d\n", s_search_dirs[d], errno);
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && list->count < SD_MIDI_MAX_FILES) {
            if (!has_mid_ext(ent->d_name)) continue;
            if (!is_safe_filename(ent->d_name)) continue;

            char fullpath[256];
            int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", s_search_dirs[d], ent->d_name);
            if (n < 0 || n >= (int)sizeof(fullpath)) continue;

            struct stat st;
            if (stat(fullpath, &st) != 0 || !(st.st_mode & S_IFREG)) continue;
            if (st.st_size > SD_MIDI_MAX_BYTES) continue; /* 上限超過はスキップ */

            SdMidiEntry *e = &list->files[list->count];
            copy_display_name(e->name, SD_MIDI_NAME_LEN, ent->d_name);
            copy_fs_name(e->fs_name, sizeof(e->fs_name), ent->d_name);
            e->size = (uint32_t)st.st_size;
            list->count++;
        }
        closedir(dir);
        if (list->count > 0) list->dir_index = d;
    }
#endif

#ifdef _WIN32
    printf("[SD] MIDI files found: %u\n", (unsigned int)list->count);
#else
    if (list->count == 0) {
        /* 全ディレクトリで 0 件: マウント失敗と空カードの区別用に errno を出す */
        DIR *dbg = opendir(s_search_dirs[0]);
        if (!dbg) {
            printf("[SD] MIDI files found: 0 (opendir %s errno=%d)\n", s_search_dirs[0], errno);
        } else {
            closedir(dbg);
            printf("[SD] MIDI files found: 0\n");
        }
    } else {
        printf("[SD] MIDI files found: %u\n", (unsigned int)list->count);
    }
#endif
    return list->count;
}

FILE *sd_midi_open_file(const SdMidiList *list, uint32_t index)
{
    if (!list || index >= list->count) return NULL;

    const char *dir = NULL;
    if (list->dir_index >= 0 && list->dir_index < (int)(sizeof(s_search_dirs) / sizeof(s_search_dirs[0])) - 1) {
        dir = s_search_dirs[list->dir_index];
    }
    if (!dir) dir = "sdmidi";

    static const char *const exts[] = { ".mid", ".MID", ".smf", ".SMF" };
    char fullpath[256];
    FILE *fp = NULL;

    if (list->files[index].fs_name[0] != '\0') {
        if (!is_safe_filename(list->files[index].fs_name)) {
            printf("[SD] Error: unsafe filename rejected: %s\n", list->files[index].fs_name);
            return NULL;
        }
        int nn = snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 dir,
                 (dir[strlen(dir)-1]=='/' || dir[strlen(dir)-1]=='\\') ? "" :
#ifdef _WIN32
                 "\\",
#else
                 "/",
#endif
                 list->files[index].fs_name);
        if (nn < 0 || nn >= (int)sizeof(fullpath)) return NULL;
        fp = fopen(fullpath, "rb");
    }

    for (int i = 0; i < 4 && fp == NULL; i++) {
        int nn2 = snprintf(fullpath, sizeof(fullpath), "%s%s%s%s",
                 dir,
                 (dir[strlen(dir)-1]=='/' || dir[strlen(dir)-1]=='\\') ? "" :
#ifdef _WIN32
                 "\\",
#else
                 "/",
#endif
                 list->files[index].name,
                 exts[i]);
        if (nn2 < 0 || nn2 >= (int)sizeof(fullpath)) continue;
        fp = fopen(fullpath, "rb");
    }

    if (!fp) {
        int err = errno;
        printf("[SD] Error: cannot open %s errno=%d\n", fullpath, err);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 14 || (uint32_t)fsize > SD_MIDI_MAX_BYTES) {
        fclose(fp);
        printf("[SD] Error: invalid size %ld (%s)\n", fsize, fullpath);
        return NULL;
    }

    return fp;
}

int sd_midi_load_file(SdMidiList *list, uint32_t index, MidiSong *out)
{
    if (!list || index >= list->count || !out) return -1;

    FILE *fp = sd_midi_open_file(list, index);
    if (!fp) return -2;

    /* イベントプールは sd_player.c 側の g_sd_player_pool を外部参照で使用。
     * 安全性: Main は本関数を呼ぶ前に必ず sd_lane_release_current (loaded=false)
     * を実行している。したがってワーカーは Main がプールを参照していない
     * 時に限りここへパースする (スキップ/完奏時の両パターンを網羅)。
     * (旧実装は本モジュール内に static プールを持ち、ワーカーが Main へ
     *  所有権移転した後も同じプールを参照していたため、次曲ロード時に
     *  再生中のイベントが破壊されるレースコンディションがあった) */
    int ret = midi_parser_load_file(fp, out, g_sd_player_pool, SD_MIDI_MAX_EVENTS, 48000);
    fclose(fp);

    if (ret != 0) {
        if (ret == -10) {
            printf("[SD] SKIP overflow (%s): %d events > %u (pool full)\n", list->files[index].name, ret, (unsigned)SD_MIDI_MAX_EVENTS);
        } else {
            printf("[SD] Error: parse failed (%s): %d\n", list->files[index].name, ret);
        }
        return ret;
    }

    printf("[SD] Loaded [%u] %s (%u events)\n",
           (unsigned int)index, list->files[index].name,
           (unsigned int)out->event_count);
    return 0;
}
