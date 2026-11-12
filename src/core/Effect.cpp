/*
 * Effect.cpp - base-class for effects
 *
 * Copyright (c) 2006-2007 Danny McRae <khjklujn/at/users.sourceforge.net>
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Effect.h"

#include <algorithm>

#include <QDebug>
#include <QDomElement>
#include <QStringList>

#include "AudioBuffer.h"
#include "ConfigManager.h"
#include "EffectChain.h"
#include "EffectControls.h"
#include "EffectView.h"
#include "SampleFrame.h"

namespace lmms
{

namespace
{

// Effect controls were historically saved under inconsistent element names
// (e.g. "Eq", "crossoevereqcontrols", "dynamicsprocessor_controls"), while
// external tools tend to write "<plugin>controls". Compare names forgivingly
// so parameters are not dropped without any indication.
QString normalizedControlNodeName(const QString& name)
{
	QString normalized = name.toLower();
	normalized.remove(QLatin1Char('_'));
	normalized.remove(QLatin1Char('-'));
	return normalized;
}

//! Child elements of <effect> which never hold effect parameters
bool isNonControlNodeName(const QString& name)
{
	static const QStringList ignored = {
		"on", "wet", "mix", "autoquit", "chunk", "state", "sampledata", "key"
	};
	return ignored.contains(name.toLower());
}

bool looksLikeControlNodeName(const QString& name)
{
	const QString normalized = normalizedControlNodeName(name);
	return normalized.endsWith(QStringLiteral("controls"))
		|| normalized.endsWith(QStringLiteral("control"));
}

//! Only accept elements which actually carry something that could be a parameter
bool hasControlContent(const QDomElement& element)
{
	return element.hasAttributes() || !element.firstChildElement().isNull();
}

} // namespace


Effect::Effect( const Plugin::Descriptor * _desc,
			Model * _parent,
			const Descriptor::SubPluginFeatures::Key * _key ) :
	Plugin( _desc, _parent, _key ),
	m_parent( nullptr ),
	m_okay( true ),
	m_noRun( false ),
	m_awake(false),
	m_enabledModel( true, this, tr( "Effect enabled" ) ),
	m_wetDryModel( 1.0f, -1.0f, 1.0f, 0.01f, this, tr( "Wet/Dry mix" ) ),
	m_autoQuitModel( 1.0f, 1.0f, 8000.0f, 100.0f, 1.0f, this, tr( "Decay" ) ),
	m_autoQuitEnabled(ConfigManager::inst()->value("ui", "disableautoquit", "1").toInt() == 0)
{
	m_wetDryModel.setCenterValue(0);

	// Call the virtual method onEnabledChanged so that effects can react to changes,
	// e.g. by resetting state.
	connect(&m_enabledModel, &BoolModel::dataChanged, [this] { onEnabledChanged(); });
}

void Effect::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	m_enabledModel.saveSettings( _doc, _this, "on" );
	m_wetDryModel.saveSettings( _doc, _this, "wet" );
	m_autoQuitModel.saveSettings( _doc, _this, "autoquit" );
	controls()->saveState( _doc, _this );
}




void Effect::loadSettings( const QDomElement & _this )
{
	m_enabledModel.loadSettings( _this, "on" );

	// "mix" is a 0..1 alias for "wet" (-1..1). "wet" stays authoritative and
	// is the only value written on save, so existing projects keep working.
	const bool hasWet = _this.hasAttribute( "wet" )
		|| !_this.firstChildElement( "wet" ).isNull();
	const bool hasMix = _this.hasAttribute( "mix" )
		|| !_this.firstChildElement( "mix" ).isNull();
	if( !hasWet && hasMix )
	{
		m_wetDryModel.loadSettings( _this, "mix" );
		// 0 (dry) .. 1 (wet), clamped to avoid negative values from the alias
		m_wetDryModel.setValue( std::clamp( m_wetDryModel.value(), 0.0f, 1.0f ) );
	}
	else
	{
		m_wetDryModel.loadSettings( _this, "wet" );
	}

	m_autoQuitModel.loadSettings( _this, "autoquit" );

	const QString expectedName = controls()->nodeName();
	QDomElement controlsElement;
	bool foundByName = false;
	bool hasChildElement = false;

	// First pass: exact, case-insensitive and separator-insensitive match
	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		const QDomElement element = node.toElement();
		if( !element.isNull() )
		{
			hasChildElement = true;
			if( !isNonControlNodeName( element.nodeName() ) )
			{
				const QString actualName = element.nodeName();
				if( actualName == expectedName )
				{
					controlsElement = element;
					foundByName = true;
					break;
				}
				if( actualName.compare( expectedName, Qt::CaseInsensitive ) == 0
					|| normalizedControlNodeName( actualName )
						== normalizedControlNodeName( expectedName ) )
				{
					controlsElement = element;
					foundByName = true;
					break;
				}
			}
		}
		node = node.nextSibling();
	}

	// Second pass: accept any "<...>controls" node, e.g. <eqcontrols> for "Eq"
	// or <crossovereqcontrols> for "crossoevereqcontrols"
	if( controlsElement.isNull() )
	{
		node = _this.firstChild();
		while( !node.isNull() )
		{
			const QDomElement element = node.toElement();
			if( !element.isNull() && !isNonControlNodeName( element.nodeName() )
				&& looksLikeControlNodeName( element.nodeName() ) )
			{
				controlsElement = element;
				break;
			}
			node = node.nextSibling();
		}
	}

	// Last resort: the first child element that can hold parameters. This
	// covers external tools which use arbitrary names for the controls node.
	if( controlsElement.isNull() )
	{
		node = _this.firstChild();
		while( !node.isNull() )
		{
			const QDomElement element = node.toElement();
			if( !element.isNull() && !isNonControlNodeName( element.nodeName() )
				&& hasControlContent( element ) )
			{
				controlsElement = element;
				break;
			}
			node = node.nextSibling();
		}
	}

	if( !controlsElement.isNull() )
	{
		if( !foundByName )
		{
			qWarning( "Effect \"%s\": control node \"%s\" does not match the expected "
				"node name \"%s\"; restoring its parameters anyway. Save the "
				"project to use the expected name.",
				descriptor()->name,
				qUtf8Printable( controlsElement.nodeName() ),
				qUtf8Printable( expectedName ) );
		}
		controls()->restoreState( controlsElement );
	}
	else if( hasChildElement )
	{
		qWarning( "Effect \"%s\": no control node named \"%s\" found; effect "
			"parameters were not restored.",
			descriptor()->name, qUtf8Printable( expectedName ) );
	}
}




bool Effect::processAudioBuffer(AudioBuffer& inOut)
{
	if (!isAwake())
	{
		if (!inOut.hasSignal(0b11))
		{
			// Sleeping plugins need to zero any track channels their output is routed to in order to
			// prevent sudden track channel passthrough behavior when the plugin is put to sleep.
			// Otherwise auto-quit could become audibly noticeable, which is not intended.

			inOut.silenceChannels(0b11);

			return false;
		}

		wakeUp();
	}

	if (!isProcessingAudio())
	{
		// Plugin is awake but not processing audio
		processBypassedImpl();
		return false;
	}

	const auto status = processImpl(inOut.interleavedBuffer().asSampleFrames().data(), inOut.frames());

	// Copy interleaved plugin output to planar
	toPlanar(inOut.interleavedBuffer(), inOut.groupBuffers(0));

	const auto sanitized = Engine::audioEngine()->sanitizationEnabled() ? inOut.sanitize(0b11) : false;
	m_corrupted.store(sanitized, std::memory_order_relaxed);

	// Update silence status for track channels the processor wrote to
	const bool silentOutput = inOut.updateSilenceFlags(0b11);

	switch (status)
	{
		case ProcessStatus::Continue:
			break;
		case ProcessStatus::ContinueIfNotQuiet:
			handleAutoQuit(silentOutput);
			break;
		case ProcessStatus::Sleep:
			goToSleep();
			return false;
		default:
			break;
	}

	return isAwake();
}




Effect * Effect::instantiate( const QString& pluginName,
				Model * _parent,
				Descriptor::SubPluginFeatures::Key * _key )
{
	Plugin * p = Plugin::instantiateWithKey( pluginName, _parent, _key );
	// check whether instantiated plugin is an effect
	if( dynamic_cast<Effect *>( p ) != nullptr )
	{
		// everything ok, so return pointer
		auto effect = dynamic_cast<Effect*>(p);
		effect->m_parent = dynamic_cast<EffectChain *>(_parent);
		return effect;
	}

	// not quite... so delete plugin and leave it up to the caller to instantiate a DummyEffect
	delete p;

	return nullptr;
}




void Effect::handleAutoQuit(bool silentOutput)
{
	if (!m_autoQuitEnabled)
	{
		return;
	}

	// Check whether we need to continue processing input. Restart the
	// counter if the threshold has been exceeded.

	if (silentOutput)
	{
		// The output buffer is quiet, so check if auto-quit should be activated yet
		if (++m_quietBufferCount > timeout())
		{
			// Activate auto-quit
			goToSleep();
		}
	}
	else
	{
		// The output buffer is not quiet
		m_quietBufferCount = 0;
	}
}




gui::PluginView * Effect::instantiateView( QWidget * _parent )
{
	return new gui::EffectView( this, _parent );
}

} // namespace lmms
