#pragma once

#include "Block.h"
#include "SNDB.h"
#include "Core/Services/LogHandler.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/Script.h"
#include "esp_console.h"
#include <cstring>
#include <cstdlib>

// Prints the log records kept in RAM on the core device, oldest first (by sequence).
static int CmdLogs(int argc, char **argv)
{
#ifdef TYPE_CORE
    EnsureLogStorage();
    if (!LogBuffer || !LogUsed || !LogSeq) { printf("No log storage available.\n"); return 0; }

    // Print in chronological order: repeatedly pick the smallest sequence number
    // above the last printed one.
    uint32_t printed = 0, last_seq = 0;
    bool any = false;
    for (;;)
    {
        int best = -1;
        uint32_t best_seq = 0xFFFFFFFF;
        for (uint32_t i = 0; i < LogCount; i++)
        {
            if (LogUsed[i] && LogSeq[i] > last_seq && LogSeq[i] < best_seq)
            {
                best_seq = LogSeq[i];
                best = (int)i;
            }
        }
        if (best < 0) break;

        const LogMessage &m = LogBuffer[best].msg;
        printf("[%03u] Dev %d | %s | Src 0x%04X | Code 0x%04X | Count %lu | t=%lums\n",
               (unsigned)printed++, LogBuffer[best].device_id, LogIsBlock(m) ? "Block" : "Svc",
               LogSourceId(m), LogCode(m), (unsigned long)LogBuffer[best].count,
               (unsigned long)m.timestamp);
        last_seq = best_seq;
        any = true;
    }
    if (!any)
        printf("No logs recorded.\n");
#else
    printf("Log records are only available on the core device.\n");
#endif
    return 0;
}

// logget [addr] : fetch a device's log database over the bus via the GetLogs CID.
static int CmdLogGet(int argc, char **argv)
{
    uint16_t addr = (argc > 1) ? (uint16_t)atoi(argv[1]) : 1;
    printf("Requesting logs from device %d...\n", addr);

    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(ServiceType::LogHandler, 1),
                     MakeService(ServiceType::CLI, 6),
                     FLAG_REQACK | FLAG_START | FLAG_STOP, nullptr, 0);
    g_cli_response_seen = false;
    DispatchPacket(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from device %d (timeout).\n", addr);
    return 0;
}

// logclear [addr] [count] : clear the N most recent logs on a device via ClearReadLogs.
static int CmdLogClear(int argc, char **argv)
{
    uint16_t addr = (argc > 1) ? (uint16_t)atoi(argv[1]) : 1;
    uint32_t count = (argc > 2) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 32;
    printf("Clearing %lu logs on device %d...\n", (unsigned long)count, addr);

    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(ServiceType::LogHandler, 2),
                     MakeService(ServiceType::CLI, 6),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     (const uint8_t *)&count, sizeof(count));
    g_cli_response_seen = false;
    DispatchPacket(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from device %d (timeout).\n", addr);
    return 0;
}

// log <is_block> <source_id_hex> <code_hex> : Send a log/error report to the Log Handler service
static int CmdLog(int argc, char **argv)
{
    if (argc < 4) { printf("Usage: log <is_block> <source_id_hex> <code_hex>\n"); return 1; }

    bool is_block = (atoi(argv[1]) != 0);
    uint16_t source_id = (uint16_t)strtoul(argv[2], nullptr, 16);
    uint16_t code = (uint16_t)strtoul(argv[3], nullptr, 16);

    printf("Sending log is_block=%d source_id=0x%04X code=0x%04X\n",
           (int)is_block, source_id, code);
    ReportLog(MakeLog(is_block, source_id, code, 0));
    return 0;
}

// Sends a memory-service extra command (Create/Delete/Save/Recall/Read backup).
// cli_srv_cid: 0 = response prints block fields, 2 = response prints a status byte.
// Waits briefly for the asynchronous reply so a dead address or a service that is not
// compiled into the target produces a timeout error instead of silence.
static int SendMemoryExtra(uint16_t addr, uint8_t block, ServiceType service, uint8_t cid, uint8_t cli_srv_cid)
{
    BlockIndex idx = {block, INVALID_INDEX, INVALID_INDEX};
    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(service, cid),
                     MakeService(ServiceType::CLI, cli_srv_cid),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     (const uint8_t *)&idx, sizeof(BlockIndex));
    g_cli_response_seen = false;
    DispatchPacket(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from device %d (timeout) - service missing on the node?\n", addr);
    return 0;
}

