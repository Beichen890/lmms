/*
 * Vst3Instrument.h - VST3 instrument plug-in
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

#ifndef LMMS_VST3_INSTRUMENT_H
#define LMMS_VST3_INSTRUMENT_H

#include <QByteArray>
#include <QMutex>

#include <memory>
#include <vector>

#include "Instrument.h"
#include "Vst3Host.h"

namespace lmms
{

class Vst3Host;

class Vst3Instrument : public Instrument
{
public:
	Vst3Instrument(InstrumentTrack* instrumentTrack,
		const Descriptor::SubPluginFeatures::Key* key);
	~Vst3Instrument() override;

	void play(SampleFrame* buf) override;

	bool handleMidiEvent(const MidiEvent& event, const TimePos& time = TimePos(),
		f_cnt_t offset = 0) override;

	void saveSettings(QDomDocument& doc, QDomElement& element) override;
	void loadSettings(const QDomElement& element) override;

	QString nodeName() const override { return "vst3instrument"; }

	gui::PluginView* instantiateView(QWidget* parent) override;

private:
	void ensureHostReady();
	void convertMidiEvent(const MidiEvent& event, f_cnt_t offset);

	std::unique_ptr<Vst3Host> m_host;
	QMutex m_mutex;
	std::vector<Vst3MidiEvent> m_eventQueue;
	std::vector<float> m_leftBuffer;
	std::vector<float> m_rightBuffer;

	QByteArray m_pendingState;

	bool m_prepared = false;
	bool m_warned = false;
};

} // namespace lmms

#endif // LMMS_VST3_INSTRUMENT_H
