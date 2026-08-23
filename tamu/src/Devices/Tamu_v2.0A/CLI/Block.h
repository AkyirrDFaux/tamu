#pragma once

#include "Handler.h"

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
    if (flags & FieldFlags::RemoteOrigin) strcat(flag_str, "[RM] ");

    if (is_key)
        printf("    |-- Key [%02d]: ", index);
    else
        printf("  |-- Field [%02d] %-15s ", index, flag_str);

    if (data_ptr != nullptr)
        PrintValue(BlockMetaType(desc.FlagsAndType), data_ptr, desc.Size);
    else
        printf("(No Data)\n");
}

#include <string>
#include <sstream>

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
        Number number = FloatToNumber(static_cast<float>(atof(val_str)));
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
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"

// Send a memory read request using the new SRV-based routing
void SendMemoryRead(uint16_t target, ServiceType svc, BlockIndex idx)
{
    PacketFrame req;
    // CID 2 (Read) on the selected memory service
    PacketConstruct(&req, target,
                     MakeService(svc, 2),
                     MakeService(ServiceType::CLI, 0),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     (uint8_t *)&idx, sizeof(BlockIndex));
    DispatchPacket(req);
}

// Parses a memory-service selector ("s"/"d"/"k" or 0x04/0x05/0x06) into a ServiceType.
// Returns ServiceType::SystemMemory as the default when `str` is null/empty.
ServiceType ParseService(const char *str)
{
    if (!str || !str[0])
        return ServiceType::SystemMemory;

    switch (str[0])
    {
        case 's': return ServiceType::SystemMemory;
        case 'd': return ServiceType::DynamicMemory;
        case 'k': return ServiceType::KeyedMemory;
        default:
            switch (atoi(str))
            {
                case 0x05: return ServiceType::DynamicMemory;
                case 0x06: return ServiceType::KeyedMemory;
                default:   return ServiceType::SystemMemory;
            }
    }
}

static uint16_t cli_target_addr = 1;

// CLI command "tree": requests a full dump of all three memory services from a device.
static int CmdTree(int argc, char **argv)
{
    cli_target_addr = (argc > 1) ? atoi(argv[1]) : 1;
    printf("Requesting full topology dump from Device %d...\n", cli_target_addr);

    BlockIndex summary = {.Block = INVALID_BLOCK, .Field = INVALID_INDEX, .Key = INVALID_INDEX};
    SendMemoryRead(cli_target_addr, ServiceType::SystemMemory, summary);
    SendMemoryRead(cli_target_addr, ServiceType::DynamicMemory, summary);
    SendMemoryRead(cli_target_addr, ServiceType::KeyedMemory, summary);
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
    ServiceType svc = (argc > 2) ? ParseService(argv[2]) : ServiceType::SystemMemory;
    uint8_t block  = (argc > 3) ? (uint8_t)atoi(argv[3]) : INVALID_BLOCK;
    uint8_t field = (argc > 4) ? atoi(argv[4]) : INVALID_INDEX;
    uint8_t key   = (argc > 5) ? atoi(argv[5]) : INVALID_INDEX;

    if (key != INVALID_INDEX && field == INVALID_INDEX)
    {
        printf("Error: Key requires a valid Field index.\n");
        return 1;
    }

    printf("Reading Device %d, Svc %d, Block [%d], Field [%d], Key [%s]...\n",
           addr, (int)svc, block, field, (key == INVALID_INDEX ? "N/A" : argv[5]));

    BlockIndex req = {.Block = block, .Field = field, .Key = key};
    SendMemoryRead(addr, svc, req);
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

    BlockIndex idx = { .Block = block, .Field = field, .Key = key };
    memcpy(payload + offset, &idx, sizeof(BlockIndex));  offset += sizeof(BlockIndex);
    memcpy(payload + offset, &desc, sizeof(BlockMeta)); offset += sizeof(BlockMeta);
    memcpy(payload + offset, buffer, len); offset += len;

    PacketFrame write_pkt;
    PacketConstruct(&write_pkt, addr,
                     MakeService(svc, 3),
                     MakeService(ServiceType::CLI, 0),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, offset);

    DispatchPacket(write_pkt);
    return 0;
}
