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
        QCOMPARE(f1->absolutePath(), QString("main.n"));
        QCOMPARE(f2->absolutePath(), QString("utils.n"));
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
        QCOMPARE(files[0]->absolutePath(), QString("a.n"));
        QCOMPARE(files[1]->absolutePath(), QString("b.n"));
        QCOMPARE(files[2]->absolutePath(), QString("c.n"));
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
        QVERIFY(!error.isEmpty());
    }

    void testProjectNodeLoadFileWithoutPath() {
        QString projPath = writeFixture("noattr.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"X\">\n  <Sources>\n    <File/>\n  </Sources>\n</Project>\n");
        ProjectNode proj("", m_tmpDir.path());
        QString error;
        QVERIFY(!proj.load(projPath, &error));
        QVERIFY(!error.isEmpty());
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
};

QTEST_MAIN(TestProjectModel)
#include "test_projectmodel.moc"
