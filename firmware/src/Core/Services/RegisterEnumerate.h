#pragma once

// CID 0: enumerate blocks, fields and keys.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 0: Enumerate =====

static void HandleEnumerate(const PacketFrame &frame, uint32_t bi) {
    if (PayloadBytes(frame) < 5) { RespondStatus(frame,false); return; }
    uint8_t enum_level = frame.payload[0];
    uint32_t bi_req = 0; memcpy(&bi_req, frame.payload+1, 4);
    
    if (enum_level == 0) { // Enumerate block types
        if (bi_req != 0) { RespondStatus(frame,false); return; }
        uint8_t types[16]; uint8_t n=0;
        for (size_t i=0;i<static_block_num && n<16;i++) {
            uint16_t t = (uint16_t)static_block_registry[i].Schema->Type;
            bool seen=false; for(uint8_t j=0;j<n;j++) if(types[j]==(t&0xFF)) seen=true;
            if(!seen) types[n++]=(uint8_t)t;
        }
        uint8_t rpl[20]; memcpy(rpl, &bi, 4); memcpy(rpl+4, types, n);
        SendResponse(frame, rpl, 4+n);
    } else if (enum_level == 1) { // Enumerate instances of type
        if (bi_req == 0) { RespondStatus(frame,false); return; }
        uint16_t req_type = BlockInfoType(bi_req);
        if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            // Scripts: list the loaded slots (stable instance = script slot). The reply is
            // a count (u8) followed by the slot ids so sparse loads stay addressable.
            uint8_t ids[MAX_SCRIPTS];
            uint8_t n = ScriptListInstances(ids, MAX_SCRIPTS);
            uint8_t rpl[8 + MAX_SCRIPTS];
            memcpy(rpl, &bi, 4);
            rpl[4] = n;
            memcpy(rpl + 5, ids, n);
            SendResponse(frame, rpl, 5 + n);
#else
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = 0;
            SendResponse(frame, rpl, 5);
#endif
        } else if (req_type == 0x3FF) { // Dynamic blocks
#ifndef DISABLE_DYNAMIC_MEMORY
            uint8_t cnt = dynamic_block_registry.block_count;
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = cnt;
            SendResponse(frame, rpl, 5);
#else
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = 0;
            SendResponse(frame, rpl, 5);
#endif
        } else {
            uint8_t cnt=0; for(size_t i=0;i<static_block_num;i++) if((uint16_t)static_block_registry[i].Schema->Type==req_type) cnt++;
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4]=cnt;
            SendResponse(frame, rpl, 5);
        }
    } else if (enum_level == 2) { // Enumerate fields
        if (bi_req == 0) { RespondStatus(frame,false); return; }
        uint16_t req_type = BlockInfoType(bi_req);
        uint8_t req_inst = BlockInfoInstance(bi_req);
        uint16_t cnt;
        if (req_type == 0 && req_inst == 0) {
            // System block is not in the static registry; its field count is board-aware
            // (nodes like the DAS omit the Core-only NetID and App/CLI fields).
            cnt = SYSTEM_FIELD_COUNT;
        } else if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            cnt = ScriptActive(req_inst) ? SCRIPT_FIELD_COUNT : 0;
#else
            cnt = 0;
#endif
        } else if (req_type == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            if (req_inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
            DynamicBlockDescriptor *dyn = dynamic_block_registry.GetBlock(req_inst);
            // Return the distinct field indexes after the count (dynamic blocks).
            uint8_t fields[256];
            cnt = dyn->ListFields(fields, 256);
            uint8_t rpl[8 + 256]; memcpy(rpl, &bi_req, 4); rpl[4]=(uint8_t)cnt; rpl[5]=(uint8_t)(cnt>>8);
            uint16_t p = 6;
            for (uint16_t i = 0; i < cnt; i++) rpl[p++] = fields[i];
            SendResponse(frame, rpl, p);
            return;
#else
            cnt = 0;
#endif
        } else {
            int idx = FindStaticBlock(req_type, req_inst);
            if (idx < 0) { RespondStatus(frame,false); return; }
            cnt = static_block_registry[idx].Schema->MapCount;
        }
        uint8_t rpl[8]; memcpy(rpl, &bi_req, 4); rpl[4]=(uint8_t)cnt; rpl[5]=(uint8_t)(cnt>>8);
        SendResponse(frame, rpl, 6);
    } else if (enum_level == 3) { // Enumerate keys in a field (dynamic blocks)
        uint8_t rpl[8 + 256];
        memcpy(rpl, &bi_req, 4);
        uint16_t p = 4;
        uint16_t req_type = BlockInfoType(bi_req);
        if (req_type == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            uint8_t req_inst = BlockInfoInstance(bi_req);
            uint8_t req_field = BlockInfoField(bi_req);
            if (req_inst < dynamic_block_registry.block_count) {
                DynamicBlockDescriptor *dyn = dynamic_block_registry.GetBlock(req_inst);
                uint8_t keys[256];
                uint16_t key_count = dyn->ListKeys(req_field, keys, 256);
                rpl[p++] = (uint8_t)key_count;
                for (uint16_t i = 0; i < key_count; i++) rpl[p++] = keys[i];
            } else {
                rpl[p++] = 0;
            }
#else
            rpl[p++] = 0;
#endif
        } else if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            uint8_t n = ScriptKeyCount(BlockInfoInstance(bi_req), BlockInfoField(bi_req));
            rpl[p++] = n;
            for (uint8_t i = 0; i < n; i++) rpl[p++] = i;
#else
            rpl[p++] = 0;
#endif
        } else {
            rpl[p++] = 0;
        }
        SendResponse(frame, rpl, p);
    } else {
        RespondStatus(frame,false);
    }
}

