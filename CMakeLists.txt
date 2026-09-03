cmake_minimum_required(VERSION 3.10)
project(HexaMIDI C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ビルド種別の既定 (未指定だと最適化なしになるため Release を既定にする)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 警告フラグ (静的バグの早期検出用)
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    # P0-4 最適化: double 混入の検出 (M4F ソフトfloat地獄の防止) と
    # libm 高速化 (errno/トラップ不要。NaN ガードは整数bit判定のため無影響)
    add_compile_options(-Wdouble-promotion -fno-math-errno -fno-trapping-math)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/asmp_sub
)

# ホスト/実機で SynthEngine 構造体レイアウトが一致しないとメモリ破壊するため、
# 必ずグローバルに SYNTH_MULTICORE を 0 で定義 (未指定時の暗黙欠損を根絶)
add_compile_definitions(SYNTH_MULTICORE=0)

# 共通ライブラリ・パーサー・音源ソース
# ※ asmp_sub の各エントリはホストではスレッド、実機ではワーカー ELF として動作するため
#    ホストビルドではすべてのターゲットにリンクされる
set(COMMON_SOURCES
    src/synth_engine.c
    src/midi_parser.c
    src/midi_player.c
    src/asmp_manager.c
    src/joystick_shield.c
    src/synth_controller.c
    src/audio_player.c
    src/sd_midi.c
    src/sd_loader.c
    src/sd_player.c
    src/async_logger.c
    src/boot_diag.c
    src/rt_profile.c
    asmp_sub/sub_sine_lut.c
    asmp_sub/sub_wavbank.c
    asmp_sub/sub_kick.c
    asmp_sub/sub1_sequencer/sub1_main.c
    asmp_sub/sub2_melody/sub2_main.c
    asmp_sub/sub3_bass/sub3_main.c
    asmp_sub/sub4_drums/sub4_main.c
    asmp_sub/sub5_dsp/sub5_main.c
)

# 内蔵プリセット曲レーン (アプリ本体からは廃止)。ホストツール専用ソース
set(TOOL_ONLY_SOURCES
    src/sequencer.c
    src/preset_songs.c
    src/midi_demo_songs.c
)

# 1. Main Core 実行可能バイナリ (synth)
add_executable(synth synth_main.c ${COMMON_SOURCES})

if(UNIX)
    target_link_libraries(synth m)
endif()

# 2. WAV エクスポーター（検証・テスト用ツール）
add_executable(synth_wav_exporter tools/synth_wav_exporter.c ${COMMON_SOURCES} ${TOOL_ONLY_SOURCES})

if(UNIX)
    target_link_libraries(synth_wav_exporter m)
endif()

# 3. 単体テスト (synth_unit_tests)
add_executable(synth_unit_tests tools/synth_unit_tests.c ${COMMON_SOURCES} ${TOOL_ONLY_SOURCES})

if(UNIX)
    target_link_libraries(synth_unit_tests m)
endif()

# 4. ベンチマークツール (synth_benchmark)
add_executable(synth_benchmark tools/synth_benchmark.c ${COMMON_SOURCES})

if(UNIX)
    target_link_libraries(synth_benchmark m)
endif()

# 5. ASMP 6コア分散パイプライン統合テスト (ホスト: OSスレッドで SubCore 1-5 をエミュレート)
add_executable(test_asmp_multicore tools/test_asmp_multicore.c ${COMMON_SOURCES})

if(UNIX)
    target_link_libraries(test_asmp_multicore m pthread)
endif()

# 6. DSP 数学検証スイート (数式の理論リファレンスに対する定量テスト)
add_executable(synth_math_tests tools/math_tests.c ${COMMON_SOURCES})
if(UNIX)
    target_link_libraries(synth_math_tests m)
endif()

# 7. RT 周辺サービス検証 (非同期ロガー / 非同期 WAV レコーダー)
add_executable(test_rt_services tools/test_rt_services.c ${COMMON_SOURCES})

# 8. フットプリント/リソースレポート (sizeof 実測のみ)
add_executable(perf_report tools/perf_report.c)

# 9. カオスストレステスト (100 万フレーム + サブコア ランダム遅延インジェクション)
#    ASMP_CHAOS_INJECT_DELAY を define し、subN のエポック処理直前に
#    0-5ms のランダム遅延を注入するビルドで検証する (実機ビルドには無関係)
add_executable(chaos_stress_test tools/chaos_stress_test.c ${COMMON_SOURCES})
target_compile_definitions(chaos_stress_test PRIVATE ASMP_CHAOS_INJECT_DELAY)
if(MSVC)
    target_link_libraries(chaos_stress_test winmm)   # timeBeginPeriod
endif()
if(UNIX)
    target_link_libraries(chaos_stress_test m pthread)
endif()
