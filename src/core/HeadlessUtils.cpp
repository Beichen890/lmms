/*
 * HeadlessUtils.cpp - helpers shared by the headless CLI actions
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "HeadlessUtils.h"

#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace lmms
{
namespace headless
{

namespace
{

QString xmlEscape(const QString& text)
{
	QString out = text;
	out.replace('&', "&amp;");
	out.replace('<', "&lt;");
	out.replace('>', "&gt;");
	out.replace('"', "&quot;");
	out.replace('\'', "&apos;");
	return out;
}


QString stableElementKey(const QDomElement& element)
{
	// Siblings are grouped by tag name with a stable sort. This keeps the
	// relative order of repeated elements (tracks, mixer channels, automation
	// nodes, ...) intact, so canonicalizing never changes project semantics.
	return element.tagName();
}


void writeElement(QTextStream& out, const QDomElement& element, int depth, bool sortChildren)
{
	const QString indent(depth * 2, ' ');

	out << indent << '<' << element.tagName();

	QMap<QString, QString> attributes;
	const auto attrMap = element.attributes();
	for (int i = 0; i < attrMap.count(); ++i)
	{
		const auto attr = attrMap.item(i).toAttr();
		attributes.insert(attr.name(), attr.value());
	}
	for (auto it = attributes.cbegin(); it != attributes.cend(); ++it)
	{
		out << ' ' << it.key() << "=\"" << xmlEscape(it.value()) << '"';
	}

	QList<QDomElement> children;
	QString text;
	QDomNode node = element.firstChild();
	while (!node.isNull())
	{
		if (node.isElement())
		{
			children << node.toElement();
		}
		else if (node.isText() || node.isCDATASection())
		{
			const QString value = node.nodeValue();
			if (!value.trimmed().isEmpty()) { text += value; }
		}
		node = node.nextSibling();
	}

	if (children.isEmpty() && text.isEmpty())
	{
		out << "/>\n";
		return;
	}

	out << ">\n";
	if (!text.isEmpty())
	{
		out << indent << "  " << xmlEscape(text.trimmed()) << '\n';
	}

	if (sortChildren)
	{
		std::stable_sort(children.begin(), children.end(),
			[](const QDomElement& a, const QDomElement& b) {
				return stableElementKey(a) < stableElementKey(b);
			});
	}

	for (const auto& child : children)
	{
		writeElement(out, child, depth + 1, sortChildren);
	}

	out << indent << "</" << element.tagName() << ">\n";
}


bool isImplicitMasterChannel(const QDomElement& channel)
{
	if (channel.tagName() != "mixerchannel") { return false; }
	if (channel.attribute("num") != "0") { return false; }
	if (channel.attribute("name") != "Master") { return false; }

	// Only strip the default, untouched master channel.
	if (channel.attribute("volume", "100").toDouble() != 100.0) { return false; }
	if (channel.attribute("muted", "0") == "1") { return false; }
	if (channel.attribute("soloed", "0") == "1") { return false; }
	if (channel.hasAttribute("color")) { return false; }

	QDomElement fx = channel.firstChildElement("fxchain");
	if (!fx.isNull())
	{
		if (fx.attribute("numofeffects", "0").toInt() != 0) { return false; }
		if (fx.attribute("enabled", "0") == "1") { return false; }
	}

	// A master channel with explicit sends is considered custom.
	if (!channel.firstChildElement("send").isNull()) { return false; }

	// Any other child element (e.g. an automated volume node) makes it custom.
	QDomNode node = channel.firstChild();
	while (!node.isNull())
	{
		if (node.isElement())
		{
			const QString name = node.toElement().tagName();
			if (name != "fxchain" && name != "send") { return false; }
		}
		node = node.nextSibling();
	}

	return true;
}


bool stripImplicitMaster(QDomDocument&, QDomElement& root)
{
	QDomElement mixer = root.firstChildElement("mixer");
	if (mixer.isNull()) { return false; }

	bool changed = false;
	QList<QDomElement> channels;
	QDomNode node = mixer.firstChild();
	while (!node.isNull())
	{
		const QDomElement element = node.toElement();
		if (isImplicitMasterChannel(element)) { channels << element; }
		node = node.nextSibling();
	}
	for (const auto& channel : channels)
	{
		mixer.removeChild(channel);
		changed = true;
	}

	if (changed && mixer.firstChildElement("mixerchannel").isNull())
	{
		root.removeChild(mixer);
	}

	return changed;
}

} // namespace


bool parseTimeSpec(const QString& spec, int ticksPerBar, int beatsPerBar,
	int* ticks, QString* error)
{
	const QString trimmed = spec.trimmed();
	QRegularExpression re("^([0-9]+(?:\\.[0-9]+)?)\\s*([A-Za-z]+)?$");
	const auto match = re.match(trimmed);
	if (!match.hasMatch())
	{
		if (error) { *error = QString("invalid time specification: %1").arg(spec); }
		return false;
	}

	const double amount = match.captured(1).toDouble();
	const QString unit = match.captured(2).toLower();

	double result = amount;
	if (unit.isEmpty() || unit == "tick" || unit == "ticks")
	{
		if (unit.isEmpty() && amount != std::floor(amount))
		{
			if (error) { *error = QString("tick position must be an integer: %1").arg(spec); }
			return false;
		}
	}
	else if (unit == "bar" || unit == "bars")
	{
		result = amount * ticksPerBar;
	}
	else if (unit == "beat" || unit == "beats")
	{
		const int ticksPerBeat = beatsPerBar > 0 ? ticksPerBar / beatsPerBar : ticksPerBar / 4;
		result = amount * ticksPerBeat;
	}
	else
	{
		if (error) { *error = QString("unknown time unit in %1").arg(spec); }
		return false;
	}

	if (result < 0.0)
	{
		if (error) { *error = QString("negative time position: %1").arg(spec); }
		return false;
	}

	if (ticks) { *ticks = static_cast<int>(std::llround(result)); }
	return true;
}


QString canonicalizeXml(const QByteArray& data, bool sortChildren,
	bool stripMaster, QString* error)
{
	QDomDocument doc;
	QString parseError;
	int line = 0;
	int column = 0;

	if (!doc.setContent(data, &parseError, &line, &column))
	{
		if (error)
		{
			*error = QString("could not parse project XML at line %1, column %2: %3")
				.arg(line).arg(column).arg(parseError);
		}
		return QString();
	}

	QDomElement root = doc.documentElement();
	if (root.isNull())
	{
		if (error) { *error = "project XML has no root element"; }
		return QString();
	}

	if (stripMaster) { stripImplicitMaster(doc, root); }

	QString output;
	QTextStream stream(&output);
	stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	writeElement(stream, root, 0, sortChildren);
	return output;
}

} // namespace headless
} // namespace lmms
