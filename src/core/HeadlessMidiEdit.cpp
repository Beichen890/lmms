/*
 * HeadlessMidiEdit.cpp - headless MIDI clip editing actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "HeadlessMidiEdit.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "Clip.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternStore.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"
#include "TrackContainer.h"

namespace lmms
{
namespace headless
{

namespace
{

const QString ErrorCodeProjectNotFound = "project-not-found";


void reportFailure(bool jsonOutput, const QString& code, const QString& message)
{
	if (jsonOutput)
	{
		QJsonObject error;
		error["code"] = code;
		error["message"] = message;

		QJsonObject root;
		root["ok"] = false;
		root["error"] = error;

		fprintf(stdout, "%s\n", QJsonDocument(root).toJson(QJsonDocument::Compact).constData());
		fflush(stdout);
	}
	fprintf(stderr, "%s\n", message.toUtf8().constData());
}


bool isKnownOp(const QString& op)
{
	return op == "quantize" || op == "swing" || op == "humanize" ||
		op == "legato" || op == "strum" || op == "reverse" || op == "transpose";
}


//! Grid in ticks; zero means a 1/16 note.
double effectiveGrid(double grid)
{
	if (grid > 0.0) { return grid; }
	return static_cast<double>(TimePos::ticksPerBar()) / 16.0;
}


//! TimePos::ticksToMilliseconds() is ticks * 1250 / bpm.
double ticksPerMillisecond(double bpm)
{
	return bpm > 0.0 ? bpm / 1250.0 : 0.0;
}


struct MidiEditTarget
{
	InstrumentTrack* track = nullptr;
	MidiClip* clip = nullptr;
};


bool collectTargets(const QString& trackFilter,
	std::vector<MidiEditTarget>& targets, int* trackCount, QString* error)
{
	QSet<const InstrumentTrack*> matchedTracks;
	QSet<const InstrumentTrack*> editedTracks;

	auto collect = [&](const TrackContainer::TrackList& tracks) {
		for (Track* track : tracks)
		{
			auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
			if (instrumentTrack == nullptr) { continue; }
			if (!trackFilter.isEmpty() &&
				instrumentTrack->name().compare(trackFilter, Qt::CaseInsensitive) != 0)
			{
				continue;
			}

			matchedTracks.insert(instrumentTrack);
			for (Clip* clip : instrumentTrack->getClips())
			{
				auto midiClip = dynamic_cast<MidiClip*>(clip);
				if (midiClip == nullptr) { continue; }
				targets.push_back({instrumentTrack, midiClip});
				editedTracks.insert(instrumentTrack);
			}
		}
	};

	collect(Engine::getSong()->tracks());
	collect(Engine::patternStore()->tracks());

	if (!trackFilter.isEmpty() && matchedTracks.isEmpty())
	{
		if (error) { *error = QString("track not found: %1").arg(trackFilter); }
		return false;
	}

	if (trackCount) { *trackCount = static_cast<int>(editedTracks.size()); }
	return true;
}


struct NoteValues
{
	int pos;
	int length;
	int key;
	int volume;
};


NoteValues valuesOf(const Note* note)
{
	return {note->pos().getTicks(), note->length().getTicks(), note->key(),
		static_cast<int>(note->getVolume())};
}


void quantizeClip(MidiClip* clip, double grid, double strength)
{
	strength = std::clamp(strength, 0.0, 1.0);

	for (Note* note : clip->notes())
	{
		const int oldPos = note->pos();
		const double nearest = std::round(static_cast<double>(oldPos) / grid) * grid;
		const int newPos = static_cast<int>(std::llround(
			static_cast<double>(oldPos) + (nearest - static_cast<double>(oldPos)) * strength));

		note->setPos(TimePos(std::max(0, newPos)));
	}
}


void swingClip(MidiClip* clip, double grid, double amount)
{
	amount = std::clamp(amount, 0.0, 1.0);
	if (amount <= 0.0) { return; }

	const int shift = static_cast<int>(std::llround(amount * grid * 0.5));
	if (shift <= 0) { return; }

	for (Note* note : clip->notes())
	{
		const int pos = note->pos();
		const long long cell = static_cast<long long>(std::floor(static_cast<double>(pos) / grid));
		if (cell % 2 != 0)
		{
			note->setPos(TimePos(std::max(0, pos + shift)));
		}
	}
}


void humanizeClip(MidiClip* clip, double amount, double bpm, std::mt19937& rng)
{
	if (amount <= 0.0) { return; }

	std::uniform_real_distribution<double> jitter(-amount, amount);
	const double ticksPerMs = ticksPerMillisecond(bpm);

	for (Note* note : clip->notes())
	{
		const int posOffset = static_cast<int>(std::llround(jitter(rng) * ticksPerMs));
		note->setPos(TimePos(std::max(0, note->pos() + posOffset)));

		const int volumeOffset = static_cast<int>(std::llround(jitter(rng)));
		const int volume = std::clamp(static_cast<int>(note->getVolume()) + volumeOffset,
			static_cast<int>(MinVolume), static_cast<int>(MaxVolume));
		note->setVolume(static_cast<volume_t>(volume));
	}
}


void legatoClip(MidiClip* clip, double grid)
{
	NoteVector ordered = clip->notes();
	if (ordered.size() < 2) { return; }

	std::stable_sort(ordered.begin(), ordered.end(), [](const Note* a, const Note* b) {
		if (a->pos().getTicks() != b->pos().getTicks())
		{
			return a->pos().getTicks() < b->pos().getTicks();
		}
		return a->key() < b->key();
	});

	const int maxExtension = std::max(1, static_cast<int>(std::llround(grid)));

	for (std::size_t i = 0; i + 1 < ordered.size(); ++i)
	{
		Note* note = ordered[i];
		const int nextPos = ordered[i + 1]->pos();
		const int end = note->endPos();
		if (nextPos <= end) { continue; }

		const int newEnd = std::min(nextPos, end + maxExtension);
		if (newEnd > end)
		{
			note->setLength(TimePos(newEnd - note->pos()));
		}
	}
}


void strumClip(MidiClip* clip, double amount, double bpm)
{
	if (amount <= 0.0) { return; }

	const double ticksPerMs = ticksPerMillisecond(bpm);

	std::map<int, std::vector<Note*>> byPosition;
	for (Note* note : clip->notes())
	{
		byPosition[note->pos().getTicks()].push_back(note);
	}

	for (auto& group : byPosition)
	{
		std::vector<Note*>& notes = group.second;
		if (notes.size() < 2) { continue; }

		std::stable_sort(notes.begin(), notes.end(),
			[](const Note* a, const Note* b) { return a->key() < b->key(); });

		const int basePos = group.first;
		for (std::size_t i = 1; i < notes.size(); ++i)
		{
			const int offset = static_cast<int>(std::llround(
				static_cast<double>(i) * amount * ticksPerMs));
			if (offset > 0)
			{
				notes[i]->setPos(TimePos(std::max(0, basePos + offset)));
			}
		}
	}
}


void transposeClip(MidiClip* clip, int semitones)
{
	if (semitones == 0) { return; }

	for (Note* note : clip->notes())
	{
		// Note::setKey() clamps to 0..NumKeys-1.
		note->setKey(note->key() + semitones);
	}
}


void processClip(MidiClip* clip, const QString& op, const MidiEditOptions& options,
	double bpm, std::mt19937& rng, int* changedNotes)
{
	if (clip->notes().empty()) { return; }

	std::unordered_map<const Note*, NoteValues> before;
	before.reserve(clip->notes().size());
	for (const Note* note : clip->notes())
	{
		before.emplace(note, valuesOf(note));
	}

	if (op == "quantize")
	{
		quantizeClip(clip, effectiveGrid(options.grid), options.strength);
	}
	else if (op == "swing")
	{
		swingClip(clip, effectiveGrid(options.grid), options.amount);
	}
	else if (op == "humanize")
	{
		humanizeClip(clip, options.amount, bpm, rng);
	}
	else if (op == "legato")
	{
		legatoClip(clip, effectiveGrid(options.grid));
	}
	else if (op == "strum")
	{
		strumClip(clip, options.amount, bpm);
	}
	else if (op == "reverse")
	{
		clip->reverseNotes(clip->notes());
	}
	else if (op == "transpose")
	{
		transposeClip(clip, options.semitones);
	}

	for (const Note* note : clip->notes())
	{
		const auto it = before.find(note);
		if (it == before.end()) { continue; }

		const NoteValues after = valuesOf(note);
		if (after.pos != it->second.pos || after.length != it->second.length ||
			after.key != it->second.key || after.volume != it->second.volume)
		{
			++(*changedNotes);
		}
	}
}

} // namespace


int runMidiEditAction(const QString& project, const MidiEditOptions& options,
	const QString& output, bool jsonOutput)
{
	const QString op = options.op.trimmed().toLower();
	if (!isKnownOp(op))
	{
		reportFailure(jsonOutput, "invalid-op",
			QString("unknown op: %1").arg(options.op));
		return EXIT_FAILURE;
	}

	if (output.isEmpty())
	{
		reportFailure(jsonOutput, "missing-output", "an output path is required");
		return EXIT_FAILURE;
	}

	if (!QFileInfo::exists(project))
	{
		reportFailure(jsonOutput, ErrorCodeProjectNotFound,
			QString("project not found: %1").arg(project));
		return EXIT_FAILURE;
	}

	Engine::getSong()->loadProject(project);
	Song* song = Engine::getSong();

	std::vector<MidiEditTarget> targets;
	int trackCount = 0;
	QString error;
	if (!collectTargets(options.track, targets, &trackCount, &error))
	{
		reportFailure(jsonOutput, "track-not-found", error);
		return EXIT_FAILURE;
	}

	const double bpm = static_cast<double>(song->getTempo());
	std::mt19937 rng(static_cast<std::mt19937::result_type>(options.seed));
	int changedNotes = 0;

	for (const MidiEditTarget& target : targets)
	{
		processClip(target.clip, op, options, bpm, rng, &changedNotes);
	}

	if (!song->saveProjectFile(output))
	{
		reportFailure(jsonOutput, "save-failed",
			QString("could not save project: %1").arg(output));
		return EXIT_FAILURE;
	}

	if (jsonOutput)
	{
		QJsonObject result;
		result["ok"] = true;
		result["op"] = op;
		result["tracks"] = trackCount;
		result["notesChanged"] = changedNotes;

		fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		fflush(stdout);
	}
	else
	{
		fprintf(stdout, "Wrote %s (%d track(s), %d note(s) changed)\n",
			output.toUtf8().constData(), trackCount, changedNotes);
	}

	return EXIT_SUCCESS;
}

} // namespace headless
} // namespace lmms
