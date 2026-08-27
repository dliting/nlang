/*--- HelpWindow.h - read-only markdown help viewer ---*/
#ifndef NLANG_TOOLS_NIDE_HELP_WINDOW_H
#define NLANG_TOOLS_NIDE_HELP_WINDOW_H

#include <QDialog>

class QTextBrowser;

namespace nlang {

//Renders one shipped markdown document (docs/ beside the IDE).
//Non-modal by design: MainWindow keeps at most one per document.
class HelpWindow : public QDialog {
    Q_OBJECT

public:
    explicit HelpWindow(const QString& documentBaseName,
                        QWidget* parent = nullptr);

    //True when the markdown was found and rendered; false leaves a
    //visible not-found notice (normal for a bin-only copy without
    //docs/; the deploy-check scratch still reaches the repo's).
    bool documentLoaded() const { return m_loaded; }

private:
    QTextBrowser* m_browser;
    bool m_loaded = false;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_HELP_WINDOW_H
