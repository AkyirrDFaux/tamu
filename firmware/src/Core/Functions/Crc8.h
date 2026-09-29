#pragma once

#include <cstdint>

// Wire checksum (Specs/Data Formats.md): CRC-8, poly 0x07, init 0x00, no reflection, no final
// xor. Kept out of Packet.h so it stays host-testable on its own (see test/native/crc_test.cpp)
// - Packet.h pulls in the device status the frame carries.
//
// Two shapes, selected by OPTIMIZE_SPEED: the table costs 256 bytes of flash (read-only, so it
// lands in flash, not RAM) and one lookup per byte; the loop costs eight iterations per byte.
// The CRC covers every packet and every USB chunk, so the core trades the flash for it, while
// the DAS keeps the loop. This is the only site where the two targets genuinely diverge - see
// TODO A9 for why Fnv1a, the fixed-point division and the log/pow10 polynomials have no useful
// second shape.
//
// The table is generated from the same step function by constexpr rather than listed, so the
// two shapes cannot disagree; test/native/crc_test.cpp additionally checks both against an
// independent reference.
#ifdef OPTIMIZE_SPEED
namespace PacketDetail
{
constexpr uint8_t Crc8Step(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t j = 0; j < 8; j++)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    return crc;
}
struct Crc8Table
{
    uint8_t entry[256];
    constexpr Crc8Table() : entry()
    {
        for (int i = 0; i < 256; i++) entry[i] = Crc8Step(0, (uint8_t)i);
    }
};
inline constexpr Crc8Table kCrc8Table{};
} // namespace PacketDetail
#endif

inline uint8_t Crc8(const uint8_t *data, uint16_t len)
{
#ifdef OPTIMIZE_SPEED
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
        crc = PacketDetail::kCrc8Table.entry[crc ^ data[i]];
    return crc;
#else
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
#endif
}

