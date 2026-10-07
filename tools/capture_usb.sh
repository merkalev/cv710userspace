#!/usr/bin/env bash
# Capture CV710 USB traffic on Linux using usbmon2
# Usage: sudo ./tools/capture_usb.sh [output_filename.pcapng]

set -euo pipefail

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
  echo "Error: This script must be run as root to access usbmon."
  echo "Usage: sudo $0 [output_filename.pcapng]"
  exit 1
fi

modprobe usbmon || true

OUTPUT_FILE="${1:-cv710_res_switch.pcapng}"

echo "Starting USB capture on usbmon2..."
echo "Saving to: ${OUTPUT_FILE}"
echo "Press Ctrl+C to stop capture when done."

dumpcap -i usbmon2 -w "${OUTPUT_FILE}"
