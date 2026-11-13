/*
 * HeadlessValidation.h - validation of a project for the headless CLI
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_VALIDATION_H
#define LMMS_HEADLESS_VALIDATION_H

#include <QString>

namespace lmms
{
namespace headless
{

//! `lmms validate <project> [--json]`
//!
//! Loads the project and reports structural problems as JSON on stdout and a
//! human readable summary on stderr. Returns EXIT_FAILURE when at least one
//! issue has severity "error", EXIT_SUCCESS otherwise.
int runValidateAction(const QString& project, bool jsonOutput);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_VALIDATION_H
