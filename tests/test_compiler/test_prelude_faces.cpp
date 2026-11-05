/*---
    test_prelude_faces.cpp — prelude String/Array face declarations
    (v0.12.2 Task 1): presence and member counts in the merged tree,
    Object-chain exclusion (spec D10 — face classes stay out of the
    class hierarchy), and binding-row stamping of the declared native
    records onto the intrinsic channel.
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/vm/Intrinsics.h>
#include "builder/ModuleRegistry.h"
#include "builder/ExprResolver.h"    //FindPreludeClassDecl
#include "SnTypes.h"                 //SnBuiltinDataType
#include "SyntaxTree.h"              //TheAST
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

namespace {

//Collects error diagnostics in memory; errors() drives the assertions.
class MemLogger : public CompileLogger
{
public:
    const std::vector<std::string>& errorsText() const
    {
        return m_errors;
    }

protected:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }

private:
    std::vector<std::string> m_errors;
};

std::string writeTempSource(const QString& name, const char* body)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "nlang_prelude_faces_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//One in-process build of a snippet; the builder stays alive for the
//whole callback so Registry()/BuildUnitImages() remain valid.
template <typename Fn>
bool withBuild(const char* szName, const char* szBody, Fn fn)
{
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(szName, szBody));
    params.m_sOutputModule = szName;
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_prelude_faces_tests";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    ModuleBuilder::UnitImages images;
    bool ok = false;
    //Exception boundary (same contract as ncc main).
    try { images = builder.BuildUnitImages(); ok = !images.units.empty(); }
    catch (const std::exception&) { ok = false; }
    if (!ok)
    {
        for (const auto& err : logger.errorsText())
            qWarning("build error: %s", err.c_str());
        return false;
    }
    fn(builder, images);
    return true;
}

int countNativeMethods(const SnClassDecl& sn)
{
    int methods = 0;
    for (auto& member : sn.Members())
        if (member.Kind() == NK_Function)
            ++methods;
    return methods;
}

//Whole-tree count of static fields (class statics and interface
//constants — both are NK_ClassField with NF_Static). The cross-unit
//static-slot archive's replay fidelity premise is "a unit's
//staticSlots table is exactly its own declarations" (v0.12.6 Task
//11): it holds only while the prelude units declare no statics. A
//prelude static would shift every unit's table; this count turns
//that silent drift into a loud failure.
int countStaticClassFields(const Node& node)
{
    //Children() yields Node&; every node in the tree is a SyntaxNode
    //in practice (same idiom as ModuleBuilder's pending-ref walk) —
    //downcast once for the kind/flag check.
    auto& syn = static_cast<const SyntaxNode&>(node);
    int count = (syn.Kind() == NK_ClassField
                 && syn.ContainFlags(NF_Static)) ? 1 : 0;
    const auto& children = node.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
        count += countStaticClassFields(*it);
    return count;
}

//The records of one class's declared methods, gathered through the
//CompiledClass method table (records carry bare method names — class
//ownership lives only in methodIndices).
std::vector<const CompiledFunction*> classMethodRecords(
    ModuleBuilder::UnitImages& images, const char* szClassName)
{
    std::vector<const CompiledFunction*> records;
    for (auto& unit : images.units)
    {
        const int classIdx = unit.FindClass(szClassName);
        if (classIdx < 0)
            continue;
        for (uint16_t funcIdx :
            unit.classes[size_t(classIdx)].methodIndices)
            records.push_back(&unit.functions[funcIdx]);
        break;
    }
    return records;
}

} //namespace

class TestPreludeFaces : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    void stringFaceDeclaredWith22MethodsAndNoBase();
    void arrayFaceDeclaredWith6MethodsAndNoBase();
    void stringFaceBindingRowsStampIntrinsics();
    void preludeDeclaresNoStaticFields();
};

void TestPreludeFaces::stringFaceDeclaredWith22MethodsAndNoBase()
{
    QVERIFY(withBuild("string_face", "int main() { return 0; }",
        [](ModuleBuilder& builder, ModuleBuilder::UnitImages&) {
        auto* pString = FindPreludeClassDecl(builder.Registry(), "String");
        QVERIFY(pString != nullptr);
        //22 methods, but substring declares twice (the no-default-param
        //overload pair, spec D3) — 23 function members.
        QCOMPARE(countNativeMethods(*pString), 23);
        //D10: face classes stay out of the Object hierarchy — no
        //implicit base attached, none declared.
        QVERIFY(pString->SuperClass() == nullptr);
        QVERIFY(pString->SuperName() == nullptr);
        //Name ownership: the declared face class is the bare name's sole
        //root entry; the runtime-minted NK_String mirror yields its name
        //index entry (it stays attached — registry identity survives).
        QCOMPARE(TheAST().Root()->FindField("String"),
            static_cast<SnField*>(pString));
        QVERIFY(SnBuiltinDataType::InstanceOf(NK_String) != nullptr);
    }));
}

void TestPreludeFaces::arrayFaceDeclaredWith6MethodsAndNoBase()
{
    QVERIFY(withBuild("array_face", "int main() { return 0; }",
        [](ModuleBuilder& builder, ModuleBuilder::UnitImages& images) {
        auto* pArray = FindPreludeClassDecl(builder.Registry(), "Array");
        QVERIFY(pArray != nullptr);
        QCOMPARE(countNativeMethods(*pArray), 6);
        QVERIFY(pArray->SuperClass() == nullptr);
        QVERIFY(pArray->SuperName() == nullptr);
        //Split surface (v0.12.7 Task 5): length/rank/shape stay
        //compile-time-static emissions with no intrinsic id (no binding
        //row, inert host-native declarations); toString/equals/
        //getHashCode joined the Object protocol and stamp binding rows.
        const auto records = classMethodRecords(images, "Array");
        QCOMPARE(int(records.size()), 6);
        std::set<std::string> names;
        for (const auto* fn : records)
        {
            names.insert(fn->name);
            if (fn->name == "length" || fn->name == "rank"
                || fn->name == "shape")
            {
                QCOMPARE(fn->intrinsicId, uint16_t(INTR_None));
                QVERIFY(fn->isNative);
            }
            else if (fn->name == "toString")
            {
                QCOMPARE(fn->intrinsicId,
                    uint16_t(INTR_Array_toString));
                QVERIFY(!fn->isNative);
            }
            else if (fn->name == "equals")
            {
                QCOMPARE(fn->intrinsicId, uint16_t(INTR_Object_Equals));
                QVERIFY(!fn->isNative);
            }
            else if (fn->name == "getHashCode")
            {
                QCOMPARE(fn->intrinsicId,
                    uint16_t(INTR_Object_GetHashCode));
                QVERIFY(!fn->isNative);
            }
        }
        //Exactly the declared face.
        QCOMPARE(names, (std::set<std::string>{
            "length", "rank", "shape", "toString", "equals",
            "getHashCode"}));
    }));
}

void TestPreludeFaces::stringFaceBindingRowsStampIntrinsics()
{
    QVERIFY(withBuild("string_stamp",
        "int main() { string s = \"ab\"; return s.substring(1).length(); }",
        [](ModuleBuilder&, ModuleBuilder::UnitImages& images) {
        const auto records = classMethodRecords(images, "String");
        QCOMPARE(int(records.size()), 23);
        for (const auto* fn : records)
        {
            if (fn->name == "substring")
            {
                //Both overloads key the same binding row, so both
                //declared records stamp onto the one intrinsic id.
                QCOMPARE(fn->intrinsicId,
                    uint16_t(INTR_String_Substring));
                QVERIFY(!fn->isNative);
            }
            else if (fn->name == "length")
            {
                //No binding row (OP_StrLen fast path): the declared
                //record stays an inert host-native declaration.
                QCOMPARE(fn->intrinsicId, uint16_t(INTR_None));
                QVERIFY(fn->isNative);
            }
        }
    }));
}

//v0.12.6 Task 11 hardening: the merged tree inlines the prelude units
//alongside the snippet. The snippet declares exactly one static field,
//so any additional static anywhere in the tree is a prelude (or other
//inlined library) static — the exact declaration the static-slot
//archive's fidelity premise forbids.
void TestPreludeFaces::preludeDeclaresNoStaticFields()
{
    QVERIFY(withBuild("prelude_no_statics",
        "class Probe {\n"
        "    public static int total = 1;\n"
        "}\n"
        "int main() { return Probe.total; }",
        [](ModuleBuilder& builder, ModuleBuilder::UnitImages&) {
        QCOMPARE(countStaticClassFields(builder.TreeRootView()), 1);
    }));
}

QTEST_GUILESS_MAIN(TestPreludeFaces)
#include "test_prelude_faces.moc"
