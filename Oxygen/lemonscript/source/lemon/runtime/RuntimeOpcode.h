/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "lemon/program/Opcode.h"
#include "lemon/runtime/Runtime.h"


namespace lemon
{
	class Runtime;
	struct RuntimeOpcode;
	struct RuntimeOpcodeBuffer;
	struct RuntimeOpcodeContext;
	class ScriptFunction;


	typedef void(*ExecFunc)(const RuntimeOpcodeContext context);

	class API_EXPORT RuntimeOpcodeProvider
	{
	public:
		virtual bool buildRuntimeOpcode(RuntimeOpcodeBuffer& buffer, const Opcode* opcodes, int numOpcodesAvailable, int firstOpcodeIndex, int& outNumOpcodesConsumed, const Runtime& runtime, const ScriptFunction& function) = 0;
	};


	struct API_EXPORT RuntimeOpcodeBase
	{
	public:
		enum class Flag : uint8
		{
			CALL_IS_BASE_CALL		 = 0x20,	// For CALL opcodes only: It is a base call
			CALL_TARGET_RESOLVED	 = 0x40,	// For CALL opcodes only: Call target is already resolved and can be found in the parameter (as pointer)
			CALL_TARGET_RUNTIME_FUNC = 0x80		// For CALL opcodes only: Call target is resolved and is a RuntimeFunction, not a Function
		};

		ExecFunc mExecFunc;
		int32 mOffsetToNext = 0;
		Opcode::Type mOpcodeType = Opcode::Type::NOP;
		uint8 mSize = 0;
		BitFlagSet<Flag> mFlags;
		uint8 mSuccessiveHandledOpcodes = 0;	// Number of internally handled opcodes (i.e. not manipulating control flow) in a row from this one -- including this one, so if this is 0, the opcode is not handled
	};

#if defined(PLATFORM_BIG_ENDIAN)
	// Opcode parameters are often written with one type and read with another (e.g. written as int64,
	// read as int16 or uint32; written as a pointer, read as uint64). That relies on little-endian
	// byte order, so on big-endian hosts the parameter memory is kept in little-endian layout:
	// every access byte-swaps, and pointers are stored zero-extended to 8 bytes like on 64-bit hosts.
	namespace detail
	{
		template<size_t SIZE> struct UnsignedOfSize;
		template<> struct UnsignedOfSize<1> { typedef uint8  Type; };
		template<> struct UnsignedOfSize<2> { typedef uint16 Type; };
		template<> struct UnsignedOfSize<4> { typedef uint32 Type; };
		template<> struct UnsignedOfSize<8> { typedef uint64 Type; };

		template<typename T> FORCE_INLINE T readLittleEndianParameter(const uint8* address)
		{
			if constexpr (std::is_pointer_v<T>)
			{
				return (T)(size_t)readLittleEndianParameter<uint64>(address);
			}
			else
			{
				typedef typename UnsignedOfSize<sizeof(T)>::Type U;
				U raw;
				memcpy(&raw, address, sizeof(T));
				if constexpr (sizeof(T) > 1)
					raw = rmx::swapBytes(raw);
				T value;
				memcpy(&value, &raw, sizeof(T));
				return value;
			}
		}

		template<typename T> FORCE_INLINE void writeLittleEndianParameter(uint8* address, T value)
		{
			if constexpr (std::is_pointer_v<T>)
			{
				writeLittleEndianParameter<uint64>(address, (uint64)(size_t)value);
			}
			else
			{
				typedef typename UnsignedOfSize<sizeof(T)>::Type U;
				U raw;
				memcpy(&raw, &value, sizeof(T));
				if constexpr (sizeof(T) > 1)
					raw = rmx::swapBytes(raw);
				memcpy(address, &raw, sizeof(T));
			}
		}
	}
#endif

	// Pointer to a value of the given byte size inside a 64-bit value slot (e.g. a global variable's
	// static memory): on big-endian hosts, the narrower value is in the slot's last bytes
	FORCE_INLINE uint8* getValuePointerInSlot(void* slot, size_t valueSize)
	{
	#if defined(PLATFORM_BIG_ENDIAN)
		return (uint8*)slot + (8 - valueSize);
	#else
		return (uint8*)slot;
	#endif
	}


	struct API_EXPORT RuntimeOpcode : public RuntimeOpcodeBase
	{
	public:
		static const constexpr size_t PARAMETER_OFFSET = sizeof(RuntimeOpcodeBase);

	#if defined(PLATFORM_BIG_ENDIAN)
		template<typename T> FORCE_INLINE T getParameter() const
		{
			return detail::readLittleEndianParameter<T>((const uint8*)this + PARAMETER_OFFSET);
		}

		template<typename T> FORCE_INLINE T getParameter(size_t offset) const
		{
			return detail::readLittleEndianParameter<T>((const uint8*)this + PARAMETER_OFFSET + offset);
		}

		template<typename T> void setParameter(T value)
		{
			detail::writeLittleEndianParameter<T>((uint8*)this + PARAMETER_OFFSET, value);
		}

		template<typename T> void setParameter(T value, size_t offset)
		{
			detail::writeLittleEndianParameter<T>((uint8*)this + PARAMETER_OFFSET + offset, value);
		}
	#else
		template<typename T> FORCE_INLINE T getParameter() const
		{
			return *reinterpret_cast<const T*>((uint8*)this + PARAMETER_OFFSET);
		}

		template<typename T> FORCE_INLINE T getParameter(size_t offset) const
		{
			return *reinterpret_cast<const T*>((uint8*)this + PARAMETER_OFFSET + offset);
		}

		template<typename T> void setParameter(T value)
		{
			*reinterpret_cast<T*>((uint8*)this + PARAMETER_OFFSET) = value;
		}

		template<typename T> void setParameter(T value, size_t offset)
		{
			*reinterpret_cast<T*>((uint8*)this + PARAMETER_OFFSET + offset) = value;
		}
	#endif

		FORCE_INLINE const RuntimeOpcode* getNextOpcode() const  { return (const RuntimeOpcode*)((uint8*)this + mOffsetToNext); }
	};

}
