/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
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


/**
* @file AampDRMSessionManager.cpp
* @brief Source file for DrmSessionManager.
*/

#include "AampDRMSessionManager.h"
#include "priv_aamp.h"
#include <pthread.h>
#include "_base64.h"
#include <iostream>
#include <uuid/uuid.h>

//#define LOG_TRACE 1
#define COMCAST_LICENCE_REQUEST_HEADER_ACCEPT "Accept: application/vnd.xcal.mds.licenseResponse+json; version=1"
#define COMCAST_LICENCE_REQUEST_HEADER_CONTENT_TYPE "Content-Type: application/vnd.xcal.mds.licenseRequest+json; version=1"
#define LICENCE_RESPONSE_JSON_LICENCE_KEY "license\":\""
#ifdef USE_SECCLIENT
#define COMCAST_QA_DRM_LICENCE_SERVER_URL "mds-qa.ccp.xcal.tv"
#define COMCAST_DRM_LICENCE_SERVER_URL "mds.ccp.xcal.tv"
#define COMCAST_ROGERS_DRM_LICENCE_SERVER_URL "mds-rogers.ccp.xcal.tv"
#else
#define COMCAST_QA_DRM_LICENCE_SERVER_URL "https://mds-qa.ccp.xcal.tv/license"
#define COMCAST_DRM_LICENCE_SERVER_URL "https://mds.ccp.xcal.tv/license"
#define COMCAST_ROGERS_DRM_LICENCE_SERVER_URL "https://mds-rogers.ccp.xcal.tv/license"
#endif
#define COMCAST_DRM_METADATA_TAG_START "<ckm:policy xmlns:ckm=\"urn:ccp:ckm\">"
#define COMCAST_DRM_METADATA_TAG_END "</ckm:policy>"
#define SESSION_TOKEN_URL "http://localhost:50050/authService/getSessionToken"
#define MAX_LICENSE_REQUEST_ATTEMPTS 2

static const char *sessionTypeName[] = {"video", "audio"};
DrmSessionContext AampDRMSessionManager::drmSessionContexts[MAX_DRM_SESSIONS] = {{dataLength : 0, data : NULL, drmSession : NULL}																		,{dataLength : 0, data : NULL, drmSession : NULL}};
KeyID AampDRMSessionManager::cachedKeyIDs[MAX_DRM_SESSIONS] = {{len : 0, data : NULL, creationTime : 0},{len : 0, data : NULL, creationTime : 0}};

char* AampDRMSessionManager::accessToken = NULL;
int AampDRMSessionManager::accessTokenLen = 0;
SessionMgrState AampDRMSessionManager::sessionMgrState = SessionMgrState::eSESSIONMGR_ACTIVE;

static pthread_mutex_t accessTokenMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t session_mutex[2] = {PTHREAD_MUTEX_INITIALIZER,PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t initDataMutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef USE_SECCLIENT
/**
 *  @brief Get formatted URL of license server
 *
 *  @param[in] url URL of license server
 *  @return		formatted url for secclient license acqusition.
 */
static string getFormattedLicenseServerURL(string url)
{
	size_t startpos = 0;
	size_t endpos, len;
	endpos = len = url.size();

	if (memcmp(url.data(), "https://", 8) == 0)
	{
		startpos += 8;
	}
	else if (memcmp(url.data(), "http://", 7) == 0)
	{
		startpos += 7;
	}

	if (startpos != 0)
	{
		endpos = url.find('/', startpos);
		if (endpos != string::npos)
		{
			len = endpos - startpos;
		}
	}

	return url.substr(startpos, len);
}
#endif

/**
 *  @brief      AampDRMSessionManager constructor.
 */
AampDRMSessionManager::AampDRMSessionManager()
{
}

/**
 *  @brief      AampDRMSessionManager Destructor.
 */
AampDRMSessionManager::~AampDRMSessionManager()
{
}

/**
 *  @brief		Clean up the memory used by session variables.
 *
 *  @return		void.
 */
void AampDRMSessionManager::clearSessionData()
{
	for(int i = 0 ; i < MAX_DRM_SESSIONS; i++)
	{
		if(drmSessionContexts[i].drmSession != NULL)
		{
			drmSessionContexts[i].drmSession->clearDecryptContext();
			delete drmSessionContexts[i].data;
			drmSessionContexts[i].data = NULL;
			drmSessionContexts[i].dataLength = 0;
		}
		if(cachedKeyIDs[i].data != NULL)
		{
			delete cachedKeyIDs[i].data;
			cachedKeyIDs[i].data = NULL;
			cachedKeyIDs[i].len = 0;
		}
	}
}

/**
 * @brief	Set Session manager state
 * @param	state
 * @return	void.
 */
void AampDRMSessionManager::setSessionMgrState(SessionMgrState state)
{
	pthread_mutex_lock(&initDataMutex);
	sessionMgrState = state;
	pthread_mutex_unlock(&initDataMutex);
}


/**
 * @brief	Clean up the failed keyIds.
 *
 * @return	void.
 */
void AampDRMSessionManager::clearFailedKeyIds()
{
	pthread_mutex_lock(&initDataMutex);
	for(int i = 0 ; i < MAX_DRM_SESSIONS; i++)
	{
		if(cachedKeyIDs[i].data != NULL && cachedKeyIDs[i].isFailedKeyId)
		{
			delete cachedKeyIDs[i].data;
			cachedKeyIDs[i].data = NULL;
			cachedKeyIDs[i].len = 0;
			cachedKeyIDs[i].isFailedKeyId = false;
		}
	}
	pthread_mutex_unlock(&initDataMutex);
}

/**
 *  @brief		Clean up the memory for accessToken.
 *
 *  @return		void.
 */
void AampDRMSessionManager::clearAccessToken()
{
	pthread_mutex_lock(&accessTokenMutex);
	if(accessToken)
	{
		free(accessToken);
		accessToken = NULL;
		accessTokenLen = 0;
	}
	pthread_mutex_unlock(&accessTokenMutex);
}

/**
 *  @brief		Curl write callback, used to get the curl o/p
 *  			from DRM license, accessToken curl requests.
 *
 *  @param[in]	ptr - Pointer to received data.
 *  @param[in]	size, nmemb - Size of received data (size * nmemb).
 *  @param[out]	userdata - Pointer to buffer where the received data is copied.
 *  @return		returns the number of bytes processed.
 */
size_t AampDRMSessionManager::write_callback(char *ptr, size_t size,
		size_t nmemb, void *userdata)
{
	DrmData *data = (DrmData *)userdata;
	size_t numBytesForBlock = size * nmemb;
	if (NULL == data->getData())
	{
		data->setData((unsigned char *) ptr, numBytesForBlock);
	}
	else
	{
		data->addData((unsigned char *) ptr, numBytesForBlock);
	}
	if (gpGlobalConfig->logging.trace)
	{
		logprintf("%s:%d wrote %zu number of blocks\n", __FUNCTION__, __LINE__, numBytesForBlock);
	}
	return numBytesForBlock;
}

/**
 *  @brief		Extract substring between (excluding) two string delimiters.
 *
 *  @param[in]	parentStr - Parent string from which substring is extracted.
 *  @param[in]	startStr, endStr - String delimiters.
 *  @return		Returns the extracted substring; Empty string if delimiters not found.
 */
string _extractSubstring(string parentStr, string startStr, string endStr)
{
	string ret = "";
	int startPos = parentStr.find(startStr);
	if(string::npos != startPos)
	{
		int offset = strlen(startStr.c_str());
		int endPos = parentStr.find(endStr, startPos + offset + 1);
		if(string::npos != endPos)
		{
			ret = parentStr.substr(startPos + offset, endPos - (startPos + offset));
		}
	}
	return ret;
}

/**
 *  @brief		Get the accessToken from authService.
 *
 *  @param[out]	tokenLen - Gets updated with accessToken length.
 *  @return		Pointer to accessToken.
 *  @note		AccessToken memory is dynamically allocated, deallocation
 *				should be handled at the caller side.
 */
const char * AampDRMSessionManager::getAccessToken(int * tokenLen)
{
	if(accessToken == NULL)
	{
		DrmData * tokenReply = new DrmData();
		CURLcode res;
		long httpCode = -1;

		CURL *curl = curl_easy_init();;
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, tokenReply);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_URL, SESSION_TOKEN_URL);

		res = curl_easy_perform(curl);

		if (res == CURLE_OK)
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
			if (httpCode == 200 || httpCode == 206)
			{
				string tokenReplyStr = string(reinterpret_cast<char*>(tokenReply->getData()));
				string tokenStatusCode = _extractSubstring(tokenReplyStr, "status\":", ",\"");
				if(tokenStatusCode.length() == 0)
				{
					//StatusCode could be last element in the json
					tokenStatusCode = _extractSubstring(tokenReplyStr, "status\":", "}");
				}
				if(tokenStatusCode.length() == 1 && tokenStatusCode.c_str()[0] == '0')
				{
					string token = _extractSubstring(tokenReplyStr, "token\":\"", "\"");
					if(token.length() != 0)
					{
						accessToken = (char*)calloc(token.length()+1, sizeof(char));
						accessTokenLen = token.length();
						strncpy(accessToken,token.c_str(),token.length());
						logprintf("%s:%d Received session token from auth service \n", __FUNCTION__, __LINE__);
					}
					else
					{
						logprintf("%s:%d Could not get access token from session token reply\n", __FUNCTION__, __LINE__);
					}
				}
				else
				{
					logprintf("%s:%d Missing or invalid status code in session token reply\n", __FUNCTION__, __LINE__);
				}
			}
			else
			{
				logprintf("%s:%d Get Session token call failed with http error %d\n", __FUNCTION__, __LINE__, httpCode);
			}
		}
		else
		{
			logprintf("%s:%d Get Session token call failed with curl error %d\n", __FUNCTION__, __LINE__, res);
		}
		delete tokenReply;
		curl_easy_cleanup(curl);
	}

	*tokenLen = accessTokenLen;
	return accessToken;
}

