/*---
test_default_exprdesc.cpp - ExprDesc default-expression grammar drift
guards (NCU v2.3).

Three layers against the serialized default-value expression grammar:
1. Wire round-trip: a real ModuleBuilder compile of default shapes,
   BuildArtifacts writing the .ncu, ModuleLoader::Load reading it back,
   and an INDEPENDENT hand-rolled decoder (raw byte reads written
   against the grammar comment in ExprDesc.h, not ExprDescCursor)
   asserting the canonical shape of every arm. A writer change that
   drifts from the documented grammar fails here without any matching
   change on the decoder side.
2. Producer boundary: literal defaults keep their scalar channels
   (never RTK_Expr); defaults outside the subset (cast, nested call)
   stamp RTK_Unfoldable — the unchanged consumer-side reject.
3. Reader hardening: SynthDefaultExpr throws the named malformed-module
   errors on truncated / garbage / over-deep / trailing / bad-call
   streams, and the ExprDesc.h link-time remap rejects the same
   malformed family (NcuLinker's corrupt-image gate).

The e2e suite (cross_mod_default_expr) pins the consumer rebuild path
end to end; this suite pins the wire contract itself.
---*/
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/ExprDesc.h>
#include "ModuleLoader.h"
#include "CompiledModuleDefaultSupport.hpp"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

//Collects error diagnostics in memory (same shape as test_array_flags).
class MemLogger : public CompileLogger
{
public:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }
    const std::vector<std::string>& errors() const { return m_errors; }
private:
    std::vector<std::string> m_errors;
};

struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

//Compile one in-memory source through the real builder and load the
//written .ncu back — the writer/wire/reader path under test. Module
//names are uniquified (ModuleManager refuses a second Create).
static CompiledModule compileAndLoad(const char* szBody,
    std::unique_ptr<CompileOutcome>& outOutcome)
{
    static uint32_t runCount = 0;
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_default_exprdesc_tests";
    std::error_code fsError;
    std::filesystem::create_directories(dir, fsError);
    const auto file = dir / "main.n";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << szBody;
    }
    auto out = std::make_unique<CompileOutcome>();
    out->params = std::make_unique<BuildParams>();
    out->params->m_SourceFiles.push_back(file.string());
    out->params->m_sProjectDir = dir.string();
    out->params->m_sOutputModule =
        "default_exprdesc_" + std::to_string(++runCount);
    out->params->m_sOutputDir = dir.string();
    out->params->m_sTempDir = dir.string();
    out->logger = std::make_unique<MemLogger>();
    out->builder = std::make_unique<ModuleBuilder>(*out->params,
        *out->logger);
    CompiledModule mod;
    try {
        out->ok = out->builder->BuildArtifacts();
        if (out->ok)
            mod = ModuleLoader::Load(
                (dir / (out->params->m_sOutputModule + ".ncu")).string());
    } catch (const std::exception&) {
        out->ok = false;
    }
    outOutcome = std::move(out);
    return mod;
}

static std::string joinErrors(const CompileOutcome& out)
{
    std::string all;
    for (const auto& e : out.logger->errors())
        all += e + "\n";
    return all;
}

//--- independent decoder -------------------------------------------------
//Raw little-endian reads against the ExprDesc.h grammar comment. No
//ExprDescCursor, no production reader code: a writer/reader co-drift
//(both sides changing together, silently redefining the wire) cannot
//pass through this decoder.

struct RawDecoder
{
    const std::vector<uint8_t>& bytes;
    size_t off = 0;
    bool ok = true;

    explicit RawDecoder(const std::vector<uint8_t>& stream) :
        bytes(stream)
    {
    }

    bool takeU8(uint8_t& v)
    {
        if (off + 1 > bytes.size()) { ok = false; return false; }
        v = bytes[off++];
        return true;
    }
    bool takeU16(uint16_t& v)
    {
        if (off + 2 > bytes.size()) { ok = false; return false; }
        std::memcpy(&v, bytes.data() + off, 2);
        off += 2;
        return true;
    }
    bool takeU32(uint32_t& v)
    {
        if (off + 4 > bytes.size()) { ok = false; return false; }
        std::memcpy(&v, bytes.data() + off, 4);
        off += 4;
        return true;
    }

    static const char* opName(uint8_t op)
    {
        static const char* kNames[] =
            { "add", "sub", "mul", "div", "mod", "shl", "shr" };
        return op <= EDB_Shr ? kNames[op] : "?op";
    }

