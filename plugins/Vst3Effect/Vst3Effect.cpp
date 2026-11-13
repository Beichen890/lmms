/*
 * Vst3Effect.cpp - VST3 effect plug-in
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

#include "Vst3Effect.h"

#include <QDebug>
#include <QDomElement>

#include "AudioEngine.h"
#include "Engine.h"
#include "LmmsCommonMacros.h"
#include "Vst3SubPluginFeatures.h"

#include "plugin_export.h"

namespace lmms
{

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT vst3effect_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"VST3",
	QT_TRANSLATE_NOOP("PluginBrowser",
		"plugin for using arbitrary VST3 effects inside LMMS."),
	"LMMS contributors",
	0x0100,
	Plugin::Type::Effect,
	nullptr,
	nullptr,
	new Vst3SubPluginFeatures(Plugin::Type::Effect)
} ;

}


Vst3Effect::Vst3Effect(Model* parent,
	const Descriptor::SubPluginFeatures::Key* key) :
	Effect(&vst3effect_plugin_descriptor, parent, key),
	m_controls(this)
{
	const QString path = key ? key->attributes.value("path") : QString();
	const QString uid = key ? key->attributes.value("uid") : QString();

	if (path.isEmpty())
	{
		// Instantiated without a sub plugin key (e.g. while listing plug-ins).
		setDontRun(true);
		return;
	}

	QString error;
	m_host = std::make_unique<Vst3Host>();
	if (!m_host->open(path, uid, &error))
	{
		qWarning() << "Vst3Effect: could not load" << path << ":" << error;
		m_host.reset();
		setDontRun(true);
	}
}


void Vst3Effect::ensureHostReady(double sampleRate)
{
	if (!m_host || !m_host->isValid()) { return; }

	if (m_host->sampleRate() != sampleRate)
	{
		m_host->setSampleRate(sampleRate);
		m_prepared = false;
	}

	if (m_prepared) { return; }

	if (!m_host->prepare(MAXIMUM_BUFFER_SIZE))
	{
		qWarning() << "Vst3Effect:" << m_host->errorString();
		setDontRun(true);
		return;
	}

	m_prepared = true;
	if (!m_pendingState.isEmpty())
	{
		m_host->setState(m_pendingState);
		m_pendingState.clear();
	}
}


Effect::ProcessStatus Vst3Effect::processImpl(SampleFrame* buf,
	const f_cnt_t frames)
{
	if (!m_host || !m_host->isValid() || frames <= 0)
	{
		return ProcessStatus::Continue;
	}

	ensureHostReady(
		static_cast<double>(Engine::audioEngine()->outputSampleRate()));
	if (!m_prepared) { return ProcessStatus::Continue; }

	if (m_leftBuffer.size() < static_cast<size_t>(frames))
	{
		m_leftBuffer.resize(frames);
		m_rightBuffer.resize(frames);
	}

	for (f_cnt_t frame = 0; frame < frames; ++frame)
	{
		m_leftBuffer[frame] = buf[frame].left();
		m_rightBuffer[frame] = buf[frame].right();
	}

	m_host->process(m_leftBuffer.data(), m_rightBuffer.data(), frames,
		nullptr, 0, true);

	const float wet = wetLevel();
	const float dry = dryLevel();
	for (f_cnt_t frame = 0; frame < frames; ++frame)
	{
		buf[frame].setLeft(wet * m_leftBuffer[frame] + dry * buf[frame].left());
		buf[frame].setRight(wet * m_rightBuffer[frame] + dry * buf[frame].right());
	}

	return ProcessStatus::ContinueIfNotQuiet;
}


void Vst3Effect::saveSettings(QDomDocument& doc, QDomElement& element)
{
	Effect::saveSettings(doc, element);
	if (!m_host || !m_host->isValid()) { return; }

	const QByteArray state = m_host->getState();
	if (state.isEmpty()) { return; }

	QDomElement stateElement = doc.createElement("state");
	stateElement.setAttribute("format", "vst3");
	stateElement.appendChild(
		doc.createTextNode(QString::fromLatin1(state.toBase64())));
	element.appendChild(stateElement);
}


void Vst3Effect::loadSettings(const QDomElement& element)
{
	Effect::loadSettings(element);

	const QDomElement stateElement = element.firstChildElement("state");
	if (stateElement.isNull()) { return; }

	const QByteArray state = QByteArray::fromBase64(
		stateElement.text().trimmed().toLatin1());
	if (state.isEmpty()) { return; }

	if (m_host && m_host->isValid())
	{
		if (!m_host->setState(state))
		{
			m_pendingState = state;
		}
	}
	else
	{
		m_pendingState = state;
	}
}

} // namespace lmms


extern "C"
{

PLUGIN_EXPORT lmms::Plugin* lmms_plugin_main(lmms::Model* parent, void* data)
{
	return new lmms::Vst3Effect(parent,
		static_cast<const lmms::Plugin::Descriptor::SubPluginFeatures::Key*>(data));
}

}
