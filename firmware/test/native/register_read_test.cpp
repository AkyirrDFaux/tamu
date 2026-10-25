// Host-side test for what a register read of a row with no bytes reports (card t_e57a9667).
//
// The defect: a size-0 register row resolved fine and was copied out as a 0-length value of the
// row's own type. The script's read then handed those 0 bytes to the assignment, which failed the
// scalar decode (a Number needs 4 bytes) and the whole access surfaced as SCRIPT_ERR_TYPE - a type
// complaint about a row that simply has no bytes in it, so the reader goes hunting for a type
// problem in the script. An empty row is a register problem, not a type problem.
//
// The three cases this pins:
//   1. a row that exists with size 0  -> not readable -> the read carries SCRIPT_ERR_REGISTER
//   2. a row that does not exist       -> not readable -> SCRIPT_ERR_REGISTER
//   3. a row that IS readable but whose contents do not fit the destination -> untouched by this
//      guard; the type check stays the type check (a short Number row is readable, so it still
//      reaches the decode that reports SCRIPT_ERR_TYPE)
//
// What can run on the host: the decision itself (ValueReadable, Core/Functions/MemoryTypes.h - the
// read-side twin of ValueUpdateApplicable) and the real dynamic-block lookup that produces the rows
// (Core/Functions/MemoryBlocks.h, the header value_update_test.cpp already drives). The read
// instruction's own code cannot be linked here (it pulls the packet and register layers), so
// run.sh asserts that wiring on the source: the copy accessor asks ValueReadable, and the read
// instruction carries SCRIPT_ERR_REGISTER for a failed access while still returning a genuine
// assignment error.
//
// No device dependency: built like align/stride/value-update (see run.sh).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/MemoryBlocks.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                        #got, #want, g, w);                                          \
        }                                                                            \
    } while (0)

int main() {
    const ValueInfo Bool1   = { (uint16_t)DataType::Bool,   1, 0 };
    const ValueInfo Index2  = { (uint16_t)DataType::Index,  2, 0 };
    const ValueInfo Number2 = { (uint16_t)DataType::Number, 2, 0 }; // short, not empty
    const ValueInfo Bool0   = { (uint16_t)DataType::Bool,   0, 0 }; // the defect's row
    const ValueInfo None0   = { (uint16_t)DataType::None,   0, 0 }; // a missing key's default

    // ---- The decision, on descriptors alone: a row with bytes reads, a row without does not.
    CHECK(ValueReadable(Bool1));
    CHECK(ValueReadable(Index2));
    CHECK(!ValueReadable(Bool0));
    CHECK(!ValueReadable(None0));
    // Case 3's precondition: the guard fires on exactly 0. A short-but-non-empty row stays
    // readable, so a genuine decode/type complaint about its contents is still raised by the
    // assignment - this guard must never swallow it.
    CHECK(ValueReadable(Number2));

    // ---- A live dynamic block: the real lookup the read path resolves through.
    DynamicBlockDescriptor Block;
    Block.Present = true;
    uint8_t One = 1;
    CHECK(Block.SetEntry(6, 0, &One, 1, Bool1)); // the bambi emote switch, Dynamic.0 field 6

    KeyResult Live = Block.GetKey(6, 0);
    CHECK(Live.Exists);
    CHECK_EQ(Live.Meta.Size, 1);
    CHECK(Live.DataPtr != nullptr);
    CHECK(ValueReadable(Live.Meta)); // a row with a byte reads

    // ---- Case 2: no such row at all. GetKey's default result is the descriptor the accessor
    // would otherwise copy: Size 0 and no data, i.e. unreadable - the read fails, not the type.
    KeyResult Missing = Block.GetKey(9, 0);
    CHECK(!Missing.Exists);
    CHECK_EQ(Missing.Meta.Size, 0);
    CHECK(Missing.DataPtr == nullptr);
    CHECK(!ValueReadable(Missing.Meta));

    // ---- Case 1: a row that EXISTS with no bytes. SetEntry now refuses a 0-length write
    // (ValueUpdateApplicable), so this is the shape a stored/recalled block still presents - the
    // one GetKey documents as "a size-0 entry exists with no value". Driven through the real
    // descriptor with a stored table, because that is exactly what the read path meets.
    DynamicEntry Row;
    Row.FieldKey = MakeFieldKey(7, 0);
    Row.MemoryOffset = 0;
    Row.Info = Bool0;

    DynamicBlockDescriptor Dead;
    Dead.Present = true;
    Dead.Table = &Row; // borrowed: this block owns nothing, so it must not be Released
    Dead.EntryCount = 1;
    Dead.EntryAllocated = 0;

    KeyResult Zero = Dead.GetKey(7, 0);
    CHECK(Zero.Exists);              // it IS a row - which is why this is a register problem
    CHECK_EQ(Zero.Meta.Size, 0);
    CHECK(Zero.DataPtr == nullptr);  // and it has nothing behind it
    CHECK(!ValueReadable(Zero.Meta));// so the read must fail: SCRIPT_ERR_REGISTER, not TYPE
    // The row's type is untouched by the decision: the access fails on the row, not on the type.
    CHECK_EQ(ValueInfoType(Zero.Meta.Type), (uint16_t)DataType::Bool);

    Dead.Table = nullptr; // leave the borrowed table alone
    Block.Release();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