    std::string expr()
    {
        uint8_t tag = 0;
        if (!takeU8(tag))
            return "!eof";
        switch (tag)
        {
            case RTK_Int32:
            {
                uint32_t v = 0;
                if (!takeU32(v)) return "!int";
                return "(int " + std::to_string(v) + ")";
            }
            case RTK_Float:
            {
                if (off + 4 > bytes.size()) { ok = false; return "!float"; }
                off += 4;
                return "(float)";
            }
            case RTK_Long:
            case RTK_ULong:
            case RTK_Double:
            {
                if (off + 8 > bytes.size()) { ok = false; return "!wide"; }
                off += 8;
                return tag == RTK_Double ? "(double)"
                    : (tag == RTK_Long ? "(long)" : "(ulong)");
            }
            case RTK_String:
            {
                uint32_t v = 0;
                if (!takeU32(v)) return "!str";
                return "(str " + std::to_string(v) + ")";
            }
            case RTK_Null:
                return "(null)";
            case ED_FormalRef:
            {
                uint8_t v = 0;
                if (!takeU8(v)) return "!formal";
                return "(formal " + std::to_string(v) + ")";
            }
            case ED_Neg:
                return "(neg " + expr() + ")";
            case ED_Binary:
            {
                uint8_t op = 0;
                if (!takeU8(op)) return "!op";
                const std::string left = expr();
                const std::string right = expr();
                return std::string("(") + opName(op) + " " + left
                    + " " + right + ")";
            }
            case ED_Call:
            {
                uint16_t keyLen = 0;
                uint32_t paramCount = 0;
                uint8_t argCount = 0;
                if (!takeU16(keyLen) || off + keyLen > bytes.size())
                    return "!call";
                std::string key(reinterpret_cast<const char*>(
                    bytes.data() + off), keyLen);
                off += keyLen;
                if (!takeU32(paramCount) || !takeU8(argCount))
                    return "!call";
                std::string text = "(call " + key + " p"
                    + std::to_string(paramCount);
                for (uint8_t i = 0; i < argCount && ok; ++i)
                    text += " " + expr();
                text += ")";
                return text;
            }
            default:
                ok = false;
                return "!tag";
        }
    }

    std::string whole()
    {
        const std::string text = expr();
        if (ok && off != bytes.size())
            ok = false;
        return text;
    }
};

//The second formal's DefaultValueDesc of a project function. The .ncu
//function table keys free functions by their PACKAGE-QUALIFIED path
//("<modulePath>.<name>") — the same key rule the ExprDesc call arm and
//the consumer stub lookup agree on, so the test addresses functions the
//same way the loader contract does.
static const DefaultValueDesc* secondDefault(const CompiledModule& mod,
    const char* szFunc)
{
    const std::string key = mod.modulePath + "." + szFunc;
    const int idx = mod.FindFunction(key);
    if (idx < 0 || mod.functions[idx].defaultValues.size() < 2)
        return nullptr;
    return &mod.functions[idx].defaultValues[1];
}

