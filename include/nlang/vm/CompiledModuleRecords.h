/*---
    CompiledModuleRecords.h — per-table record types of the .ncu image
    (public header). Split out of CompiledModule.h (0.9.12 source-size
    maintainability refactor, zero behavior change): this header carries
    the runtime type-kind constants and the record structs the container
    vectors hold. The container itself (CompiledModule), the format
    version and the slot-width ABI stay in CompiledModule.h, which
    includes this one — consumers keep including CompiledModule.h alone.
---*/
#pragma once
#include "Intrinsics.h"
#include "TypeDesc.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Runtime type kind constants for serialization (compile-time NK_* values
//exceed uint8_t range). The scalar codes (RTK_Int32/RTK_Float = 0/1 plus
//RTK_Byte..RTK_Char = 10..19) live in PrimitiveTypes.h — single source.
static constexpr uint8_t RTK_String = 2;
static constexpr uint8_t RTK_Struct = 3;
static constexpr uint8_t RTK_Class  = 4;
static constexpr uint8_t RTK_Array  = 5;
static constexpr uint8_t RTK_Boxed  = 6;  //boxed primitive (slot[0]=tag, slots[1..2]=value; 8-byte family uses both cells)
static constexpr uint8_t RTK_Func   = 7;  //Phase 13: function handle (slot[0]=target, slot[1]=this, slot[2]=form; 0=static, 1=virtual)
//Descriptor-only kinds RTK_List / RTK_Dict / RTK_NonSerialized live in
//TypeDesc.h (they appear inside TypeDesc, never in these legacy bytes).
static constexpr uint8_t RTK_Null   = 0xFD;  //Option B: null default value (any reference type)
static constexpr uint8_t RTK_Unfoldable = 0xFC;  //Option B: had default but not constant-foldable
static constexpr uint8_t RTK_Expr   = 0xFB;  //v2.3: default is the ExprDesc tree in exprBytes
static constexpr uint8_t RTK_Void   = 0xFE;  //used for ctor/void method stubs

struct LocalDescriptor {
    uint16_t offset = 0;
    uint16_t size = 0;
    uint8_t  isParam = 0;
    uint8_t  typeKind = 0;
    //Debugger scope visibility (v1.14): bytecode offset where the local
    //joins its function's flat frame. 0 = live from entry (params and
    //the implicit this receiver). The debug views hide a local while the
    //paused statement PC (frame.currentPc) is below this value; the GC
    //root scan deliberately ignores it — slots are zero-initialized at
    //every call, so tracing the whole table from entry is safe.
    uint16_t declPc = 0;
    std::string name;
};

//Option B: per-formal default-value descriptor for cross-module import.
//Tag determines which payload field is meaningful:
//  RTK_Null   — null literal for any reference type (class/string/array). No payload.
//  RTK_Int32  — intValue holds the int32 default.
//  RTK_Float  — floatValue holds the float default.
//  RTK_String — stringIdx is an index into the PRODUCER module's stringConstants.
//               The consumer loader remaps this into its own stringConstants.
//  RTK_Long   — longValue holds the long default (0.7.5).
//  RTK_ULong  — longValue holds the ulong default's bit pattern (0.7.5).
//  RTK_Double — doubleValue holds the double default (0.7.5; payloads
//               exist ahead of double literals landing in Task 7).
//  RTK_Void   — sentinel: this formal has no default. Used to keep the vector
//               dense (always == paramCount entries; entries without defaults
//               carry RTK_Void so positional alignment is preserved).
//
//Constant-foldable negative int literals (`-5`) are folded at write time into
//a single RTK_Int32 with negative intValue — no separate tag needed.
//
//v2.3: defaults outside the constant-foldable subset ride RTK_Expr — the
//wire carries a serialized ExprDesc tree in exprBytes (u16 length prefix;
//grammar and reader in ExprDesc.h). The consumer rebuilds an unresolved
//AST from it and lets
//the normal declaration-side resolution pass bind it (formal references,
//module-qualified calls). RTK_Unfoldable remains the reject stamp for
//expressions outside the ExprDesc subset (casts, …).
//
//0.9.11 D4: struct field defaults (v2.6) ride the same descriptor — the
//declaration-side check admits only the constant-foldable subset, so
//field entries never carry RTK_Expr/RTK_Unfoldable tags.
struct DefaultValueDesc {
    uint8_t  tag = RTK_Void;
    uint32_t intValue = 0;     // RTK_Int32 / RTK_Bool / RTK_Char
    float    floatValue = 0.0f;// RTK_Float
    uint32_t stringIdx = 0;    // RTK_String (producer-side index)
    //0.7.5: 8-byte scalar channels (RTK_Long/RTK_ULong/RTK_Double).
    int64_t  longValue = 0;
    double   doubleValue = 0.0;
    //v2.3: non-empty only when tag == RTK_Expr. ED_Literal_String arms
    //hold PRODUCER-side stringConstants indices — the import merge
    //remaps them (NcuLinker::FinalizeFunction, like stringIdx above).
    std::vector<uint8_t> exprBytes;

