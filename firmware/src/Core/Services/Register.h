#pragma once

// Register service (Docs/Services/Register.md): generic access to every block's fields
// through a 32-bit BlockInfo (type|instance|field|key), plus Save/Recall and the dynamic
// block management commands.
//
// Split into parts, included here in order so the service stays one translation unit:
//   RegisterDefs.h      BlockInfo accessors, response helpers
//   RegisterEnumerate.h CID 0/1 (block types, a block's Field&Keys)
//   RegisterRead.h      CID 2
//   RegisterWrite.h     CID 3
//   RegisterPersist.h   CID 4 (Recall All) / CID 5 (Save All) and 0x10-0x13
//   RegisterDispatch.h  the shared accessors and the dispatcher

// Types the class declaration below needs (the split files pull these in too).
#include "Core/Functions/Packet.h"
#include "Core/Functions/Memory.h"

#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/AppInterface.h"
#include "Core/Services/StaticMemory.h"
#include "Blocks/DeviceInfo.h"
#ifdef USE_SCRIPTS
#include "Core/Services/Script.h"
#endif

// Which list is being streamed (defined here so the class declaration can name it; the
// enumerate part of the service uses it).
enum class EnumSrc : uint8_t { System, Static, Dynamic, Script };

// Register service: the command handlers and their shared helpers, one instance per binary.
class RegisterService
{
public:
    void Init();                            // resets the service's own state
    bool Handle(const PacketFrame &frame);  // CID dispatch; true if consumed

    // Generic BlockInfo access (shared with the Subscriptions and Script services).
    bool RegisterResolveByBlockInfo(uint32_t bi, FieldResult &fr);
    bool RegisterGetByBlockInfo(uint32_t bi, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz);
    bool RegisterSetByBlockInfo(uint32_t bi, const ValueInfo &m, const uint8_t *val, uint16_t vlen);

    // Called by the boot path (Dispatcher.h's LoadAllBackups) until it self-wires.
    void StaticRecallAll();
#ifdef USE_DYNAMIC_BLOCKS
    bool RestoreDynamicBlock(uint16_t i);
#endif

    // The System block's computed-field getter and NetID write trigger: referenced as
    // function pointers from the block descriptor tables at namespace scope.
    static void* SystemGet(uint16_t field, uint8_t key);
#ifdef TYPE_CORE
    static bool OnSystemNetIdWrite(const StaticBlockDescriptor &block, uint16_t field,
                                   const void *data, uint16_t len);
#endif

private:
    // Former function-local statics of the System getter (RegisterRead.h), same order.
#ifdef TYPE_CORE
    uint8_t NetId = 0;
#endif
    uint8_t SysValueBuf[24] = {0};

    // Response helpers (RegisterDefs.h).
    void SendFieldLikeResponse(const PacketFrame &frame, uint32_t bi, const ValueInfo &v,
                               const uint8_t *data, uint16_t len);
    void SendValueResponse(const PacketFrame &frame, uint32_t bi, const ValueInfo &v,
                           const uint8_t *data, uint16_t len);
    void SendBlockMetaResponse(const PacketFrame &frame, uint32_t bi, uint16_t type,
                               uint8_t map_count, const char *name, uint16_t name_len);
    void SendFieldResponse(const PacketFrame &frame, uint32_t bi, const FieldResult &fr);
    void SendKeyResponse(const PacketFrame &frame, uint32_t bi, const KeyResult &kr);
    void SendBlockIndexAck(const PacketFrame &frame, uint8_t index);
#ifdef USE_DYNAMIC_BLOCKS
    uint16_t DynamicFieldCount(const DynamicBlockDescriptor &b);
    void ReplyDynamicBlockOrField(const PacketFrame &frame, uint32_t bi,
                                  DynamicBlockDescriptor &block, uint8_t field, uint8_t key);
#endif

