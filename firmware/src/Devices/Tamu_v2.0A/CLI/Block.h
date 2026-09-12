#pragma once

#include "Handler.h"

// BlockInfo helpers for the CLI (the Register service addresses blocks via a 32-bit
// BlockInfo: Type10 | Instance6 | Field8 | Key8, not the old BlockIndex).
static inline uint32_t CliBlockInfo(uint16_t type, uint8_t inst, uint8_t field, uint8_t key)
{
    return ((uint32_t)(type & 0x3FF) << 22) | ((uint32_t)(inst & 0x3F) << 16) | ((uint32_t)field << 8) | key;
}
static inline uint16_t CliBlockInfoType(uint32_t bi) { return (bi >> 22) & 0x3FF; }
static inline uint8_t CliBlockInfoInst(uint32_t bi) { return (bi >> 16) & 0x3F; }
static inline uint8_t CliBlockInfoField(uint32_t bi) { return (bi >> 8) & 0xFF; }
static inline uint8_t CliBlockInfoKey(uint32_t bi) { return bi & 0xFF; }

// Converts a float to a fixed-point Number (Q16.16) with rounding. Saturates instead of
// invoking UB through an out-of-range int32_t cast.
Number FloatToNumber(float f)
{
    Number number;
    float scaled = f * 65536.0f + (f >= 0 ? 0.5f : -0.5f);
    if (scaled >= 2147483647.0f)
        number.Value = 2147483647;
    else if (scaled <= -2147483648.0f)
        number.Value = -2147483648;
    else
        number.Value = static_cast<int32_t>(scaled);
    return number;
}

// Converts a fixed-point Number back to a float.
float NumberToFloat(Number n)
{
    return static_cast<float>(n.Value) / 65536.0f;
}

#include <cstdio>
#include <cinttypes>

// Prints a single field value to stdout, formatted by its data type.
void PrintValue(uint16_t type_id, const void *data_ptr, uint16_t data_len)
{
    switch (static_cast<DataType>(type_id))
    {
    case DataType::SN:
    {
        const uint8_t *sn = static_cast<const uint8_t *>(data_ptr);
        char sn_str[29] = {0};
        for (int i = 0; i < 14 && i < data_len; ++i)
            snprintf(&sn_str[i * 2], 3, "%02X", sn[i]);
        printf("Serial: %s\n", sn_str);
        break;
    }
    case DataType::Uint32:
        printf("Uint32: %" PRIu32 "\n", *static_cast<const uint32_t *>(data_ptr));
        break;
    case DataType::Number:
        printf("Number: %.4f\n", NumberToFloat(*static_cast<const Number *>(data_ptr)));
        break;
    case DataType::DevType:
        printf("DevType: %d\n", static_cast<int>(*static_cast<const DeviceType *>(data_ptr)));
        break;
    case DataType::NetAddr:
        printf("NetAddr: 0x%04X\n", *static_cast<const uint16_t *>(data_ptr));
        break;
    case DataType::Bool:
        printf("Bool: %s\n", (*static_cast<const bool *>(data_ptr) ? "true" : "false"));
        break;
    case DataType::Vector:
    {
        size_t vector_size = data_len / sizeof(Number);
        const Number *data_arr = static_cast<const Number *>(data_ptr);
        char buffer[128] = {0};
        int offset = 0;
        for (size_t i = 0; i < vector_size && offset < 100; ++i)
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%.2f ", NumberToFloat(data_arr[i]));
        printf("Vector[%zu]: %s\n", vector_size, buffer);
        break;
    }
    case DataType::Enum:
        printf("Enum: %d\n", *static_cast<const uint8_t *>(data_ptr));
        break;
    case DataType::Colour:
    {
        const ColourClass *c = static_cast<const ColourClass *>(data_ptr);
        printf("Colour: R:%d G:%d B:%d A:%d\n", c->R, c->G, c->B, c->A);
        break;
    }
    case DataType::String:
    {
        // Fixed-size string fields are space-padded, not NUL-terminated.
        char tmp[64];
        uint16_t n = data_len > sizeof(tmp) - 1 ? sizeof(tmp) - 1 : data_len;
        memcpy(tmp, data_ptr, n);
        tmp[n] = '\0';
        while (n > 0 && tmp[n - 1] == ' ') tmp[--n] = '\0';
        printf("String: \"%s\"\n", tmp);
        break;
    }
    case DataType::Matrix:
    {
        struct MatrixHeader
        {
            uint16_t h, w;
            Number d[9];
        };
        if (data_len < sizeof(MatrixHeader))
        {
            printf("Matrix: (truncated, %u bytes)\n", data_len);
            break;
        }
        const MatrixHeader *m = static_cast<const MatrixHeader *>(data_ptr);
        printf("Matrix [%dx%d] Data: ", m->h, m->w);
        for (int i = 0; i < (m->h * m->w) && i < 9; ++i)
            printf("%.2f ", NumberToFloat(m->d[i]));
        printf("\n");
        break;
    }
    default:
        printf("Unknown (0x%03X)\n", type_id);
    }
}

