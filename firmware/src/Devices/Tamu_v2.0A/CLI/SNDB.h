#pragma once

#include "Core/Functions/Packet.h"
#include "esp_log.h"
#include <string.h>

// Receives SNDB response packets and prints entries to console
void HandleCLI_SNDBResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE)) return;
    g_cli_response_seen = true;

    if (PayloadBytes(frame) == 0) {
        printf("SNDB: No entry found or empty result.\n");
        return;
    }

    // A 1-byte failure status (padded to 4) would otherwise print nothing at all.
    if (PayloadBytes(frame) == 4) {
        printf("SNDB: Operation FAILED (status %d).\n", frame.payload[0]);
        return;
    }

    // Read All streams entries as FRAG packets: info (4) + one or more 16-byte
    // (SN + ID) entries.
    const uint8_t *data = frame.payload;
    uint16_t avail = PayloadBytes(frame);
    if (frame.flags & FLAG_FRAG)
    {
        if (avail <= 4) return;
        data += 4;
        avail -= 4;
    }

    while (avail >= 16)
    {
        const uint8_t *sn_bytes = data;
        uint16_t short_id;
        memcpy(&short_id, data + 14, sizeof(short_id));

        char sn_str[29] = {0};
        for (int i = 0; i < 14; i++)
            snprintf(&sn_str[i * 2], 3, "%02X", sn_bytes[i]);

        printf("SNDB Entry | ID: 0x%04X | SN: %s\n", short_id, sn_str);
        data += 16;
        avail -= 16;
    }
}

// CLI command "sndb": builds a SNDB read_one/read_all/write request and dispatches it to the target node.
static int DispatchSNDBCommand(int argc, char **argv)
{
    // Usage: sndb <target_addr> <read_one|read_all|write> [id_or_sn]
    if (argc < 3) {
        printf("Usage: sndb <target_addr> <read_one|read_all|write> [id]\n");
        return 1;
    }

    uint16_t target_addr = (uint16_t)atoi(argv[1]);
    const char *cmd_str = argv[2];

    PacketFrame req_packet;
    uint8_t payload[20] = {0};
    uint8_t payload_len = 0;
    uint8_t cid = 0;

    if (strcmp(cmd_str, "read_all") == 0) {
        // CID 13: No payload
        cid = 13;
        payload_len = 0;
    }
    else if (strcmp(cmd_str, "read_one") == 0) {
        // CID 11 per docs 00.11: ID (2 bytes) or SN (14 bytes)
        if (argc < 4) { printf("Missing ID\n"); return 1; }
        cid = 11;
        uint16_t lookup_id = (uint16_t)atoi(argv[3]);
        memcpy(payload, &lookup_id, 2);
        payload_len = 2;
    }
    else if (strcmp(cmd_str, "write") == 0) {
        // CID 12 per docs 00.12: SN (14 bytes) + ID (2 bytes)
        if (argc < 5) { printf("Usage: sndb <addr> write <id> <sn_hex>\n"); return 1; }
        cid = 12;
        uint16_t write_id = (uint16_t)atoi(argv[3]);
        const char *sn_hex = argv[4];
        for (int i = 0; i < 14 && sn_hex[i*2] != '\0'; i++) {
            char byte_str[3] = {sn_hex[i*2], sn_hex[i*2+1], '\0'};
            payload[i] = (uint8_t)strtoul(byte_str, nullptr, 16);
        }
        memcpy(payload + 14, &write_id, 2);
        payload_len = 16;
    }
    else {
        printf("Unknown SNDB command: %s\n", cmd_str);
        return 1;
    }

    g_cli_response_seen = false;
    PacketConstruct(&req_packet, target_addr,
                     MakeService(ServiceType::Device, cid),
                     MakeService(ServiceType::CLI, 1),
                     FLAG_REQACK | FLAG_START | FLAG_STOP,
                     payload, payload_len);

    DispatchPacket(req_packet);
    printf("SNDB command '%s' dispatched to node %d\n", cmd_str, target_addr);

    // Warn when the target never answers (dead address / service missing on the node).
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_cli_response_seen)
        printf("Error: no response from device %d (timeout).\n", target_addr);
    return 0;
}