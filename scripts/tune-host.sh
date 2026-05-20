#!/bin/bash

# MachAudio Host Tuning Script
# This script tunes the host system for low-latency audio processing.
# Requires root privileges.

if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root"
   exit 1
fi

TARGET_CORE=${1:-0}

echo "Tuning host for low-latency on core ${TARGET_CORE}..."

# 1. Set scaling governor to performance
if [ -f "/sys/devices/system/cpu/cpu${TARGET_CORE}/cpufreq/scaling_governor" ]; then
    echo performance > "/sys/devices/system/cpu/cpu${TARGET_CORE}/cpufreq/scaling_governor"
    echo "  - CPU ${TARGET_CORE} scaling governor set to performance."
else
    echo "  - Warning: CPU ${TARGET_CORE} scaling governor not found."
fi

# 2. Disable CPU DMA latency (C-states)
# Note: This requires the process writing to it to stay alive. 
# We'll just document it here.
echo "  - To restrict deep C-states, you can run a small program that opens /dev/cpu_dma_latency"
echo "    and writes a 32-bit 0 to it while the server is running."

# 3. IRQ Balance
# Ideally we should move IRQs away from the target core.
# For simplicity, we just remind the user.
echo "  - Consider disabling irqbalance or pinning IRQs away from core ${TARGET_CORE}."

echo "Host tuning complete."
