#pragma once
#include "CompiledModuleRecords.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace nlang {

//Container magic: 8 bytes, NUL-padded spelling of "NLANGCU". Single source
//of truth for the writer, the reader and the packaging smoke
//(verify_package.py keeps its own byte copy — Python cannot include this header).
inline constexpr char NCU_MAGIC[8] = {'N', 'L', 'A', 'N', 'G', 'C', 'U', '\0'};
//Artifact extension; the spelling has changed before — never hard-code
//its length.
inline constexpr const char *NCU_EXTENSION = ".ncu";
//Package archive extension (the container, distinct from the unit).
inline constexpr const char *NPKG_EXTENSION = ".npkg";

//Table keys ("functions/structs/classes" name fields) are package-
//qualified linkage identity ("<unit>.<name>"). User-visible renders
//(Object.toString, debugger value displays) name the type as source
//wrote it — the leaf after the last dot. Frame/function displays stay
//qualified (that prefix is pinned debugger output).
inline std::string LeafNameOfKey(const std::string &key)
{
    const size_t lastDot = key.find_last_of('.');
    return lastDot == std::string::npos ? key : key.substr(lastDot + 1);
}

//.ncu format version. Single source of truth shared by the writer
//(ModuleSaver.cpp) and the reader (ModuleLoader); bump both atomically.
inline constexpr uint16_t NCU_FORMAT_MAJOR = 2;
//v1.7 (Phase 11): stdlib namespace intrinsics + reserved namespaces. No
//field-layout change, but the later relational string opcodes share this
//version step, so older VMs must refuse these modules outright.
//v1.8 (Phase 13): first-class function values — RTK_Func heap records,
//the OP_MakeFunc/OP_CallDelegate/OP_Eq_func/OP_Ne_func/OP_Func_to_str
//opcodes (Step 1: free function references) plus OP_MakeBoundFunc/
//OP_MakeVFunc/OP_CallDelegateOut (Step 2: bound method references).
//Older VMs cannot execute these opcodes; refuse outright.
//v1.9 (debugger): per-function source file path — CompiledFunction::
//sourceFile, emitted unconditionally after the locals block. Cross-module
//breakpoint addressing (b file.n:LINE) depends on it. No new opcodes.
//v1.10 (array redesign): SEMANTIC floor — no new serialized fields, but
//array struct/class fields now store RTK_Array as their fieldTypeKinds
//entry (previously the element kind, e.g. RTK_Int32). A v1.9 module from
//an older ncc carries the old field-kind meaning and would misroute GC
//marking and struct serialization dispatch; the loader refuses it.
//v1.11 (C-period generic array args): SEMANTIC floor — no new serialized
//fields, but array-typed elements of generic containers (List<T[]> /
//Dict keys and values) now flow as raw array handles with no boxing, and
//foreach loop variables over them occupy RTK_Array local slots the GC
//traces. A v1.10 module from an older ncc boxes those elements into
//primitive slots the GC never traces; the loader refuses it.
//v1.12 (type descriptors): true formal / return / field types — recursive
//TypeDesc payloads (see TypeDesc.h) after the source-file block (per
//formal, count-prefixed; return, gated on returnTypeKind != RTK_Void)
//and per struct/class field. Consumers rebuild imported stubs with real
//signatures instead of return-kind placeholders, so call-site type
//checking for imported callees is now performed; the loader refuses
//v1.11 and older outright.
//v1.13 (basic types): ten new scalar RTK kinds (RTK_Byte..RTK_Char,
//10..19; 8/9 stay the TypeDesc descriptor-only kinds), generalized
//numeric opcodes with kind immediates, bool conditions, 8-byte scalar
//slots (long/ulong/double) and the unified 3-slot boxed record.
//v1.14 (debugger): per-local declaration PC in the locals block — the
//debug views filter locals whose declaration the paused PC has not
//reached yet (scope visibility). A v1.13 module lacks the two bytes per
//local and misparses every name that follows.
//v2.0 (phase 6 runtime linking): LAYOUT bump — the header gains the
//module's dotted path (the compile unit's package identity) right after
//the version fields, and the version floor/ceiling move to the 2.x line.
//A v1.x module misparses at the first new field; the loader refuses it.
//v2.1 merges the master and dev lines: the 1.13/1.14 content changes
//ride on the 2.0 layout. Every older module — any 1.x, and 2.0 images
//from the pre-merge phase-6 line — is refused outright.
//v2.2 (cross-module function values): the TypeDesc grammar gains
//RTK_Func (u8 paramCount + paramCount+1 nested descriptors — params
//then return; see TypeDesc.h). A v2.1 module carries no func signatures:
//stub formals degrade to int32 placeholders, so function values cannot
//cross module borders. Refuse v2.1 and older outright.
//v2.3 (cross-module complex defaults): every DefaultValueDesc record
//gains a u16-length-prefixed ExprDesc payload (see exprBytes). A v2.2
//module carries only literal defaults; expression defaults (formal
//references, producing-module calls, arithmetic over them) cannot cross
//module borders. Refuse v2.2 and older outright.
//v2.4 (static members): every function record gains a static flag so
//name-based dispatch can skip static members (they live in a separate
//namespace from instance dispatch). Class records gain the exported
//public-static method indices, a per-unit interface table carries the
//interface statics, and function import records gain the static flag
//the linker matches on. A v2.3 module has no flag byte and misparses
//every record after the native flag; refuse v2.3 and older.
//v2.5 (interface constants): every interface record gains a constants
//block after its static method indices — per constant, a u16 name, a
//length-prefixed TypeDesc, and a DefaultValueDesc folded value. A v2.4
//module has no such block and misparses the bytes after the method
//indices as the constant count; refuse v2.4 and older.
//v2.6 (type-init modernization): every struct record gains a field-
//defaults block after its type descriptors — a u16 count (== fieldCount)
//plus one DefaultValueDesc per field (RTK_Void = no default). The same
//version line also carries the constructor-index and import ownerKind/
//ownerOrdinal segments; a v2.5 module has no defaults block and misparses
//the bytes after the type descriptors as the next record; refuse v2.5
//and older.
//v2.7 (imported instance members): class field lists become OWN-ONLY —
//the runtime derives the full instance layout by walking the superclass
//chain base-first, so a v2.6 image (whose list already carries inherited
//fields) would double-count them and misroute every field offset; this
//is a semantic floor even where the bytes would parse. The class record
//drops the public-static method index block (statics now ride the method
//table, flagged by a per-method access byte) and appends the parallel
//methodAccess/methodVirtual byte arrays after the method indices plus a
//constructorAccess byte array after the constructor indices; the struct
//record appends its constructorAccess array; the interface record gains
//an instance-method signature section after its constants block (name,
//paramCount, per-formal ParamTypeDesc, return TypeDesc, DefaultValueDesc
//list) so consumer units can resolve and contract-check imported
//instance members. Refuse v2.6 and older outright.
//v2.8 (concurrency merge, from the master line): every function record
//gains a trailing eval-area handle mask (uint32 byte count + packed
//2-bit classes; see EvalSlotClass). It gives the GC a per-slot root
//class for the whole frame band, replacing the conservative both-domain
//value test. The master line's other concurrency releases changed no
//wire shape (K1 thread stubs and K3 futures/bounded channels ride the
//existing intrinsic-stub serialization). A v2.7 module has no trailing
//mask and misparses the next record's leading bytes; refuse v2.7 and
//older outright.
//v2.9 (0.10.14): per-enum member-method tables (enumMethodIndices /
//enumMethodAccess), parallel to enumNames/enumKeys — the fourth
//instance of "a type record carries its own member-function indices"
//(classes methodIndices, structs constructorIndices, interfaces
//staticMethodIndices). Minor bump: every .ncu recompiles with the
//current ncc (the loader's hard floor, unchanged).
inline constexpr uint16_t NCU_FORMAT_MINOR = 10;

