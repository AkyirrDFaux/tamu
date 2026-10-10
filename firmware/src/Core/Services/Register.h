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
    bool Handle(const PacketFrame &Frame);  // CID dispatch; true if consumed

    // Generic BlockInfo access (shared with the Subscriptions and Script services).
    bool RegisterResolveByBlockInfo(uint32_t Bi, FieldResult &Fr);
    bool RegisterGetByBlockInfo(uint32_t Bi, ValueInfo &Meta, uint8_t *Vbuf, uint8_t &Vsz);
    bool RegisterSetByBlockInfo(uint32_t Bi, const ValueInfo &Meta, const uint8_t *Val, uint16_t Vlen);

    // Called by the boot path (Dispatcher.h's LoadAllBackups) until it self-wires.
    void StaticRecallAll();
#ifdef USE_DYNAMIC_BLOCKS
    bool RestoreDynamicBlock(uint16_t i);
#endif

    // The System block's computed-field getter and NetID write trigger: referenced as
    // function pointers from the block descriptor tables at namespace scope.
    static void* SystemGet(uint16_t Field, uint8_t Key);
#ifdef TYPE_CORE
    static bool OnSystemNetIdWrite(const StaticBlockDescriptor &Block, uint16_t Field,
                                   const void *Data, uint16_t Len);
#endif

private:
    // Former function-local statics of the System getter (RegisterRead.h), same order.
#ifdef TYPE_CORE
    uint8_t NetId = 0;
#endif
    uint8_t SysValueBuf[24] = {0};

    // Response helpers (RegisterDefs.h).
    void SendFieldLikeResponse(const PacketFrame &Frame, uint32_t Bi, const ValueInfo &Info,
                               const uint8_t *Data, uint16_t Len);
    void SendValueResponse(const PacketFrame &Frame, uint32_t Bi, const ValueInfo &Info,
                           const uint8_t *Data, uint16_t Len);
    void SendBlockMetaResponse(const PacketFrame &Frame, uint32_t Bi, uint16_t Type,
                               uint8_t MapCount, const char *Name, uint16_t NameLen);
    void SendFieldResponse(const PacketFrame &Frame, uint32_t Bi, const FieldResult &Fr);
    void SendKeyResponse(const PacketFrame &Frame, uint32_t Bi, const KeyResult &Kr);
    void SendBlockIndexAck(const PacketFrame &Frame, uint8_t Index);
#ifdef USE_DYNAMIC_BLOCKS
    uint16_t DynamicFieldCount(const DynamicBlockDescriptor &Block);
    void ReplyDynamicBlockOrField(const PacketFrame &Frame, uint32_t Bi,
                                  DynamicBlockDescriptor &Block, uint8_t Field, uint8_t Key);
#endif

    // Enumerate - CID 0/1 (RegisterEnumerate.h).
    uint16_t EnumTypeWordOf(uint16_t Type, size_t Instances);
    uint16_t EnumTypeCount();
#ifdef USE_SCRIPTS
    uint16_t EnumScriptWord(uint16_t Inst, uint16_t i);
#endif
    uint16_t EnumWord(EnumSrc Src, const void *Ctx, uint16_t i);
    void SendU16Stream(const PacketFrame &Frame, uint16_t Count, EnumSrc Src, const void *Ctx);
    void SendTypeWordsStream(const PacketFrame &Frame);
    void HandleEnumerateBlocks(const PacketFrame &Frame);
    void HandleEnumerateFields(const PacketFrame &Frame);

    // Read helpers - CID 2 (RegisterRead.h).
    static bool SystemFillValue(uint8_t Field, uint8_t *Vbuf);
    const StaticBlockDescriptor* FindBlock(uint16_t Type, uint8_t Inst);
#ifdef USE_DYNAMIC_BLOCKS
    void HandleDynamicBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Inst,
                                uint8_t Field, uint8_t Key);
#endif
#ifdef USE_SCRIPTS
    void HandleScriptBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Inst,
                               uint8_t Field, uint8_t Key);
#endif
    void HandleStaticBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Type,
                               uint8_t Inst, uint8_t Field, uint8_t Key);

    // Write helpers - CID 3 (RegisterWrite.h).
#ifdef USE_DYNAMIC_BLOCKS
    void HandleDynamicBlockWrite(const PacketFrame &Frame, uint16_t Inst, uint8_t Field,
                                 uint8_t Key, const ValueInfo *Desc, const uint8_t *Val,
                                 uint16_t Vlen);
#endif
#ifdef USE_SCRIPTS
    void HandleScriptBlockWrite(const PacketFrame &Frame, uint16_t Inst, uint8_t Field,
                                uint8_t Key, const ValueInfo *Desc, const uint8_t *Val,
                                uint16_t Vlen);
#endif
    void HandleStaticBlockWrite(const PacketFrame &Frame, uint16_t Type, uint8_t Inst,
                                uint8_t Field, uint8_t Key, const ValueInfo *Desc,
                                const uint8_t *Val, uint16_t Vlen);

    // Save/Recall and dynamic management - CID 4/5, 0x10-0x13 (RegisterPersist.h).
    bool StaticSaveAll();
#ifdef USE_DYNAMIC_BLOCKS
    bool DynamicSaveAll();
    bool DynamicRecallAll();
    void HandleCreateDynamic(const PacketFrame &Frame, uint16_t Index);
    void HandleDeleteDynamic(const PacketFrame &Frame, uint16_t BlockIdx);
    void HandleGetName(const PacketFrame &Frame, uint16_t BlockIdx);
    void HandleSetName(const PacketFrame &Frame, uint16_t BlockIdx);
#endif
    void HandleSaveRecallAll(const PacketFrame &Frame, bool Save);
};

// One instance per binary.
RegisterService Register;

// Temporary forwarders still used by the Subscriptions service (removed when it self-wires).
inline bool RegisterResolveByBlockInfo(uint32_t Bi, FieldResult &Fr) { return Register.RegisterResolveByBlockInfo(Bi, Fr); }
inline bool RegisterGetByBlockInfo(uint32_t Bi, ValueInfo &Meta, uint8_t *Vbuf, uint8_t &Vsz) { return Register.RegisterGetByBlockInfo(Bi, Meta, Vbuf, Vsz); }
inline bool RegisterSetByBlockInfo(uint32_t Bi, const ValueInfo &Meta, const uint8_t *Val, uint16_t Vlen) { return Register.RegisterSetByBlockInfo(Bi, Meta, Val, Vlen); }

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
