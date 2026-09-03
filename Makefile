############################################################################
# Makefile
# Sony Spresense SDK (NuttX) Application Build Configuration
############################################################################

include $(APPDIR)/Make.defs

# アプリケーション定義 (Kconfig の設定値を反映)
PROGNAME = $(CONFIG_EXAMPLES_SYNTH_PROGNAME)
PRIORITY = $(CONFIG_EXAMPLES_SYNTH_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_SYNTH_STACKSIZE)
MODULE = $(CONFIG_EXAMPLES_SYNTH)

# ソースコード一覧
# ※ sequencer.c / preset_songs.c / midi_demo_songs.c は内蔵プリセット曲レーン
#    廃止に伴いアプリから外れた (ホストツール専用。CMakeLists.txt 参照)
CSRCS = src/synth_engine.c \
        src/midi_parser.c \
        src/midi_player.c \
        src/asmp_manager.c \
        src/audio_player.c \
        src/joystick_shield.c \
        src/synth_controller.c \
        src/sd_midi.c \
        src/sd_loader.c \
        src/sd_player.c \
        src/async_logger.c \
        src/boot_diag.c \
        asmp_sub/sub_sine_lut.c

MAINSRC = synth_main.c

# インクルードパス
CFLAGS += -I$(CURDIR)/include -I$(SDKDIR)/modules/include

# P0-4 最適化: libm 高速化 (errno 設定・FP トラップ不要。
# NaN ガードは整数bit判定のため無影響。ワーカー側は common.mk で -ffast-math 済み)
CFLAGS += -fno-math-errno -fno-trapping-math

ifeq ($(CONFIG_EXAMPLES_SYNTH_MULTICORE),y)
CFLAGS += -I$(CURDIR)/asmp_sub
CFLAGS += -DSYNTH_MULTICORE=1
else
CFLAGS += -DSYNTH_MULTICORE=0
endif

include $(APPDIR)/Application.mk

ifeq ($(CONFIG_EXAMPLES_SYNTH_MULTICORE),y)

# romfs.h は build_worker (サブ make) が生成する。
# .depend 解決のために最初だけ空ファイルを作るプレースホルダ
# (実ヘッダは $(OBJS) -> ビルドスタンプ経由で必ず submake が先に上書きする)
ifeq ($(CONFIG_FS_ROMFS),y)
.depend: asmp_sub/romfs.h
asmp_sub/romfs.h:
	@echo >$@
endif

# サブコア ELF のビルドはスタンプで管理する。
# スタンプはワーカー側の全ソース/ヘッダ/Makefile に依存させること。
# (依存なしの無条件スタンプだと、Main 側だけ新しく Worker ELF だけ古い
#  「プロトコル不整合ビルド」が生成されてしまう — 実害: 音がぷつぷつ切れる)
.PHONY: build_worker clean_worker

# NOTE: include/*.h を含めること。asmp_protocol.h 変更時にワーカー再ビルドされないと
# Main 新 + Worker 旧のプロトコル不整合ビルド (共有メモリオフセットずれ → 音がぷつぷつ切れる) になる。
WORKER_DEPS := $(wildcard asmp_sub/Makefile) \
               $(wildcard asmp_sub/*.h) \
               $(wildcard asmp_sub/lib/*.c) \
               $(wildcard asmp_sub/lib/*.h) \
               $(wildcard asmp_sub/sub*/*.c) \
               $(wildcard asmp_sub/sub*/*.h) \
               $(wildcard asmp_sub/sub*/Makefile) \
               $(wildcard include/*.h)

build_worker: asmp_sub/.built_stamp

asmp_sub/.built_stamp: $(WORKER_DEPS)
	@$(MAKE) -C asmp_sub TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)
	@touch $@

$(OBJS): asmp_sub/.built_stamp

clean:: clean_worker

clean_worker:
	@$(MAKE) -C asmp_sub TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV) clean
	@rm -f asmp_sub/.built_stamp

endif
