/*--- SettingsDialog.h - Tools > Options form (language, build output) ---*/
#ifndef NLANG_TOOLS_NIDE_SETTINGS_DIALOG_H
#define NLANG_TOOLS_NIDE_SETTINGS_DIALOG_H

#include <QDialog>
#include <QStringList>
#include <memory>

namespace Ui {
class SettingsDialog;
}

namespace nlang {

class PathListEditor;
class SettingsStore;

//Pure form: MainWindow seeds it with the persisted values (one store
//object, not an exploding positional list) and persists the getters'
//results itself (the form tests need no QSettings; the language VALUES
//come from SettingsStore's constants). The language items carry their
//stored value ("system" | "zh" | "en") as item data.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    //Out-of-line: the unique_ptr member deletes an incomplete Ui type
    //otherwise (defined in the .cpp, where the form header lives).
    ~SettingsDialog() override;

    //Seed the form (call before exec()).
    void init(const SettingsStore& settings);
    //Current values for the caller to persist on accept.
    QString language() const;
    QString buildOutputDir() const;  // trimmed; "" = disabled
    QString toolbarIconSize() const;  // TOOLBAR_ICON_SMALL | TOOLBAR_ICON_LARGE
    QStringList librarySearchPaths() const;  // editor contents, in order
    bool noWarn() const;  // compiler-options group
    int editorFontPt() const;  // points, within the EDITOR_FONT_*_PT bounds
    int terminalFontPt() const;  // points, within the TERMINAL_FONT_*_PT bounds

private slots:
    void onBrowseDirectory();

private:
    std::unique_ptr<Ui::SettingsDialog> m_ui;
    PathListEditor* m_pathEditor = nullptr;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SETTINGS_DIALOG_H
