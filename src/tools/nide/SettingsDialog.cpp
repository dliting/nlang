/*--- SettingsDialog.cpp - Tools > Options form ---*/
#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"

#include "SettingsStore.h"  // LANGUAGE_* item values

#include <QComboBox>
#include <QFileDialog>
#include <QPushButton>

namespace nlang {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent), m_ui(new Ui::SettingsDialog) {
    m_ui->setupUi(this);
    //Self-identified language names (each spelled in its own
    //language); only the "follow the system" entry is authored text
    //and therefore translatable.
    m_ui->cmbLanguage->addItem(tr("Follow the system language"),
                               LANGUAGE_SYSTEM);
    m_ui->cmbLanguage->addItem(QStringLiteral("中文"),
                               LANGUAGE_ZH);
    m_ui->cmbLanguage->addItem(QStringLiteral("English"),
                               LANGUAGE_EN);
    m_ui->cmbIconSize->addItem(tr("Small (32x32)"), TOOLBAR_ICON_SMALL);
    m_ui->cmbIconSize->addItem(tr("Large (48x48)"), TOOLBAR_ICON_LARGE);
    connect(m_ui->btnBrowse, &QPushButton::clicked, this,
            &SettingsDialog::onBrowseDirectory);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::init(const QString& language,
                          const QString& buildOutputDir,
                          const QString& toolbarIconSize) {
    const int index = m_ui->cmbLanguage->findData(language);
    m_ui->cmbLanguage->setCurrentIndex(index < 0 ? 0 : index);
    m_ui->edtBuildOutputDir->setText(buildOutputDir);
    //Empty stays "unset" (projects fall back to the project
    //directory), but the box shows where standalone builds land by
    //default instead of a blank field.
    m_ui->edtBuildOutputDir->setPlaceholderText(
        SettingsStore::defaultStandaloneBuildDir());
    const int iconIdx = m_ui->cmbIconSize->findData(toolbarIconSize);
    m_ui->cmbIconSize->setCurrentIndex(iconIdx < 0 ? 0 : iconIdx);
}

QString SettingsDialog::language() const {
    const QVariant data = m_ui->cmbLanguage->currentData();
    return data.isValid() ? data.toString() : LANGUAGE_SYSTEM;
}

QString SettingsDialog::buildOutputDir() const {
    return m_ui->edtBuildOutputDir->text().trimmed();
}

QString SettingsDialog::toolbarIconSize() const {
    const QVariant data = m_ui->cmbIconSize->currentData();
    return data.isValid() ? data.toString() : TOOLBAR_ICON_SMALL;
}

void SettingsDialog::onBrowseDirectory() {
    const QString current = m_ui->edtBuildOutputDir->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Build Output Directory"),
        current.isEmpty()
            ? SettingsStore::defaultStandaloneBuildDir() : current);
    if (!dir.isEmpty())
        m_ui->edtBuildOutputDir->setText(dir);
}

} // namespace nlang
