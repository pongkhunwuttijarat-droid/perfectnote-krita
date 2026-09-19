/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Receives stylus gesture keys captured by the Android activity and maps them
 * onto the remappable stylus actions provided by the S-Pen settings extension.
 * The actions themselves are ordinary Krita actions, so the user can rebind each
 * gesture to any action through the existing stylus settings configuration.
 */

#include <QAction>
#include <QString>
#include <QTimer>

#include <KisApplication.h>
#include <KisMainWindow.h>
#include <KisPart.h>

#include "KisStylusRotationProvider.h"
#include <kactioncollection.h>
#include <kis_debug.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#else
#include <QAndroidJniEnvironment>
#endif

namespace {

/**
 * Key codes that vendor stylus hardware emits for its gestures. They are not
 * valid Android key codes, so Qt never turns them into key events; the activity
 * forwards them here instead.
 */
QString stylusGestureActionName(jint keyCode)
{
    switch (keyCode) {
    case 194:
        return QStringLiteral("stylus_squeeze");
    case 195:
        return QStringLiteral("stylus_double_tap");
    case 196:
        return QStringLiteral("stylus_slide_up");
    case 197:
        return QStringLiteral("stylus_slide_down");
    default:
        return QString();
    }
}

void triggerStylusGestureAction(jint keyCode)
{
    const QString actionName = stylusGestureActionName(keyCode);
    if (actionName.isEmpty()) {
        return;
    }

    if (!KisPart::exists()) {
        return;
    }

    KisMainWindow *mainWindow = KisPart::instance()->currentMainwindow();
    if (!mainWindow) {
        return;
    }

    QAction *action = mainWindow->actionCollection()->action(actionName);
    if (!action) {
        qWarning() << "Stylus gesture action is not available:" << actionName;
        return;
    }

    action->trigger();
}

} // namespace

/**
 * Barrel rotation from the vendor pen service. The value is decoded on the Java side
 * (axis 17 * 360 on the vendor stream, which Qt does not read) and handed through here.
 */
extern "C" JNIEXPORT void JNICALL
Java_org_krita_android_JNIWrappers_stylusRotation(JNIEnv * /*env*/, jobject /*obj*/,
                                                   jint degrees)
{
    KisStylusRotationProvider::setRotation(qreal(degrees));

    static int seen = 0;
    if (++seen % 20 == 1) {
        qDebug() << "[pen] rotation raw" << degrees
                 << "stored" << KisStylusRotationProvider::rotation()
                 << "active" << KisStylusRotationProvider::isActive()
                 << "seen" << seen;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_krita_android_JNIWrappers_stylusGestureKey(JNIEnv * /*env*/, jobject /*obj*/,
                                                    jint keyCode, jint action, jint repeatCount)
{
    /// Only the initial press matters, the vendor stream also sends releases.
    if (action != 0 /* KeyEvent.ACTION_DOWN */) {
        return;
    }

    /// Holding the squeeze gesture auto-repeats, and acting on every repeat would
    /// toggle a mapped action (the popup palette) on and off again. Measured on
    /// device: slide gestures never repeat, while the squeeze gesture can.
    if (repeatCount > 0) {
        return;
    }

    /// Never re-enter input dispatch from inside itself.
    QTimer::singleShot(0, KisApplication::instance(), [keyCode]() {
        triggerStylusGestureAction(keyCode);
    });
}
