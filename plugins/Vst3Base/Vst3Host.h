/*
 * Vst3Host.h - minimal VST3 hosting support for LMMS
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

#ifndef LMMS_VST3_HOST_H
#define LMMS_VST3_HOST_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

#include "vst3base_export.h"

namespace lmms
{

//! A MIDI event which is handed to a VST3 plug-in for the next process call.
struct Vst3MidiEvent
{
	enum class Type
	{
		NoteOn,
		NoteOff,
		ControlChange,
		PitchBend,
		ChannelPressure,
		AllNotesOff,
	};

	Type type = Type::NoteOn;
	//! Sample offset inside the next processed block.
	int sampleOffset = 0;
	//! MIDI channel, 0 based.
	int channel = 0;
	int note = 60;
	//! Note velocity (0..1) or controller value (0..1).
	float velocity = 0.8f;
	int controller = 0;
	//! Normalized pitch bend (0..1, 0.5 is the center) or pressure (0..1).
	float value = 0.5f;
};


struct Vst3PluginInfo
{
	//! Path of the loadable module (e.g. Contents/x86_64-linux/Dexed.so).
	QString path;
	//! Path of the bundle as found on disk, used for project files.
	QString bundlePath;
	//! 32 hex characters representing the class UID.
	QString uid;
	QString name;
	QString vendor;
	QString category;
	QString subCategories;
	bool isInstrument = false;
	bool isEffect = false;
};


struct Vst3ParameterInfo
{
	int id = -1;
	QString name;
	QString unit;
	float defaultNormalized = 0.f;
	//! Number of discrete steps, 0 for continuous parameters.
	int stepCount = 0;
	bool isBypass = false;
};


//! Hosts a single VST3 plug-in instance without any GUI.
//!
//! The class is deliberately self contained: it loads the module, creates
//! component and controller, negotiates stereo audio buses, converts MIDI
//! events, processes audio offline and provides state/parameter access.
class VST3BASE_EXPORT Vst3Host
{
public:
	Vst3Host();
	~Vst3Host();

	Vst3Host(const Vst3Host&) = delete;
	Vst3Host& operator=(const Vst3Host&) = delete;

	//! Loads and initializes the plug-in class \a classUid from \a modulePath.
	//! \a modulePath may point to a .vst3 bundle or directly to the shared
	//! library inside the bundle.
	bool open(const QString& modulePath, const QString& classUid, QString* error = nullptr);
	bool isValid() const;

	QString errorString() const;
	QString pluginName() const;
	QString pluginVendor() const;

	void setSampleRate(double sampleRate);
	double sampleRate() const;

	//! Prepares the plug-in for processing blocks of at most \a maxBlockSize.
	bool prepare(int maxBlockSize);
	bool isPrepared() const;

	//! Resets the plug-in (stops and restarts processing).
	void reset();

	//! Processes \a frames in place. \a left and \a right are planar buffers.
	//! When \a hasAudioInput is true the current buffer contents are fed to the
	//! first audio input bus, otherwise the plug-in runs as a pure instrument.
	bool process(float* left, float* right, int frames,
		const Vst3MidiEvent* events = nullptr, int eventCount = 0,
		bool hasAudioInput = false);

	//! Tempo and transport information for the process context.
	void setTempo(double bpm);
	void setTransportPosition(qint64 projectTimeSamples, bool playing);

	int parameterCount() const;
	bool parameterInfo(int index, Vst3ParameterInfo* info) const;
	float parameterNormalized(int id) const;
	void setParameterNormalized(int id, float value);

	//! Saves component and controller state into one portable blob.
	QByteArray getState() const;
	bool setState(const QByteArray& state);

	//! Resolves a bundle (or a direct module path) to the shared library to load.
	static QString resolveBundleModule(const QString& path);
	//! Lists all audio plug-in classes inside one module/bundle.
	static QList<Vst3PluginInfo> scanModule(const QString& modulePath,
		QStringList* errors = nullptr);
	//! Scans all bundles in the given directories (non recursive).
	static QList<Vst3PluginInfo> scan(const QStringList& directories,
		QStringList* errors = nullptr);
	//! Standard VST3 plug-in locations, including the LMMS_VST3_PATH override.
	static QStringList defaultSearchPaths();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace lmms

#endif // LMMS_VST3_HOST_H
