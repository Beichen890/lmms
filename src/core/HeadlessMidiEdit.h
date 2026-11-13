/*
 * HeadlessMidiEdit.h - headless MIDI clip editing actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_MIDI_EDIT_H
#define LMMS_HEADLESS_MIDI_EDIT_H

#include <QString>

namespace lmms
{
namespace headless
{

struct MidiEditOptions
{
	QString op;          // quantize|swing|humanize|legato|strum|reverse|transpose
	QString track;       // empty = all tracks
	double grid = 0.0;   // ticks; 0 => 1/16 note (TimePos::ticksPerBar()/16)
	double strength = 1.0; // 0..1
	double amount = 0.0;   // swing 0..1, humanize ms/velocity, strum ms
	int semitones = 0;
	int seed = 0;
	bool json = false;
};

//! `lmms midi-edit <project> --op <op> [--track <name>] [--grid <ticks>]
//!   [--strength <0..1>] [--amount <value>] [--semitones <n>] [--seed <n>]
//!   --out <out.mmp> [--json]`
int runMidiEditAction(const QString& project, const MidiEditOptions& options,
	const QString& output, bool jsonOutput);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_MIDI_EDIT_H