// save <addr> [svc] [block|-] : Save a block (or everything) of a service to its backup file
static int CmdSave(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: save <addr> [svc] [block|-]\n"); return 1; }
    uint16_t addr = atoi(argv[1]);
    ServiceType svc = (argc > 2) ? ParseService(argv[2]) : ServiceType::DynamicMemory;
    uint8_t block = (argc > 3 && strcmp(argv[3], "-") != 0) ? (uint8_t)atoi(argv[3]) : INVALID_BLOCK;
    printf("Saving block %d of service %d on device %d...\n", block, (int)svc, addr);
    return SendMemoryExtra(addr, block, svc, 5, 2);
}

// recall <addr> [svc] [block|-] : Recall a block (or everything) of a service from its backup file
static int CmdRecall(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: recall <addr> [svc] [block|-]\n"); return 1; }
    uint16_t addr = atoi(argv[1]);
    ServiceType svc = (argc > 2) ? ParseService(argv[2]) : ServiceType::DynamicMemory;
    uint8_t block = (argc > 3 && strcmp(argv[3], "-") != 0) ? (uint8_t)atoi(argv[3]) : INVALID_BLOCK;
    printf("Recalling block %d of service %d on device %d...\n", block, (int)svc, addr);
    return SendMemoryExtra(addr, block, svc, 6, 2);
}

// rmem <addr> [svc] <block> : Read a block from its service's backup file (prints via tree handler)
static int CmdReadMemory(int argc, char **argv)
{
    if (argc < 3) { printf("Usage: rmem <addr> [svc] <block>\n"); return 1; }
    uint16_t addr = atoi(argv[1]);
    ServiceType svc = (argc > 3) ? ParseService(argv[2]) : ServiceType::DynamicMemory;
    uint8_t block = (uint8_t)atoi(argv[(argc > 3) ? 3 : 2]);
    printf("Reading backup of block %d of service %d on device %d...\n", block, (int)svc, addr);
    return SendMemoryExtra(addr, block, svc, 4, 0);
}

// create <addr> <svc> <type_hex> <name> : Create a new dynamic/keyed block
static int CmdCreate(int argc, char **argv)
{
    if (argc < 5) { printf("Usage: create <addr> <svc> <type_hex> <name>\n"); return 1; }
    uint16_t addr = atoi(argv[1]);
    ServiceType svc = ParseService(argv[2]);
    if (svc == ServiceType::SystemMemory)
    {
        printf("Error: System Memory blocks are compiled in and cannot be created.\n");
        return 1;
    }
    uint16_t type = (uint16_t)strtoul(argv[3], nullptr, 16);
    const char *name = argv[4];
    uint16_t name_len = (uint16_t)strlen(name);
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;

    BlockIndex idx = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
    BlockMeta desc;
    desc.FlagsAndType = type;
    desc.Key = INVALID_INDEX;
    desc.Size = (uint8_t)name_len;

    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t plen = 0;
    memcpy(payload + plen, &idx, sizeof(BlockIndex)); plen += sizeof(BlockIndex);
    memcpy(payload + plen, &desc, sizeof(BlockMeta)); plen += sizeof(BlockMeta);
    memcpy(payload + plen, name, name_len); plen += name_len;

    printf("Creating %s block (Type 0x%03X) named \"%s\" on device %d...\n",
           (svc == ServiceType::KeyedMemory) ? "keyed" : "dynamic", type, name, addr);

    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(svc, 0),
                     MakeService(ServiceType::CLI, 5),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, plen);
    DispatchPacket(req);
    return 0;
}

// delete <addr> <svc> <block> : Delete a dynamic/keyed block (deallocated on next save)
static int CmdDelete(int argc, char **argv)
{
    if (argc < 4) { printf("Usage: delete <addr> <svc> <block>\n"); return 1; }
    uint16_t addr = atoi(argv[1]);
    ServiceType svc = ParseService(argv[2]);
    uint8_t block = (uint8_t)atoi(argv[3]);
    printf("Deleting block %d of service %d on device %d...\n", block, (int)svc, addr);
    return SendMemoryExtra(addr, block, svc, 1, 2);
}

