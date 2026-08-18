/*--- test_projectmodel.cpp - ProjectModel unit tests (Qt Test) ---*/
#include "../../../src/tools/nide/ProjectModel.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace nlang;

class TestProjectModel : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmpDir;

    // Helper: write a text file under the temp dir, return absolute path.
    QString writeFixture(const QString& relPath, const QString& content) {
        QString absPath = m_tmpDir.path() + "/" + relPath;
        QFileInfo fi(absPath);
        QDir().mkpath(fi.absolutePath());
        QFile f(absPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << content;
        }
        return absPath;
    }

    // Fixture: a project file with two sources.
    QString projectXml(const QString& name) {
        return QString(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"%1\">\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"util.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n").arg(name);
    }

    // Fixture: a solution referencing the given project paths.
    QString solutionXml(const QString& name, const QStringList& paths) {
        QString entries;
        for (const QString& p : paths)
            entries += QString("    <Project path=\"%1\"/>\n").arg(p);
        return QString(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"%1\">\n"
            "  <Projects>\n"
            "%2"
            "  </Projects>\n"
            "</Solution>\n").arg(name, entries);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmpDir.isValid());
    }

    // --- FileNode ---

    void testFileNodeBasic() {
        ProjectNode proj("MyProj", m_tmpDir.path());
        FileNode file("main.n", &proj);

        QCOMPARE(file.absolutePath(), QString("main.n"));
        QCOMPARE(file.project(), &proj);
    }

    void testFileNodeNoProject() {
        FileNode file("test.n", nullptr);
        QCOMPARE(file.absolutePath(), QString("test.n"));
        QCOMPARE(file.project(), (ProjectNode*)nullptr);
    }

    // --- ProjectNode ---

    void testProjectNodeBasic() {
        ProjectNode proj("Hello", m_tmpDir.path());

        QCOMPARE(proj.name(), QString("Hello"));
        QCOMPARE(proj.projectDir(), m_tmpDir.path());
        QCOMPARE(proj.namespace_(), QString());
        QCOMPARE(proj.outputDir(), QString());
        QCOMPARE(proj.intermediateDir(), QString());
        QVERIFY(!proj.isDirty());
    }

    void testProjectNodeSetProperties() {
        ProjectNode proj("Hello", m_tmpDir.path());
        proj.setNamespace("hello");
        proj.setOutputDir("bin");
        proj.setIntermediateDir("obj");

        QCOMPARE(proj.namespace_(), QString("hello"));
        QCOMPARE(proj.outputDir(), QString("bin"));
        QCOMPARE(proj.intermediateDir(), QString("obj"));
        QVERIFY(proj.isDirty());
    }

    void testProjectNodeAddFiles() {
        ProjectNode proj("Hello", m_tmpDir.path());

        FileNode* f1 = proj.addFile("main.n");
        FileNode* f2 = proj.addFile("utils.n");

        QVERIFY(f1 != nullptr);
        QVERIFY(f2 != nullptr);
        // addFile resolves relative paths against the project directory.
        QDir projDir(m_tmpDir.path());
        QCOMPARE(f1->absolutePath(), projDir.absoluteFilePath("main.n"));
        QCOMPARE(f2->absolutePath(), projDir.absoluteFilePath("utils.n"));
        QCOMPARE(f1->project(), &proj);
        QCOMPARE(f2->project(), &proj);
        QCOMPARE(proj.fileCount(), 2);
        QVERIFY(proj.isDirty());
    }

    void testProjectNodeRemoveFile() {
        ProjectNode proj("Hello", m_tmpDir.path());
        FileNode* f1 = proj.addFile("main.n");
        FileNode* f2 = proj.addFile("utils.n");

        QVERIFY(proj.removeFile(f1));
        QCOMPARE(proj.fileCount(), 1);
        // f1 is deleted, cannot access anymore
    }

    void testProjectNodeRemoveFileNotOwned() {
        ProjectNode proj1("P1", m_tmpDir.path());
        ProjectNode proj2("P2", m_tmpDir.path());
        FileNode* f = proj1.addFile("main.n");

        QVERIFY(!proj2.removeFile(f));
        QCOMPARE(proj1.fileCount(), 1);
    }

    void testProjectNodeFiles() {
        ProjectNode proj("Hello", m_tmpDir.path());
        proj.addFile("a.n");
        proj.addFile("b.n");
        proj.addFile("c.n");

        const auto& files = proj.files();
        QCOMPARE(files.size(), 3);
        QDir projDir(m_tmpDir.path());
        QCOMPARE(files[0]->absolutePath(), projDir.absoluteFilePath("a.n"));
        QCOMPARE(files[1]->absolutePath(), projDir.absoluteFilePath("b.n"));
        QCOMPARE(files[2]->absolutePath(), projDir.absoluteFilePath("c.n"));
    }

    void testProjectNodeAbsolutePathOf() {
        ProjectNode proj("Hello", m_tmpDir.path());
        QString abs = proj.absolutePathOf("sub/utils.n");
        QDir projDir(m_tmpDir.path());
        QCOMPARE(abs, projDir.absoluteFilePath("sub/utils.n"));
    }

    void testProjectNodeSaveAndLoad() {
        // Write a project, save it, then load it back and compare.
        QString projPath = m_tmpDir.path() + "/testproj.nproj";

        // Create and save
        {
            ProjectNode proj("HelloApp", m_tmpDir.path());
            proj.setNamespace("hello");
            proj.setOutputDir("bin");
            proj.setIntermediateDir("obj");
            proj.addFile("main.n");
            proj.addFile("sub/utils.n");

            QString error;
            QVERIFY(proj.save(projPath, &error));
        }

        // Load and verify
        {
            ProjectNode proj("", m_tmpDir.path());
            QString error;
            QVERIFY(proj.load(projPath, &error));

            QCOMPARE(proj.name(), QString("HelloApp"));
            QCOMPARE(proj.namespace_(), QString("hello"));
            QCOMPARE(proj.outputDir(), QString("bin"));
            QCOMPARE(proj.intermediateDir(), QString("obj"));
            QCOMPARE(proj.fileCount(), 2);
            QCOMPARE(proj.files()[0]->absolutePath(),
                     proj.absolutePathOf("main.n"));
            QCOMPARE(proj.files()[1]->absolutePath(),
                     proj.absolutePathOf("sub/utils.n"));
        }
    }

    void testProjectNodeLoadMissingFile() {
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load("/nonexistent/path.nproj", &error));
        QVERIFY(!error.isEmpty());
    }

    void testProjectNodeLoadMalformedXml() {
        QString projPath = writeFixture("broken.nproj",
            "<Project name='x'><Sources>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY(!error.isEmpty());
    }

    void testProjectNodeLoadWrongRoot() {
        QString projPath = writeFixture("wrongroot.nproj",
            "<?xml version=\"1.0\"?>\n<Solution name=\"s\"/>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY(!error.isEmpty());
    }

    void testProjectNodeLoadEmptySources() {
        QString projPath = writeFixture("empty.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"Empty\">\n  <Sources/>\n</Project>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        // An empty-but-present <Sources> must be reported as such, not as
        // a missing <Sources> element.
        QVERIFY2(error.contains("no source files"), qPrintable(error));
    }

    void testProjectNodeLoadFileWithoutPath() {
        QString projPath = writeFixture("noattr.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"X\">\n  <Sources>\n    <File/>\n  </Sources>\n</Project>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY2(error.contains("without a path"), qPrintable(error));
    }

    void testProjectNodeLoadNameDefaultsToStem() {
        QString projPath = writeFixture("mystem.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project>\n  <Sources><File path=\"main.n\"/></Sources>\n</Project>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(projPath, &error));
        QCOMPARE(proj.name(), QString("mystem"));
    }

    void testProjectNodeDirtyTracking() {
        ProjectNode proj("Hello", m_tmpDir.path());
        QVERIFY(!proj.isDirty());

        proj.setNamespace("hello");
        QVERIFY(proj.isDirty());

        proj.clearDirty();
        QVERIFY(!proj.isDirty());

        proj.addFile("main.n");
        QVERIFY(proj.isDirty());

        proj.clearDirty();
        proj.setOutputDir("bin");
        QVERIFY(proj.isDirty());
    }

    void testProjectNodeRoundTripPreservesContent() {
        // Create a project, save, load into a new project, save again,
        // and compare the two XML files byte-for-byte.
        QString path1 = m_tmpDir.path() + "/round1.nproj";
        QString path2 = m_tmpDir.path() + "/round2.nproj";

        {
            ProjectNode proj("RoundTrip", m_tmpDir.path());
            proj.setNamespace("rt");
            proj.setOutputDir("out");
            proj.setIntermediateDir("tmp");
            proj.addFile("a.n");
            proj.addFile("b/c.n");
            QString error;
            QVERIFY(proj.save(path1, &error));
        }

        {
            ProjectNode proj("", m_tmpDir.path());
            QString error;
            QVERIFY(proj.load(path1, &error));
            QVERIFY(proj.save(path2, &error));
        }

        // Compare file contents
        QFile f1(path1), f2(path2);
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f1.readAll(), f2.readAll());
    }

    // --- SolutionNode ---

    void testSolutionNodeBasic() {
        SolutionNode sol("MySolution");

        QCOMPARE(sol.name(), QString("MySolution"));
        QCOMPARE(sol.projectCount(), 0);
        QVERIFY(!sol.isDirty());
    }

    void testSolutionNodeAddProject() {
        SolutionNode sol("MySolution");
        ProjectNode* p = sol.addProject("app/app.nproj");

        QVERIFY(p != nullptr);
        QCOMPARE(sol.projectCount(), 1);
        QCOMPARE(p->name(), QString("app"));  // name defaults to stem
        QVERIFY(sol.isDirty());
    }

    void testSolutionNodeRemoveProject() {
        SolutionNode sol("MySolution");
        ProjectNode* p = sol.addProject("app/app.nproj");

        QVERIFY(sol.removeProject(p));
        QCOMPARE(sol.projectCount(), 0);
    }

    void testSolutionNodeProjects() {
        SolutionNode sol("MySolution");
        sol.addProject("app/app.nproj");
        sol.addProject("lib/lib.nproj");

        const auto& projects = sol.projects();
        QCOMPARE(projects.size(), 2);
    }

    void testSolutionNodeSaveAndLoad() {
        QString solPath = m_tmpDir.path() + "/test.nsln";

        // Create subdirectories and project files
        writeFixture("app/app.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\" namespace=\"app\" outputDir=\"bin\">\n"
            "  <Sources><File path=\"main.n\"/></Sources>\n"
            "</Project>\n");
        writeFixture("lib/lib.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Lib\" namespace=\"lib\" outputDir=\"bin\">\n"
            "  <Sources><File path=\"lib.n\"/></Sources>\n"
            "</Project>\n");

        // Save solution
        {
            SolutionNode sol("TestSolution");
            sol.addProject("app/app.nproj");
            sol.addProject("lib/lib.nproj");

            QString error;
            QVERIFY(sol.save(solPath, &error));
        }

        // Load and verify
        {
            SolutionNode sol("");
            QString error;
            QVERIFY(sol.load(solPath, &error));

            QCOMPARE(sol.name(), QString("TestSolution"));
            QCOMPARE(sol.projectCount(), 2);
            // projectPath() returns paths relative to the solution dir.
            QCOMPARE(sol.projectPath(0), QString("app/app.nproj"));
            QCOMPARE(sol.projectPath(1), QString("lib/lib.nproj"));
        }
    }

    void testSolutionNodeLoadMissingFile() {
        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load("/nonexistent/solution.nsln", &error));
        QVERIFY(!error.isEmpty());
    }

    void testSolutionNodeLoadMalformedXml() {
        QString solPath = writeFixture("broken.nsln",
            "<Solution name='x'>\n");
        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load(solPath, &error));
        QVERIFY(!error.isEmpty());
    }

    void testSolutionNodeLoadWrongRoot() {
        QString solPath = writeFixture("wrongroot.nsln",
            "<?xml version=\"1.0\"?>\n<Project name=\"p\"/>\n");
        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load(solPath, &error));
        QVERIFY(!error.isEmpty());
    }

    void testSolutionNodeRoundTrip() {
        QString path1 = m_tmpDir.path() + "/round1.nsln";
        QString path2 = m_tmpDir.path() + "/round2.nsln";

        {
            SolutionNode sol("RoundSol");
            sol.addProject("app/app.nproj");
            sol.addProject("lib/lib.nproj");
            QString error;
            QVERIFY(sol.save(path1, &error));
        }

        {
            SolutionNode sol("");
            QString error;
            QVERIFY(sol.load(path1, &error));
            QCOMPARE(sol.name(), QString("RoundSol"));
            QCOMPARE(sol.projectCount(), 2);
            QVERIFY(sol.save(path2, &error));
        }

        QFile f1(path1), f2(path2);
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f1.readAll(), f2.readAll());
    }

    void testSolutionNodeDirtyTracking() {
        SolutionNode sol("MySolution");
        QVERIFY(!sol.isDirty());

        sol.addProject("app/app.nproj");
        QVERIFY(sol.isDirty());

        sol.clearDirty();
        QVERIFY(!sol.isDirty());

        sol.setName("NewName");
        QVERIFY(sol.isDirty());
    }

    void testSolutionNodeSetName() {
        SolutionNode sol("Old");
        QCOMPARE(sol.name(), QString("Old"));
        sol.setName("New");
        QCOMPARE(sol.name(), QString("New"));
    }

    void testProjectNodeSetName() {
        ProjectNode proj("Old", m_tmpDir.path());
        QCOMPARE(proj.name(), QString("Old"));
        proj.setName("New");
        QCOMPARE(proj.name(), QString("New"));
        QVERIFY(proj.isDirty());
    }

    // Verify that the saved XML matches the expected format exactly.
    void testProjectNodeXmlFormat() {
        QString projPath = m_tmpDir.path() + "/format.nproj";

        ProjectNode proj("Hello", m_tmpDir.path());
        proj.setNamespace("hello");
        proj.setOutputDir("bin");
        proj.setIntermediateDir("obj");
        proj.addFile("main.n");
        proj.addFile("utils.n");

        QString error;
        QVERIFY(proj.save(projPath, &error));

        // Read back and verify structure
        QFile f(projPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = QTextStream(&f).readAll();

        // Must contain the expected XML declaration
        QVERIFY(content.contains("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
        // Must have the root element with all attributes
        QVERIFY(content.contains("<Project name=\"Hello\" namespace=\"hello\" outputDir=\"bin\" intermediateDir=\"obj\">"));
        // Must have Sources wrapper
        QVERIFY(content.contains("<Sources>"));
        // Must have File entries with path attributes
        QVERIFY(content.contains("<File path=\"main.n\"/>"));
        QVERIFY(content.contains("<File path=\"utils.n\"/>"));
    }

    void testSolutionNodeXmlFormat() {
        QString solPath = m_tmpDir.path() + "/format.nsln";

        SolutionNode sol("MySolution");
        sol.addProject("app/app.nproj");
        sol.addProject("lib/lib.nproj");

        QString error;
        QVERIFY(sol.save(solPath, &error));

        QFile f(solPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = QTextStream(&f).readAll();

        QVERIFY(content.contains("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
        QVERIFY(content.contains("<Solution name=\"MySolution\">"));
        QVERIFY(content.contains("<Projects>"));
        QVERIFY(content.contains("<Project path=\"app/app.nproj\"/>"));
        QVERIFY(content.contains("<Project path=\"lib/lib.nproj\"/>"));
    }

    // Verify that loading a .nproj with namespace/intermediateDir attributes
    // works (these are IDE-facing, ncc ignores them).
    void testProjectNodeLoadWithAllAttributes() {
        QString projPath = writeFixture("full.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Full\" namespace=\"full\" outputDir=\"out\" intermediateDir=\"tmp\">\n"
            "  <Sources><File path=\"main.n\"/></Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(projPath, &error));

        QCOMPARE(proj.name(), QString("Full"));
        QCOMPARE(proj.namespace_(), QString("full"));
        QCOMPARE(proj.outputDir(), QString("out"));
        QCOMPARE(proj.intermediateDir(), QString("tmp"));
        QCOMPARE(proj.fileCount(), 1);
    }

    // Verify that loading a .nproj without optional attributes works.
    void testProjectNodeLoadMinimalAttributes() {
        QString projPath = writeFixture("minimal.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Min\">\n"
            "  <Sources><File path=\"main.n\"/></Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(projPath, &error));

        QCOMPARE(proj.name(), QString("Min"));
        QCOMPARE(proj.namespace_(), QString());
        QCOMPARE(proj.outputDir(), QString());
        QCOMPARE(proj.intermediateDir(), QString());
    }

    // Verify that duplicate file entries are rejected on load.
    void testProjectNodeLoadDuplicateFiles() {
        QString projPath = writeFixture("dup.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Dup\">\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"main.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY(!error.isEmpty());
    }

    // Verify that adding a duplicate file to a project is rejected.
    void testProjectNodeAddDuplicateFile() {
        ProjectNode proj("Hello", m_tmpDir.path());
        FileNode* f1 = proj.addFile("main.n");
        FileNode* f2 = proj.addFile("main.n");

        QVERIFY(f1 != nullptr);
        QCOMPARE(f2, (FileNode*)nullptr);  // duplicate rejected
        QCOMPARE(proj.fileCount(), 1);
    }

    // Verify that adding a duplicate project to a solution is rejected.
    void testSolutionNodeAddDuplicateProject() {
        SolutionNode sol("MySolution");
        ProjectNode* p1 = sol.addProject("app/app.nproj");
        ProjectNode* p2 = sol.addProject("app/app.nproj");

        QVERIFY(p1 != nullptr);
        QCOMPARE(p2, (ProjectNode*)nullptr);  // duplicate rejected
        QCOMPARE(sol.projectCount(), 1);
    }

    // Verify projectPath() accessor.
    void testSolutionNodeProjectPath() {
        SolutionNode sol("MySolution");
        sol.addProject("app/app.nproj");
        sol.addProject("lib/lib.nproj");

        QCOMPARE(sol.projectPath(0), QString("app/app.nproj"));
        QCOMPARE(sol.projectPath(1), QString("lib/lib.nproj"));
    }

    // --- Normalized duplicate detection (same file, different spelling) ---

    // The same file with different case must collide (Windows NTFS
    // semantics -- mirrors ncc's SourceKey case folding).
    void testProjectNodeLoadDuplicateFilesCaseVariant() {
        QString projPath = writeFixture("dupcase.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Dup\">\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"MAIN.N\"/>\n"
            "  </Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY2(error.contains("duplicate"), qPrintable(error));
    }

    // "sub/../main.n" and "main.n" name the same file.
    void testProjectNodeLoadDuplicateFilesDotDot() {
        QString projPath = writeFixture("dupdot.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Dup\">\n"
            "  <Sources>\n"
            "    <File path=\"sub/../main.n\"/>\n"
            "    <File path=\"main.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY2(error.contains("duplicate"), qPrintable(error));
    }

    // addFile() duplicates must collide after normalization too.
    void testProjectNodeAddDuplicateFileNormalized() {
        ProjectNode proj("Hello", m_tmpDir.path());
        QVERIFY(proj.addFile("main.n") != nullptr);
        QCOMPARE(proj.addFile("./main.n"), (FileNode*)nullptr);
        QCOMPARE(proj.fileCount(), 1);
    }

    // --- All-or-nothing load ---

    // A second successful load must replace, not append to, prior state.
    void testProjectNodeLoadTwiceReplacesState() {
        writeFixture("one.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"One\">\n"
            "  <Sources><File path=\"a.n\"/></Sources>\n"
            "</Project>\n");
        writeFixture("two.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Two\">\n"
            "  <Sources><File path=\"b.n\"/><File path=\"c.n\"/></Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(m_tmpDir.path() + "/one.nproj", &error));
        QCOMPARE(proj.fileCount(), 1);

        QVERIFY(proj.load(m_tmpDir.path() + "/two.nproj", &error));
        QCOMPARE(proj.fileCount(), 2);  // replaced, not appended
        QCOMPARE(proj.name(), QString("Two"));
        QCOMPARE(proj.files()[0]->absolutePath(), proj.absolutePathOf("b.n"));
    }

    // A failed load must leave previously loaded state untouched.
    void testProjectNodeFailedLoadKeepsState() {
        writeFixture("good.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Good\">\n"
            "  <Sources><File path=\"a.n\"/></Sources>\n"
            "</Project>\n");
        writeFixture("bad.nproj", "<Project name='x'>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(m_tmpDir.path() + "/good.nproj", &error));
        QVERIFY(!proj.load(m_tmpDir.path() + "/bad.nproj", &error));

        QCOMPARE(proj.name(), QString("Good"));
        QCOMPARE(proj.fileCount(), 1);
        QCOMPARE(proj.files()[0]->absolutePath(), proj.absolutePathOf("a.n"));
    }

    // --- Save As relocates the project directory ---

    // Saving into a different directory must rebase the relative source
    // paths on the save target, and the reloaded project must still point
    // at the original physical files.
    void testProjectNodeSaveAsRebasesRelativePaths() {
        QString dirA = m_tmpDir.path() + "/orig";
        QString dirB = m_tmpDir.path() + "/moved";
        QDir().mkpath(dirA);
        QDir().mkpath(dirB);
        writeFixture("orig/main.n", "int x = 1;\n");

        ProjectNode proj("P", dirA);
        proj.addFile("main.n");
        QString error;
        QVERIFY(proj.save(dirB + "/p.nproj", &error));
        QCOMPARE(proj.projectDir(), dirB);  // save target becomes projectDir

        ProjectNode reloaded("", "");
        QVERIFY(reloaded.load(dirB + "/p.nproj", &error));
        QCOMPARE(reloaded.projectDir(), dirB);
        QCOMPARE(reloaded.fileCount(), 1);
        QCOMPARE(reloaded.files()[0]->absolutePath(),
                 QDir(dirA).absoluteFilePath("main.n"));
    }

    // Save As on a solution must re-relativize the project references so
    // the moved .nsln still reaches the original project files.
    void testSolutionNodeSaveAsKeepsProjectsReachable() {
        writeFixture("app/app.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\">\n"
            "  <Sources><File path=\"main.n\"/></Sources>\n"
            "</Project>\n");
        writeFixture("root.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects><Project path=\"app/app.nproj\"/></Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(sol.load(m_tmpDir.path() + "/root.nsln", &error));

        QString dirB = m_tmpDir.path() + "/elsewhere";
        QDir().mkpath(dirB);
        QVERIFY(sol.save(dirB + "/moved.nsln", &error));

        SolutionNode reloaded("");
        QVERIFY(reloaded.load(dirB + "/moved.nsln", &error));
        QCOMPARE(reloaded.projectCount(), 1);
        QCOMPARE(reloaded.projects()[0]->projectDir(),
                 m_tmpDir.path() + "/app");
    }

    // --- Schema strictness ---

    // A second <Sources> block would have its files silently dropped --
    // reject it (same rule as ncc).
    void testProjectNodeLoadMultipleSourcesRejected() {
        QString projPath = writeFixture("multisources.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"M\">\n"
            "  <Sources><File path=\"a.n\"/></Sources>\n"
            "  <Sources><File path=\"b.n\"/></Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY2(error.contains("more than one"), qPrintable(error));
    }

    // Duplicate project entries in a .nsln are a user mistake -- reject.
    void testSolutionNodeLoadDuplicateProjects() {
        writeFixture("app/app.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\"><Sources><File path=\"main.n\"/></Sources></Project>\n");
        QString solPath = writeFixture("dupsol.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects>\n"
            "    <Project path=\"app/app.nproj\"/>\n"
            "    <Project path=\"app/app.nproj\"/>\n"
            "  </Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load(solPath, &error));
        QVERIFY2(error.contains("duplicate"), qPrintable(error));
    }

    // Case-variant duplicates must collide too (NTFS semantics).
    void testSolutionNodeLoadDuplicateProjectsCaseVariant() {
        writeFixture("app/app.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\"><Sources><File path=\"main.n\"/></Sources></Project>\n");
        QString solPath = writeFixture("dupsolcase.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects>\n"
            "    <Project path=\"app/app.nproj\"/>\n"
            "    <Project path=\"APP/APP.NPROJ\"/>\n"
            "  </Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load(solPath, &error));
        QVERIFY2(error.contains("duplicate"), qPrintable(error));
    }

    // A second <Projects> block in a .nsln is rejected (mirrors the
    // <Sources> rule).
    void testSolutionNodeLoadMultipleProjectsBlocksRejected() {
        QString solPath = writeFixture("multiblock.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects><Project path=\"app/app.nproj\"/></Projects>\n"
            "  <Projects><Project path=\"lib/lib.nproj\"/></Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(!sol.load(solPath, &error));
        QVERIFY2(error.contains("more than one"), qPrintable(error));
    }

    // addProject() duplicates must collide after normalization too.
    void testSolutionNodeAddDuplicateProjectNormalized() {
        SolutionNode sol("S");
        QVERIFY(sol.addProject("app/app.nproj") != nullptr);
        QCOMPARE(sol.addProject("app/./app.nproj"), (ProjectNode*)nullptr);
        QCOMPARE(sol.projectCount(), 1);
    }

    // A failed solution load must leave previously loaded state untouched.
    void testSolutionNodeFailedLoadKeepsState() {
        writeFixture("good.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"Good\">\n"
            "  <Projects><Project path=\"app/app.nproj\"/></Projects>\n"
            "</Solution>\n");
        writeFixture("bad.nsln", "<Solution name='x'>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(sol.load(m_tmpDir.path() + "/good.nsln", &error));
        QVERIFY(!sol.load(m_tmpDir.path() + "/bad.nsln", &error));

        QCOMPARE(sol.name(), QString("Good"));
        QCOMPARE(sol.projectCount(), 1);
    }

    // --- Absolute path storage and name defaults ---

    // addFile() must store an absolute path even when given a relative one.
    void testProjectNodeAddFileStoresAbsolute() {
        ProjectNode proj("Hello", m_tmpDir.path());
        FileNode* f = proj.addFile("sub/main.n");
        QVERIFY(f != nullptr);
        QVERIFY2(QFileInfo(f->absolutePath()).isAbsolute(),
                 qPrintable(f->absolutePath()));
        QCOMPARE(f->absolutePath(),
                 QDir(m_tmpDir.path()).absoluteFilePath("sub/main.n"));
    }

    // "my.app.nproj" defaults the name to "my.app" (full stem, matching
    // ncc's std::filesystem::stem()).
    void testProjectNodeLoadNameDefaultsToFullStem() {
        QString projPath = writeFixture("my.app.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project>\n  <Sources><File path=\"main.n\"/></Sources>\n</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(proj.load(projPath, &error));
        QCOMPARE(proj.name(), QString("my.app"));
    }

    // Project names in a .nsln default to the full stem as well.
    void testSolutionNodeProjectNameWithDots() {
        writeFixture("libs/my.lib.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Lib\"><Sources><File path=\"lib.n\"/></Sources></Project>\n");
        QString solPath = writeFixture("dotsol.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects><Project path=\"libs/my.lib.nproj\"/></Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY(sol.load(solPath, &error));
        QCOMPARE(sol.projectCount(), 1);
        QCOMPARE(sol.projects()[0]->name(), QString("my.lib"));
    }

    // --- save() re-bases still-relative entries (save output reloads) ---

    // A relative entry added before the project had a directory must be
    // absolutized against the save target, so absolutePath() holds what
    // its name promises and the same file by absolute path is still
    // recognized as a duplicate.
    void testProjectNodeSaveRebasesRelativeEntries() {
        ProjectNode proj("P", QString());  // no directory yet
        QVERIFY(proj.addFile("main.n") != nullptr);

        QString error;
        QVERIFY(proj.save(m_tmpDir.path() + "/p.nproj", &error));

        QDir dir(m_tmpDir.path());
        QCOMPARE(proj.fileCount(), 1);
        QCOMPARE(proj.files()[0]->absolutePath(), dir.absoluteFilePath("main.n"));
        QCOMPARE(proj.addFile(dir.absoluteFilePath("main.n")), (FileNode*)nullptr);
    }

    // SolutionNode: project references re-base the same way.
    void testSolutionNodeSaveRebasesRelativeEntries() {
        SolutionNode sol("S");
        QVERIFY(sol.addProject("app/app.nproj") != nullptr);

        QString error;
        QVERIFY(sol.save(m_tmpDir.path() + "/s.nsln", &error));

        QCOMPARE(sol.projectCount(), 1);
        QCOMPARE(sol.addProject(m_tmpDir.path() + "/app/app.nproj"),
                 (ProjectNode*)nullptr);
    }

    // --- save() failure paths ---

    // Saving into a nonexistent directory must fail loudly, keep the
    // dirty flag, and not relocate the project.
    void testProjectNodeSaveMissingDirFails() {
        ProjectNode proj("P", m_tmpDir.path());
        proj.addFile("main.n");

        QString error;
        QVERIFY(!proj.save(m_tmpDir.path() + "/no_such_dir/p.nproj", &error));
        QVERIFY2(!error.isEmpty(), "save must report why it failed");
        QVERIFY(proj.isDirty());
        QCOMPARE(proj.projectDir(), m_tmpDir.path());
    }

    void testSolutionNodeSaveMissingDirFails() {
        SolutionNode sol("S");
        sol.addProject("app/app.nproj");

        QString error;
        QVERIFY(!sol.save(m_tmpDir.path() + "/no_such_dir/s.nsln", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(sol.isDirty());
    }

    // A failed save over an existing file must leave its bytes intact
    // (atomic write: the target is never truncated).
    void testProjectNodeSaveReadOnlyKeepsFile() {
        QString path = m_tmpDir.path() + "/locked.nproj";
        ProjectNode proj("P", m_tmpDir.path());
        proj.addFile("main.n");
        QString error;
        QVERIFY(proj.save(path, &error));  // create the file first
        proj.setNamespace("unsaved");      // make the project dirty again

        QByteArray before;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            before = f.readAll();
        }

        // Make the target unwritable, then attempt to overwrite it.
        QFile::Permissions saved = QFile::permissions(path);
        QFile::setPermissions(path, QFile::Permissions(QFile::ReadOwner | QFile::ReadUser));

        bool saveOk = proj.save(path, &error);
        bool stillDirty = proj.isDirty();
        QByteArray after;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            after = f.readAll();
        }
        QFile::setPermissions(path, saved);  // restore before asserting

        QVERIFY(!saveOk);
        QVERIFY(!error.isEmpty());
        QVERIFY(stillDirty);
        QCOMPARE(after, before);
    }

    // --- Depth-aware element matching (ncc parity) ---

    // A <Sources> nested deeper than a direct child of <Project> is not a
    // <Sources> element for ncc -- the IDE must reject it too.
    void testProjectNodeLoadNestedSourcesRejected() {
        QString projPath = writeFixture("nestedsources.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"N\">\n"
            "  <Foo><Sources><File path=\"a.n\"/></Sources></Foo>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY2(error.contains("no <Sources> element"), qPrintable(error));
    }

    // The project/solution directory stored on load/save is canonical
    // (".." segments folded), so both sites normalize identically.
    void testProjectNodeDirIsCanonical() {
        QDir().mkpath(m_tmpDir.path() + "/sub");
        QString dotdot = m_tmpDir.path() + "/sub/../canon.nproj";

        ProjectNode proj("P", QString());
        proj.addFile("main.n");
        QString error;
        QVERIFY(proj.save(dotdot, &error));
        QCOMPARE(proj.projectDir(), m_tmpDir.path());

        ProjectNode reloaded("", "");
        QVERIFY(reloaded.load(dotdot, &error));
        QCOMPARE(reloaded.projectDir(), m_tmpDir.path());
    }

    // --- Nested same-name wrappers inside the real wrapper (ncc parity) ---

    // A nested empty <Sources/> must not close the real <Sources>:
    // entries before and after it all load (ncc's FirstChildElement
    // iteration accepts both).
    void testProjectNodeLoadNestedEmptySourcesKeepsFiles() {
        QString projPath = writeFixture("nestempty.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"N\">\n"
            "  <Sources>\n"
            "    <File path=\"a.n\"/>\n"
            "    <Foo><Sources/></Foo>\n"
            "    <File path=\"b.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY2(proj.load(projPath, &error), qPrintable(error));
        QCOMPARE(proj.fileCount(), 2);
    }

    // Sibling variant: a <Sources/> directly inside the real <Sources>
    // is ignored, its following <File> still loads.
    void testProjectNodeLoadNestedSourcesSiblingKeepsFiles() {
        QString projPath = writeFixture("nestsib.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"N\">\n"
            "  <Sources><Sources/><File path=\"a.n\"/></Sources>\n"
            "</Project>\n");

        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY2(proj.load(projPath, &error), qPrintable(error));
        QCOMPARE(proj.fileCount(), 1);
    }

    // SolutionNode: a nested empty <Projects/> must not drop the
    // project entries after it.
    void testSolutionNodeLoadNestedEmptyProjectsKeepsEntries() {
        QString solPath = writeFixture("nestnsln.nsln",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"S\">\n"
            "  <Projects>\n"
            "    <Project path=\"a/a.nproj\"/>\n"
            "    <Foo><Projects/></Foo>\n"
            "    <Project path=\"b/b.nproj\"/>\n"
            "  </Projects>\n"
            "</Solution>\n");

        SolutionNode sol("");
        QString error;
        QVERIFY2(sol.load(solPath, &error), qPrintable(error));
        QCOMPARE(sol.projectCount(), 2);
    }

    // --- save() of an empty project ---

    // Saving a project with no files must fail: the written file could
    // never load back (same diagnostic as load/ncc).
    void testProjectNodeSaveEmptyProjectFails() {
        ProjectNode proj("Empty", m_tmpDir.path());
        QString path = m_tmpDir.path() + "/unsavable.nproj";
        QString error;
        QVERIFY(!proj.save(path, &error));
        QVERIFY2(error.contains("no source files"), qPrintable(error));
        QVERIFY(!QFile::exists(path));                 // nothing written
        QCOMPARE(proj.projectDir(), m_tmpDir.path());  // not relocated
    }

    // --- deep persistence (loadWithProjects / saveWithProjects) ---

    void testLoadWithProjectsLoadsProjectContent() {
        writeFixture("deep.nproj", projectXml("App"));
        writeFixture("deep.nsln", solutionXml("Solo", {"deep.nproj"}));

        SolutionNode s("");
        QString err;
        QVERIFY(s.loadWithProjects(m_tmpDir.path() + "/deep.nsln", &err));
        QCOMPARE(s.name(), QString("Solo"));
        QCOMPARE(s.projectCount(), 1);
        QCOMPARE(s.projects()[0]->name(), QString("App"));
        QCOMPARE(s.projects()[0]->fileCount(), 2);
        QCOMPARE(err, QString());   // error is cleared on success
    }

    void testLoadWithProjectsAllOrNothing() {
        writeFixture("ok.nproj", projectXml("App"));
        writeFixture("deep-good.nsln", solutionXml("Good", {"ok.nproj"}));
        writeFixture("deep-bad.nsln", solutionXml("Bad", {"missing.nproj"}));

        SolutionNode s("");
        QVERIFY(s.loadWithProjects(m_tmpDir.path() + "/deep-good.nsln"));

        QString err;
        QVERIFY(!s.loadWithProjects(m_tmpDir.path() + "/deep-bad.nsln", &err));
        QVERIFY(!err.isEmpty());
        //Previous state survives a failed deep load.
        QCOMPARE(s.name(), QString("Good"));
        QCOMPARE(s.projectCount(), 1);
        QCOMPARE(s.projects()[0]->fileCount(), 2);
    }

    void testSaveWithProjectsWritesBothFiles() {
        SolutionNode s("Solo");
        ProjectNode* proj = s.addProject(m_tmpDir.path() + "/gen.nproj");
        QVERIFY(proj != nullptr);
        QVERIFY(proj->addFile(m_tmpDir.path() + "/main.n") != nullptr);

        QString err;
        QVERIFY(s.saveWithProjects(m_tmpDir.path() + "/gen.nsln", &err));
        QVERIFY(QFile::exists(m_tmpDir.path() + "/gen.nproj"));

        SolutionNode reloaded("");
        QVERIFY(reloaded.loadWithProjects(m_tmpDir.path() + "/gen.nsln", &err));
        QCOMPARE(reloaded.projectCount(), 1);
        QCOMPARE(reloaded.projects()[0]->fileCount(), 1);
    }

    void testSaveWithProjectsRelativeProjectPathLandsAtTarget() {
        //A project added by RELATIVE path has no home yet; the deep save
        //must create it next to the save target.
        SolutionNode s("Solo");
        ProjectNode* proj = s.addProject("homeless.nproj");
        QVERIFY(proj != nullptr);
        QVERIFY(proj->addFile(m_tmpDir.path() + "/main.n") != nullptr);

        QString err;
        QVERIFY(s.saveWithProjects(m_tmpDir.path() + "/rel.nsln", &err));
        QVERIFY(QFile::exists(m_tmpDir.path() + "/homeless.nproj"));

        SolutionNode reloaded("");
        QVERIFY(reloaded.loadWithProjects(m_tmpDir.path() + "/rel.nsln", &err));
        QCOMPARE(reloaded.projects()[0]->fileCount(), 1);
    }

    void testSaveWithProjectsKeepsProjectHomeOnRelocate() {
        //Saving the .nsln into a different directory must NOT copy the
        //project data there: the .nsln keeps referencing the original
        //home (an orphan copy would go stale on the next edit).
        writeFixture("reloc.nproj", projectXml("App"));
        writeFixture("reloc.nsln", solutionXml("Solo", {"reloc.nproj"}));

        SolutionNode s("");
        QVERIFY(s.loadWithProjects(m_tmpDir.path() + "/reloc.nsln"));

        QTemporaryDir other;
        QString err;
        QVERIFY(s.saveWithProjects(other.path() + "/reloc.nsln", &err));
        QVERIFY(!QFile::exists(other.path() + "/reloc.nproj"));

        SolutionNode reloaded("");
        QVERIFY(reloaded.loadWithProjects(other.path() + "/reloc.nsln", &err));
        QCOMPARE(reloaded.projects()[0]->name(), QString("App"));
        QCOMPARE(reloaded.projects()[0]->fileCount(), 2);
    }

    void testSaveWithProjectsEmptyProjectFails() {
        SolutionNode s("Solo");
        QVERIFY(s.addProject(m_tmpDir.path() + "/blank.nproj") != nullptr);

        QString err;
        QVERIFY(!s.saveWithProjects(m_tmpDir.path() + "/fresh.nsln", &err));
        QVERIFY(err.contains("no source files"));
        //The .nsln is written only after every project saved.
        QVERIFY(!QFile::exists(m_tmpDir.path() + "/fresh.nsln"));
    }
};

//GUILESS, not plain QTEST_MAIN: the library links Qt5::Gui, whose
//transitive QT_GUI_LIB would upgrade QTEST_MAIN to a QGuiApplication
//that demands a platform plugin (modal dialogs on machines without it).
QTEST_GUILESS_MAIN(TestProjectModel)
#include "test_projectmodel.moc"
