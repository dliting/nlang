/*--- FileEditor.cpp - abstract file editor and editor registry for NLang IDE ---*/
#include "FileEditor.h"

#include "CodeEditor.h"

#include <QFileInfo>

namespace nlang {

//Map key for open editors: absolute/cleaned, and on Windows case-folded
//-- two spellings of the same physical file must map to one editor
//(same rule as ProjectModel's dedupKey and ncc's SourceKey).
static QString editorKey(const QString& absoluteFilePath)
{
    QString key = QFileInfo(absoluteFilePath).absoluteFilePath();
#ifdef _WIN32
    key = key.toLower();
#endif
    return key;
}

//--- FileEditor ---

FileEditor::FileEditor(EditorManager& owner, const QString& absoluteFilePath)
    : m_filePath(QFileInfo(absoluteFilePath).absoluteFilePath())
    , m_owner(owner)
{
    //A relative path would key the manager wrong; catch it in debug builds.
    Q_ASSERT(QFileInfo(absoluteFilePath).isAbsolute());
}

bool FileEditor::save() {
    if (!m_dirty)
        return true;
    if (!doSave())
        return false;
    markDirty(false);
    return true;
}

bool FileEditor::saveAs(const QString& absoluteFilePath) {
    QString newPath = QFileInfo(absoluteFilePath).absoluteFilePath();
    if (newPath == m_filePath)
        return save();

    //Saving over another open editor's file would overwrite that
    //editor's map entry and delete it mid-flight -- only this layer can
    //see the collision, so reject it before anything is written.
    FileEditor* clash = m_owner.find(newPath);
    if (clash != nullptr && clash != this) {
        setError(QString("file is already open in another editor: %1").arg(newPath));
        return false;
    }

    //Only a successful write moves the editor: on failure the path, the
    //manager key and the dirty flag all stay as they were.
    QString oldPath = m_filePath;
    m_filePath = newPath;
    if (!doSave()) {
        m_filePath = oldPath;
        return false;
    }
    markDirty(false);
    m_owner.rekey(oldPath, newPath);
    return true;
}

void FileEditor::onExternalRename(const QString& newAbsolutePath) {
    QString newPath = QFileInfo(newAbsolutePath).absoluteFilePath();
    if (newPath == m_filePath)
        return;

    //The file already moved on disk; only the editor's view of it
    //changes. rekey looks the editor up under its OLD path (same
    //ordering discipline as saveAs: re-point first, rekey after).
    QString oldPath = m_filePath;
    m_filePath = newPath;
    m_owner.rekey(oldPath, newPath);
    updateWidgetTitle();
    //createEditor keyed the accessible name to the original path.
    widget()->setAccessibleName(newPath);
}

bool FileEditor::open() {
    m_ignoreTextChange = true;
    bool ok = doOpen();
    m_ignoreTextChange = false;
    if (ok)
        markDirty(false);
    return ok;
}

bool FileEditor::create() {
    if (QFileInfo(m_filePath).exists()) {
        setError(QString("file already exists: %1").arg(m_filePath));
        return false;
    }
    m_ignoreTextChange = true;
    bool ok = doCreate();
    m_ignoreTextChange = false;
    if (ok)
        markDirty(false);
    return ok;
}

void FileEditor::onTextChange() {
    if (!m_ignoreTextChange)
        markDirty(true);
}

void FileEditor::markDirty(bool dirty) {
    bool changed = (m_dirty != dirty);
    m_dirty = dirty;
    //Title refresh sits outside the flip check on purpose: after a clean
    //editor's saveAs there is no dirty flip, yet the file name on screen
    //must still change.
    updateWidgetTitle();
    if (changed)
        emit m_owner.saveStateChanged(this);
}

void FileEditor::updateWidgetTitle() {
    QString title = QFileInfo(m_filePath).fileName();
    widget()->setWindowTitle(m_dirty ? title + QLatin1Char('*') : title);
}

//--- EditorManager ---

EditorManager::EditorManager(QObject* parent)
    : QObject(parent)
{
}

EditorManager::~EditorManager() {
    clear();
}

FileEditor* EditorManager::createEditor(const QString& absoluteFilePath) {
    FileEditor* editor = new CodeFileEditor(*this, absoluteFilePath);
    //Screen readers and UI automation identify the editor by its file.
    editor->widget()->setAccessibleName(absoluteFilePath);
    return editor;
}

FileEditor* EditorManager::openNew(const QString& absoluteFilePath) {
    //The file may be gone from disk but still open here; inserting under
    //the same key would silently delete that editor.
    if (find(absoluteFilePath) != nullptr) {
        m_lastError = QString("file is already open: %1").arg(absoluteFilePath);
        return nullptr;
    }

    std::unique_ptr<FileEditor> editor(createEditor(absoluteFilePath));
    if (!editor->create()) {
        m_lastError = editor->lastError();
        return nullptr;  // unique_ptr deletes the failed editor
    }
    FileEditor* raw = editor.get();
    m_editors[editorKey(editor->filePath())] = std::move(editor);
    return raw;
}

FileEditor* EditorManager::open(const QString& absoluteFilePath) {
    auto it = m_editors.find(editorKey(absoluteFilePath));
    if (it != m_editors.end())
        return it->second.get();

    std::unique_ptr<FileEditor> editor(createEditor(absoluteFilePath));
    if (!editor->open()) {
        m_lastError = editor->lastError();
        return nullptr;  // unique_ptr deletes the failed editor
    }
    FileEditor* raw = editor.get();
    m_editors[editorKey(editor->filePath())] = std::move(editor);
    return raw;
}

bool EditorManager::remove(FileEditor* editor) {
    for (auto it = m_editors.begin(); it != m_editors.end(); ++it) {
        if (it->second.get() == editor) {
            m_editors.erase(it);
            return true;
        }
    }
    return false;
}

void EditorManager::clear() {
    m_editors.clear();
}

FileEditor* EditorManager::find(const QString& absoluteFilePath) const {
    auto it = m_editors.find(editorKey(absoluteFilePath));
    return it == m_editors.end() ? nullptr : it->second.get();
}

FileEditor* EditorManager::findEditor(const QWidget* widget) const {
    for (const auto& entry : m_editors)
        if (entry.second->widget() == widget)
            return entry.second.get();
    return nullptr;
}

std::vector<FileEditor*> EditorManager::editors() const {
    std::vector<FileEditor*> result;
    result.reserve(m_editors.size());
    for (const auto& entry : m_editors)
        result.push_back(entry.second.get());
    return result;
}

void EditorManager::rekey(const QString& oldPath, const QString& newPath) {
    auto it = m_editors.find(editorKey(oldPath));
    if (it == m_editors.end())
        return;
    std::unique_ptr<FileEditor> editor = std::move(it->second);
    m_editors.erase(it);
    m_editors[editorKey(newPath)] = std::move(editor);
}

} // namespace nlang