//--- Slot-width ABI (0.7.5) ----------------------------------------------
//The frame and the heap use two different stride contracts; both live
//here because they are shared by the backend (producer) and the
//executor (consumer) — a per-TU constant on either side would drift.
//
//Frame: every slot — params, locals, temps, return, call staging, eval
//area — is one uniform 8-byte cell. 4-byte kinds live value-extended in
//the low half (narrow ints sign/zero-extended by their row); the upper
//half may hold garbage that no kind-correct consumer ever reads.
//long/ulong/double use all 8 bytes. One uniform stride replaces the
//former 4-byte per-TU VALUE_SIZE statics, so no emission site has to
//pick a copy width by kind (returns, switch value slots, temps and
//eval-area claims are width-correct by construction).
static constexpr uint16_t kFrameSlotBytes = 8;

//Heap: records are vectors of int32 cells. Struct/class data fields are
//uniform 2-cell (8-byte) slots — field i sits at cell 2i (struct) /
//1+2i (class, cell 0 = classIdx for virtual dispatch) — which keeps the
//positional field walks kind-free. Array elements are registry-driven:
//1 cell for kinds up to 4 bytes, 2 cells for the long/double family.
static constexpr uint16_t kHeapCellBytes = 4;
static constexpr uint16_t kHeapFieldStrideBytes = 8;  // = 2 * kHeapCellBytes
//Array record header (stage-2 unified layout):
//  [0]=RTK_Array, [1]=arrayTypeIdx, [2]=rank, [3]=elemCount,
//  [4..4+rank-1]=dims (elements per dimension), then padding so the
//  payload base sits at an EVEN cell index — the 8-byte element family
//  (long/double) is naturally aligned; SIMD loads in native kernels
//  rely on it. Element i of a rank-1 array lives at
//  ArrayHeaderCells(rank) + i*ArrayElemCells(elemKind); for rank >= 2
//  the payload is row-major over dims.
inline uint32_t ArrayHeaderCells(uint8_t rank)
{
    const uint32_t cells = 4u + rank;
    return cells + (cells & 1u);
}
//Element-count cap for one array, language creation and native minting
//alike: the single allocation choke point raises the OutOfBounds family
//beyond it. 100M elements is 800MB at the widest element — every
//legitimate operand fits, and 4 + n*cells can never overflow int32.
static constexpr int32_t kMaxArrayElems = 100 * 1000 * 1000;

