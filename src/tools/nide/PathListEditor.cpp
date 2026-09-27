/*--- PathListEditor.cpp - reusable directory-path list editor ---*/
#include "PathListEditor.h"
#include "ProjectPaths.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace nlang {

PathListEditor::PathListEditor(const QString& title, QWidget* parent)
    : QGroupBox(title, parent), m_list(new QListWidget(this)) {
    auto* add = new QPushButton(tr("Add..."), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* up = new QPushButton(tr("Move Up"), this);
    auto* down = new QPushButton(tr("Move Down"), this);

    auto* buttons = new QVBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addStretch();

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(m_list);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &PathListEditor::onAdd);
    connect(remove, &QPushButton::clicked, this, &PathListEditor::onRemove);
    connect(up, &QPushButton::clicked, this, &PathListEditor::onUp);
    connect(down, &QPushButton::clicked, this, &PathListEditor::onDown);
}

QStringList PathListEditor::paths() const {
    QStringList result;
    for (int i = 0; i < m_list->count(); ++i)
        result.append(m_list->item(i)->text());
    return result;
}

void PathListEditor::setPaths(const QStringList& paths) {
    m_list->clear();
    for (const QString& p : paths)
        addPath(p);
}

void PathListEditor::addPath(const QString& dir) {
    const QString d = dir.trimmed();
    if (d.isEmpty())
        return;
    const QString key = dedupKey(d);
    for (int i = 0; i < m_list->count(); ++i)
        if (dedupKey(m_list->item(i)->text()) == key)
            return;
    m_list->addItem(d);
}

bool PathListEditor::moveRow(int index, int delta) {
    const int target = index + delta;
    if (index < 0 || index >= m_list->count()
            || target < 0 || target >= m_list->count())
        return false;
    QListWidgetItem* item = m_list->takeItem(index);
    m_list->insertItem(target, item);
    return true;
}

void PathListEditor::onAdd() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Library Directory"));
    if (!dir.isEmpty())
        addPath(dir);
}

void PathListEditor::onRemove() {
    const int row = m_list->currentRow();
    if (row >= 0)
        delete m_list->takeItem(row);
}

void PathListEditor::onUp() {
    const int row = m_list->currentRow();
    if (moveRow(row, -1))
        m_list->setCurrentRow(row - 1);
}

void PathListEditor::onDown() {
    const int row = m_list->currentRow();
    if (moveRow(row, 1))
        m_list->setCurrentRow(row + 1);
}

} // namespace nlang
