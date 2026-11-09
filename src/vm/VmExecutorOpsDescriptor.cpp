/*---
    VmExecutorOpsDescriptor.cpp — v0.13.1 运行时实例化描述符配方合成
    与typeName操作码（编码契约见CompiledModule.h DescriptorEntry；
    载体与OpNew的写入位在VmExecutorOpsObjects.cpp）。
    K8: the arms run lock-free — descriptor reads are benign heap reads
    and MintNewString takes the intern table's domain lock internally.
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/RnTypes.h>
#include <cstring>
#include <exception>

namespace nlang {

namespace
{
//Little-endian u16 read at pos, advancing it (recipe wire order).
uint16_t RecipeU16(const std::vector<uint8_t>& recipe, size_t& pos)
{
    if (pos + 2 > recipe.size())
        throw std::runtime_error("NLang VM: truncated descriptor recipe");
    const uint16_t v = static_cast<uint16_t>(recipe[pos])
        | (static_cast<uint16_t>(recipe[pos + 1]) << 8);
    pos += 2;
    return v;
}
} //namespace

//v0.13.1 D3: compose this execution's descriptor from a recipe row —
//ARG_THIS reads the receiver's own descriptor (frame slot 0 = this).
//OP_New rows are TEMPLATE_HEAD-led instantiation recipes; the composed
//canonical spelling round-trips through the parser, so structure and
//leaf kinds resolve in one place; the intern pool folds repeated
//compositions into one descriptor.
GenericInstDescriptor* VmExecutor::ComposeInstDescriptor(
    const std::vector<uint8_t>& recipe, const uint8_t* locals) {
    size_t pos = 0;
    const std::string spelling =
        ComposeRecipeSpelling(recipe, pos, recipe.size(), locals);
    return InternDescriptorFromSpelling(spelling, *m_currModule);
}

//v0.13.1: one whole recipe row's spelling, no interning — typeName
//renders the string; only object stamping (OP_New) needs a descriptor.
//Rows are self-describing by leading tag: 0 walks the TEMPLATE_HEAD
//instantiation form, anything else is a single argument encoding
//(typeName of T[] rides ARG_ARRAY).
std::string VmExecutor::ComposeRecipeRowSpelling(
    const std::vector<uint8_t>& recipe, const uint8_t* locals) {
    size_t pos = 0;
    if (!recipe.empty() && recipe[0] == 0)
        return ComposeRecipeSpelling(recipe, pos, recipe.size(), locals);
    return ComposeArgSpelling(recipe, pos, locals);
}

//One argument encoding's canonical spelling (see DescriptorEntry).
std::string VmExecutor::ComposeArgSpelling(
    const std::vector<uint8_t>& recipe, size_t& pos, const uint8_t* locals)
{
    if (pos >= recipe.size())
        throw std::runtime_error("NLang VM: truncated descriptor recipe");
    const uint8_t tag = recipe[pos++];
    if (tag == 1) {  //ARG_CONST: the complete leaf form
        const uint16_t len = RecipeU16(recipe, pos);
        if (pos + len > recipe.size())
            throw std::runtime_error("NLang VM: truncated descriptor recipe");
        std::string s(recipe.begin() + static_cast<long>(pos),
                      recipe.begin() + static_cast<long>(pos + len));
        pos += len;
        return s;
    }
    if (tag == 2) {  //ARG_THIS: the receiver descriptor's argument
        const uint8_t paramIdx = recipe[pos++];
        int32_t thisIdx = 0;
        std::memcpy(&thisIdx, locals, sizeof(thisIdx));  //slot 0 = this
        const GenericInstDescriptor* recv = InstDescriptorOf(thisIdx);
        if (!recv || paramIdx >= recv->TypeArgs().size())
            throw std::runtime_error(
                "NLang VM: generic construction without an instance receiver");
        return CanonicalArgSpelling(recv->TypeArgs()[paramIdx]);
    }
    if (tag == 3) {  //ARG_ARRAY: element spelling + the bracket run
        const uint8_t rank = recipe[pos++];
        std::string s = ComposeArgSpelling(recipe, pos, locals);
        s += "[";
        for (uint8_t i = 1; i < rank; ++i) s += ",";
        s += "]";
        return s;
    }
    if (tag == 4) {  //ARG_NESTED: a complete child recipe range
        const uint16_t len = RecipeU16(recipe, pos);
        const size_t childEnd = pos + len;
        if (childEnd > recipe.size())
            throw std::runtime_error("NLang VM: truncated descriptor recipe");
        return ComposeRecipeSpelling(recipe, pos, childEnd, locals);
    }
    throw std::runtime_error("NLang VM: unknown descriptor recipe tag");
}

//One recipe range's canonical spelling: TEMPLATE_HEAD, then argument
//encodings until end.
std::string VmExecutor::ComposeRecipeSpelling(
    const std::vector<uint8_t>& recipe, size_t& pos, size_t end,
    const uint8_t* locals)
{
    if (pos >= end || recipe[pos] != 0)
        throw std::runtime_error("NLang VM: malformed descriptor recipe");
    ++pos;
    const uint16_t nameLen = RecipeU16(recipe, pos);
    if (pos + nameLen > end)
        throw std::runtime_error("NLang VM: truncated descriptor recipe");
    std::string spelling(recipe.begin() + static_cast<long>(pos),
                         recipe.begin() + static_cast<long>(pos + nameLen));
    pos += nameLen;
    spelling += "<";
    bool first = true;
    while (pos < end) {
        if (!first) spelling += ",";
        spelling += ComposeArgSpelling(recipe, pos, locals);
        first = false;
    }
    spelling += ">";
    return spelling;
}

//v0.13.1: typeName(T) — read the receiver's instantiation descriptor
//(frame slot 0 = this) and take the ordinal-th type argument's canonical
//spelling. A missing descriptor is a corrupt image (the resolver rejects
//static contexts; `this` is a valid receiver here), not a user error.
void VmExecutor::OpTypeNameArg(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint8_t ordinal = reader.ReadByte();
    const TypeArgDesc& arg = ReceiverTypeArgOf(locals, ordinal);
    const int32_t handle = MintNewString(CanonicalArgSpelling(arg));
    std::memcpy(locals + dst, &handle, sizeof(handle));
}

//v0.13.1: typeName(Hold<T>) / typeName(T[]) — compose the recipe row's
//spelling against the receiver's instantiation (ARG_THIS steps read
//this) and mint the canonical string. No descriptor is interned: only
//object stamping (OP_New) needs one.
void VmExecutor::OpTypeNameCompose(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t recipeIdx = reader.ReadUint16();
    if (recipeIdx >= m_currModule->descriptorTable.size())
        throw std::runtime_error(
            "NLang VM: invalid descriptor index in typeName");
    const int32_t handle = MintNewString(ComposeRecipeRowSpelling(
        m_currModule->descriptorTable[recipeIdx].recipe, locals));
    std::memcpy(locals + dst, &handle, sizeof(handle));
}

//v0.13.1 D5: stamp an array allocation's element instantiation
//descriptor on the record's heap slot — the array opcodes' shared tail.
//The stamp rides the same heap-parallel carrier as object descriptors
//(D1: arrays share the heap index space); for an array record the cell
//holds the ELEMENT's descriptor, and the owning allocation always
//resets it first (AllocArrayOnHeap), so a recycled slot never leaks a
//previous tenant's stamp.
void VmExecutor::StampElemDescriptor(int32_t heapIdx, uint16_t descIdx,
                                     const uint8_t* locals) {
    if (descIdx == 0xFFFF)
        return;
    m_instDescByHeap[static_cast<size_t>(heapIdx)] =
        CastTargetDescriptor(descIdx, locals);
}

//v0.13.1 D5: an array record's element instantiation descriptor — the
//owning record's stamp; a view reads its OWNER's (views validate their
//element kind against the owner at creation, so the owner's element
//descriptor is the view's).
const GenericInstDescriptor* VmExecutor::ArrayElemDescriptorOf(
    int32_t heapIdx) {
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_ArrayView) {
        const uint8_t rank = static_cast<uint8_t>(
            m_structHeap[static_cast<size_t>(heapIdx)][2]);
        heapIdx = m_structHeap[static_cast<size_t>(heapIdx)][
            ArrayViewTargetCell(rank)];
    }
    return InstDescriptorOf(heapIdx);
}

//v0.13.1 D6: the shared receiver-argument resolution of the T opcodes —
//frame slot 0 is `this`, and its instantiation descriptor's ordinal-th
//type argument names the kind this execution dispatches on. A missing
//descriptor or an out-of-range ordinal is a corrupt image (the resolver
//rejects static contexts and mismatched parameters before emission).
const TypeArgDesc& VmExecutor::ReceiverTypeArgOf(const uint8_t* locals,
                                                 uint8_t ordinal) {
    int32_t thisIdx = 0;
    std::memcpy(&thisIdx, locals, sizeof(thisIdx));
    const GenericInstDescriptor* recv = InstDescriptorOf(thisIdx);
    if (!recv || ordinal >= recv->TypeArgs().size())
        throw std::runtime_error(
            "NLang VM: generic operation without an instance receiver");
    return recv->TypeArgs()[ordinal];
}

//v0.13.1 D6: two T-typed slot values under the resolved argument kind —
//each kind mirrors its dedicated opcode's semantics exactly (scalar
//inline cells, OP_Eq_str content, OP_Eq_struct field value, func record
//content, class/array handle identity), so a T comparison always agrees
//with the same comparison written on the instantiated type.
bool VmExecutor::TypeArgValuesEqual(const TypeArgDesc& arg,
                                    const uint8_t* lhs,
                                    const uint8_t* rhs) {
    int32_t lh, rh;
    std::memcpy(&lh, lhs, sizeof(lh));
    std::memcpy(&rh, rhs, sizeof(rh));
    //Identity fast path (also settles null == null for every kind before
    //the heap-reading comparators — OpStructEquality's null rule).
    if (lh == rh)
        return true;
    if (int row = ScalarPrimIndexOfRtk(arg.resolvedKind); row >= 0) {
        //Inline cells: narrow rows hold the value-extended low cell (the
        //OpBox/BoxedValuesEqual convention); 8-byte rows carry the high
        //cell too. Raw-cell compare — the family's float rule.
        if (lh != rh)
            return false;
        if (kScalarPrims[row].slotWidth < 8)
            return true;
        int32_t lhi, rhi;
        std::memcpy(&lhi, lhs + 4, sizeof(lhi));
        std::memcpy(&rhi, rhs + 4, sizeof(rhi));
        return lhi == rhi;
    }
    switch (arg.resolvedKind) {
    case RTK_String:
        return StringsEqual(lh, rh);
    case RTK_Struct:
        return StructValuesEqual(lh, rh);
    case RTK_Func:
        return FuncRecordValuesEqual(lh, rh);
    case RTK_Class:
    case RTK_Array:
        //Handle identity — the container indexOf rule.
        return false;  //lh != rh already settled above
    default:
        throw std::runtime_error(
            "NLang VM: equality on an unsupported type argument");
    }
}

//v0.13.1 D6: OP_Eq_T/OP_Ne_T — dispatch on the receiver's type argument
//kind. The null-literal side (0xFFFF, no slot) takes the kind's
//reference null test: handle kinds compare the other side against 0
//exactly as their dedicated opcodes' null comparisons do (string rides
//StringsEqual, the `s == null` rule), and a scalar kind is never null —
//a zero value is a value, not a null.
void VmExecutor::OpTypeParamEquality(BytecodeReader& reader,
                                     uint8_t* locals, OpCode op) {
    const bool bNeg = (op == OpCode::OP_Ne_T);
    const uint16_t dst = reader.ReadUint16();
    const uint16_t ordinal = reader.ReadUint16();
    const uint16_t lhs = reader.ReadUint16();
    const uint16_t rhs = reader.ReadUint16();
    const TypeArgDesc& arg = ReceiverTypeArgOf(
        locals, static_cast<uint8_t>(ordinal));
    bool bEqual;
    if (lhs == 0xFFFF || rhs == 0xFFFF) {
        const uint8_t* other =
            (lhs == 0xFFFF) ? locals + rhs : locals + lhs;
        int32_t handle;
        std::memcpy(&handle, other, sizeof(handle));
        if (ScalarPrimIndexOfRtk(arg.resolvedKind) >= 0)
            bEqual = false;
        else if (arg.resolvedKind == RTK_String)
            bEqual = StringsEqual(handle, 0);
        else
            bEqual = (handle == 0);
    } else {
        bEqual = TypeArgValuesEqual(arg, locals + lhs, locals + rhs);
    }
    const int32_t r = ((bEqual != bNeg) ? 1 : 0);
    std::memcpy(locals + dst, &r, sizeof(r));
}

//v0.13.1 D6: the kind's own to-string on the accumulator's T value —
//scalars through the registry renderer (the OP_Prim_to_str body), a
//string value is already the result, and handle kinds render through
//the shared FormatHeapValue walk (list/dict formats, the class's
//virtual toString, "<struct>", the array bracket walk, "<null>").
void VmExecutor::OpTypeArgToStr(BytecodeReader& reader,
                                const uint8_t* locals, uint8_t* pResult) {
    const TypeArgDesc& arg =
        ReceiverTypeArgOf(locals, reader.ReadByte());
    if (arg.resolvedKind == RTK_String)
        return;
    if (int row = ScalarPrimIndexOfRtk(arg.resolvedKind); row >= 0) {
        std::string s = RnBuiltinDataType::InstanceOf(
            kScalarPrims[row].kind)->ValueToString(pResult);
        const int32_t handle = MintNewString(std::move(s));
        std::memcpy(pResult, &handle, sizeof(handle));
        return;
    }
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    const int32_t handle = MintNewString(FormatHeapValue(heapIdx, 0));
    std::memcpy(pResult, &handle, sizeof(handle));
}

} // namespace nlang
