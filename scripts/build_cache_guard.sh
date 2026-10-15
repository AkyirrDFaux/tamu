#!/usr/bin/env bash
# Shared build-cache guard, sourced by app.sh and upload.sh.
#
# Why this exists
# ---------------
# This tree is edited by AI agents that may run builds as a DIFFERENT user than the
# owner. Generated caches then record that user's absolute HOME paths, and the owner's
# next build resolves packages out of a directory it does not own. The resulting errors
# look unrelated to the cause:
#
#   Flutter      "Matrix4 isn't a type"        - vector_math resolved into a foreign
#                                                $HOME/.pub-cache
#   PlatformIO   "the .../framework-espidf/components/riscv/linker.lf fragment is not
#                found!"                       - the ESP-IDF CMake cache points into a
#                                                foreign $HOME/.platformio/packages
#
# Both caches are regenerable, so the guard clears a poisoned one before the build runs.
# That makes the tools self-healing instead of failing with a misleading error.
#
# Escape hatch: TAMU_KEEP_CACHE=1 reports the problem and fails without deleting anything.

# Absolute home prefixes that end in the cache directories we care about.
# Covers /home/<user>, /Users/<user> (macOS) and /var/lib/<user> (this box's service users).
_CACHE_PATH_RE='/(?:home|Users|var/lib)/[^/]+(?=/\.(?:pub-cache|platformio))'

# _foreign_homes <path> -> distinct home prefixes in <path> that are not $HOME
_foreign_homes() {
    grep -rohIP "$_CACHE_PATH_RE" "$1" 2>/dev/null \
        | sort -u \
        | grep -vxF "$HOME" \
        || true
}

# guard_cache <label> <path>...
#   Scans each existing path for cached references to another user's HOME. On a hit it
#   reports the foreign home(s) and clears that path so the next build regenerates it.
#   Returns non-zero only when TAMU_KEEP_CACHE=1 (nothing was cleared).
guard_cache() {
    local label="$1"
    shift
    local path homes

    for path in "$@"; do
        [ -e "$path" ] || continue
        homes="$(_foreign_homes "$path")"
        [ -n "$homes" ] || continue

        {
            echo "==> cache guard ($label): $path was built by another user"
            printf '    foreign HOME in cache: %s\n' $homes
            echo "    current HOME: $HOME"
        } >&2

        if [ "${TAMU_KEEP_CACHE:-0}" = "1" ]; then
            echo "    TAMU_KEEP_CACHE=1: nothing cleared. Remove it with: rm -rf '$path'" >&2
            return 1
        fi

        echo "    clearing it so the build regenerates against \$HOME" >&2
        rm -rf "$path"
    done

    return 0
}
