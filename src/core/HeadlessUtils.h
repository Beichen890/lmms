/*
 * HeadlessUtils.h - helpers shared by the headless CLI actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_UTILS_H
#define LMMS_HEADLESS_UTILS_H

#include <QByteArray>
#include <QString>

namespace lmms
{
namespace headless
{

//! Parse a time specification used by `--from` / `--to`.
//! Accepted forms: "<ticks>" (integer ticks), "<n> bars", "<n> beats".
//! @param ticksPerBar number of ticks per bar for the current time signature
//! @param beatsPerBar number of beats per bar for the current time signature
//! @param ticks receives the resulting tick position
//! @param error receives a human readable error message on failure
bool parseTimeSpec(const QString& spec, int ticksPerBar, int beatsPerBar,
	int* ticks, QString* error);

//! Serialize project XML deterministically.
//! @param data raw project XML
//! @param sortChildren sort sibling elements to make output reproducible
//! @param stripImplicitMaster omit a default, unused Master mixer channel
//! @param error receives a human readable error message on failure
QString canonicalizeXml(const QByteArray& data, bool sortChildren,
	bool stripImplicitMaster, QString* error);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_UTILS_H
