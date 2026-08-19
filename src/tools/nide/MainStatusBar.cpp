/*--- MainStatusBar.cpp - status bar with a permanent position label ---*/
#include "MainStatusBar.h"

#include <QLabel>
#include <QLayout>

namespace nlang {

//Wide enough for "10000, 100" without the status bar resizing per keystroke.
constexpr int kPositionLabelMinWidth = 150;

MainStatusBar::MainStatusBar(QWidget* parent)
    : QStatusBar(parent)
    , m_positionLabel(new QLabel(this))
{
    layout()->setContentsMargins(0, 0, 0, 0);
    m_positionLabel->setMinimumWidth(kPositionLabelMinWidth);
    m_positionLabel->setAlignment(Qt::AlignLeft);
    addPermanentWidget(m_positionLabel);
}

void MainStatusBar::showPosition(const QString& position) {
    m_positionLabel->setText(position);
}

} // namespace nlang
