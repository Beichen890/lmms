/*
 * HeadlessMidi.cpp - headless MIDI import/export actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * The Standard MIDI File reader/writer below is a minimal, self-contained
 * implementation. It deliberately does not use portsmf or any plugin so
 * that it builds on Windows, Linux and macOS without extra dependencies.
 */

#include "HeadlessMidi.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "Midi.h"
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

//! LMMS uses 192 ticks per bar; in 4/4 this is 48 ticks per quarter note,
//! which is what the existing MIDI import filter assumes as well.
constexpr int LmmsTicksPerQuarter = DefaultTicksPerBar / 4;

//! PPQ of all exported files. Because a LMMS quarter note is 48 ticks, one
//! LMMS tick has to be written as four MIDI ticks at 192 PPQ in order to
//! preserve the real-time duration.
constexpr int ExportPpq = 192;
constexpr int ExportTickScale = ExportPpq / LmmsTicksPerQuarter;

constexpr double DefaultMidiTempo = 120.0;


void printJson(const QJsonObject& object)
{
	fprintf(stdout, "%s\n",
		QJsonDocument(object).toJson(QJsonDocument::Compact).constData());
	fflush(stdout);
}


void reportError(bool jsonOutput, const QString& code, const QString& message)
{
	if (jsonOutput)
	{
		QJsonObject error;
		error["code"] = code;
		error["message"] = message;

		QJsonObject root;
		root["ok"] = false;
		root["error"] = error;
		printJson(root);
	}
	fprintf(stderr, "%s\n", message.toUtf8().constData());
}


class ByteReader
{
public:
	explicit ByteReader(const QByteArray& data) :
		m_data(data)
	{
	}

	bool readByte(quint8* value)
	{
		if (m_pos >= m_data.size()) { return false; }
		*value = static_cast<quint8>(m_data.at(static_cast<int>(m_pos)));
		++m_pos;
		return true;
	}

	bool readBytes(qint64 count, QByteArray* out)
	{
		if (count < 0 || m_pos + count > m_data.size()) { return false; }
		*out = m_data.mid(static_cast<int>(m_pos), static_cast<int>(count));
		m_pos += count;
		return true;
	}

	bool skip(qint64 count)
	{
		if (count < 0 || m_pos + count > m_data.size()) { return false; }
		m_pos += count;
		return true;
	}

	bool readU16(quint16* value)
	{
		QByteArray bytes;
		if (!readBytes(2, &bytes)) { return false; }
		*value = static_cast<quint16>(
			(static_cast<quint8>(bytes.at(0)) << 8) |
			static_cast<quint8>(bytes.at(1)));
		return true;
	}

	bool readU32(quint32* value)
	{
		QByteArray bytes;
		if (!readBytes(4, &bytes)) { return false; }
		*value = (static_cast<quint32>(static_cast<quint8>(bytes.at(0))) << 24) |
			(static_cast<quint32>(static_cast<quint8>(bytes.at(1))) << 16) |
			(static_cast<quint32>(static_cast<quint8>(bytes.at(2))) << 8) |
			static_cast<quint32>(static_cast<quint8>(bytes.at(3)));
		return true;
	}

	qint64 remaining() const { return m_data.size() - m_pos; }

private:
	const QByteArray& m_data;
	qint64 m_pos = 0;
};


bool readVarLen(ByteReader& reader, quint32* value)
{
	quint32 result = 0;
	for (int i = 0; i < 4; ++i)
	{
		quint8 byte = 0;
		if (!reader.readByte(&byte)) { return false; }
		result = (result << 7) | (byte & 0x7F);
		if ((byte & 0x80) == 0)
		{
			*value = result;
			return true;
		}
	}
	return false;
}


void appendVarLen(QByteArray& out, quint32 value)
{
	quint8 buffer[4];
	int count = 0;
	buffer[count++] = static_cast<quint8>(value & 0x7F);
	while ((value >>= 7) != 0)
	{
		buffer[count++] = static_cast<quint8>((value & 0x7F) | 0x80);
	}
	for (int i = count - 1; i >= 0; --i)
	{
		out.append(static_cast<char>(buffer[i]));
	}
}


