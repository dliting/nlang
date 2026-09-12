/*--- ProjectPropDialog.h - project creation/properties dialog ---*/
#ifndef NLANG_TOOLS_NIDE_PROJECT_PROP_DIALOG_H
#define NLANG_TOOLS_NIDE_PROJECT_PROP_DIALOG_H

#include <QDialog>
#include <memory>

namespace nlang {

class ProjectNode;
class SolutionNode;

} // namespace nlang

namespace Ui {
class ProjectPropDialog;
}

namespace nlang {

//--- ProjectPropDialog: one form, two modes. createProject() adds a
//  new .nproj to the solution (retrying on conflicts); editProject()
//  changes the mutable properties of an existing project (name and
//  location are fixed once the project exists).
//  Design notes: the empty output/intermediate directories
//  default to the project directory (the .nproj omits the
//  attributes when empty);
//  the "referenced packages" list is gone (the NLang project format
//  has no such element); the namespace is optional (ncc does not read
//  it) and editProject() actually applies it; the namespace label's
//  buddy now points at edtNamespace.
class ProjectPropDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectPropDialog(QWidget* parent = nullptr);
    //Out-of-line: the unique_ptr member deletes an incomplete Ui type
    //otherwise (defined in the .cpp, where the form header lives).
    ~ProjectPropDialog() override;

    //Modal creation loop: exec() until the values are acceptable or
    //the user cancels. Returns the project added to the solution, or
    //nullptr on cancel.
    ProjectNode* createProject(SolutionNode& solution);

    //Modal edit of namespace/output/intermediate directories; false on
    //cancel (the project is then untouched).
    bool editProject(ProjectNode& project);

private slots:
    void on_btnProjectDir_clicked();
    void on_btnOutputDir_clicked();
    void on_btnIntermediateDir_clicked();
    void on_edtProjectName_textChanged();
    void on_edtProjectDir_textChanged();

private:
    //Defaults for a fresh project ("Project1" in the working directory).
    void initForCreate();
    //The project's current values; identity fields become read-only.
    void initForEdit(const ProjectNode& project);

    //OK needs a name and a location.
    void updateSubmitEnabled();

    std::unique_ptr<Ui::ProjectPropDialog> m_ui;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_PROJECT_PROP_DIALOG_H
