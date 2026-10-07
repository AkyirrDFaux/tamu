#!/usr/bin/env bash
#
# Tamu flash helper - build and upload the Tamu core, DAS and Valu v2.0 firmware.
#
#   ./upload.sh app [tamu|das|valu]        build + upload the application(s)  (default: both)
#   ./upload.sh boot [tamu|das|valu] [-y]  build + upload the bootloader(s)   (default: both)
#   ./upload.sh all [tamu|das|valu] [-y]   build + upload bootloader(s) then application(s)
#   ./upload.sh build [tamu|das|valu]      build only, no upload              (default: both)
#   ./upload.sh bin tamu [file] [off]      flash a raw .bin via esptool     (default: app @ 0x70000)
#   ./upload.sh bin das  [file] [off]      flash a raw .bin via minichlink  (default: app @ 0x800)
#   ./upload.sh bin valu [file] [off]      flash a raw .bin via wlink       (default: bootloader @ 0x0)
#   ./upload.sh ports                      list the serial ports the tools would use
#   ./upload.sh help                       this text
#
# `app`/`bin` write only the application slot, so the bootloader is preserved; only `boot`
# and `all` overwrite a bootloader. `valu` builds and flashes over the WCH-Link debug probe
# (SWD) with scripts/valu_upload.py, and is never part of the implicit `both` set.
# Run `./upload.sh help` for the full list and overrides.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
FW="$ROOT/firmware"
PIO="${PIO:-pio}"

# Optional port overrides. Empty = let the tool autodetect (PlatformIO for `app`/`boot`,
# minichlink/esptool for `bin`).
TAMU_PORT="${TAMU_PORT:-}"
DAS_PORT="${DAS_PORT:-}"

# Tool locations (PlatformIO's bundled copies; the CLI is used first when on PATH).
ESPTOOL_PY="${ESPTOOL_PY:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"
MINICHLINK="${MINICHLINK:-$HOME/.platformio/packages/tool-minichlink/minichlink}"
WLINK="${WLINK:-$HOME/.platformio/packages/tool-wlink/wlink}"

# Device -> PlatformIO environment names.
TAMU_APP_ENV=Tamu_v2_0A
TAMU_BOOT_ENV=Tamu_bootloader
DAS_APP_ENV=DAS_v0_1
DAS_BOOT_ENV=DAS_bootloader
VALU_APP_ENV=Valu_v2_0
VALU_BOOT_ENV=Valu_bootloader

# Raw-bin defaults (the address each app image is linked/flashed at; the bootloaders are
# never touched by these defaults).
TAMU_APP_OFFSET=0x70000   # ota_0, derived from partitions.csv (scripts/core_app_offset.py)
DAS_APP_OFFSET=0x800      # app base; the DAS bootloader owns 0x0-0x800
VALU_BOOT_OFFSET=0x0      # bootloader at the bottom of flash (12 KB: 0x0-0x3000)
VALU_APP_OFFSET=0x3000    # app base; the Valu bootloader owns 0x0-0x3000

# The Valu has no PlatformIO uploader: its images go in over the WCH-Link probe (SWD), which is
# also the only way to get the very first bootloader onto the chip.
VALU_UPLOAD="$FW/scripts/valu_upload.py"

die() { echo "error: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Tamu flash helper - build and upload the Tamu core, DAS and Valu v2.0 firmware.

Usage: ./upload.sh <command> [device] [args]

Commands:
  app   [tamu|das|valu]       Build and upload the application(s).        (default: both)
  boot  [tamu|das|valu] [-y]  Build and upload the bootloader(s).         (default: both)
  all   [tamu|das|valu] [-y]  Build and upload the bootloader(s) then the application(s).
  build [tamu|das|valu]       Build only, no upload.                      (default: both)
  bin   <tamu|das|valu> [file] [offset]
                              Flash a raw image. `file` defaults to the built image, `offset`
                              to the device's default (tamu app 0x70000, DAS app 0x800,
                              valu bootloader 0x0).
  ports                       List /dev/ttyACM* and /dev/ttyUSB*.
  help                        Show this help.

Devices:
  tamu   ->  Tamu_v2_0A app          / Tamu_bootloader
  das    ->  DAS_v0_1 app            / DAS_bootloader
  valu   ->  Valu_v2_0 app           / Valu_bootloader     (CH32V203G8R6)