void appendU16(QByteArray& out, quint16 value)
{
	out.append(static_cast<char>((value >> 8) & 0xFF));
	out.append(static_cast<char>(value & 0xFF));
}


void appendU32(QByteArray& out, quint32 value)
{
	out.append(static_cast<char>((value >> 24) & 0xFF));
	out.append(static_cast<char>((value >> 16) & 0xFF));
	out.append(static_cast<char>((value >> 8) & 0xFF));
	out.append(static_cast<char>(value & 0xFF));
}


void appendChunk(QByteArray& out, const char* id, const QByteArray& data)
{
	out.append(id, 4);
	appendU32(out, static_cast<quint32>(data.size()));
	out.append(data);
}


void appendMetaEvent(QByteArray& chunk, quint32 delta, quint8 type,
	const QByteArray& data)
{
	appendVarLen(chunk, delta);
	chunk.append(static_cast<char>(0xFF));
	chunk.append(static_cast<char>(type));
	appendVarLen(chunk, static_cast<quint32>(data.size()));
	chunk.append(data);
}


QString decodeMidiText(const QByteArray& bytes)
{
	QString text = QString::fromUtf8(bytes);
	if (text.contains(QChar(0xFFFD))) { text = QString::fromLatin1(bytes); }
	text.remove(QChar('\0'));
	return text;
}


struct ParsedNote
{
	quint64 startTick = 0;
	quint64 endTick = 0;
	int key = 60;
	int velocity = 100;
};


struct ParsedTrack
{
	QString name;
	int program = -1;
	std::vector<ParsedNote> notes;
};


struct ParsedMidi
{
	int format = 0;
	int ppq = 0;
	double bpm = DefaultMidiTempo;
	bool hasTempo = false;
	std::vector<ParsedTrack> tracks;
};


struct PendingNote
{
	quint64 startTick = 0;
	int velocity = 100;
};


