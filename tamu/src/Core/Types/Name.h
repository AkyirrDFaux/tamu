#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// File names: 8 chars alphanumeric + Space + '.' -> compressed into 6 bytes
// (each char is one of 64 symbols = 6 bits, 8 * 6 = 48 bits = 6 bytes)
#define NAME_CHARS 8
#define NAME_BYTES 6

// Encodes one name character into its 6-bit storage code (A-Z, a-z, 0-9, space, dot).
constexpr uint8_t EncodeNameChar(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(26 + c - 'a');
    if (c >= '0' && c <= '9') return (uint8_t)(52 + c - '0');
    if (c == ' ') return 62;
    if (c == '.') return 63;
    return 0;
}

// Decodes a 6-bit storage code back into its name character.
constexpr char DecodeNameChar(uint8_t v)
{
    if (v < 26) return (char)('A' + v);
    if (v < 52) return (char)('a' + v - 26);
    if (v < 62) return (char)('0' + v - 52);
    if (v == 62) return ' ';
    return '.';
}

// Packs an 8-char name (padded with spaces) into the 6-byte stored form
inline void EncodeName(const char *plain, uint8_t out[NAME_BYTES])
{
    uint8_t codes[NAME_CHARS] = {62, 62, 62, 62, 62, 62, 62, 62};
    for (int i = 0; i < NAME_CHARS && plain[i]; i++)
        codes[i] = EncodeNameChar(plain[i]);

    out[0] = (codes[0] << 2) | (codes[1] >> 4);
    out[1] = ((codes[1] & 0x0F) << 4) | (codes[2] >> 2);
    out[2] = ((codes[2] & 0x03) << 6) | codes[3];
    out[3] = (codes[4] << 2) | (codes[5] >> 4);
    out[4] = ((codes[5] & 0x0F) << 4) | (codes[6] >> 2);
    out[5] = ((codes[6] & 0x03) << 6) | codes[7];
}

// Unpacks the 6-byte stored form back into an 8-char name (plus null)
inline void DecodeName(const uint8_t in[NAME_BYTES], char out[NAME_CHARS + 1])
{
    uint8_t codes[NAME_CHARS];
    codes[0] = in[0] >> 2;
    codes[1] = ((in[0] & 0x03) << 4) | (in[1] >> 4);
    codes[2] = ((in[1] & 0x0F) << 2) | (in[2] >> 6);
    codes[3] = in[2] & 0x3F;
    codes[4] = in[3] >> 2;
    codes[5] = ((in[3] & 0x03) << 4) | (in[4] >> 4);
    codes[6] = ((in[4] & 0x0F) << 2) | (in[5] >> 6);
    codes[7] = in[5] & 0x3F;

    for (int i = 0; i < NAME_CHARS; i++)
        out[i] = DecodeNameChar(codes[i]);
    out[NAME_CHARS] = '\0';
}

// True when two 6-byte stored names are identical (memcmp helper).
inline bool NameEqual(const uint8_t a[NAME_BYTES], const uint8_t b[NAME_BYTES])
{
    return memcmp(a, b, NAME_BYTES) == 0;
}

// Compile-time encoded name. Construct from a string literal to bake the 6 bytes into
// read-only data with zero runtime work, e.g. `static constexpr StoredName table(".TABLE");`.
struct StoredName
{
    uint8_t bytes[NAME_BYTES];

    template <size_t N>
    constexpr StoredName(const char (&plain)[N]) : bytes{}
    {
        uint8_t codes[NAME_CHARS] = {62, 62, 62, 62, 62, 62, 62, 62};
        for (size_t i = 0; i < NAME_CHARS && i < N && plain[i]; i++)
            codes[i] = EncodeNameChar(plain[i]);

        bytes[0] = (uint8_t)((codes[0] << 2) | (codes[1] >> 4));
        bytes[1] = (uint8_t)(((codes[1] & 0x0F) << 4) | (codes[2] >> 2));
        bytes[2] = (uint8_t)(((codes[2] & 0x03) << 6) | codes[3]);
        bytes[3] = (uint8_t)((codes[4] << 2) | (codes[5] >> 4));
        bytes[4] = (uint8_t)(((codes[5] & 0x0F) << 4) | (codes[6] >> 2));
        bytes[5] = (uint8_t)(((codes[6] & 0x03) << 6) | codes[7]);
    }

    // Returns the packed bytes.
    constexpr const uint8_t *Data() const { return bytes; }

    // Compile-time equality against another stored name.
    constexpr bool operator==(const StoredName &other) const
    {
        for (size_t i = 0; i < NAME_BYTES; i++)
            if (bytes[i] != other.bytes[i]) return false;
        return true;
    }
};