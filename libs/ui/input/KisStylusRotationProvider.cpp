/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStylusRotationProvider.h"

#include <QElapsedTimer>

#include <KoPointerEvent.h>

namespace KisStylusRotationProvider {
namespace {

/// The stream is only trusted for this long after the last sample.
const int MAX_AGE_MS = 1000;

QElapsedTimer &ageTimer()
{
    static QElapsedTimer timer;
    return timer;
}

qreal &storedRotation()
{
    static qreal value = 0.0;
    return value;
}

bool &hasValue()
{
    static bool value = false;
    return value;
}

} // namespace

void setRotation(qreal degrees)
{
    storedRotation() = degrees;
    hasValue() = true;
    ageTimer().restart();
}

void clear()
{
    hasValue() = false;
}

bool isActive()
{
    if (!hasValue()) {
        return false;
    }
    return ageTimer().isValid() && ageTimer().elapsed() <= MAX_AGE_MS;
}

qreal rotation()
{
    return storedRotation();
}

qreal rotationFor(const KoPointerEvent *event)
{
    return isActive() ? rotation() : event->rotation();
}

} // namespace KisStylusRotationProvider
