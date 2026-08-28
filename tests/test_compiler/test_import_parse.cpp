/*---
test_import_parse.cpp - import syntax unit tests.

In-process ModuleBuilder coverage of the identifier-path / wildcard
import grammar: acceptance of `import a;` `import a.b;` `import a.*;`,
and rejection of the removed string form and of a '*' that is not the
last path segment. Grammar-level assertions only — the import
visibility gating is a later task in the same plan.
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory; errors() drives the assertions.
//The overridable hook of CompileLogger is WriteLog; Errors() counts on
//the base-class side.
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
        std::filesystem::temp_directory_path() / "nlang_import_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//Compile one source file; returns (build ok, error texts).
std::pair<bool, std::vector<std::string>> compileSource(
    const std::string& tag, const std::string& path)
{
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_import_tests";
    BuildParams params;
    params.m_SourceFiles.push_back(path);
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool ok = false;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { ok = builder.Build(); }
    catch (const std::exception&) { ok = false; }
    return {ok, logger.errorsText()};
}

bool anyErrorMentions(const std::vector<std::string>& errors,
    const char* szNeedle)
{
    for (const auto& error : errors)
    {
        if (error.find(szNeedle) != std::string::npos)
            return true;
    }
    return false;
}

} //namespace

class TestImportParse : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() { Runtime::StaticInit(); }

    void parseIdentifierImport()
    {
        const auto path = writeTempSource("identifier.n",
            "import io;\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_id", path);
        //Grammar-level acceptance only (gating is a later task; the
        //loader skips builtin names so the build really succeeds).
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "import io; must parse");
        QVERIFY2(outcome.first, "import io; must build clean");
    }

    void parseDottedPathImport()
    {
        const auto path = writeTempSource("dotted.n",
            "import utils.helper;\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_dotted", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "import utils.helper; must parse");
    }

    void parseWildcardImport()
    {
        const auto path = writeTempSource("wildcard.n",
            "import utils.*;\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_wild", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "import utils.*; must parse");
    }

    void rejectStringImport()
    {
        const auto path = writeTempSource("string_form.n",
            "import \"lib\";\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_str", path);
        QVERIFY2(!outcome.first, "string import must not compile");
        //The production stays so the diagnostic names the mistake
        //instead of a generic bison syntax error.
        QVERIFY2(anyErrorMentions(outcome.second,
            "String import is removed"),
            "string import must get the removal diagnostic");
    }

    void rejectWildcardNotLast()
    {
        //'*' is only allowed as the last path segment.
        const auto path = writeTempSource("wild_mid.n",
            "import utils.*.helper;\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_wmid", path);
        QVERIFY2(anyErrorMentions(outcome.second, "syntax error"),
            "mid-path '*' must be a syntax error");
    }

    void rejectDoubleWildcard()
    {
        const auto path = writeTempSource("wild_double.n",
            "import utils.*.*;\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("import_parse_wdbl", path);
        QVERIFY2(anyErrorMentions(outcome.second, "syntax error"),
            "double '*' must be a syntax error");
    }
};

QTEST_GUILESS_MAIN(TestImportParse)
#include "test_import_parse.moc"
