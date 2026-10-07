/*--- MainWindowHelp.cpp - tools/view/help menus and the recent list of
    the NLang IDE main window. ---*/
#include "MainWindow.h"
#include "HelpBrowser.h"
#include "HelpPagePath.h"
#include "RecentStore.h"
#include "SettingsDialog.h"
#include "SettingsStore.h"

#include "ui_MainWindow.h"

#include <nlang_version.h>  // generated from the repo VERSION file

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStyle>
#include <QUrl>

namespace nlang {

//--- 工具 ---

void MainWindow::on_actToolsOptions_triggered() {
    const SettingsStore stored = SettingsStore::persisted();
    SettingsDialog dialog(this);
    dialog.init(stored);
    if (dialog.exec() != QDialog::Accepted)
        return;
    SettingsStore updated = stored;
    updated.setLanguage(dialog.language());
    updated.setBuildOutputDir(dialog.buildOutputDir());
    updated.setToolbarIconSize(dialog.toolbarIconSize());
    updated.setLibrarySearchPaths(dialog.librarySearchPaths());
    updated.setNoWarn(dialog.noWarn());
    updated.setEditorFontPt(dialog.editorFontPt());
    updated.setTerminalFontPt(dialog.terminalFontPt());
    updated.persist();
    applyToolbarIconSize(updated.toolbarIconSize());
    applyEditorFontPt(updated.editorFontPt());
    applyTerminalFontPt(updated.terminalFontPt());
    //Library dirs changed: rebuild the code-assistance index immediately.
    reindexConfiguredLibraries();
    //The catalogs install once at startup, so a language change needs
    //a restart (no per-widget retranslate pass exists); the build
    //output directory applies from the next build on.
    if (updated.language() != stored.language())
        QMessageBox::information(
            this, tr("Settings"),
            tr("The language change takes effect after restarting "
               "NLang IDE."));
}

//--- view ---

void MainWindow::on_actViewSolution_triggered(bool checked) {
    m_ui->dckSolution->setVisible(checked);
}

void MainWindow::on_actViewCodeEditor_triggered(bool checked) {
    m_ui->tabCodes->setVisible(checked);
}

void MainWindow::on_actViewOutput_triggered(bool checked) {
    m_ui->tabOutput->setVisible(checked);
}

void MainWindow::on_actViewToolBar_triggered(bool checked) {
    m_ui->mainToolBar->setVisible(checked);
}

//--- help ---

void MainWindow::on_actHelpAbout_triggered() {
    //The version line comes from the generated nlang_version.h (single
    //source: the repository VERSION file). The anchor makes Qt treat
    //the whole text as rich text, so line breaks must be <br> -- a raw
    //\n collapses to a space. The message box label opens external
    //links by default and renders anchors as blue underlined text.
    QMessageBox box;
    box.setWindowTitle(tr("About NLang IDE"));
    //Large app icon in the upper-left (the static about() uses the small
    //window icon); 96px reads well in the dialog.
    box.setIconPixmap(QIcon(QStringLiteral(":/nide/Resources/app_icon.png"))
                          .pixmap(96, 96));
    box.setText(
        tr("NLang IDE %1<br>The integrated development environment for "
           "the NLang scripting language.<br><br>"
           "<a href=\"https://github.com/dliting/nlang\">"
           "https://github.com/dliting/nlang</a>")
            .arg(QLatin1String(NLANG_VERSION)));
    box.exec();
}

void MainWindow::on_actHelpGettingStarted_triggered() {
    openHelpDocument(QStringLiteral("getting-started/what-is-nlang"));
}

void MainWindow::on_actHelpLanguageSpec_triggered() {
    openHelpDocument(QStringLiteral("language-spec/overview"));
}

void MainWindow::on_actHelpVmArch_triggered() {
    openHelpDocument(QStringLiteral("vm-architecture/overview"));
}

void MainWindow::on_actHelpCliTools_triggered() {
    openHelpDocument(QStringLiteral("cli-tools/overview"));
}

void MainWindow::openHelpDocument(const QString& documentPagePath) {
    const QString page = locateHelpPage(documentPagePath);
    if (page.isEmpty()) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("The document '%1' was not found next to the IDE "
               "installation.").arg(documentPagePath));
        return;
    }
    //Embedded viewer instead of the system browser. The browser
    //deletes itself on close (m_helpBrowser self-nulls), so every
    //entry re-creates it; while it is open, entries reuse it.
    if (m_helpBrowser.isNull())
        m_helpBrowser = new HelpBrowser(this);
    m_helpBrowser->openPage(QUrl::fromLocalFile(page));
}

//--- recent list ---

void MainWindow::noteRecent(const QString& absolutePath) {
    m_recent.push(absolutePath);
    saveRecent();
}

void MainWindow::saveRecent() {
    QSettings settings;
    m_recent.save(settings);
}

void MainWindow::rebuildRecentMenu() {
    QMenu* menu = m_ui->menuRecent;
    menu->clear();
    //Existing-on-disk entries only: a missing file is hidden but
    //stays stored until a newer entry evicts it.
    QStringList visible;
    QSet<QString> names;
    QSet<QString> duplicated;
    for (const QString& path : m_recent.entries()) {
        if (!QFileInfo::exists(path))
            continue;
        visible << path;
        const QString name = QFileInfo(path).fileName();
        if (names.contains(name))
            duplicated << name;
        names << name;
    }
    for (const QString& path : visible) {
        const QFileInfo info(path);
        QString text = info.fileName();
        if (duplicated.contains(text))
            text += QStringLiteral(" (") + info.dir().dirName() +
                    QStringLiteral(")");
        //Keep & out of the mnemonic role.
        text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* action = menu->addAction(
            recentEntryIcon(info), text, this,
            &MainWindow::onRecentEntryTriggered);
        action->setToolTip(path);
        action->setData(path);
    }
    menu->addSeparator();
    menu->addAction(tr("Clear Recent List"), this,
                    &MainWindow::onClearRecentTriggered);
    //Shown with something clickable in it, hidden otherwise.
    menu->menuAction()->setVisible(!visible.isEmpty());
}

QIcon MainWindow::recentEntryIcon(const QFileInfo& info) const {
    //Container metaphor: solution = drive, project = folder, file =
    //document. QFileIconProvider cannot tell custom extensions apart.
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("nsln"))
        return style()->standardIcon(QStyle::SP_DriveHDIcon);
    if (suffix == QStringLiteral("nproj"))
        return style()->standardIcon(QStyle::SP_DirIcon);
    return style()->standardIcon(QStyle::SP_FileIcon);
}

void MainWindow::onRecentEntryTriggered() {
    QAction* action = qobject_cast<QAction*>(sender());
    if (action == nullptr)
        return;
    const QString path = action->data().toString();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("nsln"))
        openSolutionAtPath(path);
    else if (suffix == QStringLiteral("nproj"))
        openProjectAtPath(path);
    else
        editExistingFile(path);
}

void MainWindow::onClearRecentTriggered() {
    m_recent.clear();
    saveRecent();
    rebuildRecentMenu();
}

} // namespace nlang
