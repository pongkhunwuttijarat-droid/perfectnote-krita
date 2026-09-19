/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTYLUSROTATIONPROVIDER_H
#define KISSTYLUSROTATIONPROVIDER_H

#include <QGlobalStatic>
#include <kritaui_export.h>

class KoPointerEvent;

/**
 * Holds the barrel rotation reported by the vendor pen service on Android.
 *
 * Qt reads the wrong axis for this hardware (AXIS_RZ, which the pen never populates) and
 * reports the tilt orientation as rotation instead, so Krita decodes the vendor stream
 * itself and feeds the value here. On every other platform, and on devices without the
 * service, nothing is ever set and callers fall back to the event.
 *
 * A value is only considered valid for a short while after it arrives, so that a stream
 * which stops does not leave a stale angle behind.
 */
namespace KisStylusRotationProvider {

/** Called from the Android bridge with degrees in -180..180. */
KRITAUI_EXPORT void setRotation(qreal degrees);

/** Forget the current value, e.g. when the vendor service disconnects. */
KRITAUI_EXPORT void clear();

/** True when a recent vendor value is available. */
KRITAUI_EXPORT bool isActive();

/**
 * True once a vendor value has ever been received, i.e. this device reports barrel
 * rotation. Used to decide whether falling back to the event is safe at all: Qt reports
 * the tilt orientation as rotation here, so falling back interleaves the correct angle
 * with a tilt dependent one and makes the brush jitter.
 */
KRITAUI_EXPORT bool isSupported();

/** Last vendor rotation in degrees. Only meaningful while isActive(). */
KRITAUI_EXPORT qreal rotation();

/**
 * Rotation to paint with: the vendor value when we have one, otherwise what the event
 * carries. This is the single place the two sources meet.
 */
KRITAUI_EXPORT qreal rotationFor(const KoPointerEvent *event);

} // namespace KisStylusRotationProvider

#endif // KISSTYLUSROTATIONPROVIDER_H