/**
 * @brief Sleep for given milliseconds
 * @param milliseconds Time to sleep
 */
static void mssleep(int milliseconds)
{
	struct timespec req, rem;
	if (milliseconds > 0)
	{
		req.tv_sec = milliseconds / 1000;
		req.tv_nsec = (milliseconds % 1000) * 1000000;
		nanosleep(&req, &rem);
	}
}

/**
 *  @brief		Get DRM license key from DRM server.
 *
 *  @param[in]	keyChallenge - Structure holding license request and it's length.
 *  @param[in]	destinationURL - Destination url to which request is send.
 *  @param[out]	httpCode - Gets updated with http error; default -1.
 *  @param[in]	isComcastStream - Flag to indicate whether Comcast specific headers
 *  			are to be used.
 *  @return		Structure holding DRM license key and it's length; NULL and 0 if request fails
 *  @note		Memory for license key is dynamically allocated, deallocation
 *				should be handled at the caller side.
 */
DrmData * AampDRMSessionManager::getLicense(DrmData * keyChallenge,
		string destinationURL, long *httpCode, bool isComcastStream)
{

	*httpCode = -1;
	CURL *curl;
	CURLcode res;
	double totalTime = 0;
	struct curl_slist *headers = NULL;
	DrmData * keyInfo = new DrmData();
	const long challegeLength = keyChallenge->getDataLength();
	char* destURL = new char[destinationURL.length() + 1];
	curl = curl_easy_init();
	if(isComcastStream)
	{
		headers = curl_slist_append(headers, COMCAST_LICENCE_REQUEST_HEADER_ACCEPT);
		headers = curl_slist_append(headers, COMCAST_LICENCE_REQUEST_HEADER_CONTENT_TYPE);
		headers = curl_slist_append(headers, "Expect:");
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "AAMP/1.0.0");
	//	headers = curl_slist_append(headers, "X-MoneyTrace: trace-id=226c94fc4d-3535-4945-a173-61af53444a3d;parent-id=4557953636469444377;span-id=803972323171353973");
	}
	else
	{
	//	headers = curl_slist_append(headers, "Expect:");
	//	headers = curl_slist_append(headers, "Connection: Keep-Alive");
	//	headers = curl_slist_append(headers, "Content-Type:");
	//	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Linux; x86_64 GNU/Linux) AppleWebKit/601.1 (KHTML, like Gecko) Version/8.0 Safari/601.1 WPE");
		headers = curl_slist_append(headers,"Content-Type: text/xml; charset=utf-8");
	}
	strcpy((char*) destURL, destinationURL.c_str());

	//headers = curl_slist_append(headers, destURL);

	logprintf("%s:%d Sending license request to server : %s \n", __FUNCTION__, __LINE__, destinationURL.c_str());
	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, destURL);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, keyInfo);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, challegeLength);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS,(uint8_t * )keyChallenge->getData());
	unsigned int attemptCount = 0;
	while(attemptCount < MAX_LICENSE_REQUEST_ATTEMPTS)
	{
		attemptCount++;
		res = curl_easy_perform(curl);
		if (res != CURLE_OK)
		{
			logprintf("%s:%d curl_easy_perform() failed: %s\n", __FUNCTION__, __LINE__, curl_easy_strerror(res));
			logprintf("%s:%d acquireLicense FAILED! license request attempt : %d; response code : curl %d\n", __FUNCTION__, __LINE__, attemptCount, res);
			*httpCode = res;
			break;
		}
		else
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode);
			curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &totalTime);
			if (*httpCode != 200 && *httpCode != 206)
			{
				logprintf("%s:%d acquireLicense FAILED! license request attempt : %d; response code : http %d\n", __FUNCTION__, __LINE__, attemptCount, *httpCode);
				if(*httpCode >= 500 && *httpCode < 600
						&& attemptCount < MAX_LICENSE_REQUEST_ATTEMPTS && gpGlobalConfig->licenseRetryWaitTime > 0)
				{
					delete keyInfo;
					keyInfo = new DrmData();
					curl_easy_setopt(curl, CURLOPT_WRITEDATA, keyInfo);
					logprintf("%s:%d acquireLicense : Sleeping %d milliseconds before next retry.\n", __FUNCTION__, __LINE__, gpGlobalConfig->licenseRetryWaitTime);
					mssleep(gpGlobalConfig->licenseRetryWaitTime);
				}
				else
				{
					break;
				}
			}
			else
			{
				logprintf("%s:%d DRM Session Manager Received license data from server; Curl total time  = %.1f\n", __FUNCTION__, __LINE__, totalTime);
				logprintf("%s:%d acquireLicense SUCCESS! license request attempt %d; response code : http %d\n",__FUNCTION__, __LINE__, attemptCount, *httpCode);
				break;
			}
		}
	}

	delete destURL;
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return keyInfo;
}

