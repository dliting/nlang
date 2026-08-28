/*--- HelpBrowser.h - embedded documentation viewer (QtWebEngine) ---*/
#ifndef NLANG_TOOLS_NIDE_HELP_BROWSER_H
#define NLANG_TOOLS_NIDE_HELP_BROWSER_H

#include <QDialog>
#include <QUrl>

class QWebEngineView;

namespace nlang {

//HelpBrowser: shows the generated docs site inside the IDE instead of
//handing pages to the system browser. Non-modal; closes to nothing
//(WA_DeleteOnClose) so the Chromium render process is released with
//the window -- MainWindow re-creates it through a QPointer on the
//next Help menu entry.
class HelpBrowser : public QDialog {
    Q_OBJECT

public:
    explicit HelpBrowser(QWidget* parent = nullptr);

    //Load the page and bring the window to front. Reused across Help
    //menu entries (navigating to another page in the same window).
    void openPage(const QUrl& pageUrl);

private:
    QWebEngineView* m_view;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_HELP_BROWSER_H
