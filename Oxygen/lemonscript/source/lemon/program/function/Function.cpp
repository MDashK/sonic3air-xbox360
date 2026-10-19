/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "lemon/pch.h"
#include "lemon/program/function/Function.h"


namespace lemon
{
	namespace detail
	{
		// Signature hashes are computed over the little-endian bytes of the data type hashes, so they are the same
		// on all platforms (they are compiled into scripts.bin, which gets built on a PC)
		uint32 getFNV1a_32_littleEndian(const uint32* values, size_t count)
		{
		#if defined(PLATFORM_BIG_ENDIAN)
			uint32 hash = rmx::startFNV1a_32();
			for (size_t i = 0; i < count; ++i)
			{
				const uint32 littleEndianValue = swapBytes32(values[i]);
				hash = rmx::addToFNV1a_32(hash, (const uint8*)&littleEndianValue, sizeof(uint32));
			}
			return hash;
		#else
			return rmx::getFNV1a_32((const uint8*)values, count * sizeof(uint32));
		#endif
		}

		uint32 getVoidSignatureHash()
		{
			uint32 value = PredefinedDataTypes::VOID.getDataTypeHash();
			return getFNV1a_32_littleEndian(&value, 1);
		}
	}


	void Function::SignatureBuilder::clear(const DataTypeDefinition& returnType)
	{
		mData.clear();
		mData.push_back(returnType.getDataTypeHash());
	}

	void Function::SignatureBuilder::addParameterType(const DataTypeDefinition& dataType)
	{
		mData.push_back(dataType.getDataTypeHash());
	}

	uint32 Function::SignatureBuilder::getSignatureHash()
	{
		uint32 hash = detail::getFNV1a_32_littleEndian(&mData[0], mData.size());
		while (hash == 0)		// That should be a really rare case anyway
		{
			mData.push_back(0xcd000000);		// Just add anything to get away from hash 0
			hash = detail::getFNV1a_32_littleEndian(&mData[0], mData.size());
		}
		return hash;
	}


	void Function::setParametersByTypes(const std::vector<const DataTypeDefinition*>& parameterTypes)
	{
		mParameters.clear();
		mParameters.resize(parameterTypes.size());
		for (size_t i = 0; i < parameterTypes.size(); ++i)
		{
			mParameters[i].mDataType = parameterTypes[i];
		}
		mSignatureHash = 0;
	}

	uint32 Function::getVoidSignatureHash()
	{
		static const uint32 signatureHash = detail::getVoidSignatureHash();
		return signatureHash;
	}

	void Function::addAliasName(const FlyweightString& name, bool isDeprecated)
	{
		if (mName != name)
		{
			if (!vectorContainsByPredicate(mAliasNames, [&name](const AliasName& alias) { return name == alias.mName; } ))
			{
				AliasName& newAliasName = vectorAdd(mAliasNames);
				newAliasName.mName = name;
				newAliasName.mIsDeprecated = isDeprecated;
			}
		}
	}

	uint32 Function::getSignatureHash() const
	{
		if (mSignatureHash == 0)
		{
			static SignatureBuilder builder;
			builder.clear(*mReturnType);
			for (const Parameter& parameter : mParameters)
				builder.addParameterType(*parameter.mDataType);
			mSignatureHash = builder.getSignatureHash();
		}
		return mSignatureHash;
	}

}
