/*
 * HeadlessValidation.cpp - validation of a project for the headless CLI
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "HeadlessValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include "AudioBusHandle.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Clip.h"
#include "DummyEffect.h"
#include "DummyInstrument.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "PathUtil.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"

namespace lmms
{
namespace headless
{

namespace
{

const QString ErrorCodeProjectNotFound = "project-not-found";
const QString ErrorCodeEmptyProject = "empty-project";
const QString ErrorCodePluginMissing = "plugin-missing";
const QString ErrorCodeSampleMissing = "sample-missing";
const QString ErrorCodeEffectControlsName = "effect-controls-name";
const QString ErrorCodeUnboundAutomation = "unbound-automation";
const QString ErrorCodeEmptyTrack = "empty-track";

const QString SeverityError = "error";
const QString SeverityWarning = "warning";

struct Issue
{
	QString severity;
	QString code;
	QString track;
	QString message;
};


void addIssue(QList<Issue>& issues, QSet<QString>& seen, const QString& severity,
	const QString& code, const QString& track, const QString& message)
{
	const QString key = severity + QChar(0x1f) + code + QChar(0x1f) + track
		+ QChar(0x1f) + message;
	if (seen.contains(key)) { return; }
	seen.insert(key);
	issues.append(Issue{severity, code, track, message});
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


InstrumentTrack* instrumentTrackOf(Track* track)
{
	return dynamic_cast<InstrumentTrack*>(track);
}


//! Whether a MIDI clip holds at least one note.
bool midiClipHasNotes(Clip* clip)
{
	auto midiClip = dynamic_cast<MidiClip*>(clip);
	return midiClip != nullptr && !midiClip->notes().empty();
}


bool instrumentTrackHasNotes(Track* track)
{
	for (Clip* clip : track->getClips())
	{
		if (midiClipHasNotes(clip)) { return true; }
	}
	return false;
}


//! Whether the pattern stored at @p pattern has any notes or non-MIDI clips.
bool patternHasContent(int pattern)
{
	if (pattern < 0) { return false; }

	for (Track* track : Engine::patternStore()->tracks())
	{
		if (pattern >= track->numOfClips()) { continue; }
		Clip* clip = track->getClip(static_cast<std::size_t>(pattern));
		if (clip == nullptr) { continue; }
		if (auto midiClip = dynamic_cast<MidiClip*>(clip))
		{
			if (!midiClip->notes().empty()) { return true; }
		}
		else
		{
			// Sample/automation clips are content on their own.
			return true;
		}
	}
	return false;
}


void scanPlugins(Track* track, QList<Issue>& issues, QSet<QString>& seen)
{
	if (auto instrumentTrack = instrumentTrackOf(track))
	{
		Instrument* instrument = instrumentTrack->instrument();
		if (instrument != nullptr &&
			dynamic_cast<DummyInstrument*>(instrument) != nullptr)
		{
			addIssue(issues, seen, SeverityError, ErrorCodePluginMissing,
				track->name(), "instrument plugin could not be loaded");
		}
	}

	AudioBusHandle* bus = audioBusForTrack(track);
	if (bus == nullptr || bus->effects() == nullptr) { return; }

	for (Effect* effect : bus->effects()->findChildren<Effect*>())
	{
		auto dummy = dynamic_cast<DummyEffect*>(effect);
		if (dummy == nullptr) { continue; }

		const QString name = dummy->originalPluginData().attribute("name");
		const QString message = name.isEmpty()
			? QString("effect plugin could not be loaded")
			: QString("effect plugin \"%1\" could not be loaded").arg(name);
		addIssue(issues, seen, SeverityError, ErrorCodePluginMissing,
			track->name(), message);
	}
}


void scanEmptyTrack(Track* track, QList<Issue>& issues, QSet<QString>& seen)
{
	switch (track->type())
	{
	case Track::Type::Instrument:
		if (!instrumentTrackHasNotes(track))
		{
			addIssue(issues, seen, SeverityWarning, ErrorCodeEmptyTrack,
				track->name(), "instrument track has no notes");
		}
		break;
	case Track::Type::Sample:
		if (track->getClips().empty())
		{
			addIssue(issues, seen, SeverityWarning, ErrorCodeEmptyTrack,
				track->name(), "sample track has no clips");
		}
		break;
	case Track::Type::Pattern:
	{
		auto patternTrack = dynamic_cast<PatternTrack*>(track);
		if (patternTrack != nullptr &&
			!patternHasContent(patternTrack->patternIndex()))
		{
			addIssue(issues, seen, SeverityWarning, ErrorCodeEmptyTrack,
				track->name(), "pattern track has no notes");
		}
		break;
	}
	default:
		break;
	}
}


void scanAutomation(Track* track, QList<Issue>& issues, QSet<QString>& seen)
{
	if (track->type() != Track::Type::Automation &&
		track->type() != Track::Type::HiddenAutomation)
	{
		return;
	}

	for (Clip* clip : track->getClips())
	{
		auto automationClip = dynamic_cast<AutomationClip*>(clip);
		if (automationClip == nullptr) { continue; }

		const auto& objects = automationClip->objects();
		const bool bound = std::any_of(objects.begin(), objects.end(),
			[](const QPointer<AutomatableModel>& object) { return !object.isNull(); });
		if (!bound)
		{
			addIssue(issues, seen, SeverityWarning, ErrorCodeUnboundAutomation,
				track->name(), "automation clip has no bound model");
		}
	}
}


QString normalizedControlNodeName(const QString& name)
{
	QString normalized = name.toLower();
	normalized.remove(QLatin1Char('_'));
	normalized.remove(QLatin1Char('-'));
	return normalized;
}


bool isNonControlNodeName(const QString& name)
{
	static const QStringList ignored = {
		"on", "wet", "mix", "autoquit", "chunk", "state", "sampledata", "key"
	};
	return ignored.contains(name.toLower());
}


QString ancestorTrackName(QDomElement element)
{
	while (!element.isNull())
	{
		const QString nodeName = element.nodeName();
		if (nodeName == "track" || nodeName == "mixerchannel")
		{
			return element.attribute("name");
		}
		element = element.parentNode().toElement();
	}
	return QString();
}


//! Collects runtime effects by plugin name for the controls-node comparison.
QHash<QString, QString> runtimeControlNodeNames(
	const TrackContainer::TrackList& tracks)
{
	QHash<QString, QString> result;
	for (Track* track : tracks)
	{
		AudioBusHandle* bus = audioBusForTrack(track);
		if (bus == nullptr || bus->effects() == nullptr) { continue; }
		for (Effect* effect : bus->effects()->findChildren<Effect*>())
		{
			if (dynamic_cast<DummyEffect*>(effect) != nullptr) { continue; }
			if (effect->descriptor() == nullptr || effect->controls() == nullptr)
			{
				continue;
			}
			const QString name = QString::fromUtf8(effect->descriptor()->name);
			if (!result.contains(name))
			{
				result.insert(name, effect->controls()->nodeName());
			}
		}
	}
	return result;
}


void scanProjectXml(const QString& project,
	const QHash<QString, QString>& controlNodes,
	QSet<QString>& reportedSamples,
	QList<Issue>& issues, QSet<QString>& seen)
{
	QFile file(project);
	if (!file.open(QIODevice::ReadOnly)) { return; }

	QDomDocument document;
	if (!document.setContent(&file)) { return; }

	// Effect controls node names.
	const QDomNodeList effectNodes = document.elementsByTagName("effect");
	for (int i = 0; i < effectNodes.count(); ++i)
	{
		const QDomElement effectElement = effectNodes.at(i).toElement();
		const QString name = effectElement.attribute("name");
		if (name.isEmpty()) { continue; }

		auto expected = controlNodes.constFind(name);
		if (expected == controlNodes.constEnd()) { continue; }

		QString actual;
		for (QDomNode child = effectElement.firstChild(); !child.isNull();
			child = child.nextSibling())
		{
			if (!child.isElement()) { continue; }
			const QString childName = child.toElement().nodeName();
			if (isNonControlNodeName(childName)) { continue; }
			actual = childName;
			break;
		}
		if (actual.isEmpty()) { continue; }

		if (normalizedControlNodeName(actual)
			!= normalizedControlNodeName(expected.value()))
		{
			addIssue(issues, seen, SeverityWarning, ErrorCodeEffectControlsName,
				ancestorTrackName(effectElement),
				QString("effect \"%1\" stores controls as \"%2\" but the plugin "
					"expects \"%3\"")
					.arg(name, actual, expected.value()));
		}
	}

	// AudioFileProcessor sample paths.
	auto checkSample = [&](const QDomElement& element) {
		const QString src = element.attribute("src");
		if (src.isEmpty()) { return; }
		if (reportedSamples.contains(src)) { return; }

		bool resolved = false;
		const QString absolute = PathUtil::toAbsolute(src, &resolved);
		if (resolved && !absolute.isEmpty() && QFileInfo::exists(absolute))
		{
			return;
		}

		addIssue(issues, seen, SeverityError, ErrorCodeSampleMissing,
			ancestorTrackName(element),
			QString("sample not found: %1").arg(src));
	};

	const QDomNodeList afpNodes =
		document.elementsByTagName("audiofileprocessor");
	for (int i = 0; i < afpNodes.count(); ++i)
	{
		checkSample(afpNodes.at(i).toElement());
	}
}


int emitResult(const QString& project, const QList<Issue>& issues,
	bool jsonOutput)
{
	int errorCount = 0;
	int warningCount = 0;

	QJsonArray issueArray;
	for (const Issue& issue : issues)
	{
		if (issue.severity == SeverityError) { ++errorCount; }
		else { ++warningCount; }

		QJsonObject object;
		object["severity"] = issue.severity;
		object["code"] = issue.code;
		object["track"] = issue.track;
		object["message"] = issue.message;
		issueArray.append(object);
	}

	const bool ok = errorCount == 0;

	QJsonObject root;
	root["ok"] = ok;
	root["project"] = project;
	root["issues"] = issueArray;

	const QByteArray json = QJsonDocument(root).toJson(
		jsonOutput ? QJsonDocument::Compact : QJsonDocument::Indented);
	fwrite(json.constData(), 1, json.size(), stdout);
	fflush(stdout);

	for (const Issue& issue : issues)
	{
		QString label = issue.severity.toUpper();
		if (!issue.track.isEmpty())
		{
			label += QString(" [%1]").arg(issue.track);
		}
		fprintf(stderr, "%s %s: %s\n", qUtf8Printable(label),
			qUtf8Printable(issue.code), qUtf8Printable(issue.message));
	}
	fprintf(stderr, "%d error(s), %d warning(s)\n", errorCount, warningCount);

	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace


int runValidateAction(const QString& project, bool jsonOutput)
{
	QList<Issue> issues;
	QSet<QString> seen;

	const QFileInfo projectInfo(project);
	if (project.isEmpty() || !projectInfo.exists() || !projectInfo.isFile())
	{
		addIssue(issues, seen, SeverityError, ErrorCodeProjectNotFound, QString(),
			QString("project not found: %1").arg(project));
		return emitResult(project, issues, jsonOutput);
	}

	Engine::getSong()->loadProject(project);

	Song* song = Engine::getSong();
	PatternStore* patternStore = Engine::patternStore();

	if (song->tracks().empty() && patternStore->tracks().empty())
	{
		addIssue(issues, seen, SeverityError, ErrorCodeEmptyProject, QString(),
			"project contains no tracks");
	}

	// Sample problems collected by the loader itself.
	QSet<QString> reportedSamples;
	const QString errorSummary = song->errorSummary();
	const QString sampleNotFoundToken = QStringLiteral("Sample not found");
	for (const QString& line : errorSummary.split('\n', Qt::SkipEmptyParts))
	{
		if (!line.contains(sampleNotFoundToken, Qt::CaseInsensitive)) { continue; }

		addIssue(issues, seen, SeverityError, ErrorCodeSampleMissing, QString(), line);

		QString rest = line.mid(line.indexOf(sampleNotFoundToken, 0,
			Qt::CaseInsensitive) + sampleNotFoundToken.size()).trimmed();
		if (rest.startsWith(QLatin1Char(':')) || rest.startsWith(QChar(0xff1a)))
		{
			rest = rest.mid(1).trimmed();
		}
		if (!rest.isEmpty()) { reportedSamples.insert(rest); }
	}

	// Runtime checks over both track containers.
	QHash<QString, QString> controlNodes =
		runtimeControlNodeNames(song->tracks());
	const QHash<QString, QString> patternControlNodes =
		runtimeControlNodeNames(patternStore->tracks());
	for (auto it = patternControlNodes.constBegin();
		it != patternControlNodes.constEnd(); ++it)
	{
		if (!controlNodes.contains(it.key())) { controlNodes.insert(it.key(), it.value()); }
	}

	for (const TrackContainer::TrackList* tracks :
		{&song->tracks(), &patternStore->tracks()})
	{
		for (Track* track : *tracks)
		{
			scanPlugins(track, issues, seen);
			scanEmptyTrack(track, issues, seen);
			scanAutomation(track, issues, seen);
		}
	}

	scanProjectXml(project, controlNodes, reportedSamples, issues, seen);

	return emitResult(project, issues, jsonOutput);
}

} // namespace headless
} // namespace lmms
