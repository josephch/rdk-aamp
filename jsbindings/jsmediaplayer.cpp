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
 * @file jsmediaplayer.cpp
 * @brief JavaScript bindings for AAMPMediaPlayer
 */

#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
	#include <core/core.h>
	#include "ThunderAccess.h"
	#include "AampUtils.h"
#endif

#include "jsbindings-version.h"
#include "jsbindings.h"
#include "jsutils.h"
#include "jseventlistener.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef AAMP_CC_ENABLED
#include "AampCCManager.h"
#endif


extern "C"
{
	JS_EXPORT JSGlobalContextRef JSContextGetGlobalContext(JSContextRef);
}

/**
 * @struct AAMPMediaPlayer_JS
 * @brief Private data structure of AAMPMediaPlayer JS object
 */
struct AAMPMediaPlayer_JS : public PrivAAMPStruct_JS
{
	/**
	 * @brief Constructor of AAMPMediaPlayer_JS structure
	 */
	AAMPMediaPlayer_JS() : _promiseCallbacks()
	{
	}
	AAMPMediaPlayer_JS(const AAMPMediaPlayer_JS&) = delete;
	AAMPMediaPlayer_JS& operator=(const AAMPMediaPlayer_JS&) = delete;

	static std::vector<AAMPMediaPlayer_JS *> _jsMediaPlayerInstances;
	std::map<std::string, JSObjectRef> _promiseCallbacks;


	/**
	 * @brief Get promise callback for an ad id
	 * @param[in] id ad id
	 */
	JSObjectRef getCallbackForAdId(std::string id) override
	{
		TRACELOG("Enter AAMPMediaPlayer_JS::%s()", __FUNCTION__);
		std::map<std::string, JSObjectRef>::const_iterator it = _promiseCallbacks.find(id);
		if (it != _promiseCallbacks.end())
		{
			TRACELOG("Exit AAMPMediaPlayer_JS::%s(), found cbObject", __FUNCTION__);
			return it->second;
		}
		else
		{
			TRACELOG("Exit AAMPMediaPlayer_JS::%s(), didn't find cbObject", __FUNCTION__);
			return NULL;
		}
	}


	/**
	 * @brief Get promise callback for an ad id
	 * @param[in] id ad id
	 */
	void removeCallbackForAdId(std::string id) override
	{
		TRACELOG("Enter AAMPMediaPlayer_JS::%s()", __FUNCTION__);
		std::map<std::string, JSObjectRef>::const_iterator it = _promiseCallbacks.find(id);
		if (it != _promiseCallbacks.end())
		{
			JSValueUnprotect(_ctx, it->second);
			_promiseCallbacks.erase(it);
		}
		TRACELOG("Exit AAMPMediaPlayer_JS::%s()", __FUNCTION__);
	}


	/**
	 * @brief Save promise callback for an ad id
	 * @param[in] id ad id
	 * @param[in] cbObject promise callback object
	 */
	void saveCallbackForAdId(std::string id, JSObjectRef cbObject)
	{
		TRACELOG("Enter AAMPMediaPlayer_JS::%s()", __FUNCTION__);
		JSObjectRef savedObject = getCallbackForAdId(id);
		if (savedObject != NULL)
		{
			JSValueUnprotect(_ctx, savedObject); //remove already saved callback
		}

		JSValueProtect(_ctx, cbObject);
		_promiseCallbacks[id] = cbObject;
		TRACELOG("Exit AAMPMediaPlayer_JS::%s()", __FUNCTION__);
	}


	/**
	 * @brief Clear all saved promise callbacks
	 */
	void clearCallbackForAllAdIds()
	{
		TRACELOG("Enter AAMPMediaPlayer_JS::%s()", __FUNCTION__);
		if (!_promiseCallbacks.empty())
		{
			for (auto it = _promiseCallbacks.begin(); it != _promiseCallbacks.end(); )
			{
				JSValueUnprotect(_ctx, it->second);
				it = _promiseCallbacks.erase(it);
			}
		}
		TRACELOG("Exit AAMPMediaPlayer_JS::%s()", __FUNCTION__);
	}
};

/**
 * @enum ConfigParamType
 */
enum ConfigParamType
{
	ePARAM_INITIALBITRATE = 0,
	ePARAM_INITIALBITRATE4K,
	ePARAM_INITIALBUFFER,
	ePARAM_PLAYBACKBUFFER,
	ePARAM_PLAYBACKOFFSET,
	ePARAM_NETWORKTIMEOUT,
	ePARAM_DOWNLOADBUFFER,
	ePARAM_MINBITRATE,
	ePARAM_MAXBITRATE,
	ePARAM_AUDIOLANGUAGE,
	ePARAM_AUDIORENDITION,
	ePARAM_AUDIOCODEC,
	ePARAM_TSBLENGTH,
	ePARAM_DRMCONFIG,
	ePARAM_STEREOONLY,
	ePARAM_BULKTIMEDMETADATA,
	ePARAM_LIVEOFFSET,
	ePARAM_NETWORKPROXY,
	ePARAM_LICENSEREQPROXY,
	ePARAM_DOWNLOADSTALLTIMEOUT,
	ePARAM_DOWNLOADSTARTTIMEOUT,
	ePARAM_SUBTITLELANGUAGE,
	ePARAM_MANIFESTTIMEOUT,
	ePARAM_PLAYLISTTIMEOUT,
	ePARAM_PARALLELPLAYLISTDL,
	ePARAM_ASYNCTUNE,
	ePARAM_USE_WESTEROS_SINK,
	ePARAM_AVGBWFORABR,
	ePARAM_PRECACHEPLAYLISTTIME,
	ePARAM_PROGRESSREPORTINTERVAL,
	ePARAM_PARALLELPLAYLISTREFRESH,
	ePARAM_USE_NEWABR,
	ePARAM_RAMPDOWN_LIMIT,
	ePARAM_INIT_RAMPDOWN_LIMIT,
	ePARAM_SEGMENTINJECTLIMIT,
	ePARAM_DRMDECRYPTLIMIT,
	ePARAM_USE_RETUNE_UNPARIED_DISCONTINUITY,
	ePARAM_USE_NEW_ADBREAKER,
	ePARAM_INIT_FRAGMENT_RETRY_COUNT,
	ePARAM_CEA_FORMAT,
	ePARAM_INITIAL_BUFFER_DURATION,
	ePARAM_USE_MATCHING_BASEURL,
	ePARAM_USE_NATIVE_CC,
	ePARAM_ENABLE_VIDEO_RECTANGLE,
	ePARAM_TUNE_EVENT_CONFIG,
	ePARAM_LANG_CODE_PREFERENCE,
	ePARAM_USE_DESCRIPTIVE_TRACK_NAME,
	ePARAM_AUTHTOKEN,
	ePARAM_USE_RETUNE_FOR_GST_INTERNAL_ERROR,
	ePARAM_ENABLE_SEEKABLE_RANGE,
	ePARAM_REPORT_VIDEO_PTS,
	ePARAM_RELOCKONTIMEOUT,
	ePARAM_RELOCKONPROGRAMCHANGE,
	ePARAM_PROPAGATE_URI_PARAMETERS,
	ePARAM_MAX_PLAYLIST_CACHE_SIZE,
	ePARAM_ON_TUNE_RATE,
	ePARAM_PERSIST_BITRATE_OVER_SEEK,
	ePARAM_SSL_VERIFY_PEER,
	ePARAM_LICENSE_CACHING,
	ePARAM_LIMIT_RESOLUTION,
	ePARAM_PAUSED_BEHAVIOR,
	ePARAM_MAX_COUNT
};

/**
 * @struct ConfigParamMap
 * @brief Data structure to map ConfigParamType and its string equivalent
 */
struct ConfigParamMap
{
	ConfigParamType paramType;
	const char* paramName;
};

/**
 * @brief Map ConfigParamType and its string equivalent
 */
static ConfigParamMap initialConfigParamNames[] =
{
	{ ePARAM_INITIALBITRATE, "initialBitrate" },
	{ ePARAM_INITIALBITRATE4K, "initialBitrate4K" },
	{ ePARAM_INITIALBUFFER, "initialBuffer" },
	{ ePARAM_PLAYBACKBUFFER, "playbackBuffer" },
	{ ePARAM_PLAYBACKOFFSET, "offset" },
	{ ePARAM_NETWORKTIMEOUT, "networkTimeout" },
	{ ePARAM_MANIFESTTIMEOUT, "manifestTimeout" },
	{ ePARAM_PLAYLISTTIMEOUT, "playlistTimeout" },
	{ ePARAM_DOWNLOADBUFFER, "downloadBuffer" },
	{ ePARAM_MINBITRATE, "minBitrate" },
	{ ePARAM_MAXBITRATE, "maxBitrate" },
	{ ePARAM_AUDIOLANGUAGE, "preferredAudioLanguage" },
	{ ePARAM_AUDIORENDITION, "preferredAudioRendition" },
	{ ePARAM_AUDIOCODEC, "preferredAudioCodec"},
	{ ePARAM_TSBLENGTH, "timeShiftBufferLength" },
	{ ePARAM_DRMCONFIG, "drmConfig" },
	{ ePARAM_STEREOONLY, "stereoOnly" },
	{ ePARAM_BULKTIMEDMETADATA, "bulkTimedMetadata" },
	{ ePARAM_LIVEOFFSET, "liveOffset" },
	{ ePARAM_NETWORKPROXY, "networkProxy" },
	{ ePARAM_LICENSEREQPROXY, "licenseProxy" },
	{ ePARAM_DOWNLOADSTALLTIMEOUT, "downloadStallTimeout" },
	{ ePARAM_DOWNLOADSTARTTIMEOUT, "downloadStartTimeout" },
	{ ePARAM_SUBTITLELANGUAGE, "preferredSubtitleLanguage" },
	{ ePARAM_PARALLELPLAYLISTDL, "parallelPlaylistDownload" },
	{ ePARAM_ASYNCTUNE, "asyncTune" },
	{ ePARAM_USE_WESTEROS_SINK, "useWesterosSink" },
	{ ePARAM_AVGBWFORABR, "useAverageBandwidth" },
	{ ePARAM_PRECACHEPLAYLISTTIME, "preCachePlaylistTime" },
	{ ePARAM_PROGRESSREPORTINTERVAL, "progressReportingInterval" },
	{ ePARAM_PARALLELPLAYLISTREFRESH, "parallelPlaylistRefresh" },
	{ ePARAM_USE_NEWABR, "useNewABR" },
	{ ePARAM_RAMPDOWN_LIMIT, "fragmentRetryLimit" },
	{ ePARAM_INIT_RAMPDOWN_LIMIT, "initRampdownLimit" },
	{ ePARAM_SEGMENTINJECTLIMIT, "segmentInjectFailThreshold" },
	{ ePARAM_DRMDECRYPTLIMIT, "drmDecryptFailThreshold" },
	{ ePARAM_USE_NEW_ADBREAKER, "useNewAdBreaker" },
	{ ePARAM_USE_RETUNE_UNPARIED_DISCONTINUITY, "useRetuneForUnpairedDiscontinuity" },
	{ ePARAM_INIT_FRAGMENT_RETRY_COUNT, "initFragmentRetryCount" },
	{ ePARAM_CEA_FORMAT, "ceaFormat" },
	{ ePARAM_USE_MATCHING_BASEURL, "useMatchingBaseUrl" },
	{ ePARAM_USE_NATIVE_CC, "nativeCCRendering" },
	{ ePARAM_ENABLE_VIDEO_RECTANGLE, "enableVideoRectangle" },
	{ ePARAM_TUNE_EVENT_CONFIG, "tuneEventConfig" },
	{ ePARAM_LANG_CODE_PREFERENCE, "langCodePreference" },
	{ ePARAM_USE_DESCRIPTIVE_TRACK_NAME, "descriptiveTrackName" },
	{ ePARAM_AUTHTOKEN, "authToken"},
	{ ePARAM_USE_RETUNE_FOR_GST_INTERNAL_ERROR, "useRetuneForGstInternalError" },
	{ ePARAM_ENABLE_SEEKABLE_RANGE, "enableSeekableRange" },
	{ ePARAM_REPORT_VIDEO_PTS, "reportVideoPTS" },
	{ ePARAM_PROPAGATE_URI_PARAMETERS, "propagateUriParameters"},
	{ ePARAM_MAX_PLAYLIST_CACHE_SIZE, "maxPlaylistCacheSize"},
	{ ePARAM_ON_TUNE_RATE, "onTuneRate"}, 	
	{ ePARAM_PERSIST_BITRATE_OVER_SEEK, "persistBitrateOverSeek"},
	{ ePARAM_SSL_VERIFY_PEER, "sslVerifyPeer"},
	{ ePARAM_LICENSE_CACHING, "setLicenseCaching"},
	{ ePARAM_LIMIT_RESOLUTION, "limitResolution"},
	{ ePARAM_PAUSED_BEHAVIOR, "livePauseBehavior"},
	{ ePARAM_MAX_COUNT, "" }
};