The app commands write only the application slot (tamu ota_0, DAS 0x800), so the
bootloader is preserved; only `boot` and `all` overwrite a bootloader. `valu` is
flashed over the WCH-Link debug probe (SWD) with scripts/valu_upload.py and is never
part of the implicit `both` set - name it explicitly.

Examples:
  ./upload.sh app                 # reflash both applications
  ./upload.sh boot                # reflash both bootloaders (prompts; -y to skip)
  ./upload.sh all                 # reflash both bootloaders + both applications
  ./upload.sh all tamu            # Tamu bootloader + Tamu application
  ./upload.sh app das             # reflash just the DAS application
  ./upload.sh boot valu           # build + flash the Valu bootloader over the WCH-Link
  ./upload.sh build valu          # compile the Valu app + bootloader, no upload
  ./upload.sh bin valu out.bin 0x2000
  ./upload.sh bin das             # flash the built DAS app image
  ./upload.sh bin tamu app.bin 0x70000
  ./upload.sh bin tamu bootloader.bin 0x10000

Port overrides (else autodetected):
  TAMU_PORT=/dev/ttyACM1      Tamu core USB-serial port
  DAS_PORT=/dev/ttyACM0       WCH-Link port for the DAS
  WLINK=/path/to/wlink        wlink binary for the Valu probe flash
  PIO=pio                     PlatformIO command

Typical rig: Tamu core on /dev/ttyACM1, DAS WCH-Link on /dev/ttyACM0:
  TAMU_PORT=/dev/ttyACM1 DAS_PORT=/dev/ttyACM0 ./upload.sh app
EOF
}

pio_run() { # <env> [pio args...]
    local env="$1"; shift
    local port_args=()
    case "$env" in
        "$TAMU_APP_ENV" | "$TAMU_BOOT_ENV") [ -n "$TAMU_PORT" ] && port_args=(--upload-port "$TAMU_PORT") ;;
        "$VALU_APP_ENV" | "$VALU_BOOT_ENV") ;;  # flashed over SWD by valu_upload.py, no serial port
        *)                                   [ -n "$DAS_PORT" ]  && port_args=(--upload-port "$DAS_PORT") ;;
    esac
    echo "==> $env $*"
    ( cd "$FW" && "$PIO" run -e "$env" "$@" "${port_args[@]}" )
}

app_env_for()  { case "$1" in tamu) echo "$TAMU_APP_ENV" ;; das) echo "$DAS_APP_ENV" ;; valu) echo "$VALU_APP_ENV" ;; esac; }
boot_env_for() { case "$1" in tamu) echo "$TAMU_BOOT_ENV" ;; das) echo "$DAS_BOOT_ENV" ;; valu) echo "$VALU_BOOT_ENV" ;; esac; }

# Flash a Valu image over the WCH-Link probe (SWD) - the Valu's only upload path.
valu_upload() { # <image> <offset>
    local image="$1" off="$2"
    [ -f "$VALU_UPLOAD" ] || die "valu_upload.py not found at $VALU_UPLOAD"
    echo "==> wlink (valu): $image -> $off"
    python3 "$VALU_UPLOAD" "$image" "$off" --wlink "$WLINK"
}

devices() { # <tamu|das|all> -> space-separated list
    if [ "$1" = all ]; then echo "tamu das"; else echo "$1"; fi
}

confirm_flash() { # <assume-y> <dev> <what>
    [ "$1" = 1 ] && return 0
    read -r -p "Reflash $3 for '$2'? This overwrites the bootloader. [y/N] " ans
    [[ "$ans" =~ ^[Yy] ]]
}

do_upload() { # <app|boot> <tamu|das|valu|all>
    local kind="$1" dev="${2:-all}" d env
    for d in $(devices "$dev"); do
        if [ "$kind" = app ]; then env="$(app_env_for "$d")"; else env="$(boot_env_for "$d")"; fi
        [ -n "$env" ] || die "unknown device '$d' (use tamu|das|valu|all)"
        if [ "$d" = valu ]; then
            # No PlatformIO uploader: build, then flash the probe-side image with valu_upload.py.
            pio_run "$env"
            if [ "$kind" = app ]; then
                valu_upload "$FW/.pio/build/$env/firmware.bin" "$VALU_APP_OFFSET"
            else
                valu_upload "$FW/.pio/build/$env/firmware.bin" "$VALU_BOOT_OFFSET"
            fi
        else
            pio_run "$env" -t upload
        fi
    done
}

