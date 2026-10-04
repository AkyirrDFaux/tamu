#!/bin/bash
# Builds and runs the host-side firmware tests. Everything here is device-free, so it runs
# without hardware - which is how the numeric core, the offset tables and the OPTIMIZE_SPEED
# variants are kept honest.
#
#   numeric_test.cpp  Number.h + Vector/Matrix/Colour, in the three shapes the targets select
#   stride_test.cpp   the script VM's prefix-sum symbol offsets (Core/Functions/StrideOffsets.h)
#   crc_test.cpp      Crc8 (Core/Functions/Packet.h), both OPTIMIZE_SPEED shapes
#
# The numeric core is built in three shapes: the core (64-bit) and the DAS (32-bit, no
# Vector/Matrix) are the real targets; the third - 32-bit *with* Vector/Matrix - is a
# supported combination that neither target selects, so without it the isqrt32/FixedMul32
# path through sqrt() is never even compiled.
#
# OPTIMIZE_SPEED (TODO A9) selects the speed variants where a site has two shapes; the size
# variant is the default and the core defines it. The flag must not change any result, so the
# tests are built and run both ways and each must pass against the same assertions.
set -e

cd "$(dirname "$0")/../.."   # firmware/
ROOT="$PWD"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-g++}"
# TAMU_INT32_IS_INT: on the host int32_t is int, so Number's extra `int` constructor would be
# a redefinition. The embedded toolchains (where int32_t is long) do not define it.
FLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -DTAMU_INT32_IS_INT -I"$ROOT/src")

fail=0

# $1 name, $2 source, then the config flags. Built once per OPTIMIZE_SPEED state.
build_run() {
    local name="$1" src="$2"; shift 2
    if "$CXX" "${FLAGS[@]}" "$@" "$ROOT/test/native/$src" -o "$OUT/$name"; then
        "$OUT/$name" || fail=1
    else
        fail=1
    fi
}

for SPEED in "-DOPTIMIZE_SPEED" ""; do
    label="size variant"
    [ -n "$SPEED" ] && label="speed variant (-DOPTIMIZE_SPEED)"

    echo "### native numeric tests: core build (64-bit, Vector/Matrix), $label"
    build_run "numeric_core_${SPEED:+speed}" "numeric_test.cpp" $SPEED
    echo

    echo "### native numeric tests: 32-bit build (NUMBER_ONLY_32BIT, Vector/Matrix), $label"
    build_run "numeric_32_${SPEED:+speed}" "numeric_test.cpp" -DNUMBER_ONLY_32BIT $SPEED
    echo

    echo "### native numeric tests: DAS build (NUMBER_ONLY_32BIT + SCALAR_ONLY), $label"
    build_run "numeric_das_${SPEED:+speed}" "numeric_test.cpp" -DNUMBER_ONLY_32BIT -DSCALAR_ONLY $SPEED
    echo

    echo "### native crc8 tests (wire checksum), $label"
    build_run "crc_${SPEED:+speed}" "crc_test.cpp" $SPEED
    echo

    echo "### native geometry tests (LED display mask shapes), $label"
    build_run "geometry_${SPEED:+speed}" "geometry_test.cpp" $SPEED
    echo
done

echo "### native alignment tests (PacketFrame alignment + unaligned buffer access)"
build_run "align" "align_test.cpp"
echo

echo "### native stride-offset tests (script VM symbol resolution)"
build_run "stride" "stride_test.cpp"
echo

echo "### native bootloader packet tests (codec layout + even parity)"
build_run "bootloader" "bootloader_test.cpp"
echo

echo "### native TRID tests (System/Log counter range + increment/wrap)"
build_run "trid" "trid_test.cpp"
echo

echo "### native log layout tests (LogMessage source/category/specifics)"
build_run "log" "log_test.cpp"

exit $fail
