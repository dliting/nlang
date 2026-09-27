/*--- PathListEditor.h - reusable editor for an ordered, de-duplicated list
    of directory paths (a QGroupBox: list + Add/Remove/Move-Up/Move-Down).
    Used by both the global Settings dialog and the project Properties
    dialog so the behavior lives in exactly one place. ---*/
#ifndef NLANG_TOOLS_NIDE_PATH_LIST_EDITOR_H
#define NLANG_TOOLS_NIDE_PATH_LIST_EDITOR_H

#include <QGroupBox>
#include <QStringList>

class QListWidget;
class QListWidgetItem;

namespace nlang {

class PathListEditor : public QGroupBox {
    Q_OBJECT

public:
    explicit PathListEditor(const QString& title,
                            QWidget* parent = nullptr);

    QStringList paths() const;
    void setPaths(const QStringList& paths);
    //Append dir unless its normalized key is already present (trimmed;
    //blank entries ignored).
    void addPath(const QString& dir);

public slots:
    void onAdd();     // browse for a directory, then add it
    void onRemove();
    void onUp();
    void onDown();

private:
    //Move the row at index by delta; false at the bounds.
    bool moveRow(int index, int delta);

    QListWidget* m_list;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_PATH_LIST_EDITOR_H
