#!/usr/bin/env bash
set -euo pipefail

# ── Somfy Sonesse2 BLE Calibration Tool ── Flash Script ──────────
#
# Compiles and flashes the firmware to an ESP32 board via arduino-cli.
#
# Usage:
#   ./flash.sh                  # auto-detect port, default board
#   ./flash.sh /dev/ttyUSB1     # specify port
#   ./flash.sh /dev/ttyUSB0 esp32:esp32:esp32  # specify port and FQBN
#
# Requirements:
#   - arduino-cli (https://arduino.github.io/arduino-cli/)
#   - ESP32 Arduino core: arduino-cli core install esp32:esp32
#
# After flashing, connect with:
#   picocom /dev/ttyUSB0 -b 115200
#   screen /dev/ttyUSB0 115200
#   minicom -D /dev/ttyUSB0 -b 115200

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH_DIR="${SCRIPT_DIR}/somfy-ble-cli"
SKETCH="${SKETCH_DIR}/somfy-ble-cli.ino"

# ── Defaults ──────────────────────────────────────────────────────
DEFAULT_FQBN="esp32:esp32:m5stack_atom"
BAUD=115200

# ── Colors ────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR]${NC}   $*" >&2; }
die()   { err "$@"; exit 1; }

# ── Preflight ─────────────────────────────────────────────────────

command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli not found. Install from https://arduino.github.io/arduino-cli/"

if [ ! -f "${SKETCH}" ]; then
    die "Sketch not found at ${SKETCH}"
fi

# ── Parse args ────────────────────────────────────────────────────

PORT="${1:-}"
FQBN="${2:-${DEFAULT_FQBN}}"

# Auto-detect port if not specified
if [ -z "${PORT}" ]; then
    info "Auto-detecting serial port..."

    # Look for common ESP32 serial devices
    for candidate in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
        if [ -e "${candidate}" ]; then
            PORT="${candidate}"
            break
        fi
    done

    # macOS
    if [ -z "${PORT}" ]; then
        for candidate in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial*; do
            if [ -e "${candidate}" ]; then
                PORT="${candidate}"
                break
            fi
        done
    fi

    if [ -z "${PORT}" ]; then
        die "No serial port detected. Plug in the ESP32 and try again, or specify the port:\n  $0 /dev/ttyUSB0"
    fi

    ok "Detected port: ${PORT}"
fi

if [ ! -e "${PORT}" ]; then
    die "Port ${PORT} does not exist. Is the ESP32 plugged in?"
fi

# ── Check ESP32 core ──────────────────────────────────────────────

info "Checking ESP32 Arduino core..."
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32"; then
    warn "ESP32 core not installed. Installing..."
    arduino-cli config add board_manager.additional_urls \
        https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json 2>/dev/null || true
    arduino-cli core install esp32:esp32 || die "Failed to install ESP32 core"
    ok "ESP32 core installed"
else
    ok "ESP32 core present"
fi

# ── Compile ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}── Compiling ──────────────────────────────────────${NC}"
info "Board FQBN: ${FQBN}"
info "Sketch:     ${SKETCH}"
echo ""

arduino-cli compile --fqbn "${FQBN}" "${SKETCH}" || die "Compilation failed"

echo ""
ok "Compilation successful"

# ── Flash ─────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}── Flashing ───────────────────────────────────────${NC}"
info "Port: ${PORT}"
echo ""

arduino-cli upload --fqbn "${FQBN}" --port "${PORT}" "${SKETCH}" || die "Upload failed"

echo ""
ok "Flash complete!"

# ── Done ──────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}── Ready ──────────────────────────────────────────${NC}"
echo ""
echo -e "  Connect to the CLI with:"
echo ""
echo -e "    ${CYAN}picocom ${PORT} -b ${BAUD}${NC}"
echo ""
echo -e "  Or:"
echo ""
echo -e "    ${CYAN}screen ${PORT} ${BAUD}${NC}"
echo -e "    ${CYAN}minicom -D ${PORT} -b ${BAUD}${NC}"
echo ""
echo -e "  Type ${BOLD}help${NC} in the CLI for commands."
echo ""
