/*--- FileEditor.h - abstract file editor and editor registry for NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_FILE_EDITOR_H
#define NLANG_TOOLS_NIDE_FILE_EDITOR_H

#include <QObject>
#include <QString>

#include <map>
#include <memory>

class QWidget;

namespace nlang {

class EditorManager;

//--- FileEditor: abstract "one open file" editor.
//  Tracks the absolute file path and the dirty state; subclasses provide
//  the widget and the load/save/create operations. Failure paths return
//  false and leave a reason in lastError() -- the UI layer (not this
//  class) decides how to present it or whether to ask the user first.
class FileEditor : public QObject {
    Q_OBJECT

public:
    //Does not touch the file system.
    FileEditor(EditorManager& owner, const QString& absoluteFilePath);
    ~FileEditor() override = default;

    FileEditor(const FileEditor&) = delete;
    FileEditor& operator=(const FileEditor&) = delete;

    //Absolute, cleaned (QFileInfo::absoluteFilePath).
    const QString& filePath() const { return m_filePath; }

    //The widget shown for this editor (owned by the editor).
    virtual QWidget* widget() = 0;

    bool dirty() const { return m_dirty; }

    //Save when dirty; a clean editor is a no-op success.
    bool save();

    //Save under a new absolute path, overwriting it without asking (the
    //caller confirms first). Fails without touching anything when the
    //path is already open in another editor. The owning manager follows
    //the rename.
    bool saveAs(const QString& absoluteFilePath);

    //Load the file into the editor and mark it clean.
    bool open();

    //Create the file on disk and edit it; fails if it already exists.
    bool create();

    //Human-readable cursor position, e.g. for a status bar.
    virtual QString positionInfo() = 0;

    const QString& lastError() const { return m_lastError; }

signals:
    //Emitted when the cursor moves (relayed from the editing widget);
    //the UI updates its position display.
    void positionInfoChanged();

protected slots:
    //Wire the editing widget's textChanged here: any edit marks dirty
    //unless a programmatic load is in progress.
    void onTextChange();

protected:
    //Load/save/create the file. On failure set a reason via setError
    //and return false. doOpen/doCreate must not touch the dirty flag --
    //open()/create() wrap them with the ignore guard.
    virtual bool doSave() = 0;
    virtual bool doOpen() = 0;
    virtual bool doCreate() = 0;

    void setError(const QString& message) { m_lastError = message; }

    //Marks dirty and keeps the widget title "*" in sync (the UI shows
    //the title as the tab text).
    void markDirty(bool dirty);

private:
    friend class EditorManager;

    void updateWidgetTitle();

    bool m_dirty = false;
    bool m_ignoreTextChange = false;  // true while loading text programmatically
    QString m_filePath;
    QString m_lastError;
    EditorManager& m_owner;
};

//--- EditorManager: registry of open editors, keyed by absolute path.
//  The save-before-close prompt belongs to the UI layer; remove()
//  closes unconditionally.
class EditorManager : public QObject {
    Q_OBJECT

public:
    explicit EditorManager(QObject* parent = nullptr);
    ~EditorManager() override;

    EditorManager(const EditorManager&) = delete;
    EditorManager& operator=(const EditorManager&) = delete;

    //Create a new file on disk and open it in a new editor. Fails when
    //the file already exists or is already open here.
    FileEditor* openNew(const QString& absoluteFilePath);

    //Open a file; an already-open file returns its existing editor.
    FileEditor* open(const QString& absoluteFilePath);

    //Close and delete an editor (returns false when not owned here).
    //The editor pointer is invalid afterwards.
    bool remove(FileEditor* editor);

    //Close and delete every editor.
    void clear();

    FileEditor* find(const QString& absoluteFilePath) const;

    size_t size() const { return m_editors.size(); }

    //Reason of the last failed openNew/open (empty on success).
    const QString& lastError() const { return m_lastError; }

signals:
    void saveStateChanged(FileEditor* editor);

private:
    friend class FileEditor;

    //Editor factory: the editor type for a given file.
    FileEditor* createEditor(const QString& absoluteFilePath);

    //Adopt a new key after a successful saveAs.
    void rekey(const QString& oldPath, const QString& newPath);

    std::map<QString, std::unique_ptr<FileEditor>> m_editors;
    QString m_lastError;
};

} // namespace nlang

//FileEditor* travels through signal arguments (QSignalSpy); it needs a
//metatype declaration for QVariant::value to work.
Q_DECLARE_METATYPE(nlang::FileEditor*)

#endif // NLANG_TOOLS_NIDE_FILE_EDITOR_H
