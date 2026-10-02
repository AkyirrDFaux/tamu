#pragma once

#include <cstdint>
#include <cstring>

// Alignment-safe multi-byte access through byte pointers.
//
// Wire payloads, file buffers and the dynamic memory's value area are only byte-aligned, so a
// `*(uint32_t *)(buf + off)` with an arbitrary `off` is undefined behaviour. The ESP32's Xtensa
// happens to handle a misaligned word in hardware; the CH32V003 (RV32EC) raises a load/store
// fault on one and the node dies. GCC is entitled to compile the cast to a single `lw`/`sw`
// (it does), so every such access must go through these helpers, which memcpy through a
// properly-aligned temporary. Where alignment is provable the compiler emits the same code.
template <typename T>
static inline T LoadUnaligned(const void *p) {
    T v;
    // The void* cast keeps -Wclass-memaccess quiet for small POD-like classes (e.g. ColourClass,
    // whose user-defined operator= is not a copy-assignment); the byte copy is still correct.
    memcpy(static_cast<void *>(&v), p, sizeof(T));
    return v;
}

template <typename T>
static inline void StoreUnaligned(void *p, T v) {
    memcpy(p, static_cast<const void *>(&v), sizeof(T));
}
