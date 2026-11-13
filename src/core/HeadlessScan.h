/*
 * HeadlessScan.h - plugin scanning for the headless CLI
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_SCAN_H
#define LMMS_HEADLESS_SCAN_H

#include <QString>

namespace lmms
{
namespace headless
{

//! `lmms scan-plugins [all|native|ladspa|vst|lv2|vst3|clap] [--json]`
//!
//! Reports all plugins that can be discovered without instantiating them.
//! Scanning failures are collected in the "errors" array and never abort the
//! scan. Returns EXIT_FAILURE only for an unknown format.
int runScanPluginsAction(const QString& format, bool jsonOutput);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_SCAN_H
