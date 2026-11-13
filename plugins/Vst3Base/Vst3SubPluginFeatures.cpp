/*
 * Vst3SubPluginFeatures.cpp - sub plug-in listing for VST3 modules
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

#include "Vst3SubPluginFeatures.h"

#include <QDebug>
#include <QLabel>
#include <QStringList>

#include <mutex>

#include "Vst3Host.h"

namespace lmms
{

namespace
{

const QList<Vst3PluginInfo>& scannedPlugins()
{
	static std::mutex mutex;
	static bool scanned = false;
	static QList<Vst3PluginInfo> plugins;
	static QStringList errors;

	std::lock_guard<std::mutex> lock(mutex);
	if (!scanned)
	{
		plugins = Vst3Host::scan(Vst3Host::defaultSearchPaths(), &errors);
		for (const QString& error : errors)
		{
			qWarning("VST3 scan: %s", qUtf8Printable(error));
		}
		scanned = true;
	}
	return plugins;
}

} // namespace


Vst3SubPluginFeatures::Vst3SubPluginFeatures(Plugin::Type type) :
	SubPluginFeatures(type)
{
}


void Vst3SubPluginFeatures::fillDescriptionWidget(QWidget* parent,
	const Key* key) const
{
	new QLabel(QWidget::tr("Name: ") + key->name, parent);
	new QLabel(QWidget::tr("File: ") + key->attributes.value("path"), parent);
	if (!key->attributes.value("uid").isEmpty())
	{
		new QLabel(QWidget::tr("UID: ") + key->attributes.value("uid"), parent);
	}
}


void Vst3SubPluginFeatures::listSubPluginKeys(
	const Plugin::Descriptor* descriptor, KeyList& keys) const
{
	for (const Vst3PluginInfo& plugin : scannedPlugins())
	{
		const bool matches = m_type == Plugin::Type::Instrument
			? plugin.isInstrument
			: (m_type == Plugin::Type::Effect && plugin.isEffect);
		if (!matches) { continue; }

		Plugin::Descriptor::SubPluginFeatures::Key::AttributeMap attributes;
		attributes["path"] = plugin.bundlePath;
		attributes["uid"] = plugin.uid;
		attributes["vendor"] = plugin.vendor;
		attributes["module"] = plugin.path;
		keys.push_back(Key(descriptor, plugin.name, attributes));
	}
}

} // namespace lmms