/**
 *  @brief		Find the position of substring in cleaned PSSH data.
 *  			Cleaned PSSH data, is PSSH data from which empty bytes are removed.
 *  			Used while extracting keyId or content meta data from PSSH.
 *
 *  @param[in]	psshData - Pointer to cleaned PSSH data.
 *  @param[in]	dataLength - Length of cleaned PSSH data.
 *  @param[in]	pos - Search start position.
 *  @param[in]	subStr - Pointer to substring to be searched.
 *  @param[out]	substrStartPos - Default NULL; If not NULL, gets updated with end
 *  			position of the substring in Cleaned PSSH; -1 if not found.
 *  @return		Start position of substring in cleaned PSSH; -1 if not found.
 */
int _findSubstr(const char* psshData, int dataLength, int pos, const char* substr, int *substrStartPos = NULL)
{
	int subStrLen = strlen(substr);
	int psshIter = pos;
	int subStrIter = 0;
	bool strMatched = false;
	while (psshIter < dataLength - (subStrLen - 1))
	{
		if (psshData[psshIter] == substr[subStrIter])
		{
			if(substrStartPos && subStrIter == 0)
			{
				*substrStartPos = psshIter;
			}
			subStrIter++;
			if (subStrIter == subStrLen)
			{
				strMatched = true;
				break;
			}
		}
		else
		{
			subStrIter = 0;
		}
		psshIter++;
	}

	if(strMatched)
	{
		return psshIter;
	}
	else
	{
		if(substrStartPos)
		{
			*substrStartPos = -1;
		}
		return -1;
	}
}

/**
 *  @brief		Swap the bytes at given positions.
 *
 *  @param[in, out]	bytes - Pointer to byte block where swapping is done.
 *  @param[in]	pos1, pos2 - Swap positions.
 *  @return		void.
 */
void Swap(unsigned char *bytes, int pos1, int pos2)
{
	unsigned char temp = bytes[pos1];
	bytes[pos1] = bytes[pos2];
	bytes[pos2] = temp;
}

/**
 *  @brief		Convert endianness of 16 byte block.
 *
 *  @param[in]	original - Pointer to source byte block.
 *  @param[out]	guidBytes - Pointer to destination byte block.
 *  @return		void.
 */
void ConvertEndianness(unsigned char *original, unsigned char *guidBytes)
{
	memcpy(guidBytes, original, 16);
	Swap(guidBytes, 0, 3);
	Swap(guidBytes, 1, 2);
	Swap(guidBytes, 4, 5);
	Swap(guidBytes, 6, 7);
}

/**
 *  @brief		Extract the keyId from PSSH data.
 *  			Different procedures are used for PlayReady and WideVine.
 *
 *  @param[in]	psshData - Pointer to PSSH data.
 *  @param[in]	dataLength - Length of PSSH data.
 *  @param[out]	len - Gets updated with length of keyId.
 *  @param[in]	isWidevine - Flag to indicate WV.
 *  @return		Pointer to extracted keyId.
 *  @note		Memory for keyId is dynamically allocated, deallocation
 *				should be handled at the caller side.
 */
unsigned char * _extractKeyIdFromPssh(const char* psshData, int dataLength, int *len, bool isWidevine)
{
	unsigned char* key_id = NULL;

	if(isWidevine)
	{
		//The following 2 are for Widevine
		//PSSH version 0
		//4+4+4+16(system id)+4(data size)+2(unknown byte + keyid size)
		uint32_t header = 33;
		uint8_t  key_id_size = (uint8_t)psshData[header];
		key_id = (unsigned char*)malloc(key_id_size + 1);
		memset(key_id, 0, key_id_size + 1);
		strncpy(reinterpret_cast<char*>(key_id), psshData + header + 1, key_id_size);
		*len = (int)key_id_size;
		AAMPLOG_INFO("%s:%d wv keyid: %s keyIdlen: %d\n",__FUNCTION__, __LINE__, key_id, key_id_size);
		if(gpGlobalConfig->logging.trace)
		{
			for (int i = 0; i < key_id_size; ++i)
				logprintf("%2x",key_id[i]);
			logprintf("\n\n");
		}

	}else{

		int keyIdLen = 0;
		unsigned char *keydata = _extractDataFromPssh(psshData, dataLength, KEYID_TAG_START, KEYID_TAG_END, &keyIdLen);

		AAMPLOG_INFO("%s:%d pr keyid: %s keyIdlen: %d\n",__FUNCTION__, __LINE__, keydata, keyIdLen);

		size_t decodedDataLen = 0;
		unsigned char* decodedKeydata = base64_Decode((const char *) keydata, &decodedDataLen);
		if(decodedDataLen != 16)
		{
			logprintf("invalid key size found while extracting PR KeyID: %d\n", decodedDataLen);
			free (keydata);
			free (decodedKeydata);
			return NULL;
		}

		unsigned char *swappedKeydata = (unsigned char*)malloc(16);

		ConvertEndianness(decodedKeydata, swappedKeydata);

		key_id = (unsigned char *)calloc(37, sizeof(char));
		uuid_t *keyiduuid = (uuid_t *) swappedKeydata;
		uuid_unparse_lower(*keyiduuid, reinterpret_cast<char*>(key_id));

		*len = 37;

		free (keydata);
		free (decodedKeydata);
		free (swappedKeydata);
	}

	AAMPLOG_INFO("%s:%d KeyId : %s\n", __FUNCTION__, __LINE__,key_id);

	return key_id;
}

