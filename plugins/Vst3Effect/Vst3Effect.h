/*
 * Vst3Effect.h - VST3 effect plug-in
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

#ifndef LMMS_VST3_EFFECT_H
#define LMMS_VST3_EFFECT_H

#include <QByteArray>

#include <memory>
#include <vector>

#include "Effect.h"
#include "EffectControls.h"
#include "Vst3Host.h"

namespace lmms
{

class Vst3Effect;

//! Placeholder controls: the minimal VST3 host does not expose parameters as
//! LMMS models yet. Plug-in state is stored as an opaque blob instead.
class Vst3EffectControls : public EffectControls
{
public:
	explicit Vst3EffectControls(Effect* effect) :
		EffectControls(effect)
	{
	}

	int controlCount() override { return 0; }
	gui::EffectControlDialog* createView() override { return nullptr; }
	QString nodeName() const override { return "vst3effectcontrols"; }
	void saveSettings(QDomDocument&, QDomElement&) override {}
	void loadSettings(const QDomElement&) override {}
};


class Vst3Effect : public Effect
{
public:
	Vst3Effect(Model* parent, const Descriptor::SubPluginFeatures::Key* key);

	EffectControls* controls() override { return &m_controls; }

	void saveSettings(QDomDocument& doc, QDomElement& element) override;
	void loadSettings(const QDomElement& element) override;

protected:
	ProcessStatus processImpl(SampleFrame* buf, const f_cnt_t frames) override;

private:
	void ensureHostReady(double sampleRate);

	Vst3EffectControls m_controls;
	std::unique_ptr<Vst3Host> m_host;
	std::vector<float> m_leftBuffer;
	std::vector<float> m_rightBuffer;
	QByteArray m_pendingState;
	bool m_prepared = false;
};

} // namespace lmms

#endif // LMMS_VST3_EFFECT_H
