/*--- test_solutiontreemodel.cpp - SolutionTreeModel unit tests ---*/
#include "../../../src/tools/nide/SolutionTreeModel.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace nlang;

namespace {

//Writes a fixture file (UTF-8) and returns its absolute path.
QString writeFile(const QTemporaryDir& dir, const QString& name,
                  const QString& content) {
    QString path = dir.path() + "/" + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << content;
    return path;
}

//A minimal project file with two sources.
QString projectXml(const QString& name) {
    return QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project name=\"%1\" namespace=\"ns\" outputDir=\"bin\" intermediateDir=\"obj\">\n"
        "  <Sources>\n"
        "    <File path=\"main.n\"/>\n"
        "    <File path=\"util.n\"/>\n"
        "  </Sources>\n"
        "</Project>\n").arg(name);
}

//A solution file referencing the given project files.
QString solutionXml(const QString& name, const QStringList& projectPaths) {
    QString entries;
    for (const QString& p : projectPaths)
        entries += QString("    <Project path=\"%1\"/>\n").arg(p);
    return QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Solution name=\"%1\">\n"
        "  <Projects>\n"
        "%2"
        "  </Projects>\n"
        "</Solution>\n").arg(name, entries);
}

} // namespace

class TestSolutionTreeModel : public QObject {
    Q_OBJECT

private slots:
    //QSignalSpy resolves each signal parameter by its moc-recorded
    //type NAME -- which lacks the namespace ("FileNode*"), while
    //QVariant::value uses the qualified one. Register both spellings.
    void initTestCase() {
        qRegisterMetaType<FileNode*>("FileNode*");
        qRegisterMetaType<FileNode*>("nlang::FileNode*");
    }

    // --- new solution ---

    void testNewSolutionBuildsRootItem() {
        SolutionTreeModel model;
        model.newSolution("Solo");

        QVERIFY(model.hasSolution());
        QCOMPARE(model.rowCount(), 1);

        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QVERIFY(root != nullptr);
        QCOMPARE(root->nodeType(), SolutionTreeItem::NT_Solution);
        QCOMPARE(root->text(), QString("Solo"));
        QVERIFY(root->solution() == model.solutionNode());
        QVERIFY(root->project() == nullptr);
        QVERIFY(root->file() == nullptr);
    }

    void testCloseSolutionClearsTree() {
        SolutionTreeModel model;
        model.newSolution("Solo");
        model.closeSolution();

        QVERIFY(!model.hasSolution());
        QVERIFY(model.solutionNode() == nullptr);
        QCOMPARE(model.rowCount(), 0);

        //Mutations on a closed model are rejected, not crashed on.
        QVERIFY(model.addProject("x/y.nproj") == nullptr);
    }

    // --- projects ---

    void testAddProjectMirrorsChildItem() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");

        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        QVERIFY(project != nullptr);

        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QCOMPARE(root->rowCount(), 1);

