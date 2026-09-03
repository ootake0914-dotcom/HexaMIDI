############################################################################
# asmp_sub/Makefile
# Sony Spresense ASMP Worker ELF ビルド (romfs 埋め込み用)
# 公式 examples/asmp/worker の構成に準拠
############################################################################

include $(APPDIR)/Make.defs
-include $(SDKDIR)/Make.defs

WORKER_ELFS = sub1_sequencer/synth_worker1 \
              sub2_melody/synth_worker2 \
              sub3_bass/synth_worker3 \
              sub4_drums/synth_worker4 \
              sub5_dsp/synth_worker5

SUBDIRS = $(dir $(WORKER_ELFS)) lib/

define DIR_template
$(1)_$(2):
	+$(Q) $(MAKE) -C $(1) $(3) TOPDIR="$(TOPDIR)" APPDIR="$(APPDIR)" SDKDIR="$(SDKDIR)" CROSSDEV=$(CROSSDEV)
endef

.PHONY: all lib depend clean
all: romfs.h

# romfs ディレクトリから romfs.img を生成

romfs.img: $(WORKER_ELFS)
	$(Q) mkdir -p romfs
	$(Q) cp $^ romfs
	$(Q) genromfs -f $@ -d romfs -V "WORKER"

# romfs.img から C ヘッダ (romfs.h) を生成してメインアプリに埋め込む

romfs.h: romfs.img
	$(Q) xxd -i $< | sed -e "s/^unsigned/static const unsigned/g" >$@

# ASMP ワーカーライブラリ (libasmpw.a) のビルド

lib:
	$(Q) $(MAKE) -C lib TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)

# 各ワーカー ELF のビルド

$(WORKER_ELFS): lib
	$(Q) $(MAKE) -C $(dir $@) TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)

.depend:
	$(Q) $(MAKE) -C lib TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV) depend
	@touch $@

depend: .depend

# クリーン

clean: $(foreach DIR, $(SUBDIRS), $(DIR)_clean)
	$(Q) rm -rf romfs romfs.img romfs.h

$(foreach DIR, $(SUBDIRS), $(eval $(call DIR_template,$(DIR),clean,clean)))