//Compound shapes must decode to the canonical S-expressions; every arm
//of the grammar is exercised by a real source spelling.
static void test_wire_shapes_compound()
{
    TEST(wire_shapes_compound);
    std::unique_ptr<CompileOutcome> out;
    CompiledModule mod = compileAndLoad(
        "int addIdent(int a, int b = a * 2) { return b; }\n"
        "int offset(int a, int b = a * 2 + 1) { return b; }\n"
        "int flipped(int a, int b = 2 * a + 1) { return b; }\n"
        "int subbed(int a, int b = a - 1) { return b; }\n"
        "int halved(int a, int b = a / 2) { return b; }\n"
        "int remainder(int a, int b = a % 3) { return b; }\n"
        "int shifted(int a, int b = a << 2) { return b; }\n"
        "int dropped(int a, int b = a >> 1) { return b; }\n"
        "int negate(int a, int b = -a) { return b; }\n"
        "float scaled(int a, float b = a * 1.5f) { return b; }\n"
        "double precise(int a, double b = a * 2.5) { return b; }\n"
        "long big(int a, long b = a * 5000000000) { return b; }\n"
        "int helper(int x) { return x + 3; }\n"
        "int callsHelper(int a, int b = helper(a)) { return b; }\n"
        "int main() { return addIdent(5); }\n",
        out);
    CHECK(out && out->ok,
        "build failed: " + (out ? joinErrors(*out) : std::string("?")));
    CHECK(out->logger->errors().empty(),
        "clean compile expected: " + joinErrors(*out));

    //The qualified-key rule itself: a project function is findable under
    //"<modulePath>.<name>" and NOT under its bare name.
    CHECK(mod.FindFunction(mod.modulePath + ".addIdent") >= 0,
        "project functions are keyed <modulePath>.<name>");
    CHECK(mod.FindFunction("addIdent") < 0,
        "the bare name is not a table key");

    struct Expectation { const char* func; const char* shape; };
    const Expectation kExpects[] = {
        {"addIdent", "(mul (formal 0) (int 2))"},
        {"offset", "(add (mul (formal 0) (int 2)) (int 1))"},
        {"flipped", "(add (mul (int 2) (formal 0)) (int 1))"},
        {"subbed", "(sub (formal 0) (int 1))"},
        {"halved", "(div (formal 0) (int 2))"},
        {"remainder", "(mod (formal 0) (int 3))"},
        {"shifted", "(shl (formal 0) (int 2))"},
        {"dropped", "(shr (formal 0) (int 1))"},
        {"negate", "(neg (formal 0))"},
        {"scaled", "(mul (formal 0) (float))"},
        {"precise", "(mul (formal 0) (double))"},
        {"big", "(mul (formal 0) (long))"},
    };
    for (const Expectation& expect : kExpects)
    {
        const DefaultValueDesc* pDv = secondDefault(mod, expect.func);
        CHECK(pDv, expect.func + std::string(": second default missing"));
        CHECK(pDv->tag == RTK_Expr,
            expect.func + std::string(": expected RTK_Expr, tag ")
            + std::to_string(pDv->tag));
        RawDecoder decoder(pDv->exprBytes);
        const std::string shape = decoder.whole();
        CHECK(decoder.ok && shape == expect.shape,
            expect.func + std::string(": expected ")
            + expect.shape + ", got " + shape);
    }

    //Call arm: the key is qualified by the PRODUCING module's path and
    //carries the target's parameter count.
    const DefaultValueDesc* pDv = secondDefault(mod, "callsHelper");
    CHECK(pDv && pDv->tag == RTK_Expr,
        "callsHelper: RTK_Expr default expected");
    RawDecoder decoder(pDv->exprBytes);
    const std::string shape = decoder.whole();
    const std::string expectKey = "(call " + mod.modulePath
        + ".helper p1 (formal 0))";
    CHECK(decoder.ok && shape == expectKey,
        "callsHelper: expected " + expectKey + ", got " + shape);
    PASS();
}

//Literal defaults keep their scalar channels — the fold boundary that
//keets simple defaults on the pre-v2.3 wire shape.
static void test_wire_shapes_literal_boundary()
{
    TEST(wire_shapes_literal_boundary);
    std::unique_ptr<CompileOutcome> out;
    CompiledModule mod = compileAndLoad(
        "int literalInt(int a, int b = 7) { return b; }\n"
        "string literalText(int a, string b = \"hi\") { return b; }\n"
        "int main() { return literalInt(5); }\n",
        out);
    CHECK(out && out->ok,
        "build failed: " + (out ? joinErrors(*out) : std::string("?")));

    const DefaultValueDesc* pInt = secondDefault(mod, "literalInt");
    CHECK(pInt && pInt->tag == RTK_Int32,
        "literalInt: scalar RTK_Int32 expected");
    CHECK(pInt->intValue == 7, "literalInt: value channel carries 7");
    CHECK(pInt->exprBytes.empty(),
        "literalInt: no exprBytes on the folded shape");

    const DefaultValueDesc* pStr = secondDefault(mod, "literalText");
    CHECK(pStr && pStr->tag == RTK_String,
        "literalText: scalar RTK_String expected");
    CHECK(pStr->stringIdx < mod.stringConstants.size()
        && mod.stringConstants[pStr->stringIdx] == "hi",
        "literalText: string index names the pooled \"hi\"");
    PASS();
}

//Defaults outside the subset compile in the producer but stamp
//RTK_Unfoldable — the unchanged consumer-side reject contract.
static void test_producer_boundary_unfoldable()
{
    TEST(producer_boundary_unfoldable);
    std::unique_ptr<CompileOutcome> out;
    CompiledModule mod = compileAndLoad(
        "int casted(float a, int b = a as int) { return b; }\n"
        "int helper(int x) { return x + 3; }\n"
        "int nestedCall(int a, int b = helper(helper(a))) { return b; }\n"
        "int main() { return 0; }\n",
        out);
    CHECK(out && out->ok,
        "build failed: " + (out ? joinErrors(*out) : std::string("?")));

    const DefaultValueDesc* pCast = secondDefault(mod, "casted");
    CHECK(pCast && pCast->tag == RTK_Unfoldable,
        "casted: RTK_Unfoldable expected, tag "
        + std::to_string(pCast ? pCast->tag : 0xFF));
    const DefaultValueDesc* pNested = secondDefault(mod, "nestedCall");
    CHECK(pNested && pNested->tag == RTK_Unfoldable,
        "nestedCall: RTK_Unfoldable expected (D3: no nested calls)");
    PASS();
}

