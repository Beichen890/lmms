/*
 * Vst3Instrument.cpp - VST3 instrument plug-in
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

#include "Vst3Instrument.h"

#include <QDebug>
#include <QDomElement>
#include <QMutexLocker>

#include <algorithm>

#include "AudioEngine.h"
#include "Engine.h"
#include "InstrumentPlayHandle.h"
#include "InstrumentTrack.h"
#include "MidiEvent.h"
#include "Song.h"
#include "Vst3SubPluginFeatures.h"

#include "plugin_export.h"

namespace lmms
{

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT vst3instrument_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"VST3",
	QT_TRANSLATE_NOOP("PluginBrowser",
		"plugin for using arbitrary VST3 instruments inside LMMS."),
	"LMMS contributors",
	0x0100,
	Plugin::Type::Instrument,
	nullptr,
	nullptr,
	new Vst3SubPluginFeatures(Plugin::Type::Instrument)
} ;

}


Vst3Instrument::Vst3Instrument(InstrumentTrack* instrumentTrack,
	const Descriptor::SubPluginFeatures::Key* key) :
	Instrument(instrumentTrack, &vst3instrument_plugin_descriptor, key,
		Flag::IsSingleStreamed | Flag::IsMidiBased)
{
	const QString path = key ? key->attributes.value("path") : QString();
	const QString uid = key ? key->attributes.value("uid") : QString();

	if (!path.isEmpty())
	{
		QString error;
		m_host = std::make_unique<Vst3Host>();
		if (!m_host->open(path, uid, &error))
		{
			qWarning() << "Vst3Instrument: could not load" << path << ":" << error;
			m_host.reset();
		}
		else if (!m_host->pluginName().isEmpty())
		{
			setDisplayName(m_host->pluginName());
		}
	}
	else
	{
		qWarning() << "Vst3Instrument: no VST3 plug-in path was specified";
	}

	// Single streamed instruments are driven by an InstrumentPlayHandle which
	// calls play() once per audio period.
	auto playHandle = new InstrumentPlayHandle(this, instrumentTrack);
	Engine::audioEngine()->addPlayHandle(playHandle);
}


Vst3Instrument::~Vst3Instrument()
{
	Engine::audioEngine()->removePlayHandlesOfTypes(instrumentTrack(),
		PlayHandle::Type::NotePlayHandle | PlayHandle::Type::InstrumentPlayHandle);
	m_host.reset();
}


void Vst3Instrument::ensureHostReady()
{
	if (!m_host || !m_host->isValid()) { return; }

	const double sampleRate =
		static_cast<double>(Engine::audioEngine()->outputSampleRate());
	if (m_host->sampleRate() != sampleRate)
	{
		m_host->setSampleRate(sampleRate);
		m_prepared = false;
	}

	if (!m_prepared)
	{
		if (!m_host->prepare(MAXIMUM_BUFFER_SIZE))
		{
			if (!m_warned)
			{
				qWarning() << "Vst3Instrument:" << m_host->errorString();
				m_warned = true;
			}
			return;
		}
		m_prepared = true;

		if (!m_pendingState.isEmpty())
		{
			m_host->setState(m_pendingState);
			m_pendingState.clear();
		}
	}
}


void Vst3Instrument::play(SampleFrame* buf)
{
	if (!m_host || !m_host->isValid()) { return; }

	ensureHostReady();
	if (!m_prepared || buf == nullptr) { return; }

	const f_cnt_t frames = Engine::audioEngine()->framesPerPeriod();
	if (frames <= 0) { return; }

	if (m_leftBuffer.size() < static_cast<size_t>(frames))
	{
		m_leftBuffer.resize(frames);
		m_rightBuffer.resize(frames);
	}

	std::vector<Vst3MidiEvent> events;
	{
		QMutexLocker locker(&m_mutex);
		events.swap(m_eventQueue);
	}

	if (Song* song = Engine::getSong())
	{
		m_host->setTempo(song->tempoModel().value());
	}

	m_host->process(m_leftBuffer.data(), m_rightBuffer.data(), frames,
		events.data(), static_cast<int>(events.size()), false);

	for (f_cnt_t frame = 0; frame < frames; ++frame)
	{
		buf[frame].setLeft(m_leftBuffer[frame]);
		buf[frame].setRight(m_rightBuffer[frame]);
	}
}


bool Vst3Instrument::handleMidiEvent(const MidiEvent& event, const TimePos& time,
	f_cnt_t offset)
{
	Q_UNUSED(time)
	if (!m_host || !m_host->isValid()) { return true; }

	convertMidiEvent(event, offset);
	return true;
}


void Vst3Instrument::convertMidiEvent(const MidiEvent& event, f_cnt_t offset)
{
	Vst3MidiEvent converted;
	converted.sampleOffset = static_cast<int>(std::max<f_cnt_t>(0, offset));
	converted.channel = event.channel() & 0xF;

	switch (event.type())
	{
	case MidiNoteOn:
		converted.type = Vst3MidiEvent::Type::NoteOn;
		converted.note = event.key();
		converted.velocity = event.velocity() / 127.0f;
		break;
	case MidiNoteOff:
		converted.type = Vst3MidiEvent::Type::NoteOff;
		converted.note = event.key();
		converted.velocity = 0.0f;
		break;
	case MidiControlChange:
		converted.type = Vst3MidiEvent::Type::ControlChange;
		converted.controller = event.controllerNumber();
		converted.value = event.controllerValue() / 127.0f;
		break;
	case MidiPitchBend:
		converted.type = Vst3MidiEvent::Type::PitchBend;
		converted.value = event.pitchBend() / 16383.0f;
		break;
	case MidiChannelPressure:
		converted.type = Vst3MidiEvent::Type::ChannelPressure;
		converted.value = event.channelPressure() / 127.0f;
		break;
	default:
		return;
	}

	QMutexLocker locker(&m_mutex);
	if (m_eventQueue.size() < 4096)
	{
		m_eventQueue.push_back(converted);
	}
}


void Vst3Instrument::saveSettings(QDomDocument& doc, QDomElement& element)
{
	if (!m_host || !m_host->isValid()) { return; }

	const QByteArray state = m_host->getState();
	if (state.isEmpty()) { return; }

	QDomElement stateElement = doc.createElement("state");
	stateElement.setAttribute("format", "vst3");
	stateElement.appendChild(
		doc.createTextNode(QString::fromLatin1(state.toBase64())));
	element.appendChild(stateElement);
}


void Vst3Instrument::loadSettings(const QDomElement& element)
{
	const QDomElement stateElement = element.firstChildElement("state");
	if (stateElement.isNull()) { return; }

	const QByteArray state = QByteArray::fromBase64(
		stateElement.text().trimmed().toLatin1());
	if (state.isEmpty()) { return; }

	if (m_host && m_host->isValid() && m_prepared)
	{
		m_host->setState(state);
	}
	else
	{
		m_pendingState = state;
	}
}


gui::PluginView* Vst3Instrument::instantiateView(QWidget* parent)
{
	// The minimal VST3 host intentionally does not embed plug-in editors.
	Q_UNUSED(parent)
	return nullptr;
}

} // namespace lmms


extern "C"
{

PLUGIN_EXPORT lmms::Plugin* lmms_plugin_main(lmms::Model* parent, void* data)
{
	return new lmms::Vst3Instrument(
		static_cast<lmms::InstrumentTrack*>(parent),
		static_cast<const lmms::Plugin::Descriptor::SubPluginFeatures::Key*>(data));
}

}
