/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <memory>
#include <iostream>

#include "AampWidevineDrmHelper.h"
#include "AampDRMutils.h"
#include "AampConfig.h"
#include "AampConstants.h"

static AampWidevineDrmHelperFactory widevine_helper_factory;

const std::string AampWidevineDrmHelper::WIDEVINE_OCDM_ID = "com.widevine.alpha";

bool AampWidevineDrmHelper::parsePssh(const uint8_t* initData, uint32_t initDataLen)
{
	this->mInitData.assign(initData, initData+initDataLen);

	// Extract key
	const char* keyIdBegin;
	uint8_t keyIdSize = 0u;
	uint8_t psshDataVer = initData[WIDEVINE_PSSH_DATA_VERSION_POSITION];
	bool ret = false;

	/*
		WV PSSH Header Format :  for version 0 and version 1
		readme : https://www.w3.org/TR/2014/WD-encrypted-media-20140828/cenc-format.html
		online parser : https://tools.axinom.com/generators/PsshBox 
		[ 4 bytes (total (header and data) size) + 4 bytes (type-PSSH)  + 
		1 byte (version) + 3 bytes (flags)  +
		16 bytes (system id) + 4 bytes (wv pssh data size) ]  

		WV PSSH Data format:

		Version 0:
		[ optinal field: [2 byte ( Author Id Indicator + Author Id ) ] + 
		2 bytes (keyId size indicator + keyid size) + ( keyId) + 
		2 bytes (content id size  indiacater + content id size) +  (content id)]

		Version 1:
		[4 byte (Number of Key Id) + 16 Byte (Key Id 1) +  16 Byte (Key Id 2) +... +
			4 byte (Data size) + (Data)  ]
	*/

	//AAMPLOG_INFO("%s:%d wv pssh data version - %u ", __FUNCTION__, __LINE__, psshDataVer);
	if (psshDataVer == 0)
	{
		uint32_t header = 0;
		if (initData[WIDEVINE_PSSH_KEYID_SIZE_OFFSET] == WIDEVINE_KEY_ID_SIZE_INDICATOR)
		{
			header = WIDEVINE_PSSH_KEYID_SIZE_OFFSET + 1; //Skip key Id size indicator
		}
		else if(initData[WIDEVINE_PSSH_KEYID_SIZE_OFFSET_WITH_AUTHOR] == WIDEVINE_KEY_ID_SIZE_INDICATOR)
		{
			header = WIDEVINE_PSSH_KEYID_SIZE_OFFSET_WITH_AUTHOR + 1; //Skip key Id size indicator
		}
		else
		{
			AAMPLOG_WARN("%s:%d WV Version: %u, Keyid indicator byte not found using default logic", __FUNCTION__, __LINE__, psshDataVer);
			header = WIDEVINE_PSSH_KEYID_SIZE_OFFSET + 1;  //pssh data in default format
		}

		keyIdSize = initData[header];
		keyIdBegin = reinterpret_cast<const char*>(initData + header + 1);
	}
	else if (psshDataVer == 1)
	{
		//PSSH version 1
		//8 byte BMFF box header + 4 byte Full box header + 16 (system id) +
		//4(KID Count) + 16 byte KID 1 + .. + 4 byte Data Size
		//TODO : Handle multiple key Id logic, right now we are choosing only first one if have multiple key Id
		uint32_t header = WIDEVINE_DASH_KEY_ID_OFFSET;
		keyIdSize = WIDEVINE_PSSH_VER1_KEY_ID_SIZE;
		keyIdBegin = reinterpret_cast<const char*>(initData + header);
	}
	else
	{
		AAMPLOG_ERR("%s:%d Unsupported PSSH version: %u", __FUNCTION__, __LINE__, psshDataVer);
	}

	if (keyIdSize != 0u)
	{
		mKeyID.assign(keyIdBegin, keyIdBegin + keyIdSize);
		ret = true;
	}
	else
	{
		AAMPLOG_INFO("%s:%d WV version: %u, KeyIdlen: %u", __FUNCTION__, __LINE__, psshDataVer, keyIdSize);
	}

	return ret;
}


void AampWidevineDrmHelper::setDrmMetaData(const std::string& metaData)
{
	mContentMetadata = metaData;
}


const std::string& AampWidevineDrmHelper::ocdmSystemId() const
{
	return WIDEVINE_OCDM_ID;
}

void AampWidevineDrmHelper::createInitData(std::vector<uint8_t>& initData) const
{
	initData = this->mInitData;
}

void AampWidevineDrmHelper::getKey(std::vector<uint8_t>& keyID) const
{
	keyID = this->mKeyID;
}

void AampWidevineDrmHelper::generateLicenseRequest(const AampChallengeInfo& challengeInfo, AampLicenseRequest& licenseRequest) const
{
	licenseRequest.method = AampLicenseRequest::POST;

	if (licenseRequest.url.empty())
	{
		licenseRequest.url = challengeInfo.url;
	}

	licenseRequest.headers = {{"Content-Type:", {"application/octet-stream"}}};

	licenseRequest.payload.assign(reinterpret_cast<const char *>(challengeInfo.data->getData()), challengeInfo.data->getDataLength());
}

bool AampWidevineDrmHelperFactory::isDRM(const struct DrmInfo& drmInfo) const
{
	return (((WIDEVINE_UUID == drmInfo.systemUUID) || (AampWidevineDrmHelper::WIDEVINE_OCDM_ID == drmInfo.keyFormat))
		&& ((drmInfo.mediaFormat == eMEDIAFORMAT_DASH) || (drmInfo.mediaFormat == eMEDIAFORMAT_HLS_MP4))
		);
}

std::shared_ptr<AampDrmHelper> AampWidevineDrmHelperFactory::createHelper(const struct DrmInfo& drmInfo) const
{
	if (isDRM(drmInfo))
	{
		return std::make_shared<AampWidevineDrmHelper>(drmInfo);
	}
	return NULL;
}

void AampWidevineDrmHelperFactory::appendSystemId(std::vector<std::string>& systemIds) const
{
	systemIds.push_back(WIDEVINE_UUID);
}