bool parseTrackChunk(const QByteArray& chunk, ParsedTrack* track,
	ParsedMidi* midi, QString* error)
{
	ByteReader reader(chunk);
	quint64 absTick = 0;
	quint64 lastTick = 0;
	quint8 runningStatus = 0;
	std::map<int, std::deque<PendingNote>> pending;

	auto closeNote = [&](int channel, int key, quint64 tick) {
		const int id = channel * 128 + key;
		auto it = pending.find(id);
		if (it == pending.end() || it->second.empty()) { return; }
		const PendingNote note = it->second.front();
		it->second.pop_front();
		const quint64 endTick = std::max<quint64>(tick, note.startTick + 1);
		ParsedNote parsed;
		parsed.startTick = note.startTick;
		parsed.endTick = endTick;
		parsed.key = key;
		parsed.velocity = note.velocity;
		track->notes.push_back(parsed);
	};

	auto closeAllNotes = [&](quint64 tick) {
		for (auto& entry : pending)
		{
			const int channel = entry.first / 128;
			const int key = entry.first % 128;
			while (!entry.second.empty())
			{
				closeNote(channel, key, tick);
			}
		}
	};

	bool finished = false;
	while (!finished && reader.remaining() > 0)
	{
		quint32 delta = 0;
		if (!readVarLen(reader, &delta))
		{
			*error = "truncated delta-time in MIDI track";
			return false;
		}
		absTick += delta;
		lastTick = absTick;

		quint8 statusByte = 0;
		if (!reader.readByte(&statusByte))
		{
			*error = "truncated MIDI event";
			return false;
		}

		quint8 firstData = 0;
		bool hasFirstData = false;
		quint8 status = 0;

		if (statusByte < 0x80)
		{
			if (runningStatus == 0)
			{
				*error = "MIDI running status used without a previous status byte";
				return false;
			}
			status = runningStatus;
			firstData = statusByte;
			hasFirstData = true;
		}
		else
		{
			status = statusByte;
			runningStatus = (status < 0xF0) ? status : 0;
		}

		auto nextData = [&](quint8* value) -> bool {
			if (hasFirstData)
			{
				*value = firstData;
				hasFirstData = false;
				return true;
			}
			return reader.readByte(value);
		};

		if (status == 0xFF)
		{
			quint8 type = 0;
			if (!reader.readByte(&type))
			{
				*error = "truncated MIDI meta event";
				return false;
			}
			quint32 length = 0;
			if (!readVarLen(reader, &length))
			{
				*error = "truncated MIDI meta event length";
				return false;
			}
			QByteArray payload;
			if (!reader.readBytes(length, &payload))
			{
				*error = "truncated MIDI meta event payload";
				return false;
			}

			if (type == 0x51 && payload.size() >= 3 && !midi->hasTempo)
			{
				const quint32 micros =
					(static_cast<quint32>(static_cast<quint8>(payload.at(0))) << 16) |
					(static_cast<quint32>(static_cast<quint8>(payload.at(1))) << 8) |
					static_cast<quint32>(static_cast<quint8>(payload.at(2)));
				if (micros > 0)
				{
					midi->bpm = 60000000.0 / static_cast<double>(micros);
					midi->hasTempo = true;
				}
			}
			else if (type == 0x03 && track->name.isEmpty())
			{
				track->name = decodeMidiText(payload);
			}
			else if (type == 0x2F)
			{
				finished = true;
			}
		}
		else if (status == 0xF0 || status == 0xF7)
		{
			quint32 length = 0;
			if (!readVarLen(reader, &length) || !reader.skip(length))
			{
				*error = "truncated MIDI SysEx event";
				return false;
			}
		}
		else if (status >= 0xF0)
		{
			*error = QString("unsupported MIDI system event 0x%1")
				.arg(static_cast<int>(status), 2, 16, QChar('0'));
			return false;
		}
		else
		{
			const int channel = status & 0x0F;
			const quint8 type = status & 0xF0;
			const int dataCount = (type == 0xC0 || type == 0xD0) ? 1 : 2;

			quint8 data[2] = {0, 0};
			for (int i = 0; i < dataCount; ++i)
			{
				if (!nextData(&data[i]))
				{
					*error = "truncated MIDI channel event";
					return false;
				}
			}

			if (type == 0x90)
			{
				const int key = data[0] & 0x7F;
				const int velocity = data[1] & 0x7F;
				if (velocity == 0)
				{
					closeNote(channel, key, absTick);
				}
				else
				{
					PendingNote note;
					note.startTick = absTick;
					note.velocity = velocity;
					pending[channel * 128 + key].push_back(note);
				}
			}
			else if (type == 0x80)
			{
				closeNote(channel, data[0] & 0x7F, absTick);
			}
			else if (type == 0xC0 && track->program < 0)
			{
				track->program = data[0] & 0x7F;
			}
		}
	}

	// Tolerate missing note-off events at the end of a track.
	closeAllNotes(lastTick);
	return true;
}


