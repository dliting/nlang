/*---
    VmBackendExprDesc.cpp — v2.3 ExprDesc encoder: the recursive wire
    encoder for complex cross-module default values, its arms and the
    wire helpers. Extracted from Types.cpp (2026-11-28 source-size
    guard, zero behavior change).
---*/
#include "VmBackend.h"
#include "EmitPrimOps.h"
#include <nlang/core/ExprDesc.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SyntaxNodeConsts.h>
#include "builder/ModuleRegistry.h"
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/RnTypes.h>
#include <nlang/runtime/StackDump.h>
#include <string>

namespace nlang {

//Little-endian append helpers for the wire grammar in CompiledModule.h.
namespace {
void AppendBytes(std::vector<uint8_t>& out, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}
void AppendU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendI64(std::vector<uint8_t>& out, int64_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendF32(std::vector<uint8_t>& out, float v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendF64(std::vector<uint8_t>& out, double v) {
    AppendBytes(out, &v, sizeof(v));
}
}  //namespace

//Literal arm of the ExprDesc encoder. Narrower than the folded-default
//surface by design: bool/char/byte literals are either folded by
//ExtractDefaultValue or outside the cross-module subset, so they return
//false here.
bool VmBackend::EncodeLiteralDesc(SnLiteralExpr& lit,
                                  std::vector<uint8_t>& out) {
    if (lit.ContainFlags(NF_NullLiteral)) {
        AppendU8(out, RTK_Null);
        return true;
    }
    auto* litType = lit.Value().Type();
    if (litType == RnInt32::Instance()) {
        AppendU8(out, RTK_Int32);
        AppendU32(out, static_cast<uint32_t>(lit.Value().Data().m_Int));
        return true;
    }
    if (litType == RnFloat::Instance()) {
        AppendU8(out, RTK_Float);
        AppendF32(out, lit.Value().Data().m_Float);
        return true;
    }
    if (litType == RnDouble::Instance()) {
        AppendU8(out, RTK_Double);
        AppendF64(out, lit.Value().Data().m_Double);
        return true;
    }
    if (litType == RnLong::Instance()) {
        AppendU8(out, RTK_Long);
        AppendI64(out, lit.Value().Data().m_Long);
        return true;
    }
    if (litType == RnULong::Instance()) {
        AppendU8(out, RTK_ULong);
        AppendI64(out, static_cast<int64_t>(lit.Value().Data().m_ULong));
        return true;
    }
    if (litType == RnString::Instance()) {
        AppendU8(out, RTK_String);
        auto* sPtr = lit.Value().Data().m_String;
        AppendU32(out, AddStringConstant(sPtr ? *sPtr : std::string()));
        return true;
    }
    return false;
}

//Arithmetic arm of the ExprDesc encoder: unary negation and the five
//arithmetic binaries (see CompiledModule.h → ExprDesc.h for the grammar).
bool VmBackend::EncodeBinaryDesc(SnBinaryExpr& bin,
                                 const std::vector<SnFormalParam*>& formals,
                                 std::vector<uint8_t>& out, bool allowCall,
                                 int depth) {
    //Unary negation / bitwise complement (unary ctor leaves Right() null).
    if (bin.Op() == SnBinaryExpr::OP_Neg && !bin.Right()) {
        AppendU8(out, ED_Neg);
        return EncodeExprDesc(*bin.Left(), formals, out, allowCall,
                              depth + 1);
    }
    if (bin.Op() == SnBinaryExpr::OP_BitNot && !bin.Right()) {
        AppendU8(out, ED_BitNot);
        return EncodeExprDesc(*bin.Left(), formals, out, allowCall,
                              depth + 1);
    }
    uint8_t opCode = 0;
    switch (bin.Op()) {
    case SnBinaryExpr::OP_Add: opCode = EDB_Add; break;
    case SnBinaryExpr::OP_Sub: opCode = EDB_Sub; break;
    case SnBinaryExpr::OP_Mul: opCode = EDB_Mul; break;
    case SnBinaryExpr::OP_Div: opCode = EDB_Div; break;
    case SnBinaryExpr::OP_Mod: opCode = EDB_Mod; break;
    case SnBinaryExpr::OP_Shl: opCode = EDB_Shl; break;
    case SnBinaryExpr::OP_Shr: opCode = EDB_Shr; break;
    case SnBinaryExpr::OP_BitAnd: opCode = EDB_BitAnd; break;
    case SnBinaryExpr::OP_BitOr: opCode = EDB_BitOr; break;
    case SnBinaryExpr::OP_BitXor: opCode = EDB_BitXor; break;
    default: return false;  //comparisons/logical ops: out of subset
    }
    AppendU8(out, ED_Binary);
    AppendU8(out, opCode);
    if (!EncodeExprDesc(*bin.Left(), formals, out, allowCall, depth + 1))
        return false;
    return EncodeExprDesc(*bin.Right(), formals, out, allowCall,
                          depth + 1);
}

//True when key starts with "<modulePath>.": the producing-module
//authority shared by every qualified-key arm (ED_Call, ED_FuncRef,
//ED_StaticMember). Imported or prelude-owned targets fail it and keep
//the RTK_Unfoldable stamp — the consumer rebinds the rebuilt chain
//through its own import registry, which only speaks this module's
//spellings.
static bool HasModulePrefix(const std::string& key,
                            const std::string& modulePath) {
    const std::string prefix = modulePath + ".";
    return key.size() > prefix.size()
        && key.compare(0, prefix.size(), prefix) == 0;
}

//Wire chain key of a type member: "<package>.<Enclosure...>.<name>".
//KeyOf/LinkKeyOf spells class statics bare (methods dispatch by
//receiver through their unqualified name), so the dotted chain is
//assembled here from the declaration's enclosure walk; the registry
//package supplies the module prefix (the same path-derived value the
//free-function keys carry). An empty result flags an ownerless member;
//prelude/foreign members come back under their own unit's path and are
//then refused by HasModulePrefix. Non-generic interface statics
//assemble to exactly their EnclosureMethodKey spelling.
std::string VmBackend::StaticMemberKey(const SnField& member) const {
    NLANG_ASSERT(m_pRegistry && "StaticMemberKey before SetModuleRegistry");
    std::vector<std::string> enclosures;  //innermost first
    for (SyntaxNode* pEnc = member.Parent();
         pEnc && (pEnc->Kind() == NK_ClassDecl
                  || pEnc->Kind() == NK_InterfaceDecl
                  || pEnc->Kind() == NK_EnumDecl);
         pEnc = pEnc->Parent())
        enclosures.push_back(static_cast<SnField*>(pEnc)->Name());
    if (enclosures.empty()) return std::string();
    //The owner comes from the ancestor walk, never the flat owner tag
    //PackageOf reads: class and enum members carry no tag of their own,
    //so the flat lookup would come back ownerless for every own-unit
    //static and silently degrade the fold to RTK_Unfoldable (the same
    //distinction IsOwnUnit draws).
    const uint32_t owner = m_pRegistry->OwnerOfContext(member);
    if (owner == ModuleRegistry::NO_OWNER) return std::string();
    const std::string package = m_pRegistry->ModulePathOf(owner);
    if (package.empty()) return std::string();
    std::string key = package;
    for (auto it = enclosures.rbegin(); it != enclosures.rend(); ++it) {
        key += '.';
        key += *it;
    }
    key += '.';
    key += member.Name();
    return key;
}

//Member-value arm of the ExprDesc encoder: a by-name static method
//reference becomes ED_FuncRef, a static field read becomes
//ED_StaticMember; both carry the StaticMemberKey chain so the consumer
//rebuilds the dotted name and re-binds it at its own use site.
bool VmBackend::EncodeMemberRefDesc(SnField& member,
                                    std::vector<uint8_t>& out) {
    const std::string key = StaticMemberKey(member);
    if (!HasModulePrefix(key, m_compiledModule.modulePath)) return false;
    AppendU8(out, member.Kind() == NK_Function ? ED_FuncRef
                                               : ED_StaticMember);
    AppendU16(out, static_cast<uint16_t>(key.size()));
    AppendBytes(out, key.data(), key.size());
    return true;
}

//True when the member expression spells its receiver as the chain of
//enclosing type names (the static-reference spelling `Knob.tick`,
//`A.B.f`), never a value receiver: the ExprDesc wire carries the key
//alone, so a bound instance reference (`a.tick` through a formal)
//would silently lose its receiver on the wire.
static bool SpellsEnclosingChain(const SnMemberExpr& member,
                                 const SnField& field) {
    std::vector<std::string> expected;  //innermost enclosure first
    for (SyntaxNode* pEnc = field.Parent();
         pEnc && (pEnc->Kind() == NK_ClassDecl
                  || pEnc->Kind() == NK_InterfaceDecl
                  || pEnc->Kind() == NK_EnumDecl);
         pEnc = pEnc->Parent())
        expected.push_back(static_cast<SnField*>(pEnc)->Name());
    std::vector<std::string> spelled;   //innermost receiver segment first
    SnExpression* pOuter = member.Outer();
    while (pOuter->Kind() == NK_MemberExpr) {
        auto& mid = static_cast<SnMemberExpr&>(*pOuter);
        SnFieldExpr* pInner = mid.Inner();
        if (!pInner || pInner->Kind() != NK_IdentifierExpr) return false;
        spelled.push_back(static_cast<SnIdentifierExpr*>(pInner)->Name());
        pOuter = mid.Outer();
    }
    if (pOuter->Kind() != NK_IdentifierExpr) return false;
    spelled.push_back(
        static_cast<SnIdentifierExpr*>(pOuter)->Name());
    return spelled == expected;
}

//Shared ED_Call frame emitter for the two call spellings (the bare-name
//invoke arm and the qualified static member call in the member arm):
//module-prefix gate, full application (D4), args encoded with
//allowCall=false (D3). The spellings differ only in how the target key
//is assembled; the frame bytes — and the consumer's rebuild — are one.
bool VmBackend::AppendCallFrame(const std::string& key, size_t paramCount,
                                SnInvokeExpr& invoke,
                                const std::vector<SnFormalParam*>& formals,
                                std::vector<uint8_t>& out, int depth) {
    if (!HasModulePrefix(key, m_compiledModule.modulePath)) return false;
    size_t argCount = 0;
    for (auto& arg : invoke.Params())
        ++argCount;
    if (argCount != paramCount || argCount > UINT8_MAX)
        return false;
    AppendU8(out, ED_Call);
    AppendU16(out, static_cast<uint16_t>(key.size()));
    AppendBytes(out, key.data(), key.size());
    AppendU32(out, static_cast<uint32_t>(argCount));
    AppendU8(out, static_cast<uint8_t>(argCount));
    for (auto& arg : invoke.Params()) {
        if (!EncodeExprDesc(arg, formals, out, false, depth + 1))
            return false;
    }
    return true;
}

//Call arm of the ExprDesc encoder: a producing-module free function or
//static method, fully applied (D4); args encode with allowCall=false
//(no nested calls, D3). The consumer rebuilds a call qualified by THIS
//module's path. The qualified `Type.member(args)` spelling is not here
//— the grammar builds it as a member with an invoke inner, folded by
//the member arm, which shares AppendCallFrame with this arm.
bool VmBackend::EncodeCallDesc(SnInvokeExpr& invoke,
                               const std::vector<SnFormalParam*>& formals,
                               std::vector<uint8_t>& out, int depth) {
    auto* pCallee = invoke.Callee();
    if (!pCallee || pCallee->IsImported()) return false;
    //Free functions of this module, or static methods of this module's
    //classes/interfaces (D-5 language surface; the receiverless
    //type-name shape EmitMemberStaticCall emits). Instance methods need
    //a receiver the wire cannot carry; enum members keep the old reject
    //(enclosure-keyed dispatch is their only binding path).
    auto* pParent = pCallee->Parent();
    std::string key;
    if (pParent && (pParent->Kind() == NK_ClassDecl
                    || pParent->Kind() == NK_InterfaceDecl)) {
        if (!pCallee->ContainFlags(NF_Static)) return false;
        key = StaticMemberKey(*pCallee);
    } else if (pParent && pParent->Kind() == NK_EnumDecl) {
        return false;
    } else {
        key = KeyOf(*pCallee);  //free function: "<module>.<name>"
    }
    return AppendCallFrame(key, pCallee->Params().size(), invoke,
                           formals, out, depth);
}

//Recursive ExprDesc encoder (see CompiledModule.h → ExprDesc.h for the
//grammar). Returns false for anything outside the subset — the caller
//discards the partial bytes and keeps the RTK_Unfoldable stamp (old
//reject behavior).
bool VmBackend::EncodeExprDesc(SnExpression& expr,
                               const std::vector<SnFormalParam*>& formals,
                               std::vector<uint8_t>& out, bool allowCall,
                               int depth) {
    if (depth > kMaxExprDescDepth) return false;
    switch (expr.Kind()) {
    case NK_LiteralExpr:
        return EncodeLiteralDesc(static_cast<SnLiteralExpr&>(expr), out);

    case NK_IdentifierExpr:
        return EncodeNameRefDesc(static_cast<SnIdentifierExpr&>(expr),
                                 formals, out);

    case NK_MemberExpr:
        return EncodeStaticSpellDesc(static_cast<SnMemberExpr&>(expr),
                                     formals, out, allowCall, depth);

    case NK_BinaryExpr:
        return EncodeBinaryDesc(static_cast<SnBinaryExpr&>(expr), formals,
                                out, allowCall, depth);

    case NK_InvokeExpr:
        if (!allowCall) return false;  //nested calls: out of subset (D3)
        return EncodeCallDesc(static_cast<SnInvokeExpr&>(expr), formals,
                              out, depth);

    default:
        return false;  //casts, init lists, non-static member access, …
    }
}

//Identifier arm of the ExprDesc encoder: a formal reference, or for a
//func-typed formal bound by name, a producing-module function value.
bool VmBackend::EncodeNameRefDesc(SnIdentifierExpr& name,
                                  const std::vector<SnFormalParam*>& formals,
                                  std::vector<uint8_t>& out) {
    auto* pField = name.Field();
    if (!pField) return false;
    //Formal references only — a global/const default keeps the old
    //reject. The declaration-time forward-ref check guarantees the
    //bound formal precedes the default's own formal.
    for (size_t i = 0; i < formals.size(); ++i) {
        if (pField == formals[i]) {
            AppendU8(out, ED_FormalRef);
            AppendU8(out, static_cast<uint8_t>(i));
            return true;
        }
    }
    //By-name function value (`= helper` in a func-typed formal, the
    //Phase 13 bound state: Field() is the function symbol): emit the
    //qualified key so the consumer rebuilds the name and re-binds it
    //against the formal's func type at its own use site.
    if (pField->Kind() == NK_Function) {
        const std::string key = KeyOf(*pField);
        if (!HasModulePrefix(key, m_compiledModule.modulePath))
            return false;  //imported or prelude target: old reject
        AppendU8(out, ED_FuncRef);
        AppendU16(out, static_cast<uint16_t>(key.size()));
        AppendBytes(out, key.data(), key.size());
        return true;
    }
    return false;
}

//Member arm of the ExprDesc encoder: the static member spellings in a
//default value. The grammar builds the qualified call `Knob.tick()` as
//a member whose inner is a named invoke (nlang.y MemberExpr: Expression
//'.' InvokeExpr), so the inner kind separates the three spells: a
//by-name static method reference (`= Knob.tick`, ED_FuncRef), a static
//field read (`= Knob.K`, ED_StaticMember), and a static method call
//(`= Knob.tick()`, ED_Call). All three require the type-name receiver
//the resolver guarantees for static shapes (EmitExprMemberStatic D3),
//re-proven here by SpellsEnclosingChain: a bound instance reference
//cannot ride the key-only wire.
bool VmBackend::EncodeStaticSpellDesc(SnMemberExpr& member,
                                      const std::vector<SnFormalParam*>& formals,
                                      std::vector<uint8_t>& out,
                                      bool allowCall, int depth) {
    auto* pField = member.Field();
    auto* pInner = member.Inner();
    if (!pField || !pInner || !pField->ContainFlags(NF_Static)
        || (pField->Kind() != NK_Function
            && pField->Kind() != NK_ClassField))
        return false;
    if (!SpellsEnclosingChain(member, *pField))
        return false;
    if (pInner->Kind() == NK_InvokeExpr) {
        //Static method call: the receiver and the target both ride
        //the StaticMemberKey. A call in a call-argument slot stays
        //out of the subset (D3). Callee() answers the bound
        //function symbol (null for a non-function target — a `()`
        //suffix on a static field keeps the old reject); the
        //receiver-spell gate above already proved the type-name
        //chain against this member.
        if (!allowCall) return false;
        auto& invoke = static_cast<SnInvokeExpr&>(*pInner);
        auto* pCallee = invoke.Callee();
        if (!pCallee || pCallee->IsImported()) return false;
        return AppendCallFrame(StaticMemberKey(*pCallee),
                               pCallee->Params().size(), invoke,
                               formals, out, depth);
    }
    if (pInner->Kind() != NK_IdentifierExpr)
        return false;  //not a plain static read/reference spell
    return EncodeMemberRefDesc(*pField, out);
}

//Top entry: encode func's default expression into an ExprDesc tree.
//True = dv carries RTK_Expr + exprBytes; false = outside the subset and
//dv stays without a default (caller stamps RTK_Unfoldable).
bool VmBackend::EncodeDefaultExpr(SnFunction& func, SnExpression* pExpr,
                                  DefaultValueDesc& dv) {
    if (!pExpr) return false;
    std::vector<SnFormalParam*> formals;
    for (auto& param : func.Params())
        formals.push_back(&param);
    std::vector<uint8_t> bytes;
    if (!EncodeExprDesc(*pExpr, formals, bytes, true, 0)
        || bytes.size() > kMaxExprDescBytes)
        return false;
    dv.tag = RTK_Expr;
    dv.exprBytes = std::move(bytes);
    return true;
}

} //namespace nlang