    bool hasDefault() const { return tag != RTK_Void; }
};

struct CompiledStruct {
    std::string name;
    uint16_t fieldCount = 0;
    std::vector<std::string> fieldNames;
    std::vector<uint16_t> fieldTypeKinds;       // RTK_* per field
    std::vector<uint16_t> fieldStructIndices;    // struct index for struct-typed fields, 0xFFFF for non-struct
    std::vector<uint16_t> fieldClassIndices;     // class index for class-typed fields, 0xFFFF for non-class
    //v1.12: per-field recursive type descriptors (empty entries = not
    //expressible). Survive the import merge (indices remapped). 0.10.14:
    //the consumer struct stub reads these back to rebuild field members
    //(CreateStructStub, mirroring the class stub); they also let a
    //consumer re-saving the module emit correct descriptors.
    std::vector<TypeDesc> fieldTypeDescs;
    //0.9.11 (v2.6): constructor function indices in declaration order —
    //struct constructors are registered bare-name members, so the
    //ordinal within this vector is the only stable cross-module selector
    //(SymbolImport::ownerOrdinal). Empty = the struct has no constructor.
    std::vector<uint16_t> constructorIndices;
    //0.9.12 (v2.7): FA_* per constructor, parallel to constructorIndices
    //(size equality enforced by the loader). A consumer stub rebuilds
    //each constructor with its declared access so cross-module calls see
    //the same visibility as own-module ones.
    std::vector<uint8_t> constructorAccess;
    //0.9.11 D4: per-field compile-time defaults (v2.6), one entry per
    //field in declaration order (RTK_Void = no default, keeping the
    //vector dense and positionally aligned). Applied at every
    //AllocStructOnHeap; RTK_String entries carry producer-pool indices
    //that the import merge remaps.
    std::vector<DefaultValueDesc> fieldDefaults;
};

struct CompiledArrayType {
    uint8_t  elemKind = 0;       // RTK_Int32/RTK_Float/RTK_String/RTK_Struct/RTK_Class
    uint16_t elemTypeIdx = 0xFFFF; // struct/class index (0xFFFF for primitives)
    uint8_t  rank = 1;           // stage-2: 1 for T[], 2 for T[,], ...
};

//Phase 9d: try/catch descriptor. One entry per catch clause. At throw time,
//VmExecutor linearly scans func.tryBlocks for the first entry where
//startPc <= opPc < endPc and (exceptionClassIdx == 0xFFFF or the thrown
//class is instance-of-target); on match, control jumps to handlerPc with
//the exception heap idx bound to locals[catchLocalOff].
//  exceptionClassIdx: 0xFFFF = unused/invalid (defensive default)
//                      any other value = CompiledClass index of catch type
//                      (Exception base class catches all subclasses via
//                      IsInstanceOrSubclass walk).
struct TryBlock {
    uint16_t startPc = 0;
    uint16_t endPc = 0;
    uint16_t handlerPc = 0;
    uint16_t exceptionClassIdx = 0xFFFF;
    uint16_t catchLocalOff = 0;
};

struct CompiledFunction {
    std::string name;
    std::vector<uint8_t> bytecode;
    std::vector<LocalDescriptor> locals;
    uint16_t localsSize = 0;
    uint16_t paramCount = 0;
    uint16_t returnTypeKind = 0;
    uint16_t intrinsicId = INTR_None;    //INTR_None = normal bytecode, else VM intrinsic
    //Phase 9f: native function declaration (`native int f(...);`). No
    //bytecode — OP_CallFunc dispatches by name through the host's
    //native table (VmExecutor::RegisterNative). Body-less by contract.
    bool isNative = false;
    //0.9.10 (v2.4): static member. Static frames carry no __this slot,
    //and name-based instance dispatch (FindMethodByName) must skip
    //these records — static members are a separate namespace.
    bool isStatic = false;
    //Option B: per-formal default values. Free functions: size ==
    //paramCount. Methods and constructors: paramCount counts the
    //implicit __this slot (locals[0], isParam), so size == paramCount-1
    //— one entry per written formal, never for 'this'. Formals without
    //defaults carry tag=RTK_Void.
    std::vector<DefaultValueDesc> defaultValues;
    //Phase 9d: try/catch table. Empty for functions without try blocks.
    std::vector<TryBlock> tryBlocks;
    //v1.9 (debugger): source file path this function was compiled from,
    //as recorded by the TU (empty for intrinsics/unknown). Survives the
    //import merge so breakpoints can address imported functions.
    std::string sourceFile;
    //v1.12: true formal types (one per AST formal — methods exclude the
    //implicit this slot, mirroring defaultValues' sizing) and the return
    //type. Empty / NonSerialized entries are "not expressible in the
    //descriptor grammar"; stub reconstruction degrades to the int32
    //placeholder for those, while embed call marshalling defers to the
    //frame-layout locals table (CallMarshalling.h DeclaredParamKind).
    //Intrinsics/builtins carry none (their records are minted without
    //the AST capture pass and never become stubs).
    std::vector<ParamTypeDesc> paramTypeDescs;
    TypeDesc returnTypeDesc;
    //K2 slice 5 (v2.8): per-slot root classes for the frame band (see
    //EvalSlotClass). Built by the backend at function finalize: the
    //fixed scratch region (temps + call staging) is Dual, named slots
    //and the return slot take their declared kinds, evalArea slots
    //start None and widen by OR-merged claim classes. Serialized as
    //the function record's trailing field. Empty when the band is
    //empty (native/builtin bandless records); a length that disagrees
    //with localsSize/paramCount makes the GC fall back to the
    //conservative band scan.
    std::vector<uint8_t> evalAreaHandleMask;
};

