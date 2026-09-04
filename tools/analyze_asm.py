#!/usr/bin/env python3
import subprocess
import re
import sys

OBJDUMP = "/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin/arm-none-eabi-objdump"
NM = "/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin/arm-none-eabi-nm"
APPS = "/home/ootak/spresense/sdk/apps/examples/synth/asmp_sub"

def run_cmd(cmd):
    p = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    return p.stdout

print("=== Sub2 symbols ===")
print(run_cmd(f"{NM} {APPS}/sub2_melody/synth_worker2.debug | grep -E 'sub2_render|render'"))

print("=== Sub3 symbols ===")
print(run_cmd(f"{NM} {APPS}/sub3_bass/synth_worker3.debug | grep -E 'sub3_render|render|voice'"))

print("=== Sub4 symbols ===")
print(run_cmd(f"{NM} {APPS}/sub4_drums/synth_worker4.debug | grep -E 'sub4_render|render'"))

print("=== Sub5 symbols ===")
print(run_cmd(f"{NM} {APPS}/sub5_dsp/synth_worker5.debug | grep -E 'subcore5|process'"))
