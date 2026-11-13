/*
 * RenderManager - exporting logic common between the CLI and GUI.
 *
 * Copyright (c) 2015 Ryan Roden-Corrent <ryan/at/rcorre.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <vector>

#include "RenderManager.h"

#include "PatternStore.h"
#include "Song.h"
#include "Track.h"


namespace lmms
{

RenderManager::RenderManager(
	const OutputSettings& outputSettings, ProjectRenderer::ExportFileFormat fmt, QString outputPath,
	const RenderOptions& options)
	: m_outputSettings(outputSettings)
	, m_format(fmt)
	, m_outputPath(outputPath)
	, m_options(options)
{
	Engine::audioEngine()->storeAudioDevice();
}

RenderManager::~RenderManager()
{
	Engine::audioEngine()->restoreAudioDevice();  // Also deletes audio dev.
}

void RenderManager::abortProcessing()
{
	if ( m_activeRenderer ) {
		disconnect( m_activeRenderer.get(), SIGNAL(finished()),
				this, SLOT(renderNextTrack()));
		m_activeRenderer->abortProcessing();
	}
	restoreMutedState();
}

void RenderManager::accumulateStats(const RenderStats& stats)
{
	if (!stats.valid) { return; }

	if (!m_renderingTracks)
	{
		const int trackCount = m_stats.tracks;
		m_stats = stats;
		m_stats.tracks = trackCount;
		m_stats.warnings = m_options.warnings;
		for (const auto& w : stats.warnings)
		{
			if (!m_stats.warnings.contains(w)) { m_stats.warnings << w; }
		}
		return;
	}

	// Aggregate per-track output.
	m_stats.valid = true;
	++m_stats.tracks;
	for (const auto& w : m_options.warnings)
	{
		if (!m_stats.warnings.contains(w)) { m_stats.warnings << w; }
	}
	m_stats.duration = std::max(m_stats.duration, stats.duration);
	m_stats.peak = std::max(m_stats.peak, stats.peak);
	m_stats.rms = std::max(m_stats.rms, stats.rms);
	m_stats.silent = m_stats.silent && stats.silent;
	m_stats.file = m_outputPath;
	m_stats.format = stats.format;
	m_stats.sampleRate = stats.sampleRate;
	for (const auto& w : stats.warnings)
	{
		if (!m_stats.warnings.contains(w)) { m_stats.warnings << w; }
	}
}


// Called to render each new track when rendering tracks individually.
void RenderManager::renderNextTrack()
{
	if (m_activeRenderer)
	{
		const RenderStats stats = m_activeRenderer->stats();
		if (m_renderingTracks && m_currentTrack != nullptr
			&& !m_options.manifestPath.isEmpty())
		{
			recordManifestTrack(m_currentTrack, m_currentTrackIndex, stats);
		}
		accumulateStats(stats);
	}
	m_activeRenderer.reset();
	m_currentTrack = nullptr;

	if (m_tracksToRender.empty())
	{
		// nothing left to render
		restoreMutedState();
		if (m_renderingTracks && !m_options.manifestPath.isEmpty())
		{
			writeManifest();
		}
		emit finished();
	}
	else
	{
		// pop the next track from our rendering queue
		Track* renderTrack = m_tracksToRender.back();
		m_tracksToRender.pop_back();

		// mute everything but the track we are about to render
		for (auto track : m_unmuted)
		{
			track->setMuted(track != renderTrack);
		}

		// for multi-render, prefix each output file with a different number
		int trackNum = m_tracksToRender.size() + 1;
		const QString outputPath = pathForTrack(renderTrack, trackNum);

		m_currentTrack = renderTrack;
		m_currentTrackIndex = trackNum;
		m_currentTrackFile = outputPath;

		render( outputPath );
	}
}

// Record the statistics of a finished stem for the rendertracks manifest.
void RenderManager::recordManifestTrack(const Track* track, int index, const RenderStats& stats)
{
	if (track == nullptr) { return; }

	QJsonObject entry;
	entry["index"] = index;
	entry["name"] = track->name();
	entry["type"] = track->type() == Track::Type::Sample ? "sample" : "instrument";
	entry["file"] = stats.file.isEmpty() ? m_currentTrackFile : stats.file;
	entry["peak"] = stats.peak;
	entry["rms"] = stats.rms;
	entry["silent"] = stats.silent;
	entry["duration"] = stats.duration;
	m_manifestTracks.append(entry);
}

// Write the rendertracks manifest after every stem has been rendered.
void RenderManager::writeManifest()
{
	auto reportFailure = [this](const QString& message) {
		fprintf(stderr, "%s\n", message.toUtf8().constData());
		if (!m_stats.warnings.contains(message)) { m_stats.warnings << message; }
	};

	std::vector<QJsonObject> entries;
	entries.reserve(static_cast<std::size_t>(m_manifestTracks.size()));
	for (const QJsonValue value : m_manifestTracks)
	{
		entries.push_back(value.toObject());
	}
	std::sort(entries.begin(), entries.end(),
		[](const QJsonObject& a, const QJsonObject& b) {
			return a.value("index").toInt() < b.value("index").toInt();
		});

	QJsonArray tracks;
	for (const auto& entry : entries) { tracks.append(entry); }

	QJsonObject root;
	root["ok"] = true;
	root["project"] = Engine::getSong()->projectFileName();
	root["tracks"] = tracks;

	QFile file(m_options.manifestPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		reportFailure(QString("could not write manifest: %1").arg(m_options.manifestPath));
		return;
	}

	const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
	if (file.write(data) != data.size() || !file.flush())
	{
		reportFailure(QString("could not write manifest: %1").arg(m_options.manifestPath));
		return;
	}
}

// Render the song into individual tracks
void RenderManager::renderTracks()
{
	m_renderingTracks = true;
	m_manifestTracks = QJsonArray();
	m_currentTrack = nullptr;
	m_currentTrackIndex = 0;
	m_currentTrackFile.clear();

	const TrackContainer::TrackList& tl = Engine::getSong()->tracks();

	// find all currently unnmuted tracks -- we want to render these.
	for (const auto& tk : tl)
	{
		Track::Type type = tk->type();

		// Don't render automation tracks
		if ( tk->isMuted() == false &&
				( type == Track::Type::Instrument || type == Track::Type::Sample ) )
		{
			m_unmuted.push_back(tk);
		}
	}

	const TrackContainer::TrackList& t2 = Engine::patternStore()->tracks();
	for (const auto& tk : t2)
	{
		Track::Type type = tk->type();

		// Don't render automation tracks
		if ( tk->isMuted() == false &&
				( type == Track::Type::Instrument || type == Track::Type::Sample ) )
		{
			m_unmuted.push_back(tk);
		}
	}

	// copy the list of unmuted tracks into our rendering queue.
	// we need to remember which tracks were unmuted to restore state at the end.
	m_tracksToRender = m_unmuted;

	renderNextTrack();
}

// Render the song into a single track
void RenderManager::renderProject()
{
	m_renderingTracks = false;

	// Count the tracks that participate in the mix for the JSON report.
	int tracks = 0;
	for (const auto& tk : Engine::getSong()->tracks())
	{
		const auto type = tk->type();
		if (type == Track::Type::Instrument || type == Track::Type::Sample ||
			type == Track::Type::Pattern)
		{
			++tracks;
		}
	}
	m_stats.tracks = tracks;

	render( m_outputPath );
}

void RenderManager::render(QString outputPath)
{
	RenderPostProcessOptions postProcess;
	// The manifest needs per-stem statistics, which are only computed when the
	// render is captured in memory.
	postProcess.capture = m_options.json || !m_options.manifestPath.isEmpty()
		|| m_options.tailSeconds > 0.0 || m_options.normalize;
	postProcess.tailSeconds = m_options.tailSeconds;
	postProcess.normalize = m_options.normalize;
	postProcess.normalizeDb = m_options.normalizeDb;
	postProcess.noMetadata = m_options.noMetadata;

	m_activeRenderer = std::make_unique<ProjectRenderer>(
		m_outputSettings, m_format, outputPath, postProcess);

	if( m_activeRenderer->isReady() )
	{
		// pass progress signals through
		connect( m_activeRenderer.get(), SIGNAL(progressChanged(int)),
				this, SIGNAL(progressChanged(int)));

		// when it is finished, render the next track.
		// if we have not queued any tracks, renderNextTrack will just clean up
		connect( m_activeRenderer.get(), SIGNAL(finished()),
				this, SLOT(renderNextTrack()));

		m_activeRenderer->startProcessing();
	}
	else
	{
		qDebug( "Renderer failed to acquire a file device!" );
		m_stats.warnings << QString("could not open output file: %1").arg(outputPath);
		renderNextTrack();
	}
}

// Unmute all tracks that were muted while rendering tracks
void RenderManager::restoreMutedState()
{
	while (!m_unmuted.empty())
	{
		Track* restoreTrack = m_unmuted.back();
		m_unmuted.pop_back();
		restoreTrack->setMuted( false );
	}
}

// Determine the output path for a track when rendering tracks individually
QString RenderManager::pathForTrack(const Track *track, int num)
{
	QString extension = ProjectRenderer::getFileExtensionFromFormat( m_format );
	QString name = track->name();
	name = name.remove(QRegularExpression(FILENAME_FILTER));
	name = QString( "%1_%2%3" ).arg( num ).arg( name ).arg( extension );
	return QDir(m_outputPath).filePath(name);
}

void RenderManager::updateConsoleProgress()
{
	if ( m_activeRenderer )
	{
		m_activeRenderer->updateConsoleProgress();

		int totalNum = m_unmuted.size();
		if ( totalNum > 0 )
		{
			// we are rendering multiple tracks, append a track counter to the output
			int trackNum = totalNum - m_tracksToRender.size();
			fprintf( stderr, "(%d/%d)", trackNum, totalNum );
		}
	}
}


} // namespace lmms