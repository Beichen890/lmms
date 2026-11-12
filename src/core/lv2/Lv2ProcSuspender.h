/*
 * Lv2ProcSuspender.h - temporarily suspend an Lv2 processor
 *
 * Copyright (c) 2018-2024 Johannes Lorenz <jlsf2013$users.sourceforge.net, $=@>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LMMS_LV2_PROC_SUSPENDER_H
#define LMMS_LV2_PROC_SUSPENDER_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_LV2

#include <lilv/lilv.h>

#include "Lv2Proc.h"
#include "NoCopyNoMove.h"

namespace lmms
{


//! Temporarily shuts down a processor's plugin instance and restores it on
//! destruction.
//! This class is a friend of Lv2Proc, so it also gives access to the raw
//! LilvInstance (needed for LV2 state save/restore).
class Lv2ProcSuspender : NoCopyNoMove
{
public:
	explicit Lv2ProcSuspender(Lv2Proc* proc)
		: m_proc(proc)
		, m_wasActive(proc->m_instance)
	{
		if (m_wasActive) { m_proc->shutdownPlugin(); }
	}
	~Lv2ProcSuspender()
	{
		if (m_wasActive) { m_proc->initPlugin(); }
	}

	//! Return the processor's current plugin instance without suspending it
	static LilvInstance* instanceOf(Lv2Proc* proc)
	{
		return proc->m_instance;
	}

private:
	Lv2Proc* const m_proc;
	const bool m_wasActive;
};


} // namespace lmms

#endif // LMMS_HAVE_LV2

#endif // LMMS_LV2_PROC_SUSPENDER_H
