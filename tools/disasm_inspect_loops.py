#!/usr/bin/env python3
import subprocess
import re

OBJDUMP = "/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin/arm-none-eabi-objdump"
APPS = "/home/ootak/spresense/sdk/apps/examples/synth/asmp_sub"

# Cortex-M4F instruction cycle estimates
# In-order, 3-stage pipeline.
# vdiv.f32: 14 cycles
# vfma.f32, vmla.f32, vmls.f32: 3 cycles (issue 1, latency 3)
# vmul.f32, vadd.f32, vsub.f32: 1 cycle (latency 1-2)
# vldr.32, vstr.32: 1-2 cycles (single cycle to TCM/SRAM without contention, 2 with stall)
# branch: 1 cycle if not taken, 2-3 cycles if taken
# vcmp.f32 + vmrs: 2-3 cycles

def dump_loop(elf_path, func_name):
    cmd = f"{OBJDUMP} -d {elf_path}"
    p = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    lines = p.stdout.splitlines()
    func_lines = []
    in_func = False
    for line in lines:
        if f"<{func_name}>:" in line:
            in_func = True
            continue
        if in_func:
            if re.match(r"^[0-9a-fA-F]{8} <[^>]+>:", line):
                break
            func_lines.append(line)
    return func_lines

lines_c3 = dump_loop(f"{APPS}/sub2_melody/synth_worker2.debug", "sub2_render_classic3.isra.0")
lines_w3 = dump_loop(f"{APPS}/sub2_melody/synth_worker2.debug", "sub2_render_wt3.isra.0")

print(f"Classic3 line count: {len(lines_c3)}")
print(f"WT3 line count: {len(lines_w3)}")

# Print a snippet of the inner loop in classic3
print("=== Classic3 inner loop snippet ===")
for l in lines_c3[100:160]:
    print(l)

print("=== WT3 inner loop snippet ===")
for l in lines_w3[50:110]:
    print(l)