/**
 * @brief Map relockConditionParamNames and its string equivalent
 */
static ConfigParamMap relockConditionParamNames[] =
{
	{ ePARAM_RELOCKONTIMEOUT, "time" },
	{ ePARAM_RELOCKONPROGRAMCHANGE, "programChange" },
	{ ePARAM_MAX_COUNT, "" }
};

std::vector<AAMPMediaPlayer_JS *> AAMPMediaPlayer_JS::_jsMediaPlayerInstances = std::vector<AAMPMediaPlayer_JS *>();

/**
 * @brief Mutex for global cache of AAMPMediaPlayer_JS instances
 */
static pthread_mutex_t jsMediaPlayerCacheMutex = PTHREAD_MUTEX_INITIALIZER;


/**
 * @brief Helper function to parse a JS property value as number
 * @param[in] ctx JS execution context
 * @param[in] jsObject JS object whose property has to be parsed
 * @param[in] prop property name
 * @param[out] value to store parsed number
 * return true if value was parsed sucessfully, false otherwise
 */
bool ParseJSPropAsNumber(JSContextRef ctx, JSObjectRef jsObject, const char *prop, double &value)
{
	bool ret = false;
	JSStringRef propName = JSStringCreateWithUTF8CString(prop);
	JSValueRef propValue = JSObjectGetProperty(ctx, jsObject, propName, NULL);
	if (JSValueIsNumber(ctx, propValue))
	{
		value = JSValueToNumber(ctx, propValue, NULL);
		INFO("[AAMP_JS]: Parsed value for property %s - %f", prop, value);
		ret = true;
	}
	else
	{
		TRACELOG("%s(): Invalid value for property %s passed", __FUNCTION__, prop);
	}

	JSStringRelease(propName);
	return ret;
}


/**
 * @brief Helper function to parse a JS property value as string
 * @param[in] ctx JS execution context
 * @param[in] jsObject JS object whose property has to be parsed
 * @param[in] prop property name
 * @param[out] value to store parsed string
 * return true if value was parsed sucessfully, false otherwise
 */
bool ParseJSPropAsString(JSContextRef ctx, JSObjectRef jsObject, const char *prop, char * &value)
{
	bool ret = false;
	JSStringRef propName = JSStringCreateWithUTF8CString(prop);
	JSValueRef propValue = JSObjectGetProperty(ctx, jsObject, propName, NULL);
	if (JSValueIsString(ctx, propValue))
	{
		value = aamp_JSValueToCString(ctx, propValue, NULL);
		INFO("[AAMP_JS]: Parsed value for property %s - %s", prop, value);
		ret = true;
	}
	else
	{
		TRACELOG("%s(): Invalid value for property - %s passed", __FUNCTION__, prop);
	}

	JSStringRelease(propName);
	return ret;
}


/**
 * @brief Helper function to parse a JS property value as object
 * @param[in] ctx JS execution context
 * @param[in] jsObject JS object whose property has to be parsed
 * @param[in] prop property name
 * @param[out] value to store parsed value
 * return true if value was parsed sucessfully, false otherwise
 */
bool ParseJSPropAsObject(JSContextRef ctx, JSObjectRef jsObject, const char *prop, JSValueRef &value)
{
	bool ret = false;
	JSStringRef propName = JSStringCreateWithUTF8CString(prop);
	JSValueRef propValue = JSObjectGetProperty(ctx, jsObject, propName, NULL);
	if (JSValueIsObject(ctx, propValue))
	{
		value = propValue;
		INFO("[AAMP_JS]: Parsed object as value for property %s", prop);
		ret = true;
	}
	else
	{
		TRACELOG("%s(): Invalid value for property - %s passed", __FUNCTION__, prop);
	}

	JSStringRelease(propName);
	return ret;
}

/**
 * @brief Helper function to parse a JS property value as boolean
 * @param[in] ctx JS execution context
 * @param[in] jsObject JS object whose property has to be parsed
 * @param[in] prop property name
 * @param[out] value to store parsed value
 * return true if value was parsed sucessfully, false otherwise
 */
bool ParseJSPropAsBoolean(JSContextRef ctx, JSObjectRef jsObject, const char *prop, bool &value)
{
       bool ret = false;
       JSStringRef propName = JSStringCreateWithUTF8CString(prop);
       JSValueRef propValue = JSObjectGetProperty(ctx, jsObject, propName, NULL);
       if (JSValueIsBoolean(ctx, propValue))
       {
               value = JSValueToBoolean(ctx, propValue);
               INFO("[AAMP_JS]: Parsed value for property %s - %d", prop, value);
               ret = true;
       }
       else
       {
               TRACELOG("%s(): Invalid value for property - %s passed", __FUNCTION__, prop);
       }

       JSStringRelease(propName);
       return ret;
}

/**
 * @brief API to release internal resources of an AAMPMediaPlayerJS object
 * @param[in] object AAMPMediaPlayerJS object being released
 */
static void releaseNativeResources(AAMPMediaPlayer_JS *privObj)
{
	if (privObj != NULL)
	{
		ERROR("[%s] Deleting AAMPMediaPlayer_JS instance:%p ", __FUNCTION__, privObj);
		if (privObj->_aamp != NULL)
		{
			//when finalizing JS object, don't generate state change events
			privObj->_aamp->Stop(false);
			privObj->clearCallbackForAllAdIds();
			if (privObj->_listeners.size() > 0)
			{
				AAMP_JSEventListener::RemoveAllEventListener(privObj);
			}
			ERROR("[%s] Deleting PlayerInstanceAAMP instance:%p", __FUNCTION__, privObj->_aamp);
			delete privObj->_aamp;
			privObj->_aamp = NULL;
		}

		delete privObj;
		privObj = NULL;
	}
}


/**
 * @brief API to check if AAMPMediaPlayer_JS object present in cache and release it
 *
 * @param[in] object AAMPMediaPlayerJS object
 * @return true if instance was found and native resources released, false otherwise
 */
static bool findInGlobalCacheAndRelease(AAMPMediaPlayer_JS *privObj)
{
	bool found = false;

	if (privObj != NULL)
	{
		pthread_mutex_lock(&jsMediaPlayerCacheMutex);
		for (std::vector<AAMPMediaPlayer_JS *>::iterator iter = AAMPMediaPlayer_JS::_jsMediaPlayerInstances.begin(); iter != AAMPMediaPlayer_JS::_jsMediaPlayerInstances.end(); iter++)
		{
			if (privObj == *iter)
			{
				//Remove this instance from global cache
				AAMPMediaPlayer_JS::_jsMediaPlayerInstances.erase(iter);
				found = true;
				break;
			}
		}
		pthread_mutex_unlock(&jsMediaPlayerCacheMutex);

		if (found)
		{
			//Release private resources
			releaseNativeResources(privObj);
		}
	}

	return found;
}


/**
 * @brief Helper function to parse DRM config params received from JS
 * @param[in] ctx JS execution context
 * @param[in] privObj AAMPMediaPlayer instance to set the drm configuration
 * @param[in] drmConfigParam parameters received as argument
 */