// Prints a field or key entry with its flags, index and value.
void PrintField(const BlockMeta &desc, const void *data_ptr, uint16_t index, bool is_key)
{
    uint16_t flags = BlockMetaFlags(desc.FlagsAndType);
    char flag_str[32] = {0};
    if (flags & FieldFlags::ReadOnly)     strcat(flag_str, "[RO] ");
    if (flags & FieldFlags::NotSaved)     strcat(flag_str, "[NS] ");
    if (flags & FieldFlags::ScriptUpdated) strcat(flag_str, "[SC] ");

    if (is_key)
        printf("    |-- Key [%02d]: ", index);
    else
        printf("  |-- Field [%02d] %-15s ", index, flag_str);

    if (data_ptr != nullptr)
        PrintValue(BlockMetaType(desc.FlagsAndType), data_ptr, desc.Size);
    else
        printf("(No Data)\n");
}

// Helper function to parse CLI strings into raw data buffers
bool ParseCLIValue(uint16_t type, const char *val_str, void *out_buffer, uint8_t &out_len)
{
    switch (static_cast<DataType>(type))
    {
    case DataType::Bool:
    {
        bool value = (atoi(val_str) != 0);
        memcpy(out_buffer, &value, sizeof(bool));
        out_len = sizeof(bool);
        return true;
    }
    case DataType::Uint32:
    {
        uint32_t value = static_cast<uint32_t>(strtoul(val_str, nullptr, 10));
        memcpy(out_buffer, &value, sizeof(uint32_t));
        out_len = sizeof(uint32_t);
        return true;
    }
    case DataType::NetAddr:
    {
        uint16_t value = static_cast<uint16_t>(strtoul(val_str, nullptr, 16));
        memcpy(out_buffer, &value, sizeof(uint16_t));
        out_len = sizeof(uint16_t);
        return true;
    }
    case DataType::DevType:
    {
        uint16_t dev_type = static_cast<uint16_t>(strtoul(val_str, nullptr, 0));
        memcpy(out_buffer, &dev_type, sizeof(uint16_t));
        out_len = sizeof(uint16_t);
        return true;
    }
    case DataType::Index:
    {
        int32_t value = static_cast<int32_t>(strtol(val_str, nullptr, 10));
        memcpy(out_buffer, &value, sizeof(int32_t));
        out_len = sizeof(int32_t);
        return true;
    }
    case DataType::Enum:
    {
        uint8_t value = static_cast<uint8_t>(strtoul(val_str, nullptr, 10));
        memcpy(out_buffer, &value, sizeof(uint8_t));
        out_len = sizeof(uint8_t);
        return true;
    }
    case DataType::SN:
    {
        // 14-byte serial number given as 28 hex chars.
        uint8_t serial[14] = {0};
        size_t hex_len = strlen(val_str);
        if (hex_len > 28) hex_len = 28;
        for (size_t i = 0; i + 1 < hex_len; i += 2)
        {
            char byte_str[3] = {val_str[i], val_str[i + 1], '\0'};
            serial[i / 2] = static_cast<uint8_t>(strtoul(byte_str, nullptr, 16));
        }
        memcpy(out_buffer, serial, sizeof(serial));
        out_len = sizeof(serial);
        return true;
    }
    case DataType::String:
    {
        size_t length = strlen(val_str);
        if (length > 127) length = 127;
        memcpy(out_buffer, val_str, length);
        out_len = static_cast<uint8_t>(length);
        return true;
    }
    case DataType::Colour:
    {
        // "R,G,B,A" (0-255 each, A defaults to 255).
        char temp_str[32];
        strncpy(temp_str, val_str, sizeof(temp_str) - 1);
        temp_str[sizeof(temp_str) - 1] = '\0';
        ColourClass colour(0, 0, 0, 255);
        char *token = strtok(temp_str, ",");
        if (token) colour.R = static_cast<uint8_t>(atoi(token));
        token = strtok(nullptr, ",");
        if (token) colour.G = static_cast<uint8_t>(atoi(token));
        token = strtok(nullptr, ",");
        if (token) colour.B = static_cast<uint8_t>(atoi(token));
        token = strtok(nullptr, ",");
        if (token) colour.A = static_cast<uint8_t>(atoi(token));
        memcpy(out_buffer, &colour, sizeof(ColourClass));
        out_len = sizeof(ColourClass);
        return true;
    }
    case DataType::Vector:
    {
        // Comma-separated numbers into a Number array (up to 8 elements).
        char temp_str[128];
        strncpy(temp_str, val_str, sizeof(temp_str) - 1);
        temp_str[sizeof(temp_str) - 1] = '\0';
        Number vector[8];
        uint8_t count = 0;
        char *token = strtok(temp_str, ",");
        while (token && count < 8)
        {
            vector[count++] = FloatToNumber(static_cast<float>(atof(token)));
            token = strtok(nullptr, ",");
        }
        if (count == 0) return false;
        memcpy(out_buffer, vector, count * sizeof(Number));
        out_len = static_cast<uint8_t>(count * sizeof(Number));
        return true;
    }
    case DataType::Number:
    {
        // Strict decimal float parsing: atof would silently turn "abc" into 0.0 and
        // accept hex ("0x10" -> 16.0). Require a full, non-hex conversion.
        if (!val_str || val_str[0] == '\0')
            return false;
        if (val_str[0] == '0' && (val_str[1] == 'x' || val_str[1] == 'X'))
            return false;
        char *end = nullptr;
        double parsed = strtod(val_str, &end);
        if (end == val_str || *end != '\0')
            return false;
        Number number = FloatToNumber(static_cast<float>(parsed));
        memcpy(out_buffer, &number, sizeof(Number));
        out_len = sizeof(Number);
        return true;
    }
    case DataType::Matrix:
    {
        char temp_str[128];
        strncpy(temp_str, val_str, sizeof(temp_str) - 1);
        temp_str[sizeof(temp_str) - 1] = '\0';
        char *token = strtok(temp_str, ",");
        if (!token)
            return false;

        char mode = token[0];
        Matrix<3, 3> mat;

        if (mode == 'I')
        {
            mat = Matrix<3, 3>::Identity();
        }
        else if (mode == 'R')
        {
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                {
                    token = strtok(nullptr, ",");
                    if (token) mat(row, col) = N(atof(token));
                }
        }
        else if (mode == 'T')
        {
            bool ok = true;
            float args[5];
            for (int a = 0; a < 5 && ok; a++)
            {
                token = strtok(nullptr, ",");
                if (!token)
                    ok = false;
                else
                    args[a] = atof(token);
            }
            if (!ok)
                return false;
            mat = Matrix<3, 3>::CreateTransform2D(N(args[0]), {N(args[1]), N(args[2])}, {N(args[3]), N(args[4])});
        }
        else
        {
            return false;
        }

        memcpy(out_buffer, &mat, sizeof(Matrix<3, 3>));
        out_len = sizeof(Matrix<3, 3>);
        return true;
    }
    default:
        return false;
    }
}

