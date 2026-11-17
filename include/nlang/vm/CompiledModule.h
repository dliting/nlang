#pragma once
#include "CompiledModuleRecords.h"
#include <nlang/runtime/Config.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace nlang {

//Container magic: 8 bytes, NUL-padded "NLANGCU". Single source of truth
//for writer, reader and packaging smoke (verify_package.py keeps a byte
//copy — Python cannot include this header).
inline constexpr char NCU_MAGIC[8] = {'N', 'L', 'A', 'N', 'G', 'C', 'U', '\0'};
//Artifact extension; the spelling has changed before — never hard-code
//its length.
inline constexpr const char *NCU_EXTENSION = ".ncu";
//Package archive extension (the container, distinct from the unit).
inline constexpr const char *NPKG_EXTENSION = ".npkg";

//Table keys ("functions/structs/classes" name fields) are package-
//qualified linkage identity ("<unit>.<name>"). User-visible renders
//(Object.toString, debugger displays) name the leaf as source wrote it;
//frame/function displays stay qualified (pinned debugger output).
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
//step, so older VMs must refuse these modules outright.
//v1.8 (Phase 13): first-class function values — RTK_Func heap records,
//the OP_MakeFunc/OP_CallDelegate/OP_Eq_func/OP_Ne_func/OP_Func_to_str
//opcodes (Step 1: free function references) plus OP_MakeBoundFunc/
//OP_MakeVFunc/OP_CallDelegateOut (Step 2: bound methods); refuse older.
//v1.9 (debugger): per-function source file path — CompiledFunction::
//sourceFile, emitted unconditionally after the locals block. Cross-module
//breakpoint addressing (b file.n:LINE) depends on it. No new opcodes.
//v1.10 (array redesign): SEMANTIC floor — no new serialized fields, but
//array struct/class fields now store RTK_Array as their fieldTypeKinds
//entry (previously the element kind, e.g. RTK_Int32). A v1.9 module
//carries the old field-kind meaning and would misroute GC marking and
//struct serialization dispatch; the loader refuses it.
//v1.11 (C-period generic array args): SEMANTIC floor — no new serialized
//fields, but array-typed elements of generic containers (List<T[]> /
//Dict keys and values) now flow as raw array handles with no boxing, and
//foreach loop variables over them occupy RTK_Array local slots the GC
//traces. A v1.10 module boxes those elements into primitive slots the
//GC never traces; the loader refuses it.
//v1.12 (type descriptors): true formal / return / field types — recursive
//TypeDesc payloads (see TypeDesc.h) after the source-file block (per
//formal, count-prefixed; return, gated on returnTypeKind != RTK_Void)
//and per struct/class field. Consumers rebuild imported stubs with real
//signatures instead of return-kind placeholders, so imported callees get
//call-site type checking; the loader refuses v1.11 and older outright.
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
//stub formals degrade to int32 placeholders. Refuse v2.1 and older.
//v2.3 (cross-module complex defaults): every DefaultValueDesc record
//gains a u16-length-prefixed ExprDesc payload (see exprBytes). A v2.2
//module carries only literal defaults; expression defaults cannot cross
//module borders. Refuse v2.2 and older outright.
//v2.4 (static members): every function record gains a static flag so
//name-based dispatch can skip static members (a namespace separate from
//instance dispatch). Class records gain the exported public-static method
//indices, a per-unit interface table carries the interface statics, and
//function import records gain the static flag the linker matches on. A
//v2.3 module misparses every record after the native flag; refuse older.
//v2.5 (interface constants): every interface record gains a constants
//block after its static method indices — per constant, a u16 name, a
//length-prefixed TypeDesc, and a DefaultValueDesc folded value. A v2.4
//module misparses those bytes as the constant count; refuse v2.4 and older.
//v2.6 (type-init modernization): every struct record gains a field-
//defaults block after its type descriptors — a u16 count (== fieldCount)
//plus one DefaultValueDesc per field (RTK_Void = no default). The same
//version line carries the constructor-index and import ownerKind/
//ownerOrdinal segments. A v2.5 module misparses the bytes after the
//type descriptors as the next record; refuse v2.5 and older.
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
//(classes, structs, interfaces carry the first three). Minor bump:
//every .ncu recompiles with the current ncc (loader floor unchanged).
//v2.11 (stage-2 task 5): the TypeDesc array arm carries a rank byte
//(int[,] formals/returns round-trip; the rank-less rebuild degraded
//them to rank-1 and broke consumer indexing). Minor bump: every .ncu
//recompiles with the current ncc (the loader's hard floor, unchanged).
//v2.12 (v0.12.4): every class and struct record carries a const-
//aggregate marker byte (after superClassIdx / fieldCount). A v2.11
//module has no byte and misparses every record after the leading
//scalars; refuse v2.11 and older outright.
//v2.13 (v0.12.6): the static-slot section — a module-level
//staticSlots table between the enum and import sections, a
//staticFields block plus staticInitFuncIdx at each class-record tail,
//slotIdx per interface constant and staticInitFuncIdx per interface
//record, and a fifth import section (staticSlotImports). A v2.12
//module lacks the class/interface tail bytes and misparses every
//record after the first class; refuse v2.12 and older outright.
//v2.14 (v0.13.0): OP_Eq_struct/OP_Ne_struct join the opcode set —
//struct ==/!= now compiles to them instead of a raw handle compare.
//Wire layout unchanged; a v2.13 module silently compares heap indices
//(identity), so refuse v2.13 and older to keep struct ==/!= value-correct.
//v2.14 (v0.13.7 bitwise batch, unified): bitwise operators also enter
//the foldable default-expression grammar — the ED_BitNot tag (0x24)
//and the EDB_BitAnd / EDB_BitOr / EDB_BitXor binary codes (7/8/9)
//rode the same minor on the dev line; the unified format keeps both
//additions and refuses everything older through the v2.19 floor.
//v2.15 (v0.13.1): the module descriptor table — one row per generic
//instantiation descriptor the unit registers (canonical name +
//composition record bytes), between the string-constant and function sections.
//A v2.14 module has no section and misparses at the first record
//after the string table; refuse v2.14 and older outright.
//v2.16 (v0.13.1, ABI v12): every function record gains an instance-
//method flag byte after the static flag (the compiler's
//HasThisReceiver verdict). The native call bridge reads it to locate
//the receiver handle in the call's slot 0. A v2.15 module lacks the
//byte and misparses the record tail; refuse v2.15 and older outright.
//v2.17 (0.13.1a): every function record gains a generic flag byte
//after the instance-method flag plus a type-parameter name list for
//generic records; the free-call opcodes gain a u16 descriptor operand
//(OP_CallStatic family). A v2.16 module misparses the record tail and
//call operands; refuse v2.16 and older outright.
//v2.18 (0.13.1c): every function record gains an abstract flag byte
//after the instance-method flag; body-less abstract methods now carry
//a signature-only record (previously they registered nothing, so the
//wire lost them and consumer stubs could not see the obligation). A
//v2.17 module lacks the byte and misparses the record tail; refuse
//v2.17 and older outright.
//v2.19 (0.13.1c Task 8): every interface record gains a generic
//declaration tail (isTemplate byte + type-parameter name list) after
//staticInitFuncIdx — the generic-interface template marker a consumer
//stub rebuilds a generic DECLARATION from. A v2.18 module lacks the
//tail and misparses at the next interface record; refuse v2.18 and
//older outright.
//v2.20 (0.13.8 item 5): declared access bytes — free functions,
//top-level types (record tails) and enums (enumAccess, parallel to
//enumNames). The consumer stub rebuilds cross-module visibility; the
//loader refuses FA_Default and above as corrupt images. A v2.19 module
//lacks the bytes and misparses each record tail; refuse older.
//v2.21 (0.13.9 phase 3 Task 10): &/const marks ride the formal
//descriptor flags and per-element flag bytes follow every RTK_Func
//element; a v2.20 module misparses the func arm — refuse older.
//Minor bump: every .ncu recompiles with the current ncc.
inline constexpr uint16_t NCU_FORMAT_MINOR = 21;

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
//Element-count cap for one array, language creation and native creation
//alike: the single allocation choke point raises the OutOfBounds family
//beyond it. 100M elements is 800MB at the widest element — every
//legitimate operand fits, and 4 + n*cells can never overflow int32.
static constexpr int32_t kMaxArrayElems = 100 * 1000 * 1000;
//View record tail (v0.12.7, spec §4.2 of the array/tensor design). The
//strides/offset/target cells follow the dims run DIRECTLY — no even-
//padding: ArrayHeaderCells' pad serves the OWNED record's payload
//alignment only, and a view record has no payload.
//  [4+rank .. 4+2*rank-1] = strides (elements per logical step; 0 =
//                           broadcast axis, negative = reversed axis)
//INVARIANT — enforced at the consumer, not trusted from producers: a
//stride is the axis's true affine step in owner space; 0 exists only
//from broadcast semantics (broadcastTo or slicing an already-broadcast
//axis). Writeability is DERIVED from the record at the store gate
//(RejectZeroStrideStore): an axis blocks stores iff stride==0 AND
//dim>1 (several coordinates fold onto one owner cell). A size-1 axis
//addresses one cell through any stride, so no producer's encoding
//choice — fold, placeholder, or true step — can lock a legal write
//out or let an aliasing write through. Producers still record true
//steps where they fit; the one address-irrelevant corner (an
//int32-overflowing step on a single-element axis) takes a non-zero
//placeholder so the record carries no fake broadcast marker.
//  [4+2*rank]             = offset (element position in the OWNER's
//                           payload space)
//  [4+2*rank+1]           = target (always an owned RTK_Array record —
//                           depth one)
//Only view-aware consumers read past the dims run; ordinary payload
//consumers are kind-gated and never touch a view record. The shared
//header prefix ([0]=kind, [1]=arrayTypeIdx, [2]=rank, [3]=elemCount,
//[4..4+rank-1]=dims) is field-for-field identical to the owned layout,
//so every metadata consumer (length/shape/rank/toString/foreach length)
//reads views with zero branching.
inline uint32_t ArrayViewStridesBase(uint8_t rank)
{
    return 4u + rank;
}
inline uint32_t ArrayViewOffsetCell(uint8_t rank)
{
    return 4u + 2u * rank;
}
inline uint32_t ArrayViewTargetCell(uint8_t rank)
{
    return 5u + 2u * rank;
}
inline uint32_t ArrayViewRecordCells(uint8_t rank)
{
    return 6u + 2u * rank;
}
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
//Element i of an owned array record lives at
//ArrayHeaderCells(rank) + i*cells (the padded payload base).
inline int ArrayElemCells(uint8_t elemRtk)
{
    int pi = ScalarPrimIndexOfRtk(elemRtk);
    return (pi >= 0 && kScalarPrims[pi].slotWidth > 4) ? 2 : 1;
}