void parseDRMConfiguration (JSContextRef ctx, AAMPMediaPlayer_JS* privObj, JSValueRef drmConfigParam)
{
	JSValueRef exception = NULL;
	JSObjectRef drmConfigObj = JSValueToObject(ctx, drmConfigParam, &exception);

	if (drmConfigObj != NULL && exception == NULL)
	{
		char *prLicenseServerURL = NULL;
		char *wvLicenseServerURL = NULL;
		char *ckLicenseServerURL = NULL;
		char *keySystem = NULL;
		bool ret = false;
		ret = ParseJSPropAsString(ctx, drmConfigObj, "com.microsoft.playready", prLicenseServerURL);
		if (ret)
		{
			ERROR("%s(): Playready License Server URL config param received - %s", __FUNCTION__, prLicenseServerURL);
			privObj->_aamp->SetLicenseServerURL(prLicenseServerURL, eDRM_PlayReady);

			delete[] prLicenseServerURL;
		}

		ret = ParseJSPropAsString(ctx, drmConfigObj, "com.widevine.alpha", wvLicenseServerURL);
		if (ret)
		{
			ERROR("%s(): Widevine License Server URL config param received - %s", __FUNCTION__, wvLicenseServerURL);
			privObj->_aamp->SetLicenseServerURL(wvLicenseServerURL, eDRM_WideVine);

			delete[] wvLicenseServerURL;
		}

		ret = ParseJSPropAsString(ctx, drmConfigObj, "org.w3.clearkey", ckLicenseServerURL);
		if (ret)
		{
			ERROR("%s(): ClearKey License Server URL config param received - %s", __FUNCTION__, ckLicenseServerURL);
			privObj->_aamp->SetLicenseServerURL(ckLicenseServerURL, eDRM_ClearKey);

			delete[] ckLicenseServerURL;
		}

		ret = ParseJSPropAsString(ctx, drmConfigObj, "preferredKeysystem", keySystem);
		if (ret)
		{
			if (strncmp(keySystem, "com.microsoft.playready", 23) == 0)
			{
				ERROR("%s(): Preferred key system config received - playready", __FUNCTION__);
				privObj->_aamp->SetPreferredDRM(eDRM_PlayReady);
			}
			else if (strncmp(keySystem, "com.widevine.alpha", 18) == 0)
			{
				ERROR("%s(): Preferred key system config received - widevine", __FUNCTION__);
				privObj->_aamp->SetPreferredDRM(eDRM_WideVine);
			}
			else
			{
				LOG("%s(): Value passed preferredKeySystem(%s) not supported", __FUNCTION__, keySystem);
			}
			delete[] keySystem;
		}
	}
	else
	{
		ERROR("%s(): InvalidProperty - drmConfigParam is NULL", __FUNCTION__);
	}
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.load()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_load (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call load() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	bool autoPlay = true;
	bool bFinalAttempt = false;
	bool bFirstAttempt = true;
	char* url = NULL;
	char* contentType = NULL;
	char* strTraceId = NULL;

	switch(argumentCount)
	{
		case 3:
		{
			JSObjectRef argument = JSValueToObject(ctx, arguments[2], NULL);
			JSStringRef paramName = JSStringCreateWithUTF8CString("contentType");
			JSValueRef paramValue = JSObjectGetProperty(ctx, argument, paramName, NULL);
			if (JSValueIsString(ctx, paramValue))
			{
				contentType = aamp_JSValueToCString(ctx, paramValue, NULL);
			}
			JSStringRelease(paramName);

			paramName = JSStringCreateWithUTF8CString("traceId");
			paramValue = JSObjectGetProperty(ctx, argument, paramName, NULL);
			if (JSValueIsString(ctx, paramValue))
			{
				strTraceId = aamp_JSValueToCString(ctx, paramValue, NULL);
			}
			JSStringRelease(paramName);

			paramName = JSStringCreateWithUTF8CString("isInitialAttempt");
			paramValue = JSObjectGetProperty(ctx, argument, paramName, NULL);
			if (JSValueIsBoolean(ctx, paramValue))
			{
				bFirstAttempt = JSValueToBoolean(ctx, paramValue);
			}
			JSStringRelease(paramName);

			paramName = JSStringCreateWithUTF8CString("isFinalAttempt");
			paramValue = JSObjectGetProperty(ctx, argument, paramName, NULL);
			if (JSValueIsBoolean(ctx, paramValue))
			{
				bFinalAttempt = JSValueToBoolean(ctx, paramValue);
			}
			JSStringRelease(paramName);
		}
		case 2:
			autoPlay = JSValueToBoolean(ctx, arguments[1]);
		case 1:
		{
			url = aamp_JSValueToCString(ctx, arguments[0], exception);

			if(privObj->_aamp->GetAsyncTuneConfig())
			{
				const std::string manifest = std::string(url);
				const std::string cType = (contentType != NULL) ? std::string(contentType) : std::string();
				const std::string traceId = (strTraceId != NULL) ? std::string(strTraceId) : std::string();

				INFO("[AAMP_JS] %s() ASYNC_TUNE CREATE url='%s'", __FUNCTION__, url);
				privObj->_aamp->ScheduleTask(AsyncTaskObj(
							[manifest, autoPlay, cType, bFirstAttempt, bFinalAttempt, traceId](void *data)
							{
								PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
								instance->Tune(manifest.c_str(), autoPlay, cType.c_str(), bFirstAttempt, bFinalAttempt, traceId.c_str());
							}, (void *) privObj->_aamp));
			}
			else
			{
				char* url = aamp_JSValueToCString(ctx, arguments[0], exception);
				privObj->_aamp->Tune(url, autoPlay, contentType, bFirstAttempt, bFinalAttempt,strTraceId);

			}

			delete [] url;
			delete[] contentType;
			delete[] strTraceId;
		
			break;
		}

		default:
			ERROR("%s(): InvalidArgument - argumentCount=%d, expected atmost 3 arguments", __FUNCTION__, argumentCount);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute load() <= 3 arguments required");
	}

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.initConfig()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_initConfig (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call initConfig() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}


	if (argumentCount == 1 && JSValueIsObject(ctx, arguments[0]))
	{
		JSValueRef _exception = NULL;
		bool ret = false;
		bool valueAsBoolean = false;
		double valueAsNumber = 0;
		char *valueAsString = NULL;
		JSValueRef valueAsObject = NULL;
		int langCodePreference = -1; // value not passed
		bool useRole = false; //default value in func arg
		int enableVideoRectangle = -1; //-1: value not passed, 0: false, 1:true
		int numConfigParams = sizeof(initialConfigParamNames)/sizeof(initialConfigParamNames[0]);

		JSObjectRef initConfigObj = JSValueToObject(ctx, arguments[0], &_exception);
		if (initConfigObj == NULL || _exception != NULL)
		{
			ERROR("%s(): InvalidArgument - argument passed is NULL/not a valid object", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute initConfig() - object of IConfig required");
			return JSValueMakeUndefined(ctx);
		}

		for (int iter = 0; iter < numConfigParams; iter++)
		{
			ret = false;
			switch(initialConfigParamNames[iter].paramType)
			{
			case ePARAM_INITIALBITRATE:
			case ePARAM_INITIALBITRATE4K:
			case ePARAM_INITIALBUFFER:
			case ePARAM_PLAYBACKBUFFER:
			case ePARAM_PLAYBACKOFFSET:
			case ePARAM_NETWORKTIMEOUT:
			case ePARAM_MANIFESTTIMEOUT:
			case ePARAM_PLAYLISTTIMEOUT:
			case ePARAM_DOWNLOADBUFFER:
			case ePARAM_MINBITRATE:
			case ePARAM_MAXBITRATE:
			case ePARAM_TSBLENGTH:
			case ePARAM_LIVEOFFSET:
			case ePARAM_DOWNLOADSTALLTIMEOUT:
			case ePARAM_DOWNLOADSTARTTIMEOUT:
			case ePARAM_PRECACHEPLAYLISTTIME:
			case ePARAM_PROGRESSREPORTINTERVAL:
			case ePARAM_RAMPDOWN_LIMIT:
			case ePARAM_INIT_RAMPDOWN_LIMIT:
			case ePARAM_SEGMENTINJECTLIMIT:
			case ePARAM_DRMDECRYPTLIMIT:
			case ePARAM_INIT_FRAGMENT_RETRY_COUNT:
			case ePARAM_CEA_FORMAT:
			case ePARAM_TUNE_EVENT_CONFIG:
			case ePARAM_LANG_CODE_PREFERENCE:
			case ePARAM_MAX_PLAYLIST_CACHE_SIZE:
			case ePARAM_ON_TUNE_RATE:
			case ePARAM_PAUSED_BEHAVIOR:
				ret = ParseJSPropAsNumber(ctx, initConfigObj, initialConfigParamNames[iter].paramName, valueAsNumber);
				break;
			case ePARAM_AUDIOLANGUAGE:
			case ePARAM_AUDIORENDITION:
			case ePARAM_AUDIOCODEC:
			case ePARAM_NETWORKPROXY:
			case ePARAM_LICENSEREQPROXY:
			case ePARAM_SUBTITLELANGUAGE:
			case ePARAM_AUTHTOKEN:
				ret = ParseJSPropAsString(ctx, initConfigObj, initialConfigParamNames[iter].paramName, valueAsString);
				break;
			case ePARAM_DRMCONFIG:
				ret = ParseJSPropAsObject(ctx, initConfigObj, initialConfigParamNames[iter].paramName, valueAsObject);
				break;
			case ePARAM_STEREOONLY:
			case ePARAM_BULKTIMEDMETADATA:
			case ePARAM_ASYNCTUNE:
			case ePARAM_PARALLELPLAYLISTDL:
			case ePARAM_PARALLELPLAYLISTREFRESH:
			case ePARAM_USE_WESTEROS_SINK:
			case ePARAM_USE_NEWABR:
			case ePARAM_USE_NEW_ADBREAKER:
			case ePARAM_AVGBWFORABR:
			case ePARAM_USE_RETUNE_UNPARIED_DISCONTINUITY:
			case ePARAM_USE_MATCHING_BASEURL:
			case ePARAM_USE_NATIVE_CC:
			case ePARAM_ENABLE_VIDEO_RECTANGLE:
			case ePARAM_USE_DESCRIPTIVE_TRACK_NAME:
			case ePARAM_USE_RETUNE_FOR_GST_INTERNAL_ERROR:
			case ePARAM_ENABLE_SEEKABLE_RANGE:
			case ePARAM_REPORT_VIDEO_PTS:
			case ePARAM_PROPAGATE_URI_PARAMETERS:
			case ePARAM_PERSIST_BITRATE_OVER_SEEK:
			case ePARAM_SSL_VERIFY_PEER:
			case ePARAM_LICENSE_CACHING:
			case ePARAM_LIMIT_RESOLUTION:
				ret = ParseJSPropAsBoolean(ctx, initConfigObj, initialConfigParamNames[iter].paramName, valueAsBoolean);
				break;
			default: //ePARAM_MAX_COUNT
				ret = false;
				break;
			}

			if(ret)
			{
				switch(initialConfigParamNames[iter].paramType)
				{
				case ePARAM_INITIALBITRATE:
					privObj->_aamp->SetInitialBitrate((long) valueAsNumber);
					break;
				case ePARAM_INITIALBITRATE4K:
					privObj->_aamp->SetInitialBitrate4K((long) valueAsNumber);
					break;
				case ePARAM_PLAYBACKOFFSET:
					privObj->_aamp->Seek(valueAsNumber);
					break;
				case ePARAM_NETWORKTIMEOUT:
					privObj->_aamp->SetNetworkTimeout(valueAsNumber);
					break;
				case ePARAM_MANIFESTTIMEOUT:
					privObj->_aamp->SetManifestTimeout(valueAsNumber);
					break;
				case ePARAM_PLAYLISTTIMEOUT:
					privObj->_aamp->SetPlaylistTimeout(valueAsNumber);
					break;
				case ePARAM_PROGRESSREPORTINTERVAL:
					privObj->_aamp->SetReportInterval((valueAsNumber*1000));
					break;
				case ePARAM_INIT_FRAGMENT_RETRY_COUNT:
					privObj->_aamp->SetInitFragTimeoutRetryCount(valueAsNumber);
					break;
				case ePARAM_CEA_FORMAT:
					privObj->_aamp->SetCEAFormat((int) valueAsNumber);
					break;
				case ePARAM_DOWNLOADBUFFER:
					privObj->_aamp->SetDownloadBufferSize((int) valueAsNumber);
					break;
				case ePARAM_AUDIOLANGUAGE:
					privObj->_aamp->SetPreferredLanguages(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_AUDIORENDITION:
					privObj->_aamp->SetPreferredRenditions(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_AUDIOCODEC:
					privObj->_aamp->SetPreferredCodec(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_DRMCONFIG:
					parseDRMConfiguration(ctx, privObj, valueAsObject);
					break;
				case ePARAM_STEREOONLY:
					privObj->_aamp->SetStereoOnlyPlayback(valueAsBoolean);
					break;
				case ePARAM_BULKTIMEDMETADATA:
					privObj->_aamp->SetBulkTimedMetaReport(valueAsBoolean);
					break;
				case ePARAM_LIVEOFFSET:
					privObj->_aamp->SetLiveOffset((int) valueAsNumber);
					break;
				case ePARAM_NETWORKPROXY:
					privObj->_aamp->SetNetworkProxy(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_LICENSEREQPROXY:
					privObj->_aamp->SetLicenseReqProxy(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_DOWNLOADSTALLTIMEOUT:
					privObj->_aamp->SetDownloadStallTimeout((long) valueAsNumber);
					break;
				case ePARAM_DOWNLOADSTARTTIMEOUT:
					privObj->_aamp->SetDownloadStartTimeout((long) valueAsNumber);
					break;
				case ePARAM_SUBTITLELANGUAGE:
					privObj->_aamp->SetPreferredSubtitleLanguage(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_PARALLELPLAYLISTDL:
					privObj->_aamp->SetParallelPlaylistDL(valueAsBoolean);
					break;
				case ePARAM_PARALLELPLAYLISTREFRESH:
					privObj->_aamp->SetParallelPlaylistRefresh(valueAsBoolean);
					break;
				case ePARAM_ASYNCTUNE:
					if (valueAsBoolean)
					{
						privObj->_aamp->EnableAsyncOperation();
					}
					break;
				case ePARAM_USE_WESTEROS_SINK:
					privObj->_aamp->SetWesterosSinkConfig(valueAsBoolean);
					break;
				case ePARAM_LICENSE_CACHING:
					privObj->_aamp->SetLicenseCaching(valueAsBoolean);
					break;
				case ePARAM_USE_NEWABR:
					privObj->_aamp->SetNewABRConfig(valueAsBoolean);
					break;
				case ePARAM_USE_NEW_ADBREAKER:
					privObj->_aamp->SetNewAdBreakerConfig(valueAsBoolean);
					break;
				case ePARAM_USE_RETUNE_UNPARIED_DISCONTINUITY:
					privObj->_aamp->SetRetuneForUnpairedDiscontinuity(valueAsBoolean);
					break;
				case ePARAM_USE_RETUNE_FOR_GST_INTERNAL_ERROR:
					privObj->_aamp->SetRetuneForGSTInternalError(valueAsBoolean);
					break;
				case ePARAM_AVGBWFORABR:
					privObj->_aamp->SetAvgBWForABR(valueAsBoolean);
					break;
				case ePARAM_PRECACHEPLAYLISTTIME:
					privObj->_aamp->SetPreCacheTimeWindow((int) valueAsNumber);
					break;					
				case ePARAM_INITIALBUFFER:
					privObj->_aamp->SetInitialBufferDuration((int) valueAsNumber);
					break;
				case ePARAM_PLAYBACKBUFFER:
					//TODO: Support these config params
					break;
				case ePARAM_MINBITRATE:
					privObj->_aamp->SetMinimumBitrate((long) valueAsNumber);
					break;
				case ePARAM_MAXBITRATE:
					privObj->_aamp->SetMaximumBitrate((long) valueAsNumber);
					break;
				case ePARAM_RAMPDOWN_LIMIT:
					privObj->_aamp->SetRampDownLimit((int) valueAsNumber);
					break;
				case ePARAM_INIT_RAMPDOWN_LIMIT:
					privObj->_aamp->SetInitRampdownLimit((int) valueAsNumber);
					break;
				case ePARAM_SEGMENTINJECTLIMIT:
					privObj->_aamp->SetSegmentInjectFailCount((int) valueAsNumber);
					break;
				case ePARAM_DRMDECRYPTLIMIT:
					privObj->_aamp->SetSegmentDecryptFailCount((int) valueAsNumber);
					break;
				case ePARAM_TSBLENGTH:
					//TODO: Support these config params
					break;
				case ePARAM_USE_MATCHING_BASEURL:
					privObj->_aamp->SetMatchingBaseUrlConfig(valueAsBoolean);
					break;
				case ePARAM_USE_NATIVE_CC:
					privObj->_aamp->SetNativeCCRendering(valueAsBoolean);
					break;
				case ePARAM_ENABLE_VIDEO_RECTANGLE:
					enableVideoRectangle = (int)valueAsBoolean;
					break;
				case ePARAM_TUNE_EVENT_CONFIG:
					privObj->_aamp->SetTuneEventConfig((int) valueAsNumber);
					break;
				case ePARAM_LANG_CODE_PREFERENCE:
					langCodePreference = (int) valueAsNumber;
					break;
				case ePARAM_USE_DESCRIPTIVE_TRACK_NAME:
					useRole = valueAsBoolean;
					break;
				case ePARAM_AUTHTOKEN:
					privObj->_aamp->SetSessionToken(valueAsString);
					delete[] valueAsString;
					break;
				case ePARAM_ENABLE_SEEKABLE_RANGE:
					privObj->_aamp->EnableSeekableRange(valueAsBoolean);
					break;
				case ePARAM_REPORT_VIDEO_PTS:
					privObj->_aamp->SetReportVideoPTS(valueAsBoolean);
					break;
				case ePARAM_PROPAGATE_URI_PARAMETERS:
					privObj->_aamp->SetPropagateUriParameters(valueAsBoolean);
					break;	
				case ePARAM_MAX_PLAYLIST_CACHE_SIZE:
					privObj->_aamp->SetMaxPlaylistCacheSize((int) valueAsNumber);
					break;
				case ePARAM_ON_TUNE_RATE:
					privObj->_aamp->SetRate((int) valueAsNumber); 
					break;
				case ePARAM_PERSIST_BITRATE_OVER_SEEK:
					privObj->_aamp->PersistBitRateOverSeek(valueAsBoolean);
					break;
				case ePARAM_SSL_VERIFY_PEER:
					privObj->_aamp->SetSslVerifyPeerConfig(valueAsBoolean);
					break;
				case ePARAM_LIMIT_RESOLUTION:
                                        privObj->_aamp->SetOutputResolutionCheck(valueAsBoolean);
                                        break;
				case ePARAM_PAUSED_BEHAVIOR:
					privObj->_aamp->SetPausedBehavior((int) valueAsNumber);
					break;
				default: //ePARAM_MAX_COUNT
					break;
				}
			}
		}

		if (langCodePreference != -1)
		{
			privObj->_aamp->SetLanguageFormat((LangCodePreference) langCodePreference, useRole);
		}
		// This is kept outside because this config is tied to the westerossink configuration.
		// In the input JSON, the order of configs might not be correct, so to avoid any errors set it at the end
		if (enableVideoRectangle != -1)
		{
			privObj->_aamp->EnableVideoRectangle(enableVideoRectangle == 1);
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute initConfig() - 1 argument of type IConfig required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.play()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_play (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call play() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	if (privObj->_aamp->GetAsyncTuneConfig())
	{
		privObj->_aamp->ScheduleTask(AsyncTaskObj(
					[](void *data)
					{
						PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
						instance->SetRate(AAMP_NORMAL_PLAY_RATE);
					}, (void *) privObj->_aamp));
	}
	else
	{
		privObj->_aamp->SetRate(AAMP_NORMAL_PLAY_RATE);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.detach()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_detach (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call play() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	privObj->_aamp->detach();

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.pause()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_pause (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call pause() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	if (privObj->_aamp->GetAsyncTuneConfig())
	{
		privObj->_aamp->ScheduleTask(AsyncTaskObj(
					[](void *data)
					{
						PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
						instance->SetRate(0);
					}, (void *) privObj->_aamp));
	}
	else
	{
		privObj->_aamp->SetRate(0);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.stop()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_stop (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj || (privObj && !privObj->_aamp))
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call stop() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	privObj->_aamp->Stop();
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.seek()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_seek (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call seek() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1 || argumentCount == 2)
	{
		double newSeekPos = JSValueToNumber(ctx, arguments[0], exception);
		bool keepPaused = (argumentCount == 2)? JSValueToBoolean(ctx, arguments[1]) : false;

		if (privObj->_aamp->GetAsyncTuneConfig())
		{
			privObj->_aamp->ScheduleTask(AsyncTaskObj(
						[newSeekPos, keepPaused](void *data)
						{
							PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
							instance->Seek(newSeekPos, keepPaused);
						}, (void *) privObj->_aamp));
		}
		else
		{
			privObj->_aamp->Seek(newSeekPos, keepPaused);
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1 or 2", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute seek() - 1 or 2 arguments required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.GetThumbnails()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getThumbnails (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call seek() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		double thumbnailPosition = JSValueToNumber(ctx, arguments[0], exception);

		std::string value = privObj->_aamp->GetThumbnails(thumbnailPosition, 0);
	        if (!value.empty())
        	{
	                TRACELOG("Exit %s()", __FUNCTION__);
        	        return aamp_CStringToJSValue(ctx, value.c_str());
	        }

	}
	else if (argumentCount == 2)
	{
		double startPos = JSValueToNumber(ctx, arguments[0], exception);
		double endPos = JSValueToNumber(ctx, arguments[1], exception);
		std::string value = privObj->_aamp->GetThumbnails(startPos, endPos);
		if (!value.empty())
		{
			ERROR("Exit %s()", __FUNCTION__);
			return aamp_CStringToJSValue(ctx, value.c_str());
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1 or 2", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute seek() - 1 or 2 arguments required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getAvailableThumbnailTracks()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getAvailableThumbnailTracks (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getVideoBitrates() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	std::string value = privObj->_aamp->GetAvailableThumbnailTracks();
	if (!value.empty())
	{
		ERROR("Exit %s()", __FUNCTION__);
		return aamp_CStringToJSValue(ctx, value.c_str());
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setThumbnailTrack()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setThumbnailTrack (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{

	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call SetThumbnailTrack() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute SetThumbnailTrack() - 1 argument required");
	}
	else
	{
		int thumbnailIndex = (int) JSValueToNumber(ctx, arguments[0], NULL);
		if (thumbnailIndex >= 0)
		{
			return JSValueMakeBoolean(ctx, privObj->_aamp->SetThumbnailTrack(thumbnailIndex));
		}
		else
		{
			ERROR("%s(): InvalidArgument - Index should be >= 0!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Index should be >= 0!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getCurrentState()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getCurrentState (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getCurrentState() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetState());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getDurationSec()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getDurationSec (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	double duration = 0;
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getDurationSec() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	duration = privObj->_aamp->GetPlaybackDuration();
	if (duration < 0)
	{
		ERROR("%s(): Duration returned by GetPlaybackDuration() is less than 0!", __FUNCTION__);
		duration = 0;
	}

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, duration);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getCurrentPosition()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getCurrentPosition (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	double currPosition = 0;
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getCurrentPosition() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	currPosition = privObj->_aamp->GetPlaybackPosition();
	if (currPosition < 0)
	{
		ERROR("%s(): Current position returned by GetPlaybackPosition() is less than 0!", __FUNCTION__);
		currPosition = 0;
	}

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, currPosition);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getVideoBitrates()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getVideoBitrates (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getVideoBitrates() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	std::vector<long> bitrates = privObj->_aamp->GetVideoBitrates();
	if (!bitrates.empty())
	{
		unsigned int length = bitrates.size();
		JSValueRef* array = new JSValueRef[length];
		for (int i = 0; i < length; i++)
		{
			array[i] = JSValueMakeNumber(ctx, bitrates[i]);
		}

		JSValueRef retVal = JSObjectMakeArray(ctx, length, array, NULL);
		delete [] array;
		TRACELOG("Exit %s()", __FUNCTION__);
		return retVal;
	}
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getAudioBitrates()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getAudioBitrates (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getAudioBitrates() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	std::vector<long> bitrates = privObj->_aamp->GetAudioBitrates();
	if (!bitrates.empty())
	{
		unsigned int length = bitrates.size();
		JSValueRef* array = new JSValueRef[length];
		for (int i = 0; i < length; i++)
		{
			array[i] = JSValueMakeNumber(ctx, bitrates[i]);
		}

		JSValueRef retVal = JSObjectMakeArray(ctx, length, array, NULL);
		delete [] array;
		TRACELOG("Exit %s()", __FUNCTION__);
		return retVal;
	}
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getCurrentVideoBitrate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getCurrentVideoBitrate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getCurrentVideoBitrate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetVideoBitrate());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setVideoBitrate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setVideoBitrate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setVideoBitrate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setVideoBitrate() - 1 argument required");
	}
	else
	{
		long bitrate = (long) JSValueToNumber(ctx, arguments[0], NULL);
		//bitrate 0 is for ABR
		if (bitrate >= 0)
		{
			privObj->_aamp->SetVideoBitrate(bitrate);
		}
		else
		{
			ERROR("%s(): InvalidArgument - bitrate should be >= 0!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Bitrate should be >= 0!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getCurrentAudioBitrate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getCurrentAudioBitrate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getCurrentAudioBitrate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetAudioBitrate());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setAudioBitrate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setAudioBitrate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setAudioBitrate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setAudioBitrate() - 1 argument required");
	}
	else
	{
		long bitrate = (long) JSValueToNumber(ctx, arguments[0], NULL);
		if (bitrate >= 0)
		{
			privObj->_aamp->SetAudioBitrate(bitrate);
		}
		else
		{
			ERROR("%s(): InvalidArgument - bitrate should be >= 0!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Bitrate should be >= 0!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getAudioTrack()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getAudioTrack (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getAudioTrack() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetAudioTrack());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setAudioTrack()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setAudioTrack (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setAudioTrack() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setAudioTrack() - 1 argument required");
	}
	else  if (JSValueIsObject(ctx, arguments[0]))
	{
	/*
		 * Parmater format
		 * "audioTuple": object {
		 *    "language": "en",
		 *    "rendition": "main"
		 *    "codec": "avc", 
		 *    "channel": 6  
		 * }
		 */
		char *language = NULL;
		int channel = 0;
		char *rendition = NULL;
		char *codec = NULL;
		//Parse the ad object
		JSObjectRef audioProperty = JSValueToObject(ctx, arguments[0], NULL);
		if (audioProperty == NULL)
		{
			ERROR("%s() Unable to convert argument to JSObject", __FUNCTION__);
			return JSValueMakeUndefined(ctx);
		}
		JSStringRef propName = JSStringCreateWithUTF8CString("language");
		JSValueRef propValue = JSObjectGetProperty(ctx, audioProperty, propName, NULL);
		if (JSValueIsString(ctx, propValue))
		{
			language = aamp_JSValueToCString(ctx, propValue, NULL);
		}
		JSStringRelease(propName);
		
		propName = JSStringCreateWithUTF8CString("rendition");
		propValue = JSObjectGetProperty(ctx, audioProperty, propName, NULL);
		if (JSValueIsString(ctx, propValue))
		{
			rendition = aamp_JSValueToCString(ctx, propValue, NULL);
		}
		JSStringRelease(propName);
		
		propName = JSStringCreateWithUTF8CString("codec");
		propValue = JSObjectGetProperty(ctx, audioProperty, propName, NULL);
		if (JSValueIsString(ctx, propValue))
		{
			codec = aamp_JSValueToCString(ctx, propValue, NULL);
		}
		JSStringRelease(propName);

		propName = JSStringCreateWithUTF8CString("channel");
		propValue = JSObjectGetProperty(ctx, audioProperty, propName, NULL);
		if (JSValueIsNumber(ctx, propValue))
		{
			channel = JSValueToNumber(ctx, propValue, NULL);
		}
		JSStringRelease(propName);

		std::string strLanguage =  language?std::string(language):"";
		if(language)delete[] language;
		std::string strRendition = rendition?std::string(rendition):"";
		if(rendition)delete[] rendition;
		std::string strCodec = codec?std::string(codec):"";
		if(codec)delete[] codec;

		INFO("%s() Calling SetAudioTrack ", __FUNCTION__);
		if (privObj->_aamp->GetAsyncTuneConfig())
		{
			privObj->_aamp->ScheduleTask(AsyncTaskObj(
				[strLanguage, strRendition, strCodec, channel](void *data)
				{
					PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
					instance->SetAudioTrack(strLanguage, strRendition, strCodec, channel);
				}, (void *) privObj->_aamp)
			);
		}
		else
		{
			privObj->_aamp->SetAudioTrack(strLanguage, strRendition, strCodec, channel);
		}
		
	}
	else
	{
		int index = (int) JSValueToNumber(ctx, arguments[0], NULL);
		if (index >= 0)
		{
			if (privObj->_aamp->GetAsyncTuneConfig())
			{
				privObj->_aamp->ScheduleTask(AsyncTaskObj(
					[index](void *data)
					{
						PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
						instance->SetAudioTrack(index);
					}, (void *) privObj->_aamp));
			}
			else
			{
				privObj->_aamp->SetAudioTrack(index);
			}
		}
		else
		{
			ERROR("%s(): InvalidArgument - track index should be >= 0!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Audio track index should be >= 0!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getTextTrack()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getTextTrack (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getTextTrack() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetTextTrack());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setTextTrack()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setTextTrack (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setTextTrack() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setTextTrack() - 1 argument required");
	}
	else
	{
		int index = (int) JSValueToNumber(ctx, arguments[0], NULL);
		if (index >= 0)
		{
			privObj->_aamp->SetTextTrack(index);
		}
		else
		{
			ERROR("%s(): InvalidArgument - track index should be >= 0!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Text track index should be >= 0!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getVolume()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getVolume (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getVolume() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetAudioVolume());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setVolume()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setVolume (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setVolume() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		int volume = (int) JSValueToNumber(ctx, arguments[0], exception);
		if (volume >= 0)
		{
			privObj->_aamp->SetAudioVolume(volume);
		}
		else
		{
			ERROR("%s(): InvalidArgument - volume should not be a negative number", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Volume should not be a negative number");
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setVolume() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setAudioLanguage()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setAudioLanguage (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setAudioLanguage() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		const char *lang = aamp_JSValueToCString(ctx, arguments[0], exception);
		if (privObj->_aamp->GetAsyncTuneConfig())
		{
			std::string language = std::string(lang);
			privObj->_aamp->ScheduleTask(AsyncTaskObj(
						[language](void *data)
						{
							PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
							instance->SetLanguage(language.c_str());
						}, (void *) privObj->_aamp));
		}
		else
		{
			privObj->_aamp->SetLanguage(lang);
		}
		delete[] lang;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setAudioLanguage() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getPlaybackRate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getPlaybackRate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getPlaybackRate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeNumber(ctx, privObj->_aamp->GetPlaybackRate());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setPlaybackRate()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setPlaybackRate (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setPlaybackRate() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1 || argumentCount == 2)
	{
		int overshootCorrection = 0;
		int rate = (int) JSValueToNumber(ctx, arguments[0], exception);
		if (argumentCount == 2)
		{
			overshootCorrection = (int) JSValueToNumber(ctx, arguments[1], exception);
		}
		if (privObj->_aamp->GetAsyncTuneConfig())
		{
			privObj->_aamp->ScheduleTask(AsyncTaskObj(
						[rate, overshootCorrection](void *data)
						{
							PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
							instance->SetRate(rate, overshootCorrection);
						}, (void *) privObj->_aamp));
		}
		else
		{
			privObj->_aamp->SetRate(rate, overshootCorrection);
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setPlaybackRate() - atleast 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getSupportedKeySystems()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_getSupportedKeySystems (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getSupportedKeySystems() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	ERROR("%s(): Invoked getSupportedKeySystems", __FUNCTION__);
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setVideoMute()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setVideoMute (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setVideoMute() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		bool videoMute = JSValueToBoolean(ctx, arguments[0]);
		privObj->_aamp->SetVideoMute(videoMute);
		ERROR("%s(): Invoked setVideoMute", __FUNCTION__);
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setVideoMute() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setSubscribedTags()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setSubscribedTags (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setSubscribedTags() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setSubscribedTags() - 1 argument required");
	}
	else if (!aamp_JSValueIsArray(ctx, arguments[0]))
	{
		ERROR("[AAMP_JS] %s() InvalidArgument: aamp_JSValueIsArray=%d", __FUNCTION__, aamp_JSValueIsArray(ctx, arguments[0]));
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setSubscribeTags() - parameter 1 is not an array");
	}
	else
	{
		std::vector<std::string> subscribedTags = aamp_StringArrayToCStringArray(ctx, arguments[0]);
		privObj->_aamp->SetSubscribedTags(subscribedTags);
		ERROR("%s(): Invoked setSubscribedTags", __FUNCTION__);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.addEventListener()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_addEventListener (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call addEventListener() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount >= 2)
	{
		char* type = aamp_JSValueToCString(ctx, arguments[0], NULL);
		JSObjectRef callbackObj = JSValueToObject(ctx, arguments[1], NULL);

		if (callbackObj != NULL && JSObjectIsFunction(ctx, callbackObj))
		{
			AAMPEventType eventType = aampPlayer_getEventTypeFromName(type);
			LOG("%s() eventType='%s', %d", __FUNCTION__, type, eventType);

			if ((eventType >= 0) && (eventType < AAMP_MAX_NUM_EVENTS))
			{
				AAMP_JSEventListener::AddEventListener(privObj, eventType, callbackObj);
			}
		}
		else
		{
			ERROR("%s() callbackObj=%p, JSObjectIsFunction(context, callbackObj)=%d", __FUNCTION__, callbackObj, JSObjectIsFunction(ctx, callbackObj));
			char errMsg[512];
			memset(errMsg, '\0', 512);
			snprintf(errMsg, 511, "Failed to execute addEventListener() for event %s - parameter 2 is not a function", type);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, (const char*)errMsg);
		}

		delete[] type;
	}
	else
	{
		ERROR("[AAMP_JS] %s() InvalidArgument: argumentCount=%d, expected: 2", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute addEventListener() - 2 arguments required.");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.removeEventListener()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_removeEventListener (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call removeEventListener() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount >= 2)
	{
		char* type = aamp_JSValueToCString(ctx, arguments[0], NULL);
		JSObjectRef callbackObj = JSValueToObject(ctx, arguments[1], NULL);

		if (callbackObj != NULL && JSObjectIsFunction(ctx, callbackObj))
		{
			AAMPEventType eventType = aampPlayer_getEventTypeFromName(type);
			LOG("[AAMP_JS] %s() eventType='%s', %d", __FUNCTION__, type, eventType);

			if ((eventType >= 0) && (eventType < AAMP_MAX_NUM_EVENTS))
			{
				AAMP_JSEventListener::RemoveEventListener(privObj, eventType, callbackObj);
			}
		}
		else
		{
			ERROR("%s() InvalidArgument: callbackObj=%p, JSObjectIsFunction(context, callbackObj)=%d", __FUNCTION__, callbackObj, JSObjectIsFunction(ctx, callbackObj));
			char errMsg[512];
			memset(errMsg, '\0', 512);
			snprintf(errMsg, 511, "Failed to execute removeEventListener() for event %s - parameter 2 is not a function", type);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, (const char*)errMsg);
		}

		delete[] type;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute removeEventListener() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setDRMConfig()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setDRMConfig (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setDrmConfig() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setDrmConfig() - 1 argument required");
	}
	else
	{
		if (JSValueIsObject(ctx, arguments[0]))
		{
			parseDRMConfiguration(ctx, privObj, arguments[0]);
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.addCustomHTTPHeader()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_addCustomHTTPHeader (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call addCustomHTTPHeader() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	//optional parameter 3 for identifying if the header is for a license request
	if (argumentCount == 2 || argumentCount == 3)
	{
		char *name = aamp_JSValueToCString(ctx, arguments[0], exception);
		std::string headerName(name);
		std::vector<std::string> headerVal;
		bool isLicenseHeader = false;

		delete[] name;

		if (aamp_JSValueIsArray(ctx, arguments[1]))
		{
			headerVal = aamp_StringArrayToCStringArray(ctx, arguments[1]);
		}
		else if (JSValueIsString(ctx, arguments[1]))
		{
			headerVal.reserve(1);
			char *value =  aamp_JSValueToCString(ctx, arguments[1], exception);
			headerVal.push_back(value);
			delete[] value;
		}

		// Don't support empty values now
		if (headerVal.size() == 0)
		{
			ERROR("%s() InvalidArgument: Custom header's value is empty", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute addCustomHTTPHeader() - 2nd argument should be a string or array of strings");
			return JSValueMakeUndefined(ctx);
		}

		if (argumentCount == 3)
		{
			isLicenseHeader = JSValueToBoolean(ctx, arguments[2]);
		}
		privObj->_aamp->AddCustomHTTPHeader(headerName, headerVal, isLicenseHeader);
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute addCustomHTTPHeader() - 2 arguments required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.removeCustomHTTPHeader()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_removeCustomHTTPHeader (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call removeCustomHTTPHeader() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		char *name = aamp_JSValueToCString(ctx, arguments[0], exception);
		std::string headerName(name);
		privObj->_aamp->AddCustomHTTPHeader(headerName, std::vector<std::string>());
		delete[] name;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute removeCustomHTTPHeader() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setVideoRect()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setVideoRect (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setVideoRect() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 4)
	{
		int x = (int) JSValueToNumber(ctx, arguments[0], exception);
		int y = (int) JSValueToNumber(ctx, arguments[1], exception);
		int w = (int) JSValueToNumber(ctx, arguments[2], exception);
		int h = (int) JSValueToNumber(ctx, arguments[3], exception);
		privObj->_aamp->SetVideoRectangle(x,y,w,h);
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setVideoRect() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setVideoZoom()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setVideoZoom (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setVideoZoom() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		VideoZoomMode zoom;
		char* zoomStr = aamp_JSValueToCString(ctx, arguments[0], exception);
		if (0 == strcmp(zoomStr, "none"))
		{
			zoom = VIDEO_ZOOM_NONE;
		}
		else
		{
			zoom = VIDEO_ZOOM_FULL;
		}
		privObj->_aamp->SetVideoZoom(zoom);
		delete[] zoomStr;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setVideoZoom() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.release()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_release (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);

	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call release() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (false == findInGlobalCacheAndRelease(privObj))
	{
		ERROR("%s:%d [WARN] Invoked release of a AAMPMediaPlayer_JS object(%p) which was already/being released!!", __FUNCTION__, __LINE__, privObj);
	}
	else
	{
		ERROR("%s:%d [WARN] Cleaned up native object for AAMPMediaPlayer_JS object(%p)!!", __FUNCTION__, __LINE__, privObj);
	}

	// This prevents further release/clean up on the native object
	JSObjectSetPrivate(thisObject, NULL);

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setAlternateContent()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_setAlternateContent(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setAlternateContent() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 2)
	{
		/*
		 * Parmater format
		 * "reservationObject": object {
		 *   "reservationId": "773701056",
		 *    "reservationBehavior": number
		 *    "placementRequest": {
		 *      "id": string,
		 *      "pts": number,
		 *      "url": "",
		 *    },
		 * },
		 * "promiseCallback": function
		 */
		char *reservationId = NULL;
		int reservationBehavior = -1;
		char *adId = NULL;
		long adPTS = -1;
		char *adURL = NULL;
		if (JSValueIsObject(ctx, arguments[0]))
		{
			//Parse the ad object
			JSObjectRef reservationObject = JSValueToObject(ctx, arguments[0], NULL);
			if (reservationObject == NULL)
			{
				ERROR("%s() Unable to convert argument to JSObject", __FUNCTION__);
				return JSValueMakeUndefined(ctx);
			}
			JSStringRef propName = JSStringCreateWithUTF8CString("reservationId");
			JSValueRef propValue = JSObjectGetProperty(ctx, reservationObject, propName, NULL);
			if (JSValueIsString(ctx, propValue))
			{
				reservationId = aamp_JSValueToCString(ctx, propValue, NULL);
			}
			JSStringRelease(propName);

			propName = JSStringCreateWithUTF8CString("reservationBehavior");
			propValue = JSObjectGetProperty(ctx, reservationObject, propName, NULL);
			if (JSValueIsNumber(ctx, propValue))
			{
				reservationBehavior = JSValueToNumber(ctx, propValue, NULL);
			}
			JSStringRelease(propName);

			propName = JSStringCreateWithUTF8CString("placementRequest");
			propValue = JSObjectGetProperty(ctx, reservationObject, propName, NULL);
			if (JSValueIsObject(ctx, propValue))
			{
				JSObjectRef adObject = JSValueToObject(ctx, propValue, NULL);

				JSStringRef adPropName = JSStringCreateWithUTF8CString("id");
				JSValueRef adPropValue = JSObjectGetProperty(ctx, adObject, adPropName, NULL);
				if (JSValueIsString(ctx, adPropValue))
				{
					adId = aamp_JSValueToCString(ctx, adPropValue, NULL);
				}
				JSStringRelease(adPropName);

				adPropName = JSStringCreateWithUTF8CString("pts");
				adPropValue = JSObjectGetProperty(ctx, adObject, adPropName, NULL);
				if (JSValueIsNumber(ctx, adPropValue))
				{
					adPTS = (long) JSValueToNumber(ctx, adPropValue, NULL);
				}
				JSStringRelease(adPropName);

				adPropName = JSStringCreateWithUTF8CString("url");
				adPropValue = JSObjectGetProperty(ctx, adObject, adPropName, NULL);
				if (JSValueIsString(ctx, adPropValue))
				{
					adURL = aamp_JSValueToCString(ctx, adPropValue, NULL);
				}
				JSStringRelease(adPropName);
			}
			JSStringRelease(propName);
		}

		JSObjectRef callbackObj = JSValueToObject(ctx, arguments[1], NULL);

		if (callbackObj != NULL && JSObjectIsFunction(ctx, callbackObj) && adId && reservationId && adURL)
		{
			std::string adIdStr(adId);  //CID:115002 - Resolve Forward null
			std::string adBreakId(reservationId);  //CID:115001 - Resolve Forward null
			std::string url(adURL);  //CID:115000 - Resolve Forward null

			privObj->saveCallbackForAdId(adIdStr, callbackObj); //save callback for sending status later, if ad can be played or not

			ERROR("%s() Calling privObj->_aamp->SetAlternateContents with promiseCallback:%p", __FUNCTION__, callbackObj);
			privObj->_aamp->SetAlternateContents(adBreakId, adIdStr, url);
		}
		else
		{
			ERROR("%s() Unable to parse the promiseCallback argument", __FUNCTION__);
		}
		if (reservationId)
		{
			delete[] reservationId;
		}
		if (adURL)
		{
			delete[] adURL;
		}
		if (adId)
		{
			delete[] adId;
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 2", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setAlternateContent() - 2 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setPreferredAudioLanguage()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setPreferredAudioLanguage(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setPreferredAudioLanguage() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if( argumentCount==1 || argumentCount==2 )
	{
		char* lanList = aamp_JSValueToCString(ctx,arguments[0], NULL);
		if( lanList )
		{
			if( argumentCount==2 )
			{  
				char* rendition = aamp_JSValueToCString(ctx,arguments[1], NULL);
				if( rendition )
				{    
					privObj->_aamp->SetPreferredLanguages(lanList,rendition);
					delete[] rendition;
				}
			}
			else
			{
				privObj->_aamp->SetPreferredLanguages(lanList);
			}
			delete[] lanList;
		}
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setPreferredAudioLanguage() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setPreferredAudioRendition()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setPreferredAudioRendition(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{ // placeholder - not ready for use/publishing
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setPreferredAudioRendition() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setPreferredAudioRendition() - 1 argument required");
	}
	else
	{
		char* renditionList = aamp_JSValueToCString(ctx,arguments[0], NULL);
		privObj->_aamp->SetPreferredRenditions(renditionList);
		if(renditionList)delete[] renditionList;
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setPreferredAudioCodec()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setPreferredAudioCodec(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{ // placeholder - not ready for use/publishing
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setPreferredAudioCodec() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setPreferredAudioCodec() - 1 argument required");
	}
	else
	{
		char *codecList = aamp_JSValueToCString(ctx,arguments[0], NULL);
		privObj->_aamp->SetPreferredCodec(codecList);
		if(codecList)delete[] codecList;
	}

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.notifyReservationCompletion()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_notifyReservationCompletion(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call notifyReservationCompletion() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 2)
	{
		const char * reservationId = aamp_JSValueToCString(ctx, arguments[0], exception);
		long time = (long) JSValueToNumber(ctx, arguments[1], exception);
		//Need an API in AAMP to notify that placements for this reservation are over and AAMP might have to trim
		//the ads to the period duration or not depending on time param
		ERROR("%s(): Called reservation close for periodId:%s and time:%ld", __FUNCTION__, reservationId, time);
		delete[] reservationId;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 2", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute notifyReservationCompletion() - 2 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getAvailableAudioTracks()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_getAvailableAudioTracks(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getAvailableAudioTracks() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	std::string tracks = privObj->_aamp->GetAvailableAudioTracks();
	if (!tracks.empty())
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return aamp_CStringToJSValue(ctx, tracks.c_str());
	}
	else
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return JSValueMakeUndefined(ctx);
	}
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getAvailableTextTracks()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_getAvailableTextTracks(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getAvailableTextTracks() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	std::string tracks = privObj->_aamp->GetAvailableTextTracks();
	if (!tracks.empty())
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return aamp_CStringToJSValue(ctx, tracks.c_str());
	}
	else
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return JSValueMakeUndefined(ctx);
	}
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getVideoRectangle()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_getVideoRectangle(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getVideoRectangle() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return aamp_CStringToJSValue(ctx, privObj->_aamp->GetVideoRectangle().c_str());
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setClosedCaptionStatus()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setClosedCaptionStatus(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setClosedCaptionStatus() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setClosedCaptionStatus() - 1 argument required");
	}
	else
	{
		bool enabled = JSValueToBoolean(ctx, arguments[0]);
		privObj->_aamp->SetCCStatus(enabled);
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setTextStyleOptions()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_setTextStyleOptions(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setTextStyleOptions() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount != 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setTextStyleOptions() - 1 argument required");
	}
	else
	{
		if (JSValueIsString(ctx, arguments[0]))
		{
			const char *options = aamp_JSValueToCString(ctx, arguments[0], NULL);
			privObj->_aamp->SetTextStyle(std::string(options));
			delete[] options;
		}
		else
		{
			ERROR("%s(): InvalidArgument - Argument should be a JSON formatted string!", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Argument should be a JSON formatted string!");
		}
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.getTextStyleOptions()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_getTextStyleOptions(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call getTextStyleOptions() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	std::string options = privObj->_aamp->GetTextStyle();
	if (!options.empty())
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return aamp_CStringToJSValue(ctx, options.c_str());
	}
	else
	{
		TRACELOG("Exit %s()", __FUNCTION__);
		return JSValueMakeUndefined(ctx);
	}
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.setAuxiliaryLanguage()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
static JSValueRef AAMPMediaPlayerJS_setAuxiliaryLanguage(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if(!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call setAuxiliaryLanguage() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	if (argumentCount == 1)
	{
		const char *lang = aamp_JSValueToCString(ctx, arguments[0], NULL);
		std::string language = std::string(lang);
		if (privObj->_aamp->GetAsyncTuneConfig())
		{
			privObj->_aamp->ScheduleTask(AsyncTaskObj(
						[language](void *data)
						{
							PlayerInstanceAAMP *instance = static_cast<PlayerInstanceAAMP *>(data);
							instance->SetAuxiliaryLanguage(language);
						}, (void *) privObj->_aamp));
		}
		else
		{
			privObj->_aamp->SetAuxiliaryLanguage(language);
		}
		delete[] lang;
	}
	else
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute setAuxiliaryLanguage() - 1 argument required");
	}
	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.disableContentRestrictions()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_disableContentRestrictions (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call disableContentRestrictions() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	long grace = 0;
	long time = -1;
	bool eventChange=false;
	bool updateStatus = false;
	if (argumentCount == 1 && JSValueIsObject(ctx, arguments[0]))
	{
		JSValueRef _exception = NULL;
		bool ret = false;
		bool valueAsBoolean = false;
		double valueAsNumber = 0;

		int numRelockParams = sizeof(relockConditionParamNames)/sizeof(relockConditionParamNames[0]);
		JSObjectRef unlockConditionObj = JSValueToObject(ctx, arguments[0], &_exception);
		if (unlockConditionObj == NULL || _exception != NULL)
		{
			ERROR("%s(): InvalidArgument - argument passed is NULL/not a valid object", __FUNCTION__);
			*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute disableContentRestrictions() - object of unlockConditions required");
			return JSValueMakeUndefined(ctx);
		}


		for (int iter = 0; iter < numRelockParams; iter++)
		{
			ret = false;
			switch(relockConditionParamNames[iter].paramType)
			{
			case ePARAM_RELOCKONTIMEOUT:
				ret = ParseJSPropAsNumber(ctx, unlockConditionObj, relockConditionParamNames[iter].paramName, valueAsNumber);
				break;
			case ePARAM_RELOCKONPROGRAMCHANGE:
				ret = ParseJSPropAsBoolean(ctx, unlockConditionObj, relockConditionParamNames[iter].paramName, valueAsBoolean);
				break;
			default: //ePARAM_MAX_COUNT
				ret = false;
				break;
			}

			if(ret)
			{
				updateStatus = true;
				switch(relockConditionParamNames[iter].paramType)
				{
				case ePARAM_RELOCKONTIMEOUT:
					time = (long) valueAsNumber;
					break;
				case ePARAM_RELOCKONPROGRAMCHANGE:
					eventChange = valueAsBoolean;
					break;

				default: //ePARAM_MAX_COUNT
					break;
				}
			}
		}
		if(updateStatus)
		{
			privObj->_aamp->DisableContentRestrictions(grace, time, eventChange);
		}
	}
	else if(argumentCount > 1)
	{
		ERROR("%s(): InvalidArgument - argumentCount=%d, expected: 1 or no argument", __FUNCTION__, argumentCount);
		*exception = aamp_GetException(ctx, AAMPJS_INVALID_ARGUMENT, "Failed to execute disableContentRestrictions() - 1 argument of type IConfig required");
	}
	else
	{
		//No parameter:parental control locking will be disabled until settop reboot, or explicit call to enableContentRestrictions
		grace = -1;
		privObj->_aamp->DisableContentRestrictions(grace, time, eventChange);
	}

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}


/**
 * @brief API invoked from JS when executing AAMPMediaPlayer.enableContentRestrictions()
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef AAMPMediaPlayerJS_enableContentRestrictions (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(thisObject);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Can only call enableContentRestrictions() on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}

	privObj->_aamp->EnableContentRestrictions();

	TRACELOG("Exit %s()", __FUNCTION__);
	return JSValueMakeUndefined(ctx);
}

/**
 * @brief Array containing the AAMPMediaPlayer's statically declared functions
 */
static const JSStaticFunction AAMPMediaPlayer_JS_static_functions[] = {
	{ "load", AAMPMediaPlayerJS_load, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "initConfig", AAMPMediaPlayerJS_initConfig, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "play", AAMPMediaPlayerJS_play, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "detach", AAMPMediaPlayerJS_detach, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "pause", AAMPMediaPlayerJS_pause, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "stop", AAMPMediaPlayerJS_stop, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "seek", AAMPMediaPlayerJS_seek, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getCurrentState", AAMPMediaPlayerJS_getCurrentState, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getDurationSec", AAMPMediaPlayerJS_getDurationSec, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getCurrentPosition", AAMPMediaPlayerJS_getCurrentPosition, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getVideoBitrates", AAMPMediaPlayerJS_getVideoBitrates, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getAudioBitrates", AAMPMediaPlayerJS_getAudioBitrates, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getCurrentVideoBitrate", AAMPMediaPlayerJS_getCurrentVideoBitrate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setVideoBitrate", AAMPMediaPlayerJS_setVideoBitrate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getCurrentAudioBitrate", AAMPMediaPlayerJS_getCurrentAudioBitrate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setAudioBitrate", AAMPMediaPlayerJS_setAudioBitrate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getAudioTrack", AAMPMediaPlayerJS_getAudioTrack, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setAudioTrack", AAMPMediaPlayerJS_setAudioTrack, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getTextTrack", AAMPMediaPlayerJS_getTextTrack, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setTextTrack", AAMPMediaPlayerJS_setTextTrack, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getVolume", AAMPMediaPlayerJS_getVolume, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setVolume", AAMPMediaPlayerJS_setVolume, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setAudioLanguage", AAMPMediaPlayerJS_setAudioLanguage, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getPlaybackRate", AAMPMediaPlayerJS_getPlaybackRate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setPlaybackRate", AAMPMediaPlayerJS_setPlaybackRate, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getSupportedKeySystems", AAMPMediaPlayerJS_getSupportedKeySystems, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setVideoMute", AAMPMediaPlayerJS_setVideoMute, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setSubscribedTags", AAMPMediaPlayerJS_setSubscribedTags, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "addEventListener", AAMPMediaPlayerJS_addEventListener, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "removeEventListener", AAMPMediaPlayerJS_removeEventListener, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setDRMConfig", AAMPMediaPlayerJS_setDRMConfig, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},

	{ "addCustomHTTPHeader", AAMPMediaPlayerJS_addCustomHTTPHeader, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "removeCustomHTTPHeader", AAMPMediaPlayerJS_removeCustomHTTPHeader, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "setVideoRect", AAMPMediaPlayerJS_setVideoRect, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "setVideoZoom", AAMPMediaPlayerJS_setVideoZoom, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "release", AAMPMediaPlayerJS_release, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "setAlternateContent", AAMPMediaPlayerJS_setAlternateContent, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "notifyReservationCompletion", AAMPMediaPlayerJS_notifyReservationCompletion, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "getAvailableAudioTracks", AAMPMediaPlayerJS_getAvailableAudioTracks, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getAvailableTextTracks", AAMPMediaPlayerJS_getAvailableTextTracks, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "getVideoRectangle", AAMPMediaPlayerJS_getVideoRectangle, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setClosedCaptionStatus", AAMPMediaPlayerJS_setClosedCaptionStatus, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "setTextStyleOptions", AAMPMediaPlayerJS_setTextStyleOptions, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "getTextStyleOptions", AAMPMediaPlayerJS_getTextStyleOptions, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "setAuxiliaryLanguage", AAMPMediaPlayerJS_setAuxiliaryLanguage, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "disableContentRestrictions", AAMPMediaPlayerJS_disableContentRestrictions, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "enableContentRestrictions", AAMPMediaPlayerJS_enableContentRestrictions, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "getThumbnail", AAMPMediaPlayerJS_getThumbnails, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ "getAvailableThumbnailTracks", AAMPMediaPlayerJS_getAvailableThumbnailTracks, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setThumbnailTrack", AAMPMediaPlayerJS_setThumbnailTrack, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setPreferredAudioLanguage", AAMPMediaPlayerJS_setPreferredAudioLanguage, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setPreferredAudioRendition", AAMPMediaPlayerJS_setPreferredAudioRendition, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ "setPreferredAudioCodec", AAMPMediaPlayerJS_setPreferredAudioCodec, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ NULL, NULL, 0 }
};


/**
 * @brief API invoked from JS when reading value of AAMPMediaPlayer.version
 * @param[in] ctx JS execution context
 * @param[in] object JSObject to search for the property
 * @param[in] propertyName JSString containing the name of the property to get
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval property's value if object has the property, otherwise NULL
 */
JSValueRef AAMPMediaPlayerJS_getProperty_Version(JSContextRef ctx, JSObjectRef object, JSStringRef propertyName, JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	AAMPMediaPlayer_JS* privObj = (AAMPMediaPlayer_JS*)JSObjectGetPrivate(object);
	if (!privObj)
	{
		ERROR("%s(): Error - JSObjectGetPrivate returned NULL!", __FUNCTION__);
		*exception = aamp_GetException(ctx, AAMPJS_MISSING_OBJECT, "Get property version on instances of AAMPPlayer");
		return JSValueMakeUndefined(ctx);
	}
	return aamp_CStringToJSValue(ctx, AAMP_UNIFIED_VIDEO_ENGINE_VERSION);
}


/**
 * @brief Array containing the AAMPMediaPlayer's statically declared value properties
 */
static const JSStaticValue AAMPMediaPlayer_JS_static_values[] = {
	{ "version", AAMPMediaPlayerJS_getProperty_Version, NULL, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly },
	{ NULL, NULL, NULL, 0 }
};


/**
 * @brief API invoked from JS when an object of AAMPMediaPlayerJS is destroyed
 * @param[in] object JSObject being finalized
 */
void AAMPMediaPlayer_JS_finalize(JSObjectRef object)
{
	ERROR("Enter %s()", __FUNCTION__);

	AAMPMediaPlayer_JS *privObj = (AAMPMediaPlayer_JS *) JSObjectGetPrivate(object);
	if (privObj != NULL)
	{
		if (false == findInGlobalCacheAndRelease(privObj))
		{
			ERROR("%s:%d [WARN] Invoked finalize of a AAMPMediaPlayer_JS object(%p) which was already/being released!!", __FUNCTION__, __LINE__, privObj);
		}
		else
		{
			ERROR("%s:%d [WARN] Cleaned up native object for AAMPMediaPlayer_JS object(%p)!!", __FUNCTION__, __LINE__, privObj);
		}
		JSObjectSetPrivate(object, NULL);
	}
	else
	{
		ERROR("%s:%d Native memory already cleared, since privObj is NULL", __FUNCTION__, __LINE__);
	}

	ERROR("Exit %s()", __FUNCTION__);
}


/**
 * @brief Object declaration of AAMPMediaPlayer JS object
 */
static JSClassDefinition AAMPMediaPlayer_JS_object_def {
	0, /* current (and only) version is 0 */
	kJSClassAttributeNone,
	"__AAMPMediaPlayer",
	NULL,

	AAMPMediaPlayer_JS_static_values,
	AAMPMediaPlayer_JS_static_functions,

	NULL,
	AAMPMediaPlayer_JS_finalize,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};


/**
 * @brief Creates a JavaScript class of AAMPMediaPlayer object for use with JSObjectMake
 * @retval singleton instance of JavaScript class created
 */
static JSClassRef AAMPMediaPlayer_object_ref() {
	static JSClassRef _mediaPlayerObjRef = NULL;
	if (!_mediaPlayerObjRef) {
		_mediaPlayerObjRef = JSClassCreate(&AAMPMediaPlayer_JS_object_def);
	}
	return _mediaPlayerObjRef;
}


/**
 * @brief API invoked when AAMPMediaPlayer is used along with 'new'
 * @param[in] ctx JS execution context
 * @param[in] constructor JSObject that is the constructor being called
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSObject that is the constructor's return value
 */
JSObjectRef AAMPMediaPlayer_JS_class_constructor(JSContextRef ctx, JSObjectRef constructor, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	TRACELOG("Enter %s()", __FUNCTION__);

	std::string appName;
	if (argumentCount > 0)
	{
		if (JSValueIsString(ctx, arguments[0]))
		{
			char *value =  aamp_JSValueToCString(ctx, arguments[0], exception);
			appName.assign(value);
			ERROR("%s:%d AAMPMediaPlayer created with app name: %s", __FUNCTION__, __LINE__, appName.c_str());
			delete[] value;
		}
	}

	AAMPMediaPlayer_JS* privObj = new AAMPMediaPlayer_JS();

	privObj->_ctx = JSContextGetGlobalContext(ctx);
	privObj->_aamp = new PlayerInstanceAAMP(NULL, NULL);
	if (!appName.empty())
	{
		privObj->_aamp->SetAppName(appName);
	}
	privObj->_listeners.clear();

	JSObjectRef newObj = JSObjectMake(ctx, AAMPMediaPlayer_object_ref(), privObj);

	pthread_mutex_lock(&jsMediaPlayerCacheMutex);
	AAMPMediaPlayer_JS::_jsMediaPlayerInstances.push_back(privObj);
	pthread_mutex_unlock(&jsMediaPlayerCacheMutex);

	// Add a dummy event listener without any function callback.
	// Upto JS application to register a common callback function for AAMP to notify ad resolve status
	// or individually as part of setAlternateContent call. NULL checks added in eventlistener to avoid undesired behaviour
	AAMP_JSEventListener::AddEventListener(privObj, AAMP_EVENT_AD_RESOLVED, NULL);

	// Required for viper-player
	JSStringRef fName = JSStringCreateWithUTF8CString("toString");
	JSStringRef fString = JSStringCreateWithUTF8CString("return \"[object __AAMPMediaPlayer]\";");
	JSObjectRef toStringFunc = JSObjectMakeFunction(ctx, NULL, 0, NULL, fString, NULL, 0, NULL);

	JSObjectSetProperty(ctx, newObj, fName, toStringFunc, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, NULL);

	JSStringRelease(fName);
	JSStringRelease(fString);

	TRACELOG("Exit %s()", __FUNCTION__);
	return newObj;
}


/**
 * @brief Class declaration of AAMPMediaPlayer JS object
 */
static JSClassDefinition AAMPMediaPlayer_JS_class_def {
	0, /* current (and only) version is 0 */
	kJSClassAttributeNone,
	"__AAMPMediaPlayer__class",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	AAMPMediaPlayer_JS_class_constructor,
	NULL,
	NULL
};


/**
 * @brief Clear any remaining/active AAMPPlayer instances
 */
void ClearAAMPPlayerInstances(void)
{
	pthread_mutex_lock(&jsMediaPlayerCacheMutex);
	ERROR("Number of active jsmediaplayer instances: %d", AAMPMediaPlayer_JS::_jsMediaPlayerInstances.size());
	while(AAMPMediaPlayer_JS::_jsMediaPlayerInstances.size() > 0)
	{
		AAMPMediaPlayer_JS *obj = AAMPMediaPlayer_JS::_jsMediaPlayerInstances.back();
		releaseNativeResources(obj);
		AAMPMediaPlayer_JS::_jsMediaPlayerInstances.pop_back();
	}
	pthread_mutex_unlock(&jsMediaPlayerCacheMutex);
}

#ifdef PLATCO

class XREReceiver_onEventHandler
{
public:
	static void handle(JSContextRef ctx, std::string method, size_t argumentCount, const JSValueRef arguments[])
	{
		if(handlers_map.count(method) != 0)
		{
			handlers_map.at(method)(ctx, argumentCount, arguments);
		}
		else
		{
			ERROR("[XREReceiver]:%s:%d no handler for method %s", __FUNCTION__, __LINE__, method.c_str());
		}
	}

	static std::string findTextTrackWithLang(JSContextRef ctx, std::string selectedLang)
	{
		const auto textTracks = AampCCManager::GetInstance()->getLastTextTracks();
		ERROR("[XREReceiver]:%s:%d found %d text tracks", __FUNCTION__, __LINE__, (int)textTracks.size());

		if(!selectedLang.empty() && isdigit(selectedLang[0]))
		{
			ERROR("[XREReceiver]:%s:%d trying to parse selected lang as index", __FUNCTION__, __LINE__);
			try
			{
				//input index starts from 1, not from 0, hence '-1'
				int idx = std::stoi(selectedLang)-1;
				ERROR("[XREReceiver]:%s:%d parsed index = %d", __FUNCTION__, __LINE__, idx);
				return textTracks.at(idx).instreamId;
			}
			catch(const std::exception& e)
			{
				ERROR("[XREReceiver]:%s:%d exception during parsing lang selection %s", __FUNCTION__, __LINE__, e.what());
			}
		}

		for(const auto& track : textTracks)
		{
			ERROR("[XREReceiver]:%s:%d found language '%s', expected '%s'", __FUNCTION__, __LINE__, track.language.c_str(), selectedLang.c_str());
			if(selectedLang == track.language)
			{
				return track.instreamId;
			}
		}

		ERROR("[XREReceiver]:%s:%d cannot find text track matching the selected language, defaulting to 'CC1'", __FUNCTION__, __LINE__);
		return "CC1";
	}

private:

	static void handle_onClosedCaptions(JSContextRef ctx, size_t argumentCount, const JSValueRef arguments[])
	{
		ERROR("[XREReceiver]:%s:%d ", __FUNCTION__, __LINE__);
		if(argumentCount != 2)
		{
			ERROR("[XREReceiver]:%s wrong argument count (expected 2) %d", __FUNCTION__, argumentCount);
			return;
		}

		JSObjectRef argument = JSValueToObject(ctx, arguments[1], NULL);

		JSStringRef param_enable 		= JSStringCreateWithUTF8CString("enable");
		JSStringRef param_setOptions	= JSStringCreateWithUTF8CString("setOptions");
		JSStringRef param_setTrack		= JSStringCreateWithUTF8CString("setTrack");

		JSValueRef param_enable_value 		= JSObjectGetProperty(ctx, argument, param_enable, NULL);
		JSValueRef param_setOptions_value	= JSObjectGetProperty(ctx, argument, param_setOptions, NULL);
		JSValueRef param_setTrack_value		= JSObjectGetProperty(ctx, argument, param_setTrack, NULL);

		if(JSValueIsBoolean(ctx, param_enable_value))
		{
			const bool enable_value = JSValueToBoolean(ctx, param_enable_value);
			ERROR("[XREReceiver]:%s:%d received enable boolean %d", __FUNCTION__, __LINE__, enable_value);

#ifdef AAMP_CC_ENABLED
			AampCCManager::GetInstance()->SetStatus(enable_value);
#endif
		}

		if(JSValueIsObject(ctx, param_setOptions_value))
		{
			ERROR("[XREReceiver]:%s:%d received setOptions, ignoring for now", __FUNCTION__, __LINE__);
		}

		if(JSValueIsString(ctx, param_setTrack_value))
		{
			char* lang = aamp_JSValueToCString(ctx, param_setTrack_value, NULL);
			ERROR("[XREReceiver]:%s:%d received setTrack language:  %s", __FUNCTION__, __LINE__, lang);

			std::string lang_str;
			lang_str.assign(lang);
			delete[] lang;

			std::string textTrack = findTextTrackWithLang(ctx, lang_str);

			ERROR("[XREReceiver]:%s:%d selected textTrack = '%s'", __FUNCTION__, __LINE__, textTrack.c_str());

#ifdef AAMP_CC_ENABLED
			AampCCManager::GetInstance()->SetTrack(textTrack);
#endif

		}

		JSStringRelease(param_enable);
		JSStringRelease(param_setOptions);
		JSStringRelease(param_setTrack);
	}

	using Handler_t = std::function<void(JSContextRef, size_t, const JSValueRef[])>;
	static std::unordered_map<std::string, Handler_t> handlers_map;
};

std::unordered_map<std::string, XREReceiver_onEventHandler::Handler_t>  XREReceiver_onEventHandler::handlers_map = {
	{ std::string{"onClosedCaptions"}, &XREReceiver_onEventHandler::handle_onClosedCaptions}
};

// temporary patch to avoid JavaScript exception in webapps referencing XREReceiverObject in builds that don't support it

/**
 * @brief API invoked from JS when executing XREReceiver.oneven() method)
 * @param[in] ctx JS execution context
 * @param[in] function JSObject that is the function being called
 * @param[in] thisObject JSObject that is the 'this' variable in the function's scope
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSValue that is the function's return value
 */
JSValueRef XREReceiverJS_onevent (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
	INFO("[XREReceiver]:%s arg count - %d", __FUNCTION__, argumentCount);
	if (argumentCount > 0)
	{
		if (JSValueIsString(ctx, arguments[0]))
		{
			char* value =  aamp_JSValueToCString(ctx, arguments[0], exception);
			std::string method;
			method.assign(value);
			XREReceiver_onEventHandler::handle(ctx, method, argumentCount, arguments);

			delete[] value;
		}
	}
	return JSValueMakeUndefined(ctx);
}

static const JSStaticFunction XREReceiver_JS_static_functions[] = 
{
	{ "onEvent", XREReceiverJS_onevent, kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly},
	{ NULL, NULL, 0 }
};

/**
 * @brief API invoked when AAMPMediaPlayer is used along with 'new'
 * @param[in] ctx JS execution context
 * @param[in] constructor JSObject that is the constructor being called
 * @param[in] argumentCount number of args
 * @param[in] arguments[] JSValue array of args
 * @param[out] exception pointer to a JSValueRef in which to return an exception, if any
 * @retval JSObject that is the constructor's return value
 */
JSObjectRef XREReceiver_JS_class_constructor(JSContextRef ctx, JSObjectRef constructor, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
        TRACELOG("Enter %s()", __FUNCTION__);
	*exception = aamp_GetException(ctx, AAMPJS_GENERIC_ERROR, "Cannot create an object of XREReceiver");
        TRACELOG("Exit %s()", __FUNCTION__);
	return NULL;
}

static void XREReceiver_JS_finalize(JSObjectRef thisObject)
{
       TRACELOG("%s(): object=%p", __FUNCTION__, thisObject);
}

static JSClassDefinition XREReceiver_JS_class_def {
	0, // version
	kJSClassAttributeNone, // attributes: JSClassAttributes
	"__XREReceiver_class", // className: *const c_char
	NULL, // parentClass: JSClassRef
	NULL, // staticValues: *const JSStaticValue
	XREReceiver_JS_static_functions, // staticFunctions: *const JSStaticFunction
	NULL, // initialize: JSObjectInitializeCallback
	XREReceiver_JS_finalize, // finalize: JSObjectFinalizeCallback
	NULL, // hasProperty: JSObjectHasPropertyCallback
	NULL, // getProperty: JSObjectGetPropertyCallback
	NULL, // setProperty: JSObjectSetPropertyCallback
	NULL, // deleteProperty: JSObjectDeletePropertyCallback
	NULL, // getPropertyNames: JSObjectGetPropertyNamesCallback
	NULL, // callAsFunction: JSObjectCallAsFunctionCallback
	XREReceiver_JS_class_constructor, // callAsConstructor: JSObjectCallAsConstructorCallback,
	NULL, // hasInstance: JSObjectHasInstanceCallback
	NULL  // convertToType: JSObjectConvertToTypeCallback
};

void LoadXREReceiverStub(void* context)
{
	TRACELOG("Enter %s(), context = %p", __FUNCTION__, context);
	JSGlobalContextRef jsContext = (JSGlobalContextRef)context;

	JSClassRef myClass = JSClassCreate(&XREReceiver_JS_class_def);
	JSObjectRef classObj = JSObjectMake(jsContext, myClass, NULL);
	JSObjectRef globalObj = JSContextGetGlobalObject(jsContext);

	JSStringRef str = JSStringCreateWithUTF8CString("XREReceiver");
	JSObjectSetProperty(jsContext, globalObj, str, classObj, kJSPropertyAttributeReadOnly, NULL);

	TRACELOG("Exit %s()", __FUNCTION__);
}
#endif // PLATCO


/**
 * @brief Loads AAMPMediaPlayer JS constructor into JS context
 * @param[in] context JS execution context
 */
void AAMPPlayer_LoadJS(void* context)
{
	TRACELOG("Enter %s()", __FUNCTION__);
	ERROR("Enter %s(), context = %p", __FUNCTION__, context);
	JSGlobalContextRef jsContext = (JSGlobalContextRef)context;

	JSObjectRef globalObj = JSContextGetGlobalObject(jsContext);

	JSClassRef mediaPlayerClass = JSClassCreate(&AAMPMediaPlayer_JS_class_def);
	JSObjectRef classObj = JSObjectMakeConstructor(jsContext, mediaPlayerClass, AAMPMediaPlayer_JS_class_constructor);
	JSValueProtect(jsContext, classObj);

	JSStringRef str = JSStringCreateWithUTF8CString("AAMPMediaPlayer");
	JSObjectSetProperty(jsContext, globalObj, str, classObj, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, NULL);

	JSClassRelease(mediaPlayerClass);
	JSStringRelease(str);
	
#ifdef PLATCO
	LoadXREReceiverStub(context);
#endif
	
	TRACELOG("Exit %s()", __FUNCTION__);
}

/**
 * @brief Removes the AAMPMediaPlayer constructor from JS context
 * @param[in] context JS execution context
 */
void AAMPPlayer_UnloadJS(void* context)
{
	INFO("[AAMP_JS] %s() context=%p", __FUNCTION__, context);

	JSValueRef exception = NULL;
	JSGlobalContextRef jsContext = (JSGlobalContextRef)context;

	//Clear all active js mediaplayer instances and its resources
	ClearAAMPPlayerInstances();

	JSObjectRef globalObj = JSContextGetGlobalObject(jsContext);
	JSStringRef str = JSStringCreateWithUTF8CString("AAMPMediaPlayer");

	JSValueRef playerConstructor = JSObjectGetProperty(jsContext, globalObj, str, &exception);

	if (playerConstructor == NULL || exception != NULL)
	{
		JSStringRelease(str);
		return;
	}

	JSObjectRef playerObj = JSValueToObject(jsContext, playerConstructor, &exception);
	if (playerObj == NULL || exception != NULL)
	{
		JSStringRelease(str);
		return;
	}

	if (!JSObjectIsConstructor(jsContext, playerObj))
	{
		JSStringRelease(str);
		return;
	}

	JSValueUnprotect(jsContext, playerConstructor);
	JSObjectSetProperty(jsContext, globalObj, str, JSValueMakeUndefined(jsContext), kJSPropertyAttributeReadOnly, NULL);
	JSStringRelease(str);

	// Force a garbage collection to clean-up all AAMP objects.
	LOG("[AAMP_JS] JSGarbageCollect() context=%p", context);
	JSGarbageCollect(jsContext);
	TRACELOG("Exit %s()", __FUNCTION__);
}