bool parseSmf(const QByteArray& data, ParsedMidi* midi, QString* error)
{
	if (data.size() < 14)
	{
		*error = "file is too small to be a Standard MIDI File";
		return false;
	}

	ByteReader reader(data);
	QByteArray id;
	quint32 headerLength = 0;
	if (!reader.readBytes(4, &id) || id != QByteArray("MThd") ||
		!reader.readU32(&headerLength))
	{
		*error = "missing MThd header";
		return false;
	}
	if (headerLength < 6)
	{
		*error = "invalid MThd header length";
		return false;
	}

	QByteArray header;
	if (!reader.readBytes(6, &header))
	{
		*error = "truncated MThd header";
		return false;
	}
	ByteReader headerReader(header);
	quint16 format = 0;
	quint16 declaredTracks = 0;
	quint16 division = 0;
	headerReader.readU16(&format);
	headerReader.readU16(&declaredTracks);
	headerReader.readU16(&division);
	Q_UNUSED(declaredTracks)

	if (headerLength > 6 && !reader.skip(headerLength - 6))
	{
		*error = "truncated MThd header";
		return false;
	}

	if (format > 1)
	{
		*error = QString("unsupported SMF format %1 (only format 0 and 1 are supported)")
			.arg(format);
		return false;
	}
	if ((division & 0x8000) != 0)
	{
		*error = "SMPTE time division is not supported";
		return false;
	}
	const int ppq = division & 0x7FFF;
	if (ppq == 0)
	{
		*error = "invalid PPQ division (zero)";
		return false;
	}

	midi->format = format;
	midi->ppq = ppq;

	int trackIndex = 0;
	while (reader.remaining() > 0)
	{
		QByteArray chunkId;
		quint32 chunkLength = 0;
		if (!reader.readBytes(4, &chunkId) || !reader.readU32(&chunkLength))
		{
			*error = "truncated MIDI chunk header";
			return false;
		}

		if (chunkId == QByteArray("MTrk"))
		{
			QByteArray chunk;
			if (!reader.readBytes(chunkLength, &chunk))
			{
				*error = "truncated MTrk chunk";
				return false;
			}
			ParsedTrack track;
			if (!parseTrackChunk(chunk, &track, midi, error))
			{
				*error = QString("track %1: %2").arg(trackIndex).arg(*error);
				return false;
			}
			midi->tracks.push_back(track);
			++trackIndex;
		}
		else if (!reader.skip(chunkLength))
		{
			*error = "truncated MIDI chunk";
			return false;
		}
	}

	if (midi->tracks.empty())
	{
		*error = "MIDI file contains no tracks";
		return false;
	}

	return true;
}


qint64 midiTickToLmms(quint64 midiTick, int ppq)
{
	return static_cast<qint64>(std::llround(
		static_cast<double>(midiTick) * LmmsTicksPerQuarter / ppq));
}


int clampToTicks(qint64 value)
{
	const qint64 maxTicks = 0x7FFFFFFF;
	return static_cast<int>(std::max<qint64>(0, std::min(value, maxTicks)));
}


bool saveSongToFile(Song* song, const QString& output, QString* error)
{
	const QString parent = QFileInfo(output).absolutePath();
	if (!parent.isEmpty() && !QDir().mkpath(parent))
	{
		*error = QString("could not create output directory: %1").arg(parent);
		return false;
	}

	// LMMS saves by renaming over the destination, which fails on Windows if
	// the destination already exists. Save to a fresh temporary path first.
	QTemporaryDir tempDir(QDir::tempPath() + "/lmms-midi-XXXXXX");
	if (!tempDir.isValid())
	{
		*error = "could not create a temporary directory for saving";
		return false;
	}
	const QString tempPath = tempDir.filePath("project.mmp");

	if (!song->saveProjectFile(tempPath))
	{
		QFile::remove(tempPath);
		QFile::remove(tempPath + ".bak");
		*error = QString("could not save project: %1").arg(output);
		return false;
	}

	QFile tempFile(tempPath);
	if (!tempFile.open(QIODevice::ReadOnly))
	{
		*error = QString("could not read back saved project: %1").arg(tempPath);
		QFile::remove(tempPath);
		QFile::remove(tempPath + ".bak");
		return false;
	}
	const QByteArray data = tempFile.readAll();
	tempFile.close();
	QFile::remove(tempPath);
	QFile::remove(tempPath + ".bak");

	QFile outFile(output);
	if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		*error = QString("could not open output file: %1").arg(output);
		return false;
	}
	const bool ok = outFile.write(data) == data.size();
	outFile.close();
	if (!ok) { *error = QString("could not write output file: %1").arg(output); }
	return ok;
}


bool loadProjectFile(const QString& path, QString* error)
{
	const QFileInfo info(path);
	if (!info.exists() || !info.isFile())
	{
		*error = QString("project not found: %1").arg(path);
		return false;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		*error = QString("could not open project: %1").arg(path);
		return false;
	}
	const QByteArray data = file.readAll();
	file.close();

	// .mmpz files are zlib-compressed and cannot be checked as text here.
	const QString suffix = info.suffix().toLower();
	if (suffix != "mmpz" && !data.contains("<lmms-project"))
	{
		*error = QString("not an LMMS project file: %1").arg(path);
		return false;
	}

	Engine::getSong()->loadProject(path);
	return true;
}


