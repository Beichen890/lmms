/*
 * HeadlessScan.cpp - plugin scanning for the headless CLI
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "HeadlessScan.h"
#include "lmmsconfig.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QtGlobal>

#ifdef LMMS_HAVE_LV2
#include <lilv/lilv.h>
#endif

#include "Engine.h"
#include "Ladspa2LMMS.h"
#include "Plugin.h"
#include "PluginFactory.h"

#ifdef LMMS_HAVE_LV2
#include "Lv2Manager.h"
#endif

namespace lmms
{
namespace headless
{

namespace
{

QString pluginTypeName(Plugin::Type type)
{
	switch (type)
	{
	case Plugin::Type::Instrument: return "instrument";
	case Plugin::Type::Effect: return "effect";
	case Plugin::Type::ImportFilter: return "importfilter";
	case Plugin::Type::ExportFilter: return "exportfilter";
	case Plugin::Type::Tool: return "tool";
	case Plugin::Type::Library: return "library";
	case Plugin::Type::Other: return "other";
	default: return "undefined";
	}
}


void scanNativePlugins(QJsonArray& native, QJsonArray& errors)
{
	try
	{
		PluginFactory* factory = getPluginFactory();

		QHash<const Plugin::Descriptor*, QString> files;
		for (const auto& info : factory->pluginInfos())
		{
			if (info.descriptor != nullptr)
			{
				files.insert(info.descriptor, info.file.filePath());
			}
		}

		Plugin::DescriptorList descriptors = factory->descriptors();
		std::sort(descriptors.begin(), descriptors.end(),
			[](const Plugin::Descriptor* a, const Plugin::Descriptor* b) {
				return QString::fromUtf8(a->name) < QString::fromUtf8(b->name);
			});

		for (const Plugin::Descriptor* descriptor : descriptors)
		{
			QJsonObject object;
			object["name"] = QString::fromUtf8(descriptor->name);
			object["displayName"] = QString::fromUtf8(descriptor->displayName);
			object["type"] = pluginTypeName(descriptor->type);
			object["file"] = files.value(descriptor);
			native.append(object);
		}
	}
	catch (...)
	{
		errors.append(QStringLiteral("native plugin scan failed"));
	}
}


void scanLadspaPlugins(QJsonArray& ladspa, QJsonArray& errors)
{
	try
	{
		Ladspa2LMMS* manager = Engine::getLADSPAManager();
		if (manager == nullptr)
		{
			errors.append(QStringLiteral("LADSPA manager is not available"));
			return;
		}

		for (const auto& entry : manager->getSortedPlugins())
		{
			const ladspa_key_t& key = entry.second;
			QJsonObject object;
			object["file"] = key.first;
			object["label"] = key.second;
			object["name"] = manager->getName(key);
			ladspa.append(object);
		}
	}
	catch (...)
	{
		errors.append(QStringLiteral("LADSPA plugin scan failed"));
	}
}


void scanVstPlugins(QJsonArray& vst, QJsonArray& errors)
{
	auto scanDescriptor = [&](const char* pluginName) {
		try
		{
			const PluginFactory::PluginInfo info =
				getPluginFactory()->pluginInfo(pluginName);
			const QString displayName = QString::fromUtf8(pluginName);

			if (info.isNull() || info.descriptor == nullptr)
			{
				errors.append(QString("VST plugin \"%1\" is not available")
					.arg(displayName));
				return;
			}
			if (info.descriptor->subPluginFeatures == nullptr)
			{
				errors.append(QString("VST plugin \"%1\" does not expose sub plugins")
					.arg(displayName));
				return;
			}

			Plugin::Descriptor::SubPluginFeatures::KeyList keys;
			info.descriptor->subPluginFeatures->listSubPluginKeys(
				info.descriptor, keys);

			for (const auto& key : keys)
			{
				QJsonObject attributes;
				for (auto it = key.attributes.constBegin();
					it != key.attributes.constEnd(); ++it)
				{
					attributes[it.key()] = it.value();
				}

				QJsonObject object;
				object["name"] = key.name;
				object["file"] = key.attributes.value("file");
				object["key"] = attributes;
				vst.append(object);
			}
		}
		catch (...)
		{
			errors.append(QString("failed to scan \"%1\" plugins")
				.arg(QString::fromUtf8(pluginName)));
		}
	};

	scanDescriptor("vsteffect");
	scanDescriptor("vestige");
}


void scanVst3Plugins(QJsonArray& vst3, QJsonArray& errors)
{
	auto scanDescriptor = [&](const char* pluginName, const char* typeName) {
		try
		{
			const PluginFactory::PluginInfo info =
				getPluginFactory()->pluginInfo(pluginName);
			if (info.isNull() || info.descriptor == nullptr)
			{
				errors.append(QString("VST3 host plugin \"%1\" is not available")
					.arg(QString::fromUtf8(pluginName)));
				return;
			}
			if (info.descriptor->subPluginFeatures == nullptr)
			{
				errors.append(QString("VST3 host plugin \"%1\" does not expose sub plugins")
					.arg(QString::fromUtf8(pluginName)));
				return;
			}

			Plugin::Descriptor::SubPluginFeatures::KeyList keys;
			info.descriptor->subPluginFeatures->listSubPluginKeys(
				info.descriptor, keys);

			for (const auto& key : keys)
			{
				QJsonObject object;
				object["name"] = key.name;
				object["path"] = key.attributes.value("path");
				object["uid"] = key.attributes.value("uid");
				object["vendor"] = key.attributes.value("vendor");
				object["type"] = QString::fromUtf8(typeName);
				object["format"] = QStringLiteral("vst3");
				object["supported"] = true;
				vst3.append(object);
			}
		}
		catch (...)
		{
			errors.append(QString("failed to scan \"%1\" plug-ins")
				.arg(QString::fromUtf8(pluginName)));
		}
	};

	scanDescriptor("vst3instrument", "instrument");
	scanDescriptor("vst3effect", "effect");
}


void appendLv2Plugins(QJsonArray& lv2, QJsonArray& errors)
{
#ifdef LMMS_HAVE_LV2
	try
	{
		Lv2Manager* manager = Engine::getLv2Manager();
		if (manager == nullptr)
		{
			errors.append(QStringLiteral("LV2 manager is not available"));
			return;
		}

		for (auto it = manager->begin(); it != manager->end(); ++it)
		{
			const LilvPlugin* plugin = it->second.plugin();
			QString name;
			if (plugin != nullptr)
			{
				LilvNode* nameNode = lilv_plugin_get_name(plugin);
				if (nameNode != nullptr)
				{
					name = QString::fromUtf8(lilv_node_as_string(nameNode));
					lilv_node_free(nameNode);
				}
			}

			QJsonObject object;
			object["uri"] = QString::fromStdString(it->first);
			object["name"] = name;
			object["type"] = pluginTypeName(it->second.type());
			object["valid"] = it->second.isValid();
			lv2.append(object);
		}
	}
	catch (...)
	{
		errors.append(QStringLiteral("LV2 plugin scan failed"));
	}
#else
	Q_UNUSED(lv2)
	errors.append(QStringLiteral("LV2 support is not compiled in"));
#endif
}


QStringList clapSearchDirectories()
{
	QStringList directories;
#ifdef Q_OS_WIN
	const QString commonFiles = qEnvironmentVariable("COMMONPROGRAMFILES");
	const QString programFiles = qEnvironmentVariable("PROGRAMFILES");
	if (!commonFiles.isEmpty())
	{
		directories << QDir(commonFiles).filePath("CLAP");
	}
	if (!programFiles.isEmpty())
	{
		directories << QDir(programFiles).filePath("Common Files/CLAP");
	}
#elif defined(Q_OS_MACOS)
	directories << QStringLiteral("/Library/Audio/Plug-Ins/CLAP");
#else
	directories << QStringLiteral("/usr/lib/clap");
	directories << QDir::home().filePath(QStringLiteral(".clap"));
#endif
	return directories;
}


//! Lists plugin bundles in standard locations. CLAP cannot be hosted by
//! LMMS, so every entry is explicitly reported as unsupported.
void scanBundleDirectories(const QStringList& directories, const QString& suffix,
	const QString& format, QJsonArray& target, QJsonArray& errors)
{
	for (const QString& path : directories)
	{
		const QFileInfo directoryInfo(path);
		if (!directoryInfo.exists() || !directoryInfo.isDir()) { continue; }

		QDir directory(path);
		if (!directory.isReadable())
		{
			errors.append(QString("cannot read %1 directory: %2")
				.arg(format, path));
			continue;
		}

		const QStringList entries = directory.entryList(
			QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		for (const QString& entry : entries)
		{
			if (!entry.endsWith(suffix, Qt::CaseInsensitive)) { continue; }

			QJsonObject object;
			object["name"] = entry;
			object["path"] = directory.filePath(entry);
			object["format"] = format;
			object["supported"] = false;
			object["reason"] = QStringLiteral("host does not support VST3/CLAP");
			target.append(object);
		}
	}
}

} // namespace


int runScanPluginsAction(const QString& format, bool jsonOutput)
{
	const QString requested = format.trimmed().toLower();
	const QStringList validFormats = {
		"all", "native", "ladspa", "vst", "lv2", "vst3", "clap"
	};
	const bool valid = validFormats.contains(requested);

	QJsonArray native;
	QJsonArray ladspa;
	QJsonArray vst;
	QJsonArray lv2;
	QJsonArray vst3;
	QJsonArray clap;
	QJsonArray errors;

	if (!valid)
	{
		errors.append(QString("unknown format: %1").arg(format));
	}
	else
	{
		const bool all = requested == "all";
		const auto wants = [&](const char* category) {
			return all || requested == QLatin1String(category);
		};

		if (wants("native")) { scanNativePlugins(native, errors); }
		if (wants("ladspa")) { scanLadspaPlugins(ladspa, errors); }
		if (wants("vst")) { scanVstPlugins(vst, errors); }
		if (wants("lv2")) { appendLv2Plugins(lv2, errors); }
		if (wants("vst3"))
		{
			scanVst3Plugins(vst3, errors);
		}
		if (wants("clap"))
		{
			scanBundleDirectories(clapSearchDirectories(), QStringLiteral(".clap"),
				QStringLiteral("clap"), clap, errors);
		}
	}

	QJsonObject root;
	root["ok"] = valid;
	root["native"] = native;
	root["ladspa"] = ladspa;
	root["vst"] = vst;
	root["lv2"] = lv2;
	root["vst3"] = vst3;
	root["clap"] = clap;
	root["errors"] = errors;

	const QByteArray json = QJsonDocument(root).toJson(
		jsonOutput ? QJsonDocument::Compact : QJsonDocument::Indented);
	fwrite(json.constData(), 1, json.size(), stdout);
	fflush(stdout);

	if (!valid)
	{
		fprintf(stderr, "unknown plugin format: %s\n", qUtf8Printable(format));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace headless
} // namespace lmms
