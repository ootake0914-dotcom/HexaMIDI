#!/usr/bin/env python3
import subprocess
import re
import os

OBJDUMP = "/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin/arm-none-eabi-objdump"
APPS = "/home/ootak/spresense/sdk/apps/examples/synth/asmp_sub"

def disassemble_func(elf_path, func_name):
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

def analyze_instructions(lines):
    stats = {
        'total': 0,
        'vdiv': 0,
        'vfma': 0,
        'vmul': 0,
        'vadd': 0,
        'vsub': 0,
        'vmla': 0,
        'vmls': 0,
        'vldr': 0,
        'vstr': 0,
        'vcmp': 0,
        'branch': 0,
        'load': 0,
        'store': 0,
    }
    for line in lines:
        parts = line.split('\t')
        if len(parts) >= 3:
            stats['total'] += 1
            inst = parts[2].strip()
            op = inst.split()[0] if inst.split() else ''
            # check instruction
            if op.startswith('vdiv'): stats['vdiv'] += 1
            elif op.startswith('vfma'): stats['vfma'] += 1
            elif op.startswith('vmul'): stats['vmul'] += 1
            elif op.startswith('vadd'): stats['vadd'] += 1
            elif op.startswith('vsub'): stats['vsub'] += 1
            elif op.startswith('vmla'): stats['vmla'] += 1
            elif op.startswith('vmls'): stats['vmls'] += 1
            elif op.startswith('vldr'): stats['vldr'] += 1
            elif op.startswith('vstr'): stats['vstr'] += 1
            elif op.startswith('vcmp') or op.startswith('vcmpe'): stats['vcmp'] += 1
            elif op.startswith('b') and not op.startswith('bic'): stats['branch'] += 1
            elif op.startswith('ldr'): stats['load'] += 1
            elif op.startswith('str'): stats['store'] += 1
    return stats

funcs = [
    (f"{APPS}/sub2_melody/synth_worker2.debug", "sub2_render_classic3.isra.0"),
    (f"{APPS}/sub2_melody/synth_worker2.debug", "sub2_render_wt3.isra.0"),
    (f"{APPS}/sub2_melody/synth_worker2.debug", "sub2_render.constprop.0"),
    (f"{APPS}/sub3_bass/synth_worker3.debug", "sub3_render.constprop.0"),
    (f"{APPS}/sub4_drums/synth_worker4.debug", "sub4_render.constprop.0"),
    (f"{APPS}/sub4_drums/synth_worker4.debug", "sub4_render_kick.constprop.0"),
    (f"{APPS}/sub4_drums/synth_worker4.debug", "sub4_render_snare.constprop.0"),
    (f"{APPS}/sub5_dsp/synth_worker5.debug", "subcore5_entry"),
]

for elf, fn in funcs:
    lines = disassemble_func(elf, fn)
    stats = analyze_instructions(lines)
    print(f"=== Function: {fn} (Lines: {len(lines)}, Total Insts: {stats['total']}) ===")
    print(f"  vdiv: {stats['vdiv']}, vfma: {stats['vfma']}, vmul: {stats['vmul']}, vadd: {stats['vadd']}, vsub: {stats['vsub']}")
    print(f"  vmla: {stats['vmla']}, vmls: {stats['vmls']}, vcmp: {stats['vcmp']}")
    print(f"  vldr: {stats['vldr']}, vstr: {stats['vstr']}, ldr: {stats['load']}, str: {stats['store']}")
    print(f"  branch: {stats['branch']}")
    print()