#include "esp_console.h"
#include "esp_log.h"

// Send a Register service read request (CID 1) for the given BlockInfo.
void SendMemoryRead(uint16_t target, ServiceType svc, uint32_t block_info)
{
    PacketFrame req;
    PacketConstruct(&req, target,
                     MakeService(svc, 1), // Register CID 1 = Read
                     MakeService(ServiceType::CLI, 0),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     (uint8_t *)&block_info, sizeof(uint32_t));
    DispatchPacket(req);
}

// Parses a memory-service selector ("r" or 0x01) into a ServiceType.
// Static memory is accessed via Register service (0x01).
  ServiceType ParseService(const char *str)
  {
      if (!str || !str[0])
          return ServiceType::Register; // Default to Register (static memory)

      switch (str[0])
      {
          case 'r': return ServiceType::Register;
          default:
              // strtol with base 0 accepts "0x05" (hex) AND "5" (decimal).
              switch ((int)strtol(str, nullptr, 0))
              {
                  case 0x01: return ServiceType::Register;
                  default:   return ServiceType::Register;
              }
      }
  }

static uint16_t cli_target_addr = 1;

// CLI command "tree": requests a full dump of all memory services from a device.
static int CmdTree(int argc, char **argv)
{
    cli_target_addr = (argc > 1) ? atoi(argv[1]) : 1;
    printf("Requesting full topology dump from Device %d...\n", cli_target_addr);

    // Start with the System block summary (BlockInfo type 0, inst 0, field 0xFF).
    uint32_t summary = CliBlockInfo(0, 0, 0xFF, 0xFF);
    // Space the requests out: back-to-back transmissions collide with the responder's
    // replies on the half-duplex bus (the node answers request 1 while we transmit
    // request 2), losing the responses. The 100 ms gap is enough for the core (10 ms
    // loop) but too short for slow nodes like the DAS (its CSMA + echo-verify reply
    // takes longer), so use a larger gap that also works for them.
    SendMemoryRead(cli_target_addr, ServiceType::Register, summary);
    Sleep(400);
    SendMemoryRead(cli_target_addr, ServiceType::Register, summary);
    Sleep(400);
    SendMemoryRead(cli_target_addr, ServiceType::Register, summary);
    return 0;
}