struct ExportEvent
{
	qint64 tick = 0;
	int key = 60;
	int velocity = 100;
	bool noteOn = true;
};


struct ExportTrack
{
	QString name;
	int program = 0;
	std::vector<ExportEvent> events;
};


int exportChannelFor(int index)
{
	// Channel 10 (index 9) is reserved for percussion by General MIDI.
	static const int channels[15] =
		{0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
	return channels[index % 15];
}


void collectTrack(InstrumentTrack* instrumentTrack, ExportTrack* out,
	int* noteCount)
{
	out->name = instrumentTrack->name().trimmed();
	out->program = qBound(0, instrumentTrack->midiPort()->outputProgram() - 1,
		127);

	for (Clip* clip : instrumentTrack->getClips())
	{
		auto* midiClip = dynamic_cast<MidiClip*>(clip);
		if (midiClip == nullptr) { continue; }

		const qint64 clipStart = midiClip->startPosition().getTicks();
		for (const Note* note : midiClip->notes())
		{
			const qint64 start = std::max<qint64>(
				0, clipStart + note->pos().getTicks());
			const qint64 length = std::max<qint64>(
				1, note->length().getTicks());

			ExportEvent on;
			on.tick = start * ExportTickScale;
			on.key = note->key();
			on.velocity = qBound(1,
				static_cast<int>(std::lround(note->getVolume() * 127.0 / MaxVolume)),
				127);
			on.noteOn = true;
			out->events.push_back(on);

			ExportEvent off;
			off.tick = (start + length) * ExportTickScale;
			off.key = on.key;
			off.velocity = 0;
			off.noteOn = false;
			out->events.push_back(off);

			++(*noteCount);
		}
	}
}


bool buildSmf(const std::vector<ExportTrack>& tracks, int bpm, QByteArray* out,
	QString* error)
{
	QByteArray result;
	result.append("MThd", 4);
	appendU32(result, 6);
	appendU16(result, 1); // format 1
	appendU16(result, static_cast<quint16>(tracks.size() + 1));
	appendU16(result, static_cast<quint16>(ExportPpq));

	QByteArray conductor;
	const quint32 microsPerQuarter = static_cast<quint32>(std::lround(
		60000000.0 / static_cast<double>(std::max(1, bpm))));
	appendMetaEvent(conductor, 0, 0x03, QByteArray("LMMS Tempo"));
	appendVarLen(conductor, 0);
	conductor.append(static_cast<char>(0xFF));
	conductor.append(static_cast<char>(0x51));
	conductor.append(static_cast<char>(0x03));
	conductor.append(static_cast<char>((microsPerQuarter >> 16) & 0xFF));
	conductor.append(static_cast<char>((microsPerQuarter >> 8) & 0xFF));
	conductor.append(static_cast<char>(microsPerQuarter & 0xFF));
	appendMetaEvent(conductor, 0, 0x2F, QByteArray());
	appendChunk(result, "MTrk", conductor);

	for (std::size_t i = 0; i < tracks.size(); ++i)
	{
		const ExportTrack& track = tracks[i];
		const int channel = exportChannelFor(static_cast<int>(i));

		QByteArray chunk;
		appendMetaEvent(chunk, 0, 0x03, track.name.toUtf8());
		appendVarLen(chunk, 0);
		chunk.append(static_cast<char>(0xC0 | channel));
		chunk.append(static_cast<char>(qBound(0, track.program, 127)));

		std::vector<ExportEvent> events = track.events;
		std::stable_sort(events.begin(), events.end(),
			[](const ExportEvent& a, const ExportEvent& b) {
				if (a.tick != b.tick) { return a.tick < b.tick; }
				// Note-offs first so repeated notes do not get stuck.
				if (a.noteOn != b.noteOn) { return !a.noteOn; }
				return false;
			});

		quint64 lastTick = 0;
		for (const ExportEvent& event : events)
		{
			const quint64 tick = static_cast<quint64>(
				std::max<qint64>(event.tick, static_cast<qint64>(lastTick)));
			const quint64 delta = tick - lastTick;
			if (delta > 0x0FFFFFFF)
			{
				*error = "project is too long to be represented as a MIDI file";
				return false;
			}
			appendVarLen(chunk, static_cast<quint32>(delta));
			lastTick = tick;

			if (event.noteOn)
			{
				chunk.append(static_cast<char>(0x90 | channel));
				chunk.append(static_cast<char>(event.key & 0x7F));
				chunk.append(static_cast<char>(qBound(1, event.velocity, 127)));
			}
			else
			{
				chunk.append(static_cast<char>(0x80 | channel));
				chunk.append(static_cast<char>(event.key & 0x7F));
				chunk.append(static_cast<char>(0x00));
			}
		}

		appendMetaEvent(chunk, 0, 0x2F, QByteArray());
		appendChunk(result, "MTrk", chunk);
	}

	*out = result;
	return true;
}

} // namespace


