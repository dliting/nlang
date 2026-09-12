/*--- CompileLogBrowser.h - compiler output browser with error navigation ---*/
#ifndef NLANG_TOOLS_NIDE_COMPILE_LOG_BROWSER_H
#define NLANG_TOOLS_NIDE_COMPILE_LOG_BROWSER_H

#include "ProjectModel.h"

#include <QTextBrowser>

namespace nlang {

class ProjectNode;

//One parsed log line. For an ncc diagnostic line it carries the error
//site (file/line/column); otherwise just the raw text as the message.
struct CompileLogItemInfo {
    QString filePath;   // absolute when a project resolved it
    size_t line = 0;
    size_t column = 0;  // "char N" as printed: 1-based (QTextCursor is 0-based)
    QString message;
};

//--- CompileLogBrowser: shows compiler output; double-clicking a
//  diagnostic line emits lineSelected so the MainWindow can open the
//  error site in an editor. Design notes: parsing does not require
//  a project -- ncc prints the path as given (absolute in project mode,
//  otherwise as typed on the command line), so a project only resolves
//  relative paths; blank lines emit nothing; only the left button
//  navigates.
class CompileLogBrowser : public QTextBrowser {
    Q_OBJECT

public:
    explicit CompileLogBrowser(QWidget* parent = nullptr);

    ProjectNode* project() const { return m_project; }
    void setProject(ProjectNode* project) { m_project = project; }

    //Parse "<file>(line N, char N): <message>" (the ncc diagnostic
    //shape) into info; false when the line does not match. filePath is
    //as captured -- resolution against the project is the caller's job.
    static bool parseLogLine(const QString& logLine, CompileLogItemInfo& info);

signals:
    void lineSelected(const CompileLogItemInfo& info);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void emitLineUnderCursor();

    ProjectNode* m_project = nullptr;
};

} // namespace nlang

//uic cannot emit namespaced custom widgets; expose the class name it
//generates (MainWindow.ui promotes this class). The class stays in
//nlang::.
using nlang::CompileLogBrowser;

Q_DECLARE_METATYPE(nlang::CompileLogItemInfo)

#endif // NLANG_TOOLS_NIDE_COMPILE_LOG_BROWSER_H