//4+4+4+16(system id)+4(data size)
/**
 *  @brief		Extract WideVine content meta data from Comcast DRM
 *  			Agnostic PSSH header. Might not work with WideVine PSSH header
 *
 *  @param[in]	Pointer to PSSH data.
 *  @param[in]	dataLength - Length of PSSH data.
 *  @param[out]	len - Gets updated with length of content meta data.
 *  @return		Extracted ContentMetaData.
 *  @note		Memory for ContentMetaData is dynamically allocated, deallocation
 *				should be handled at the caller side.
 */
unsigned char * _extractWVContentMetadataFromPssh(const char* psshData, int dataLength, int *len)
{
	uint32_t header = 28;
	unsigned char* content_id = NULL;
	uint32_t  content_id_size =
                    (uint32_t)((psshData[header] & 0x000000FFu) << 24 |
                               (psshData[header+1] & 0x000000FFu) << 16 |
                               (psshData[header+2] & 0x000000FFu) << 8 |
                               (psshData[header+3] & 0x000000FFu));

	AAMPLOG_INFO("%s:%d content meta data length  : %d\n", __FUNCTION__, __LINE__,content_id_size);

	content_id = (unsigned char*)malloc(content_id_size + 1);
	memset(content_id, 0, content_id_size + 1);
	strncpy(reinterpret_cast<char*>(content_id), psshData + header + 4, content_id_size);
//	logprintf("%s:%d content meta data : %s\n", __FUNCTION__, __LINE__,content_id);

	*len = (int)content_id_size;
	return content_id;
}
//End of special for Widevine

/**
 *  @brief		Extract content meta data or keyID from given PSSH data.
 *  			For example for content meta data,
 *  			When strings are given as "ckm:policy xmlns:ckm="urn:ccp:ckm"" and "ckm:policy"
 *  			<ckm:policy xmlns:ckm="urn:ccp:ckm">we need the contents from here</ckm:policy>
 *
 *  			PSSH is cleaned of empty bytes before extraction steps, since from manifest the
 *  			PSSH data is in 2 bytes. Data dump looking like below, so direct string comparison
 *  			would strstr fail.

 *				000003c0 (0x14d3c0): 3c 00 63 00 6b 00 6d 00 3a 00 70 00 6f 00 6c 00  <.c.k.m.:.p.o.l.
 *				000003d0 (0x14d3d0): 69 00 63 00 79 00 20 00 78 00 6d 00 6c 00 6e 00  i.c.y. .x.m.l.n.
 *				000003e0 (0x14d3e0): 73 00 3a 00 63 00 6b 00 6d 00 3d 00 22 00 75 00  s.:.c.k.m.=.".u.
 *				000003f0 (0x14d3f0): 72 00 6e 00 3a 00 63 00 63 00 70 00 3a 00 63 00  r.n.:.c.c.p.:.c.
 *				00000400 (0x14d400): 6b 00 6d 00 22 00 3e 00 65 00 79 00 4a 00 34 00  k.m.".>.e.y.J.4.
 *				00000410 (0x14d410): 4e 00 58 00 51 00 6a 00 55 00 7a 00 49 00 31 00  N.X.Q.j.U.z.I.1.
 *				00000420 (0x14d420): 4e 00 69 00 49 00 36 00 49 00 6c 00 64 00 51 00  N.i.I.6.I.l.d.Q.
 *
 *  @param[in]	Pointer to PSSH data.
 *  @param[in]	dataLength - Length of PSSH data.
 *  @param[in]	startStr, endStr - Pointer to delimiter strings.
 *  @param[out]	len - Gets updated with length of content meta data.
 *  @return		Extracted data between delimiters; NULL if not found.
 *  @note		Memory of returned data is dynamically allocated, deallocation
 *				should be handled at the caller side.
 */
unsigned char * _extractDataFromPssh(const char* psshData, int dataLength,
		const char* startStr, const char* endStr, int *len) {
	int endPos = -1;
	int startPos = -1;
	unsigned char* contentMetaData = NULL;

	//Clear the 00  bytes
	char* cleanedPssh = (char*) malloc(dataLength);
	int cleanedPsshLen = 0;
	for(int itr = 0; itr < dataLength; itr++)
	{
		if(psshData[itr] != 0)
		{
			//cout<<psshData[itr];
			cleanedPssh[cleanedPsshLen++] = psshData[itr];
		}
	}

	startPos = _findSubstr(cleanedPssh, cleanedPsshLen, 0, startStr);

	if(startPos >= 0)
	{
		_findSubstr(cleanedPssh, cleanedPsshLen, startPos, endStr, &endPos);
		if(endPos > 0 && startPos < endPos)
		{
			*len = endPos - startPos - 1;
			contentMetaData = (unsigned char*)malloc(*len + 1);
			memset(contentMetaData, 0, *len + 1);
			strncpy(reinterpret_cast<char*>(contentMetaData),reinterpret_cast<char*>(cleanedPssh + startPos + 1), *len);
			//logprintf("%s:%d Content Meta data length  : %d\n", __FUNCTION__, __LINE__,*len);
		}
	}
	free(cleanedPssh);
	return contentMetaData;
}

/**
 *  @brief		Overloaded version of createDrmSession where contentMetadataPtr is not there
 *  			Called from gstaampopencdmiplugins.
 *
 *  @param[in]	systemId - UUID of the DRM system.
 *  @param[in]	initDataPtr - Pointer to PSSH data.
 *  @param[in]	dataLength - Length of PSSH data.
 *  @param[in]	streamType - Whether audio or video.
 *  @param[in]	aamp - Pointer to PrivateInstanceAAMP, for DRM related profiling.
 *  @param[out]	error_code - Gets updated with proper error code, if session creation fails.
 *  			No NULL checks are done for error_code, caller should pass a valid pointer.
 *  @return		Pointer to DrmSession for the given PSSH data; NULL if session creation/mapping fails.
 */