int runImportMidiAction(const QString& midiFile, const QString& outProject,
	const QString& instrument, const QString& trackName, bool jsonOutput)
{
	auto fail = [&](const QString& code, const QString& message) {
		reportError(jsonOutput, code, message);
		return EXIT_FAILURE;
	};

	const QFileInfo midiInfo(midiFile);
	if (!midiInfo.exists() || !midiInfo.isFile())
	{
		return fail("midi-not-found",
			QString("MIDI file not found: %1").arg(midiFile));
	}

	QFile file(midiFile);
	if (!file.open(QIODevice::ReadOnly))
	{
		return fail("midi-not-found",
			QString("could not open MIDI file: %1").arg(midiFile));
	}
	const QByteArray data = file.readAll();
	file.close();

	ParsedMidi midi;
	QString error;
	if (!parseSmf(data, &midi, &error))
	{
		return fail("invalid-midi", error);
	}

	if (outProject.isEmpty())
	{
		return fail("invalid-argument", "--out <out.mmp> is required");
	}

	Song* song = Engine::getSong();

	// Start from a genuinely empty project; this removes any tracks that a
	// template or the engine may have created.
	song->clearProject();
	song->tempoModel().setValue(static_cast<int>(std::lround(midi.bpm)));

	const QString plugin = instrument.trimmed().isEmpty()
		? QString("tripleoscillator") : instrument.trimmed();

	int tracksWithNotes = 0;
	for (const auto& track : midi.tracks)
	{
		if (!track.notes.empty()) { ++tracksWithNotes; }
	}

	int createdTracks = 0;
	int createdNotes = 0;

	for (std::size_t i = 0; i < midi.tracks.size(); ++i)
	{
		const ParsedTrack& parsed = midi.tracks[i];
		if (parsed.notes.empty()) { continue; }

		QString name;
		if (!trackName.trimmed().isEmpty())
		{
			name = trackName.trimmed();
			if (tracksWithNotes > 1)
			{
				name += QString(" %1").arg(createdTracks + 1);
			}
		}
		else if (!parsed.name.trimmed().isEmpty())
		{
			name = parsed.name.trimmed();
		}
		else
		{
			name = QString("Track %1").arg(static_cast<int>(i) + 1);
		}

		Track* rawTrack = Track::create(Track::Type::Instrument, song);
		auto* instrumentTrack = dynamic_cast<InstrumentTrack*>(rawTrack);
		if (instrumentTrack == nullptr)
		{
			return fail("invalid-midi", "could not create an instrument track");
		}

		Instrument* loaded = instrumentTrack->loadInstrument(plugin);
		if (loaded == nullptr || loaded->nodeName() == "dummyinstrument")
		{
			song->removeTrack(rawTrack);
			delete rawTrack;
			return fail("instrument-missing",
				QString("instrument plugin not available: %1").arg(plugin));
		}

		// Loading an instrument resets the track name, so apply it afterwards.
		instrumentTrack->setName(name);
		const int program = parsed.program >= 0
			? qBound(0, parsed.program, MidiProgramCount - 1) : 0;
		instrumentTrack->midiPort()->setOutputProgram(program + 1);

		auto* clip = dynamic_cast<MidiClip*>(
			instrumentTrack->createClip(TimePos(0)));
		if (clip == nullptr)
		{
			song->removeTrack(rawTrack);
			delete rawTrack;
			return fail("invalid-midi",
				QString("could not create a MIDI clip on track \"%1\"").arg(name));
		}

		for (const ParsedNote& parsedNote : parsed.notes)
		{
			const qint64 start = midiTickToLmms(parsedNote.startTick, midi.ppq);
			const qint64 end = midiTickToLmms(parsedNote.endTick, midi.ppq);
			const qint64 length = std::max<qint64>(1, end - start);

			const int key = qBound(0, parsedNote.key, NumKeys - 1);
			const int velocity = qBound(1, parsedNote.velocity, 127);
			const int volume = qBound(0,
				static_cast<int>(std::lround(velocity * (MaxVolume / 127.0))),
				static_cast<int>(MaxVolume));

			clip->addNote(Note(TimePos(clampToTicks(length)),
				TimePos(clampToTicks(start)), key,
				static_cast<volume_t>(volume)), false);
			++createdNotes;
		}

		++createdTracks;
	}

	if (!saveSongToFile(song, outProject, &error))
	{
		return fail("save-failed", error);
	}

	if (jsonOutput)
	{
		QJsonObject result;
		result["ok"] = true;
		result["tracks"] = createdTracks;
		result["notes"] = createdNotes;
		result["tempo"] = static_cast<int>(song->getTempo());
		printJson(result);
	}
	else
	{
		fprintf(stdout, "Imported %d track(s), %d note(s) into %s\n",
			createdTracks, createdNotes, outProject.toUtf8().constData());
		fflush(stdout);
	}

	return EXIT_SUCCESS;
}


