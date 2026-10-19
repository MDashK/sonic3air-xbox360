/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"

#include <lemon/program/Program.h>
#include <lemon/runtime/provider/NativizedOpcodeProvider.h>
#include <lemon/runtime/OpcodeExecUtils.h>
#include <lemon/runtime/RuntimeOpcodeContext.h>


namespace lemon
{
#if defined(PLATFORM_BIG_ENDIAN)
	// The nativized code uses byte swaps for memory that is stored in big endian, i.e. the emulated memory
	//  -> These hide the global swapBytes functions in here, as no swap is needed on a big-endian host
	FORCE_INLINE uint16 swapBytes16(uint16 value)  { return value; }
	FORCE_INLINE uint32 swapBytes32(uint32 value)  { return value; }
	FORCE_INLINE uint64 swapBytes64(uint64 value)  { return value; }
#endif

	#include "NativizedCode.inc"

#ifndef NATIVIZED_CODE_AVAILABLE
	void createNativizedCodeLookup(Nativizer::LookupDictionary& dict) {}
#endif
}
