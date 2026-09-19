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

bool &overridesBrushAngleFlag()
{
    static bool value = false;
    return value;
}

bool &invertedFlag()
{
    static bool value = false;
    return value;
}

} // namespace

void setRotation(qreal degrees)
{
    // Keep the value in the -180..180 the Rotation sensor is built around:
    // KisDynamicSensorRotation hands the event rotation to the option as
    // rotation / 180, and that has to stay inside the sensor curve's own 0..1
    // domain.  Wrapping to 0..360 instead would still produce the same dab angle
    // for an identity curve, but it pushes the curve input out of range and makes
    // any custom curve behave erratically.
    degrees = std::fmod(degrees, 360.0);
    if (degrees >= 180.0) {
        degrees -= 360.0;
    } else if (degrees < -180.0) {
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

void setOverridesBrushAngle(bool value)
{
    overridesBrushAngleFlag() = value;
}

bool overridesBrushAngle()
{
    return overridesBrushAngleFlag();
}

void setInverted(bool value)
{
    invertedFlag() = value;
}

bool inverted()
{
    return invertedFlag();
}

qreal rotation()
{
    return invertedFlag() ? -storedRotation() : storedRotation();
}

qreal rotationFor(const KoPointerEvent *event)
{
    // Once the device has reported barrel rotation, keep using it even between samples.
    // Falling back to the event would mix in the tilt orientation that Qt reports, which
    // is what made rotation follow the pen tilt and look unstable.
    return isSupported() ? rotation() : event->rotation();
}

} // namespace KisStylusRotationProvider
