/*--- RunErrorBrowser.h - finished-run output browser with error navigation ---*/
#ifndef NLANG_TOOLS_NIDE_RUN_ERROR_BROWSER_H
#define NLANG_TOOLS_NIDE_RUN_ERROR_BROWSER_H

#include <QTextBrowser>
#include <QVector>

namespace nlang {

//One parsed run-output line: a "Runtime error: <msg>" message row, or
//an "  at <func> (<file>:<line>)" backtrace frame row carrying the
//site (line 0 for the `?` form — nothing to navigate to).
struct RunErrorItemInfo {
    QString filePath;   // absolute when a run base dir resolved it
    int line = 0;
    QString message;    // full raw line
};

//--- RunErrorBrowser: fills from a finished run's output tail; double-
//  clicking an entry emits itemSelected so the MainWindow can open the
//  error site in an editor. Parsing does not require a project: nvm
//  prints backtrace files as compiled-in paths (absolute); a run base
//  dir resolves the rest. Blank lines emit nothing; only the left
//  button navigates.
class RunErrorBrowser : public QTextBrowser {
    Q_OBJECT

public:
    explicit RunErrorBrowser(QWidget* parent = nullptr);

    //Replace the list with the parsed tail of one finished run:
    //message rows plus frame rows, one block per kept row. Empty/blank
    //text clears the page.
    void setRunOutput(const QString& text);

    //Parse one output line; false when it is neither shape.
    static bool parseRunLine(const QString& line, RunErrorItemInfo& info);

signals:
    void itemSelected(const RunErrorItemInfo& info);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void emitItemUnderCursor();

    //Parallel to the appended blocks (one item per block).
    QVector<RunErrorItemInfo> m_items;
};

} // namespace nlang

//uic cannot emit namespaced custom widgets; expose the class name it
//generates (MainWindow.ui promotes this class). The class stays in
//nlang::.
using nlang::RunErrorBrowser;

Q_DECLARE_METATYPE(nlang::RunErrorItemInfo)

#endif // NLANG_TOOLS_NIDE_RUN_ERROR_BROWSER_H
