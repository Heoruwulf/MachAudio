#!/usr/bin/env bash

# Best practices for robustness
set -euo pipefail

# Script metadata
readonly SCRIPT_NAME="${0##*/}"

# Usage function
usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} <input_audio_file>

This script converts an input audio file (e.g., WAV, MP3) into multiple 
raw, headerless audio formats required for MachAudio testing.

Outputs generated:
  - PCMU / G.711 mu-law (8kHz, Mono)
  - PCMA / G.711 a-law  (8kHz, Mono)
  - L16                 (48kHz, Mono, Little-Endian)
  - L16                 (48kHz, Mono, Big-Endian)
  - L16                 (16kHz, Mono, Little-Endian)
  - L16                 (16kHz, Mono, Big-Endian)

Output format: input_<filename>_<sample_rate>_<le|be>.<ext>
EOF
}

# Check for ffmpeg
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "Error: 'ffmpeg' is not installed or not in PATH." >&2
    exit 1
fi

# Check arguments
if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

readonly INPUT_FILE="$1"

if [[ ! -f "$INPUT_FILE" ]]; then
    echo "Error: Input file '$INPUT_FILE' not found." >&2
    exit 1
fi

# Extract filename without extension
BASENAME=$(basename "$INPUT_FILE")
FILENAME="${BASENAME%.*}"

echo "Converting '$INPUT_FILE' to raw audio formats..."

# 1. PCMU (G.711 mu-law) - 8kHz Mono
# FFmpeg format: mulaw, codec: pcm_mulaw
OUT_PCMU="input_${FILENAME}_8000.mulaw"
echo "  -> Generating $OUT_PCMU"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f mulaw -acodec pcm_mulaw -ar 8000 -ac 1 \
    "$OUT_PCMU"

# 2. PCMA (G.711 a-law) - 8kHz Mono
# FFmpeg format: alaw, codec: pcm_alaw
OUT_PCMA="input_${FILENAME}_8000.alaw"
echo "  -> Generating $OUT_PCMA"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f alaw -acodec pcm_alaw -ar 8000 -ac 1 \
    "$OUT_PCMA"

# 3. L16 (Linear PCM 16-bit) - 48kHz Mono, Little-Endian
# FFmpeg format: s16le, codec: pcm_s16le
OUT_L16_LE="input_${FILENAME}_48000_le.l16"
echo "  -> Generating $OUT_L16_LE"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f s16le -acodec pcm_s16le -ar 48000 -ac 1 \
    "$OUT_L16_LE"

# 4. L16 (Linear PCM 16-bit) - 48kHz Mono, Big-Endian
# FFmpeg format: s16be, codec: pcm_s16be
OUT_L16_BE="input_${FILENAME}_48000_be.l16"
echo "  -> Generating $OUT_L16_BE"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f s16be -acodec pcm_s16be -ar 48000 -ac 1 \
    "$OUT_L16_BE"

# 5. L16 (Linear PCM 16-bit) - 16kHz Mono, Little-Endian
# FFmpeg format: s16le, codec: pcm_s16le
OUT_L16_LE="input_${FILENAME}_16000_le.l16"
echo "  -> Generating $OUT_L16_LE"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f s16le -acodec pcm_s16le -ar 16000 -ac 1 \
    "$OUT_L16_LE"

# 6. L16 (Linear PCM 16-bit) - 16kHz Mono, Big-Endian
# FFmpeg format: s16be, codec: pcm_s16be
OUT_L16_BE="input_${FILENAME}_16000_be.l16"
echo "  -> Generating $OUT_L16_BE"
ffmpeg -v error -y -i "$INPUT_FILE" \
    -f s16be -acodec pcm_s16be -ar 16000 -ac 1 \
    "$OUT_L16_BE"

echo "Done."
