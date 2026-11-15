/*---
test_descriptor_spelling.cpp — 0.13.8 Task 7: the two-key contract.

Row keys (LinkKeyOf) stay bare under the erasure model: two packages may
each declare a same-named generic template and their VM table keys are
identical strings. Descriptor spellings (DescriptorSpellingOf) carry the
producing package — a.Pipe<int> and b.Pipe<int> stay distinct descriptor
rows, and builtin instantiations (List<int>, no declared template) spell
bare. Walks REAL emitted rows: two one-file packages built through the
in-process harness, and one main per package from IDENTICAL source text
(the qualified generic source spelling is item 7 / Plan B, so one main
cannot yet name both packages' Pipe), asserting the row keys stay
identical while the descriptor rows diverge by producing package.
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/TypeArgSpelling.h>
#include <nlang/compiler/SyntaxNode.h>
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/CompiledModuleRecords.h>
#include "builder/ModuleRegistry.h"
#include "test_support/test_scratch.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace
{
class MemLogger : public CompileLogger
{
public:
    const std::vector<std::string>& errorsText() const
    {
        return m_errors;
    }

protected:
    void WriteLog(CompileLogLevel level, const ISourceLocation* pLocation,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }

private:
    std::vector<std::string> m_errors;
};

bool writeFile(const std::filesystem::path& file, const char* szBody)
{
    std::ofstream stream(file, std::ios::binary);
    if (!stream)
        return false;
    stream << szBody;
    return stream.good();
}

//Two one-file packages, each declaring the SAME-named generic template —
//the erasure model's hardest case for descriptor identity.
const char* szPipeLib =
    "class Pipe<T> {\n"
    "    public native Pipe();\n"
    "    public native int f(T v);\n"
    "}\n";

//IDENTICAL main source text used twice: once importing package a, once
//package b. The qualified generic source spelling (a.Pipe<int>) does not
//exist in the grammar yet (spec item 7, Plan B — D7.4 owns the one-main
//both-packages disambiguation family), so this shape is how Plan A pins
//the identity half: same source, different producing package, distinct
//dotted descriptor rows.
const char* szMainBody =
    "import %PKG%;\n"
    "int main() {\n"
    "    Pipe<int> p = Pipe();\n"
    "    List<int> lst = {};\n"
    "    return 0;\n"
    "}\n";

//Join one unit's descriptor-table spellings for assertion messages (the
//row set is the evidence the test reasons over).
QString spellingDump(const CompiledModule& unit)
{
    QString dump;
    for (const auto& row : unit.descriptorTable)
    {
        if (row.spelling.empty())
            continue;
        if (!dump.isEmpty())
            dump += ", ";
        dump += QString::fromStdString(row.spelling);
    }
    return dump;
}
} //namespace

class TestDescriptorSpelling : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    }

    void twoKeyContractOnRealDescriptorRows()
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            nlang::test::ScratchDir("nlang_descriptor_spelling");
        const fs::path out = dir / "out";
        std::error_code fsError;
        fs::remove_all(dir, fsError);
        fs::create_directories(out, fsError);
        QVERIFY2(writeFile(dir / "a.n", szPipeLib),
            "lib a source could not be written");
        QVERIFY2(writeFile(dir / "b.n", szPipeLib),
            "lib b source could not be written");
        QVERIFY2(writeFile(dir / "main.n", szMainBody),
            "main source could not be written");

        //Stage 1: the two same-named packages, compiled to out/a.ncu and
        //out/b.ncu. The builders stay alive so the row-key half of the
        //contract can read each tree's Pipe declaration.
        auto buildLib = [&dir, &out](const char* szName,
            MemLogger& logger) -> std::unique_ptr<ModuleBuilder>
        {
            BuildParams params;
            params.m_SourceFiles.push_back(
                (dir / (std::string(szName) + ".n")).string());
            params.m_sOutputModule = szName;
            params.m_sOutputDir = out.string();
            params.m_sTempDir = out.string();
            params.m_sStdLibDir = STDLIB_DIR;
            std::unique_ptr<ModuleBuilder> builder =
                std::make_unique<ModuleBuilder>(params, logger);
            bool ok = false;
            try { ok = builder->BuildArtifacts(); }
            catch (const std::exception&) { ok = false; }
            if (!ok)
                return nullptr;
            return builder;
        };
        MemLogger loggerA;
        MemLogger loggerB;
        std::unique_ptr<ModuleBuilder> builderA = buildLib("a", loggerA);
        std::unique_ptr<ModuleBuilder> builderB = buildLib("b", loggerB);
        QString libErrors;
        for (const auto& e : loggerA.errorsText())
            libErrors += "a: " + QString::fromStdString(e) + " | ";
        for (const auto& e : loggerB.errorsText())
            libErrors += "b: " + QString::fromStdString(e) + " | ";
        QVERIFY2(builderA && builderB,
            qPrintable("both packages must build clean — " + libErrors));

        //Row keys: LinkKeyOf is the key space Register.cpp writes VM
        //table rows under — the erasure model keeps both packages'
        //template keys the identical bare string.
        auto findPipe = [](const ModuleBuilder& builder) -> const SnField*
        {
            const SnField* pFound = nullptr;
            std::function<void(const Node&)> walk =
                [&](const Node& node)
            {
                if (pFound)
                    return;
                if (node.Kind() == NK_ClassDecl)
                {
                    const auto& field = static_cast<const SnField&>(node);
                    if (field.Name() == "Pipe")
                    {
                        pFound = &field;
                        return;
                    }
                }
                const auto& children = node.Children();
                for (auto it = children.cbegin(); it != children.cend(); ++it)
                    walk(*it);
            };
            walk(builder.TreeRootView());
            return pFound;
        };
        const SnField* pPipeA = findPipe(*builderA);
        const SnField* pPipeB = findPipe(*builderB);
        QVERIFY2(pPipeA && pPipeB,
            "each package tree must declare its Pipe template");
        QCOMPARE(LinkKeyOf(builderA->Registry(), *pPipeA),
            std::string("Pipe"));
        QCOMPARE(LinkKeyOf(builderB->Registry(), *pPipeB),
            std::string("Pipe"));

        //Stage 2: two mains from the SAME source text, one per package;
        //the emitted descriptor rows carry the identity side.
        auto buildMain = [&dir, &out](const char* szPackage,
            const char* szModuleName, QString& errorsText)
            -> ModuleBuilder::UnitImages
        {
            const std::string body = [&szPackage]()
            {
                std::string text = szMainBody;
                const size_t at = text.find("%PKG%");
                if (at != std::string::npos)
                    text.replace(at, 5, szPackage);
                return text;
            }();
            const fs::path src = dir / (std::string(szModuleName) + ".n");
            if (!writeFile(src, body.c_str()))
                return {};
            BuildParams mainParams;
            mainParams.m_SourceFiles.push_back(src.string());
            mainParams.m_ImportDirs.clear();
            mainParams.m_ImportDirs.push_back(out.string());
            mainParams.m_sOutputModule = szModuleName;
            mainParams.m_sOutputDir = out.string();
            mainParams.m_sTempDir = out.string();
            mainParams.m_sStdLibDir = STDLIB_DIR;
            MemLogger mainLogger;
            ModuleBuilder mainBuilder(mainParams, mainLogger);
            ModuleBuilder::UnitImages imgs;
            bool built = false;
            QString thrown;
            try
            {
                imgs = mainBuilder.BuildUnitImages();
                built = true;
            }
            catch (const std::exception& e)
            {
                thrown = QString::fromLatin1(e.what());
            }
            for (const auto& e : mainLogger.errorsText())
                errorsText += QString::fromStdString(e) + " | ";
            if (!thrown.isEmpty())
                errorsText += thrown;
            if (!built || !mainLogger.errorsText().empty())
                return {};
            return imgs;
        };
        QString errorsA;
        QString errorsB;
        ModuleBuilder::UnitImages imgsA = buildMain("a", "main_a", errorsA);
        ModuleBuilder::UnitImages imgsB = buildMain("b", "main_b", errorsB);
        QVERIFY2(!imgsA.units.empty(),
            qPrintable("main_a must build clean — " + errorsA));
        QVERIFY2(!imgsB.units.empty(),
            qPrintable("main_b must build clean — " + errorsB));

        const CompiledModule& unitA = imgsA.units[0];
        const CompiledModule& unitB = imgsB.units[0];
        auto contains = [](const CompiledModule& unit,
            const std::string& spelling)
        {
            for (const auto& row : unit.descriptorTable)
                if (row.spelling == spelling)
                    return true;
            return false;
        };
        QVERIFY2(contains(unitA, "a.Pipe<int>"),
            qPrintable("package a's Pipe<int> must spell qualified — rows: "
                + spellingDump(unitA)));
        QVERIFY2(contains(unitB, "b.Pipe<int>"),
            qPrintable("package b's Pipe<int> must spell qualified — rows: "
                + spellingDump(unitB)));
        QVERIFY2(!contains(unitA, "Pipe<int>")
            && !contains(unitB, "Pipe<int>"),
            qPrintable("the bare collapsed spelling must be gone — rows a: "
                + spellingDump(unitA) + " rows b: " + spellingDump(unitB)));
        QVERIFY2(contains(unitA, "List<int>"),
            qPrintable("builtin instantiation must spell bare — rows: "
                + spellingDump(unitA)));
    }
};

#include "test_descriptor_spelling.moc"
QTEST_GUILESS_MAIN(TestDescriptorSpelling)
