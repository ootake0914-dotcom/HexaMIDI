############################################################################
# asmp_sub/common.mk
# 共通ワーカーCELFFLAGS / HDR依存 (全5ワーカーで共有)
############################################################################

# 共通最適化フラグ: 全ワーカーで統一 (sub1の漏れを防止、VFMA有効)
# 512B増のfreq LUTと合わせても破綻しない小最適化の積み重ねで192kHzは無理だが48kHzの厚みは出せる
CELFFLAGS_COMMON := -O3 -ffast-math -ffp-contract=fast -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant -funroll-loops -Wdouble-promotion

# ヘッダ依存: プロトコル混在による無音を防止
HDR_DEPS = ..$(DELIM)sub_common.h ..$(DELIM)sub_kick.h ..$(DELIM)sub_metal.h ..$(DELIM)sub_perc.h $(wildcard ..$(DELIM)..$(DELIM)include$(DELIM)*.h)
