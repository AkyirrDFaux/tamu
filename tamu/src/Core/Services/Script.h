#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// Script file structure based on documentation
struct ScriptFile
{
    char name[8];
    uint32_t input_offset;
    uint32_t output_offset;
    uint32_t variables_offset;
    uint32_t constants_offset;
    uint32_t instructions_offset;
    uint16_t input_count;
    uint16_t output_count;
    uint16_t variable_count;
    uint16_t constant_count;
    uint16_t instruction_count;
};

// Script service handler - manages script files and basic operations
void HandleScriptService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Script service only processes requests

    PacketFrame reply;

    switch (cid) {
        case 0: { // List Scripts (Response: Script file count + stream of ScriptFile entries)
            uint8_t script_count = 0; // TODO: Implement script counting
            
            if (script_count == 0) {
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
                break;
            }

            // TODO: Stream script file entries
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, (const uint8_t *)&script_count, 1);
            reply.frag_id = NextFragmentId(reply.flags);
            DispatchPacket(reply);
            break;
        }

        case 1: { // Create Script (Request: Name + Input count + Output count)
            if (frame.payload_len >= 10) {
                // TODO: Implement script creation logic
                uint32_t script_id = 0; // Placeholder
                
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, (const uint8_t *)&script_id, 4);
                DispatchPacket(reply);
            }
            break;
        }

        case 2: { // Delete Script (Request: Name)
            if (frame.payload_len >= 6) {
                // TODO: Implement script deletion logic
                
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(reply);
            }
            break;
        }

        case 3: { // Read Script Info (Request: Name, Response: ScriptFile structure)
            if (frame.payload_len >= 6) {
                // TODO: Implement script info reading
                ScriptFile script_info = {};
                
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, (const uint8_t *)&script_info, sizeof(ScriptFile));
                DispatchPacket(reply);
            }
            break;
        }

        case 4: { // Write Script Data (Request: Script ID + Data offset + Data)
            // TODO: Implement script data writing
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
            DispatchPacket(reply);
            break;
        }

        case 5: { // Read Script Data (Request: Script ID + Data offset + Size, Response: Data stream)
            // TODO: Implement script data reading
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
            DispatchPacket(reply);
            break;
        }

        default:
            // Unknown CID
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
            DispatchPacket(reply);
            break;
    }
}

