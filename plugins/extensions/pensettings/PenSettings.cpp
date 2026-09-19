/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PenSettings.h"
#include <KisPenSettings.h>

#include <KisMainWindow.h>
#include <kpluginfactory.h>
#include <kis_action.h>
#include <KisViewManager.h>
#include <kis_action_manager.h>
#include <KisPart.h>
#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <kis_preference_set_registry.h>
#include <kis_canvas2.h>
#include <input/kis_input_manager.h>
#include <kis_canvas_controller.h>
#include <kis_popup_palette.h>


K_PLUGIN_FACTORY_WITH_JSON(PenSettingsFactory, "kritapensettings.json", registerPlugin<PenSettings>();)


PenSettings::PenSettings(QObject* parent, const QVariantList&)
    : KisActionPlugin(parent)
{
    // Krita has no dedicated popup palette action, so provide one for the pen.
    m_actionShowPopupPalette.reset(viewManager()->actionManager()->createAction("pen_show_popup_palette"));
    connect(m_actionShowPopupPalette.data(), SIGNAL(triggered()), this, SLOT(slotTriggerPopupPalette()));

    KisPreferenceSetRegistry* preferenceSetRegistry = KisPreferenceSetRegistry::instance();
    KisPenSettingsFactory* settingsFactory = new KisPenSettingsFactory();
    preferenceSetRegistry->add("KisPenSettingsFactory", settingsFactory);

    // Reload the mapping whenever it is changed in the preferences.
    connect(&(settingsFactory->repeater), SIGNAL(settingsUpdated()), this, SLOT(slotLoadSettings()), Qt::UniqueConnection);

    slotLoadSettings();

    KisAction* actionSqueeze = viewManager()->actionManager()->createAction("stylus_squeeze");
    connect(actionSqueeze, &KisAction::triggered, this, [this]() { slotActivateAction(Gesture::Squeeze); });

    KisAction* actionDoubleTap = viewManager()->actionManager()->createAction("stylus_double_tap");
    connect(actionDoubleTap, &KisAction::triggered, this, [this]() { slotActivateAction(Gesture::DoubleTap); });

    KisAction* actionSlideUp = viewManager()->actionManager()->createAction("stylus_slide_up");
    connect(actionSlideUp, &KisAction::triggered, this, [this]() { slotActivateAction(Gesture::SlideUp); });

    KisAction* actionSlideDown = viewManager()->actionManager()->createAction("stylus_slide_down");
    connect(actionSlideDown, &KisAction::triggered, this, [this]() { slotActivateAction(Gesture::SlideDown); });
}

PenSettings::~PenSettings()
{
}

void PenSettings::slotActivateAction(PenSettings::Gesture gesture)
{
    const QString actionName = m_actionMap.value(gesture);

    if (!actionName.isEmpty()) {
        KisKActionCollection* actionCollection = KisPart::instance()->currentMainwindow()->actionCollection();
        QAction* action = actionCollection->action(actionName);
        if (action) {
            action->trigger();
        }
    }
}

void PenSettings::slotLoadSettings()
{
    m_actionMap.clear();

    KConfigGroup cfg = KSharedConfig::openConfig()->group("PenSettings");

    m_actionMap.insert(Gesture::Squeeze, cfg.readEntry("actionSqueeze", QString::fromLatin1(PenSettingsDefaults::Squeeze)));
    m_actionMap.insert(Gesture::DoubleTap, cfg.readEntry("actionDoubleTap", QString::fromLatin1(PenSettingsDefaults::DoubleTap)));
    m_actionMap.insert(Gesture::SlideUp, cfg.readEntry("actionSlideUp", QString::fromLatin1(PenSettingsDefaults::SlideUp)));
    m_actionMap.insert(Gesture::SlideDown, cfg.readEntry("actionSlideDown", QString::fromLatin1(PenSettingsDefaults::SlideDown)));
}

void PenSettings::slotTriggerPopupPalette()
{
    if (KisPart::instance()->currentInputManager()->canvas()) {
        // determine the current location of cursor on the screen, for popup palette placement
        QPoint cursorPosition = KisPart::instance()->currentInputManager()->canvas()->canvasWidget()->mapFromGlobal(QCursor::pos());
        KisPopupPalette *popupPalette = KisPart::instance()->currentInputManager()->canvas()->popupPalette();
        if (popupPalette) {
            if (popupPalette->isVisible()) {
                popupPalette->dismiss();
            } else {
                popupPalette->popup(cursorPosition);
            }
        }
    }
}

#include "PenSettings.moc"
