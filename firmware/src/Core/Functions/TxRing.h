#pragma once

// App-link TX ring geometry (Core/Functions/AppInterface.h owns the ring itself).
//
// The ring indexes are 16-bit FREE-RUNNING counters (TxHead/TxTail): they are not kept in
// [0, SIZE) - they just count bytes and wrap at 65536. That is what makes "empty" one
// comparison (head == tail) and the byte count one subtraction. It also means a position may
// only ever be computed from a 16-bit counter: 65536 is not a multiple of APP_TX_RING_SIZE
// (65536 % 24576 = 16384), so a sum taken in a wider type lands 16384 bytes away from where the
// writer put the byte.
//
// Getting that wrong is not theoretical. `TxPeek` used to index with `(TxTail + Count) % SIZE`
// - an `int` sum, which does not wrap when the counter does - while `Send` wrote at
// `TxHead % SIZE` from the uint16 counter. Every ~3 whole-file replies (a `.TABLE` reply is
// 22,528 B) the counter crossed 65536 mid-notification and the reader handed out ring bytes
// 16384 positions away: a stalled `.TABLE  ` read then carried whole frames of EARLIER content
// where later fragments belong, lost those fragments, and still measured 22,528 B with its STOP
// frame (Issues.md E75). Measured on the rig at 4 of 10 reads; the 8 KiB ring this replaced
// divides 65536 exactly, which is why the defect only appeared after the ring grew.
//
// So: every position, writer and reader alike, comes from AppTxRingPos() below.
#include <cstdint>

// Outgoing stream ring size.
//
// A whole-file reply must be able to sit in the ring in one burst. The largest file the device
// serves the app is `.TABLE`, measured on the rig as 256 fragments of 64 B content (16,384 B of
// file, 22,528 B on the wire: each fragment carries a 12 B header, 4 B frag info and the 8 B
// name echo). 24 KiB holds that whole reply.
//
// The ring is NOT what makes a long reply complete: the storage read keeps a cursor and emits a
// few fragments per app-task pass (Core/Services/Storage.h), so a reply of any size survives a
// link that stops accepting notifications. The ring only absorbs the burst between two drains.
//
// Cost: malloc'd once in AppInterfaceService::Init(). Measured free heap on the rig before the
// increase (iteration 26.11.4.5, System field 4): 97,048 B; with this size: 80,560-80,752 B.
#define APP_TX_RING_SIZE 24576

// Ring position of a free-running 16-bit counter.
//
// The argument is uint16_t on purpose: the wrap at 65536 has to happen BEFORE the modulo, so
// reader and writer agree on where a byte lives. Callers pass their counter as-is (or the sum
// already narrowed to uint16_t).
inline uint16_t AppTxRingPos(uint16_t Counter)
{
    return (uint16_t)(Counter % APP_TX_RING_SIZE);
}

// The used-byte count is a uint16_t difference, so a window of pending bytes must stay under
// 65536. One ring of slack short of that is plenty - the room check never lets the ring fill.
static_assert(APP_TX_RING_SIZE <= 32768, "APP_TX_RING_SIZE must leave the uint16 counter room to wrap once");