// Sends a Device service request to `addr` and routes the reply to the CLI (cid 3).
// Device service CIDs: 0 Discover, 1 Ping, 2 Type, 3 SN, 4 Version, 5 Capability, 6 Read Name, 7 Set Name, 8 Uptime, 9 Loop, 10 Time sync.
static int CmdDevice(int argc, char **argv)
{
    // Usage: dev <addr> <discover|ping|type|sn|version|cap|name [newname]|uptime|loop|time>
    if (argc < 3) { printf("Usage: dev <addr> <discover|ping|type|sn|version|cap|name [newname]|uptime|loop|time>\n"); return 1; }
    uint16_t addr = (uint16_t)atoi(argv[1]);
    const char *cmd = argv[2];

    uint8_t cid = 0;
    uint8_t payload[32] = {0};
    uint8_t plen = 0;

    if (strcmp(cmd, "discover") == 0)
    {
        cid = 0;
        memcpy(payload, &GetSerialNumber(), sizeof(SerialNumber));
        plen = sizeof(SerialNumber);
    }
    else if (strcmp(cmd, "ping") == 0) { cid = 1; }
    else if (strcmp(cmd, "type") == 0) { cid = 2; }
    else if (strcmp(cmd, "sn") == 0) { cid = 3; }
    else if (strcmp(cmd, "version") == 0) { cid = 4; }
    else if (strcmp(cmd, "cap") == 0) { cid = 5; }
    else if (strcmp(cmd, "name") == 0)
    {
        if (argc > 3)
        {
            cid = 7; // Set Name
            plen = (uint8_t)strlen(argv[3]);
            if (plen > 23) plen = 23;
            memcpy(payload, argv[3], plen);
        }
        else
        {
            cid = 6; // Read Name
        }
    }
    else if (strcmp(cmd, "uptime") == 0) { cid = 8; }
    else if (strcmp(cmd, "loop") == 0) { cid = 9; }
    else if (strcmp(cmd, "time") == 0)
    {
        // Time sync request: payload carries the local send timestamp (t0). Without it the
        // node would reply with t0=0 and the offset estimate would be off by half the uptime.
        cid = 10;
        uint32_t t0 = DeviceStatus.UptimeMs;
        memcpy(payload, &t0, 4);
        plen = 4;
    }
    else { printf("Unknown dev command: %s\n", cmd); return 1; }

    printf("Device service '%s' dispatched to node %d...\n", cmd, addr);

    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(ServiceType::Device, cid),
                     MakeService(ServiceType::CLI, 3),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, plen);
    g_cli_response_seen = false;
    DispatchPacket(req);

    // Wait briefly for the asynchronous reply so a dead address produces an error
    // message instead of silence.
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from device %d (timeout).\n", addr);
    return 0;
}

// Sends a Storage service request to `addr` and routes the reply to the CLI (cid 4).
// Storage service CIDs (Docs/Services/Storage.md): 0 File Table, 1 Format, 2 Create,
// 3 Delete, 4 Resize, 5 Rename, 6 Read File, 7 Write Stream Open, 8 Write Stream Close.
static int CmdFile(int argc, char **argv)
{
    // Usage: file <addr> <table|read|create|delete|resize|rename> [args]
    if (argc < 3) { printf("Usage: file <addr> <table|read|create|delete|resize|rename> [args]\n"); return 1; }
    uint16_t addr = (uint16_t)atoi(argv[1]);
    const char *cmd = argv[2];

    uint8_t cid = 0;
    uint8_t payload[32] = {0};
    uint8_t plen = 0;
    char name8[8] = {0};

    if (strcmp(cmd, "table") == 0) { cid = 0; }
    else if (strcmp(cmd, "read") == 0)
    {
        // file <addr> read <name> <offset> <num>
        if (argc < 6) { printf("Usage: file <addr> read <name> <offset> <num>\n"); return 1; }
        cid = 6; // Read File
        PackName(argv[3], name8);
        uint32_t off = (uint32_t)strtoul(argv[4], nullptr, 10);
        uint32_t num = (uint32_t)strtoul(argv[5], nullptr, 10);
        memcpy(payload, name8, 8);
        memcpy(payload + 8, &off, 4);
        memcpy(payload + 12, &num, 4);
        plen = 8 + 8;
    }
    else if (strcmp(cmd, "create") == 0)
    {
        // file <addr> create <name> <size>
        if (argc < 5) { printf("Usage: file <addr> create <name> <size>\n"); return 1; }
        cid = 2;
        PackName(argv[3], name8);
        uint32_t size = (uint32_t)strtoul(argv[4], nullptr, 10);
        memcpy(payload, name8, 8);
        memcpy(payload + 8, &size, 4);
        plen = 8 + 4;
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        // file <addr> delete <name>
        if (argc < 4) { printf("Usage: file <addr> delete <name>\n"); return 1; }
        cid = 3;
        PackName(argv[3], name8);
        memcpy(payload, name8, 8);
        plen = 8;
    }
    else if (strcmp(cmd, "resize") == 0)
    {
        // file <addr> resize <name> <newsize>
        if (argc < 5) { printf("Usage: file <addr> resize <name> <newsize>\n"); return 1; }
        cid = 4;
        PackName(argv[3], name8);
        uint32_t size = (uint32_t)strtoul(argv[4], nullptr, 10);
        memcpy(payload, name8, 8);
        memcpy(payload + 8, &size, 4);
        plen = 8 + 4;
    }
    else if (strcmp(cmd, "rename") == 0)
    {
        // file <addr> rename <oldname> <newname>
        if (argc < 5) { printf("Usage: file <addr> rename <oldname> <newname>\n"); return 1; }
        cid = 5;
        char new8[8] = {0};
        PackName(argv[3], name8);
        PackName(argv[4], new8);
        memcpy(payload, name8, 8);
        memcpy(payload + 8, new8, 8);
        plen = 16;
    }
    else { printf("Unknown file command: %s\n", cmd); return 1; }

    printf("Storage service '%s' dispatched to node %d...\n", cmd, addr);

    PacketFrame req;
    PacketConstruct(&req, addr,
                     MakeService(ServiceType::Storage, cid),
                     MakeService(ServiceType::CLI, 4),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, plen);
    DispatchPacket(req);
    return 0;
}

