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
#include "test_support/test_scratch.h"
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/core/CompiledModule.h>
#include <nlang/core/ExprDesc.h>
#include <nlang/core/ModuleLoader.h>
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
    const auto dir = nlang::test::ScratchDir("nlang_default_exprdesc_tests");
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
            { "add", "sub", "mul", "div", "mod", "shl", "shr",
              "band", "bor", "bxor" };
        return op <= EDB_BitXor ? kNames[op] : "?op";
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
            case ED_BitNot:
                return "(bnot " + expr() + ")";
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
            case ED_FuncRef:
            case ED_StaticMember:
            {
                uint16_t keyLen = 0;
                if (!takeU16(keyLen) || off + keyLen > bytes.size())
                    return "!ref";
                const std::string key(reinterpret_cast<const char*>(
                    bytes.data() + off), keyLen);
                off += keyLen;
                return std::string(tag == ED_FuncRef ? "(ref "
                    : "(member ") + key + ")";
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

//Decodes one function's second-formal default and compares it against
//the canonical S-expression. False = mismatch, with the reason in
//*pReason (the CHECK call site prefixes the function name).
static bool checkSecondShape(const CompiledModule& mod, const char* szFunc,
    const std::string& expect, std::string* pReason)
{
    const DefaultValueDesc* pDv = secondDefault(mod, szFunc);
    if (!pDv) { *pReason = "second default missing"; return false; }
    if (pDv->tag != RTK_Expr) {
        *pReason = "expected RTK_Expr, tag " + std::to_string(pDv->tag);
        return false;
    }
    RawDecoder decoder(pDv->exprBytes);
    const std::string shape = decoder.whole();
    if (!decoder.ok || shape != expect) {
        *pReason = "expected " + expect + ", got " + shape;
        return false;
    }
    return true;
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
        "int anded(int a, int b = a & 3) { return b; }\n"
        "int ored(int a, int b = a | 8) { return b; }\n"
        "int xored(int a, int b = a ^ 5) { return b; }\n"
        "int bnoted(int a, int b = ~a) { return b; }\n"
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
    //"<modulePath>.<name>" and NOT under its unqualified name.
    CHECK(mod.FindFunction(mod.modulePath + ".addIdent") >= 0,
        "project functions are keyed <modulePath>.<name>");
    CHECK(mod.FindFunction("addIdent") < 0,
        "the unqualified name is not a table key");

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
        {"anded", "(band (formal 0) (int 3))"},
        {"ored", "(bor (formal 0) (int 8))"},
        {"xored", "(bxor (formal 0) (int 5))"},
        {"bnoted", "(bnot (formal 0))"},
        {"negate", "(neg (formal 0))"},
        {"scaled", "(mul (formal 0) (float))"},
        {"precise", "(mul (formal 0) (double))"},
        {"big", "(mul (formal 0) (long))"},
    };
    for (const Expectation& expect : kExpects)
    {
        std::string reason;
        CHECK(checkSecondShape(mod, expect.func, expect.shape, &reason),
            std::string(expect.func) + ": " + reason);
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

//The three cross-module forms Task 8 adds must encode as RTK_Expr with
//the producing module's path prefixing the qualified key. A form that
//falls back to RTK_Unfoldable makes the default unusable by every
//importing module even though the spelling is legal at language level
//the producer compiled it from.
static void test_wire_shapes_new_forms()
{
    TEST(wire_shapes_new_forms);
    std::unique_ptr<CompileOutcome> out;
    CompiledModule mod = compileAndLoad(
        "int helper(int x) { return x + 3; }\n"
        "class Knob {\n"
        "    public static int K = 7;\n"
        "    public static int tick() { return 9; }\n"
        "}\n"
        "int byName(int a, func<int,int> b = helper) { return a; }\n"
        "int staticRef(int a, func<int> b = Knob.tick) { return a; }\n"
        "int staticField(int a, int b = Knob.K) { return b; }\n"
        "int staticCall(int a, int b = Knob.tick()) { return b; }\n"
        "int main() { return 0; }\n",
        out);
    CHECK(out && out->ok,
        "build failed: " + (out ? joinErrors(*out) : std::string("?")));
    CHECK(out->logger->errors().empty(),
        "clean compile expected: " + joinErrors(*out));

    struct Expectation { const char* func; std::string shape; };
    const Expectation kExpects[] = {
        //By-name free function: the key carries the producer's path.
        {"byName", "(ref " + mod.modulePath + ".helper)"},
        //By-name static method: three segments, class in the middle.
        {"staticRef", "(ref " + mod.modulePath + ".Knob.tick)"},
        //Static field read.
        {"staticField", "(member " + mod.modulePath + ".Knob.K)"},
        //Qualified static call: the call arm's key space extends the
        //same way; paramCount is the callee's (zero here).
        {"staticCall", "(call " + mod.modulePath + ".Knob.tick p0)"},
    };
    for (const Expectation& expect : kExpects)
    {
        std::string reason;
        CHECK(checkSecondShape(mod, expect.func, expect.shape, &reason),
            std::string(expect.func) + ": " + reason);
    }
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

//Cross-module default lambdas (0.13.13a ruling B): a library compile
//(no entry candidate) rejects a lambda default at the producer instead
//of stamping a silently unusable RTK_Unfoldable — the wire carries no
//closure, so every importing module would hit the consumer-side reject
//at its own use site. The wording splits by cause: an instance-capturing
//lambda (method hosts) gets the capture sentence, everything else the
//scope sentence. A program compilation (main present) keeps the same
//sources green — the guard is about export, not about lambda defaults
//at large (the same-module capture semantics are pinned in Task 2's
//legs).
static void test_exported_default_lambda_reject()
{
    TEST(exported_default_lambda_reject);

    //Library free function whose lambda captures its own formal: the
    //cross-module scope sentence.
    {
        std::unique_ptr<CompileOutcome> out;
        compileAndLoad(
            "int step(int n, func<int,int> g = (x) -> x + n)"
            " { return g(n); }\n", out);
        CHECK(out && !out->ok,
            "a library default lambda must reject at the producer");
        CHECK(joinErrors(*out).find("lambda expressions are supported"
            " in default values of non-exported declarations")
            != std::string::npos,
            "expected the scope sentence, got: " + joinErrors(*out));
    }

    //Library method whose lambda reads an instance field: the capture
    //sentence outranks the scope one — the capture is the binding
    //reason the value cannot be exported at all (a closure over a
    //dangling receiver is meaningless in any importing module).
    {
        std::unique_ptr<CompileOutcome> out;
        compileAndLoad(
            "class Box {\n"
            "    public int k = 1;\n"
            "    public int use(int d, func<int,int> g ="
            " (x) -> x + k) { return g(d); }\n"
            "}\n", out);
        CHECK(out && !out->ok,
            "a library method default lambda capturing an instance"
            " member must reject at the producer");
        CHECK(joinErrors(*out).find("a default lambda that captures"
            " 'this' or an instance member cannot be exported")
            != std::string::npos,
            "expected the capture sentence, got: " + joinErrors(*out));
    }

    //Program witness: the same two sources plus main compile clean —
    //no diagnostic may leak onto the program side.
    {
        std::unique_ptr<CompileOutcome> out;
        compileAndLoad(
            "int step(int n, func<int,int> g = (x) -> x + n)"
            " { return g(n); }\n"
            "class Box {\n"
            "    public int k = 1;\n"
            "    public int use(int d, func<int,int> g ="
            " (x) -> x + k) { return g(d); }\n"
            "}\n"
            "int main() { return step(2); }\n", out);
        CHECK(out && out->ok,
            "a program's default lambdas must compile: "
            + (out ? joinErrors(*out) : std::string("?")));
        CHECK(out->logger->errors().empty(),
            "clean compile expected: " + joinErrors(*out));
    }
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
//read out of bounds or create a tree from a corrupt stream.
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
        {"bad binary operator", {ED_Binary, 0x0A}, "bad binary operator"},
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
        CompiledFunction unqualifiedNamed;
        unqualifiedNamed.name = "helper";
        unqualifiedNamed.paramCount = 1;
        unqualified.functions.push_back(std::move(unqualifiedNamed));
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

    //The valid call arm creates the module-qualified member-invoke shape.
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

//--- new-form reader legs (built with the byte push helpers) ------------

//The consumer rebuilds every qualified key as the dotted member chain
//it spells — a two-segment free-function key must hit the producing
//module's function table (the call arm's rule), while a three-segment
//class-member key binds through the CONSUMER's import registry at the
//use site, so the producing module's table need not name the class.
static void test_reader_rebuilds_new_forms()
{
    TEST(reader_rebuilds_new_forms);
    const CompiledModule cm = readerFixtureModule();
    const std::vector<std::string> formals = {"a", "b"};

    struct RefCase { uint8_t tag; const char* key; };
    const RefCase kValid[] = {
        {ED_FuncRef, "lib.helper"},       //free function, table-backed
        {ED_FuncRef, "lib.Knob.tick"},    //static method, import-bound
        {ED_StaticMember, "lib.Knob.K"},  //static field, import-bound
    };
    for (const RefCase& valid : kValid)
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes.push_back(valid.tag);
        pushU16(dv.exprBytes,
            static_cast<uint16_t>(std::strlen(valid.key)));
        dv.exprBytes.insert(dv.exprBytes.end(), valid.key,
            valid.key + std::strlen(valid.key));
        SnExpression* pExpr = SynthDefaultExpr(dv, cm, "caller", 1,
            formals, stubLocation());
        CHECK(pExpr && pExpr->Kind() == NK_MemberExpr,
            std::string(valid.key) + ": expected a member chain rebuild");
    }

    //A three-segment static call key flows through the existing call
    //arm unchanged: leaf invoke on the rebuilt class chain.
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes.push_back(ED_Call);
        const char* key = "lib.Knob.tick";
        pushU16(dv.exprBytes, static_cast<uint16_t>(std::strlen(key)));
        dv.exprBytes.insert(dv.exprBytes.end(), key, key + std::strlen(key));
        pushU32(dv.exprBytes, 0);   //paramCount = 0
        dv.exprBytes.push_back(0);  //argCount = 0
        SnExpression* pExpr = SynthDefaultExpr(dv, cm, "caller", 1,
            formals, stubLocation());
        CHECK(pExpr && pExpr->Kind() == NK_MemberExpr,
            "static call rebuilds a member-chain invoke");
    }

    //Rejections: an unqualified key (the writer always prefixes the
    //producing path), and a two-segment key missing from the table.
    struct RejectCase { uint8_t tag; const char* key; const char* part; };
    const RejectCase kRejects[] = {
        {ED_FuncRef, "helper", "unqualified reference key"},
        {ED_FuncRef, "lib.nope", "not a function of the producing module"},
    };
    for (const RejectCase& bad : kRejects)
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes.push_back(bad.tag);
        pushU16(dv.exprBytes, static_cast<uint16_t>(std::strlen(bad.key)));
        dv.exprBytes.insert(dv.exprBytes.end(), bad.key,
            bad.key + std::strlen(bad.key));
        bool threw = false;
        std::string message;
        try {
            SynthDefaultExpr(dv, cm, "caller", 1, formals, stubLocation());
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        CHECK(threw && message.find(bad.part) != std::string::npos,
            std::string(bad.key) + ": expected throw containing '"
            + bad.part + "', got " + (threw ? message : "no throw"));
    }

    //A key truncated below its declared length is malformed on both
    //new tags.
    {
        DefaultValueDesc dv;
        dv.tag = RTK_Expr;
        dv.exprBytes = {ED_StaticMember, 4, 0, 'l', 'i', 'b'};
        bool threw = false;
        std::string message;
        try {
            SynthDefaultExpr(dv, cm, "caller", 1, formals, stubLocation());
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        CHECK(threw
            && message.find("truncated stream") != std::string::npos,
            "truncated member key: expected the named throw, got "
            + (threw ? message : "no throw"));
    }
    PASS();
}

//The consumer's RTK_Unfoldable reject message enumerates the FULL
//supported subset; the assertion pins the whole assembled sentence so a
//tail edit cannot silently drop or mistranscribe one of the forms.
static void test_rejects_unfoldable_message()
{
    TEST(rejects_unfoldable_message);
    const CompiledModule cm = readerFixtureModule();
    const std::vector<std::string> formals = {"a", "b"};
    DefaultValueDesc dv;
    dv.tag = RTK_Unfoldable;
    bool threw = false;
    std::string message;
    try {
        SynthDefaultExpr(dv, cm, "caller", 1, formals, stubLocation());
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    const std::string expect =
        "imported function 'caller' has a default for parameter 1 outside"
        " the cross-module subset; supported: int/float/double/long/ulong/"
        "string/bool/char/null literals, formal parameter references,"
        " unary - and ~, + - * / % << >> & | ^ combinations over those,"
        " calls of exported free functions or static members, and"
        " by-name references to free functions or static members as"
        " function values";
    CHECK(threw, "RTK_Unfoldable default must throw");
    CHECK(message == expect,
        "expected the exact sentence '" + expect + "', got '" + message
        + "'");
    PASS();
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
    std::vector<uint8_t> badOp{ED_Binary, 0x0A};
    pushU32(badOp, 0);
    pushU32(badOp, 0);
    CHECK(!ExprDescRemapStrings(badOp, identity),
        "binary operator above EDB_BitXor is rejected");
    std::vector<uint8_t> trailing{RTK_Null, RTK_Null};
    CHECK(!ExprDescRemapStrings(trailing, identity),
        "trailing bytes after the root node are rejected");
    std::vector<uint8_t> badCall{ED_Call};
    pushU16(badCall, 4);
    const char* shortKey = "abcd";
    badCall.insert(badCall.end(), shortKey, shortKey + 4);
    CHECK(!ExprDescRemapStrings(badCall, identity),
        "call header truncated before paramCount is rejected");

    //The new name-key tags pass through the remap unchanged (key bytes
    //name symbols, not string-pool indices) and their truncation
    //family is rejected like the rest.
    std::vector<uint8_t> member{ED_StaticMember};
    const char* memberKey = "lib.Knob.K";
    pushU16(member, static_cast<uint16_t>(std::strlen(memberKey)));
    member.insert(member.end(), memberKey, memberKey
        + std::strlen(memberKey));
    CHECK(ExprDescRemapStrings(member, identity),
        "static member stream remaps as a no-op");
    std::vector<uint8_t> cutRef{ED_FuncRef, 5, 0, 'l', 'i', 'b'};
    CHECK(!ExprDescRemapStrings(cutRef, identity),
        "reference key truncated below its length is rejected");
    //A func ref riding as a call argument validates through the call
    //arm's child loop.
    std::vector<uint8_t> refArg{ED_Call};
    const char* fnKey = "lib.helper";
    pushU16(refArg, static_cast<uint16_t>(std::strlen(fnKey)));
    refArg.insert(refArg.end(), fnKey, fnKey + std::strlen(fnKey));
    pushU32(refArg, 1);
    refArg.push_back(1);
    refArg.push_back(ED_FuncRef);
    const char* fnRefKey = "lib.Knob.tick";
    pushU16(refArg, static_cast<uint16_t>(std::strlen(fnRefKey)));
    refArg.insert(refArg.end(), fnRefKey, fnRefKey
        + std::strlen(fnRefKey));
    CHECK(ExprDescRemapStrings(refArg, identity),
        "call carrying a function reference remaps");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    test_wire_shapes_compound();
    test_wire_shapes_new_forms();
    test_wire_shapes_literal_boundary();
    test_producer_boundary_unfoldable();
    test_exported_default_lambda_reject();
    test_reader_rejects_malformed();
    test_reader_rebuilds_formal_ref();
    test_reader_rebuilds_new_forms();
    test_rejects_unfoldable_message();
    test_remap_strings();
    std::cerr << "\ndefault_exprdesc: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