//v2.15 (v0.13.1): one generic-instantiation descriptor row. `spelling`
//is the canonical no-space form ("Box<int>", see GenericInstDescriptor);
//`composition` is empty for a static entry (the spelling is the whole fact)
//or the runtime-composition bytecode for an entry whose arguments
//contain type parameters — a tag stream, little-endian u16 lengths:
//   0 TEMPLATE_HEAD  u16 nameLen + bytes (template unqualified name)
//   1 ARG_CONST      u16 spellingLen + bytes (complete leaf form)
//   2 ARG_THIS       u8 paramIdx (receiver descriptor's argument index)
//   3 ARG_ARRAY      u8 rank + one child ARG encoding (the element)
//   4 ARG_NESTED     u16 compositionLen + child composition record bytes (a full
//                    TEMPLATE_HEAD-led stream; length-prefixed so the
//                    parent's argument loop resumes after the child)
//Rows are self-describing by leading tag: an instantiation-path row
//(OP_New, typeName(Box<T>)) leads with TEMPLATE_HEAD; a typeName row for
//a non-instantiation operand with a type parameter (T[]) is a single ARG
//encoding — tag 1..4, never 0, so the two forms cannot alias; identical
//rows merge. A composition row's spelling is empty; the runtime composes
//and interns the canonical spelling per execution — spelling equality IS
//descriptor equality, so consumers resolve a row by interning it.
struct DescriptorEntry {
    std::string spelling;
    std::vector<uint8_t> composition;
};