// Sends a Script service request to the core (the script service is local) and routes the
// reply to the CLI (cid 7). Script manager CIDs per Docs/Services/Script.md.
static int SendScriptRequest(uint8_t cid, const uint8_t *payload, uint8_t plen)
{
    PacketFrame req;
    PacketConstruct(&req, 1,
                     MakeService(ServiceType::Script, cid),
                     MakeService(ServiceType::CLI, 7),
                     FLAG_REQACK | FLAG_START | FLAG_STOP, payload, plen);
    g_cli_response_seen = false;
    DispatchPacket(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from the script service (timeout).\n");
    return 0;
}

static const char *ScriptOpName(uint8_t op)
{
    switch (op)
    {
        case OP_ADD: return "ADD"; case OP_SUB: return "SUB";
        case OP_MUL: return "MUL"; case OP_DIV: return "DIV";
        case OP_NEG: return "NEG";
        case OP_AND: return "AND"; case OP_OR: return "OR"; case OP_NOT: return "NOT";
        case OP_CMP_EQ: return "EQ"; case OP_CMP_NE: return "NE";
        case OP_CMP_LT: return "LT"; case OP_CMP_LE: return "LE";
        case OP_CMP_GT: return "GT"; case OP_CMP_GE: return "GE";
        case OP_COMPOSE_VEC: return "COMPOSE_VEC"; case OP_COMPOSE_COLOUR: return "COMPOSE_COLOR";
        case OP_EXTRACT: return "EXTRACT";
        case OP_MEM_READ: return "MEM_READ"; case OP_MEM_WRITE: return "MEM_WRITE";
        case OP_IF: return "IF"; case OP_WHILE: return "WHILE";
        case OP_END_IF: return "END_IF"; case OP_END_WHILE: return "END_WHILE"; case OP_END: return "END";
        case OP_DELAY: return "DELAY"; case OP_GET_TIME: return "GET_TIME";
        case OP_PAUSE: return "PAUSE"; case OP_RESUME: return "RESUME";
        case OP_TERMINATE: return "TERMINATE"; case OP_RESTART: return "RESTART";
        case OP_INFO_REPORT: return "INFO_REPORT"; case OP_ERROR_HALT: return "ERROR_HALT";
        case OP_MACRO_CALL: return "MACRO_CALL";
        default: return "?";
    }
}

static void ScriptPrintSymbol(const ScriptSymbol &s)
{
    switch (s.type)
    {
        case SYM_INSTRUCTION:
            printf(" %s", ScriptOpName(s.subtype));
            if (s.value) printf("(%u)", s.value);
            break;
        case SYM_INPUT: printf(" In%u", s.value); break;
        case SYM_OUTPUT: printf(" Out%u", s.value); break;
        case SYM_VARIABLE: printf(" Var%u", s.value); break;
        case SYM_CONSTANT: printf(" Const%u", s.value); break;
        case SYM_PREDEFINE: printf(" Pre{%u,%u}", s.subtype, s.value); break;
        default: break;
    }
}

// script <list|read <id>|create <id|auto>|delete <id>|start <id>|stop <id>|state <id>>
static int CmdScript(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: script <list|read <id>|create <id|auto>|delete <id>|start <id>|stop <id>|state <id>>\n");
        return 1;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0)
    {
        uint8_t n = ScriptFileCount();
        printf("%u script(s):\n", n);
        for (uint16_t id = 1; id <= 255; id++)
        {
            char fname[8];
            ScriptFileIdToName(id, fname);
            if (Storage.FileExists(fname) == 0xFFFFFFFF) continue;
            uint32_t off, sz;
            if (!Storage.GetFileInfo(fname, &off, &sz) || sz < ScriptHeaderSize()) continue;
            char name[17] = {0};
            Storage_FlashRead(off, name, 16);
            printf("  Script %u: \"%s\" (%lu B)\n", id, name, (unsigned long)sz);
        }
        return 0;
    }

    if (strcmp(cmd, "read") == 0)
    {
        if (argc < 3) { printf("Usage: script read <id>\n"); return 1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        ScriptProgram p;
        if (!LoadScriptProgram(id, p))
        {
            printf("Script %u: load failed.\n", id);
            return 1;
        }
        if (!ValidateScriptProgram(p))
        {
            printf("Script %u: invalid program (unbalanced flow / no End / bad operand).\n", id);
            p.Release();
            return 1;
        }
        printf("Script %u: name \"%.*s\" in=%u out=%u var=%u const=%u lines=%u\n",
               id, 16, p.header.name, p.header.input_count, p.header.output_count,
               p.header.variable_count, p.header.constant_count, p.line_count);
        uint32_t offset = 0;
        for (uint32_t line = 0; line < p.line_count; line++)
        {
            printf("  %3lu:", (unsigned long)line);
            for (;;)
            {
                ScriptSymbol s;
                if (!ScriptSymbolRead(p.instructions, p.header.instruction_len, offset, &s)) break;
                offset += 4;
                if (s.type == SYM_ENDLINE) break;
                ScriptPrintSymbol(s);
            }
            printf("\n");
        }
        p.Release();
        return 0;
    }

    if (argc < 3) { printf("Usage: script <%s> <id>\n", cmd); return 1; }

    if (strcmp(cmd, "create") == 0)
    {
        uint8_t payload[1] = {(uint8_t)((strcmp(argv[2], "auto") == 0) ? 0xFF : (uint8_t)atoi(argv[2]))};
        return SendScriptRequest(13, payload, 1);
    }
    if (strcmp(cmd, "delete") == 0)
    {
        uint8_t payload[1] = {(uint8_t)atoi(argv[2])};
        return SendScriptRequest(14, payload, 1);
    }
    if (strcmp(cmd, "start") == 0)
    {
        uint8_t payload[2] = {(uint8_t)atoi(argv[2]), SCRIPT_RUNNING};
        return SendScriptRequest(4, payload, 2);
    }
    if (strcmp(cmd, "stop") == 0)
    {
        uint8_t payload[2] = {(uint8_t)atoi(argv[2]), SCRIPT_STOPPED};
        return SendScriptRequest(4, payload, 2);
    }
    if (strcmp(cmd, "state") == 0)
    {
        uint8_t payload[1] = {(uint8_t)atoi(argv[2])};
        return SendScriptRequest(3, payload, 1);
    }

    printf("Unknown script command: %s\n", cmd);
    return 1;
}

// Sets up the CLI command registry and starts the USB console/app mode task
// (see AppUSB.h). Commands: tree, read, write, sndb, logs, logget, logclear, save,
// recall, rmem, dev, create, delete, log, file.
void StartCLI(void)
{
    // Initialize the console module. The old REPL setup used to do this
    // internally (esp_console_new_repl_usb_serial_jtag); now that the console
    // runs on our own task, initialization must be explicit - without it every
    // esp_console_run() takes the "not found" exit and never writes cmd_ret.
    esp_console_config_t console_config = {
        .max_cmdline_length = 256,
        .max_cmdline_args = 16,
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    // Register commands
    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t tree_cmd = {
        .command = "tree",
        .help    = "List all devices, their blocks, and fields",
        .hint    = "[addr]",
        .func    = &CmdTree,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&tree_cmd));

    const esp_console_cmd_t read_cmd = {
        .command = "read",
        .help    = "Read a specific block or field from a device",
        .hint    = "<addr> [svc] [block] [field] [key]",
        .func    = &CmdRead,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&read_cmd));

    const esp_console_cmd_t write_cmd = {
        .command = "write",
        .help    = "Write a value to a block field",
        .hint    = "<addr> [svc] <block> <field> [key] <type_hex> <value>",
        .func    = &CmdWrite,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&write_cmd));

    const esp_console_cmd_t sndb_cmd = {
        .command = "sndb",
        .help    = "SNDB management: read_one, read_all, write",
        .hint    = "<target_addr> <cmd> [id]",
        .func    = &DispatchSNDBCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sndb_cmd));

    const esp_console_cmd_t logs_cmd = {
        .command = "logs",
        .help    = "Show logs collected from the network (local database)",
        .hint    = "",
        .func    = &CmdLogs,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&logs_cmd));

    const esp_console_cmd_t logget_cmd = {
        .command = "logget",
        .help    = "Fetch a device's log database over the bus (GetLogs)",
        .hint    = "[addr]",
        .func    = &CmdLogGet,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&logget_cmd));

    const esp_console_cmd_t logclear_cmd = {
        .command = "logclear",
        .help    = "Clear the N most recent logs on a device (ClearReadLogs)",
        .hint    = "[addr] [count]",
        .func    = &CmdLogClear,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&logclear_cmd));

    const esp_console_cmd_t save_cmd = {
        .command = "save",
        .help    = "Save a memory block to its backup file",
        .hint    = "<addr> [svc] [block|-]",
        .func    = &CmdSave,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&save_cmd));

    const esp_console_cmd_t recall_cmd = {
        .command = "recall",
        .help    = "Recall a memory block from its backup file",
        .hint    = "<addr> [svc] [block|-]",
        .func    = &CmdRecall,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&recall_cmd));

    const esp_console_cmd_t rmem_cmd = {
        .command = "rmem",
        .help    = "Read a memory block directly from its backup file",
        .hint    = "<addr> [svc] <block>",
        .func    = &CmdReadMemory,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&rmem_cmd));

    const esp_console_cmd_t dev_cmd = {
        .command = "dev",
        .help    = "Query the Device service of a node",
        .hint    = "<addr> <discover|ping|type|sn|version|cap|name [newname]|uptime|loop|time>",
        .func    = &CmdDevice,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&dev_cmd));

    const esp_console_cmd_t create_cmd = {
        .command = "create",
        .help    = "Create a new dynamic/keyed memory block",
        .hint    = "<addr> <svc> <type_hex> <name>",
        .func    = &CmdCreate,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&create_cmd));

    const esp_console_cmd_t delete_cmd = {
        .command = "delete",
        .help    = "Delete a dynamic/keyed memory block (deallocated on next save)",
        .hint    = "<addr> <svc> <block>",
        .func    = &CmdDelete,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&delete_cmd));

    const esp_console_cmd_t log_cmd = {
        .command = "log",
        .help    = "Send a log/error report to the Log Handler service",
        .hint    = "<is_block> <source_id_hex> <code_hex>",
        .func    = &CmdLog,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&log_cmd));

    const esp_console_cmd_t file_cmd = {
        .command = "file",
        .help    = "Access the Storage service of a node",
        .hint    = "<addr> <table|read|create|delete|resize> [args]",
        .func    = &CmdFile,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&file_cmd));

    const esp_console_cmd_t script_cmd = {
        .command = "script",
        .help    = "Script service: list, read, create, delete, start, stop, state",
        .hint    = "<list|read <id>|create <id|auto>|delete <id>|start <id>|stop <id>|state <id>>",
        .func    = &CmdScript,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&script_cmd));

    // 5. Bring up the USB port (driver + VFS stdio) and run the console/app mode
    // machine on its own task. Same stack sizing rationale as the old REPL config:
    // nested local dispatch chains need the headroom.
    AppUSBInit();
    xTaskCreate(ConsoleTask, "console", 16384, NULL, 5, NULL);

    ESP_LOGI("CLI", "Console initialized over USB Serial/JTAG.");
}