/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPenSettings.h"

#include <QAction>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QList>
#include <QMap>
#include <QModelIndex>

#include <KisMainWindow.h>
#include <kactioncollection.h>
#include <KisPart.h>
#include <kactioncategory.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <klocalizedstring.h>
#include <KisActionsSnapshot.h>
#include <kis_icon_utils.h>

KisPenSettings::KisPenSettings(QWidget *parent)
    : KisPreferenceSet(parent)
    , m_model(new QStandardItemModel())
{
    mUi = new WdgPenSettings(this);
    mUi->setupUi(this);

    m_model->setColumnCount(2);

    // Thanks to the KisActionSnapshot, we can list all actions even when no document is open
    QScopedPointer<KisActionsSnapshot> actionsSnapshot(new KisActionsSnapshot());

    KisKActionCollection *actionCollection = KisPart::instance()->currentMainwindow()->actionCollection();
    for (QAction *action: actionCollection->actions()) {
        actionsSnapshot->addAction(action->objectName(), action);
    }

    QMap<QString, KisKActionCollection*> sortedCollections = actionsSnapshot->actionCollections();
    for (KisKActionCollection* collection: sortedCollections) {
        for (QAction* action: collection->actions()) {
            QString actionName = KLocalizedString::removeAcceleratorMarker(action->text());
            QStandardItem* item = new QStandardItem(action->icon(), actionName);
            QStandardItem* actionNameItem = new QStandardItem(action->objectName());
            m_model->appendRow(QList<QStandardItem*>() << item << actionNameItem);
        }
    }

    m_model->sort(m_ACTION_TEXT_COLUMN);
    m_model->insertRow(0, new QStandardItem(i18n("Do nothing")));

    mUi->cmbSqueezeAction->setModel(m_model);
    mUi->cmbDoubleTapAction->setModel(m_model);
    mUi->cmbSlideUpAction->setModel(m_model);
    mUi->cmbSlideDownAction->setModel(m_model);

    loadPreferences();
}

KisPenSettings::~KisPenSettings()
{
    delete mUi;
    delete m_model;
}

QString KisPenSettings::id()
{
    return QString("PenSettings");
}

QString KisPenSettings::name()
{
    return header();
}

QString KisPenSettings::header()
{
    return QString(i18n("Pen"));
}

QIcon KisPenSettings::icon()
{
    return QIcon();
}

void KisPenSettings::savePreferences() const
{
    KConfigGroup cfg = KSharedConfig::openConfig()->group("PenSettings");

    cfg.writeEntry("actionSqueeze", actionNameForIndex(mUi->cmbSqueezeAction->currentIndex()));
    cfg.writeEntry("actionDoubleTap", actionNameForIndex(mUi->cmbDoubleTapAction->currentIndex()));
    cfg.writeEntry("actionSlideUp", actionNameForIndex(mUi->cmbSlideUpAction->currentIndex()));
    cfg.writeEntry("actionSlideDown", actionNameForIndex(mUi->cmbSlideDownAction->currentIndex()));

    Q_EMIT settingsChanged();
}

void KisPenSettings::loadPreferences()
{
    KConfigGroup cfg = KSharedConfig::openConfig()->group("PenSettings");

    mUi->cmbSqueezeAction->setCurrentIndex(indexFromActionName(cfg.readEntry("actionSqueeze", QString::fromLatin1(PenSettingsDefaults::Squeeze))));
    mUi->cmbDoubleTapAction->setCurrentIndex(indexFromActionName(cfg.readEntry("actionDoubleTap", QString::fromLatin1(PenSettingsDefaults::DoubleTap))));
    mUi->cmbSlideUpAction->setCurrentIndex(indexFromActionName(cfg.readEntry("actionSlideUp", QString::fromLatin1(PenSettingsDefaults::SlideUp))));
    mUi->cmbSlideDownAction->setCurrentIndex(indexFromActionName(cfg.readEntry("actionSlideDown", QString::fromLatin1(PenSettingsDefaults::SlideDown))));
}

void KisPenSettings::loadDefaultPreferences()
{
    mUi->cmbSqueezeAction->setCurrentIndex(indexFromActionName(QString::fromLatin1(PenSettingsDefaults::Squeeze)));
    mUi->cmbDoubleTapAction->setCurrentIndex(indexFromActionName(QString::fromLatin1(PenSettingsDefaults::DoubleTap)));
    mUi->cmbSlideUpAction->setCurrentIndex(indexFromActionName(QString::fromLatin1(PenSettingsDefaults::SlideUp)));
    mUi->cmbSlideDownAction->setCurrentIndex(indexFromActionName(QString::fromLatin1(PenSettingsDefaults::SlideDown)));
}

QString KisPenSettings::actionNameForIndex(int index) const
{
    QModelIndex modelIndex = m_model->index(index, m_ACTION_NAME_COLUMN);
    QString actionName = m_model->itemFromIndex(modelIndex)->data(Qt::DisplayRole).toString();
    return actionName;
}

int KisPenSettings::indexFromActionName(QString actionName) const
{
    if (actionName.isEmpty()) {
        return 0;
    } else {
        QList<QStandardItem*> itemsFound = m_model->findItems(actionName, Qt::MatchExactly, m_ACTION_NAME_COLUMN);
        if (itemsFound.size() == 0) {
            return 1;
        } else {
            return itemsFound[0]->index().row();
        }
    }
}