struct CompiledModule {
    //v2.0: the module's dotted path (the compile unit's package identity).
    std::string modulePath;
    std::string name;
    std::vector<CompiledFunction> functions;
    std::vector<std::string> stringConstants;
    //v2.15 (v0.13.1): generic-instantiation descriptor rows, written
    //between the string-constant and function sections. Bytecode
    //addresses rows by index (OP_New's descIdx operand).
    std::vector<DescriptorEntry> descriptorTable;
    std::vector<CompiledStruct> structs;
    std::vector<CompiledClass> classes;
    //0.9.10 (v2.4): interface statics, keyed like enumKeys (qualified).
    std::vector<CompiledInterface> interfaces;
    std::vector<CompiledArrayType> arrayTypes;
    //Phase 8e-9b: per-enum value name tables (outer = enumDefIdx, inner =
    //int value); OP_Enum_to_str renders `Color.Red.toString()` → "Red".
    std::vector<std::vector<std::string>> enumNames;
    //v2.0 phase 6: qualified key per enum entry, parallel to enumNames
    //(same count; placeholder slots carry the target's key with empty
    //names). nlink name-addresses enum import slots and dedups enums
    //by key — enumNames alone carries no type identity.
    std::vector<std::string> enumKeys;
    //v2.9 (0.10.14): per-enum member-method tables, parallel to
    //enumNames/enumKeys (all sites keep the vectors in lockstep;
    //placeholder slots carry empty vectors). enumMethodIndices[i] = the
    //enum's method functions' indices into the producer's own table,
    //declaration order; enumMethodAccess[i] = the per-method byte (FA_*).
    std::vector<std::vector<uint16_t>> enumMethodIndices;
    std::vector<std::vector<uint8_t>> enumMethodAccess;
    //v2.20 (0.13.8 item 5): the per-enum declared access byte (FA_*),
    //parallel to enumNames/enumKeys like the method tables — the enum
    //TYPE's visibility, which the consumer stub rebuilds the declaration
    //from (the method tables above carry the MEMBERS' access).
    std::vector<uint8_t> enumAccess;
    //v2.13 (v0.12.6): static slot table — one entry per runtime-addressed
    //static (mutable class statics, runtime-const fields, static locals)
    //in allocation order; bytecode addresses slots by index.
    std::vector<CompiledStaticSlot> staticSlots;

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
        std::string name;         //qualified key; unqualified name when ownerClassKey is set
        uint32_t paramCount = 0;  //functions only
        //Functions only (empty for the type tables): "" = namespace-level
        //function (name is its qualified key); non-empty = a method or
        //constructor of that class's qualified key (unqualified name, the
        //class's name for a constructor). Table keys stay bare — the owner
        //key lives in the slot record only (phase6 design section 2).
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
    //v2.13 (v0.12.6): cross-unit static-slot placeholder tail. ownerKind
    //here is table-local — 0 = class owner, 1 = interface owner —
    //unrelated to functionImports' encoding (never dispatched that way).
    std::vector<SymbolImport> staticSlotImports; //slots after own staticSlots

