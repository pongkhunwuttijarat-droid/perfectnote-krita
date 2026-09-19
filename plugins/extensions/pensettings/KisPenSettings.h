/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPENSETTINGS_H
#define KISPENSETTINGS_H

#include <QObject>
#include <QString>
#include <QStandardItemModel>
#include <kis_preference_set_registry.h>

#include "ui_wdg_pensettings.h"

class QModelIndex;

/**
 * Default gesture mapping, shared by the preferences page and the plugin.
 *
 * The page has to show the same values the plugin falls back to, otherwise opening
 * the preferences and pressing OK would write "do nothing" over a working default.
 */
namespace PenSettingsDefaults {
constexpr const char *Squeeze = "pen_show_popup_palette";
constexpr const char *DoubleTap = "erase_action";
constexpr const char *SlideUp = "increase_brush_size";
constexpr const char *SlideDown = "decrease_brush_size";
}

class WdgPenSettings : public QWidget, public Ui::WdgPenSettings
{
    Q_OBJECT

public:
    WdgPenSettings(QWidget *parent) : QWidget(parent) {
    }
};


/**
 * The "Pen" page of the preferences: one combo box per pen gesture.
 *
 * Mobile Krita has no keyboard, so gestures take the place of shortcuts. The page
 * lists the gestures the pen itself reports; each one can run any Krita action.
 */
class KisPenSettings : public KisPreferenceSet
{
    Q_OBJECT
public:
    KisPenSettings(QWidget* parent = 0);
    ~KisPenSettings() override;

    QString id() override;
    QString name() override;
    QString header() override;
    QIcon icon() override;

public Q_SLOTS:
    void savePreferences() const override;
    void loadPreferences() override;
    void loadDefaultPreferences() override;

Q_SIGNALS:
    void settingsChanged() const;

private:
    static const int m_ACTION_TEXT_COLUMN = 0;
    static const int m_ACTION_NAME_COLUMN = 1;

    QString actionNameForIndex(int index) const;
    int indexFromActionName(QString actionName) const;

    WdgPenSettings* mUi;
    QStandardItemModel* m_model;
};

class KisPenSettingsUpdateRepeater : public QObject {
    Q_OBJECT
Q_SIGNALS:
    void settingsUpdated();
public Q_SLOTS:
    void updateSettings() {
        Q_EMIT settingsUpdated();
    }
};

class KisPenSettingsFactory : public KisAbstractPreferenceSetFactory {
public:
    KisPreferenceSet* createPreferenceSet() override {
        KisPenSettings* ps = new KisPenSettings();
        QObject::connect(ps, SIGNAL(settingsChanged()), &repeater, SLOT(updateSettings()), Qt::UniqueConnection);
        return ps;
    }
    QString id() const override { return "PenSettings"; }
    KisPenSettingsUpdateRepeater repeater;
};

#endif // KISPENSETTINGS_H