//--- Eval-area slot classes (K2 slice 5) ----------------------------------
//Precise GC root class for one frame-band slot (the band is
//[paramCount, localsSize) — return slot, temps, call staging, evalArea
//and the user locals). Packed 2 bits per slot into CompiledFunction::
//evalAreaHandleMask, byte i's low half holding slot 4i. The class says
//which domain a slot's value can belong to at a safepoint; the GC tests
//only that domain (None skips the slot entirely). Dual == StrHandle |
//HeapIndex by construction — overlapping claim classes merge by OR, a
//union that can only widen toward Dual (no site's claim narrows what
//another site's claim widened; soundness of a narrow class is each
//site's own contract, see EvalAreaClaim).
enum EvalSlotClass : uint8_t {
    ESC_None      = 0,  //scalar kinds — never a reference, never tested
    ESC_StrHandle = 1,  //string-store handle (RTK_String slots)
    ESC_HeapIndex = 2,  //struct/class/array/func heap index
    ESC_Dual      = 3   //unknown — value-test both domains (conservative)
};

//Cells occupied by one array element of the given RTK elem kind:
//2 for the 8-byte scalar family (long/ulong, double when it lands),
//1 for everything else (≤4-byte scalars and every reference kind).
//Element i of an array record lives at cell 3 + i*cells.
inline int ArrayElemCells(uint8_t elemRtk)
{
    int pi = ScalarPrimIndexOfRtk(elemRtk);
    return (pi >= 0 && kScalarPrims[pi].slotWidth > 4) ? 2 : 1;
}

struct CompiledModule {
    //v2.0: the module's dotted path (the compile unit's package identity).
    std::string modulePath;
    std::string name;
    std::vector<CompiledFunction> functions;
    std::vector<std::string> stringConstants;
    std::vector<CompiledStruct> structs;
    std::vector<CompiledClass> classes;
    //0.9.10 (v2.4): interface statics, keyed like enumKeys (qualified).
    std::vector<CompiledInterface> interfaces;
    std::vector<CompiledArrayType> arrayTypes;
    //Phase 8e-9b: per-enum value name tables. Outer index = enumDefIdx (in
    //declaration order across the module), inner index = enum int value.
    //Used by OP_Enum_to_str to render `Color.Red.toString()` → "Red".
    std::vector<std::vector<std::string>> enumNames;
    //v2.0 phase 6: qualified key per enum entry, parallel to enumNames
    //(same count; placeholder slots carry the target's key with empty
    //names). nlink name-addresses enum import slots and dedups enums
    //by key — enumNames alone carries no type identity.
    std::vector<std::string> enumKeys;
    //v2.9 (0.10.14): per-enum member-method tables, parallel to
    //enumNames/enumKeys (every construction site keeps the four vectors
    //in lockstep, including placeholder slots, which carry empty
    //vectors). enumMethodIndices[i] = the enum's method functions'
    //indices into the producer's own function table, declaration order;
    //enumMethodAccess[i] = the per-method declared access byte (FA_*),
    //mirroring the class methodAccess gate on the consumer side.
    std::vector<std::vector<uint16_t>> enumMethodIndices;
    std::vector<std::vector<uint8_t>> enumMethodAccess;

