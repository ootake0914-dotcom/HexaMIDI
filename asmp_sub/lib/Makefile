############################################################################
# asmp_sub/lib/Makefile
# SDK modules/asmp/worker の libasmpw.a をビルドして取り込む
############################################################################

include $(APPDIR)/Make.defs
-include $(SDKDIR)/Make.defs

BIN = libasmpw$(LIBEXT)
WORKER_DIR = $(SDKDIR)$(DELIM)modules$(DELIM)asmp$(DELIM)worker
LIB = $(WORKER_DIR)$(DELIM)libasmpw$(LIBEXT)
CFLAGS += -I$(CURDIR)/../.. -I$(CURDIR)/../../include -I$(SDKDIR)/modules/include

.PHONY: depend clean distclean
all: $(BIN)

depend:
	$(Q) $(MAKE) -C $(WORKER_DIR) TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" depend

worker_compat.o: worker_compat.c
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

$(LIB): depend
	$(Q) $(MAKE) -C $(WORKER_DIR) TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" $(BIN)

$(BIN): $(LIB) worker_compat.o
	$(Q) install $< $@
	$(Q) $(AR) rcs $@ worker_compat.o

clean:
	$(call CLEAN)
	$(Q) rm -f worker_compat.o

distclean: clean
