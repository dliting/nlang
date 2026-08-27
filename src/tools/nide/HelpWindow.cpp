/*--- HelpWindow.cpp - read-only markdown help viewer ---*/
#include "HelpWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace nlang {

namespace {

//How far above the executable to search for the shipped docs/
//directory. One hop covers the installed package (bin/../docs); the
//dev, test and deploy-check layouts sit deeper in the build tree.
//The dev nide.exe (build-ide/src/tools/nide/Release) resolves at
//hop 5 -- the cap's last slot: re-check before nesting it deeper.
const int MAX_DOC_ANCESTOR_HOPS = 6;

//First ancestor whose docs/ holds this document; empty when none does
//(the window then shows the not-found notice instead).
QString locateDocument(const QString& documentBaseName) {
    QDir dir = QCoreApplication::applicationDirPath();
    for (int hop = 0; hop < MAX_DOC_ANCESTOR_HOPS; ++hop) {
        const QString candidate =
            dir.absoluteFilePath("docs/" + documentBaseName + ".md");
        if (QFileInfo::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return QString();
}

} // namespace

HelpWindow::HelpWindow(const QString& documentBaseName, QWidget* parent)
    : QDialog(parent)
    , m_browser(new QTextBrowser(this))
{
    setWindowTitle(tr("NLang Documentation - %1").arg(documentBaseName));
    resize(800, 600);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_browser);

    //A missing docs/ directory is a normal state (bin-only copy):
    //say so inside the window rather than popping a modal box.
    QFile file(locateDocument(documentBaseName));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = file.readAll();
        m_browser->setMarkdown(QString::fromUtf8(raw));
        //open() success does not guarantee the read: only a clean
        //readAll counts as loaded (an empty file is a valid empty doc).
        m_loaded = (file.error() == QFile::NoError);
    } else {
        m_browser->setPlainText(
            tr("The document '%1' was not found next to the IDE "
               "installation.").arg(documentBaseName + ".md"));
    }
}

} // namespace nlang
