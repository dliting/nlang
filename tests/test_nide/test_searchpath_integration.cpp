/*--- test_searchpath_integration.cpp - end-to-end library search path ---*
Real tools only (no mocks), mirroring test_mainwindow: a real ncc builds,
a real nvm/ndb runs. Covers the IDE wiring of the library search path:
  * a global (Tools > Options) library dir is indexed for completion and
    passed as -I so a standalone build resolves the library;
  * a project's .nproj <ImportPaths> feeds the same build path.
The data/UI layers have their own unit tests (test_projectmodel,
test_dialogs, test_searchpathargs); here the seams are real builds and the
real completion popup. ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "SettingsStore.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using nlang::CodeEditor;
using nlang::MainWindow;
using nlang::SettingsStore;

namespace {

constexpr int kToolTimeoutMs = 20000;

//Schedule an action inside the modal loop the NEXT blocking call opens.
template<typename Fn>
void inExec(Fn action) { QTimer::singleShot(0, action); }

//Close a (possibly QFileDialog, whose accept is protected) via meta-object.
void acceptDialog(QDialog* dialog) {
    QMetaObject::invokeMethod(dialog, "accept");
}

//Accept the active non-native file dialog with the chosen path.
void acceptFileDialog(const QString& filePath) {
    if (QFileDialog* dialog =
            qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
        dialog->selectFile(filePath);
        acceptDialog(dialog);
    }
}

void writeFile(const QString& filePath, const char* content) {
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(content);
}

QAction* act(MainWindow& window, const char* name) {
    return window.findChild<QAction*>(name);
}

QTabWidget* tabCodes(MainWindow& window) {
    return window.findChild<QTabWidget*>("tabCodes");
}

CodeEditor* currentCode(MainWindow& window) {
    return qobject_cast<CodeEditor*>(tabCodes(window)->currentWidget());
}

//A nlang-implemented library in its own dir: namespace greetlib with one
//function hello (exercising the nlang-source side of a package).
QString writeGreetLib(const QString& baseDir) {
    const QString libDir = QDir(baseDir).filePath("libs");
    QDir().mkpath(libDir);
    writeFile(QDir(libDir).filePath("greetlib.n"),
        "namespace greetlib {\n"
        "    // Return the given name (a nlang-implemented library fn).\n"
        "    string hello(string who) {\n"
        "        return who;\n"
        "    }\n"
        "}\n");
    return libDir;
}

//Open a standalone snippet through the real File > Open action.
CodeEditor* openSnippet(MainWindow& window, const QString& path,
                        const char* content) {
    writeFile(path, content);
    inExec([&] { acceptFileDialog(path); });
    act(window, "actOpenFile")->trigger();
    return currentCode(window);
}

//Type "greetlib." and return the completion popup (null when absent).
QListWidget* completeAfterDot(CodeEditor* editor, const char* prefix) {
    editor->setPlainText(QString::fromUtf8(prefix));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    QTest::keyClick(editor, Qt::Key_Period);
    const QList<QListWidget*> popups =
        editor->findChildren<QListWidget*>();
    return popups.isEmpty() ? nullptr : popups.constFirst();
}

void persistGlobalPaths(const QStringList& dirs) {
    SettingsStore store = SettingsStore::persisted();
    store.setLibrarySearchPaths(dirs);
    store.persist();
}

} // namespace

class TestSearchPathIntegration : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setOrganizationName(QStringLiteral("NLang"));
        QCoreApplication::setApplicationName(
            QStringLiteral("nide-searchpath-test"));
        persistGlobalPaths({});
    }

    //A global library dir is indexed at startup and offered in completion,
    //and a standalone build importing it succeeds via the -I args.
    void globalLibraryDirIndexedAndBuilt() {
        QTemporaryDir work;
        const QString libDir = writeGreetLib(work.path());
        persistGlobalPaths({libDir});   // before the window: startup reindex

        MainWindow window;
        const QString srcPath = QDir(work.path()).filePath("app.n");
        CodeEditor* src = openSnippet(window, srcPath,
            "public int main() {\n    return 0;\n}\n");
        QVERIFY(src != nullptr);

        //Completion after "greetlib." lists the indexed hello().
        QListWidget* popup = completeAfterDot(src, "greetlib");
        QVERIFY2(popup != nullptr, "completion must open for the global lib");
        QCOMPARE(popup->count(), 1);
        QCOMPARE(popup->item(0)->text(), QString("hello"));

        //A real standalone build that imports the global library succeeds:
        //the IDE passed the dir as -I to ncc.
        src->setPlainText(
            "import greetlib;\n"
            "public int main() {\n"
            "    string s = greetlib.hello(\"x\");\n"
            "    return 0;\n"
            "}\n");
        act(window, "actBuild")->trigger();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build succeeded"));

        persistGlobalPaths({});   // restore
    }

    //A project .nproj <ImportPaths> entry makes the library resolve in a
    //project build (ncc reads it from the project; the IDE -I is de-duped).
    void projectImportPathsUsedForBuild() {
        QTemporaryDir work;
        const QString libDir = writeGreetLib(work.path());
        const QString appDir = QDir(work.path()).filePath("App");
        QDir().mkpath(appDir);
        const QString nproj = QDir(appDir).filePath("App.nproj");
        const QString main = QDir(appDir).filePath("main.n");
        //Import path stored relative to the .nproj (../libs).
        writeFile(nproj,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\">\n"
            "  <ImportPaths>\n"
            "    <Dir path=\"../libs\"/>\n"
            "  </ImportPaths>\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");
        writeFile(main,
            "import greetlib;\n"
            "public int main() {\n"
            "    string s = greetlib.hello(\"x\");\n"
            "    return 0;\n"
            "}\n");

        MainWindow window;
        inExec([&] { acceptFileDialog(nproj); });
        act(window, "actOpenProject")->trigger();

        act(window, "actBuild")->trigger();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build succeeded"));
    }
};

QTEST_MAIN(TestSearchPathIntegration)
#include "test_searchpath_integration.moc"
