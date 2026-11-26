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
#include "GenericInstDescriptor.h"
#include "NcuLinker.h"
#include "NcuLoader.h"
#include "VmExecutor.h"
#include "builder/ModuleRegistry.h"
#include "test_support/test_scratch.h"
#ifndef Q_MOC_RUN
#include <filesystem>
#endif
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
//the erasure model's hardest case for descriptor identity. The generic
//interface rides along: its rows key the interfaces table (KeyOf exact,
//no erasure bridge), the other family the intern resolver must admit.
const char* szPipeLib =
    "class Pipe<T> {\n"
    "    public native Pipe();\n"
    "    public native int f(T v);\n"
    "}\n"
    "interface Sink<T> {\n"
    "    public void put(T v);\n"
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

    //0.13.8 item 8 (D8.3): a dotted descriptor head must belong to the
    //image's known package set at intern time — self modulePath, live
    //SymbolImport.modulePath slots, or the nlink-populated merged-unit
    //package paths. Anything else is a corrupt image and throws, and so
    //does an unresolvable leaf (the old silent rowRef -1 is gone). The
    //executor face: InitDescriptorTable interns every row at Execute, so
    //a poisoned row refuses before main runs.
    void qualifiedHeadValidatedAtIntern()
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            nlang::test::ScratchDir("nlang_descriptor_head_validation");
        const fs::path out = dir / "out";
        std::error_code fsError;
        fs::remove_all(dir, fsError);
        fs::create_directories(out, fsError);
        QVERIFY2(writeFile(dir / "a.n", szPipeLib),
            "lib a source could not be written");

        auto buildUnit = [&dir, &out](const char* szSourceName,
            const char* szModuleName, const char* szBody,
            bool bImportLibs, QString& errorsText)
            -> ModuleBuilder::UnitImages
        {
            const fs::path src =
                dir / (std::string(szSourceName) + ".n");
            if (!writeFile(src, szBody))
                return {};
            BuildParams params;
            params.m_SourceFiles.push_back(src.string());
            params.m_sOutputModule = szModuleName;
            params.m_sOutputDir = out.string();
            params.m_sTempDir = out.string();
            params.m_sStdLibDir = STDLIB_DIR;
            //The import arm needs the live-slot shape: -I resolves the
            //imported package's .ncu stub (no sibling source-inline).
            if (bImportLibs)
                params.m_ImportDirs.push_back(out.string());
            MemLogger logger;
            ModuleBuilder builder(params, logger);
            ModuleBuilder::UnitImages imgs;
            bool built = false;
            try { imgs = builder.BuildUnitImages(); built = true; }
            catch (const std::exception&) {}
            for (const auto& e : logger.errorsText())
                errorsText += QString::fromStdString(e) + " | ";
            if (!built || !logger.errorsText().empty())
                return {};
            return imgs;
        };
        //The lib's .ncu must exist under out/ before main_a compiles —
        //the -I layer resolves package imports against written artifacts.
        {
            BuildParams params;
            params.m_SourceFiles.push_back((dir / "a.n").string());
            params.m_sOutputModule = "a";
            params.m_sOutputDir = out.string();
            params.m_sTempDir = out.string();
            params.m_sStdLibDir = STDLIB_DIR;
            MemLogger logger;
            ModuleBuilder builder(params, logger);
            bool built = false;
            try { built = builder.BuildArtifacts(); }
            catch (const std::exception&) {}
            QString artifactsErrors;
            for (const auto& e : logger.errorsText())
                artifactsErrors += QString::fromStdString(e) + " | ";
            QVERIFY2(built && logger.errorsText().empty(),
                qPrintable("lib a must artifact clean — " + artifactsErrors));
        }

        auto internsClean = [](const char* szSpelling,
            const CompiledModule& module) -> bool
        {
            try
            {
                InternDescriptorFromSpelling(szSpelling, module);
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        };
        auto internsThrowing = [](const char* szSpelling,
            const CompiledModule& module,
            QString& what) -> bool
        {
            try
            {
                InternDescriptorFromSpelling(szSpelling, module);
                return false;
            }
            catch (const std::exception& e)
            {
                what = QString::fromLatin1(e.what());
                return true;
            }
        };

        //The import arm: the UNLINKED main unit still carries its live
        //import slots and has an empty merged-unit set — only
        //SymbolImport.modulePath can admit the head.
        std::string bodyOfA = szMainBody;
        {
            const size_t at = bodyOfA.find("%PKG%");
            if (at != std::string::npos)
                bodyOfA.replace(at, 5, "a");
        }
        QString errors;
        ModuleBuilder::UnitImages imgsMainA = buildUnit("main_a",
            "main_a", bodyOfA.c_str(), true, errors);
        QVERIFY2(!imgsMainA.units.empty(),
            qPrintable("main_a must build clean — " + errors));
        QVERIFY2(internsClean("a.Pipe<int>", imgsMainA.units[0]),
            "the import arm must admit the imported package's head");

        //The self arm: an entry unit declaring its own generic — the
        //spelling carries the unit's own package.
        const char* szAppBody =
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(T v);\n"
            "}\n"
            "interface Shape {\n"
            "    public int area();\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    Box<Shape> bs = Box();\n"
            "    return b.f(1);\n"
            "}\n";
        QString errorsApp;
        ModuleBuilder::UnitImages imgsApp = buildUnit("app", "app",
            szAppBody, false, errorsApp);
        QVERIFY2(!imgsApp.units.empty(),
            qPrintable("app must build clean — " + errorsApp));
        CompiledModule mergedApp = NcuLinker::Link(
            std::move(imgsApp.units), "app.main");
        QVERIFY2(internsClean("app.Box<int>", mergedApp),
            "the self arm must admit the unit's own package head");

        //Interface leaves admit through the interfaces table (KeyOf
        //exact — the registry package path both sides), rowless:
        //consumers are class-row scoped (arg kind model v2, roadmap).
        QVERIFY2(internsClean("app.Box<app.Shape>", mergedApp),
            qPrintable("an interface leaf must admit — rows: "
                + spellingDump(mergedApp)));

        //The merged arm: link main + lib a. The merged image keeps NO
        //import records and its self path is main_a — only the
        //nlink-populated merged-unit set can admit "a".
        ModuleBuilder::UnitImages imgsLibA = buildUnit("a", "a",
            szPipeLib, false, errorsApp);
        QVERIFY2(!imgsLibA.units.empty(),
            qPrintable("lib a must build clean — " + errorsApp));
        std::vector<CompiledModule> unitsMainFirst;
        unitsMainFirst.push_back(std::move(imgsMainA.units[0]));
        unitsMainFirst.push_back(std::move(imgsLibA.units[0]));
        CompiledModule merged = NcuLinker::Link(
            std::move(unitsMainFirst), "main_a.main");
        QVERIFY2(merged.functionImports.empty()
            && merged.classImports.empty(),
            "the merged image must keep no import records");
        QVERIFY2(internsClean("a.Pipe<int>", merged),
            "the merged arm must admit a linked unit's package head");

        //Corrupt-image faces: an unknown head (the dotted strip makes it
        //resolve onto the shared erasure row today — the silent wrong
        //identity), an unresolvable leaf (silent rowRef -1 today) and a
        //nested unknown template all refuse with the corrupt-image
        //wording.
        QString what;
        QVERIFY2(internsThrowing("ghost.Pipe<int>", merged, what),
            "an unknown head must refuse — it silently interned today");
        QVERIFY2(what.contains("corrupt"),
            qPrintable("the refusal must name the corrupt-image family — "
                + what));
        QVERIFY2(internsThrowing("a.Pipe<ghost.Foo>", merged, what),
            "an unresolvable leaf must refuse — it stayed rowRef -1 today");
        QVERIFY2(internsThrowing("a.Pipe<ghost.Pipe<int>>", merged, what),
            "a nested unknown template must refuse — it silently interned "
                "today");

        //Bare prelude spellings keep the bare arm (no head to validate).
        QVERIFY2(internsClean("List<int>", merged),
            "bare prelude spellings must keep interning");

        //The func builtin template is the one legitimate row-less nested
        //template: a keyword type has no class row and its handle never
        //walks a super chain (the func_ref_list_store e2e family).
        QVERIFY2(internsClean("List<func<int,int>>", merged),
            "the row-less func nested template must keep interning");

        //Interface families admit exactly (KeyOf spellings, no erasure
        //bridge): a packaged interface as leaf, and a generic interface
        //instantiation as nested template head.
        QVERIFY2(internsClean("a.Pipe<a.Sink>", merged),
            "a packaged interface leaf must admit exactly");
        QVERIFY2(internsClean("a.Sink<a.Pipe<int>>", merged),
            "a generic interface instantiation must admit as a nested "
                "template head");

        //The executor face: a poisoned descriptor row refuses at
        //InitDescriptorTable, before main runs (first use is executor
        //init, not load).
        merged.descriptorTable.push_back(
            DescriptorEntry{"ghost.Pipe<int>", {}});
        VmExecutor exec;
        bool threw = false;
        try
        {
            exec.Execute(merged);
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        QVERIFY2(threw,
            "executor init must refuse a poisoned row before main runs");
    }

    //D8.4(b): the descriptor table survives the wire verbatim —
    //qualified spellings, nested spellings, the row-less func nested
    //face and interface leaves all reload byte-identical, and the
    //runtime-only merged-unit package-path set stays empty on the
    //loaded side (the field is a linker product, never serialized).
    void descriptorTableRoundTripsVerbatim()
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            nlang::test::ScratchDir("nlang_descriptor_round_trip");
        std::error_code fsError;
        fs::remove_all(dir, fsError);
        fs::create_directories(dir);

        CompiledModule mod;
        mod.modulePath = "pkg";
        mod.name = "pkg";
        mod.descriptorTable.push_back(
            DescriptorEntry{"pkg.Pipe<int>", {}});
        mod.descriptorTable.push_back(
            DescriptorEntry{"pkg.Box<pkg.Pipe<int>>", {}});
        mod.descriptorTable.push_back(
            DescriptorEntry{"List<func<int,int>>", {}});
        mod.descriptorTable.push_back(
            DescriptorEntry{"pkg.Box<pkg.Shape>", {}});
        //A composition row: the spelling is empty, the record bytes are
        //opaque at this layer — only the verbatim reload is pinned.
        mod.descriptorTable.push_back(
            DescriptorEntry{"", {0x03, 'p', 'k', 'g', 0x02, 'a'}});

        const fs::path artifact = dir / "pkg.ncu";
        {
            std::ofstream out(artifact, std::ios::binary);
            QVERIFY2(WriteCompiledModule(out, mod, ""),
                "the hand-built image must serialize");
        }
        NcuLoader::Options opts;
        opts.searchDirs.push_back(dir.string());
        NcuLoader::Result loaded =
            NcuLoader::LoadClosure(artifact.string(), opts);
        QVERIFY2(loaded.units.size() == 1, "the entry unit must load");
        const CompiledModule& back = loaded.units[0];
        QVERIFY2(back.descriptorTable.size() == mod.descriptorTable.size(),
            "the descriptor table must reload with every row");
        for (size_t i = 0; i < mod.descriptorTable.size(); ++i)
        {
            QVERIFY2(back.descriptorTable[i].spelling ==
                    mod.descriptorTable[i].spelling,
                qPrintable(QString::fromStdString(
                    "spelling row " + std::to_string(i)
                        + " must round-trip verbatim")));
            QVERIFY2(back.descriptorTable[i].composition ==
                    mod.descriptorTable[i].composition,
                qPrintable(QString::fromStdString(
                    "composition row " + std::to_string(i)
                        + " must round-trip verbatim")));
        }
        QVERIFY2(back.mergedUnitPackagePaths.empty(),
            "the runtime-only merged-unit set must never serialize");
    }
};

#include "test_descriptor_spelling.moc"
QTEST_GUILESS_MAIN(TestDescriptorSpelling)
