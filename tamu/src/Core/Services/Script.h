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

// Script service handler - manages script files and basic operations.
// NOTE: The script operations are not implemented yet; every request is answered
// with a failure status (0xFF) instead of a false success so clients can tell
// "not implemented" apart from "done".
void HandleScriptService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Script service only processes requests

    PacketFrame reply;

    switch (cid) {
        case 0: { // List Scripts - implemented: there are no script files yet
            uint8_t script_count = 0;
            PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, &script_count, 1);
            DispatchPacket(reply);
            break;
        }

        default:
            // TODO: Implement Create/Delete/Read/Write script operations
            {
                uint8_t status = 0xFF; // not implemented
                PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, &status, 1);
                DispatchPacket(reply);
            }
            break;
    }
}

