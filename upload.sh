#!/usr/bin/env bash
#
# Tamu flash helper - build and upload the Tamu core and DAS firmware.
#
#   ./upload.sh app [tamu|das]         build + upload the application(s)   (default: both)
#   ./upload.sh boot [tamu|das] [-y]   build + upload the bootloader(s)    (default: both)
#   ./upload.sh build [tamu|das]       build only, no upload               (default: both)
#   ./upload.sh bin tamu [file] [off]  flash a raw .bin via esptool   (default: app @ 0x70000)
#   ./upload.sh bin das  [file] [off]  flash a raw .bin via minichlink (default: app @ 0x800)
#   ./upload.sh ports                  list the serial ports the tools would use
#   ./upload.sh help                   this text
#
# `app`/`bin` write only the application slot, so the bootloader is preserved; only `boot`
# overwrites a bootloader. Run `./upload.sh help` for the full list and port overrides.
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

# Device -> PlatformIO environment names.
TAMU_APP_ENV=Tamu_v2_0A
TAMU_BOOT_ENV=Tamu_bootloader
DAS_APP_ENV=DAS_v0_1
DAS_BOOT_ENV=DAS_bootloader

# Raw-bin defaults (the address each app image is linked/flashed at; the bootloaders are
# never touched by these defaults).
TAMU_APP_OFFSET=0x70000   # ota_0, derived from partitions.csv (scripts/core_app_offset.py)
DAS_APP_OFFSET=0x800      # app base; the DAS bootloader owns 0x0-0x800

die() { echo "error: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Tamu flash helper - build and upload the Tamu core and DAS firmware.

Usage: ./upload.sh <command> [device] [args]

Commands:
  app   [tamu|das]            Build and upload the application(s).        (default: both)
  boot  [tamu|das] [-y]       Build and upload the bootloader(s).         (default: both)
  build [tamu|das]            Build only, no upload.                      (default: both)
  bin   <tamu|das> [file] [offset]
                              Flash a raw .bin. `file` defaults to the built app image,
                              `offset` to the device's app address (tamu 0x70000, DAS 0x800).
  ports                       List /dev/ttyACM* and /dev/ttyUSB*.
  help                        Show this help.

Devices:
  tamu   ->  Tamu_v2_0A app          / Tamu_bootloader
  das    ->  DAS_v0_1 app            / DAS_bootloader

The app commands write only the application slot (tamu ota_0, DAS 0x800), so the
bootloader is preserved; only `boot` overwrites a bootloader.

Examples:
  ./upload.sh app                 # reflash both applications
  ./upload.sh boot                # reflash both bootloaders (prompts; -y to skip)
  ./upload.sh app das             # reflash just the DAS application
  ./upload.sh build tamu          # compile Tamu app + bootloader, no upload
  ./upload.sh bin das             # flash the built DAS app image
  ./upload.sh bin tamu app.bin 0x70000
  ./upload.sh bin tamu bootloader.bin 0x10000

Port overrides (else autodetected):
  TAMU_PORT=/dev/ttyACM1      Tamu core USB-serial port
  DAS_PORT=/dev/ttyACM0       WCH-Link port for the DAS
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
        *)                                   [ -n "$DAS_PORT" ]  && port_args=(--upload-port "$DAS_PORT") ;;
    esac
    echo "==> $env $*"
    ( cd "$FW" && "$PIO" run -e "$env" "$@" "${port_args[@]}" )
}

app_env_for()  { case "$1" in tamu) echo "$TAMU_APP_ENV" ;; das) echo "$DAS_APP_ENV" ;; esac; }
boot_env_for() { case "$1" in tamu) echo "$TAMU_BOOT_ENV" ;; das) echo "$DAS_BOOT_ENV" ;; esac; }

devices() { # <tamu|das|all> -> space-separated list
    if [ "$1" = all ]; then echo "tamu das"; else echo "$1"; fi
}

do_upload() { # <app|boot> <tamu|das|all>
    local kind="$1" dev="${2:-all}" d env
    for d in $(devices "$dev"); do
        if [ "$kind" = app ]; then env="$(app_env_for "$d")"; else env="$(boot_env_for "$d")"; fi
        [ -n "$env" ] || die "unknown device '$d' (use tamu|das|all)"
        pio_run "$env" -t upload
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
        *)
            die "unknown device '$dev' (use tamu|das)"
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
                tamu | das | all) dev="$a" ;;
                *) die "unknown boot option '$a' (use tamu|das|all|-y)" ;;
            esac
        done
        if [ "$assume" != 1 ]; then
            read -r -p "Reflash bootloader(s) for '$dev'? This overwrites the bootloader. [y/N] " ans
            [[ "$ans" =~ ^[Yy] ]] || { echo "aborted"; exit 1; }
        fi
        do_upload boot "$dev"
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
