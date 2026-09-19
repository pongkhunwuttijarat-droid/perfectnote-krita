/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStylusRotationProvider.h"

#include <QElapsedTimer>

#include <cmath>

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
    // The vendor reports -180..180, while Krita's rotation sensor spans 0..360
    // (KisDynamicSensorFactoryRegistry), so wrap it into range here.
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) {
        degrees += 360.0;
    }

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

bool isSupported()
{
    return hasValue();
}

qreal rotation()
{
    return storedRotation();
}

qreal rotationFor(const KoPointerEvent *event)
{
    // Once the device has reported barrel rotation, keep using it even between samples.
    // Falling back to the event would mix in the tilt orientation that Qt reports, which
    // is what made rotation follow the pen tilt and look unstable.
    return isSupported() ? rotation() : event->rotation();
}

} // namespace KisStylusRotationProvider