        SolutionTreeItem* item = root->childItem(0);
        QCOMPARE(item->nodeType(), SolutionTreeItem::NT_Project);
        QCOMPARE(item->text(), project->name());
        QVERIFY(item->project() == project);
        QVERIFY(item->solution() == nullptr);
    }

    void testDuplicateProjectRejected() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");

        const QString path = dir.path() + "/app.nproj";
        QVERIFY(model.addProject(path) != nullptr);
        QVERIFY(model.addProject(path) == nullptr);

        QCOMPARE(model.solutionNode()->projectCount(), 1);
        QCOMPARE(model.itemAt(model.index(0, 0))->rowCount(), 1);
    }

    // --- open project (add + load, all-or-nothing) ---

    void testOpenProjectRollbackKeepsSolutionClean() {
        QTemporaryDir dir;
        writeFile(dir, "bad.nproj", "this is not project XML");

        SolutionTreeModel model;
        model.newSolution("Solo");  // fresh solution: clean

        QString error;
        QVERIFY(model.openProject(dir.path() + "/bad.nproj", &error)
                == nullptr);
        QVERIFY(!error.isEmpty());

        //All-or-nothing includes the dirty flag: addProject and the
        //rollback's removeProject both mark the solution dirty, which a
        //clean solution must not inherit from a failed open.
        QCOMPARE(model.solutionNode()->projectCount(), 0);
        QVERIFY(!model.solutionNode()->isDirty());
        QCOMPARE(model.itemAt(model.index(0, 0))->rowCount(), 0);
    }

    void testOpenProjectWithoutSolution() {
        SolutionTreeModel model;

        QString error;
        QVERIFY(model.openProject("wherever/app.nproj", &error) == nullptr);
        QCOMPARE(error, QString("no solution is open"));
    }

    void testRemoveProjectUpdatesTree() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");

        QVERIFY(model.removeProject(project));
        QCOMPARE(model.solutionNode()->projectCount(), 0);
        QCOMPARE(model.itemAt(model.index(0, 0))->rowCount(), 0);

        //An unknown pointer is rejected, not crashed on.
        ProjectNode stale("stale", dir.path());
        QVERIFY(!model.removeProject(&stale));
    }

    // --- files ---

    void testAddFileShowsFileNameOnly() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");

        FileNode* file = model.addFile(project, dir.path() + "/src/main.n");
        QVERIFY(file != nullptr);

        SolutionTreeItem* projectItem = model.itemAt(model.index(0, 0))->childItem(0);
        QCOMPARE(projectItem->rowCount(), 1);
        QCOMPARE(projectItem->childItem(0)->nodeType(), SolutionTreeItem::NT_File);
        QCOMPARE(projectItem->childItem(0)->text(), QString("main.n"));
        QVERIFY(projectItem->childItem(0)->file() == file);

        //itemAt also unwraps file-level indexes.
        QModelIndex fileIndex = model.index(0, 0).child(0, 0).child(0, 0);
        QVERIFY(model.itemAt(fileIndex) != nullptr);
        QVERIFY(model.itemAt(fileIndex)->file() == file);
    }

    void testForeignProjectRejected() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        model.addProject(dir.path() + "/app.nproj");

        //A project not owned by this solution is rejected BEFORE any
        //mutation: the foreign node must stay untouched.
        ProjectNode foreign("Foreign", dir.path());
        QVERIFY(model.addFile(&foreign, dir.path() + "/x.n") == nullptr);
        QCOMPARE(foreign.fileCount(), 0);

        FileNode* foreignFile = foreign.addFile(dir.path() + "/y.n");
        QVERIFY(!model.removeFile(foreignFile));
        QCOMPARE(foreign.fileCount(), 1);
    }

    void testDuplicateFileRejected() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");

        const QString path = dir.path() + "/main.n";
        QVERIFY(model.addFile(project, path) != nullptr);
        QVERIFY(model.addFile(project, path) == nullptr);

        QCOMPARE(project->fileCount(), 1);
        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)->rowCount(), 1);
    }

    void testRemoveFileUpdatesTree() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        FileNode* file = model.addFile(project, dir.path() + "/main.n");

        QVERIFY(model.removeFile(file));
        QCOMPARE(project->fileCount(), 0);
        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)->rowCount(), 0);
    }

    // --- load / save ---

    void testLoadSolutionBuildsFullTree() {
        QTemporaryDir dir;
        writeFile(dir, "app.nproj", projectXml("App"));
        writeFile(dir, "lib.nproj", projectXml("Lib"));
        writeFile(dir, "solo.nsln", solutionXml("Solo", {"app.nproj", "lib.nproj"}));

        SolutionTreeModel model;
        QString error = "stale";  // a successful load must clear this
        QVERIFY2(model.loadSolution(dir.path() + "/solo.nsln", &error), qUtf8Printable(error));
        QCOMPARE(error, QString());

        QCOMPARE(model.solutionNode()->name(), QString("Solo"));
        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QCOMPARE(root->rowCount(), 2);

        //The .nproj name attribute wins over the file stem.
        QCOMPARE(root->childItem(0)->text(), QString("App"));
        QCOMPARE(root->childItem(0)->rowCount(), 2);   // main.n + util.n
        QCOMPARE(root->childItem(0)->childItem(0)->text(), QString("main.n"));
        QCOMPARE(root->childItem(1)->text(), QString("Lib"));
    }

    void testLoadFailureKeepsPreviousSolution() {
        QTemporaryDir dir;
        writeFile(dir, "app.nproj", projectXml("App"));
        writeFile(dir, "good.nsln", solutionXml("Good", {"app.nproj"}));
        writeFile(dir, "bad.nsln", solutionXml("Bad", {"missing.nproj"}));

        SolutionTreeModel model;
        QVERIFY2(model.loadSolution(dir.path() + "/good.nsln"), "good fixture must load");

        QString error;
        QVERIFY(!model.loadSolution(dir.path() + "/bad.nsln", &error));
        QVERIFY(!error.isEmpty());

        //All-or-nothing: the previous solution and its tree survive.
        QCOMPARE(model.solutionNode()->name(), QString("Good"));
        QCOMPARE(model.itemAt(model.index(0, 0))->rowCount(), 1);
    }

    void testLoadFailureOnClosedModelKeepsItClosed() {
        QTemporaryDir dir;
        writeFile(dir, "bad.nsln", solutionXml("Bad", {"missing.nproj"}));

        SolutionTreeModel model;
        QString error;
        QVERIFY(!model.loadSolution(dir.path() + "/bad.nsln", &error));

        //A failed open leaves no phantom solution without a tree;
        //mutations are rejected instead of crashing on a null root.
        QVERIFY(!model.hasSolution());
        QVERIFY(model.solutionNode() == nullptr);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.addProject(dir.path() + "/x.nproj") == nullptr);
        ProjectNode stale("stale", dir.path());
        QVERIFY(!model.removeProject(&stale));
    }

    void testSaveSolutionWritesProjectsToo() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        model.addFile(project, dir.path() + "/main.n");

        QString error;
        QVERIFY(model.saveSolution(dir.path() + "/solo.nsln", &error));
        QCOMPARE(error, QString());

        //Saving the solution must also persist the referenced project.
        QVERIFY(QFile::exists(dir.path() + "/app.nproj"));

        //A fresh model round-trips the whole tree.
        SolutionTreeModel reloaded;
        QVERIFY2(reloaded.loadSolution(dir.path() + "/solo.nsln", &error), qUtf8Printable(error));
        SolutionTreeItem* root = reloaded.itemAt(reloaded.index(0, 0));
        QCOMPARE(root->rowCount(), 1);
        QCOMPARE(root->childItem(0)->text(), QString("app"));
        QCOMPARE(root->childItem(0)->rowCount(), 1);
    }

    void testSaveAsKeepsProjectHome() {
        QTemporaryDir home, other;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(home.path() + "/app.nproj");
        model.addFile(project, home.path() + "/main.n");

        //Save-as into another directory.
        QString error;
        QVERIFY2(model.saveSolution(other.path() + "/solo.nsln", &error), qUtf8Printable(error));

        //The project keeps its home: no orphan copy next to the new .nsln.
        QVERIFY(!QFile::exists(other.path() + "/app.nproj"));

        //The new .nsln still resolves the project where it lives.
        SolutionTreeModel reloaded;
        QVERIFY2(reloaded.loadSolution(other.path() + "/solo.nsln", &error), qUtf8Printable(error));
        QCOMPARE(reloaded.solutionNode()->projectCount(), 1);
        QCOMPARE(reloaded.solutionNode()->projects()[0]->fileCount(), 1);
    }

    void testLoadSolutionAbsoluteProjectEntry() {
        QTemporaryDir dir;
        writeFile(dir, "app.nproj", projectXml("App"));
        //An absolute <Project path> entry must load as-is.
        writeFile(dir, "solo.nsln", solutionXml("Solo", {dir.path() + "/app.nproj"}));

        SolutionTreeModel model;
        QString error;
        QVERIFY2(model.loadSolution(dir.path() + "/solo.nsln", &error), qUtf8Printable(error));
        QCOMPARE(model.solutionNode()->projectCount(), 1);
        QCOMPARE(model.itemAt(model.index(0, 0))->rowCount(), 1);
        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)->rowCount(), 2);
    }

    // --- item access ---

    void testItemAtInvalidIndexIsNull() {
        SolutionTreeModel model;
        model.newSolution("Solo");

        QVERIFY(model.itemAt(QModelIndex()) == nullptr);
        QVERIFY(model.itemAt(model.index(5, 0)) == nullptr);
    }

    void testRefreshPicksUpDirectMutation() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        model.addProject(dir.path() + "/app.nproj");

        model.solutionNode()->projects()[0]->setName("Renamed");
        model.refresh();

        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)->text(),
                 QString("Renamed"));
    }

    void testOnlyFileItemsAreEditable() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        model.addFile(project, dir.path() + "/main.n");

        //Solution/project rows stay read-only; the file row hosts the
        //inline rename editor.
        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QVERIFY(!(root->flags() & Qt::ItemIsEditable));
        QVERIFY(!(root->childItem(0)->flags() & Qt::ItemIsEditable));
        QVERIFY(root->childItem(0)->childItem(0)->flags() & Qt::ItemIsEditable);
    }

    // --- inline rename (setData -> fileRenameRequested) ---

    void testFileEditEmitsRenameRequest() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        FileNode* file = model.addFile(project, dir.path() + "/main.n");
        const QModelIndex fileIndex =
            model.index(0, 0).child(0, 0).child(0, 0);

        QSignalSpy spy(&model, &SolutionTreeModel::fileRenameRequested);
        QVERIFY(model.setData(fileIndex, QVariant("renamed.n"), Qt::EditRole));

        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).value<FileNode*>() == file);
        QCOMPARE(spy.at(0).at(1).toString(), QString("renamed.n"));

        //The mirror is never written directly: the text (and the domain
        //path) only move when the owner commits the rename.
        QCOMPARE(model.itemAt(fileIndex)->text(), QString("main.n"));
        QCOMPARE(file->absolutePath(), dir.path() + "/main.n");
    }

    void testFileEditUnchangedNameEmitsNothing() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        model.addFile(project, dir.path() + "/main.n");
        const QModelIndex fileIndex =
            model.index(0, 0).child(0, 0).child(0, 0);

        QSignalSpy spy(&model, &SolutionTreeModel::fileRenameRequested);
        QVERIFY(model.setData(fileIndex, QVariant("main.n"), Qt::EditRole));
        QVERIFY(model.setData(fileIndex, QVariant("  main.n  "), Qt::EditRole));

        QCOMPARE(spy.count(), 0);  // trimmed text equals the current name
    }

    void testNonFileRowsSwallowEdit() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        model.addProject(dir.path() + "/app.nproj");

        QSignalSpy spy(&model, &SolutionTreeModel::fileRenameRequested);
        const QModelIndex solutionIndex = model.index(0, 0);
        const QModelIndex projectIndex = solutionIndex.child(0, 0);
        QVERIFY(model.setData(solutionIndex, QVariant("X"), Qt::EditRole));
        QVERIFY(model.setData(projectIndex, QVariant("X"), Qt::EditRole));

        QCOMPARE(spy.count(), 0);
        QCOMPARE(model.itemAt(solutionIndex)->text(), QString("Solo"));
        QCOMPARE(model.itemAt(projectIndex)->text(), QString("app"));
    }

    void testRefreshDoesNotEmitRenameRequest() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        model.addProject(dir.path() + "/app.nproj");

        //refresh() rebuilds items through the constructors' setText,
        //which must never surface as an edit; only a user edit does.
        QSignalSpy spy(&model, &SolutionTreeModel::fileRenameRequested);
        model.solutionNode()->projects()[0]->setName("Renamed");
        model.refresh();

        QCOMPARE(spy.count(), 0);
    }

    // --- renameFile (domain + mirror in lockstep) ---

    void testRenameFileUpdatesDomainAndMirror() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        FileNode* file = model.addFile(project, dir.path() + "/main.n");

        QString error;
        QVERIFY(model.renameFile(file, dir.path() + "/renamed.n", &error));

        QCOMPARE(file->absolutePath(), dir.path() + "/renamed.n");
        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)
                     ->childItem(0)->text(),
                 QString("renamed.n"));
        QVERIFY(project->isDirty());
    }

    void testRenameFileDuplicateKeepsMirror() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode* project = model.addProject(dir.path() + "/app.nproj");
        FileNode* main = model.addFile(project, dir.path() + "/main.n");
        model.addFile(project, dir.path() + "/util.n");

        QString error;
        QVERIFY(!model.renameFile(main, dir.path() + "/util.n", &error));
        QVERIFY(!error.isEmpty());

        QCOMPARE(main->absolutePath(), dir.path() + "/main.n");
        QCOMPARE(model.itemAt(model.index(0, 0))->childItem(0)
                     ->childItem(0)->text(),
                 QString("main.n"));
    }

    void testRenameFileForeignRejected() {
        QTemporaryDir dir;
        SolutionTreeModel model;
        model.newSolution("Solo");
        ProjectNode foreign("Foreign", dir.path());
        FileNode* foreignFile = foreign.addFile(dir.path() + "/x.n");

        QString error;
        QVERIFY(!model.renameFile(foreignFile, dir.path() + "/y.n", &error));
        QCOMPARE(foreignFile->absolutePath(), dir.path() + "/x.n");
    }

    // --- standalone files group ---

    void testStandaloneGroupAsSoleRootWithoutSolution() {
        SolutionTreeModel model;  // closed: no solution open
        model.setStandaloneFiles({"/x/a.n", "/x/b.n"});
        QCOMPARE(model.rowCount(), 1);
        SolutionTreeItem* group = model.itemAt(model.index(0, 0));
        QCOMPARE(group->nodeType(), SolutionTreeItem::NT_StandaloneFiles);
        QVERIFY(group->solution() == nullptr);
        QCOMPARE(group->text(), QString("Standalone Files"));
        QCOMPARE(group->rowCount(), 2);
        SolutionTreeItem* first = group->childItem(0);
        QCOMPARE(first->nodeType(), SolutionTreeItem::NT_StandaloneFile);
        QCOMPARE(first->text(), QString("a.n"));
        QCOMPARE(first->standalonePath(), QString("/x/a.n"));
        QCOMPARE(model.standaloneGroupItem(), group);  // public lookup
    }

    void testStandaloneEmptyListShowsNoRow() {
        SolutionTreeModel model;
        model.setStandaloneFiles({"/x/a.n"});
        model.setStandaloneFiles({});
        QCOMPARE(model.rowCount(), 0);
    }

    void testStandaloneGroupUnderSolutionRoot() {
        QTemporaryDir dir;
        const QString nproj =
            writeFile(dir, "P.nproj", projectXml("P"));
        const QString nsln = writeFile(dir, "S.nsln",
                                       solutionXml("S", {nproj}));
        SolutionTreeModel model;
        QVERIFY(model.loadSolution(nsln));
        model.setStandaloneFiles({"/x/solo.n"});
        // Solution root; project; then the group appended LAST (D1).
        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(root->rowCount(), 2);  // project + group
        SolutionTreeItem* group = root->childItem(1);
        QCOMPARE(group->nodeType(), SolutionTreeItem::NT_StandaloneFiles);
        QCOMPARE(group->childItem(0)->standalonePath(), QString("/x/solo.n"));
    }

    void testRefreshKeepsStandaloneGroup() {
        SolutionTreeModel model;
        model.setStandaloneFiles({"/x/solo.n"});
        model.newSolution("Solo");  // refresh() inside
        QCOMPARE(model.rowCount(), 1);  // solution root
        SolutionTreeItem* root = model.itemAt(model.index(0, 0));
        QCOMPARE(root->rowCount(), 1);  // the group
        QCOMPARE(root->childItem(0)->childItem(0)->text(),
                 QString("solo.n"));
        model.closeSolution();  // refresh again: group survives as root
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.itemAt(model.index(0, 0))->nodeType(),
                 SolutionTreeItem::NT_StandaloneFiles);
    }

    void testSetStandaloneFilesSyncsIncrementally() {
        QTemporaryDir dir;
        const QString nproj = writeFile(dir, "app.nproj", projectXml("App"));
        const QString nsln = writeFile(dir, "S.nsln",
                                       solutionXml("S", {nproj}));
        SolutionTreeModel model;
        QVERIFY(model.loadSolution(nsln));
        model.setStandaloneFiles({"/x/a.n"});
        const QModelIndex rootIndex = model.index(0, 0);
        model.setStandaloneFiles({"/x/b.n", "/x/c.n"});  // no rebuild
        QCOMPARE(model.itemAt(rootIndex)->nodeType(),
                 SolutionTreeItem::NT_Solution);  // index still valid
        SolutionTreeItem* group = model.itemAt(rootIndex)->childItem(1);
        QCOMPARE(group->rowCount(), 2);
        QCOMPARE(group->childItem(0)->standalonePath(), QString("/x/b.n"));
    }

    void testStandaloneRowsNotEditable() {
        SolutionTreeModel model;
        model.setStandaloneFiles({"/x/a.n"});
        const QModelIndex fileIndex =
            model.index(0, 0, model.index(0, 0));
        QVERIFY(!(model.flags(fileIndex) & Qt::ItemIsEditable));
        QVERIFY(!(model.flags(model.index(0, 0)) & Qt::ItemIsEditable));
    }
};

QTEST_GUILESS_MAIN(TestSolutionTreeModel)
#include "test_solutiontreemodel.moc"
