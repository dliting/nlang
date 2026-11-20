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
//v0.12.3: declared kind of a type-parameter (T) slot — the template row's
//one-implementation layout. A T slot holds one value slot whatever the
//instantiation binds, and its runtime representation depends on the type
//argument (string store handle / heap index / inline scalar), so every
//consumer routes it conservatively (GC tests both domains). First kind
//above the scalar block (10..19), keeping the two ranges disjoint.
static constexpr uint8_t RTK_TypeParam = 20;
//v0.12.7: runtime-only kind of an array VIEW record (spec §4.2 of the
//array/tensor design). Never a declared type kind — it never appears on
//the wire or in locals tables; it labels heap records whose header
//prefix mirrors RTK_Array (kind/typeIdx/rank/elemCount/dims at the same
//offsets) and whose tail carries strides/offset/target. The target cell
//always names an OWNED RTK_Array record (depth one).
static constexpr uint8_t RTK_ArrayView = 21;
//v0.13.9 phase ③: declared kind of a byref slot (an &-bound local,
//parameter or return slot). The slot holds the pair (base heap-record
//handle, cell offset) — design doc §3.2 — so the base handle is always a
//heap-domain record (struct/boxed/array) and the GC classifies the slot
//as ESC_HeapIndex (single domain, like a struct slot's low half). Byrefs
//are never stored: no field, element, container entry or static slot ever
//declares this kind — it exists only on frame slot descriptors (and the
//return-kind cell), which is why MarkFrameLocals/MarkFrameBand style
//consumers route it and record-side consumers never see it.
static constexpr uint8_t RTK_Byref = 22;
//v0.13.9 phase ③ Task 11: declared kind of a PROMOTED slot (a scalar or
//string local/param whose storage became a stable boxed record because a
//& binding aliases it). Only the slot's low half is meaningful — the box
//handle; the upper half is stale bytes the prologue's Assign left (every
//consumer reads the low half). The GC routes it exactly like RTK_Byref
//(ESC_HeapIndex, over-mark before the prologue rewrites a raw argument),
//and like RTK_Byref it never appears on fields, elements, container
//entries, static slots or return kinds. The debug display is the reason
//the two kinds split: a promoted slot renders its plain value, a byref
//slot renders the base->cell->value pair (ndb, spec §4). Kind-value
//extension of the locals-table byte — rides the v2.21 floor, no layout
//change.
static constexpr uint8_t RTK_Promoted = 23;
//v0.13.9 phase ③ Task 11: declared kind of a CELL-addressed byref slot
//(`int&`/`string&`/enum-`&` bindings — a reference whose declared element
//type is a scalar, string or enum). The pair always names a payload CELL
//(box payload, struct/class field cell, array element cell), never a
//whole record, so the debug display resolves the element kind from the
//base family's own metadata even at cell 0 — the aggregate family
//(RTK_Byref) renders an offset-0 pair as the whole record per the D9
//convention, which is the wrong read for a first-field scalar &. Same
//GC routing as RTK_Byref (ESC_HeapIndex on the pair's base handle).
//Kind-value extension of the locals-table byte — rides the v2.21 floor,
//no layout change.
static constexpr uint8_t RTK_ByrefCell = 24;
//v0.13.9 phase ③ Task 12: the IDENTITY-tier kind of an interface type —
//TypeDesc channels (static-slot/param/return descriptors, field
//descriptors) and arrayTypes element rows. It names WHICH interface the
//declaration carries: the type-index/elem-index cell holds the
//interfaces-table row, so cast rows and linker merges keep interface
//identity (a shared sentinel row would let `I[] as J[]` pass; the old
//scalar degrade also collided with int[] rows). The STORAGE tier is a
//different question with a different answer: a slot or field typed by an
//interface HOLDS a class handle, and RuntimeTypeKind files it RTK_Class —
//RTK_Interface never appears in locals-table bytes, fieldTypeKinds,
//signature leaves or return kinds. GC routing: a value under this kind
//is a heap reference exactly like RTK_Class (MarkStaticSlotRoots's
//reference arm; ArrayElemKindAccepts accepts runtime RTK_Class records).
static constexpr uint8_t RTK_Interface = 25;
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
    //v2.12 (v0.12.4 D10): declared const-struct marker — one wire byte
    //after fieldCount. The consumer stub rebuilds SetConstAggregate()
    //from it, so a lost byte silently unseals imported const structs.
    bool isConstAggregate = false;
    //v2.20 (0.13.8 item 5): the STRUCT's declared access (FA_*), the
    //record's trailing wire byte — the consumer stub rebuilds the type
    //declaration's visibility from it (the member bytes below carry the
    //members'). FA_Public default keeps records created outside the AST
    //capture pass (intrinsics, test forges) valid.
    uint8_t access = FA_Public;
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
    //struct constructors are registered unqualified-name members, so the
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
    //bytecode — OP_CallStatic dispatches by name through the host's
    //native table (VmExecutor::RegisterNative). Body-less by contract.
    bool isNative = false;
    //0.13.1c (v2.18): pure-virtual declaration (`virtual int f(...);`
    //without a body). Signature-only record with empty bytecode —
    //dispatch must find an override; FindMethodByName raises when a
    //lookup lands here (unreachable through the compiler's own output:
    //the instantiation gate refuses abstract classes, so a dispatch
    //winner is always concrete — the flag guards artifacts produced
    //outside compiler invariants). The consumer stub rebuild marks the
    //member NF_Abstract from it, keeping Task 5's on-demand
    //IsAbstractClass()/CollectAbstractObligations working across the
    //import boundary.
    bool isAbstract = false;
    //0.9.10 (v2.4): static member. Static frames carry no __this slot,
    //and name-based instance dispatch (FindMethodByName) must skip
    //these records — static members are a separate namespace.
    bool isStatic = false;
    //v2.16 (ABI v12): instance method — a class/struct/enum member that
    //is not static (the compiler's HasThisReceiver verdict). The frame
    //carries a slot-0 __this receiver, and the native call bridge reads
    //the receiver's heap handle from the call's slot 0 when this is set
    //— a flag, not a name-shape guess, so the method-key convention can
    //evolve without silently disabling receiver reads.
    bool isInstanceMethod = false;
    //0.13.1a (v2.17): generic record — a member of a generic class row
    //or a generic free function. Generic frames carry the instantiation
    //descriptor cell filled at push (receiver head, caller forward, or
    //call-site descriptor row); T operations read it. Non-generic
    //records leave the cell null and pay nothing.
    bool isGeneric = false;
    //v2.23 (0.13.10 Task 5): lambda record — the function was written as
    //a lambda expression (no declared name); toString rendering and the
    //debugger pick the synthetic name from this instead of guessing by
    //the name shape. A flag, not a name-shape guess, so the synthetic
    //naming convention can evolve without silently changing rendering.
    bool isLambda = false;
    //v2.17: declared type-parameter names (generic records only). The
    //stub rebuild matches these to bind placeholder leaves in
    //signatures, mirroring CompiledClass::typeParamNames.
    std::vector<std::string> typeParamNames;
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
    //Intrinsics/builtins carry none (their records are created without
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
    //v2.20 (0.13.8 item 5): the function's declared access (FA_*), the
    //record's trailing wire byte. For FREE (namespace-level) functions
    //this is the only access carrier on the wire; for members the owner
    //row's per-member access tables already exist (the class
    //methodAccess/constructorAccess, the struct ctor table, the enum
    //method table) and this byte mirrors them. FA_Public default keeps
    //records created outside the AST capture pass (intrinsics, test
    //forges) valid.
    uint8_t access = FA_Public;
};

