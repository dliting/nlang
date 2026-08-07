#pragma once
#include <nlang/compiler/ICodeBackend.h>
#include "CompiledModule.h"
#include "BytecodeEmitter.h"
#include <unordered_map>

namespace nlang {

class SnExpression;
class SnField;
class SnStatement;
class SnFunction;

class VmBackend : public ICodeBackend {
public:
    VmBackend();
    ~VmBackend() override;

    void OnModuleCreate(Module& module) override;
    void GenerateTypes(SnNamespace& root) override;
    void GenerateData(SnNamespace& root) override;
    void GenerateStatements(SnNamespace& root) override;
    bool SaveModule(BuildEnvironment& env) override;

    const CompiledModule& Result() const { return m_compiledModule; }

private:
    void GenerateFunction(SnFunction& func, size_t funcIdx);
    void EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                        uint16_t resultOffset);
    void EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter);

    //Compilation phases (called by GenerateStatements in order).
    //Each phase corresponds to a distinct compilation pass over the AST.
    //Future evolution: each phase can become an Accessor for multi-backend support.
    void RegisterBuiltinClasses();
    void RegisterStructs(SnNamespace& root);
    void RegisterClasses(SnNamespace& root);
    void ResolveStructClassRefs();
    void RegisterArrayTypes(SnNamespace& root);
    void RegisterFunctions(SnNamespace& root);
    void PopulateClassMethods(SnNamespace& root);
    void GenerateAllBytecode(SnNamespace& root);

    //Register an array type from its element type field.
    //Returns the arrayTypeIdx in m_compiledModule.arrayTypes.
    uint16_t RegisterArrayType(SnField* pElemType);

    static uint8_t RuntimeTypeKind(SnField* pType);
    //Phase 8e-4: RTK_* tag for boxing a primitive-T argument, plus an
    //isPrimitive flag (needed because RTK_Int32 == 0 — same collision as
    //the Phase 8e-3 C1 fix). Shared by List<T> and Dict<K,V> codegen.
    struct BoxingTagResult { uint8_t tag; bool isPrimitive; };
    static BoxingTagResult BoxingTagFor(SnField* pT);
    uint16_t AllocLocal(const std::string& name, uint16_t size,
                        uint8_t typeKind, bool isParam);
    uint16_t FindLocal(const std::string& name) const;
    uint16_t AddStringConstant(const std::string& s);

    //Pick a temp slot distinct from `exclude` so a sub-expression can use it
    //without clobbering `exclude`. With only two temp slots available, the
    //rule is: if exclude == tempSlot, return tempSlot2; otherwise return
    //tempSlot. This composes for nested expressions because each level
    //alternates between tempSlot and tempSlot2.
    //Used by: BinaryExpr (right operand), SubscriptExpr (index), and any
    //other expression that needs one extra slot besides its resultOffset.
    uint16_t PickTempSlot(uint16_t exclude) const;

    CompiledModule m_compiledModule;
    std::unordered_map<SnFunction*, size_t> m_funcIndexMap;
    std::vector<std::vector<std::string>> m_structFieldTypeNames;
    int16_t m_objectClassIdx = -1;  //Phase 8e-1: index of synthesized Object class (-1 until RegisterBuiltinClasses)
    int16_t m_listClassIdx = -1;    //Phase 8e-3: index of List<T> built-in class (-1 until RegisterBuiltinClasses)
    int16_t m_dictClassIdx = -1;    //Phase 8e-4: index of Dict<K,V> built-in class (-1 until RegisterBuiltinClasses)

    //Per-loop code generation context.
    //Reference: EN's Compiler::NestBreaks/NestContinues (Compiler.h:108-111).
    struct LoopContext {
        std::vector<size_t> breakJumps;
        std::vector<size_t> continueJumps;
        bool isSwitch = false;  //true for switch contexts, false for loops
    };
    std::vector<LoopContext> m_loopStack;

    //Per-function code generation context.
    //Layout of the local variable frame (all slots are VALUE_SIZE=4 bytes):
    //  [params...] [returnSlot] [tempSlot..tempSlot4] [callParamBase(8 slots)] [user locals...]
    //The 4-slot temp pool supports PickTempSlot's depth-chaining for nested
    //binary expressions (e.g. `a == b*c + d` needs 3 distinct slots: outer-left,
    //inner-left-result, inner-right). Pre-8e-1.5 had only 2 slots which caused
    //the outer-left to be clobbered by inner-right intermediates.
    struct FuncContext {
        CompiledFunction* func = nullptr;
        std::unordered_map<std::string, uint16_t> localOffsets;  //name -> frame offset
        uint16_t nextOffset = 0;     //next free offset in the frame
        uint16_t tempSlot = 0;       //temp pool slot 0 (binary op left / condition eval)
        uint16_t tempSlot2 = 0;      //temp pool slot 1
        uint16_t tempSlot3 = 0;      //temp pool slot 2
        uint16_t tempSlot4 = 0;      //temp pool slot 3
        uint16_t returnSlot = 0;     //slot for function return value
        uint16_t callParamBase = 0;  //base of 8-slot area for call arguments
        //Phase 8e-5: per-function counter for foreach hidden-local uniquification.
        //AllocLocal dedupes by name (VmBackend.cpp:2194); without uniquification,
        //nested foreach loops would collide on __foreach_iter / __foreach_i / __foreach_n.
        uint16_t foreachCounter = 0;
    };
    FuncContext* m_currFunc = nullptr;
};

} // namespace nlang