//--- reader hardening ----------------------------------------------------

//The stub location type from CompiledModuleStubSupport.hpp.
static CompiledModuleNodeLocation& stubLocation()
{
    static CompiledModuleNodeLocation loc("exprdesc_tests");
    return loc;
}

//A minimal CompiledModule for the direct SynthDefaultExpr probes:
//one caller function (the queried name) and one target the call arm
//validates against.
static CompiledModule readerFixtureModule()
{
    CompiledModule cm;
    cm.modulePath = "lib";
    cm.stringConstants.push_back("hi");
    CompiledFunction caller;
    caller.name = "caller";
    caller.paramCount = 2;
    cm.functions.push_back(std::move(caller));
    CompiledFunction target;
    target.name = "lib.helper";
    target.paramCount = 1;
    cm.functions.push_back(std::move(target));
    return cm;
}

//SynthDefaultExpr must throw the named malformed-module errors, never
//read out of bounds or mint a tree from a corrupt stream.
static void test_reader_rejects_malformed()
{
    TEST(reader_rejects_malformed);
    const CompiledModule cm = readerFixtureModule();
    const std::vector<std::string> formals = {"a", "b"};
    const ISourceLocation& loc = stubLocation();

    struct MalformedCase
    {
        const char* name;
        std::vector<uint8_t> bytes;
        const char* messagePart;
    };
    const MalformedCase kCases[] = {
        //ED_Binary missing its right operand.
        {"truncated binary", {ED_Binary, EDB_Add, ED_FormalRef, 0},
            "truncated stream"},
        {"unknown tag", {0x7A}, "unknown node tag"},
        {"bad binary operator", {ED_Binary, 0x07}, "bad binary operator"},
        //A complete node followed by junk.
        {"trailing bytes",
            {ED_FormalRef, 0, RTK_Int32, 0, 0, 0, 0}, "trailing bytes"},
        {"string index out of range",
            {RTK_String, 0x09, 0, 0, 0}, "string index out of range"},
        //17 nested ED_Neg — one past kMaxExprDescDepth.
        {"expression too deep",
            [] {
                std::vector<uint8_t> deep(17, ED_Neg);
                deep.push_back(RTK_Int32);
                deep.push_back(0); deep.push_back(0);
                deep.push_back(0); deep.push_back(0);
                return deep;
            }(), "expression too deep"},
        //The call target must be a function OF the producing module.
        {"call target not a function",
            [] {
                std::vector<uint8_t> bytes{ED_Call, 8, 0};
                const char* key = "lib.nope";
                bytes.insert(bytes.end(), key, key + 8);
                bytes.push_back(1); bytes.push_back(0); bytes.push_back(0);
                bytes.push_back(0);  //paramCount = 1
                bytes.push_back(1);  //argCount = 1
                bytes.push_back(ED_FormalRef); bytes.push_back(0);
                return bytes;
            }(), "not a function of the producing module"},
    };
    for (const MalformedCase& bad : kCases)
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes = bad.bytes;
        bool threw = false;
        std::string message;
        try {
            SynthDefaultExpr(dv, cm, "caller", 1, formals, loc);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        CHECK(threw && message.find(bad.messagePart) != std::string::npos,
            bad.name + std::string(": expected throw containing '")
            + bad.messagePart + "', got " + (threw ? message : "no throw"));
    }

    //An unqualified call key is malformed too (the writer prefix-checks,
    //so only a corrupt image can carry one).
    {
        CompiledModule unqualified = readerFixtureModule();
        CompiledFunction bare;
        bare.name = "helper";
        bare.paramCount = 1;
        unqualified.functions.push_back(std::move(bare));
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes = {ED_Call, 6, 0, 'h', 'e', 'l', 'p', 'e', 'r',
            1, 0, 0, 0, 1, ED_FormalRef, 0};
        bool threw = false;
        std::string message;
        try {
            SynthDefaultExpr(dv, unqualified, "caller", 1, formals, loc);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        CHECK(threw
            && message.find("unqualified call key") != std::string::npos,
            "unqualified key: expected the named throw, got "
            + (threw ? message : "no throw"));
    }

    //The valid call arm mints the module-qualified member-invoke shape.
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes = {ED_Call, 10, 0, 'l', 'i', 'b', '.', 'h', 'e',
            'l', 'p', 'e', 'r', 1, 0, 0, 0, 1, ED_FormalRef, 0};
        SnExpression* pExpr = SynthDefaultExpr(dv, cm, "caller", 1,
            formals, loc);
        CHECK(pExpr && pExpr->Kind() == NK_MemberExpr,
            "valid call rebuilds a module-qualified member invoke");
    }
    PASS();
}

