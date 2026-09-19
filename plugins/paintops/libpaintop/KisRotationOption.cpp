/*
 *  SPDX-FileCopyrightText: 2009 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisRotationOption.h"

#include <kis_algebra_2d.h>
#include <kis_properties_configuration.h>
#include <kis_paint_information.h>
#include <KisStandardOptionData.h>
#include <kis_paintop.h>

#include <input/KisStylusRotationProvider.h>

#include <KisPaintOpOptionUtils.h>
namespace kpou = KisPaintOpOptionUtils;


KisRotationOption::KisRotationOption(const KisPropertiesConfiguration *setting)
    : KisRotationOption(kpou::loadOptionData<KisRotationOptionData>(setting))
{
}

KisRotationOption::KisRotationOption(const KisRotationOptionData &data)
    : KisCurveOption(data)
{
    if (data.sensorStruct().sensorDrawingAngle.isActive) {
        m_fanCornersEnabled =
             data.sensorStruct().sensorDrawingAngle.fanCornersEnabled &&
             !data.sensorStruct().sensorDrawingAngle.lockedAngleMode;
        m_fanCornersStep = qreal(data.sensorStruct().sensorDrawingAngle.fanCornersStep);
    }
}

qreal KisRotationOption::apply(const KisPaintInformation & info) const
{
    /**
     * A pen that reports its own barrel rotation can drive the dab angle of every preset
     * at once, including the presets that do not drive their Rotation parameter from the
     * Rotation sensor. Nothing is written to the presets, so switching this off restores
     * the stock behaviour exactly; the switch lives in the Pen preferences page.
     */
    if (KisStylusRotationProvider::overridesBrushAngle() &&
        KisStylusRotationProvider::isSupported()) {

        const qreal normalizedBaseAngle = -info.canvasRotation() / 360.0;
        const qreal penAngle = KisStylusRotationProvider::rotation() / 180.0;

        // Same shape as computeRotationLikeValue() below, with the pen angle standing in
        // for the sensor, so that the canvas rotation keeps being folded in.
        qreal value = KisAlgebra2D::wrapValue(2.0 * normalizedBaseAngle + penAngle, -1.0, 1.0);
        value = 1.0 - value;

        return normalizeAngle(value * M_PI);
    }

    if (!isChecked()) return kisDegreesToRadians(info.canvasRotation());

    const bool absoluteAxesFlipped = info.canvasMirroredH() != info.canvasMirroredV();

    const qreal normalizedBaseAngle = -info.canvasRotation() / 360.0;

    // we should invert scaling part because it is expected
    // to rotate the brush counterclockwise
    const qreal scalingPartCoeff = -1.0;

    qreal value = computeRotationLikeValue(info, normalizedBaseAngle, absoluteAxesFlipped, scalingPartCoeff, info.isHoveringMode());

    /// flip to conform global legacy code
    /// we measure rotation in the opposite direction relative Qt's way
    value = 1.0 - value;

    return normalizeAngle(value * M_PI);
 }

void KisRotationOption::applyFanCornersInfo(KisPaintOp *op)
{
    if (!this->isChecked()) return;

    /**
     * A special case for the Drawing Angle sensor, because it
     * changes the behavior of KisPaintOp::paintLine()
     */
    op->setFanCornersInfo(m_fanCornersEnabled, m_fanCornersStep * M_PI / 180.0);
}
