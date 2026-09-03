# HexaMIDI

A 6-core parallel MIDI synthesizer and player for Sony Spresense (CXD5602).

## Overview

HexaMIDI distributes real-time audio synthesis across all 6 ARM Cortex-M4F cores of the Spresense microcontroller using asymmetric multiprocessing (ASMP). It streams Standard MIDI Files (SMF) directly from an SD card and renders multi-timbral polyphonic audio in real-time.

## Multi-Core Architecture

- **Core 0 (Main)**: System coordination, 16-buffer DMA audio output, SD streaming reader, and JoyStick controls.
- **Core 1 (SubCore 1)**: MIDI event router, channel dispatcher, and tempo synchronization.
- **Core 2 (SubCore 2)**: Melody & lead synth engine (polyphonic oscillators, resonant filters, ADSR envelopes).
- **Core 3 (SubCore 3)**: Bass & chord engine (sub-oscillators, strings, unison voices).
- **Core 4 (SubCore 4)**: Percussion & drum kit synthesizer (kick, snare, hi-hat, cymbal, tom).
- **Core 5 (SubCore 5)**: Master DSP pipeline (3D binaural spatializer, chorus, delay, reverb, and stereo mixer).

## Features

- **6-Core Parallel Synthesis**: Concurrently rendered audio pipeline on bare-metal RTOS (NuttX).
- **16-Channel Multi-Timbral**: General MIDI tone support with dynamic timbre-based core allocation.
- **Lock-Free SPSC Streaming**: Real-time MIDI stream processing with memory-barrier synchronization.
- **Binaural 3D Spatial Audio**: Headphone-optimized spatial positioning via Interaural Time Difference (ITD).
- **Interactive Control**: JoyStick Shield support for real-time live performance and track navigation.

## Building

### Hardware Target (Sony Spresense)

Requires the Sony Spresense SDK and GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`).

```bash
# Build Spresense binary package
tools/cxd56/mkspk -c2 nuttx nuttx nuttx.spk
```

### Host Target (Unit Tests)

The core synthesizer engine and MIDI parser can be verified on host platforms (Windows / Linux) using CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/synth_unit_tests
```

## License

MIT License
