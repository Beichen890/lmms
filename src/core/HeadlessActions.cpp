/*
 * HeadlessActions.cpp - implementations of the headless CLI actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "HeadlessActions.h"

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

#include "AudioBusHandle.h"
#include "AutomatableModel.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "DummyEffect.h"
#include "DummyInstrument.h"
#include "Effect.h"
#include "EffectChain.h"
#include "EffectControls.h"
#include "Engine.h"
#include "HeadlessUtils.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "Ladspa2LMMS.h"
#include "LadspaManager.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Plugin.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"
#include "TrackContainer.h"
#include "lmmsversion.h"

namespace lmms
{
namespace headless
{

namespace
{

const QString ErrorCodeProjectNotFound = "project-not-found";


void printJsonError(const QString& code, const QString& message)
{
	QJsonObject error;
	error["code"] = code;
	error["message"] = message;

	QJsonObject root;
	root["ok"] = false;
	root["error"] = error;

	fprintf(stdout, "%s\n", QJsonDocument(root).toJson(QJsonDocument::Compact).constData());
	fflush(stdout);
	fprintf(stderr, "%s\n", message.toUtf8().constData());
}


QString trackTypeName(Track::Type type)
{
	switch (type)
	{
	case Track::Type::Instrument: return "instrument";
	case Track::Type::Pattern: return "pattern";
	case Track::Type::Sample: return "sample";
	case Track::Type::Event: return "event";
	case Track::Type::Video: return "video";
	case Track::Type::Automation: return "automation";
	case Track::Type::HiddenAutomation: return "hiddenautomation";
	default: return "unknown";
	}
}


bool loadProject(const QString& path, QString* error)
{
	if (!QFileInfo::exists(path))
	{
		if (error) { *error = QString("project not found: %1").arg(path); }
		return false;
	}

	Engine::getSong()->loadProject(path);
	return true;
}


QList<AutomatableModel::ModelMetadata> collectMetadata(const std::function<void()>& fn)
{
	AutomatableModel::clearMetadataRegistry();
	AutomatableModel::setMetadataRegistryEnabled(true);
	fn();
	AutomatableModel::setMetadataRegistryEnabled(false);
	return AutomatableModel::metadataRegistry();
}


QString buildSavedProject(Song* song, bool canonical, bool minimal, QString* error)
{
	// Do not pre-create the target: LMMS saves by renaming a temporary file
	// over the destination, which fails on Windows when the destination
	// already exists. A temporary directory gives us a fresh path.
	QTemporaryDir tempDir(QDir::tempPath() + "/lmms-save-XXXXXX");
	if (!tempDir.isValid())
	{
		if (error) { *error = "could not create a temporary directory for saving"; }
		return QString();
	}
	const QString tempPath = tempDir.filePath("project.mmp");

	if (!song->saveProjectFile(tempPath))
	{
		if (error) { *error = QString("could not save project: %1").arg(tempPath); }
		QFile::remove(tempPath);
		return QString();
	}

	QFile file(tempPath);
	QString result;
	if (file.open(QIODevice::ReadOnly))
	{
		const QByteArray data = file.readAll();
		result = canonicalizeXml(data, canonical, minimal, error);
		file.close();
	}
	else if (error)
	{
		*error = QString("could not read back saved project: %1").arg(tempPath);
	}

	QFile::remove(tempPath);
	QFile::remove(tempPath + ".bak");
	return result;
}


bool saveSongTo(Song* song, const QString& output, bool canonical, bool minimal, QString* error)
{
	const QString xml = buildSavedProject(song, canonical, minimal, error);
	if (xml.isEmpty()) { return false; }

	QFile file(output);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		if (error) { *error = QString("could not open output file: %1").arg(output); }
		return false;
	}
	const QByteArray data = xml.toUtf8();
	const bool ok = file.write(data) == data.size();
	file.close();
	if (!ok && error) { *error = QString("could not write output file: %1").arg(output); }
	return ok;
}


Track* findTrack(const QString& spec, Track* lastTrack, QString* error)
{
	Song* song = Engine::getSong();

	if (spec.isEmpty() || spec.compare("last", Qt::CaseInsensitive) == 0)
	{
		if (lastTrack) { return lastTrack; }
		if (error) { *error = "no track has been added yet"; }
		return nullptr;
	}

	auto findByExactName = [&](const TrackContainer::TrackList& tracks) -> Track* {
		for (Track* track : tracks)
		{
			if (track->name() == spec) { return track; }
		}
		return nullptr;
	};
	auto findByLooseName = [&](const TrackContainer::TrackList& tracks) -> Track* {
		for (Track* track : tracks)
		{
			if (track->name().compare(spec, Qt::CaseInsensitive) == 0) { return track; }
		}
		return nullptr;
	};

	if (Track* track = findByExactName(song->tracks())) { return track; }
	if (Track* track = findByExactName(Engine::patternStore()->tracks())) { return track; }
	if (Track* track = findByLooseName(song->tracks())) { return track; }
	if (Track* track = findByLooseName(Engine::patternStore()->tracks())) { return track; }

	bool numeric = false;
	const int index = spec.toInt(&numeric);
	if (numeric)
	{
		const auto& tracks = song->tracks();
		if (index >= 0 && index < static_cast<int>(tracks.size()))
		{
			return tracks[static_cast<std::size_t>(index)];
		}
	}

	if (error) { *error = QString("track not found: %1").arg(spec); }
	return nullptr;
}


InstrumentTrack* resolveInstrumentTrack(Track* track)
{
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
	{
		return instrumentTrack;
	}

	if (dynamic_cast<PatternTrack*>(track) != nullptr)
	{
		for (Track* candidate : Engine::patternStore()->tracks())
		{
			if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(candidate))
			{
				if (candidate->name().compare(track->name(), Qt::CaseInsensitive) == 0)
				{
					return instrumentTrack;
				}
			}
		}
	}

	return nullptr;
}


AudioBusHandle* audioBusForTrack(Track* track)
{
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
	{
		return instrumentTrack->audioBusHandle();
	}
	if (auto sampleTrack = dynamic_cast<SampleTrack*>(track))
	{
		return sampleTrack->audioBusHandle();
	}
	return nullptr;
}


MidiClip* midiClipForTrack(InstrumentTrack* track, int patternIndex)
{
	if (patternIndex >= 0)
	{
		if (track->trackContainer() == Engine::patternStore())
		{
			track->createClipsForPattern(patternIndex);
		}
		else
		{
			while (track->numOfClips() <= patternIndex)
			{
				track->createClip(TimePos(track->numOfClips() * TimePos::ticksPerBar()));
			}
		}

		if (patternIndex < track->numOfClips())
		{
			return dynamic_cast<MidiClip*>(track->getClip(patternIndex));
		}
		return nullptr;
	}

	for (Clip* clip : track->getClips())
	{
		if (auto midiClip = dynamic_cast<MidiClip*>(clip)) { return midiClip; }
	}

	return dynamic_cast<MidiClip*>(track->createClip(TimePos(0)));
}


bool setEffectParameter(Effect* effect, const QString& name, double value)
{
	const auto records = collectMetadata([&] {
		QDomDocument doc;
		QDomElement root = doc.createElement("effect");
		effect->saveSettings(doc, root);
	});

	for (const auto& record : records)
	{
		if (record.name.compare(name, Qt::CaseInsensitive) == 0 ||
			record.model->displayName().compare(name, Qt::CaseInsensitive) == 0)
		{
			const_cast<AutomatableModel*>(record.model)->setValue(static_cast<float>(value));
			return true;
		}
	}
	return false;
}


QJsonObject modelToJson(const QString& name, const AutomatableModel* model)
{
	QJsonObject obj;
	obj["name"] = name;
	obj["id"] = static_cast<double>(ProjectJournal::idToSave(model->id()));
	obj["min"] = model->minValue<float>();
	obj["max"] = model->maxValue<float>();
	obj["default"] = model->initValue<float>();
	obj["value"] = model->value<float>();
	return obj;
}


QJsonArray effectModelsToJson(Effect* effect, QString* controlNode)
{
	QDomDocument doc;
	QDomElement root = doc.createElement("effect");
	const auto records = collectMetadata([&] { effect->saveState(doc, root); });

	if (controlNode) { *controlNode = effect->controls()->nodeName(); }

	QJsonArray models;
	QSet<const AutomatableModel*> seen;
	QSet<QString> names;
	for (const auto& record : records)
	{
		if (seen.contains(record.model) || names.contains(record.name)) { continue; }
		seen.insert(record.model);
		names.insert(record.name);
		models.append(modelToJson(record.name, record.model));
	}
	return models;
}


QString descriptorNote(const Plugin::Descriptor* descriptor)
{
	return descriptor->subPluginFeatures ? QString("subplugin") : QString();
}


void addInstrumentParameters(const Plugin::Descriptor* descriptor,
	InstrumentTrack* track, QJsonArray& parameters, QString* note)
{
	*note = descriptorNote(descriptor);
	if (descriptor->subPluginFeatures) { return; }

	Instrument* instrument = Instrument::instantiate(
		QString::fromUtf8(descriptor->name), track, nullptr);
	if (instrument == nullptr || instrument->nodeName() == "dummyinstrument")
	{
		delete instrument;
		*note = "instantiation-failed";
		return;
	}

	QDomDocument doc;
	QDomElement root = doc.createElement("instrument");
	const auto records = collectMetadata([&] { instrument->saveState(doc, root); });

	QSet<const AutomatableModel*> seen;
	QSet<QString> names;
	for (const auto& record : records)
	{
		if (seen.contains(record.model) || names.contains(record.name)) { continue; }
		seen.insert(record.model);
		names.insert(record.name);
		parameters.append(modelToJson(record.name, record.model));
	}

	// The metadata records refer to models owned by the instrument, so the
	// instance must stay alive until all values have been read. Do not delete
	// the instrument here: some instruments (e.g. ZynAddSubFX) own worker
	// threads and destroying them right after a synchronous saveState() can
	// race with plugin startup. This is a short-lived CLI process, so leaking
	// the introspection instance is safe.
}


void addEffectParameters(const Plugin::Descriptor* descriptor,
	EffectChain* chain, QJsonArray& parameters, QString* controlNode, QString* note)
{
	*note = descriptorNote(descriptor);
	if (descriptor->subPluginFeatures) { return; }

	Effect* effect = Effect::instantiate(
		QString::fromUtf8(descriptor->name), chain, nullptr);
	if (effect == nullptr || dynamic_cast<DummyEffect*>(effect) != nullptr)
	{
		delete effect;
		*note = "instantiation-failed";
		return;
	}

	parameters = effectModelsToJson(effect, controlNode);
	delete effect;
}


bool isNoHint(float value)
{
	return std::abs(value - NOHINT) < 1.0f;
}


QJsonValue ladspaValue(float value)
{
	return isNoHint(value) ? QJsonValue() : QJsonValue(static_cast<double>(value));
}

} // namespace


int runSaveAction(const QString& input, const QString& output,
	bool canonical, bool minimal)
{
	QString error;
	if (!loadProject(input, &error))
	{
		fprintf(stderr, "%s\n", error.toUtf8().constData());
		return EXIT_FAILURE;
	}

	const QString xml = buildSavedProject(Engine::getSong(), canonical, minimal, &error);
	if (xml.isEmpty())
	{
		fprintf(stderr, "%s\n", error.toUtf8().constData());
		return EXIT_FAILURE;
	}

	if (output.isEmpty())
	{
		const QByteArray data = xml.toUtf8();
		fwrite(data.constData(), 1, data.size(), stdout);
		fflush(stdout);
		return EXIT_SUCCESS;
	}

	QFile file(output);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		fprintf(stderr, "could not open output file: %s\n", output.toUtf8().constData());
		return EXIT_FAILURE;
	}
	const QByteArray data = xml.toUtf8();
	if (file.write(data) != data.size())
	{
		fprintf(stderr, "could not write output file: %s\n", output.toUtf8().constData());
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


int runScriptAction(const QString& project, const QString& scriptFile,
	const QString& output, bool jsonOutput)
{
	auto reportFailure = [&](const QString& code, const QString& message) {
		if (jsonOutput) { printJsonError(code, message); }
		else { fprintf(stderr, "%s\n", message.toUtf8().constData()); }
		return EXIT_FAILURE;
	};

	QFile script(scriptFile);
	if (!script.open(QIODevice::ReadOnly))
	{
		return reportFailure("script-not-found",
			QString("could not open script file: %1").arg(scriptFile));
	}

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(script.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError)
	{
		return reportFailure("invalid-script",
			QString("invalid JSON in %1: %2").arg(scriptFile, parseError.errorString()));
	}

	QJsonArray commands;
	if (doc.isArray())
	{
		commands = doc.array();
	}
	else if (doc.isObject())
	{
		const QJsonValue value = doc.object().value("commands");
		if (!value.isArray())
		{
			return reportFailure("invalid-script", "script object must contain a \"commands\" array");
		}
		commands = value.toArray();
	}
	else
	{
		return reportFailure("invalid-script", "script must be a JSON array or object");
	}

	QString error;
	if (!loadProject(project, &error))
	{
		return reportFailure(ErrorCodeProjectNotFound, error);
	}

	Song* song = Engine::getSong();
	Track* lastTrack = nullptr;
	QString savePath = output;
	bool saved = false;

	auto fail = [&](const QString& code, const QString& message) {
		if (jsonOutput) { printJsonError(code, message); }
		else { fprintf(stderr, "%s\n", message.toUtf8().constData()); }
		return EXIT_FAILURE;
	};

	for (int i = 0; i < commands.size(); ++i)
	{
		if (!commands.at(i).isObject())
		{
			return fail("invalid-script",
				QString("command %1 must be an object").arg(i));
		}
		const QJsonObject command = commands.at(i).toObject();
		const QString op = command.value("op").toString(
			command.value("action").toString(command.value("cmd").toString())).trimmed().toLower();

		if (op == "settempo")
		{
			const double bpm = command.value("bpm").toDouble(command.value("value").toDouble(0.0));
			if (bpm <= 0.0)
			{
				return fail("invalid-script", "setTempo requires a positive \"bpm\"");
			}
			song->tempoModel().setValue(static_cast<float>(bpm));
		}
		else if (op == "addtrack")
		{
			const QString type = command.value("type").toString("instrument").toLower();
			const QString name = command.value("name").toString();
			const QString plugin = command.value("plugin").toString();

			Track::Type trackType = Track::Type::Instrument;
			if (type == "instrument") { trackType = Track::Type::Instrument; }
			else if (type == "sample") { trackType = Track::Type::Sample; }
			else if (type == "pattern") { trackType = Track::Type::Pattern; }
			else if (type == "automation") { trackType = Track::Type::Automation; }
			else { return fail("invalid-script", QString("unknown track type: %1").arg(type)); }

			Track* track = Track::create(trackType, song);
			if (!track)
			{
				return fail("invalid-script", QString("could not create track of type %1").arg(type));
			}

			if (trackType == Track::Type::Instrument && !plugin.isEmpty())
			{
				auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
				Instrument* instrument = instrumentTrack->loadInstrument(plugin);
				if (instrument == nullptr || instrument->nodeName() == "dummyinstrument")
				{
					return fail("plugin-missing",
						QString("instrument plugin not available: %1").arg(plugin));
				}
			}
			else if (trackType == Track::Type::Pattern)
			{
				// Make the pattern audible: create a matching track in the pattern store.
				auto patternInstrumentTrack = dynamic_cast<InstrumentTrack*>(
					Track::create(Track::Type::Instrument, Engine::patternStore()));
				if (patternInstrumentTrack)
				{
					patternInstrumentTrack->setName(track->name());
					if (!plugin.isEmpty())
					{
						Instrument* instrument = patternInstrumentTrack->loadInstrument(plugin);
						if (instrument == nullptr || instrument->nodeName() == "dummyinstrument")
						{
							return fail("plugin-missing",
								QString("instrument plugin not available: %1").arg(plugin));
						}
					}
				}
			}
			else if (trackType == Track::Type::Sample && !plugin.isEmpty() && !jsonOutput)
			{
				fprintf(stderr, "warning: sample tracks do not accept a plugin; ignoring \"%s\"\n",
					plugin.toUtf8().constData());
			}

			// Loading an instrument resets the track name to the plugin's
			// display name, so apply the requested name afterwards.
			if (!name.isEmpty()) { track->setName(name); }

			lastTrack = track;
		}
		else if (op == "addnotes")
		{
			const QString trackSpec = command.value("track").toString();
			Track* track = findTrack(trackSpec, lastTrack, &error);
			if (!track) { return fail("invalid-script", error); }

			InstrumentTrack* instrumentTrack = resolveInstrumentTrack(track);
			if (!instrumentTrack)
			{
				return fail("invalid-script",
					QString("track \"%1\" cannot hold MIDI notes").arg(track->name()));
			}

			const int patternIndex = command.contains("pattern")
				? command.value("pattern").toInt(-1) : -1;
			MidiClip* clip = midiClipForTrack(instrumentTrack, patternIndex);
			if (!clip)
			{
				return fail("invalid-script",
					QString("could not create MIDI clip on track \"%1\"").arg(track->name()));
			}

			const QJsonArray notes = command.value("notes").toArray();
			if (notes.isEmpty())
			{
				return fail("invalid-script", "addNotes requires a non-empty \"notes\" array");
			}
			for (const auto& noteValue : notes)
			{
				const QJsonObject note = noteValue.toObject();
				const int key = note.value("key").toInt(60);
				const int pos = note.value("pos").toInt(0);
				const int len = note.value("len").toInt(TimePos::ticksPerBar() / 4);
				const int velocity = qBound<int>(MinVolume,
					note.value("vel").toInt(DefaultVolume), MaxVolume);

				clip->addNote(Note(TimePos(len), TimePos(pos), key,
					static_cast<volume_t>(velocity)), false);
			}
		}
		else if (op == "addeffect")
		{
			const QString trackSpec = command.value("track").toString();
			const QString plugin = command.value("plugin").toString();
			if (plugin.isEmpty())
			{
				return fail("invalid-script", "addEffect requires \"plugin\"");
			}

			Track* track = findTrack(trackSpec, lastTrack, &error);
			if (!track) { return fail("invalid-script", error); }

			Track* effectHost = track;
			if (auto instrumentTrack = resolveInstrumentTrack(track))
			{
				effectHost = instrumentTrack;
			}
			AudioBusHandle* bus = audioBusForTrack(effectHost);
			if (!bus)
			{
				return fail("invalid-script",
					QString("track \"%1\" cannot hold effects").arg(track->name()));
			}

			Effect* effect = Effect::instantiate(plugin, bus->effects(), nullptr);
			if (!effect || dynamic_cast<DummyEffect*>(effect) != nullptr)
			{
				delete effect;
				return fail("plugin-missing",
					QString("effect plugin not available: %1").arg(plugin));
			}
			bus->effects()->appendEffect(effect);

			const QJsonObject params = command.value("params").toObject();
			for (auto it = params.begin(); it != params.end(); ++it)
			{
				if (!setEffectParameter(effect, it.key(), it.value().toDouble()))
				{
					return fail("invalid-script",
						QString("unknown parameter \"%1\" for effect \"%2\"").arg(it.key(), plugin));
				}
			}
		}
		else if (op == "settrackvolume")
		{
			const QString trackSpec = command.value("track").toString();
			Track* track = findTrack(trackSpec, lastTrack, &error);
			if (!track) { return fail("invalid-script", error); }
			auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
			if (!instrumentTrack)
			{
				instrumentTrack = resolveInstrumentTrack(track);
			}
			if (!instrumentTrack)
			{
				return fail("invalid-script",
					QString("track \"%1\" has no volume").arg(track->name()));
			}
			const double volume = command.value("volume").toDouble(
				command.value("value").toDouble());
			instrumentTrack->volumeModel()->setValue(static_cast<float>(volume));
		}
		else if (op == "save")
		{
			const QString commandOut = command.value("out").toString();
			if (!commandOut.isEmpty()) { savePath = commandOut; }
			if (savePath.isEmpty())
			{
				savePath = project + ".scripted.mmp";
			}
			if (!saveSongTo(song, savePath, false, false, &error))
			{
				return fail("save-failed", error);
			}
			saved = true;
		}
		else
		{
			return fail("invalid-script",
				QString("unknown command \"%1\"").arg(op));
		}
	}

	if (!saved || savePath.isEmpty())
	{
		if (savePath.isEmpty()) { savePath = project + ".scripted.mmp"; }
	}
	if (!saveSongTo(song, savePath, false, false, &error))
	{
		return fail("save-failed", error);
	}

	if (jsonOutput)
	{
		QJsonObject result;
		result["ok"] = true;
		result["out"] = savePath;
		result["commands"] = commands.size();
		fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		fflush(stdout);
	}
	else
	{
		fprintf(stdout, "Wrote %s\n", savePath.toUtf8().constData());
	}

	return EXIT_SUCCESS;
}


int runPluginsJsonAction(const QString& typeFilter)
{
	const QString filter = typeFilter.trimmed().toLower();
	if (!QStringList{"all", "instrument", "effect", "ladspa", "vst"}.contains(filter))
	{
		fprintf(stderr, "invalid --type value: %s\n", typeFilter.toUtf8().constData());
		return EXIT_FAILURE;
	}

	const bool includeInstruments = filter == "all" || filter == "instrument";
	const bool includeEffects = filter == "all" || filter == "effect";
	const bool includeLadspa = filter == "all" || filter == "ladspa";
	const bool includeVst = filter == "all" || filter == "vst";
	Q_UNUSED(includeVst)

	// PatternStore is a concrete TrackContainer and is available in headless
	// mode; the track is only used as a parent for metadata instantiation.
	InstrumentTrack* toolTrack = new InstrumentTrack(Engine::patternStore());
	EffectChain toolChain(nullptr);

	QJsonArray instruments;
	QJsonArray effects;

	if (includeInstruments || includeEffects)
	{
		QList<Plugin::Descriptor*> descriptors = getPluginFactory()->descriptors();
		std::sort(descriptors.begin(), descriptors.end(),
			[](const Plugin::Descriptor* a, const Plugin::Descriptor* b) {
				return QString::fromUtf8(a->name) < QString::fromUtf8(b->name);
			});

		for (const auto* descriptor : descriptors)
		{
			const QString name = QString::fromUtf8(descriptor->name);
			if (includeInstruments && descriptor->type == Plugin::Type::Instrument)
			{
				QJsonObject plugin;
				plugin["name"] = name;
				plugin["displayName"] = QString::fromUtf8(descriptor->displayName);

				QJsonArray parameters;
				QString note;
				addInstrumentParameters(descriptor, toolTrack, parameters, &note);
				plugin["parameters"] = parameters;
				if (!note.isEmpty()) { plugin["note"] = note; }
				instruments.append(plugin);
			}
			else if (includeEffects && descriptor->type == Plugin::Type::Effect)
			{
				QJsonObject plugin;
				plugin["name"] = name;
				plugin["displayName"] = QString::fromUtf8(descriptor->displayName);

				QJsonArray parameters;
				QString controlNode;
				QString note;
				addEffectParameters(descriptor, &toolChain, parameters, &controlNode, &note);
				plugin["controlNode"] = controlNode;
				plugin["parameters"] = parameters;
				if (!note.isEmpty()) { plugin["note"] = note; }
				effects.append(plugin);
			}
		}
	}

	QJsonArray ladspa;
	if (includeLadspa && Engine::getLADSPAManager() != nullptr)
	{
		const auto plugins = Engine::getLADSPAManager()->getSortedPlugins();
		for (const auto& entry : plugins)
		{
			const ladspa_key_t& key = entry.second;
			QJsonObject plugin;
			plugin["file"] = key.first;
			plugin["label"] = key.second;
			plugin["name"] = Engine::getLADSPAManager()->getName(key);

			QJsonArray ports;
			const auto portCount = Engine::getLADSPAManager()->getPortCount(key);
			for (uint32_t port = 0; port < portCount; ++port)
			{
				if (!Engine::getLADSPAManager()->isPortControl(key, port) ||
					!Engine::getLADSPAManager()->isPortInput(key, port))
				{
					continue;
				}
				QJsonObject portObj;
				portObj["name"] = Engine::getLADSPAManager()->getPortName(key, port);
				portObj["min"] = ladspaValue(Engine::getLADSPAManager()->getLowerBound(key, port));
				portObj["max"] = ladspaValue(Engine::getLADSPAManager()->getUpperBound(key, port));
				portObj["default"] = ladspaValue(Engine::getLADSPAManager()->getDefaultSetting(key, port));
				ports.append(portObj);
			}
			plugin["ports"] = ports;
			ladspa.append(plugin);
		}
	}

	// VST scanning is environment dependent and not exposed through a stable
	// headless API; report an empty list rather than inventing entries.
	QJsonArray vst;

	QJsonObject result;
	result["version"] = QString::fromUtf8(LMMS_VERSION);
	result["instruments"] = instruments;
	result["effects"] = effects;
	result["ladspa"] = ladspa;
	result["vst"] = vst;

	fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
	fflush(stdout);

	delete toolTrack;
	return EXIT_SUCCESS;
}


int runNormalizeBaseNoteAction(const QString& project, const QString& output,
	bool inPlace, bool dryRun, bool jsonOutput)
{
	QString error;
	if (!loadProject(project, &error))
	{
		if (jsonOutput) { printJsonError(ErrorCodeProjectNotFound, error); }
		else { fprintf(stderr, "%s\n", error.toUtf8().constData()); }
		return EXIT_FAILURE;
	}

	QJsonArray changedTracks;
	int changed = 0;

	auto processContainer = [&](const TrackContainer::TrackList& tracks) {
		for (Track* track : tracks)
		{
			auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
			if (!instrumentTrack) { continue; }
			if (instrumentTrack->baseNoteModel()->value() != 57) { continue; }

			instrumentTrack->baseNoteModel()->setValue(69);
			changedTracks.append(track->name());
			++changed;
		}
	};
	processContainer(Engine::getSong()->tracks());
	processContainer(Engine::patternStore()->tracks());

	const int pitchShift = changed > 0 ? 12 : 0;
	QString savedTo = output;
	if (!dryRun && (inPlace || !output.isEmpty()))
	{
		if (savedTo.isEmpty()) { savedTo = project; }
		if (!saveSongTo(Engine::getSong(), savedTo, false, false, &error))
		{
			if (jsonOutput) { printJsonError("save-failed", error); }
			else { fprintf(stderr, "%s\n", error.toUtf8().constData()); }
			return EXIT_FAILURE;
		}
	}

	if (jsonOutput)
	{
		QJsonObject result;
		result["ok"] = true;
		result["changed"] = changed;
		result["pitchShiftSemitones"] = pitchShift;
		result["tracks"] = changedTracks;
		result["written"] = !dryRun && (inPlace || !output.isEmpty());
		if (!savedTo.isEmpty() && !dryRun) { result["file"] = savedTo; }
		fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		fflush(stdout);
	}
	else
	{
		fprintf(stderr, "%d instrument track(s) use basenote 57", changed);
		if (changed > 0)
		{
			fprintf(stderr, "; migrating to 69 shifts pitch by +12 semitones");
		}
		fprintf(stderr, "\n");
		if (dryRun || (!inPlace && output.isEmpty()))
		{
			fprintf(stderr, "No file was modified (use --in-place or --out).\n");
		}
		else
		{
			fprintf(stdout, "Wrote %s\n", savedTo.toUtf8().constData());
		}
	}

	return EXIT_SUCCESS;
}


int runModelIdsAction(const QString& project)
{
	QString error;
	if (!loadProject(project, &error))
	{
		printJsonError(ErrorCodeProjectNotFound, error);
		return EXIT_FAILURE;
	}

	auto handleContainer = [](const TrackContainer::TrackList& tracks, QJsonArray& output) {
		int index = 0;
		for (Track* track : tracks)
		{
			QJsonObject trackObj;
			trackObj["index"] = index++;
			trackObj["name"] = track->name();
			trackObj["type"] = trackTypeName(track->type());

			QJsonArray models;
			if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
			{
				models.append(modelToJson("vol", instrumentTrack->volumeModel()));
				models.append(modelToJson("pan", instrumentTrack->panningModel()));
				models.append(modelToJson("pitch", instrumentTrack->pitchModel()));
				models.append(modelToJson("pitchrange", instrumentTrack->pitchRangeModel()));
				models.append(modelToJson("mixch", instrumentTrack->mixerChannelModel()));
				models.append(modelToJson("basenote", instrumentTrack->baseNoteModel()));
				models.append(modelToJson("firstkey", instrumentTrack->firstKeyModel()));
				models.append(modelToJson("lastkey", instrumentTrack->lastKeyModel()));
			}
			trackObj["models"] = models;

			QJsonArray effects;
			if (AudioBusHandle* bus = audioBusForTrack(track))
			{
				int effectIndex = 0;
				for (Effect* effect : bus->effects()->findChildren<Effect*>())
				{
					if (dynamic_cast<DummyEffect*>(effect) != nullptr)
					{
						++effectIndex;
						continue;
					}
					QJsonObject effectObj;
					effectObj["index"] = effectIndex++;
					effectObj["plugin"] = QString::fromUtf8(effect->descriptor()->name);
					QString controlNode;
					effectObj["models"] = effectModelsToJson(effect, &controlNode);
					effectObj["controlNode"] = controlNode;
					effects.append(effectObj);
				}
			}
			trackObj["effects"] = effects;

			output.append(trackObj);
		}
	};

	QJsonArray songTracks;
	handleContainer(Engine::getSong()->tracks(), songTracks);
	QJsonArray patternTracks;
	handleContainer(Engine::patternStore()->tracks(), patternTracks);

	QJsonObject result;
	result["ok"] = true;
	result["project"] = project;
	result["tracks"] = songTracks;
	result["patternStore"] = patternTracks;

	fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
	fflush(stdout);
	return EXIT_SUCCESS;
}


int runAutomationAddAction(const QString& project, const QString& trackName,
	int effectIndex, const QString& paramName,
	const QList<AutomationPoint>& points, const QString& output)
{
	QString error;
	if (!loadProject(project, &error))
	{
		fprintf(stderr, "%s\n", error.toUtf8().constData());
		return EXIT_FAILURE;
	}

	Track* track = findTrack(trackName, nullptr, &error);
	if (!track)
	{
		fprintf(stderr, "%s\n", error.toUtf8().constData());
		return EXIT_FAILURE;
	}

	AutomatableModel* target = nullptr;

	if (effectIndex >= 0)
	{
		AudioBusHandle* bus = audioBusForTrack(track);
		if (!bus)
		{
			fprintf(stderr, "track \"%s\" has no effects\n", trackName.toUtf8().constData());
			return EXIT_FAILURE;
		}

		QList<Effect*> effects;
		for (Effect* effect : bus->effects()->findChildren<Effect*>())
		{
			effects.append(effect);
		}
		if (effectIndex >= effects.size())
		{
			fprintf(stderr, "effect index %d out of range (track \"%s\" has %d effect(s))\n",
				effectIndex, trackName.toUtf8().constData(), static_cast<int>(effects.size()));
			return EXIT_FAILURE;
		}

		Effect* effect = effects[effectIndex];
		if (dynamic_cast<DummyEffect*>(effect) != nullptr)
		{
			fprintf(stderr, "effect %d on track \"%s\" is a missing plugin\n",
				effectIndex, trackName.toUtf8().constData());
			return EXIT_FAILURE;
		}
		QDomDocument doc;
		QDomElement root = doc.createElement("effect");
		const auto records = collectMetadata([&] { effect->saveState(doc, root); });

		QStringList available;
		for (const auto& record : records)
		{
			available << record.name;
			if (target == nullptr &&
				(record.name.compare(paramName, Qt::CaseInsensitive) == 0 ||
				 record.model->displayName().compare(paramName, Qt::CaseInsensitive) == 0))
			{
				target = const_cast<AutomatableModel*>(record.model);
			}
		}
		if (!target)
		{
			fprintf(stderr, "unknown parameter \"%s\" for effect %d; available: %s\n",
				paramName.toUtf8().constData(), effectIndex,
				available.join(", ").toUtf8().constData());
			return EXIT_FAILURE;
		}
	}
	else
	{
		auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
		if (!instrumentTrack)
		{
			fprintf(stderr, "track \"%s\" has no automatable parameters\n", trackName.toUtf8().constData());
			return EXIT_FAILURE;
		}

		const QMap<QString, AutomatableModel*> models {
			{"vol", instrumentTrack->volumeModel()},
			{"volume", instrumentTrack->volumeModel()},
			{"pan", instrumentTrack->panningModel()},
			{"panning", instrumentTrack->panningModel()},
			{"pitch", instrumentTrack->pitchModel()},
			{"pitchrange", instrumentTrack->pitchRangeModel()},
			{"mixch", instrumentTrack->mixerChannelModel()},
			{"basenote", instrumentTrack->baseNoteModel()},
			{"firstkey", instrumentTrack->firstKeyModel()},
			{"lastkey", instrumentTrack->lastKeyModel()},
		};
		for (auto it = models.begin(); it != models.end(); ++it)
		{
			if (it.key().compare(paramName, Qt::CaseInsensitive) == 0)
			{
				target = it.value();
				break;
			}
		}
		if (!target)
		{
			fprintf(stderr, "unknown track parameter \"%s\"\n", paramName.toUtf8().constData());
			return EXIT_FAILURE;
		}
	}

	if (points.isEmpty())
	{
		fprintf(stderr, "at least one --point is required\n");
		return EXIT_FAILURE;
	}

	auto automationTrack = dynamic_cast<AutomationTrack*>(
		Track::create(Track::Type::Automation, Engine::getSong()));
	if (!automationTrack)
	{
		fprintf(stderr, "could not create automation track\n");
		return EXIT_FAILURE;
	}
	automationTrack->setName(QString("Automation: %1/%2").arg(track->name(), paramName));

	auto clip = dynamic_cast<AutomationClip*>(automationTrack->createClip(TimePos(0)));
	if (!clip)
	{
		fprintf(stderr, "could not create automation clip\n");
		return EXIT_FAILURE;
	}

	clip->setProgressionType(AutomationClip::ProgressionType::Linear);
	clip->addObject(target, false);
	for (const auto& point : points)
	{
		clip->putValue(TimePos(point.tick),
			target->inverseScaledValue(static_cast<float>(point.value)), false);
	}
	clip->updateLength();

	if (output.isEmpty())
	{
		fprintf(stderr, "--out <out.mmp> is required\n");
		return EXIT_FAILURE;
	}
	if (!saveSongTo(Engine::getSong(), output, false, false, &error))
	{
		fprintf(stderr, "%s\n", error.toUtf8().constData());
		return EXIT_FAILURE;
	}

	fprintf(stdout, "Wrote %s with %d automation point(s)\n",
		output.toUtf8().constData(), static_cast<int>(points.size()));
	return EXIT_SUCCESS;
}

} // namespace headless
} // namespace lmms
