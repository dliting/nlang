/*---
    VmExecutorOpsDescriptor.cpp — v0.13.1 运行时实例化描述符配方合成
    与typeName操作码（编码契约见CompiledModule.h DescriptorEntry；
    载体与OpNew的写入位在VmExecutorOpsObjects.cpp）。
    K8: the arms run lock-free — descriptor reads are benign heap reads
    and MintNewString takes the intern table's domain lock internally.
---*/
#include "VmExecutor.h"
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
    int32_t thisIdx = 0;
    std::memcpy(&thisIdx, locals, sizeof(thisIdx));
    const GenericInstDescriptor* recv = InstDescriptorOf(thisIdx);
    if (!recv || ordinal >= recv->TypeArgs().size())
        throw std::runtime_error(
            "NLang VM: typeName argument without an instance receiver");
    const int32_t handle = MintNewString(
        CanonicalArgSpelling(recv->TypeArgs()[ordinal]));
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

} // namespace nlang
