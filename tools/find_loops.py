#!/usr/bin/env python3
import subprocess
import re

OBJDUMP = "/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin/arm-none-eabi-objdump"
APPS = "/home/ootak/spresense/sdk/apps/examples/synth/asmp_sub"

def get_disasm(elf, func):
    p = subprocess.run(f"{OBJDUMP} -d {elf}", shell=True, stdout=subprocess.PIPE, universal_newlines=True)
    in_fn = False
    lines = []
    for l in p.stdout.splitlines():
        if f"<{func}>:" in l:
            in_fn = True
            continue
        if in_fn:
            if re.match(r"^[0-9a-fA-F]{8} <", l):
                break
            lines.append(l)
    return lines

# Let's inspect loop branches in sub2_render_classic3.isra.0 and sub2_render_wt3.isra.0
for fn in ["sub2_render_classic3.isra.0", "sub2_render_wt3.isra.0"]:
    lines = get_disasm(f"{APPS}/sub2_melody/synth_worker2.debug", fn)
    print(f"=== {fn} ===")
    for idx, l in enumerate(lines):
        if "bne" in l or "beq" in l or "b.w" in l or "bcs" in l or "bcc" in l:
            # print jump instruction and target
            print(f"  [{idx:4d}] {l}")