struct CompiledModule;

//v2.13 (v0.12.6): one runtime-addressed static — mutable class statics,
//runtime-const fields, static locals. Compile-time-folded constants
//occupy no slot (they never materialize at runtime).
struct CompiledStaticSlot {
    std::string name;          //declared name; static locals append "#<n>" to disambiguate same-named sites in one function
    std::string ownerKey;      //class/interface qualified key; owning function key for static locals
    uint8_t ownerKind = 0;     //0 = class, 1 = interface, 2 = static local
    uint8_t isRuntimeConst = 0;
    uint8_t hasInitializer = 0;   //drives D12 init-bit participation
    TypeDesc type;
};

//v2.13 (v0.12.6): per-class declared static field, textual order. The
//consumer stub rebuilds it for cross-module access checks; slotIdx ties
//the field to its module slot (0xFFFF = compile-time-folded constant —
//foldedValue carries it and no slot exists).
struct CompiledStaticField {
    std::string name;
    TypeDesc type;
    uint8_t access = 0;        //FA_*, mirrors CompiledClass::fieldAccess
    bool isConst = false;
    uint16_t slotIdx = 0xFFFF; //0xFFFF = compile-time-folded const (foldedValue); u16 matches the operand width that addresses slots
    DefaultValueDesc foldedValue;
};

