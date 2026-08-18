/*--- NewFileDialog.h - "new source file" dialog for the NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_NEW_FILE_DIALOG_H
#define NLANG_TOOLS_NIDE_NEW_FILE_DIALOG_H

#include <QDialog>
#include <memory>

namespace nlang {

class ProjectNode;

} // namespace nlang

namespace Ui {
class NewFileDialog;
}

namespace nlang {

//--- NewFileDialog: collects the name and directory for a new source
//  file. Deviations from EN's NewFileDiaLog: the "add to project" combo
//  is gone -- EN filled it but no code ever read it (dead UI); a new
//  file reaches its project through the solution-tree selection
//  (MainWindow, Step 7). The default directory is the project directory
//  itself -- NLang sources sit next to their .nproj, not in a src/
//  child directory.
class NewFileDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewFileDialog(QWidget* parent = nullptr);
    //Out-of-line: the unique_ptr member deletes an incomplete Ui type
    //otherwise (defined in the .cpp, where the form header lives).
    ~NewFileDialog() override;

    //Reset to defaults: name "Untitled.n" (stem selected so typing
    //replaces it), directory = the project's directory, or the current
    //working directory without a project.
    void init(ProjectNode* currentProject = nullptr);

    //Run modally; false on cancel. On accept filePath receives the
    //absolute path built from the entered directory and file name.
    bool getFilePath(QString& filePath);

private slots:
    void on_edtName_textChanged();
    void on_edtDirectory_textChanged();
    void on_btnDirectory_clicked();

private:
    //OK needs both a name and a directory.
    void updateSubmitEnabled();

    std::unique_ptr<Ui::NewFileDialog> m_ui;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_NEW_FILE_DIALOG_H
