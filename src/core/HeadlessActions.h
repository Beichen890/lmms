/*
 * HeadlessActions.h - implementations of the headless CLI actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_ACTIONS_H
#define LMMS_HEADLESS_ACTIONS_H

#include <QList>
#include <QString>

namespace lmms
{
namespace headless
{

struct AutomationPoint
{
	int tick = 0;
	double value = 0.0;
};

//! `lmms save <in> [out] [--canonical] [--minimal]`
int runSaveAction(const QString& input, const QString& output,
	bool canonical, bool minimal);

//! `lmms script <project> <file.json> [--out out.mmp] [--json]`
int runScriptAction(const QString& project, const QString& scriptFile,
	const QString& output, bool jsonOutput);

//! `lmms --plugins-json [--type instrument|effect|ladspa|vst|all]`
int runPluginsJsonAction(const QString& typeFilter);

//! `lmms normalize-basenote <project> [--out out.mmp] [--in-place] [--dry-run] [--json]`
int runNormalizeBaseNoteAction(const QString& project, const QString& output,
	bool inPlace, bool dryRun, bool jsonOutput);

//! `lmms --model-ids <project>`
int runModelIdsAction(const QString& project);

//! `lmms automation-add <project> --track <name> [--effect <index>]
//!   --param <name> --point <tick>:<value> ... --out <out.mmp>`
int runAutomationAddAction(const QString& project, const QString& trackName,
	int effectIndex, const QString& paramName,
	const QList<AutomationPoint>& points, const QString& output);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_ACTIONS_H
