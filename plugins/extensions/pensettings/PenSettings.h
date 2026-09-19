/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PENSETTINGS_H
#define PENSETTINGS_H

#include <QMap>
#include <QObject>
#include <QScopedPointer>

#include <KisActionPlugin.h>

class KisAction;

/**
 * Maps the barrel gestures of a pen that reports them itself onto Krita actions.
 *
 * On Android the stylus service of the tablet delivers "squeeze", "double tap",
 * "slide up" and "slide down" as separate key events. The platform bridge turns
 * those into the `stylus_*` actions created here, and this plugin decides which
 * Krita action each one actually runs. The mapping is configured on the "Pen"
 * page of the preferences, which is the mobile friendly equivalent of a shortcut.
 *
 * Barrel rotation is not an action: it is fed straight into the "Rotation" sensor
 * of the brush engine, so it is configured per brush preset instead.
 */
class PenSettings : public KisActionPlugin
{
    Q_OBJECT
public:
    PenSettings(QObject *parent, const QVariantList &);
    ~PenSettings() override;

    enum Gesture {
        Squeeze,
        DoubleTap,
        SlideUp,
        SlideDown
    };

public Q_SLOTS:
    void slotActivateAction(PenSettings::Gesture gesture);
    void slotLoadSettings();
    void slotTriggerPopupPalette();

private:
    QMap<PenSettings::Gesture, QString> m_actionMap;
    QScopedPointer<KisAction> m_actionShowPopupPalette;
};

#endif // PENSETTINGS_H