    //Runtime only (never serialized): nlink sets it after linking — the
    //global index of this module's main() (or the tool layer's entry).
    int32_t entryPoint = -1;
    //Runtime only (never serialized): every linked unit's modulePath —
    //admits descriptor heads at intern (linked images keep no imports).
    std::vector<std::string> mergedUnitPackagePaths;

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

    //Bare input: the D8 UNIQUE ".name" suffix scan (ambiguous -> -1).
    //Dotted input: only a generic-template spelling can miss exact —
    //strip the package, FindClass first-match (rows key bare BaseName).
    int FindClassBareOrUniqueSuffix(const std::string& name) const {
        if (int idx = FindClass(name); idx >= 0)
            return idx;
        if (name.find('.') != std::string::npos)   //erasure arm
            return FindClass(name.substr(name.rfind('.') + 1));
        const std::string suffix = "." + name;
        int found = -1;
        for (size_t i = 0; i < classes.size(); ++i)
        {
            if (classes[i].name.size() > suffix.size()
                && classes[i].name.compare(
                       classes[i].name.size() - suffix.size(),
                       suffix.size(), suffix) == 0)
            {
                if (found >= 0)
                    return -1;   //ambiguous: two packages declare it
                found = static_cast<int>(i);
            }
        }
        return found;
    }

    int FindEnum(const std::string& enumKey) const {
        for (int i = 0; i < static_cast<int>(enumKeys.size()); ++i)
            if (enumKeys[i] == enumKey)
                return i;
        return -1;
    }

    //Interface rows key the KeyOf spellings exactly (no erasure model,
    //no dotted-strip bridge — both producer faces spell the same name).
    int FindInterface(const std::string& ifaceName) const {
        for (int i = 0; i < static_cast<int>(interfaces.size()); ++i)
            if (interfaces[i].name == ifaceName)
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
