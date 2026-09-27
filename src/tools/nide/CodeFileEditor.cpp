/*--- CodeFileEditor.cpp - file-backed code editor tab for NLang IDE ---*/
#include "CodeEditor.h"

#include <QFile>
#include <QSaveFile>
#include <QTextStream>

namespace nlang {

CodeFileEditor::CodeFileEditor(EditorManager& owner,
                               const QString& absoluteFilePath)
    : FileEditor(owner, absoluteFilePath)
    , m_editor(nullptr)
{
    //Named through the derived class: onTextChange is protected in
    //FileEditor and cannot be named as a base member pointer here.
    connect(&m_editor, &QPlainTextEdit::textChanged, this,
            &CodeFileEditor::onTextChange);
    //Cursor moves relay to positionInfoChanged for the status bar.
    connect(&m_editor, &QPlainTextEdit::cursorPositionChanged,
            this, &FileEditor::positionInfoChanged);
}

QWidget* CodeFileEditor::widget() {
    return &m_editor;
}

QString CodeFileEditor::positionInfo() {
    return QString("line: %1\tcharacter: %2")
        .arg(m_editor.textCursor().blockNumber() + 1)
        .arg(m_editor.textCursor().positionInBlock() + 1);
}

bool CodeFileEditor::doOpen() {
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QString("cannot open file for reading: %1 (%2)")
                     .arg(filePath(), file.errorString()));
        return false;
    }

    //NLang sources are UTF-8; the stream default follows the locale
    //(GBK on a Chinese Windows) and would corrupt non-ASCII bytes.
    QTextStream in(&file);
    in.setCodec("UTF-8");
    m_editor.setPlainText(in.readAll());
    return true;
}

bool CodeFileEditor::doSave() {
    //QSaveFile: atomic write -- a failed save never truncates the file.
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QString("cannot open file for writing: %1 (%2)")
                     .arg(filePath(), file.errorString()));
        return false;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << m_editor.toPlainText();
    out.flush();
    if (!file.commit()) {
        setError(QString("write failed: %1 (%2)")
                     .arg(filePath(), file.errorString()));
        return false;
    }
    return true;
}

bool CodeFileEditor::doCreate() {
    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QString("cannot create file: %1 (%2)")
                     .arg(filePath(), file.errorString()));
        return false;
    }
    file.close();
    m_editor.setPlainText(QString());
    return true;
}

} // namespace nlang
