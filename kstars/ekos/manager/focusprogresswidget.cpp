/*
    SPDX-FileCopyrightText: 2012 Jasem Mutlaq <mutlaqja@ikartech.com>
    SPDX-FileCopyrightText: 2021 Wolfgang Reissenberger <sterne-jaeger@openfuture.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "focusprogresswidget.h"
#include "kstarsdata.h"
#include "Options.h"

namespace Ekos
{
FocusProgressWidget::FocusProgressWidget(QWidget * parent) : QWidget(parent)
{
    setupUi(this);
}

void FocusProgressWidget::updateCurrentHFR(double newHFR)
{
    currentHFR->setText(QString("%1").arg(newHFR, 0, 'f', 2) + " px");
    profilePlot->drawProfilePlot(newHFR);
}

void FocusProgressWidget::updateFocusDetailView()
{
    const int pos = focusDetailView->currentIndex();
    if (pos == 1 && focusStarPixmap.get() != nullptr)
    {
        focusStarView->setPixmap(focusStarPixmap.get()->scaled(focusDetailView->width(), focusDetailView->height(),
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void FocusProgressWidget::updateFocusStarPixmap(QPixmap &starPixmap)
{
    if (starPixmap.isNull())
        return;

    focusStarPixmap.reset(new QPixmap(starPixmap));
    updateFocusDetailView();
}

void FocusProgressWidget::updateFocusStatus(Ekos::FocusState status)
{
    focusStatus->setFocusState(status);
}

void FocusProgressWidget::init()
{

    // focus details buttons
    connect(focusDetailNextButton, &QPushButton::clicked, [this]()
    {
        const int pos = focusDetailView->currentIndex();
        if (pos == 0 || (pos == 1 && focusStarPixmap.get() != nullptr))
            focusDetailView->setCurrentIndex(pos + 1);
        else if (pos > 0)
            focusDetailView->setCurrentIndex(0);
        updateFocusDetailView();
    });
    connect(focusDetailPrevButton, &QPushButton::clicked, [this]()
    {
        const int pos = focusDetailView->currentIndex();
        if (pos == 0 && focusStarPixmap.get() != nullptr)
            focusDetailView->setCurrentIndex(pos + 2);
        else if (pos == 0)
            focusDetailView->setCurrentIndex(pos + 1);
        else if (pos > 0)
            focusDetailView->setCurrentIndex(pos - 1);
        updateFocusDetailView();
    });
}

void FocusProgressWidget::reset()
{
    focusStatus->setFocusState(FOCUS_IDLE);
}

} // namespace Ekos
