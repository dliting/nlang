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
    void RegisterStructs(SnNamespace& root);
    void RegisterClasses(SnNamespace& root);
    void ResolveStructClassRefs();
    void RegisterFunctions(SnNamespace& root);
    void PopulateClassMethods(SnNamespace& root);
    void GenerateAllBytecode(SnNamespace& root);

    static uint8_t RuntimeTypeKind(SnField* pType);
    uint16_t AllocLocal(const std::string& name, uint16_t size,
                        uint8_t typeKind, bool isParam);
    uint16_t FindLocal(const std::string& name) const;
    uint16_t AddStringConstant(const std::string& s);

    CompiledModule m_compiledModule;
    std::unordered_map<SnFunction*, size_t> m_funcIndexMap;
    std::vector<std::vector<std::string>> m_structFieldTypeNames;

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
    //  [params...] [returnSlot] [tempSlot] [tempSlot2] [callParamBase(8 slots)] [user locals...]
    struct FuncContext {
        CompiledFunction* func = nullptr;
        std::unordered_map<std::string, uint16_t> localOffsets;  //name -> frame offset
        uint16_t nextOffset = 0;     //next free offset in the frame
        uint16_t tempSlot = 0;       //temp for binary op right operand / condition eval
        uint16_t tempSlot2 = 0;      //second temp for nested binary expressions
        uint16_t returnSlot = 0;     //slot for function return value
        uint16_t callParamBase = 0;  //base of 8-slot area for call arguments
    };
    FuncContext* m_currFunc = nullptr;
};

} // namespace nlang