AampDrmSession * AampDRMSessionManager::createDrmSession(
		const char* systemId, const unsigned char * initDataPtr,
		uint16_t dataLength, MediaType streamType, PrivateInstanceAAMP* aamp,  AAMPEvent *e)
{
	return createDrmSession(systemId, initDataPtr, dataLength, streamType, NULL, aamp, e);
}

/**
 *  @brief		Creates and/or returns the DRM session corresponding to keyId (Present in initDataPtr)
 *  			AampDRMSession manager has two static AampDrmSession objects.
 *  			This method will return the existing DRM session pointer if any one of these static
 *  			DRM session objects are created against requested keyId. Binds the oldest DRM Session
 *  			with new keyId if no matching keyId is found in existing sessions.
 *
 *  @param[in]	systemId - UUID of the DRM system.
 *  @param[in]	initDataPtr - Pointer to PSSH data.
 *  @param[in]	dataLength - Length of PSSH data.
 *  @param[in]	streamType - Whether audio or video.
 *  @param[in]	contentMetadataPtr - Pointer to content meta data, when content meta data
 *  			is already extracted during manifest parsing. Used when content meta data
 *  			is available as part of another PSSH header, like Comcast DRM Agnostic PSSH
 *  			header.
 *  @param[in]	aamp - Pointer to PrivateInstanceAAMP, for DRM related profiling.
 *  @param[out]	error_code - Gets updated with proper error code, if session creation fails.
 *  			No NULL checks are done for error_code, caller should pass a valid pointer.
 *  @return		Pointer to DrmSession for the given PSSH data; NULL if session creation/mapping fails.
 */
