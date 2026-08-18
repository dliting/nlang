/*--- NewFileDialog.cpp - "new source file" dialog for the NLang IDE ---*/
#include "NewFileDialog.h"
#include "ProjectModel.h"

#include "ui_NewFileDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

namespace nlang {

NewFileDialog::NewFileDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::NewFileDialog)
{
    m_ui->setupUi(this);
}

NewFileDialog::~NewFileDialog() = default;

void NewFileDialog::init(ProjectNode* currentProject) {
    //A fresh "Untitled.n" per run; the stem is selected so typing a new
    //name keeps the ".n" extension.
    const QString defaultName = tr("Untitled.n");
    m_ui->edtName->setText(defaultName);
    m_ui->edtName->setSelection(0, defaultName.lastIndexOf('.'));

    m_ui->edtDirectory->setText(
        currentProject ? currentProject->projectDir() : QDir::currentPath());
    updateSubmitEnabled();
}

bool NewFileDialog::getFilePath(QString& filePath) {
    if (exec() != QDialog::Accepted)
        return false;
    filePath = QFileInfo(m_ui->edtDirectory->text(),
                         m_ui->edtName->text()).absoluteFilePath();
    return true;
}

void NewFileDialog::on_edtName_textChanged() {
    updateSubmitEnabled();
}

void NewFileDialog::on_edtDirectory_textChanged() {
    updateSubmitEnabled();
}

void NewFileDialog::on_btnDirectory_clicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Directory"), m_ui->edtDirectory->text());
    if (!dir.isEmpty())
        m_ui->edtDirectory->setText(dir);
}

void NewFileDialog::updateSubmitEnabled() {
    m_ui->btnSubmit->setEnabled(
        !m_ui->edtName->text().isEmpty() &&
        !m_ui->edtDirectory->text().isEmpty());
}

} // namespace nlang