// CLI command "read": sends a memory read request for the given service/address/block/field/key.
static int CmdRead(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: read <addr> [svc] [block] [field] [key]\n");
        return 1;
    }

    uint16_t addr  = atoi(argv[1]);
    ServiceType svc = (argc > 2) ? ParseService(argv[2]) : ServiceType::Register;
    uint8_t block  = (argc > 3) ? (uint8_t)atoi(argv[3]) : INVALID_BLOCK;
    uint8_t field = (argc > 4) ? atoi(argv[4]) : INVALID_INDEX;
    // Key defaults to 0 (the first member of a keyed/system field); only an explicit
    // key argument requires a concrete field.
    uint8_t key   = (argc > 5) ? atoi(argv[5]) : 0;

    if (argc > 5 && field == INVALID_INDEX)
    {
        printf("Error: Key requires a valid Field index.\n");
        return 1;
    }

    printf("Reading Device %d, Svc %d, Block [%d], Field [%d], Key [%s]...\n",
           addr, (int)svc, block, field, (argc > 5 ? argv[5] : "0"));

    // Register service uses BlockInfo (Type10|Inst6|Field8|Key8), CID 1 for Read.
    // `block` is the block TYPE (e.g. 4 = PWM); inst is always 0 for static blocks.
    uint32_t bi = CliBlockInfo((block == INVALID_BLOCK) ? 0 : block, 0, field, key);
    SendMemoryRead(addr, svc, bi);
    return 0;
}

// CLI command "write": parses a typed value and sends a memory write request to a device.
static int CmdWrite(int argc, char **argv)
{
    // Usage: write <addr> [svc] <block> <field> [key] <type_hex> <value>
    if (argc < 7 || argc > 8) {
        printf("Usage: write <addr> [svc] <block> <field> [key] <type_hex> <value>\n");
        return 1;
    }

    uint16_t addr  = (uint16_t)atoi(argv[1]);
    ServiceType svc = ParseService(argv[2]);
    uint8_t  block = (uint8_t)atoi(argv[3]);
    uint8_t  field = (uint8_t)atoi(argv[4]);

    uint8_t  key = INVALID_INDEX;
    uint16_t type;
    const char *val_str;

    if (argc == 8) {
        key     = (uint8_t)atoi(argv[5]);
        type    = (uint16_t)strtoul(argv[6], nullptr, 16);
        val_str = argv[7];
    } else {
        type    = (uint16_t)strtoul(argv[5], nullptr, 16);
        val_str = argv[6];
    }

    printf("Writing Device %d, Svc %d, Block %d, Field %d, Key %d, Type 0x%03X, Val %s\n",
           addr, (int)svc, block, field, key, type, val_str);

    uint8_t buffer[128];
    uint8_t len = 0;
    if (!ParseCLIValue(type, val_str, buffer, len)) {
        printf("Error: Failed to parse value for type 0x%03X\n", type);
        return 1;
    }

    BlockMeta desc;
    desc.FlagsAndType = type;
    desc.Key = (key == INVALID_INDEX) ? 0x00 : key;
    desc.Size = len;

    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t offset = 0;

    // Register service uses BlockInfo (Type10|Inst6|Field8|Key8) and CID 2 for Write.
    // `block` is the block TYPE (e.g. 4 = PWM); inst is always 0 for static blocks.
    uint32_t bi = CliBlockInfo((block == INVALID_BLOCK) ? 0 : block, 0, field, key);
    memcpy(payload + offset, &bi, 4); offset += 4;  // BlockInfo
    memcpy(payload + offset, &desc, sizeof(BlockMeta)); offset += sizeof(BlockMeta);
    memcpy(payload + offset, buffer, len); offset += len;

    PacketFrame write_pkt;
    PacketConstruct(&write_pkt, addr,
                     MakeService(svc, 2),  // Register CID 2 = Write
                     MakeService(ServiceType::CLI, 0),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, offset);
    DispatchPacket(write_pkt);
    return 0;
}
