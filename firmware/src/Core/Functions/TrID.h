#pragma once
#include <cstdint>
#include "Core/Functions/Packet.h"

#define TRID_TABLE_SIZE 8
#define TRID_RESERVED_LOW 0xE000

struct TridEntry {
    uint16_t trid = 0;
    uint16_t flags = 0;
    uint32_t validUntil = 0;
    void (*callback)(const PacketFrame&) = nullptr;
    bool used = false;
};

class TridManager {
public:
    TridEntry table[TRID_TABLE_SIZE];
    uint16_t next = 1;

    uint16_t Allocate(uint16_t flags, uint32_t timeoutMs, void (*cb)(const PacketFrame&), uint32_t nowMs) {
        // find free slot
        for (int i=0;i<TRID_TABLE_SIZE;i++) if (!table[i].used) {
            uint16_t id = NextId();
            table[i].trid = id;
            table[i].flags = flags;
            table[i].validUntil = timeoutMs==0 ? 0 : nowMs + timeoutMs;
            table[i].callback = cb;
            table[i].used = true;
            return id;
        }
        return 0;
    }

    bool HandleResponse(const PacketFrame& frame, uint32_t nowMs) {
        for (int i=0;i<TRID_TABLE_SIZE;i++) if (table[i].used && table[i].trid==frame.trid) {
            if (table[i].callback) table[i].callback(frame);
            bool oneshot = (table[i].flags & 1);
            bool isStop = (frame.flags & FLAG_STOP);
            if (oneshot && isStop) table[i].used=false;
            // also expire check
            return true;
        }
        return false;
    }

    void Tick(uint32_t nowMs) {
        for (int i=0;i<TRID_TABLE_SIZE;i++) if (table[i].used && table[i].validUntil!=0 && (int32_t)(nowMs - table[i].validUntil)>=0) {
            table[i].used=false;
        }
    }

private:
    uint16_t NextId() {
        for (int tries=0; tries<0x2000; tries++) {
            uint16_t cand = next++;
            if (cand >= TRID_RESERVED_LOW) { next=1; cand=next++; }
            if (cand==0) cand=next++;
            bool coll=false;
            for (int i=0;i<TRID_TABLE_SIZE;i++) if (table[i].used && table[i].trid==cand) coll=true;
            if (!coll) return cand;
        }
        return 0;
    }
};

inline TridManager GlobalTrid{};