//RTK_Expr happy path through SynthDefaultExpr: a formal reference
//rebuilds as an identifier with the stub formal's name.
static void test_reader_rebuilds_formal_ref()
{
    TEST(reader_rebuilds_formal_ref);
    const CompiledModule cm = readerFixtureModule();
    const std::vector<std::string> formals = {"a", "b"};
    DefaultValueDesc dv;
    dv.tag = RTK_Expr;
    dv.exprBytes = {ED_FormalRef, 0};
    SnExpression* pExpr = SynthDefaultExpr(dv, cm, "caller", 1, formals,
        stubLocation());
    CHECK(pExpr && pExpr->Kind() == NK_IdentifierExpr,
        "formal reference rebuilds an identifier");
    PASS();
}

//--- link-time remap helper (ExprDesc.h) ---------------------------------

//Little-endian push helpers for the hand-built streams.
static void pushU16(std::vector<uint8_t>& bytes, uint16_t v)
{
    const auto raw = static_cast<uint16_t>(v);
    bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&raw),
        reinterpret_cast<const uint8_t*>(&raw) + 2);
}
static void pushU32(std::vector<uint8_t>& bytes, uint32_t v)
{
    bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&v),
        reinterpret_cast<const uint8_t*>(&v) + 4);
}

//The remap rewrites RTK_String producer-pool indices in place, fully
//validates the stream, and rejects the malformed family NcuLinker must
//never link.
static void test_remap_strings()
{
    TEST(remap_strings);
    const auto identity = [](uint32_t idx) { return idx * 10; };

    //Empty stream: a no-op default is valid.
    std::vector<uint8_t> empty;
    CHECK(ExprDescRemapStrings(empty, identity),
        "empty stream remaps as a no-op");

    //A string index is rewritten in place.
    std::vector<uint8_t> str{RTK_String};
    pushU32(str, 2);
    CHECK(ExprDescRemapStrings(str, identity), "string stream remaps");
    uint32_t remapped = 0;
    std::memcpy(&remapped, str.data() + 1, 4);
    CHECK(remapped == 20, "string index rewritten through the lookup");

    //A string arm nested in a call argument remaps too.
    std::vector<uint8_t> call{ED_Call};
    pushU16(call, 10);
    const char* key = "lib.helper";
    call.insert(call.end(), key, key + 10);
    pushU32(call, 1);
    call.push_back(1);
    call.push_back(RTK_String);
    pushU32(call, 3);
    CHECK(ExprDescRemapStrings(call, identity), "call stream remaps");
    std::memcpy(&remapped, call.data() + call.size() - 4, 4);
    CHECK(remapped == 30, "nested string argument rewritten in place");

    //Malformed family: truncation, garbage, over-depth, trailing.
    //Non-const: the remap takes the stream by mutable reference even
    //when it rejects it.
    std::vector<uint8_t> truncated{RTK_String, 2, 0};
    CHECK(!ExprDescRemapStrings(truncated, identity),
        "truncated string stream is rejected");
    std::vector<uint8_t> garbage{0x99};
    CHECK(!ExprDescRemapStrings(garbage, identity),
        "unknown tag is rejected");
    std::vector<uint8_t> deep(17, ED_Neg);
    deep.push_back(RTK_Int32);
    pushU32(deep, 0);
    CHECK(!ExprDescRemapStrings(deep, identity),
        "depth above kMaxExprDescDepth is rejected");
    std::vector<uint8_t> badOp{ED_Binary, 0x09};
    pushU32(badOp, 0);
    pushU32(badOp, 0);
    CHECK(!ExprDescRemapStrings(badOp, identity),
        "binary operator above EDB_Shr is rejected");
    std::vector<uint8_t> trailing{RTK_Null, RTK_Null};
    CHECK(!ExprDescRemapStrings(trailing, identity),
        "trailing bytes after the root node are rejected");
    std::vector<uint8_t> badCall{ED_Call};
    pushU16(badCall, 4);
    const char* shortKey = "abcd";
    badCall.insert(badCall.end(), shortKey, shortKey + 4);
    CHECK(!ExprDescRemapStrings(badCall, identity),
        "call header truncated before paramCount is rejected");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    test_wire_shapes_compound();
    test_wire_shapes_literal_boundary();
    test_producer_boundary_unfoldable();
    test_reader_rejects_malformed();
    test_reader_rebuilds_formal_ref();
    test_remap_strings();
    std::cerr << "\ndefault_exprdesc: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
