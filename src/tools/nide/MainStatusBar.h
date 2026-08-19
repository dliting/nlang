/*--- MainStatusBar.h - status bar with a permanent position label ---*/
#ifndef NLANG_TOOLS_NIDE_MAIN_STATUS_BAR_H
#define NLANG_TOOLS_NIDE_MAIN_STATUS_BAR_H

#include <QStatusBar>

class QLabel;

namespace nlang {

//--- MainStatusBar: one permanent label showing the editor cursor
//  position text. Promoted into MainWindow.ui; the global
//  using-declaration below lets uic's unqualified generated code see
//  the nlang:: class.
class MainStatusBar : public QStatusBar {
    Q_OBJECT

public:
    explicit MainStatusBar(QWidget* parent = nullptr);

    //Empty text clears the display (no editor open).
    void showPosition(const QString& position);

private:
    QLabel* m_positionLabel;
};

} // namespace nlang

//uic cannot emit namespaced custom widgets; expose the class name it
//generates. The class itself stays in nlang::.
using nlang::MainStatusBar;

#endif // NLANG_TOOLS_NIDE_MAIN_STATUS_BAR_H