AampDrmSession * AampDRMSessionManager::createDrmSession(
		const char* systemId, const unsigned char * initDataPtr,
		uint16_t dataLength, MediaType streamType,
		const unsigned char* contentMetadataPtr, PrivateInstanceAAMP* aamp, AAMPEvent *e)
{
	KeyState code = KEY_CLOSED;
	long responseCode = -1;
	unsigned char * contentMetaData = NULL;
	int contentMetaDataLen = 0;
	unsigned char *keyId = NULL;
	int keyIdLen = 0;
	string destinationURL;
	DrmData * key = NULL;

	if(gpGlobalConfig->logging.debug)
	{
		logprintf("%s:%d Received DRM Session request, Init Data length  : %d\n", __FUNCTION__, __LINE__,dataLength);
		logprintf("%s:%d Printing InitData from %s stream \n", __FUNCTION__, __LINE__,systemId,sessionTypeName[streamType]);
		for (int i = 0; i < dataLength; ++i)
			cout <<(char)initDataPtr[i];
		cout << endl;
	}
	int sessionType = 0;
	e->data.dash_drmmetadata.accessStatus = "accessAttributeStatus";
        e->data.dash_drmmetadata.accessStatus_value = 3;

	if(eMEDIATYPE_AUDIO == streamType)
	{
		sessionType = AUDIO_SESSION;
	}
	else if (eMEDIATYPE_VIDEO == streamType)
	{
		sessionType = VIDEO_SESSION;
	}
	else
	{
		e->data.dash_drmmetadata.failure = AAMP_TUNE_UNSUPPORTED_STREAM_TYPE;
		return NULL;
	}

	const char *keySystem = NULL;
	bool isWidevine = false;
	logprintf("%s:%d systemId is %s \n", __FUNCTION__, __LINE__, systemId);
	if (!strncmp(systemId, PLAYREADY_PROTECTION_SYSTEM_ID, sizeof(PLAYREADY_PROTECTION_SYSTEM_ID)))
	{
		AAMPLOG_INFO("%s:%d [HHH]systemId is PLAYREADY\n", __FUNCTION__, __LINE__);
#ifdef USE_SECCLIENT
		keySystem = SEC_CLIENT_PLAYREADY_KEYSYSTEMID;
#else
		keySystem = PLAYREADY_KEY_SYSTEM_STRING;
#endif
	}
	else if (!strncmp(systemId, WIDEVINE_PROTECTION_SYSTEM_ID, sizeof(WIDEVINE_PROTECTION_SYSTEM_ID)))
	{
#ifdef USE_SECCLIENT
		keySystem = SEC_CLIENT_WIDEVINE_KEYSYSTEMID;
#else
		keySystem = WIDEVINE_KEY_SYSTEM_STRING;
#endif
		AAMPLOG_INFO("%s:%d [HHH]systemId is Widevine\n", __FUNCTION__, __LINE__);
		isWidevine = true;
	}
	else
	{
		logprintf("Unsupported systemid: %s !\n", systemId);
	}
	logprintf("keysystem is %s\n", keySystem);

	keyId = _extractKeyIdFromPssh(reinterpret_cast<const char*>(initDataPtr),dataLength, &keyIdLen, isWidevine);

	if (keyId == NULL)
	{
		logprintf("%s:%d Key Id not found in initdata\n", __FUNCTION__, __LINE__);
		e->data.dash_drmmetadata.failure = AAMP_TUNE_FAILED_TO_GET_KEYID;
		return NULL;
	}

	pthread_mutex_lock(&initDataMutex);

	if(SessionMgrState::eSESSIONMGR_INACTIVE == sessionMgrState)
	{
		AAMPLOG_INFO("%s:%d SessionManager state inactive, aborting request", __FUNCTION__, __LINE__);
		free(keyId);
		pthread_mutex_unlock(&initDataMutex);
		return NULL;
	}
	/*Check if audio/video session for keyid already exists or is in progress of being created
	*Else clear oldest session and create new one
	*/
	int otherSession = (sessionType + 1) % 2;
	bool sessionFound = true;
	if (keyIdLen == cachedKeyIDs[otherSession].len && 0 == memcmp(cachedKeyIDs[otherSession].data, keyId, keyIdLen))
	{
		if(gpGlobalConfig->logging.debug)
		{
			logprintf("%s:%d %s session (created/inprogress) with same keyID %s, can reuse same for %s\n",
					__FUNCTION__, __LINE__, sessionTypeName[otherSession], keyId, sessionTypeName[sessionType]);
		}
		sessionType = otherSession;
	}
	else if(!(keyIdLen == cachedKeyIDs[sessionType].len && 0 == memcmp(cachedKeyIDs[sessionType].data, keyId, keyIdLen)))
	{
		logprintf("%s:%d No active session found with keyId %s, proceeding to create new session\n", __FUNCTION__, __LINE__, keyId);
		sessionFound = false;
		/*
		 * Check if both sessions have valid KeyIDs
		 * if yes proceed to clear the oldest session
		 */
		if(cachedKeyIDs[otherSession].creationTime < cachedKeyIDs[sessionType].creationTime)
		{
			sessionType = otherSession;
		}

		if(cachedKeyIDs[sessionType].data != NULL)
		{
			delete cachedKeyIDs[sessionType].data;
		}
		
		cachedKeyIDs[sessionType].len = keyIdLen;
		cachedKeyIDs[sessionType].isFailedKeyId = false;
		cachedKeyIDs[sessionType].data = new unsigned char[keyIdLen];
		memcpy(reinterpret_cast<void*>(cachedKeyIDs[sessionType].data),
        reinterpret_cast<const void*>(keyId), keyIdLen);
		cachedKeyIDs[sessionType].creationTime = aamp_GetCurrentTimeMS();
	}
	pthread_mutex_unlock(&initDataMutex);

	pthread_mutex_lock(&session_mutex[sessionType]);
	aamp->profiler.ProfileBegin(PROFILE_BUCKET_LA_PREPROC);
	//logprintf("%s:%d Locked session mutex for %s\n", __FUNCTION__, __LINE__, sessionTypeName[sessionType]);
	if(drmSessionContexts[sessionType].drmSession == NULL)
	{
		drmSessionContexts[sessionType].drmSession = AampDrmSessionFactory::GetDrmSession(systemId);
	}
	else if(drmSessionContexts[sessionType].drmSession->getKeySystem() != string(keySystem))
	{
		AAMPLOG_WARN("%s:%d Switching DRM from %s to %s\n", __FUNCTION__, __LINE__, drmSessionContexts[sessionType].drmSession->getKeySystem().c_str(), keySystem);
		delete drmSessionContexts[sessionType].drmSession;
		drmSessionContexts[sessionType].drmSession = AampDrmSessionFactory::GetDrmSession(systemId);
	}
	else
	{
			if(keyIdLen == drmSessionContexts[sessionType].dataLength)
			{
				if ((0 == memcmp(drmSessionContexts[sessionType].data, keyId, keyIdLen))
						&& (drmSessionContexts[sessionType].drmSession->getState()
								== KEY_READY))
				{
					AAMPLOG_INFO("%s:%d Found drm session READY with same keyID %s - Reusing drm session for %s\n",
								__FUNCTION__, __LINE__, keyId, sessionTypeName[streamType]);
					pthread_mutex_unlock(&session_mutex[sessionType]);
					free(keyId);
					return drmSessionContexts[sessionType].drmSession;
				}
			}

			if(sessionFound && NULL == contentMetadataPtr)
			{
				AAMPLOG_INFO("%s:%d Aborting session creation for keyId %s: StreamType %s, since previous try failed\n",
								__FUNCTION__, __LINE__, keyId, sessionTypeName[streamType]);
				cachedKeyIDs[sessionType].isFailedKeyId = true;
				pthread_mutex_unlock(&session_mutex[sessionType]);
				free(keyId);
				return NULL;
			}
			drmSessionContexts[sessionType].drmSession->clearDecryptContext();
	}

	if(drmSessionContexts[sessionType].drmSession)
	{
		code = drmSessionContexts[sessionType].drmSession->getState();
	}
	if (code != KEY_INIT)
	{
		logprintf("%s:%d DRM initialization failed : Key State %d \n", __FUNCTION__, __LINE__, code);
		pthread_mutex_unlock(&session_mutex[sessionType]);
		free(keyId);
		e->data.dash_drmmetadata.failure = AAMP_TUNE_DRM_INIT_FAILED;
		return NULL;
	}


	drmSessionContexts[sessionType].drmSession->generateAampDRMSession(initDataPtr, dataLength);
	code = drmSessionContexts[sessionType].drmSession->getState();
	if (code != KEY_INIT)
	{
		logprintf("%s:%d DRM init data binding failed: Key State %d \n", __FUNCTION__, __LINE__, code);
		pthread_mutex_unlock(&session_mutex[sessionType]);
		free(keyId);
		e->data.dash_drmmetadata.failure = AAMP_TUNE_DRM_DATA_BIND_FAILED;
		return NULL;
	}

	DrmData * licenceChallenge = drmSessionContexts[sessionType].drmSession->aampGenerateKeyRequest(
			destinationURL);
	code = drmSessionContexts[sessionType].drmSession->getState();
	if (code == KEY_PENDING)
	{
		aamp->profiler.ProfileEnd(PROFILE_BUCKET_LA_PREPROC);
		//license request logic here
		if (gpGlobalConfig->logging.debug)
		{
			logprintf("%s:%d Licence challenge from DRM  : length = %d \n",
						__FUNCTION__, __LINE__, licenceChallenge->getDataLength());
		}

#ifdef LOG_TRACE
		logprintf("\n\n%s:%d Licence challenge = \n\n", __FUNCTION__, __LINE__);
		unsigned char * data = licenceChallenge->getData();
		for (int i = 0; i < licenceChallenge->getDataLength(); ++i)
			cout << data[i];
		cout << endl;
#endif

		if (contentMetadataPtr)
		{
			contentMetaDataLen = strlen((const char*)contentMetadataPtr);
			contentMetaData = (unsigned char *)malloc(contentMetaDataLen + 1);
			memset(contentMetaData, 0, contentMetaDataLen + 1);
			strncpy(reinterpret_cast<char*>(contentMetaData), reinterpret_cast<const char*>(contentMetadataPtr), contentMetaDataLen);
			logprintf("%s:%d [HHH]contentMetaData length=%d\n", __FUNCTION__, __LINE__, contentMetaDataLen);
		}
		//For WV _extractWVContentMetadataFromPssh() won't work at this point
		//Since the content meta data is with Agnostic DRM PSSH.
		else if (!isWidevine)
		{
				contentMetaData = _extractDataFromPssh(reinterpret_cast<const char*>(initDataPtr),dataLength,COMCAST_DRM_METADATA_TAG_START, COMCAST_DRM_METADATA_TAG_END, &contentMetaDataLen);
		}

		bool isComcastStream = false;

		char *externLicenseServerURL = NULL;
		if (gpGlobalConfig->prLicenseServerURL && !isWidevine)
		{
			externLicenseServerURL = gpGlobalConfig->prLicenseServerURL;
		}
		else if (gpGlobalConfig->wvLicenseServerURL && isWidevine)
		{
			externLicenseServerURL = gpGlobalConfig->wvLicenseServerURL;
		}
		else if (gpGlobalConfig->licenseServerURL)
		{
			externLicenseServerURL = gpGlobalConfig->licenseServerURL;
		}


		if(contentMetaData)
		{
			/*
				Constuct the licence challenge in the form of JSON message which can be parsed by MDS server
				For the time keySystem and mediaUsage are constants
				licenceChallenge from drm and contentMetadata are to be b64 encoded in the JSON
			*/
			logprintf("%s:%d MDS server spcific conent metadata found in initdata\n", __FUNCTION__, __LINE__);

#ifdef LOG_TRACE
			logprintf("\n\n%s:%d ContentMetaData = \n\n", __FUNCTION__, __LINE__);
			for (int i = 0; i < contentMetaDataLen; ++i)
			{
				cout << (char)contentMetaData[i];
			}
			cout<<endl;
#endif
			GrowableBuffer comChallenge = {0,0,0};
			const char * availableFields = "{\"keySystem\":\"playReady\",\"mediaUsage\":\"stream\",\"licenseRequest\":\"";
			aamp_AppendBytes(&comChallenge, availableFields, strlen(availableFields));

			char *licenseRequest = base64_Encode(licenceChallenge->getData(),licenceChallenge->getDataLength());
			delete licenceChallenge;
			aamp_AppendBytes(&comChallenge, licenseRequest, strlen(licenseRequest));
			aamp_AppendBytes(&comChallenge,"\",\"contentMetadata\":\"", strlen("\",\"contentMetadata\":\""));
			char * encodedData = base64_Encode(contentMetaData,contentMetaDataLen);
			free(contentMetaData);
			aamp_AppendBytes(&comChallenge, encodedData,strlen(encodedData));

			pthread_mutex_lock(&accessTokenMutex);
			int tokenLen = 0;
			const char * sessionToken = getAccessToken(&tokenLen);
			const char * secclientSessionToken = NULL;
			pthread_mutex_unlock(&accessTokenMutex);
			if(sessionToken != NULL && !gpGlobalConfig->licenseAnonymousRequest)
			{
				logprintf("%s:%d access token is available\n", __FUNCTION__, __LINE__);
				aamp_AppendBytes(&comChallenge,"\",\"accessToken\":\"", strlen("\",\"accessToken\":\""));
				aamp_AppendBytes(&comChallenge, sessionToken, tokenLen);
				secclientSessionToken = sessionToken;
			}
			else
			{
				if(NULL == sessionToken)
				{
					e->data.dash_drmmetadata.failure = AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN;
				}
				logprintf("%s:%d Trying to get license without token\n", __FUNCTION__, __LINE__);
			}
			aamp_AppendBytes(&comChallenge, "\"}",strlen("\"}"));

#ifdef LOG_TRACE
			cout << systemId << endl << "Inside Session manager; printing Challenge : ";
			for (int i = 0; i < comChallenge.len; ++i)
				cout <<(char) comChallenge.ptr[i];
			cout << endl;
#endif
			licenceChallenge = new DrmData(reinterpret_cast<unsigned char*>(comChallenge.ptr),comChallenge.len);
			aamp_Free(&comChallenge.ptr);

			if (externLicenseServerURL)
			{
#ifdef USE_SECCLIENT
				destinationURL = getFormattedLicenseServerURL(string(externLicenseServerURL));
#else
				destinationURL = string(externLicenseServerURL);
#endif
			}
			else
			{
				if (string::npos != destinationURL.find("rogers.ccp.xcal.tv"))
				{
					destinationURL = string(COMCAST_ROGERS_DRM_LICENCE_SERVER_URL);
				}
				else if (string::npos != destinationURL.find("qa.ccp.xcal.tv"))
				{
					destinationURL = string(COMCAST_QA_DRM_LICENCE_SERVER_URL);
				}
				else if (string::npos != destinationURL.find("ccp.xcal.tv"))
				{
					destinationURL = string(COMCAST_DRM_LICENCE_SERVER_URL);
				}
			}
			isComcastStream = true;
			aamp->profiler.ProfileBegin(PROFILE_BUCKET_LA_NETWORK);
#ifdef USE_SECCLIENT
			const char *mediaUsage = "stream";

			int32_t sec_client_result = SEC_CLIENT_RESULT_FAILURE;
			char *licenseResponse = NULL;
			size_t licenseResponseLength = 2;
			uint32_t refreshDuration = 3;
			SecClient_ExtendedStatus statusInfo;
			const char *requestMetadata[1][2];
			std::string moneytracestr;
			requestMetadata[0][0] = "X-MoneyTrace";
			aamp->GetMoneyTraceString(moneytracestr);
			requestMetadata[0][1] = moneytracestr.c_str();			

			logprintf("[HHH] Before calling SecClient_AcquireLicense-----------\n");
			logprintf("destinationURL is %s\n", destinationURL.c_str());
			logprintf("MoneyTrace[%s]\n", requestMetadata[0][1]);
			//logprintf("encodedData is %s, length=%d\n", encodedData, strlen(encodedData));
			//logprintf("licenseRequest is %s\n", licenseRequest);
			logprintf("keySystem is %s\n", keySystem);
			//logprintf("mediaUsage is %s\n", mediaUsage);
			//logprintf("sessionToken is %s\n", sessionToken);
			unsigned int attemptCount = 0;
			while(attemptCount < MAX_LICENSE_REQUEST_ATTEMPTS)
			{
				attemptCount++;
				sec_client_result = SecClient_AcquireLicense(destinationURL.c_str(), 1,
									requestMetadata, 0, NULL,
									encodedData,
									strlen(encodedData),
									licenseRequest, strlen(licenseRequest), keySystem, mediaUsage,
									secclientSessionToken,
									&licenseResponse, &licenseResponseLength, &refreshDuration, &statusInfo);
				if (sec_client_result >= 500 && sec_client_result < 600
						&& attemptCount < MAX_LICENSE_REQUEST_ATTEMPTS && gpGlobalConfig->licenseRetryWaitTime > 0)
				{
					logprintf("%s:%d acquireLicense FAILED! license request attempt : %d; response code : sec_client %d\n", __FUNCTION__, __LINE__, attemptCount, sec_client_result);
					if (licenseResponse) SecClient_FreeResource(licenseResponse);
					logprintf("%s:%d acquireLicense : Sleeping %d milliseconds before next retry.\n", __FUNCTION__, __LINE__, gpGlobalConfig->licenseRetryWaitTime);
					mssleep(gpGlobalConfig->licenseRetryWaitTime);
				}
				else
				{
					break;
				}
			}

			if (gpGlobalConfig->logging.debug)
			{
				logprintf("licenseResponse is %s\n", licenseResponse);
				logprintf("licenseResponse len is %zd\n", licenseResponseLength);
				logprintf("accessAttributesStatus is %d\n", statusInfo.accessAttributeStatus);
				logprintf("refreshDuration is %d\n", refreshDuration);
			}

			if (sec_client_result != SEC_CLIENT_RESULT_SUCCESS)
			{
				logprintf("%s:%d acquireLicense FAILED! license request attempt : %d; response code : sec_client %d\n", __FUNCTION__, __LINE__, attemptCount, sec_client_result);
				responseCode = sec_client_result;
			}
			else
			{
				logprintf("%s:%d acquireLicense SUCCESS! license request attempt %d; response code : sec_client %d\n",__FUNCTION__, __LINE__, attemptCount, sec_client_result);
				e->type = AAMP_EVENT_DRM_METADATA;
                                e->data.dash_drmmetadata.accessStatus_value = statusInfo.accessAttributeStatus;
				key = new DrmData((unsigned char *)licenseResponse, licenseResponseLength);
			}
			if (licenseResponse) SecClient_FreeResource(licenseResponse);
#else
			logprintf("%s:%d License request ready for %s stream\n", __FUNCTION__, __LINE__, sessionTypeName[streamType]);
			key = getLicense(licenceChallenge, destinationURL, &responseCode, isComcastStream);
#endif
			free(licenseRequest);
			free(encodedData);

		}
		else 
		{
			if (externLicenseServerURL)
			{
				destinationURL = string(externLicenseServerURL);
			}
			logprintf("%s:%d License request ready for %s stream\n", __FUNCTION__, __LINE__, sessionTypeName[streamType]);
			aamp->profiler.ProfileBegin(PROFILE_BUCKET_LA_NETWORK);
			key = getLicense(licenceChallenge, destinationURL, &responseCode ,isComcastStream);
		}

		if(key != NULL && key->getDataLength() != 0)
		{
			aamp->profiler.ProfileEnd(PROFILE_BUCKET_LA_NETWORK);
			if(isComcastStream)
			{
#ifndef USE_SECCLIENT
				/*
					Licence response from MDS server is in JSON form
					Licence to decrypt the data can be found by extracting the contents for JSON key licence
					Format : {"licence":"b64encoded licence","accessAttributes":"0"}
				*/
				size_t keylen = 0;
				string jsonStr = string(reinterpret_cast<char*>(key->getData()));
				string keyStr = _extractSubstring(jsonStr, LICENCE_RESPONSE_JSON_LICENCE_KEY, "\"");
				if(keyStr.length() != 0)
				{
					delete key;
					unsigned char* keydata = base64_Decode(keyStr.c_str(),&keylen);
					key = new DrmData(keydata, keylen);
					free(keydata);
				}
#endif
#ifdef LOG_TRACE
				cout << "Printing key data  from server \n";
				unsigned char * data1 = key->getData();
				for (int i = 0; i < key->getDataLength(); ++i)
					cout << data1[i];
				cout << endl;
#endif
			}
			aamp->profiler.ProfileBegin(PROFILE_BUCKET_LA_POSTPROC);
			drmSessionContexts[sessionType].drmSession->aampDRMProcessKey(key);
			aamp->profiler.ProfileEnd(PROFILE_BUCKET_LA_POSTPROC);
		}
		else
		{
			aamp->profiler.ProfileError(PROFILE_BUCKET_LA_NETWORK);
			logprintf("%s:%d Could not get license from server for %s stream\n", __FUNCTION__, __LINE__, sessionTypeName[streamType]);
			if(412 == responseCode)
			{
				if(e->data.dash_drmmetadata.failure != AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN)
				{
					e->data.dash_drmmetadata.failure = AAMP_TUNE_AUTHORISATION_FAILURE;
				}
			}
#ifdef USE_SECCLIENT
			else if(SEC_CLIENT_RESULT_HTTP_RESULT_FAILURE_TIMEOUT == responseCode)
			{
				e->data.dash_drmmetadata.failure = AAMP_TUNE_LICENCE_TIMEOUT;
			}
			else if(SEC_CLIENT_RESULT_MAC_AUTH_NOT_PROVISIONED == responseCode)
			{
				e->data.dash_drmmetadata.failure = AAMP_TUNE_DEVICE_NOT_PROVISIONED;
			}
#endif
			else if(CURLE_OPERATION_TIMEDOUT == responseCode)
			{
				e->data.dash_drmmetadata.failure = AAMP_TUNE_LICENCE_TIMEOUT;
			}
			else
			{
				e->data.dash_drmmetadata.failure = AAMP_TUNE_LICENCE_REQUEST_FAILED;
			}
		}
		delete key;
	}
	else
	{
		logprintf("%s:%d Error in getting license challenge for %s stream : Key State %d \n",
					__FUNCTION__, __LINE__, sessionTypeName[streamType], code);
		aamp->profiler.ProfileError(PROFILE_BUCKET_LA_PREPROC);
		e->data.dash_drmmetadata.failure = AAMP_TUNE_DRM_CHALLENGE_FAILED;
	}

	delete licenceChallenge;
	code = drmSessionContexts[sessionType].drmSession->getState();

	if (code == KEY_READY)
	{
		logprintf("%s:%d Key Ready for %s stream\n", __FUNCTION__, __LINE__, sessionTypeName[streamType]);
		if(drmSessionContexts[sessionType].data != NULL)
		{
			delete drmSessionContexts[sessionType].data;
		}
		drmSessionContexts[sessionType].dataLength = keyIdLen;
		drmSessionContexts[sessionType].data = new unsigned char[keyIdLen];
		memcpy(reinterpret_cast<void*>(drmSessionContexts[sessionType].data),
		reinterpret_cast<const void*>(keyId),keyIdLen);
		pthread_mutex_unlock(&session_mutex[sessionType]);
		free(keyId);
		return drmSessionContexts[sessionType].drmSession;
	}
	else if (code == KEY_ERROR)
	{
		if(AAMP_TUNE_FAILURE_UNKNOWN == e->data.dash_drmmetadata.failure)
		{
			e->data.dash_drmmetadata.failure = AAMP_TUNE_DRM_KEY_UPDATE_FAILED;
		}
	}
	else if (code == KEY_PENDING)
	{
		logprintf("%s:%d Failed to get %s DRM keys for %s stream\n",
					__FUNCTION__, __LINE__, systemId ,sessionTypeName[streamType]);
		if(AAMP_TUNE_FAILURE_UNKNOWN == e->data.dash_drmmetadata.failure)
		{
			e->data.dash_drmmetadata.failure = AAMP_TUNE_INVALID_DRM_KEY;
		}
	}

	pthread_mutex_unlock(&session_mutex[sessionType]);
	free(keyId);
	return NULL;
}