    //v2.0 phase 6: cross-unit reference slots. Each unit's tables hold its
    //OWN declarations first (indices 0..n-1); slots n..n+m are PLACEHOLDER
    //entries pushed by codegen when the unit references another unit's
    //symbol, described here. nlink (the load-time linker) resolves each
    //slot against the linked closure's tables and remaps the unit's
    //bytecode operands (local slot → global index). modulePath drives the
    //import-closure discovery; paramCount disambiguates overloads
    //(functions share the qualified key across overloads).
    struct SymbolImport {
        std::string modulePath;   //target unit's dotted path
        std::string name;         //qualified key; bare name when ownerClassKey is set
        uint32_t paramCount = 0;  //functions only
        //Functions only (empty for the type tables): "" = namespace-level
        //function (name is its qualified key); non-empty = a method or
        //constructor of that class's qualified key (name is the bare name;
        //the class's bare name for a constructor). Table keys stay bare —
        //runtime dispatch is by name — the owner key lives in the slot
        //record only (phase6 design section 2).
        std::string ownerClassKey;
        //0.9.10 (v2.4): part of the linkage identity — a static and an
        //instance member of the same name and arity are distinct slots.
        bool isStatic = false;
        //0.9.11 (v2.6): which table kind ownerClassKey addresses —
        //0 = namespace-level (ownerClassKey empty), 1 = class member,
        //2 = struct member. The linker dispatches its owner lookup on
        //this (a bare key alone cannot tell a struct constructor from a
        //same-named class method). Unused (0) on the type import lists.
        uint8_t ownerKind = 0;
        //0.9.11 (v2.6), generalized 0.9.12 D-new-8: the member's ordinal
        //in the owner's table — constructors: index into
        //constructorIndices; methods and statics: position in
        //methodIndices with constructor entries skipped. Overloaded
        //members share name+paramCount, so the ordinal single-selects.
        //0xFFFF = no ordinal (namespace-level records, malformed or
        //legacy method matches).
        uint16_t ownerOrdinal = 0xFFFF;
    };
    std::vector<SymbolImport> functionImports;   //slots after own functions
    std::vector<SymbolImport> classImports;      //slots after own classes
    std::vector<SymbolImport> structImports;     //slots after own structs
    std::vector<SymbolImport> enumImports;       //slots after own enum tables

    //Runtime only (never serialized): set by nlink after linking — the
    //global function index of this module's main(), or the entry the tool
    //layer selected. -1 until then.
    int32_t entryPoint = -1;

    int FindFunction(const std::string& funcName) const {
        for (int i = 0; i < static_cast<int>(functions.size()); ++i)
            if (functions[i].name == funcName)
                return i;
        return -1;
    }

    int FindStruct(const std::string& structName) const {
        for (int i = 0; i < static_cast<int>(structs.size()); ++i)
            if (structs[i].name == structName)
                return i;
        return -1;
    }

    int FindClass(const std::string& className) const {
        for (int i = 0; i < static_cast<int>(classes.size()); ++i)
            if (classes[i].name == className)
                return i;
        return -1;
    }

    int FindEnum(const std::string& enumKey) const {
        for (int i = 0; i < static_cast<int>(enumKeys.size()); ++i)
            if (enumKeys[i] == enumKey)
                return i;
        return -1;
    }

    int FindArray(uint8_t elemKind, uint16_t elemTypeIdx, uint8_t rank) const {
        for (int i = 0; i < static_cast<int>(arrayTypes.size()); ++i)
            if (arrayTypes[i].elemKind == elemKind
                && arrayTypes[i].elemTypeIdx == elemTypeIdx
                && arrayTypes[i].rank == rank)
                return i;
        return -1;
    }
};

//Serialize a CompiledModule to a stream in the current .ncu format.
//Single writer shared by the artifact writers (VmBackend::
//WriteModuleArtifact / the .npkg packer) and unit tests, so
//hand-written byte layouts can never drift from the reader again.
bool WriteCompiledModule(std::ostream& fs, const CompiledModule& mod,
                         const std::string& entryKey);

} // namespace nlang
