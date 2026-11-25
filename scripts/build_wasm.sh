#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v em++ &>/dev/null; then
    if [[ -f "/tmp/emsdk/emsdk_env.sh" ]]; then
        # shellcheck source=/dev/null
        source "/tmp/emsdk/emsdk_env.sh"
    else
        echo "Error: em++ not found and /tmp/emsdk/emsdk_env.sh does not exist" >&2
        exit 1
    fi
fi

echo "Building HQRN WebAssembly module with em++ $(em++ --version | head -n 1)..."

cd "${REPO_DIR}"
em++ -std=c++20 -O3 hqrn_c_api.cpp -o hqrn_wasm.js \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="createHQRNEngine" \
  -s EXPORTED_FUNCTIONS="['_hqrn_create','_hqrn_destroy','_hqrn_prepare','_hqrn_reset','_hqrn_set_adapter_mode','_hqrn_get_adapter_mode','_hqrn_get_input_buffer_l','_hqrn_get_input_buffer_r','_hqrn_get_output_buffer_l','_hqrn_get_output_buffer_r','_hqrn_get_buffer_capacity','_hqrn_process','_hqrn_process_planar','_hqrn_process_interleaved','_hqrn_set_base_frequency','_hqrn_set_reverb_time','_hqrn_set_source_position','_hqrn_set_listener_position','_hqrn_set_mix','_hqrn_set_output_gain_db','_hqrn_set_clipper_params','_hqrn_set_source_target','_hqrn_set_listener_target','_hqrn_set_reverb_time_target','_hqrn_set_base_frequency_target','_hqrn_set_mix_target','_hqrn_get_latency_samples','_hqrn_get_sample_rate','_hqrn_get_node_coordinates','_hqrn_get_node_count','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAPF32','HEAP32']" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT="web,worker" \
  -s SINGLE_FILE=1

echo "WASM build successful: $(ls -lh hqrn_wasm.js)"