int runExportMidiAction(const QString& project, const QString& outMidi,
	bool jsonOutput)
{
	auto fail = [&](const QString& code, const QString& message) {
		reportError(jsonOutput, code, message);
		return EXIT_FAILURE;
	};

	if (outMidi.isEmpty())
	{
		return fail("invalid-argument", "--out <out.mid> is required");
	}

	QString error;
	if (!loadProjectFile(project, &error))
	{
		return fail("project-not-found", error);
	}

	std::vector<ExportTrack> tracks;
	int noteCount = 0;

	auto collectContainer = [&](TrackContainer* container) {
		for (Track* track : container->tracks())
		{
			auto* instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
			if (instrumentTrack == nullptr) { continue; }

			ExportTrack exportTrack;
			collectTrack(instrumentTrack, &exportTrack, &noteCount);
			if (!exportTrack.events.empty())
			{
				tracks.push_back(exportTrack);
			}
		}
	};

	Song* song = Engine::getSong();
	collectContainer(song);
	collectContainer(Engine::patternStore());

	const int bpm = static_cast<int>(song->getTempo());

	QByteArray smf;
	if (!buildSmf(tracks, bpm, &smf, &error))
	{
		return fail("export-failed", error);
	}

	const QString parent = QFileInfo(outMidi).absolutePath();
	if (!parent.isEmpty() && !QDir().mkpath(parent))
	{
		return fail("write-failed",
			QString("could not create output directory: %1").arg(parent));
	}

	QFile file(outMidi);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		return fail("write-failed",
			QString("could not open output file: %1").arg(outMidi));
	}
	const bool written = file.write(smf) == smf.size();
	file.close();
	if (!written)
	{
		return fail("write-failed",
			QString("could not write output file: %1").arg(outMidi));
	}

	if (jsonOutput)
	{
		QJsonObject result;
		result["ok"] = true;
		result["tracks"] = static_cast<int>(tracks.size());
		result["notes"] = noteCount;
		printJson(result);
	}
	else
	{
		fprintf(stdout, "Exported %d track(s), %d note(s) to %s\n",
			static_cast<int>(tracks.size()), noteCount,
			outMidi.toUtf8().constData());
		fflush(stdout);
	}

	return EXIT_SUCCESS;
}

} // namespace headless
} // namespace lmms
