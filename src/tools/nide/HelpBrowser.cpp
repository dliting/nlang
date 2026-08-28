/*--- HelpBrowser.cpp - embedded documentation viewer (QtWebEngine) ---*/
#include "HelpBrowser.h"

#include <QToolBar>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>

namespace nlang {

namespace {
//Reading size for a two-column manual (left nav / right content); the
//user maximizes or resizes from here.
const int HELP_WINDOW_DEFAULT_WIDTH = 1000;
const int HELP_WINDOW_DEFAULT_HEIGHT = 720;
} // namespace

HelpBrowser::HelpBrowser(QWidget* parent)
    : QDialog(parent), m_view(new QWebEngineView(this)) {
    setWindowTitle(tr("NLang Help"));
    //QDialog has no min/max buttons by default; a document window
    //wants to be maximizable.
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(HELP_WINDOW_DEFAULT_WIDTH, HELP_WINDOW_DEFAULT_HEIGHT);

    //History toolbar: the embedded view replaces the system browser's
    //navigation affordances with the page's own Back/Forward actions.
    QToolBar* navigation = new QToolBar(this);
    navigation->addAction(m_view->pageAction(QWebEnginePage::Back));
    navigation->addAction(m_view->pageAction(QWebEnginePage::Forward));

    //The test suite finds the view by name to assert the loaded page.
    m_view->setObjectName(QStringLiteral("helpWebView"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(navigation);
    layout->addWidget(m_view, 1);
}

void HelpBrowser::openPage(const QUrl& pageUrl) {
    m_view->load(pageUrl);
    show();
    raise();
    activateWindow();
}

} // namespace nlang
