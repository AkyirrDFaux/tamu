#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/SNDB.h"

#ifdef TYPE_CORE

#define TIMESYNC_SAMPLES      3
#define TIMESYNC_GAP_MS       3000
#define TIMESYNC_INTERVAL_MS  (5 * 60 * 1000)
#define TIMESYNC_MAX_DEVICES  64

// Core provides periodic time sync (Docs/Services/Device service.md):
// about once per few minutes, at least 3 samples with a short delay are
// taken and averaged before the offset is pushed to the node (CID 12).
class TimeSyncService
{
public:
    enum State { Idle, Waiting };

    // Called on the main loop; starts a sync round when due and sends/retries samples while waiting.
    // Scheduling uses signed differences of the uint32 millisecond counters so the
    // comparisons stay correct across UptimeMs wraparound.
    void Tick(uint32_t now_ms)
    {
        if (state == Idle)
        {
            if ((int32_t)(now_ms - round_due_ms) >= 0)
                BeginRound();
        }
        else if ((int32_t)(now_ms - next_send_ms) >= 0)
        {
            if (missed >= TIMESYNC_SAMPLES)
                NextTarget();
            else
                SendSample();
        }
    }

    // Immediately starts a 3-sample sync round for a single target.
    // Available for CLI or ad-hoc use; the normal initial sync is now
    // device-initiated (node sends CID 11 to core after ID assignment).
    void SyncTarget(uint16_t target)
    {
        if (state != Idle)
            return;

        targets[0] = target;
        target_count = 1;
        current_target = 0;
        sample_count = 0;
        missed = 0;
        offset_sum = 0;
        state = Waiting;
        SendSample();
    }

    // Accumulates a time-offset sample from a target; moves to the next target once 3 samples are collected.
    void HandleResponse(uint16_t target, int32_t offset)
    {
        if (state != Waiting || current_target >= target_count)
            return;
        if (target != targets[current_target])
            return;

        offset_sum += offset;
        sample_count++;
        missed = 0;

        if (sample_count >= TIMESYNC_SAMPLES)
            NextTarget();
        else
            next_send_ms = DeviceStatus.UptimeMs + TIMESYNC_GAP_MS;
    }

private:
    State state = Idle;
    uint16_t targets[TIMESYNC_MAX_DEVICES];
    uint16_t target_count = 0;
    uint16_t current_target = 0;
    uint8_t sample_count = 0;
    uint8_t missed = 0;
    int64_t offset_sum = 0;
    uint32_t round_due_ms = 0;
    uint32_t next_send_ms = 0;

    // Builds the target list from the SN registry and starts the first sample round.
    void BeginRound()
    {
        target_count = 0;
        SNDB::IterReset();
        RegistryEntry entry;
        while (SNDB::IterNext(entry))
        {
            if (entry.shortID == DeviceStatus.ShortAddress)
                continue;
            if (target_count < TIMESYNC_MAX_DEVICES)
                targets[target_count++] = entry.shortID;
        }

        if (target_count == 0)
        {
            state = Idle;
            round_due_ms = DeviceStatus.UptimeMs + TIMESYNC_INTERVAL_MS;
            return;
        }

        current_target = 0;
        sample_count = 0;
        missed = 0;
        offset_sum = 0;
        state = Waiting;
        SendSample();
    }

    // Sends one time-sample request (CID 11) to the current target and schedules the retry.
    void SendSample()
    {
        if (current_target >= target_count)
            return;

        uint32_t sent_time = TimeFromBoot();
        PacketConstruct(&tx_frame, targets[current_target],
                         MakeService(ServiceType::Device, 11),
                         MakeService(ServiceType::Device, 11),
                         FLAG_REQACK | FLAG_START | FLAG_STOP,
                         (const uint8_t *)&sent_time, sizeof(uint32_t));
        DispatchPacket(tx_frame);

        missed++;
        next_send_ms = DeviceStatus.UptimeMs + TIMESYNC_GAP_MS;
    }

    // Pushes the averaged offset to the current target, then advances to the next one or ends the round.
    void NextTarget()
    {
        if (sample_count >= TIMESYNC_SAMPLES)
            SendTimeOffset(targets[current_target], (int32_t)(offset_sum / TIMESYNC_SAMPLES));

        current_target++;
        sample_count = 0;
        missed = 0;
        offset_sum = 0;

        if (current_target >= target_count)
        {
            state = Idle;
            round_due_ms = DeviceStatus.UptimeMs + TIMESYNC_INTERVAL_MS;
        }
        else
        {
            SendSample();
        }
    }

    // Sends the computed time offset (CID 12) to a single node.
    void SendTimeOffset(uint16_t target, int32_t offset)
    {
        PacketConstruct(&tx_frame, target,
                         MakeService(ServiceType::Device, 12),
                         MakeService(ServiceType::Device, 12),
                         FLAG_START | FLAG_STOP,
                         (const uint8_t *)&offset, sizeof(int32_t));
        DispatchPacket(tx_frame);
    }
};

TimeSyncService TimeSync;

#endif // TYPE_CORE

