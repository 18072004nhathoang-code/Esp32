#!/usr/bin/env bash
set -euo pipefail

build_dir="$(mktemp -d -t esp32-native-tests.XXXXXX)"
trap 'rm -rf "$build_dir"' EXIT

compile_and_run() {
    local name="$1"
    shift
    g++ -std=c++11 -Wall -Wextra -Werror -Iinclude "$@" -o "$build_dir/$name"
    "$build_dir/$name"
}

compile_and_run native_contract tests/native_contract_test.cpp
compile_and_run service_logic tests/service_logic_test.cpp src/core/service_state_logic.cpp
compile_and_run fault_injection tests/fault_injection_test.cpp src/core/service_state_logic.cpp
compile_and_run music_decoder_lifecycle tests/music_decoder_lifecycle_test.cpp
compile_and_run ai_voice_protocol tests/ai_voice_protocol_test.cpp
compile_and_run wifi_time_service tests/wifi_time_service_test.cpp
compile_and_run wifi_scan_adapter tests/wifi_scan_adapter_test.cpp
compile_and_run xiaozhi_protocol tests/xiaozhi_protocol_test.cpp
compile_and_run xiaozhi_session_logic tests/xiaozhi_session_logic_test.cpp
compile_and_run system_defects_regression tests/system_defects_regression_test.cpp

echo "All native firmware tests passed."