do_build() { # <tamu|das|all>
    local dev="${1:-all}" d env
    for d in $(devices "$dev"); do
        env="$(app_env_for "$d")";  [ -n "$env" ] || die "unknown device '$d'"; pio_run "$env"
        env="$(boot_env_for "$d")"; pio_run "$env"
    done
}

do_bin() { # <tamu|das> [file] [offset]
    local dev="${1:-}" file off
    [ -n "$dev" ] || die "usage: ./upload.sh bin <tamu|das> [file] [offset]"
    shift
    case "$dev" in
        tamu)
            file="${1:-$FW/.pio/build/$TAMU_APP_ENV/firmware.bin}"
            off="${2:-$TAMU_APP_OFFSET}"
            [ -f "$file" ] || die "no binary '$file' (build it first: ./upload.sh app tamu)"
            local args=(--chip esp32c3 --baud 921600)
            [ -n "$TAMU_PORT" ] && args+=(--port "$TAMU_PORT")
            args+=(write_flash "$off" "$file")
            echo "==> esptool: $file -> $off"
            if command -v esptool >/dev/null 2>&1; then
                esptool "${args[@]}"
            elif [ -f "$ESPTOOL_PY" ]; then
                python3 "$ESPTOOL_PY" "${args[@]}"
            else
                die "esptool not found (set ESPTOOL_PY)"
            fi
            ;;
        das)
            file="${1:-$FW/.pio/build/$DAS_APP_ENV/firmware.bin}"
            off="${2:-$DAS_APP_OFFSET}"
            [ -f "$file" ] || die "no binary '$file' (build it first: ./upload.sh app das)"
            [ -x "$MINICHLINK" ] || die "minichlink not found at $MINICHLINK (set MINICHLINK)"
            local args=()
            [ -n "$DAS_PORT" ] && args+=(-c "$DAS_PORT")
            args+=(-w "$file" "flash+$off" -b)
            echo "==> minichlink: $file -> flash+$off"
            "$MINICHLINK" "${args[@]}"
            ;;
        valu)
            file="${1:-$FW/.pio/build/$VALU_BOOT_ENV/firmware.bin}"
            off="${2:-$VALU_BOOT_OFFSET}"
            [ -f "$file" ] || die "no binary '$file' (build it first: ./upload.sh boot valu)"
            valu_upload "$file" "$off"
            ;;
        *)
            die "unknown device '$dev' (use tamu|das|valu)"
            ;;
    esac
}

cmd="${1:-help}"
case "$cmd" in
    help | -h | --help | "")
        usage
        ;;
    app | flash)
        do_upload app "${2:-all}"
        ;;
    boot | bootloader)
        dev=all
        assume="${ASSUME_YES:-0}"
        for a in "${@:2}"; do
            case "$a" in
                -y | --yes) assume=1 ;;
                tamu | das | valu | all) dev="$a" ;;
                *) die "unknown boot option '$a' (use tamu|das|valu|all|-y)" ;;
            esac
        done
        confirm_flash "$assume" "$dev" "bootloader(s)" || { echo "aborted"; exit 1; }
        do_upload boot "$dev"
        ;;
    all | everything)
        dev=all
        assume="${ASSUME_YES:-0}"
        for a in "${@:2}"; do
            case "$a" in
                -y | --yes) assume=1 ;;
                tamu | das | valu | all) dev="$a" ;;
                *) die "unknown all option '$a' (use tamu|das|valu|all|-y)" ;;
            esac
        done
        confirm_flash "$assume" "$dev" "bootloader(s) and application(s)" || { echo "aborted"; exit 1; }
        do_upload boot "$dev"
        do_upload app "$dev"
        ;;
    build)
        do_build "${2:-all}"
        ;;
    bin | install)
        do_bin "${@:2}"
        ;;
    ports)
        shopt -s nullglob
        found=(/dev/ttyACM* /dev/ttyUSB*)
        if [ "${#found[@]}" -eq 0 ]; then
            echo "no /dev/ttyACM* or /dev/ttyUSB* found"
        else
            ls -l "${found[@]}"
        fi
        ;;
    *)
        echo "unknown command: $cmd" >&2
        echo >&2
        usage >&2
        exit 2
        ;;
esac
