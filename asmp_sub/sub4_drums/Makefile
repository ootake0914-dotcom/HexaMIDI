############################################################################
# asmp_sub/sub4_drums/Makefile
# SubCore 4: GM ドラム音源 ワーカー ELF
############################################################################

include $(APPDIR)/Make.defs
-include $(SDKDIR)/Make.defs

ifeq ($(WINTOOL),y)
LIB_DIR = "${shell cygpath -w ../lib}"
else
LIB_DIR = "../lib"
endif

LDLIBPATH += -L $(LIB_DIR)
LDLIBS += $(ASMPWORKER_LIB)
LIB_ASMPWORKER := $(LIB_DIR)$(DELIM)libasmpw.a

BIN = synth_worker4

CSRCS = worker_entry.c sub4_main.c ../sub_sine_lut.c ../lib/worker_compat.c

include ../common.mk
CELFFLAGS += $(CELFFLAGS_COMMON)
CELFFLAGS += ${INCDIR_PREFIX}$(CURDIR)$(DELIM)..$(DELIM)..$(DELIM)include
CELFFLAGS += ${INCDIR_PREFIX}$(CURDIR)$(DELIM)..
CELFFLAGS += ${INCDIR_PREFIX}$(SDKDIR)$(DELIM)modules$(DELIM)asmp$(DELIM)worker

AOBJS = $(ASRCS:.S=$(OBJEXT))
COBJS = $(CSRCS:.c=$(OBJEXT))

OBJEXT ?= .o

all: $(BIN)


$(COBJS): %$(OBJEXT): %.c $(HDR_DEPS)
	@echo "CC: $<"
	$(Q) $(CC) -c $(CELFFLAGS) $< -o $@

$(AOBJS): %$(OBJEXT): %.S
	@echo "AS: $<"
	$(Q) $(CC) -c $(AFLAGS) $< -o $@

$(BIN): $(COBJS) $(AOBJS) $(LIB_ASMPWORKER)
	@echo "LD: $@"
	$(Q) $(LD) $(LDRAWELFFLAGS) $(LDLIBPATH) -o $@.debug $^ $(LDLIBS)
	$(Q) $(STRIP) -d -o $@ $@.debug

$(LIB_ASMPWORKER):

clean:
	$(call DELFILE, $(BIN))
	$(call DELFILE, $(BIN).debug)
	$(call CLEAN)
