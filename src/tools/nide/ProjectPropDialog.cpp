/*--- ProjectPropDialog.cpp - project creation/properties dialog ---*/
#include "ProjectPropDialog.h"
#include "ProjectModel.h"

#include "ui_ProjectPropDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

namespace nlang {
namespace {

const char* const kProjectFileExt = ".nproj";

} // namespace

ProjectPropDialog::ProjectPropDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::ProjectPropDialog)
{
    m_ui->setupUi(this);
}

ProjectPropDialog::~ProjectPropDialog() = default;

ProjectNode* ProjectPropDialog::createProject(SolutionNode& solution) {
    initForCreate();
    m_ui->edtProjectName->setFocus();
    m_ui->edtProjectName->selectAll();

    while (true) {
        if (exec() != QDialog::Accepted)
            return nullptr;

        const QString name = m_ui->edtProjectName->text();
        const QString projectDirText = m_ui->edtProjectDir->text();

        //Validate before touching the solution. Path separators in the
        //name would escape the chosen directory; empty fields are
        //normally blocked by the disabled OK button but a programmatic
        //accept skips it. Emptiness is judged on the raw text: the
        //normalization below would turn an empty field into the CWD.
        QString problem;
        if (name.isEmpty() || projectDirText.isEmpty())
            problem = tr("Name and location must not be empty.");
        else if (name.contains('/') || name.contains('\\'))
            problem = tr("The project name must not contain path separators.");
        if (!problem.isEmpty()) {
            QMessageBox::warning(this, tr("Error"), problem);
            continue;
        }

        //Normalize before use: the exists-check below and addProject()
        //must agree on where a relative directory resolves (addProject
        //anchors relative paths at the solution dir when known, not
        //the CWD this check would use).
        const QString projectDir = QDir(projectDirText).absolutePath();

        const QString projectFilePath =
            QDir(projectDir).filePath(name + kProjectFileExt);

        //Never overwrite an existing project file; the solution's
        //addProject() rejects a path it already owns. Both keep the
        //dialog open so the user can adjust or cancel.
        if (QFileInfo::exists(projectFilePath)) {
            QMessageBox::warning(this, tr("Error"),
                tr("A project file already exists at '%1'.")
                    .arg(projectFilePath));
            continue;
        }
        ProjectNode* project = solution.addProject(projectFilePath);
        if (project == nullptr) {
            QMessageBox::warning(this, tr("Error"),
                tr("The solution already contains the project '%1'.")
                    .arg(projectFilePath));
            continue;
        }

        project->setNamespace(m_ui->edtNamespace->text());
        project->setOutputDir(m_ui->edtOutputDir->text());
        project->setIntermediateDir(m_ui->edtIntermediateDir->text());
        return project;
    }
}

bool ProjectPropDialog::editProject(ProjectNode& project) {
    initForEdit(project);
    if (exec() != QDialog::Accepted)
        return false;

    project.setNamespace(m_ui->edtNamespace->text());
    project.setOutputDir(m_ui->edtOutputDir->text());
    project.setIntermediateDir(m_ui->edtIntermediateDir->text());
    return true;
}

void ProjectPropDialog::on_btnProjectDir_clicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Project Directory"), m_ui->edtProjectDir->text());
    if (!dir.isEmpty())
        m_ui->edtProjectDir->setText(dir);
}

void ProjectPropDialog::on_btnOutputDir_clicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Output Directory"), m_ui->edtOutputDir->text());
    if (!dir.isEmpty())
        m_ui->edtOutputDir->setText(dir);
}

void ProjectPropDialog::on_btnIntermediateDir_clicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Intermediate Directory"),
        m_ui->edtIntermediateDir->text());
    if (!dir.isEmpty())
        m_ui->edtIntermediateDir->setText(dir);
}

void ProjectPropDialog::on_edtProjectName_textChanged() {
    updateSubmitEnabled();
}

void ProjectPropDialog::on_edtProjectDir_textChanged() {
    updateSubmitEnabled();
}

void ProjectPropDialog::initForCreate() {
    setWindowTitle(tr("New Project"));
    m_ui->edtProjectName->setReadOnly(false);
    m_ui->edtProjectDir->setReadOnly(false);
    m_ui->btnProjectDir->setEnabled(true);
    m_ui->edtProjectName->setText(tr("Project1"));
    m_ui->edtNamespace->setText("");
    m_ui->edtProjectDir->setText(QDir::currentPath());
    //Empty = the project directory itself (the .nproj default).
    m_ui->edtOutputDir->setText("");
    m_ui->edtIntermediateDir->setText("");
}

void ProjectPropDialog::initForEdit(const ProjectNode& project) {
    setWindowTitle(project.name() + tr(" Property"));
    m_ui->edtProjectName->setText(project.name());
    m_ui->edtNamespace->setText(project.namespace_());
    m_ui->edtProjectDir->setText(project.projectDir());
    m_ui->edtOutputDir->setText(project.outputDir());
    m_ui->edtIntermediateDir->setText(project.intermediateDir());
    //A saved project cannot be renamed or relocated from here -- the
    //solution reference and the file location belong together.
    m_ui->edtProjectName->setReadOnly(true);
    m_ui->edtProjectDir->setReadOnly(true);
    m_ui->btnProjectDir->setEnabled(false);
}

void ProjectPropDialog::updateSubmitEnabled() {
    m_ui->btnSubmit->setEnabled(
        !m_ui->edtProjectName->text().isEmpty() &&
        !m_ui->edtProjectDir->text().isEmpty());
}

} // namespace nlang
