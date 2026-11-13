/*
 * HeadlessMidi.h - headless MIDI import/export actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef LMMS_HEADLESS_MIDI_H
#define LMMS_HEADLESS_MIDI_H

#include <QString>

namespace lmms
{
namespace headless
{

//! Import a Standard MIDI File into a new LMMS project.
//! Creates one InstrumentTrack and one MidiClip per MIDI track that
//! contains notes and writes the result to \p outProject.
int runImportMidiAction(const QString& midiFile, const QString& outProject,
	const QString& instrument, const QString& trackName, bool jsonOutput);

//! Export all MIDI clips of an LMMS project to a Standard MIDI File
//! (format 1, 192 PPQ).
int runExportMidiAction(const QString& project, const QString& outMidi,
	bool jsonOutput);

} // namespace headless
} // namespace lmms

#endif // LMMS_HEADLESS_MIDI_H
