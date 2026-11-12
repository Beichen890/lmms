/*
 * ProjectRenderer.cpp - ProjectRenderer-class for easily rendering projects
 *
 * Copyright (c) 2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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


#include <QFile>

#include <algorithm>
#include <cmath>
#include <vector>

#include <sndfile.h>

#include "ProjectRenderer.h"
#include "Song.h"
#include "PerfLog.h"

#include "AudioFileWave.h"
#include "AudioFileOgg.h"
#include "AudioFileMP3.h"
#include "AudioFileFlac.h"


namespace lmms
{


namespace
{

//! Silent audio device used while capturing a render in memory. It carries the
//! requested sample rate so that the engine renders at the correct rate, but
//! does not open any hardware or file.
class CaptureAudioDevice : public AudioDevice
{
public:
	CaptureAudioDevice(const OutputSettings& settings, AudioEngine* engine) :
		AudioDevice(DEFAULT_CHANNELS, engine)
	{
		setSampleRate(settings.getSampleRate());
	}

private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};


QString formatName(ProjectRenderer::ExportFileFormat fmt)
{
	switch (fmt)
	{
	case ProjectRenderer::ExportFileFormat::Wave: return "wav";
	case ProjectRenderer::ExportFileFormat::Flac: return "flac";
	case ProjectRenderer::ExportFileFormat::Ogg: return "ogg";
	case ProjectRenderer::ExportFileFormat::MP3: return "mp3";
	default: return QString();
	}
}

} // namespace


const std::array<ProjectRenderer::FileEncodeDevice, 5> ProjectRenderer::fileEncodeDevices
{

	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Wave,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "WAV (*.wav)" ),
					".wav", &AudioFileWave::getInst },
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Flac,
		QT_TRANSLATE_NOOP("ProjectRenderer", "FLAC (*.flac)"),
		".flac",
		&AudioFileFlac::getInst
	},
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Ogg,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "OGG (*.ogg)" ),
					".ogg",
#ifdef LMMS_HAVE_OGGVORBIS
					&AudioFileOgg::getInst
#else
					nullptr
#endif
									},
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::MP3,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "MP3 (*.mp3)" ),
					".mp3",
#ifdef LMMS_HAVE_MP3LAME
					&AudioFileMP3::getInst
#else
					nullptr
#endif
									},
	// Insert your own file-encoder infos here.
	// Maybe one day the user can add own encoders inside the program.

	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Count, nullptr, nullptr, nullptr }

} ;

ProjectRenderer::ProjectRenderer(
	const OutputSettings& outputSettings, ExportFileFormat exportFileFormat, const QString& outputFilename,
	const RenderPostProcessOptions& postProcess)
	: QThread(Engine::audioEngine())
	, m_fileDev(nullptr)
	, m_postProcess(postProcess)
	, m_finalSettings(outputSettings)
	, m_finalFormat(exportFileFormat)
	, m_finalOutput(outputFilename)
	, m_progress(0)
	, m_abort(false)
{
	if (m_postProcess.capture)
	{
		// Capture in memory; the actual encoding is done in finalizeCapturedRender().
		return;
	}

	AudioFileDeviceInstantiaton audioEncoderFactory = fileEncodeDevices[static_cast<std::size_t>(exportFileFormat)].m_getDevInst;

	if (audioEncoderFactory)
	{
		bool successful = false;

		m_fileDev = audioEncoderFactory(
					outputFilename, outputSettings, DEFAULT_CHANNELS,
					Engine::audioEngine(), successful );
		if( !successful )
		{
			delete m_fileDev;
			m_fileDev = nullptr;
		}
	}
}




// Little help function for getting file format from a file extension
// (only for registered file-encoders).
ProjectRenderer::ExportFileFormat ProjectRenderer::getFileFormatFromExtension(
							const QString & _ext )
{
	int idx = 0;
	while( fileEncodeDevices[idx].m_fileFormat != ExportFileFormat::Count )
	{
		if( QString( fileEncodeDevices[idx].m_extension ) == _ext )
		{
			return( fileEncodeDevices[idx].m_fileFormat );
		}
		++idx;
	}

	return( ExportFileFormat::Wave ); // Default.
}




QString ProjectRenderer::getFileExtensionFromFormat(
		ExportFileFormat fmt )
{
	return fileEncodeDevices[static_cast<std::size_t>(fmt)].m_extension;
}




bool ProjectRenderer::isSupportedExtension( const QString & ext )
{
	for( const auto & device : fileEncodeDevices )
	{
		if( device.m_fileFormat == ExportFileFormat::Count )
		{
			break;
		}
		QString deviceExtension = QString( device.m_extension );
		if( deviceExtension.startsWith( '.' ) )
		{
			deviceExtension.remove( 0, 1 );
		}
		if( device.m_getDevInst != nullptr
			&& deviceExtension.compare( ext, Qt::CaseInsensitive ) == 0 )
		{
			return true;
		}
	}
	return false;
}




void ProjectRenderer::startProcessing()
{

	if( isReady() )
	{
		if (m_postProcess.capture)
		{
			// Use a silent device at the requested sample rate, but keep the
			// rendered audio in memory.
			Engine::audioEngine()->setAudioDevice(
				new CaptureAudioDevice(m_finalSettings, Engine::audioEngine()), false);
		}
		else
		{
			// Have to do audio engine stuff with GUI-thread affinity in order to
			// make slots connected to sampleRateChanged()-signals being called immediately.
			Engine::audioEngine()->setAudioDevice(m_fileDev, false);
		}

		start(
#ifndef LMMS_BUILD_WIN32
			QThread::HighPriority
#endif
						);

	}
}


void ProjectRenderer::run()
{
	PerfLogTimer perfLog("Project Render");

	Engine::getSong()->startExport();
	// Skip first empty buffer, but do not discard the first period of an
	// explicit export range: the engine starts at the requested tick and
	// dropping it would shift the rendered range.
	if (!Engine::getSong()->hasExportRange())
	{
		Engine::audioEngine()->renderNextPeriod();
	}

	m_progress = 0;

	// Now start processing
	Engine::audioEngine()->startProcessing();

	// Continually track and emit progress percentage to listeners.
	while (!Engine::getSong()->isExportDone() && !m_abort)
	{
		const auto buffer = Engine::audioEngine()->renderNextPeriod();

		if (m_postProcess.capture)
		{
			for (const auto& frame : buffer)
			{
				for (ch_cnt_t ch = 0; ch < DEFAULT_CHANNELS; ++ch)
				{
					m_capturedSamples.push_back(frame[ch]);
				}
			}
		}
		else
		{
			m_fileDev->writeBuffer(buffer.data(), buffer.size());
		}

		const int nprog = Engine::getSong()->getExportProgress();
		if (m_progress != nprog)
		{
			m_progress = nprog;
			emit progressChanged( m_progress );
		}
	}

	// Notify the audio engine of the end of processing.
	Engine::audioEngine()->stopProcessing();

	Engine::getSong()->stopExport();

	perfLog.end();

	if (m_postProcess.capture)
	{
		finalizeCapturedRender();
		return;
	}

	// If the user aborted export-process, the file has to be deleted.
	const QString f = m_fileDev->outputFile();
	if( m_abort )
	{
		QFile( f ).remove();
	}
}


void ProjectRenderer::finalizeCapturedRender()
{
	m_stats.file = m_finalOutput;
	m_stats.format = formatName(m_finalFormat);
	m_stats.sampleRate = m_finalSettings.getSampleRate();

	if (m_abort)
	{
		QFile::remove(m_finalOutput);
		m_stats.warnings << "render aborted";
		return;
	}

	const auto channels = static_cast<std::size_t>(DEFAULT_CHANNELS);

	// Append silence for the requested tail length.
	if (m_postProcess.tailSeconds > 0.0)
	{
		const auto tailFrames = static_cast<std::size_t>(
			std::llround(m_postProcess.tailSeconds * m_finalSettings.getSampleRate()));
		m_capturedSamples.insert(m_capturedSamples.end(), tailFrames * channels, 0.0f);
	}

	// Normalize before encoding so that 16 bit output does not clip.
	if (m_postProcess.normalize)
	{
		float peak = 0.0f;
		for (const float s : m_capturedSamples) { peak = std::max(peak, std::abs(s)); }

		if (peak > 0.0f)
		{
			const double target = std::pow(10.0, m_postProcess.normalizeDb / 20.0);
			const auto gain = static_cast<float>(target / peak);
			for (float& s : m_capturedSamples) { s *= gain; }
		}
		else
		{
			m_stats.warnings << "normalize skipped: rendered audio is silent";
		}
	}

	// Encode the final file from the (possibly modified) float samples.
	bool encoded = false;
	const auto factory = fileEncodeDevices[static_cast<std::size_t>(m_finalFormat)].m_getDevInst;
	if (factory)
	{
		bool success = false;
		AudioFileDevice* dev = factory(
			m_finalOutput, m_finalSettings, DEFAULT_CHANNELS, Engine::audioEngine(), success);
		if (success && dev)
		{
			const auto totalFrames = m_capturedSamples.size() / channels;
			const f_cnt_t chunkFrames = 1024;
			std::vector<SampleFrame> chunk(chunkFrames);

			std::size_t frame = 0;
			while (frame < totalFrames)
			{
				const auto n = static_cast<f_cnt_t>(
					std::min<std::size_t>(chunkFrames, totalFrames - frame));
				for (f_cnt_t i = 0; i < n; ++i)
				{
					for (std::size_t ch = 0; ch < channels; ++ch)
					{
						chunk[i][ch] = m_capturedSamples[(frame + i) * channels + ch];
					}
				}
				dev->writeBuffer(chunk.data(), n);
				frame += n;
			}

			delete dev;
			encoded = true;
		}
		else
		{
			delete dev;
			m_stats.warnings << "could not create output encoder for "
				<< formatName(m_finalFormat);
		}
	}
	else
	{
		m_stats.warnings << "output format is not available in this build: "
			<< formatName(m_finalFormat);
	}

	// Statistics should reflect the audio that was actually written. Read the
	// encoded file back when libsndfile supports the format; otherwise fall
	// back to the float samples we encoded.
	std::vector<float> statsSamples;
	int statsChannels = DEFAULT_CHANNELS;
	int statsSampleRate = m_stats.sampleRate;

	if (encoded)
	{
		QFile encodedFile(m_finalOutput);
		if (encodedFile.open(QIODevice::ReadOnly))
		{
			SF_INFO info{};
			SNDFILE* sf = sf_open_fd(encodedFile.handle(), SFM_READ, &info, false);
			if (sf != nullptr)
			{
				const auto frames = static_cast<sf_count_t>(info.frames);
				if (frames > 0 && info.channels > 0)
				{
					statsSamples.resize(static_cast<std::size_t>(frames) *
						static_cast<std::size_t>(info.channels));
					sf_readf_float(sf, statsSamples.data(), frames);
					statsChannels = info.channels;
					statsSampleRate = info.samplerate;
				}
				sf_close(sf);
			}
		}
	}

	if (statsSamples.empty())
	{
		statsSamples = m_capturedSamples;
	}

	// Statistics are computed over all channels of all frames.
	double sumSquares = 0.0;
	double peak = 0.0;
	for (const float s : statsSamples)
	{
		const double v = s;
		sumSquares += v * v;
		peak = std::max(peak, std::abs(v));
	}

	const auto frames = statsSamples.size() / static_cast<std::size_t>(statsChannels);
	m_stats.duration = statsSampleRate > 0
		? static_cast<double>(frames) / statsSampleRate
		: 0.0;
	m_stats.sampleRate = statsSampleRate;
	m_stats.peak = peak;
	m_stats.rms = statsSamples.empty()
		? 0.0
		: std::sqrt(sumSquares / static_cast<double>(statsSamples.size()));
	m_stats.silent = peak < 1e-6;
	m_stats.valid = encoded;
}




void ProjectRenderer::abortProcessing()
{
	m_abort = true;
	wait();
}



void ProjectRenderer::updateConsoleProgress()
{
	constexpr int cols = 50;
	static int rot = 0;
	auto buf = std::array<char, 80>{};
	auto prog = std::array<char, cols + 1>{};

	for( int i = 0; i < cols; ++i )
	{
		prog[i] = ( i*100/cols <= m_progress ? '-' : ' ' );
	}
	prog[cols] = 0;

	const auto activity = "|/-\\";
	std::fill(buf.begin(), buf.end(), 0);
	std::snprintf(buf.data(), buf.size(), "\r|%s|    %3d%%   %c  ", prog.data(), m_progress,
							activity[rot] );
	rot = ( rot+1 ) % 4;

	fprintf( stderr, "%s", buf.data() );
	fflush( stderr );
}


} // namespace lmms
