/*
 * Lv2ControlBase.cpp - Lv2 control base class
 *
 * Copyright (c) 2018-2023 Johannes Lorenz <jlsf2013$users.sourceforge.net, $=@>
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

#include "Lv2ControlBase.h"

#ifdef LMMS_HAVE_LV2

#include <algorithm>
#include <memory>
#include <type_traits>

#include <lv2/state/state.h>

#include <QByteArray>
#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QtGlobal>

#include "Engine.h"
#include "lmms_constants.h"
#include "Lv2Manager.h"
#include "Lv2Proc.h"
#include "Lv2ProcSuspender.h"


namespace lmms
{


namespace
{

struct TempLilvWorldDeleter
{
	void operator()(LilvWorld* world) const { lilv_world_free(world); }
};
using AutoTempLilvWorld = std::unique_ptr<LilvWorld, TempLilvWorldDeleter>;

struct TempLilvStateDeleter
{
	void operator()(LilvState* state) const { lilv_state_free(state); }
};
using AutoTempLilvState = std::unique_ptr<LilvState, TempLilvStateDeleter>;

//! Serialize the complete state (including non-port state) of an instance
//! into a Turtle string. Returns an empty string if the plugin has no state.
QString lv2StateToString(const LilvPlugin* plugin, LilvInstance* instance)
{
	Lv2Manager* manager = Engine::getLv2Manager();
	LV2_URID_Map* map = manager->uridMap().mapFeature();
	LV2_URID_Unmap* unmap = manager->uridMap().unmapFeature();

	AutoTempLilvWorld world{lilv_world_new()};
	if (!world) { return {}; }

	AutoTempLilvState state{lilv_state_new_from_instance(plugin, instance, map,
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr)};
	if (!state) { return {}; }

	const char* pluginUri = lilv_node_as_uri(lilv_plugin_get_uri(plugin));
	char* str = lilv_state_to_string(world.get(), map, unmap, state.get(),
		pluginUri ? pluginUri : "http://lmms.sourceforge.net/ns/lv2#state",
		nullptr);
	if (!str) { return {}; }

	const QString result = QString::fromUtf8(str);
	lilv_free(str);
	return result;
}

//! Restore an instance from a state. Newer lilv versions take the instance,
//! older ones (e.g. 0.24.0) take the state interface and handle.
template<typename RestoreFunc>
void lv2RestoreState(RestoreFunc restore, const LilvState* state,
	LilvInstance* instance)
{
	if constexpr (std::is_same_v<RestoreFunc,
		void (*)(const LilvState*, LilvInstance*, LilvSetPortValueFunc,
			void*, uint32_t, const LV2_Feature* const*)>)
	{
		restore(state, instance, nullptr, nullptr, 0u, nullptr);
	}
	else
	{
		const auto iface = instance
			? static_cast<const LV2_State_Interface*>(
				lilv_instance_get_extension_data(instance, LV2_STATE__interface))
			: nullptr;
		restore(state, iface,
			instance ? lilv_instance_get_handle(instance) : nullptr,
			nullptr, nullptr, 0u, nullptr);
	}
}

} // anonymous namespace


Plugin::Type Lv2ControlBase::check(const LilvPlugin *plugin,
	std::vector<PluginIssue> &issues)
{
	// for some reason, all checks can be done by one processor...
	return Lv2Proc::check(plugin, issues);
}




Lv2ControlBase::Lv2ControlBase(Model* that, const QString &uri) :
	m_plugin(Engine::getLv2Manager()->getPlugin(uri))
{
	if (m_plugin)
	{
		init(that);
	}
	else
	{
		qCritical() << "No Lv2 plugin found for URI" << uri;
		throw std::runtime_error("No Lv2 plugin found for given URI");
	}
}




Lv2ControlBase::~Lv2ControlBase() = default;




void Lv2ControlBase::init(Model* meAsModel)
{
	int channelsLeft = DEFAULT_CHANNELS; // LMMS plugins are stereo
	while (channelsLeft > 0)
	{
		std::unique_ptr<Lv2Proc> newOne = std::make_unique<Lv2Proc>(m_plugin, meAsModel);
		channelsLeft -= std::max(
			1 + static_cast<bool>(newOne->inPorts().m_right),
			1 + static_cast<bool>(newOne->outPorts().m_right));
		Q_ASSERT(channelsLeft >= 0);
		m_procs.push_back(std::move(newOne));
	}
	m_channelsPerProc = DEFAULT_CHANNELS / m_procs.size();
	linkAllModels();
}




void Lv2ControlBase::shutdown()
{
	// currently nothing to do here
}




void Lv2ControlBase::reload()
{
	for (const auto& c : m_procs) { c->reload(); }
}




LinkedModelGroup *Lv2ControlBase::getGroup(std::size_t idx)
{
	return (m_procs.size() > idx) ? m_procs[idx].get() : nullptr;
}




const LinkedModelGroup *Lv2ControlBase::getGroup(std::size_t idx) const
{
	return (m_procs.size() > idx) ? m_procs[idx].get() : nullptr;
}




void Lv2ControlBase::copyModelsFromLmms() {
	for (const auto& c : m_procs) { c->copyModelsFromCore(); }
}




void Lv2ControlBase::copyModelsToLmms() const
{
	for (const auto& c : m_procs) { c->copyModelsToCore(); }
}




void Lv2ControlBase::copyBuffersFromLmms(const SampleFrame* buf, f_cnt_t frames) {
	unsigned firstChan = 0; // tell the procs which channels they shall read from
	for (const auto& c : m_procs) 
	{
		c->copyBuffersFromCore(buf, firstChan, m_channelsPerProc, frames);
		firstChan += m_channelsPerProc;
	}
}




void Lv2ControlBase::copyBuffersToLmms(SampleFrame* buf, f_cnt_t frames) const {
	unsigned firstChan = 0; // tell the procs which channels they shall write to
	for (const auto& c : m_procs) {
		c->copyBuffersToCore(buf, firstChan, m_channelsPerProc, frames);
		firstChan += m_channelsPerProc;
	}
}




void Lv2ControlBase::run(f_cnt_t frames) {
	for (const auto& c : m_procs) { c->run(frames); }
}




void Lv2ControlBase::saveSettings(QDomDocument &doc, QDomElement &that)
{
	LinkedModelGroups::saveSettings(doc, that);

	// Save the plugin's own state (state:interface). Port values are already
	// covered by LinkedModelGroups, but plugins may keep additional internal
	// state which would otherwise be lost on save/load.
	const AutoLilvNode stateInterfaceUri = Engine::getLv2Manager()->uri(LV2_STATE__interface);
	if (lilv_plugin_has_extension_data(m_plugin, stateInterfaceUri.get()))
	{
		for (std::size_t i = 0; i < m_procs.size(); ++i)
		{
			LilvInstance* instance = Lv2ProcSuspender::instanceOf(m_procs[i].get());
			if (!instance) { continue; }

			const QString state = lv2StateToString(m_plugin, instance);
			if (state.isEmpty()) { continue; }

			QDomElement stateElement = doc.createElement("lv2state");
			stateElement.setAttribute("proc", static_cast<int>(i));
			stateElement.appendChild(doc.createTextNode(state));
			that.appendChild(stateElement);
		}
	}
	else
	{
		qDebug() << "Lv2:" << qStringFromPluginNode(m_plugin, lilv_plugin_get_name)
			<< "does not support plugin state; only port values are saved";
	}
}




void Lv2ControlBase::loadSettings(const QDomElement &that)
{
	LinkedModelGroups::loadSettings(that);

	Lv2Manager* manager = Engine::getLv2Manager();
	LV2_URID_Map* map = manager->uridMap().mapFeature();

	for (QDomElement stateElement = that.firstChildElement("lv2state");
		!stateElement.isNull();
		stateElement = stateElement.nextSiblingElement("lv2state"))
	{
		const int procIndex = stateElement.attribute("proc", "0").toInt();
		if (procIndex < 0
			|| static_cast<std::size_t>(procIndex) >= m_procs.size())
		{
			qWarning() << "Lv2: ignoring state for unknown processor"
				<< procIndex << "of"
				<< qStringFromPluginNode(m_plugin, lilv_plugin_get_name);
			continue;
		}

		LilvInstance* instance = Lv2ProcSuspender::instanceOf(m_procs[procIndex].get());
		if (!instance) { continue; }

		const QByteArray stateString = stateElement.text().toUtf8();
		if (stateString.isEmpty()) { continue; }

		AutoTempLilvWorld world{lilv_world_new()};
		AutoTempLilvState state{world
			? lilv_state_new_from_string(world.get(), map, stateString.constData())
			: nullptr};
		if (!state)
		{
			qWarning() << "Lv2: could not parse saved state for"
				<< qStringFromPluginNode(m_plugin, lilv_plugin_get_name)
				<< "- plugin state was not restored";
			continue;
		}

		// Note: state:restore must not run concurrently with the plugin's
		// process function. loadSettings() is called while a project is
		// loaded, i.e. before rendering starts, so this is safe in the
		// headless work flow.
		lv2RestoreState(&lilv_state_restore, state.get(), instance);
	}
}




void Lv2ControlBase::loadFile(const QString &file)
{
	(void)file;
}




std::size_t Lv2ControlBase::controlCount() const {
	std::size_t res = 0;
	for (const auto& c : m_procs) { res += c->controlCount(); }
	return res;
}




bool Lv2ControlBase::hasNoteInput() const
{
	return std::any_of(m_procs.begin(), m_procs.end(),
		[](const auto& c) { return c->hasNoteInput(); });
}




void Lv2ControlBase::handleMidiInputEvent(const MidiEvent &event,
	const TimePos &time, f_cnt_t offset)
{
	for (const auto& c : m_procs) { c->handleMidiInputEvent(event, time, offset); }
}


} // namespace lmms


#endif // LMMS_HAVE_LV2
