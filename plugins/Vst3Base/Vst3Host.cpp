/*
 * Vst3Host.cpp - minimal VST3 hosting support for LMMS
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

#include "Vst3Host.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

namespace lmms
{

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

QString uidToHex(const void* uid)
{
	return QString::fromLatin1(
		QByteArray(static_cast<const char*>(uid), 16).toHex());
}


bool uidFromHex(const QString& text, TUID uid)
{
	const QByteArray bytes = QByteArray::fromHex(text.trimmed().toLatin1());
	if (bytes.size() != 16) { return false; }
	std::memcpy(uid, bytes.constData(), 16);
	return true;
}


QString fromTChar(const TChar* text)
{
	return QString::fromStdString(StringConvert::convert(text, 128));
}


bool bundleContainsModule(const QFileInfo& info)
{
	return info.isFile();
}


//! Returns the path which has to be handed to VST3::Hosting::Module::create.
//! Linux requires the bundle directory, Windows accepts both the bundle
//! directory and a plain DLL.
QString moduleLoadPath(const QString& path)
{
	const QFileInfo info(path);
	if (info.isDir()) { return info.absoluteFilePath(); }

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
	// Accept a direct path to Contents/<arch>-linux/<name>.so by walking
	// back to the bundle directory.
	QDir directory = info.absoluteDir();
	if (directory.dirName().endsWith(QStringLiteral("-linux")))
	{
		if (directory.cdUp() && directory.dirName() == QStringLiteral("Contents"))
		{
			directory.cdUp();
			return directory.absolutePath();
		}
	}
#endif

	return info.absoluteFilePath();
}

} // namespace


struct Vst3Host::Impl
{
	struct Bus
	{
		BusDirection direction = kInput;
		int index = 0;
		int channelCount = 0;
		std::vector<std::vector<float>> channels;
		std::vector<float*> pointers;
		AudioBusBuffers description{};
	};

	VST3::Hosting::Module::Ptr module;
	std::unique_ptr<PlugProvider> provider;
	IPtr<IComponent> component;
	IPtr<IAudioProcessor> processor;
	IPtr<IEditController> controller;
	IPtr<IMidiMapping> midiMapping;
	IPtr<IHostApplication> hostApplication;

	bool controllerIsComponent = false;
	bool prepared = false;
	bool processing = false;

	QString error;
	QString pluginName;
	QString pluginVendor;

	double sampleRate = 44100.0;
	double tempo = 120.0;
	int maxBlockSize = 0;
	qint64 projectTimeSamples = 0;
	bool playing = true;

	std::vector<Bus> inputBuses;
	std::vector<Bus> outputBuses;
	int outputChannels = 0;

	EventList eventList{256};
	ParameterChanges inputParameters{64};
	ParameterChanges outputParameters{64};
	ProcessData processData{};
	ProcessContext processContext{};

	std::mutex pendingMutex;
	std::vector<std::pair<ParamID, float>> pendingParameters;

	void clearBuses()
	{
		inputBuses.clear();
		outputBuses.clear();
		outputChannels = 0;
	}

	bool createBuses()
	{
		clearBuses();

		auto setup = [this](BusDirection direction, std::vector<Bus>& buses) {
			const int32 count = component->getBusCount(kAudio, direction);
			for (int32 i = 0; i < count; ++i)
			{
				BusInfo busInfo{};
				if (component->getBusInfo(kAudio, direction, i, busInfo) != kResultOk)
				{
					continue;
				}

				Bus bus;
				bus.direction = direction;
				bus.index = i;
				bus.channelCount = busInfo.channelCount;
				bus.channels.resize(std::max(0, bus.channelCount));
				bus.pointers.resize(std::max(0, bus.channelCount));
				for (int channel = 0; channel < bus.channelCount; ++channel)
				{
					bus.channels[channel].assign(
						static_cast<size_t>(std::max(1, maxBlockSize)), 0.0f);
					bus.pointers[channel] = bus.channels[channel].data();
				}
				bus.description.numChannels = bus.channelCount;
				bus.description.channelBuffers32 =
					bus.pointers.empty() ? nullptr : bus.pointers.data();
				bus.description.silenceFlags = 0;

				// Only activate buses which are active by default. This avoids
				// enabling optional side chain inputs.
				if ((busInfo.flags & BusInfo::kDefaultActive) != 0)
				{
					component->activateBus(kAudio, direction, i, true);
				}
				buses.push_back(std::move(bus));
			}
		};

		setup(kInput, inputBuses);
		setup(kOutput, outputBuses);

		if (outputBuses.empty())
		{
			error = QStringLiteral("plug-in does not provide an audio output bus");
			return false;
		}

		outputChannels = outputBuses.front().channelCount;
		if (outputChannels <= 0)
		{
			error = QStringLiteral("plug-in output bus has no channels");
			return false;
		}

		return true;
	}

	void refreshProcessContext()
	{
		processContext = ProcessContext{};
		processContext.state = ProcessContext::kContTimeValid
			| ProcessContext::kTempoValid | ProcessContext::kTimeSigValid
			| ProcessContext::kProjectTimeMusicValid;
		if (playing) { processContext.state |= ProcessContext::kPlaying; }
		processContext.sampleRate = sampleRate;
		processContext.projectTimeSamples = projectTimeSamples;
		processContext.continousTimeSamples = projectTimeSamples;
		processContext.tempo = tempo;
		processContext.timeSigNumerator = 4;
		processContext.timeSigDenominator = 4;
		processContext.projectTimeMusic =
			sampleRate > 0.0 ? (static_cast<double>(projectTimeSamples) / sampleRate)
				* (tempo / 60.0) : 0.0;
	}

	bool ensurePrepared(int frames)
	{
		if (prepared && frames <= maxBlockSize) { return true; }
		return prepare(frames);
	}

	bool prepare(int frames)
	{
		if (!processor || !component) { return false; }

		if (prepared)
		{
			processor->setProcessing(false);
			component->setActive(false);
			prepared = false;
			processing = false;
		}

		if (frames < 1) { frames = 1; }
		maxBlockSize = frames;

		if (!createBuses()) { return false; }

		ProcessSetup setup{};
		setup.processMode = kRealtime;
		setup.symbolicSampleSize = kSample32;
		setup.maxSamplesPerBlock = maxBlockSize;
		setup.sampleRate = sampleRate;
		if (processor->setupProcessing(setup) != kResultOk)
		{
			error = QStringLiteral("setupProcessing failed");
			return false;
		}

		if (component->setActive(true) != kResultOk)
		{
			error = QStringLiteral("could not activate plug-in");
			return false;
		}
		if (processor->setProcessing(true) != kResultOk)
		{
			error = QStringLiteral("could not start plug-in processing");
			return false;
		}

		prepared = true;
		processing = true;
		projectTimeSamples = 0;
		return true;
	}

	void fillInputBuses(bool hasAudioInput, const float* left, const float* right,
		int frames)
	{
		for (size_t busIndex = 0; busIndex < inputBuses.size(); ++busIndex)
		{
			Bus& bus = inputBuses[busIndex];
			if (busIndex == 0 && hasAudioInput && bus.channelCount > 0)
			{
				std::memcpy(bus.channels[0].data(), left,
					sizeof(float) * frames);
				if (bus.channelCount > 1)
				{
					std::memcpy(bus.channels[1].data(), right,
						sizeof(float) * frames);
				}
				for (int channel = 2; channel < bus.channelCount; ++channel)
				{
					std::fill_n(bus.channels[channel].begin(), frames, 0.0f);
				}
				bus.description.silenceFlags = 0;
			}
			else
			{
				for (auto& channel : bus.channels)
				{
					std::fill_n(channel.begin(), frames, 0.0f);
				}
				bus.description.silenceFlags = 0xffffffffffffffffULL;
			}
		}
	}

	void clearOutputBuses(int frames)
	{
		for (auto& bus : outputBuses)
		{
			for (auto& channel : bus.channels)
			{
				std::fill_n(channel.begin(), frames, 0.0f);
			}
			bus.description.silenceFlags = 0;
		}
	}

	void buildEvents(const Vst3MidiEvent* events, int eventCount, int frames)
	{
		eventList.clear();
		if (events == nullptr || eventCount <= 0) { return; }

		for (int i = 0; i < eventCount; ++i)
		{
			const Vst3MidiEvent& input = events[i];
			const int offset = std::clamp(input.sampleOffset, 0,
				std::max(0, frames - 1));
			const int channel = input.channel & 0xF;

			if (input.type == Vst3MidiEvent::Type::ControlChange ||
				input.type == Vst3MidiEvent::Type::PitchBend ||
				input.type == Vst3MidiEvent::Type::ChannelPressure)
			{
				ParamID parameterId = kNoParamId;
				int controllerNumber = input.controller;
				if (input.type == Vst3MidiEvent::Type::PitchBend)
				{
					controllerNumber = ControllerNumbers::kPitchBend;
				}
				else if (input.type == Vst3MidiEvent::Type::ChannelPressure)
				{
					controllerNumber = ControllerNumbers::kAfterTouch;
				}
				if (midiMapping && midiMapping->getMidiControllerAssignment(
						0, channel, static_cast<int16>(controllerNumber),
						parameterId) == kResultTrue)
				{
					// MIDI controllers are applied as plain parameter changes
					// before processing; no event is needed here.
					const float value = std::clamp(input.value, 0.0f, 1.0f);
					controller->setParamNormalized(parameterId, value);
					addPendingParameter(parameterId, value);
				}
				continue;
			}

			if (input.type == Vst3MidiEvent::Type::AllNotesOff)
			{
				for (int note = 0; note < 128; ++note)
				{
					Event event{};
					event.type = Event::kNoteOffEvent;
					event.noteOff.channel = static_cast<int16>(channel);
					event.noteOff.pitch = static_cast<int16>(note);
					event.noteOff.velocity = 0.0f;
					event.noteOff.noteId = note;
					event.sampleOffset = offset;
					if (eventList.addEvent(event) != kResultOk) { break; }
				}
				continue;
			}

			Event event{};
			event.type = input.type == Vst3MidiEvent::Type::NoteOn
				? Event::kNoteOnEvent : Event::kNoteOffEvent;
			if (event.type == Event::kNoteOnEvent)
			{
				event.noteOn.channel = static_cast<int16>(channel);
				event.noteOn.pitch = static_cast<int16>(std::clamp(input.note, 0, 127));
				event.noteOn.velocity = std::clamp(input.velocity, 0.0f, 1.0f);
				event.noteOn.noteId = event.noteOn.pitch;
				event.noteOn.length = 0;
				event.noteOn.tuning = 0.0f;
			}
			else
			{
				event.noteOff.channel = static_cast<int16>(channel);
				event.noteOff.pitch = static_cast<int16>(std::clamp(input.note, 0, 127));
				event.noteOff.velocity = std::clamp(input.velocity, 0.0f, 1.0f);
				event.noteOff.noteId = event.noteOff.pitch;
			}
			event.sampleOffset = offset;
			if (eventList.addEvent(event) != kResultOk) { break; }
		}
	}

	void addPendingParameter(ParamID id, float value)
	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		pendingParameters.emplace_back(id, value);
	}

	void applyPendingParameters()
	{
		std::vector<std::pair<ParamID, float>> pending;
		{
			std::lock_guard<std::mutex> lock(pendingMutex);
			pending.swap(pendingParameters);
		}

		inputParameters.clearQueue();
		for (const auto& entry : pending)
		{
			int32 queueIndex = 0;
			IParamValueQueue* queue =
				inputParameters.addParameterData(entry.first, queueIndex);
			if (queue == nullptr) { continue; }
			int32 pointIndex = 0;
			queue->addPoint(0, entry.second, pointIndex);
		}
	}
};


Vst3Host::Vst3Host() : m_impl(std::make_unique<Impl>())
{
}


Vst3Host::~Vst3Host()
{
	if (!m_impl) { return; }

	if (m_impl->processor && m_impl->prepared)
	{
		m_impl->processor->setProcessing(false);
	}
	if (m_impl->component && m_impl->prepared)
	{
		m_impl->component->setActive(false);
	}

	m_impl->processor = nullptr;
	m_impl->midiMapping = nullptr;
	m_impl->controller = nullptr;
	m_impl->component = nullptr;
	m_impl->hostApplication = nullptr;
	// The provider terminates component and controller in its destructor.
	m_impl->provider.reset();
	m_impl->module.reset();
}


bool Vst3Host::open(const QString& modulePath, const QString& classUid,
	QString* error)
{
	if (!m_impl) { m_impl = std::make_unique<Impl>(); }

	const QString resolved = resolveBundleModule(modulePath);
	if (resolved.isEmpty())
	{
		m_impl->error = QStringLiteral("no VST3 module found at %1").arg(modulePath);
		if (error) { *error = m_impl->error; }
		return false;
	}

	std::string moduleError;
	VST3::Hosting::Module::Ptr module =
		VST3::Hosting::Module::create(moduleLoadPath(modulePath).toStdString(),
			moduleError);
	if (!module)
	{
		m_impl->error = QString::fromStdString(moduleError);
		if (m_impl->error.isEmpty())
		{
			m_impl->error = QStringLiteral("could not load %1").arg(resolved);
		}
		if (error) { *error = m_impl->error; }
		return false;
	}

	const auto factory = module->getFactory();
	const auto classInfos = factory.classInfos();

	VST3::Hosting::ClassInfo classInfo;
	bool found = false;
	TUID wantedUid{};
	const bool haveUid = uidFromHex(classUid, wantedUid);
	for (const auto& info : classInfos)
	{
		if (info.category() != kVstAudioEffectClass) { continue; }
		if (haveUid && std::memcmp(info.ID().data(), wantedUid, 16) != 0)
		{
			continue;
		}
		classInfo = info;
		found = true;
		break;
	}

	if (!found)
	{
		m_impl->error = haveUid
			? QStringLiteral("plug-in class %1 not found in %2")
				.arg(classUid, modulePath)
			: QStringLiteral("no audio plug-in class found in %1").arg(modulePath);
		if (error) { *error = m_impl->error; }
		return false;
	}

	m_impl->module = module;
	m_impl->pluginName = QString::fromStdString(classInfo.name());
	m_impl->pluginVendor = QString::fromStdString(classInfo.vendor());

	m_impl->hostApplication = owned(new HostApplication());
	PluginContextFactory::instance().setPluginContext(
		m_impl->hostApplication.get());

	m_impl->provider = std::make_unique<PlugProvider>(factory, classInfo);
	if (!m_impl->provider->initialize())
	{
		m_impl->error = QStringLiteral("could not initialize plug-in %1")
			.arg(m_impl->pluginName);
		if (error) { *error = m_impl->error; }
		m_impl->provider.reset();
		m_impl->module.reset();
		return false;
	}

	m_impl->component = m_impl->provider->getComponentPtr();
	m_impl->controller = m_impl->provider->getControllerPtr();
	m_impl->processor = U::cast<IAudioProcessor>(m_impl->component);
	if (!m_impl->component || !m_impl->processor)
	{
		m_impl->error = QStringLiteral("plug-in %1 does not implement IAudioProcessor")
			.arg(m_impl->pluginName);
		if (error) { *error = m_impl->error; }
		return false;
	}

	m_impl->controllerIsComponent = FUnknownPtr<IEditController>(
		m_impl->component).getInterface() != nullptr;
	if (!m_impl->controller)
	{
		m_impl->error = QStringLiteral("plug-in %1 has no edit controller")
			.arg(m_impl->pluginName);
		if (error) { *error = m_impl->error; }
		return false;
	}

	// Let the controller reflect the default component state.
	if (!m_impl->controllerIsComponent)
	{
		MemoryStream stream;
		if (m_impl->component->getState(&stream) == kResultOk)
		{
			stream.seek(0, IBStream::kIBSeekSet, nullptr);
			m_impl->controller->setComponentState(&stream);
		}
	}

	m_impl->midiMapping = U::cast<IMidiMapping>(m_impl->controller);
	m_impl->error.clear();
	if (error) { error->clear(); }
	return true;
}


bool Vst3Host::isValid() const
{
	return m_impl && m_impl->component && m_impl->processor
		&& m_impl->controller && m_impl->error.isEmpty();
}


QString Vst3Host::errorString() const
{
	return m_impl ? m_impl->error : QString();
}


QString Vst3Host::pluginName() const
{
	return m_impl ? m_impl->pluginName : QString();
}


QString Vst3Host::pluginVendor() const
{
	return m_impl ? m_impl->pluginVendor : QString();
}


void Vst3Host::setSampleRate(double sampleRate)
{
	if (!m_impl || sampleRate <= 0.0) { return; }
	if (m_impl->sampleRate == sampleRate) { return; }

	m_impl->sampleRate = sampleRate;
	if (m_impl->prepared)
	{
		m_impl->prepare(m_impl->maxBlockSize);
	}
}


double Vst3Host::sampleRate() const
{
	return m_impl ? m_impl->sampleRate : 0.0;
}


bool Vst3Host::prepare(int maxBlockSize)
{
	if (!isValid()) { return false; }
	return m_impl->prepare(maxBlockSize);
}


bool Vst3Host::isPrepared() const
{
	return m_impl && m_impl->prepared;
}


void Vst3Host::reset()
{
	if (!m_impl || !m_impl->prepared) { return; }
	m_impl->processor->setProcessing(false);
	m_impl->processor->setProcessing(true);
	m_impl->projectTimeSamples = 0;
}


bool Vst3Host::process(float* left, float* right, int frames,
	const Vst3MidiEvent* events, int eventCount, bool hasAudioInput)
{
	if (!isValid() || left == nullptr || right == nullptr || frames <= 0)
	{
		return false;
	}
	if (!m_impl->ensurePrepared(frames)) { return false; }

	const int blockFrames = std::min(frames, m_impl->maxBlockSize);

	m_impl->fillInputBuses(hasAudioInput, left, right, blockFrames);
	m_impl->clearOutputBuses(blockFrames);
	m_impl->buildEvents(events, eventCount, blockFrames);
	m_impl->applyPendingParameters();
	m_impl->outputParameters.clearQueue();
	m_impl->refreshProcessContext();

	m_impl->processData = ProcessData{};
	m_impl->processData.processMode = kRealtime;
	m_impl->processData.symbolicSampleSize = kSample32;
	m_impl->processData.numSamples = blockFrames;
	m_impl->processData.numInputs =
		static_cast<int32>(m_impl->inputBuses.size());
	m_impl->processData.numOutputs =
		static_cast<int32>(m_impl->outputBuses.size());
	// Rebuild contiguous bus description arrays to keep the process data
	// contract intact.
	std::vector<AudioBusBuffers> inputDescriptions;
	inputDescriptions.reserve(m_impl->inputBuses.size());
	for (auto& bus : m_impl->inputBuses)
	{
		inputDescriptions.push_back(bus.description);
	}
	std::vector<AudioBusBuffers> outputDescriptions;
	outputDescriptions.reserve(m_impl->outputBuses.size());
	for (auto& bus : m_impl->outputBuses)
	{
		outputDescriptions.push_back(bus.description);
	}
	m_impl->processData.inputs =
		inputDescriptions.empty() ? nullptr : inputDescriptions.data();
	m_impl->processData.outputs = outputDescriptions.data();
	m_impl->processData.inputParameterChanges = &m_impl->inputParameters;
	m_impl->processData.outputParameterChanges = &m_impl->outputParameters;
	m_impl->processData.inputEvents =
		m_impl->eventList.getEventCount() > 0 ? &m_impl->eventList : nullptr;
	m_impl->processData.outputEvents = nullptr;
	m_impl->processData.processContext = &m_impl->processContext;

	const tresult result = m_impl->processor->process(m_impl->processData);

	const Impl::Bus& mainOutput = m_impl->outputBuses.front();
	if (mainOutput.channelCount > 0)
	{
		std::memcpy(left, mainOutput.channels[0].data(),
			sizeof(float) * blockFrames);
		if (mainOutput.channelCount > 1)
		{
			std::memcpy(right, mainOutput.channels[1].data(),
				sizeof(float) * blockFrames);
		}
		else
		{
			std::memcpy(right, mainOutput.channels[0].data(),
				sizeof(float) * blockFrames);
		}
	}

	m_impl->outputParameters.clearQueue();
	m_impl->projectTimeSamples += blockFrames;
	return result == kResultOk;
}


void Vst3Host::setTempo(double bpm)
{
	if (m_impl && bpm > 0.0) { m_impl->tempo = bpm; }
}


void Vst3Host::setTransportPosition(qint64 projectTimeSamples, bool playing)
{
	if (!m_impl) { return; }
	m_impl->projectTimeSamples = std::max<qint64>(0, projectTimeSamples);
	m_impl->playing = playing;
}


int Vst3Host::parameterCount() const
{
	if (!isValid()) { return 0; }
	return std::max(0, m_impl->controller->getParameterCount());
}


bool Vst3Host::parameterInfo(int index, Vst3ParameterInfo* info) const
{
	if (!isValid() || info == nullptr || index < 0) { return false; }

	ParameterInfo parameter{};
	if (m_impl->controller->getParameterInfo(
			static_cast<int32>(index), parameter) != kResultOk)
	{
		return false;
	}

	info->id = parameter.id;
	info->name = fromTChar(parameter.title);
	info->unit = fromTChar(parameter.units);
	info->defaultNormalized = parameter.defaultNormalizedValue;
	info->stepCount = parameter.stepCount;
	info->isBypass =
		(parameter.flags & ParameterInfo::kIsBypass) != 0;
	return true;
}


float Vst3Host::parameterNormalized(int id) const
{
	if (!isValid()) { return 0.0f; }
	return m_impl->controller->getParamNormalized(static_cast<ParamID>(id));
}


void Vst3Host::setParameterNormalized(int id, float value)
{
	if (!isValid()) { return; }
	const ParamValue normalized = std::clamp(value, 0.0f, 1.0f);
	m_impl->controller->setParamNormalized(static_cast<ParamID>(id), normalized);
	m_impl->addPendingParameter(static_cast<ParamID>(id), normalized);
}


namespace
{

void appendBlock(QByteArray& target, const QByteArray& block)
{
	char header[4];
	qToBigEndian<quint32>(static_cast<quint32>(block.size()), header);
	target.append(header, 4);
	target.append(block);
}


QByteArray captureComponentState(const IPtr<IComponent>& component)
{
	if (!component) { return {}; }
	MemoryStream stream;
	if (component->getState(&stream) != kResultOk) { return {}; }
	return QByteArray(stream.getData(), static_cast<int>(stream.getSize()));
}


QByteArray captureControllerState(const IPtr<IEditController>& controller)
{
	if (!controller) { return {}; }
	MemoryStream stream;
	if (controller->getState(&stream) != kResultOk) { return {}; }
	return QByteArray(stream.getData(), static_cast<int>(stream.getSize()));
}

} // namespace


QByteArray Vst3Host::getState() const
{
	if (!isValid()) { return {}; }

	QByteArray result;
	result.append(char(1));
	appendBlock(result, captureComponentState(m_impl->component));
	appendBlock(result, m_impl->controllerIsComponent
		? QByteArray() : captureControllerState(m_impl->controller));
	return result;
}


bool Vst3Host::setState(const QByteArray& state)
{
	if (!isValid() || state.size() < 9 || state.at(0) != 1)
	{
		return false;
	}

	auto readBlock = [&state](int& offset) -> QByteArray {
		if (offset + 4 > state.size()) { return {}; }
		quint32 size = qFromBigEndian<quint32>(
			reinterpret_cast<const uchar*>(state.constData() + offset));
		offset += 4;
		if (size > static_cast<quint32>(state.size() - offset)) { return {}; }
		const QByteArray block = state.mid(offset, static_cast<int>(size));
		offset += static_cast<int>(size);
		return block;
	};

	int offset = 1;
	const QByteArray componentState = readBlock(offset);
	const QByteArray controllerState = readBlock(offset);

	if (!componentState.isEmpty())
	{
		MemoryStream stream(const_cast<char*>(componentState.constData()),
			static_cast<TSize>(componentState.size()));
		if (m_impl->component->setState(&stream) != kResultOk)
		{
			return false;
		}
		stream.seek(0, IBStream::kIBSeekSet, nullptr);
		if (!m_impl->controllerIsComponent)
		{
			m_impl->controller->setComponentState(&stream);
		}
	}

	if (!controllerState.isEmpty() && !m_impl->controllerIsComponent)
	{
		MemoryStream stream(const_cast<char*>(controllerState.constData()),
			static_cast<TSize>(controllerState.size()));
		if (m_impl->controller->setState(&stream) != kResultOk)
		{
			return false;
		}
	}

	return true;
}


QString Vst3Host::resolveBundleModule(const QString& path)
{
	const QFileInfo info(path);
	if (!info.exists()) { return {}; }
	if (info.isFile()) { return info.absoluteFilePath(); }

	const QString baseName = info.completeBaseName();
	const QString bundlePath = info.absoluteFilePath();

	QStringList relativeCandidates;
#if defined(Q_OS_WIN)
	relativeCandidates << QStringLiteral("Contents/x86_64-win/%1.vst3").arg(baseName)
		<< QStringLiteral("Contents/x86-win/%1.vst3").arg(baseName);
#elif defined(Q_OS_MACOS)
	relativeCandidates << QStringLiteral("Contents/MacOS/%1").arg(baseName);
#else
	relativeCandidates << QStringLiteral("Contents/x86_64-linux/%1.so").arg(baseName)
		<< QStringLiteral("Contents/aarch64-linux/%1.so").arg(baseName)
		<< QStringLiteral("Contents/i386-linux/%1.so").arg(baseName);
#endif

	for (const auto& candidate : relativeCandidates)
	{
		const QFileInfo file(bundlePath + QLatin1Char('/') + candidate);
		if (bundleContainsModule(file)) { return file.absoluteFilePath(); }
	}

	// Some bundles use non standard layouts; fall back to a shallow search.
	QStringList filters;
#if defined(Q_OS_WIN)
	filters << QStringLiteral("*.vst3");
#elif defined(Q_OS_MACOS)
	// The macOS binary has no extension; do not guess here.
#else
	filters << QStringLiteral("*.so");
#endif
	if (!filters.isEmpty())
	{
		QDirIterator iterator(bundlePath, filters, QDir::Files,
			QDirIterator::Subdirectories);
		if (iterator.hasNext())
		{
			return iterator.next();
		}
	}

	return {};
}


QList<Vst3PluginInfo> Vst3Host::scanModule(const QString& modulePath,
	QStringList* errors)
{
	QList<Vst3PluginInfo> result;

	const QString resolved = resolveBundleModule(modulePath);
	if (resolved.isEmpty())
	{
		if (errors)
		{
			errors->append(QStringLiteral("no VST3 module found at %1")
				.arg(modulePath));
		}
		return result;
	}

	std::string moduleError;
	VST3::Hosting::Module::Ptr module =
		VST3::Hosting::Module::create(moduleLoadPath(modulePath).toStdString(),
			moduleError);
	if (!module)
	{
		if (errors)
		{
			errors->append(QStringLiteral("failed to load %1: %2")
				.arg(modulePath, QString::fromStdString(moduleError)));
		}
		return result;
	}

	const QFileInfo bundleInfo(modulePath);
	const QString bundlePath = bundleInfo.isDir()
		? bundleInfo.absoluteFilePath() : resolved;

	for (const auto& classInfo : module->getFactory().classInfos())
	{
		if (classInfo.category() != kVstAudioEffectClass) { continue; }

		const auto subCategories = classInfo.subCategories();
		bool isInstrument = false;
		bool isEffect = false;
		for (const auto& subCategory : subCategories)
		{
			if (subCategory == "Instrument" || subCategory == "Synth")
			{
				isInstrument = true;
			}
			if (subCategory == "Fx")
			{
				isEffect = true;
			}
		}
		if (!isInstrument && !isEffect) { continue; }

		Vst3PluginInfo info;
		info.path = resolved;
		info.bundlePath = bundlePath;
		info.uid = uidToHex(classInfo.ID().data());
		info.name = QString::fromStdString(classInfo.name());
		info.vendor = QString::fromStdString(classInfo.vendor());
		info.category = QString::fromStdString(classInfo.category());
		info.subCategories = QString::fromStdString(
			classInfo.subCategoriesString());
		info.isInstrument = isInstrument;
		info.isEffect = isEffect;
		result.append(info);
	}

	return result;
}


QList<Vst3PluginInfo> Vst3Host::scan(const QStringList& directories,
	QStringList* errors)
{
	QList<Vst3PluginInfo> result;
	QSet<QString> seenPaths;

	for (const QString& directory : directories)
	{
		const QFileInfo directoryInfo(directory);
		if (!directoryInfo.exists() || !directoryInfo.isDir()) { continue; }

		QDir dir(directory);
		if (!dir.isReadable())
		{
			if (errors)
			{
				errors->append(QStringLiteral("cannot read %1").arg(directory));
			}
			continue;
		}

		const QStringList entries = dir.entryList(
			QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
		for (const QString& entry : entries)
		{
			if (!entry.endsWith(QStringLiteral(".vst3"), Qt::CaseInsensitive))
			{
				continue;
			}
			const QString path = dir.filePath(entry);
			const QString resolved = resolveBundleModule(path);
			if (resolved.isEmpty() || seenPaths.contains(resolved)) { continue; }
			seenPaths.insert(resolved);
			result.append(scanModule(path, errors));
		}
	}

	return result;
}


QStringList Vst3Host::defaultSearchPaths()
{
	QStringList paths;

	const QString environment = qEnvironmentVariable("LMMS_VST3_PATH");
	if (!environment.isEmpty())
	{
		for (const QString& entry : environment.split(QDir::listSeparator(),
			Qt::SkipEmptyParts))
		{
			paths << entry;
		}
	}

#if defined(Q_OS_WIN)
	const QString commonFiles = qEnvironmentVariable("COMMONPROGRAMFILES");
	const QString programFiles = qEnvironmentVariable("PROGRAMFILES");
	const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
	if (!commonFiles.isEmpty())
	{
		paths << QDir(commonFiles).filePath(QStringLiteral("VST3"));
	}
	if (!programFiles.isEmpty())
	{
		paths << QDir(programFiles).filePath(QStringLiteral("Common Files/VST3"));
	}
	if (!localAppData.isEmpty())
	{
		paths << QDir(localAppData).filePath(
			QStringLiteral("Programs/Common/VST3"));
	}
#elif defined(Q_OS_MACOS)
	paths << QStringLiteral("/Library/Audio/Plug-Ins/VST3");
	paths << QDir::home().filePath(QStringLiteral("Library/Audio/Plug-Ins/VST3"));
#else
	paths << QStringLiteral("/usr/lib/vst3");
	paths << QStringLiteral("/usr/local/lib/vst3");
	paths << QDir::home().filePath(QStringLiteral(".vst3"));
#endif

	paths.removeDuplicates();
	return paths;
}

} // namespace lmms