struct CompiledModule;

struct CompiledClass {
    std::string name;
    uint16_t fieldCount = 0;
    int16_t superClassIdx = -1;
    std::vector<std::string> fieldNames;
    std::vector<uint16_t> fieldTypeKinds;       // RTK_* per field
    std::vector<uint16_t> fieldStructIndices;    // struct index for struct-typed fields, 0xFFFF for non-struct
    std::vector<uint16_t> fieldClassIndices;     // class index for class-typed fields, 0xFFFF for non-class
    std::vector<uint8_t>  fieldAccess;           // FA_* per field
    //0.9.12 (v2.7): OWN-ONLY instance fields — fieldNames/fieldTypeKinds/
    //.../fieldTypeDescs list just the fields this class declares; the
    //full instance layout is derived at runtime by walking superClassIdx
    //base-first (VmClassLayout). A v2.6 image carries the inherited
    //fields too and would double-count them; hence the semantic floor.
    //v1.12: per-field recursive type descriptors (empty entries = not
    //expressible). Survive the import merge (indices remapped). Wire
    //fidelity only — like the struct sibling, no stub-side reader. Built-in
    //classes carry none (no AST capture; no consumer).
    std::vector<TypeDesc> fieldTypeDescs;
    //0.9.12 (v2.7): every member function this class declares — static
    //and instance, in declaration order. The public-static index block
    //of v2.4 is gone: statics ride this table flagged by the parallel
    //access bytes, so a consumer stub rebuilds the complete member
    //surface (instance methods included).
    std::vector<uint16_t> methodIndices;
    //0.9.12 (v2.7): FA_* / virtual-0-1 per method, parallel to
    //methodIndices (size equalities enforced by the loader). The stub
    //rebuilder applies them (NF_Static for static entries, NF_Virtual
    //for virtual entries, declared access for visibility checks).
    std::vector<uint8_t> methodAccess;
    std::vector<uint8_t> methodVirtual;
    //0.9.11 (v2.6): constructor function indices in declaration order —
    //replaces the single constructorIdx (last-wins) record; the
    //cross-unit ownerOrdinal is the index into this vector. Empty = the
    //class has no constructor. Constructors also appear in methodIndices
    //and in the parallel access/virtual arrays (the producer registers
    //every member function once); the stub rebuilder must skip those
    //entries when rebuilding methods and rebuild them from
    //constructorIndices instead, or every constructor becomes a ghost
    //instance method on the consumer side.
    std::vector<uint16_t> constructorIndices;
    //0.9.12 (v2.7): FA_* per constructor, parallel to constructorIndices
    //(size equality enforced by the loader).
    std::vector<uint8_t> constructorAccess;
};

//0.9.12 (v2.7): one exported interface instance-method signature. The
//interface table previously carried only statics (instance members need
//a receiver the consumer could not build); with the signature on the
//wire a consumer resolves calls on imported interface-typed receivers
//and the implements contract can be checked across modules. Interface
//members are implicitly public and non-virtual (declaration contract),
//so no access/virtual bytes are carried. paramTypeDescs/returnType/
//defaultValues follow the same conventions as CompiledFunction.
struct CompiledInterfaceMethod {
    std::string name;
    uint16_t paramCount = 0;
    std::vector<ParamTypeDesc> paramTypeDescs;
    TypeDesc returnType;
    std::vector<DefaultValueDesc> defaultValues;   //size == paramCount
};

//0.9.10 (v2.4): per-unit interface table. name is the interface's
//qualified key and staticMethodIndices point at its static function
//records.
//0.9.12 (v2.7): instance-method signatures join the record — a consumer
//resolves calls on imported interface-typed receivers and checks the
//implements contract across the module border.
//0.9.10 D3: one exported interface constant. `type` is the declared
//type (scalar or string by the declaration contract); `value` is the
//folded initializer in the same RTK channel as a formal default
//(stringIdx is a producer-side pool index, remapped on merge).
struct CompiledInterfaceConstant {
    std::string name;
    TypeDesc type;
    DefaultValueDesc value;
};

struct CompiledInterface {
    std::string name;
    std::vector<uint16_t> staticMethodIndices;
    std::vector<CompiledInterfaceConstant> constants;
    //0.9.12 (v2.7): instance-method signatures in declaration order.
    std::vector<CompiledInterfaceMethod> instanceMethods;
};

} // namespace nlang
