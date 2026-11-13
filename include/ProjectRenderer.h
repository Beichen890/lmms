/*
 * ProjectRenderer.h - ProjectRenderer class for easily rendering projects
 *
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_PROJECT_RENDERER_H
#define LMMS_PROJECT_RENDERER_H

#include <vector>

#include <QStringList>

#include "AudioFileDevice.h"
#include "AudioEngine.h"
#include "OutputSettings.h"

#include "lmms_export.h"

namespace lmms
{

//! Optional post-processing / statistics collection applied to a render.
//! When @a capture is true the rendered audio is kept in memory, analysed,
//! optionally padded with silence and normalized, and then encoded into the
//! requested target format.
struct RenderPostProcessOptions
{
	bool capture = false;
	double tailSeconds = 0.0;
	bool normalize = false;
	double normalizeDb = 0.0;
	//! Strip non-audio metadata chunks (PEAK, LIST, ...) from WAV output so
	//! that repeated renders of the same project are byte-identical.
	bool noMetadata = false;
};

//! Statistics computed from the samples that were written to the output file.
struct RenderStats
{
	bool valid = false;
	QString file;
	QString format;
	int sampleRate = 0;
	double duration = 0.0;
	double peak = 0.0;
	double rms = 0.0;
	bool silent = true;
	int tracks = 0;
	QStringList warnings;
};


class LMMS_EXPORT ProjectRenderer : public QThread
{
	Q_OBJECT
public:
	enum class ExportFileFormat : int
	{
		Wave,
		Flac,
		Ogg,
		MP3,
		Count
	} ;
	constexpr static auto NumFileFormats = static_cast<std::size_t>(ExportFileFormat::Count);

	struct FileEncodeDevice
	{
		bool isAvailable() const { return m_getDevInst != nullptr; }

		ExportFileFormat m_fileFormat;
		const char * m_description;
		const char * m_extension;
		AudioFileDeviceInstantiaton m_getDevInst;
	} ;

	ProjectRenderer(const OutputSettings& _os, ExportFileFormat _file_format, const QString& _out_file,
		const RenderPostProcessOptions& _postProcess = RenderPostProcessOptions());
	~ProjectRenderer() override = default;

	bool isReady() const
	{
		return m_postProcess.capture || m_fileDev != nullptr;
	}

	const RenderStats& stats() const
	{
		return m_stats;
	}

	static ExportFileFormat getFileFormatFromExtension(
							const QString & _ext );

	static QString getFileExtensionFromFormat( ExportFileFormat fmt );

	//! Whether the given extension (without dot, case-insensitive) is
	//! available as an export format in this build.
	static bool isSupportedExtension( const QString & ext );

	static const std::array<FileEncodeDevice, 5> fileEncodeDevices;

public slots:
	void startProcessing();
	void abortProcessing();

	void updateConsoleProgress();


signals:
	void progressChanged( int );


private:
	void run() override;

	void finalizeCapturedRender();

	AudioFileDevice * m_fileDev;

	RenderPostProcessOptions m_postProcess;
	OutputSettings m_finalSettings;
	ExportFileFormat m_finalFormat;
	QString m_finalOutput;

	//! Interleaved float samples of the rendered range (capture mode only).
	std::vector<float> m_capturedSamples;

	RenderStats m_stats;

	volatile int m_progress;
	volatile bool m_abort;

} ;


} // namespace lmms

#endif // LMMS_PROJECT_RENDERER_H
