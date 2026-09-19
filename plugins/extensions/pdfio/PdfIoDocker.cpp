/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoDocker.h"
#include "PdfPageNavigator.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

namespace {

QString pageLabel(int index) {
    return QStringLiteral("Page %1").arg(index + 1);
}

} // namespace

class PdfIoDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override { return QStringLiteral("PdfIoDocker"); }

    DockPosition defaultDockPosition() const override { return DockRight; }

    QDockWidget *createDockWidget() override
    {
        PdfIoDocker *docker = new PdfIoDocker();
        docker->setObjectName(id());
        return docker;
    }
};

PdfIoDocker::PdfIoDocker()
{
    setWindowTitle(QStringLiteral("Notebook"));

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);

    m_status = new QLabel(QStringLiteral("No notebook is open"), content);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_pages = new QListWidget(content);
    layout->addWidget(m_pages);

    auto *buttons = new QHBoxLayout();
    m_previous = new QPushButton(QStringLiteral("◀"), content);
    m_next = new QPushButton(QStringLiteral("▶"), content);
    buttons->addWidget(m_previous);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);

    setWidget(content);

    connect(m_previous, &QPushButton::clicked, this, []() {
        QString why;
        PdfPageNavigator::instance()->previous(&why);
    });
    connect(m_next, &QPushButton::clicked, this, []() {
        QString why;
        PdfPageNavigator::instance()->next(&why);
    });
    connect(m_pages, &QListWidget::itemActivated, this, &PdfIoDocker::openSelected);
    connect(m_pages, &QListWidget::itemClicked, this, &PdfIoDocker::openSelected);

    /// The navigator is the single source of truth about which page is open, so the strip follows
    /// it instead of keeping a second copy of that state.
    connect(PdfPageNavigator::instance(), &PdfPageNavigator::pageChanged,
            this, &PdfIoDocker::refresh);
}

PdfIoDocker::~PdfIoDocker() = default;

void PdfIoDocker::refresh(int index, int pageCount, const QString &label)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const QDir project(navigator->projectDir());

    /// Rebuilt when the notebook changes, and the entry for the page that just changed is given a
    /// fresh icon: a page saves its thumbnail on the way out, so by the time this runs the page
    /// that was left behind has one and the list should show it.
    if (m_pages->count() != pageCount) {
        m_pages->clear();
        for (int i = 0; i < pageCount; ++i) {
            m_pages->addItem(new QListWidgetItem(pageLabel(i)));
        }
    }

    if (index >= 0 && index < pageCount && index < navigator->manifest().pages.size()) {
        const QString thumbPath =
            project.filePath(navigator->manifest().pages.at(index).thumbFile);
        if (QFileInfo::exists(thumbPath)) {
            const QPixmap pixmap(thumbPath);
            if (!pixmap.isNull()) {
                m_pages->item(index)->setIcon(QIcon(pixmap.scaled(128, 128, Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation)));
            }
        }
    }

    m_status->setText(pageCount > 0
                          ? QStringLiteral("%1 — page %2 of %3").arg(label).arg(index + 1).arg(pageCount)
                          : QStringLiteral("No notebook is open"));

    if (index >= 0 && index < m_pages->count()) {
        const QSignalBlocker blocker(m_pages);
        m_pages->setCurrentRow(index);
    }

    m_previous->setEnabled(index > 0);
    m_next->setEnabled(index >= 0 && index + 1 < pageCount);
}

void PdfIoDocker::openSelected()
{
    const int row = m_pages->currentRow();
    if (row < 0 || row == PdfPageNavigator::instance()->currentIndex()) {
        return;
    }

    QString why;
    PdfPageNavigator::instance()->showPage(row, &why);
}

void registerPdfIoDocker()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    KoDockRegistry::instance()->add(new PdfIoDockFactory());
}