    // Enumerate - CID 0/1 (RegisterEnumerate.h).
    uint16_t EnumTypeWordOf(uint16_t type, size_t instances);
    uint16_t EnumTypeCount();
#ifdef USE_SCRIPTS
    uint16_t EnumScriptWord(uint16_t inst, uint16_t i);
#endif
    uint16_t EnumWord(EnumSrc src, const void *ctx, uint16_t i);
    void SendU16Stream(const PacketFrame &frame, uint16_t count, EnumSrc src, const void *ctx);
    void SendTypeWordsStream(const PacketFrame &frame);
    void HandleEnumerateBlocks(const PacketFrame &frame);
    void HandleEnumerateFields(const PacketFrame &frame);

    // Read helpers - CID 2 (RegisterRead.h).
    static bool SystemFillValue(uint8_t field, uint8_t *vbuf);
    const StaticBlockDescriptor* FindBlock(uint16_t type, uint8_t inst);
#ifdef USE_DYNAMIC_BLOCKS
    void HandleDynamicBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t inst,
                                uint8_t field, uint8_t key);
#endif
#ifdef USE_SCRIPTS
    void HandleScriptBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t inst,
                               uint8_t field, uint8_t key);
#endif
    void HandleStaticBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t type,
                               uint8_t inst, uint8_t field, uint8_t key);

    // Write helpers - CID 3 (RegisterWrite.h).
#ifdef USE_DYNAMIC_BLOCKS
    void HandleDynamicBlockWrite(const PacketFrame &frame, uint16_t inst, uint8_t field,
                                 uint8_t key, const ValueInfo *desc, const uint8_t *val,
                                 uint16_t vlen);
#endif
#ifdef USE_SCRIPTS
    void HandleScriptBlockWrite(const PacketFrame &frame, uint16_t inst, uint8_t field,
                                uint8_t key, const ValueInfo *desc, const uint8_t *val,
                                uint16_t vlen);
#endif
    void HandleStaticBlockWrite(const PacketFrame &frame, uint16_t type, uint8_t inst,
                                uint8_t field, uint8_t key, const ValueInfo *desc,
                                const uint8_t *val, uint16_t vlen);

    // Save/Recall and dynamic management - CID 4/5, 0x10-0x13 (RegisterPersist.h).
    bool StaticSaveAll();
#ifdef USE_DYNAMIC_BLOCKS
    bool DynamicSaveAll();
    bool DynamicRecallAll();
    void HandleCreateDynamic(const PacketFrame &frame, uint16_t index);
    void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx);
    void HandleGetName(const PacketFrame &frame, uint16_t block_idx);
    void HandleSetName(const PacketFrame &frame, uint16_t block_idx);
#endif
    void HandleSaveRecallAll(const PacketFrame &frame, bool save);
};

// One instance per binary.
RegisterService Register;

// Temporary forwarders still used by the Subscriptions service (removed when it self-wires).
inline bool RegisterResolveByBlockInfo(uint32_t bi, FieldResult &fr) { return Register.RegisterResolveByBlockInfo(bi, fr); }
inline bool RegisterGetByBlockInfo(uint32_t bi, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) { return Register.RegisterGetByBlockInfo(bi, m, vbuf, vsz); }
inline bool RegisterSetByBlockInfo(uint32_t bi, const ValueInfo &m, const uint8_t *val, uint16_t vlen) { return Register.RegisterSetByBlockInfo(bi, m, val, vlen); }

#include "Core/Services/RegisterDefs.h"
#include "Core/Services/RegisterEnumerate.h"
#include "Core/Services/RegisterRead.h"
#include "Core/Services/RegisterWrite.h"
#include "Core/Services/RegisterPersist.h"
#include "Core/Services/RegisterDispatch.h"

// The service's own state reset. Left for the parent to wire into Main.
inline void RegisterService::Init()
{
#ifdef TYPE_CORE
    NetId = 0;
#endif
    memset(SysValueBuf, 0, sizeof(SysValueBuf));
}