struct CompiledClass {
    std::string name;
    uint16_t fieldCount = 0;
    int16_t superClassIdx = -1;
    //v2.12 (v0.12.4 D10): declared const-class marker (own bit — the
    //chain is derivable from superClassIdx). One wire byte after
    //superClassIdx; the consumer stub rebuilds SetConstAggregate() from
    //it, keeping the store gate and copy-on-read seal working against
    //imported const classes.
    bool isConstAggregate = false;
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
    //v2.13 (v0.12.6): declared static fields in textual order — the
    //consumer stub rebuilds the static surface from them; slotIdx ties
    //each to the module's staticSlots table.
    std::vector<CompiledStaticField> staticFields;
    //v2.13 (v0.12.6): the class's lazy static-initializer function
    //(0xFFFF = none; u16 = function-index operand width).
    uint16_t staticInitFuncIdx = 0xFFFF;
    //v0.13.1 (D7): the template marker and the declared type-parameter
    //names — a generic declaration registers the one erased row, and
    //this tail is what the consumer stub rebuilds a generic DECLARATION
    //from, so a sourceless package can still be instantiated (the
    //dictionary model's consumer-side shape). Non-template rows carry
    //the defaults.
    bool isTemplate = false;
    std::vector<std::string> typeParamNames;
    //v2.20 (0.13.8 item 5): the CLASS's declared access (FA_*), the
    //record's trailing wire byte — the consumer stub rebuilds the type
    //declaration's visibility from it (the member tables above carry the
    //members'). FA_Public default keeps records created outside the AST
    //capture pass (intrinsics, test forges) valid.
    uint8_t access = FA_Public;
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
    //v2.13 (v0.12.6): the constant's module slot when it is a
    //runtime-initialized static const (D11's non-foldable arm); 0xFFFF =
    //compile-time-folded (value carries it, no slot).
    uint16_t slotIdx = 0xFFFF;
};

struct CompiledInterface {
    std::string name;
    std::vector<uint16_t> staticMethodIndices;
    std::vector<CompiledInterfaceConstant> constants;
    //0.9.12 (v2.7): instance-method signatures in declaration order.
    std::vector<CompiledInterfaceMethod> instanceMethods;
    //v2.13 (v0.12.6): the interface's lazy static-initializer function
    //(0xFFFF = none).
    uint16_t staticInitFuncIdx = 0xFFFF;
    //v2.19 (0.13.1c Task 8): the generic-declaration tail, mirroring
    //CompiledClass::isTemplate/typeParamNames — a generic interface
    //registers the one erased template row (its statics compile against
    //T; call sites ride the descriptor), and this tail is what a
    //consumer stub rebuilds a generic DECLARATION from so sourceless
    //packages stay instantiable. Non-template rows carry the defaults.
    bool isTemplate = false;
    std::vector<std::string> typeParamNames;
    //v2.20 (0.13.8 item 5): the INTERFACE's declared access (FA_*), the
    //record's trailing wire byte. Members are implicitly public by the
    //declaration contract (see CompiledInterfaceMethod), so this byte
    //gates only the type name itself. FA_Public default keeps records
    //created outside the AST capture pass (intrinsics, test forges)
    //valid.
    uint8_t access = FA_Public;
};

} // namespace nlang
