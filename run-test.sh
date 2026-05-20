#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
# Exit if any pipeline command fails
# Treat unset variables as an error
set -euo pipefail

# Define variables for easy configuration
CLIENT_BIN="./build/bin/machaudio-client"
#INPUT_FILE="input.mulaw"
INPUT_FILE="input_16khz_mono_le.l16"

# Input configuration
#IN_FORMAT=0    # 0 = G.711 mu-law (PCMU)
#IN_CHANNELS=1  # 1 = Mono
#
## Output configuration
#OUT_FORMAT=96  # 96 = L16
#OUT_RATE=16000 # 16000 Hz
#OUT_CHANNELS=1 # 1 = Mono
#OUT_ENDIAN=1   # 1 = Little-Endian

# Input configuration
IN_FORMAT=96   # 96 = L16
IN_CHANNELS=1  # 1 = Mono
IN_RATE=16000  # 16000 Hz
IN_ENDIAN=1    # 1 = Little-Endian

# Output configuration
OUT_FORMAT=0  # 0 = G.711 mu-law (PCMU)
OUT_RATE=8000 # 8000 Hz
OUT_CHANNELS=1 # 1 = Mono
OUT_ENDIAN=0   # 0 = None (not applicable for mu-law)


# Test duration and execution
DURATION_SEC=120

echo "Starting MachAudio client test for ${DURATION_SEC} seconds..."
echo "Input:  Format=${IN_FORMAT}, Channels=${IN_CHANNELS}, File=${INPUT_FILE}"
echo "Output: Format=${OUT_FORMAT}, Rate=${OUT_RATE}Hz, Channels=${OUT_CHANNELS}, Endian=${OUT_ENDIAN}"
echo "----------------------------------------------------------------------"

# Execute the client
"${CLIENT_BIN}" \
  -i "${INPUT_FILE}" \
  -f "${IN_FORMAT}" \
  -c "${IN_CHANNELS}" \
  -r "${IN_RATE}" \
  -e "${IN_ENDIAN}" \
  -F "${OUT_FORMAT}" \
  -R "${OUT_RATE}" \
  -C "${OUT_CHANNELS}" \
  -E "${OUT_ENDIAN}" \
  -w \
  -l \
  -d "${DURATION_SEC}"

echo "----------------------------------------------------------------------"
echo "Test completed."
