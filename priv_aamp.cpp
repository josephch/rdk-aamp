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

/**
 * @file priv_aamp.cpp
 * @brief Advanced Adaptive Media Player (AAMP) PrivateInstanceAAMP impl
 */

#include "priv_aamp.h"
#include "AampConstants.h"
#include "AampCacheHandler.h"
#include "AampUtils.h"
#include "iso639map.h"
#include "fragmentcollector_mpd.h"
#include "admanager_mpd.h"
#include "fragmentcollector_hls.h"
#include "fragmentcollector_progressive.h"
#include "hdmiin_shim.h"
#include "compositein_shim.h"
#include "ota_shim.h"
#include "_base64.h"
#include "base16.h"
#include "aampgstplayer.h"
#include "AampDRMSessionManager.h"
#ifdef AAMP_CC_ENABLED
#include "AampCCManager.h"
#endif
#ifdef USE_OPENCDM // AampOutputProtection is compiled when this  flag is enabled 
#include "aampoutputprotection.h"
#endif

#ifdef IARM_MGR
#include "host.hpp"
#include "manager.hpp"
#include "libIBus.h"
#include "libIBusDaemon.h"
#include <hostIf_tr69ReqHandler.h>
#include <sstream>
#endif
#include <sys/time.h>
#include <cmath>
#include <regex>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <uuid/uuid.h>

#define LOCAL_HOST_IP       "127.0.0.1"
#define AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS (20*1000LL)

//Description size
#define MAX_DESCRIPTION_SIZE 128

//Stringification of Macro :  use two levels of macros
#define MACRO_TO_STRING(s) X_STR(s)
#define X_STR(s) #s

// Uncomment to test GetMediaFormatType without locator inspection
#define TRUST_LOCATOR_EXTENSION_IF_PRESENT

#define VALIDATE_INT(param_name, param_value, default_value)        \
    if ((param_value <= 0) || (param_value > INT_MAX))  { \
        logprintf("%s(): Parameter '%s' not within INTEGER limit. Using default value instead.", __FUNCTION__, param_name); \
        param_value = default_value; \
    }

#define VALIDATE_LONG(param_name, param_value, default_value)        \
    if ((param_value <= 0) || (param_value > LONG_MAX))  { \
        logprintf("%s(): Parameter '%s' not within LONG INTEGER limit. Using default value instead.", __FUNCTION__, param_name); \
        param_value = default_value; \
    }

#define VALIDATE_DOUBLE(param_name, param_value, default_value)        \
    if ((param_value <= 0) || (param_value > DBL_MAX))  { \
        logprintf("%s(): Parameter '%s' not within DOUBLE limit. Using default value instead.", __FUNCTION__, param_name); \
        param_value = default_value; \
    }

#define FOG_REASON_STRING			"Fog-Reason:"
#define FOG_ERROR_STRING			"X-Fog-Error:"
#define CURLHEADER_X_REASON			"X-Reason:"
#define BITRATE_HEADER_STRING		"X-Bitrate:"
#define CONTENTLENGTH_STRING		"Content-Length:"
#define SET_COOKIE_HEADER_STRING	"Set-Cookie:"
#define LOCATION_HEADER_STRING		"Location:"
#define CONTENT_ENCODING_STRING		"Content-Encoding:"
#define FOG_RECORDING_ID_STRING		"Fog-Recording-Id:"

#define STRLEN_LITERAL(STRING) (sizeof(STRING)-1)
#define STARTS_WITH_IGNORE_CASE(STRING, PREFIX) (0 == strncasecmp(STRING, PREFIX, STRLEN_LITERAL(PREFIX)))

/**
 * New state for treating a VOD asset as a "virtual linear" stream
 */
// Note that below state/impl currently assumes single profile, and so until fixed should be tested with "abr" in aamp.cfg to disable ABR
static long long simulation_start; // time at which main manifest was downloaded.
// Simulation_start is used to calculate elapsed time, used to advance virtual live window
static char *full_playlist_video_ptr = NULL; // Cache of initial full vod video playlist
static size_t full_playlist_video_len = 0; // Size (bytes) of initial full vod video playlist
static char *full_playlist_audio_ptr = NULL; // Cache of initial full vod audio playlist
static size_t full_playlist_audio_len = 0; // Size (bytes) of initial full vod audio playlist

/**
 * @struct gActivePrivAAMP_t
 * @brief Used for storing active PrivateInstanceAAMPs
 */
struct gActivePrivAAMP_t
{
	PrivateInstanceAAMP* pAAMP;
	bool reTune;
	int numPtsErrors;
};

static std::list<gActivePrivAAMP_t> gActivePrivAAMPs = std::list<gActivePrivAAMP_t>();

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gCond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gCurlInitMutex = PTHREAD_MUTEX_INITIALIZER;

static int PLAYERID_CNTR = 0;

static const char* strAAMPPipeName = "/tmp/ipc_aamp";

static bool activeInterfaceWifi = false;

GlobalConfigAAMP *gpGlobalConfig;

/**
 * @struct ChannelInfo 
 * @brief Holds information of a channel
 */
struct ChannelInfo
{
	ChannelInfo() : name(), uri()
	{
	}
	std::string name;
	std::string uri;
};

static std::list<ChannelInfo> mChannelOverrideMap;

/**
 * @struct CurlCallbackContext
 * @brief context during curl callbacks
 */
struct CurlCallbackContext
{
	PrivateInstanceAAMP *aamp;
	MediaType fileType;
	std::vector<std::string> allResponseHeadersForErrorLogging;
	GrowableBuffer *buffer;
	httpRespHeaderData *responseHeaderData;
	long bitrate;
	bool downloadIsEncoded;

	CurlCallbackContext() : aamp(NULL), buffer(NULL), responseHeaderData(NULL),bitrate(0),downloadIsEncoded(false), fileType(eMEDIATYPE_DEFAULT), allResponseHeadersForErrorLogging{""}
	{

	}
	CurlCallbackContext(PrivateInstanceAAMP *_aamp, GrowableBuffer *_buffer) : aamp(_aamp), buffer(_buffer), responseHeaderData(NULL),bitrate(0),downloadIsEncoded(false), fileType(eMEDIATYPE_DEFAULT), allResponseHeadersForErrorLogging{""}{}

	~CurlCallbackContext() {}

	CurlCallbackContext(const CurlCallbackContext &other) = delete;
	CurlCallbackContext& operator=(const CurlCallbackContext& other) = delete;
};

/**
 * @struct CurlProgressCbContext
 * @brief context during curl progress callbacks
 */
struct CurlProgressCbContext
{
	PrivateInstanceAAMP *aamp;
	CurlProgressCbContext() : aamp(NULL), downloadStartTime(-1), abortReason(eCURL_ABORT_REASON_NONE), downloadUpdatedTime(-1), startTimeout(-1), stallTimeout(-1), downloadSize(-1) {}
	CurlProgressCbContext(PrivateInstanceAAMP *_aamp, long long _downloadStartTime) : aamp(_aamp), downloadStartTime(_downloadStartTime), abortReason(eCURL_ABORT_REASON_NONE), downloadUpdatedTime(-1), startTimeout(-1), stallTimeout(-1), downloadSize(-1) {}
	long long downloadStartTime;
	long long downloadUpdatedTime;
	long startTimeout;
	long stallTimeout;
	double downloadSize;
	CurlAbortReason abortReason;
};

/**
 * @brief Enumeration for net_srv_mgr active interface event callback
 */
typedef enum _NetworkManager_EventId_t {
        IARM_BUS_NETWORK_MANAGER_EVENT_SET_INTERFACE_ENABLED=50,
        IARM_BUS_NETWORK_MANAGER_EVENT_INTERFACE_IPADDRESS=55,
        IARM_BUS_NETWORK_MANAGER_MAX
} IARM_Bus_NetworkManager_EventId_t;

/**
 * @struct _IARM_BUS_NetSrvMgr_Iface_EventData_t
 * @brief IARM Bus struct contains active streaming interface, origional definition present in homenetworkingservice.h
 */
typedef struct _IARM_BUS_NetSrvMgr_Iface_EventData_t {
	union{
		char activeIface[10];
		char allNetworkInterfaces[50];
		char enableInterface[10];
	};
	char interfaceCount;
	bool isInterfaceEnabled;
} IARM_BUS_NetSrvMgr_Iface_EventData_t;

static TuneFailureMap tuneFailureMap[] =
{
	{AAMP_TUNE_INIT_FAILED, 10, "AAMP: init failed"}, //"Fragmentcollector initialization failed"
	{AAMP_TUNE_INIT_FAILED_MANIFEST_DNLD_ERROR, 10, "AAMP: init failed (unable to download manifest)"},
	{AAMP_TUNE_INIT_FAILED_MANIFEST_CONTENT_ERROR, 10, "AAMP: init failed (manifest missing tracks)"},
	{AAMP_TUNE_INIT_FAILED_MANIFEST_PARSE_ERROR, 10, "AAMP: init failed (corrupt/invalid manifest)"},
	{AAMP_TUNE_INIT_FAILED_PLAYLIST_VIDEO_DNLD_ERROR, 10, "AAMP: init failed (unable to download video playlist)"},
	{AAMP_TUNE_INIT_FAILED_PLAYLIST_AUDIO_DNLD_ERROR, 10, "AAMP: init failed (unable to download audio playlist)"},
	{AAMP_TUNE_INIT_FAILED_TRACK_SYNC_ERROR, 10, "AAMP: init failed (unsynchronized tracks)"},
	{AAMP_TUNE_MANIFEST_REQ_FAILED, 10, "AAMP: Manifest Download failed"}, //"Playlist refresh failed"
	{AAMP_TUNE_AUTHORISATION_FAILURE, 40, "AAMP: Authorization failure"},
	{AAMP_TUNE_FRAGMENT_DOWNLOAD_FAILURE, 10, "AAMP: fragment download failures"},
	{AAMP_TUNE_INIT_FRAGMENT_DOWNLOAD_FAILURE, 10, "AAMP: init fragment download failed"},
	{AAMP_TUNE_UNTRACKED_DRM_ERROR, 50, "AAMP: DRM error untracked error"},
	{AAMP_TUNE_DRM_INIT_FAILED, 50, "AAMP: DRM Initialization Failed"},
	{AAMP_TUNE_DRM_DATA_BIND_FAILED, 50, "AAMP: InitData-DRM Binding Failed"},
	{AAMP_TUNE_DRM_SESSIONID_EMPTY, 50, "AAMP: DRM Session ID Empty"},
	{AAMP_TUNE_DRM_CHALLENGE_FAILED, 50, "AAMP: DRM License Challenge Generation Failed"},
	{AAMP_TUNE_LICENCE_TIMEOUT, 50, "AAMP: DRM License Request Timed out"},
	{AAMP_TUNE_LICENCE_REQUEST_FAILED, 50, "AAMP: DRM License Request Failed"},
	{AAMP_TUNE_INVALID_DRM_KEY, 50, "AAMP: Invalid Key Error, from DRM"},
	{AAMP_TUNE_UNSUPPORTED_STREAM_TYPE, 50, "AAMP: Unsupported Stream Type"}, //"Unable to determine stream type for DRM Init"
	{AAMP_TUNE_UNSUPPORTED_AUDIO_TYPE, 50, "AAMP: No supported Audio Types in Manifest"},
	{AAMP_TUNE_FAILED_TO_GET_KEYID, 50, "AAMP: Failed to parse key id from PSSH"},
	{AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN, 50, "AAMP: Failed to get access token from Auth Service"},
	{AAMP_TUNE_CORRUPT_DRM_DATA, 51, "AAMP: DRM failure due to Corrupt DRM files"},
	{AAMP_TUNE_CORRUPT_DRM_METADATA, 50, "AAMP: DRM failure due to Bad DRMMetadata in stream"},
	{AAMP_TUNE_DRM_DECRYPT_FAILED, 50, "AAMP: DRM Decryption Failed for Fragments"},
	{AAMP_TUNE_DRM_UNSUPPORTED, 50, "AAMP: DRM format Unsupported"},
	{AAMP_TUNE_DRM_SELF_ABORT, 50, "AAMP: DRM license request aborted by player"},
	{AAMP_TUNE_GST_PIPELINE_ERROR, 80, "AAMP: Error from gstreamer pipeline"},
	{AAMP_TUNE_PLAYBACK_STALLED, 7600, "AAMP: Playback was stalled due to lack of new fragments"},
	{AAMP_TUNE_CONTENT_NOT_FOUND, 20, "AAMP: Resource was not found at the URL(HTTP 404)"},
	{AAMP_TUNE_DRM_KEY_UPDATE_FAILED, 50, "AAMP: Failed to process DRM key"},
	{AAMP_TUNE_DEVICE_NOT_PROVISIONED, 52, "AAMP: Device not provisioned"},
	{AAMP_TUNE_HDCP_COMPLIANCE_ERROR, 53, "AAMP: HDCP Compliance Check Failure"},
	{AAMP_TUNE_INVALID_MANIFEST_FAILURE, 10, "AAMP: Invalid Manifest, parse failed"},
	{AAMP_TUNE_FAILED_PTS_ERROR, 80, "AAMP: Playback failed due to PTS error"},
	{AAMP_TUNE_MP4_INIT_FRAGMENT_MISSING, 10, "AAMP: init fragments missing in playlist"},
	{AAMP_TUNE_FAILURE_UNKNOWN, 100, "AAMP: Unknown Failure"}
};

static constexpr const char *BITRATECHANGE_STR[] =
{
	(const char *)"BitrateChanged - Network adaptation",				// eAAMP_BITRATE_CHANGE_BY_ABR
	(const char *)"BitrateChanged - Rampdown due to network failure",		// eAAMP_BITRATE_CHANGE_BY_RAMPDOWN
	(const char *)"BitrateChanged - Reset to default bitrate due to tune",		// eAAMP_BITRATE_CHANGE_BY_TUNE
	(const char *)"BitrateChanged - Reset to default bitrate due to seek",		// eAAMP_BITRATE_CHANGE_BY_SEEK
	(const char *)"BitrateChanged - Reset to default bitrate due to trickplay",	// eAAMP_BITRATE_CHANGE_BY_TRICKPLAY
	(const char *)"BitrateChanged - Rampup since buffers are full",			// eAAMP_BITRATE_CHANGE_BY_BUFFER_FULL
	(const char *)"BitrateChanged - Rampdown since buffers are empty",		// eAAMP_BITRATE_CHANGE_BY_BUFFER_EMPTY
	(const char *)"BitrateChanged - Network adaptation by FOG",			// eAAMP_BITRATE_CHANGE_BY_FOG_ABR
	(const char *)"BitrateChanged - Unknown reason"					// eAAMP_BITRATE_CHANGE_MAX
};

#define BITRATEREASON2STRING(id) BITRATECHANGE_STR[id]

static constexpr const char *ADEVENT_STR[] =
{
	(const char *)"AAMP_EVENT_AD_RESERVATION_START",
	(const char *)"AAMP_EVENT_AD_RESERVATION_END",
	(const char *)"AAMP_EVENT_AD_PLACEMENT_START",
	(const char *)"AAMP_EVENT_AD_PLACEMENT_END",
	(const char *)"AAMP_EVENT_AD_PLACEMENT_ERROR",
	(const char *)"AAMP_EVENT_AD_PLACEMENT_PROGRESS"
};

#define ADEVENT2STRING(id) ADEVENT_STR[id - AAMP_EVENT_AD_RESERVATION_START]

/**
 * @brief Get the idle task's source ID
 * @retval source ID
 */
static guint aamp_GetSourceID()
{
	guint callbackId = 0;
	GSource *source = g_main_current_source();
	if (source != NULL)
	{
		callbackId = g_source_get_id(source);
	}
	return callbackId;
}

/**
 * @brief Idle task to resume aamp
 * @param ptr pointer to PrivateInstanceAAMP object
 * @retval 
 */
static gboolean PrivateInstanceAAMP_Resume(gpointer ptr)
{
	bool retValue = true;
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP* )ptr;
	aamp->NotifyFirstBufferProcessed();
	if (aamp->pipeline_paused)
	{
		if (aamp->rate == AAMP_NORMAL_PLAY_RATE)
		{
			retValue = aamp->mStreamSink->Pause(false, false);
			aamp->pipeline_paused = false;
		}
		else
		{
			aamp->rate = AAMP_NORMAL_PLAY_RATE;
			aamp->pipeline_paused = false;
			aamp->TuneHelper(eTUNETYPE_SEEK);
		}
		aamp->ResumeDownloads();

		if(retValue)
		{
			aamp->NotifySpeedChanged(aamp->rate);
		}
	}
	return G_SOURCE_REMOVE;
}

/**
 * @brief Idle task to process discontinuity
 * @param ptr pointer to PrivateInstanceAAMP object
 * @retval G_SOURCE_REMOVE
 */
static gboolean PrivateInstanceAAMP_ProcessDiscontinuity(gpointer ptr)
{
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP*) ptr;

	if (!g_source_is_destroyed(g_main_current_source()))
	{
		bool ret = aamp->ProcessPendingDiscontinuity();
		// This is to avoid calling cond signal, in case Stop() interrupts the ProcessPendingDiscontinuity
		if (ret)
		{
			aamp->SyncBegin();
			aamp->mDiscontinuityTuneOperationId = 0;
			pthread_cond_signal(&aamp->mCondDiscontinuity);
			aamp->SyncEnd();
		}
	}
	return G_SOURCE_REMOVE;
}

/**
 * @brief Tune again to currently viewing asset. Used for internal error handling
 * @param ptr pointer to PrivateInstanceAAMP object
 * @retval G_SOURCE_REMOVE
 */
static gboolean PrivateInstanceAAMP_Retune(gpointer ptr)
{
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP*) ptr;
	bool activeAAMPFound = false;
	bool reTune = false;
	gActivePrivAAMP_t *gAAMPInstance = NULL;
	pthread_mutex_lock(&gMutex);
	for (std::list<gActivePrivAAMP_t>::iterator iter = gActivePrivAAMPs.begin(); iter != gActivePrivAAMPs.end(); iter++)
	{
		if (aamp == iter->pAAMP)
		{
			gAAMPInstance = &(*iter);
			activeAAMPFound = true;
			reTune = gAAMPInstance->reTune;
			break;
		}
	}
	if (!activeAAMPFound)
	{
		logprintf("PrivateInstanceAAMP::%s : %p not in Active AAMP list", __FUNCTION__, aamp);
	}
	else if (!reTune)
	{
		logprintf("PrivateInstanceAAMP::%s : %p reTune flag not set", __FUNCTION__, aamp);
	}
	else
	{
		aamp->mIsRetuneInProgress = true;
		pthread_mutex_unlock(&gMutex);

		aamp->TuneHelper(eTUNETYPE_RETUNE);

		pthread_mutex_lock(&gMutex);
		aamp->mIsRetuneInProgress = false;
		gAAMPInstance->reTune = false;
		pthread_cond_signal(&gCond);
	}
	pthread_mutex_unlock(&gMutex);
	return G_SOURCE_REMOVE;
}

/**
 * @brief Function invoked at the end of idle task for freeing resources
 * @param user_data pointer to AsyncEventDescriptor object
 */
static void AsyncEventDestroyNotify(gpointer user_data)
{
	AsyncEventDescriptor* e = (AsyncEventDescriptor*)user_data;
	delete e;
}

/**
 * @brief Idle task for sending asynchronous event
 * @param user_data pointer to AsyncEventDescriptor object
 * @retval G_SOURCE_REMOVE
 */
static gboolean SendAsynchronousEvent(gpointer user_data)
{
	//TODO protect mEventListener
	AsyncEventDescriptor* e = (AsyncEventDescriptor*)user_data;
	//Get current idle handler's id
	guint callbackID = aamp_GetSourceID();
	if (callbackID != 0)
	{
		e->aamp->SetCallbackAsDispatched(callbackID);
	}
	else
	{
		AAMPLOG_ERR("PrivateInstanceAAMP::%s:%d [type = %d] aamp_GetSourceID returned zero, which is unexpected behavior!", __FUNCTION__, __LINE__, e->event->getType());
	}
	e->aamp->SendEventSync(e->event);
	return G_SOURCE_REMOVE;
}

/**
 * @brief Simulate VOD asset as a "virtual linear" stream.
 */
static void SimulateLinearWindow( struct GrowableBuffer *buffer, const char *ptr, size_t len )
{
	// Calculate elapsed time in seconds since virtual linear stream started
	float cull = (aamp_GetCurrentTimeMS() - simulation_start)/1000.0;
	buffer->len = 0; // Reset Growable Buffer length
	float window = 20.0; // Virtual live window size; can be increased/decreasedint
	const char *fin = ptr+len;
	bool wroteHeader = false; // Internal state used to decide whether HLS playlist header has already been output
	int seqNo = 0;

	while (ptr < fin)
	{
		int count = 0;
		char line[1024];
		float fragmentDuration;

		for(;;)
		{
			char c = *ptr++;
			line[count++] = c;
			if( ptr>=fin || c<' ' ) break;
		}

		line[count] = 0x00;

		if (sscanf(line,"#EXTINF:%f",&fragmentDuration) == 1)
		{
			if (cull > 0)
			{
				cull -= fragmentDuration;
				seqNo++;
				continue; // Not yet in active window
			}

			if (!wroteHeader)
			{
				// Write a simple linear HLS header, without the type:VOD, and with dynamic media sequence number
				wroteHeader = true;
				char header[1024];
				sprintf( header,
					"#EXTM3U\n"
					"#EXT-X-VERSION:3\n"
					"#EXT-X-TARGETDURATION:2\n"
					"#EXT-X-MEDIA-SEQUENCE:%d\n", seqNo );
				aamp_AppendBytes(buffer, header, strlen(header) );
			}

			window -= fragmentDuration;

			if (window < 0.0)
			{
				// Finished writing virtual linear window
				break;
			}
		}

		if (wroteHeader)
		{
			aamp_AppendBytes(buffer, line, count );
		}
	}

	// Following can be used to debug
	// aamp_AppendNulTerminator( buffer );
	// printf( "Virtual Linear Playlist:\n%s\n***\n", buffer->ptr );
}

/**
 * @brief Get the telemetry type for a media type
 * @param type media type
 * @retval telemetry type
 */
static MediaTypeTelemetry aamp_GetMediaTypeForTelemetry(MediaType type)
{
	MediaTypeTelemetry ret;
	switch(type)
	{
			case eMEDIATYPE_VIDEO:
			case eMEDIATYPE_AUDIO:
			case eMEDIATYPE_SUBTITLE:
			case eMEDIATYPE_IFRAME:
						ret = eMEDIATYPE_TELEMETRY_AVS;
						break;
			case eMEDIATYPE_MANIFEST:
			case eMEDIATYPE_PLAYLIST_VIDEO:
			case eMEDIATYPE_PLAYLIST_AUDIO:
			case eMEDIATYPE_PLAYLIST_SUBTITLE:
			case eMEDIATYPE_PLAYLIST_IFRAME:
						ret = eMEDIATYPE_TELEMETRY_MANIFEST;
						break;
			case eMEDIATYPE_INIT_VIDEO:
			case eMEDIATYPE_INIT_AUDIO:
			case eMEDIATYPE_INIT_SUBTITLE:
			case eMEDIATYPE_INIT_IFRAME:
						ret = eMEDIATYPE_TELEMETRY_INIT;
						break;
			case eMEDIATYPE_LICENCE:
						ret = eMEDIATYPE_TELEMETRY_DRM;
						break;
			default:
						ret = eMEDIATYPE_TELEMETRY_UNKNOWN;
						break;
	}
	return ret;
}

/**
 * @brief Check and replace if local overrides match manifest url
 * @param mainManifestUrl manifest url
 * @retval url to override or NULL
 */
static const char *RemapManifestUrl(const char *mainManifestUrl)
{
	if (!mChannelOverrideMap.empty())
	{
		for (std::list<ChannelInfo>::iterator it = mChannelOverrideMap.begin(); it != mChannelOverrideMap.end(); ++it)
		{
			ChannelInfo &pChannelInfo = *it;
			if (strstr(mainManifestUrl, pChannelInfo.name.c_str()))
			{
				logprintf("override!");
				return pChannelInfo.uri.c_str();
			}
		}
	}
	return NULL;
}

/**
 * @brief de-fog playback URL to play directly from CDN instead of fog
 * @param[in][out] dst Buffer containing URL
 */
static void DeFog(std::string& url)
{
	std::string prefix("&recordedUrl=");
	size_t startPos = url.find(prefix);
	if( startPos != std::string::npos )
	{
		startPos += prefix.size();
		size_t len = url.find( '&',startPos );
		if( len != std::string::npos )
		{
			len -= startPos;
		}
		url = url.substr(startPos,len);
		aamp_DecodeUrlParameter(url);
	}
}

/**
 * @brief replace all occurrences of existingSubStringToReplace in str with replacementString
 * @param str string to be scanned/modified
 * @param existingSubStringToReplace substring to be replaced
 * @param replacementString string to be substituted
 * @retval true iff str was modified
 */
static bool replace(std::string &str, const char *existingSubStringToReplace, const char *replacementString)
{
	bool rc = false;
	std::size_t fromPos = 0;
	size_t existingSubStringToReplaceLen = 0;
	size_t replacementStringLen = 0;
	for(;;)
	{
		std::size_t pos = str.find(existingSubStringToReplace,fromPos);
		if( pos == std::string::npos )
		{ // done - pattern not found
			break;
		}
		if( !rc )
		{ // lazily meaasure input strings - no need to measure unless match found
			rc = true;
			existingSubStringToReplaceLen = strlen(existingSubStringToReplace);
			replacementStringLen = strlen(replacementString);
		}
		str.replace( pos, existingSubStringToReplaceLen, replacementString );
		fromPos  = pos + replacementStringLen;
	}
	return rc;
}

// Helper functions for loading configuration (from file/TR181)

#ifdef IARM_MGR
/**
 * @brief
 * @param paramName
 * @param iConfigLen
 * @retval
 */
char * GetTR181AAMPConfig(const char * paramName, size_t & iConfigLen)
{
	char *  strConfig = NULL;
	IARM_Result_t result; 
	HOSTIF_MsgData_t param;
	memset(&param,0,sizeof(param));
	snprintf(param.paramName,TR69HOSTIFMGR_MAX_PARAM_LEN,"%s",paramName);
	param.reqType = HOSTIF_GET;

	result = IARM_Bus_Call(IARM_BUS_TR69HOSTIFMGR_NAME,IARM_BUS_TR69HOSTIFMGR_API_GetParams,
                    (void *)&param,	sizeof(param));
	if(result  == IARM_RESULT_SUCCESS)
	{
		if(fcNoFault == param.faultCode)
		{
			if(param.paramtype == hostIf_StringType && param.paramLen > 0 )
			{
				std::string strforLog(param.paramValue,param.paramLen);

				iConfigLen = param.paramLen;
				const char *src = (const char*)(param.paramValue);
				strConfig = (char * ) base64_Decode(src,&iConfigLen);

				logprintf("GetTR181AAMPConfig: Got:%s En-Len:%d Dec-len:%d",strforLog.c_str(),param.paramLen,iConfigLen);
			}
			else
			{
				logprintf("GetTR181AAMPConfig: Not a string param type=%d or Invalid len:%d ",param.paramtype, param.paramLen);
			}
		}
	}
	else
	{
		logprintf("GetTR181AAMPConfig: Failed to retrieve value result=%d",result);
	}
	return strConfig;
}
/**
 * @brief Active interface state change from netsrvmgr
 * @param owner reference to net_srv_mgr
 * @param IARM eventId received
 * @data pointer reference to interface struct
 */
void getActiveInterfaceEventHandler (const char *owner, IARM_EventId_t eventId, void *data, size_t len)
{

	if (strcmp (owner, "NET_SRV_MGR") != 0)
		return;

	IARM_BUS_NetSrvMgr_Iface_EventData_t *param = (IARM_BUS_NetSrvMgr_Iface_EventData_t *) data;

	AAMPLOG_WARN("getActiveInterfaceEventHandler EventId %d activeinterface %s", eventId,  param->activeIface);

	if (NULL != strstr (param->activeIface, "wlan"))
	{
		 activeInterfaceWifi = true;
	}
	else if (NULL != strstr (param->activeIface, "eth"))
	{
		 activeInterfaceWifi = false;
	}
}
#endif

/**
 * @brief Active streaming interface is wifi
 *
 * @return bool - true if wifi interface connected
 */
static bool IsActiveStreamingInterfaceWifi (void)
{
        bool wifiStatus = false;
#ifdef IARM_MGR
        IARM_Result_t ret = IARM_RESULT_SUCCESS;
        IARM_BUS_NetSrvMgr_Iface_EventData_t param;

        ret = IARM_Bus_Call("NET_SRV_MGR", "getActiveInterface", (void*)&param, sizeof(param));
        if (ret != IARM_RESULT_SUCCESS) {
                AAMPLOG_ERR("NET_SRV_MGR getActiveInterface read failed : %d", ret);
        }
        else
        {
                logprintf("NET_SRV_MGR getActiveInterface = %s", param.activeIface);
                if (!strcmp(param.activeIface, "WIFI")){
                        wifiStatus = true;
                }
        }
        IARM_Bus_RegisterEventHandler("NET_SRV_MGR", IARM_BUS_NETWORK_MANAGER_EVENT_INTERFACE_IPADDRESS, getActiveInterfaceEventHandler);
#endif
        return wifiStatus;
}

/**
* @brief helper function to avoid dependency on unsafe sscanf while reading strings
* @param bufPtr pointer to CString buffer to scan
* @param prefixPtr - prefix string to match in bufPtr
* @param valueCopyPtr receives allocated copy of string following prefix (skipping delimiting whitesace) if prefix found
* @retval 0 if prefix not present or error
* @retval 1 if string extracted/copied to valueCopyPtr
*/
static int ReadConfigStringHelper(std::string buf, const char *prefixPtr, const char **valueCopyPtr)
{
	int ret = 0;
	if (buf.find(prefixPtr) != std::string::npos)
	{
		std::size_t pos = strlen(prefixPtr);
		if (*valueCopyPtr != NULL)
		{
			free((void *)*valueCopyPtr);
			*valueCopyPtr = NULL;
		}
		*valueCopyPtr = strdup(buf.substr(pos).c_str());
		if (*valueCopyPtr)
		{
			ret = 1;
		}
	}
	return ret;
}

/**
* @brief helper function to extract numeric value from given buf after removing prefix
* @param buf String buffer to scan
* @param prefixPtr - prefix string to match in bufPtr
* @param value - receives numeric value after extraction
* @retval 0 if prefix not present or error
* @retval 1 if string converted to numeric value
*/
template<typename T>
static int ReadConfigNumericHelper(std::string buf, const char* prefixPtr, T& value)
{
	int ret = 0;

	try
	{
		std::size_t pos = buf.rfind(prefixPtr,0); // starts with check
		if (pos != std::string::npos)
		{
			pos += strlen(prefixPtr);
			std::string valStr = buf.substr(pos);
			if (std::is_same<T, int>::value)
				value = std::stoi(valStr);
			else if (std::is_same<T, long>::value)
				value = std::stol(valStr);
			else if (std::is_same<T, float>::value)
				value = std::stof(valStr);
			else
				value = std::stod(valStr);
			ret = 1;
		}
	}
	catch(exception& e)
	{
		// NOP
	}

	return ret;
}

/**
 * @brief Process config entries,i and update gpGlobalConfig params
 *        based on the config setting.
 * @param cfg config to process
 */
static void ProcessConfigEntry(std::string cfg)
{
	if (!cfg.empty() && cfg.at(0) != '#')
	{ // ignore comments
		//Removing unnecessary spaces and newlines
		trim(cfg);

		//CID:98018- Remove the seconds variable which is declared but not used
		double inputTimeout;
		int value;
		char * tmpValue = NULL;
		if(ReadConfigStringHelper(cfg, "map-mpd=", (const char**)&gpGlobalConfig->mapMPD))
		{
			logprintf("map-mpd=%s", gpGlobalConfig->mapMPD);
		}
		else if(ReadConfigStringHelper(cfg, "map-m3u8=", (const char**)&gpGlobalConfig->mapM3U8))
		{
			logprintf("map-m3u8=%s", gpGlobalConfig->mapM3U8);
		}
		else if (ReadConfigNumericHelper(cfg, "fragmp4-license-prefetch=", value) == 1)
		{
			gpGlobalConfig->fragmp4LicensePrefetch = (value != 0);
			logprintf("fragmp4-license-prefetch=%d", gpGlobalConfig->fragmp4LicensePrefetch);
		}
		else if (ReadConfigNumericHelper(cfg, "enable_videoend_event=", value) == 1)
		{
			gpGlobalConfig->mEnableVideoEndEvent = (value==1);
			logprintf("enable_videoend_event=%d", gpGlobalConfig->mEnableVideoEndEvent);
		}
		else if (ReadConfigNumericHelper(cfg, "fog=", value) == 1)
		{
			gpGlobalConfig->noFog = (value==0);
			logprintf("fog=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "harvest-count-limit=", gpGlobalConfig->harvestCountLimit) == 1)
		{
			logprintf("harvest-count-limit=%d", gpGlobalConfig->harvestCountLimit);
		}
		else if (ReadConfigNumericHelper(cfg, "harvest-config=", gpGlobalConfig->harvestConfig) == 1)
		{
			logprintf("harvest-config=%d", gpGlobalConfig->harvestConfig);
		}
		else if (ReadConfigStringHelper(cfg, "harvest-path=", (const char**)&gpGlobalConfig->harvestPath))
		{
				logprintf("harvest-path=%s\n", gpGlobalConfig->harvestPath);
		}
		else if (ReadConfigNumericHelper(cfg, "forceEC3=", value) == 1)
		{
			gpGlobalConfig->forceEC3 = (TriState)(value != 0);
			logprintf("forceEC3=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "disableEC3=", value) == 1)
		{
			gpGlobalConfig->disableEC3 = (TriState)(value != 0);
			logprintf("disableEC3=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "disableATMOS=", value) == 1)
		{
			gpGlobalConfig->disableATMOS = (TriState)(value != 0);
			logprintf("disableATMOS=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "cdvrlive-offset=", gpGlobalConfig->cdvrliveOffset) == 1)
		{
                        VALIDATE_INT("cdvrlive-offset", gpGlobalConfig->cdvrliveOffset, AAMP_CDVR_LIVE_OFFSET)
			logprintf("cdvrlive-offset=%d", gpGlobalConfig->cdvrliveOffset);
		}
		else if (ReadConfigNumericHelper(cfg, "live-offset=", gpGlobalConfig->liveOffset) == 1)
		{
                        VALIDATE_INT("live-offset", gpGlobalConfig->liveOffset, AAMP_LIVE_OFFSET)
			logprintf("live-offset=%d", gpGlobalConfig->liveOffset);
		}
		else if (ReadConfigNumericHelper(cfg, "disablePlaylistIndexEvent=", gpGlobalConfig->disablePlaylistIndexEvent) == 1)
		{
			logprintf("disablePlaylistIndexEvent=%d", gpGlobalConfig->disablePlaylistIndexEvent);
		}
		else if (cfg.compare("enableSubscribedTags") == 0)
		{
			gpGlobalConfig->enableSubscribedTags = true;
			logprintf("enableSubscribedTags set");
		}
		else if (cfg.compare("disableSubscribedTags") == 0)
		{
			gpGlobalConfig->enableSubscribedTags = false;
			logprintf("disableSubscribedTags set");
		}
		else if (ReadConfigNumericHelper(cfg, "enableSubscribedTags=", gpGlobalConfig->enableSubscribedTags) == 1)
		{
			logprintf("enableSubscribedTags=%d", gpGlobalConfig->enableSubscribedTags);
		}
		else if (ReadConfigNumericHelper(cfg, "networkTimeout=", inputTimeout) == 1)
		{
			VALIDATE_DOUBLE("networkTimeout", inputTimeout, CURL_FRAGMENT_DL_TIMEOUT)
			gpGlobalConfig->networkTimeoutMs = (long)CONVERT_SEC_TO_MS(inputTimeout);
			logprintf("networkTimeout=%ld", gpGlobalConfig->networkTimeoutMs);
		}
		else if (ReadConfigNumericHelper(cfg, "manifestTimeout=", inputTimeout) == 1)
		{
			VALIDATE_DOUBLE("manifestTimeout", inputTimeout, CURL_FRAGMENT_DL_TIMEOUT)
			gpGlobalConfig->manifestTimeoutMs = (long)CONVERT_SEC_TO_MS(inputTimeout);
			logprintf("manifestTimeout=%ld ms", gpGlobalConfig->manifestTimeoutMs);
		}
		else if (ReadConfigNumericHelper(cfg, "playlistTimeout=", inputTimeout) == 1)
		{
			VALIDATE_DOUBLE("playlist", inputTimeout, CURL_FRAGMENT_DL_TIMEOUT)
			gpGlobalConfig->playlistTimeoutMs = (long)CONVERT_SEC_TO_MS(inputTimeout);
			logprintf("playlistTimeout=%ld ms", gpGlobalConfig->playlistTimeoutMs);
		}
		else if (cfg.compare("dash-ignore-base-url-if-slash") == 0)
		{
			gpGlobalConfig->dashIgnoreBaseURLIfSlash = true;
			logprintf("dash-ignore-base-url-if-slash set");
		}
		else if (cfg.compare("license-anonymous-request") == 0)
		{
			gpGlobalConfig->licenseAnonymousRequest = true;
			logprintf("license-anonymous-request set");
		}
		else if (cfg.compare("useLinearSimulator") == 0)
		{
			gpGlobalConfig->useLinearSimulator = true;
			logprintf("useLinearSimulator set");
		}
		else if ((cfg.compare("info") == 0) && (!gpGlobalConfig->logging.debug))
		{
			gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_INFO);
			gpGlobalConfig->logging.info = true;
			logprintf("info logging %s", gpGlobalConfig->logging.info ? "on" : "off");
		}
		else if (cfg.compare("stream") == 0) 
		{
			gpGlobalConfig->logging.stream = true;
			logprintf("stream logging %s", gpGlobalConfig->logging.stream ? "on" : "off");
		}
		else if (cfg.compare("failover") == 0)
		{
			gpGlobalConfig->logging.failover = true;
			logprintf("failover logging %s", gpGlobalConfig->logging.failover ? "on" : "off");
		}
		else if (cfg.compare("curlHeader") == 0)
		{
			gpGlobalConfig->logging.curlHeader = true;
			logprintf("curlHeader logging %s", gpGlobalConfig->logging.curlHeader ? "on" : "off");
		}
		else if (cfg.compare("curlLicense") == 0)
                {
                        gpGlobalConfig->logging.curlLicense = true;
                        logprintf("curlLicense logging %s", gpGlobalConfig->logging.curlLicense ? "on" : "off");
                }
		else if(cfg.compare("logMetadata") == 0)
		{
			gpGlobalConfig->logging.logMetadata = true;
			logprintf("logMetadata logging %s", gpGlobalConfig->logging.logMetadata ? "on" : "off");
		}
		else if (ReadConfigStringHelper(cfg, "customHeader=", (const char**)&tmpValue))
		{
			if (tmpValue)
			{
				gpGlobalConfig->customHeaderStr.push_back(tmpValue);
				logprintf("customHeader = %s", tmpValue);

				free(tmpValue);
				tmpValue = NULL;
			}
		}
		else if (ReadConfigStringHelper(cfg, "uriParameter=", (const char**)&gpGlobalConfig->uriParameter))
		{
			logprintf("uriParameter = %s", gpGlobalConfig->uriParameter);
		}
		else if (cfg.compare("gst") == 0)
		{
			gpGlobalConfig->logging.gst = !gpGlobalConfig->logging.gst;
			logprintf("gst logging %s", gpGlobalConfig->logging.gst ? "on" : "off");
		}
		else if (cfg.compare("progress") == 0)
		{
			gpGlobalConfig->logging.progress = !gpGlobalConfig->logging.progress;
			logprintf("progress logging %s", gpGlobalConfig->logging.progress ? "on" : "off");
		}
		else if (cfg.compare("debug") == 0)
		{
			gpGlobalConfig->logging.info = false;
			gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_TRACE);
			gpGlobalConfig->logging.debug = true;
			logprintf("debug logging %s", gpGlobalConfig->logging.debug ? "on" : "off");
		}
		else if (cfg.compare("trace") == 0)
		{
			gpGlobalConfig->logging.trace = !gpGlobalConfig->logging.trace;
			logprintf("trace logging %s", gpGlobalConfig->logging.trace ? "on" : "off");
		}
		else if (cfg.compare("curl") == 0)
		{
			gpGlobalConfig->logging.curl = !gpGlobalConfig->logging.curl;
			logprintf("curl logging %s", gpGlobalConfig->logging.curl ? "on" : "off");
		}
		else if (ReadConfigNumericHelper(cfg, "default-bitrate=", gpGlobalConfig->defaultBitrate) == 1)
		{
			VALIDATE_LONG("default-bitrate",gpGlobalConfig->defaultBitrate, DEFAULT_INIT_BITRATE)
			logprintf("aamp default-bitrate: %ld", gpGlobalConfig->defaultBitrate);
		}
		else if (ReadConfigNumericHelper(cfg, "default-bitrate-4k=", gpGlobalConfig->defaultBitrate4K) == 1)
		{
			VALIDATE_LONG("default-bitrate-4k", gpGlobalConfig->defaultBitrate4K, DEFAULT_INIT_BITRATE_4K)
			logprintf("aamp default-bitrate-4k: %ld", gpGlobalConfig->defaultBitrate4K);
		}
		else if (cfg.compare("abr") == 0)
		{
			gpGlobalConfig->bEnableABR = !gpGlobalConfig->bEnableABR;
			logprintf("abr %s", gpGlobalConfig->bEnableABR ? "on" : "off");
		}
		else if (ReadConfigNumericHelper(cfg, "abr-cache-life=", gpGlobalConfig->abrCacheLife) == 1)
		{
			gpGlobalConfig->abrCacheLife *= 1000;
			logprintf("aamp abr cache lifetime: %ldmsec", gpGlobalConfig->abrCacheLife);
		}
		else if (ReadConfigNumericHelper(cfg, "abr-cache-length=", gpGlobalConfig->abrCacheLength) == 1)
		{
			VALIDATE_INT("abr-cache-length", gpGlobalConfig->abrCacheLength, DEFAULT_ABR_CACHE_LENGTH)
			logprintf("aamp abr cache length: %ld", gpGlobalConfig->abrCacheLength);
		}
		else if (ReadConfigNumericHelper(cfg, "useNewABR=", value) == 1)
		{
			gpGlobalConfig->abrBufferCheckEnabled  = (TriState)(value != 0);
			logprintf("useNewABR =%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "useNewAdBreaker=", value) == 1)
		{
			gpGlobalConfig->useNewDiscontinuity  = (TriState)(value != 0);
			logprintf("useNewAdBreaker =%d", value);
		}
		else if (cfg.compare("reportvideopts") == 0)
		{
			gpGlobalConfig->bReportVideoPTS = eTrueState;
			logprintf("reportvideopts:%s", gpGlobalConfig->bReportVideoPTS ? "on" : "off");
		}
		else if (cfg.compare("decoderunavailablestrict") == 0)
		{
			gpGlobalConfig->decoderUnavailableStrict = true;
			logprintf("decoderunavailablestrict:%s", gpGlobalConfig->decoderUnavailableStrict ? "on" : "off");
		}
		else if( cfg.compare("descriptiveaudiotrack") == 0 )
		{
			gpGlobalConfig->bDescriptiveAudioTrack  = true;
			logprintf("descriptiveaudiotrack:%s", gpGlobalConfig->bDescriptiveAudioTrack ? "on" : "off");
		}
		else if( ReadConfigNumericHelper( cfg, "langcodepref=", value) == 1 )
		{
			const char *langCodePrefName[] =
			{
				"ISO639_NO_LANGCODE_PREFERENCE",
				"ISO639_PREFER_3_CHAR_BIBLIOGRAPHIC_LANGCODE",
				"ISO639_PREFER_3_CHAR_TERMINOLOGY_LANGCODE",
				"ISO639_PREFER_2_CHAR_LANGCODE"
			};
			if( value>=0 && value<4 )
			{
				gpGlobalConfig->langCodePreference = (LangCodePreference)value;
				logprintf("langcodepref:%s\n", langCodePrefName[gpGlobalConfig->langCodePreference] );
			}
		}
		else if (cfg.compare("appSrcForProgressivePlayback") == 0)
		{
			gpGlobalConfig->useAppSrcForProgressivePlayback = true;
			logprintf("appSrcForProgressivePlayback:%s\n", gpGlobalConfig->useAppSrcForProgressivePlayback ? "on" : "off");
		}
		else if (ReadConfigNumericHelper(cfg, "abr-cache-outlier=", gpGlobalConfig->abrOutlierDiffBytes) == 1)
		{
			VALIDATE_INT("abr-cache-outlier", gpGlobalConfig->abrOutlierDiffBytes, DEFAULT_ABR_OUTLIER)
			logprintf("aamp abr outlier in bytes: %ld", gpGlobalConfig->abrOutlierDiffBytes);
		}
		else if (ReadConfigNumericHelper(cfg, "abr-skip-duration=", gpGlobalConfig->abrSkipDuration) == 1)
		{
			VALIDATE_INT("abr-skip-duration",gpGlobalConfig->abrSkipDuration, DEFAULT_ABR_SKIP_DURATION)
			logprintf("aamp abr skip duration: %d", gpGlobalConfig->abrSkipDuration);
		}
		else if (ReadConfigNumericHelper(cfg, "abr-nw-consistency=", gpGlobalConfig->abrNwConsistency) == 1)
		{
			VALIDATE_INT("abr-nw-consistency", gpGlobalConfig->abrNwConsistency, DEFAULT_ABR_NW_CONSISTENCY_CNT)
			logprintf("aamp abr NetworkConsistencyCnt: %d", gpGlobalConfig->abrNwConsistency);
		}
		else if (ReadConfigNumericHelper(cfg, "min-buffer-rampdown=", gpGlobalConfig->minABRBufferForRampDown) == 1)
		{
			VALIDATE_INT("min-buffer-rampdown", gpGlobalConfig->minABRBufferForRampDown, AAMP_LOW_BUFFER_BEFORE_RAMPDOWN)
			logprintf("aamp abr low buffer for rampdown: %d", gpGlobalConfig->minABRBufferForRampDown);
		}
		else if (ReadConfigNumericHelper(cfg, "max-buffer-rampup=", gpGlobalConfig->maxABRBufferForRampUp) == 1)
		{
			logprintf("aamp abr high buffer for rampup: %d", gpGlobalConfig->maxABRBufferForRampUp);
		}
		else if (ReadConfigNumericHelper(cfg, "flush=", gpGlobalConfig->gPreservePipeline) == 1)
		{
			logprintf("aamp flush=%d", gpGlobalConfig->gPreservePipeline);
		}
		else if (ReadConfigNumericHelper(cfg, "demux-hls-audio-track=", gpGlobalConfig->gAampDemuxHLSAudioTsTrack) == 1)
		{ // default 1, set to 0 for hw demux audio ts track
			logprintf("demux-hls-audio-track=%d", gpGlobalConfig->gAampDemuxHLSAudioTsTrack);
		}
		else if (ReadConfigNumericHelper(cfg, "demux-hls-video-track=", gpGlobalConfig->gAampDemuxHLSVideoTsTrack) == 1)
		{ // default 1, set to 0 for hw demux video ts track
			logprintf("demux-hls-video-track=%d", gpGlobalConfig->gAampDemuxHLSVideoTsTrack);
		}
		else if (ReadConfigNumericHelper(cfg, "demux-hls-video-track-tm=", gpGlobalConfig->demuxHLSVideoTsTrackTM) == 1)
		{ // default 0, set to 1 to demux video ts track during trickmodes
			logprintf("demux-hls-video-track-tm=%d", gpGlobalConfig->demuxHLSVideoTsTrackTM);
		}
		else if (ReadConfigNumericHelper(cfg, "demuxed-audio-before-video=", gpGlobalConfig->demuxedAudioBeforeVideo) == 1)
		{ // default 0, set to 1 to send audio es before video in case of s/w demux.
			logprintf("demuxed-audio-before-video=%d", gpGlobalConfig->demuxedAudioBeforeVideo);
		}
		else if (ReadConfigNumericHelper(cfg, "throttle=", gpGlobalConfig->gThrottle) == 1)
		{ // default is true; used with restamping?
			logprintf("aamp throttle=%d", gpGlobalConfig->gThrottle);
		}
		else if (ReadConfigNumericHelper(cfg, "min-init-cache=", gpGlobalConfig->minInitialCacheSeconds) == 1)
		{ // override for initial cache
			VALIDATE_INT("min-init-cache", gpGlobalConfig->minInitialCacheSeconds, DEFAULT_MINIMUM_INIT_CACHE_SECONDS)
			logprintf("min-init-cache=%d", gpGlobalConfig->minInitialCacheSeconds);
		}
		else if (ReadConfigNumericHelper(cfg, "buffer-health-monitor-delay=", gpGlobalConfig->bufferHealthMonitorDelay) == 1)
		{ // override for buffer health monitor delay after tune/ seek
			VALIDATE_INT("buffer-health-monitor-delay", gpGlobalConfig->bufferHealthMonitorDelay, DEFAULT_BUFFER_HEALTH_MONITOR_DELAY)
			logprintf("buffer-health-monitor-delay=%d", gpGlobalConfig->bufferHealthMonitorDelay);
		}
		else if (ReadConfigNumericHelper(cfg, "buffer-health-monitor-interval=", gpGlobalConfig->bufferHealthMonitorInterval) == 1)
		{ // override for buffer health monitor interval
			VALIDATE_INT("buffer-health-monitor-interval", gpGlobalConfig->bufferHealthMonitorInterval, DEFAULT_BUFFER_HEALTH_MONITOR_INTERVAL)
			logprintf("buffer-health-monitor-interval=%d", gpGlobalConfig->bufferHealthMonitorInterval);
		}
		else if (ReadConfigNumericHelper(cfg, "preferred-drm=", value) == 1)
		{ // override for preferred drm value
			if(value <= eDRM_NONE || value > eDRM_PlayReady)
			{
				logprintf("preferred-drm=%d is unsupported", value);
			}
			else
			{
				gpGlobalConfig->isUsingLocalConfigForPreferredDRM = true;
				gpGlobalConfig->preferredDrm = (DRMSystems) value;
			}
			logprintf("preferred-drm=%s", GetDrmSystemName(gpGlobalConfig->preferredDrm));
		}
		else if (ReadConfigNumericHelper(cfg, "playready-output-protection=", value) == 1)
		{
			gpGlobalConfig->enablePROutputProtection = (value != 0);
			logprintf("playready-output-protection is %s", (value ? "on" : "off"));
		}
		else if (ReadConfigNumericHelper(cfg, "live-tune-event=", value) == 1)
                { // default is 0; set 1 for sending tuned for live
                        logprintf("live-tune-event = %d", value);
                        if (value >= 0 && value < eTUNED_EVENT_MAX)
                        {
                                gpGlobalConfig->tunedEventConfigLive = (TunedEventConfig)(value);
                        }
                }
                else if (ReadConfigNumericHelper(cfg, "vod-tune-event=", value) == 1)
                { // default is 0; set 1 for sending tuned event for vod
                        logprintf("vod-tune-event = %d", value);
                        if (value >= 0 && value < eTUNED_EVENT_MAX)
                        {
                                gpGlobalConfig->tunedEventConfigVOD = (TunedEventConfig)(value);
                        }
                }
		else if (ReadConfigNumericHelper(cfg, "playlists-parallel-fetch=", value) == 1)
		{
			gpGlobalConfig->playlistsParallelFetch = (TriState)(value != 0);
			logprintf("playlists-parallel-fetch=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "useDashParallelFragDownload=", value) == 1)
		{
			gpGlobalConfig->dashParallelFragDownload = (TriState)(value != 0);
			logprintf("useDashParallelFragDownload=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "parallelPlaylistRefresh=", value) == 1)
		{
			gpGlobalConfig->parallelPlaylistRefresh  = (TriState)(value != 0);
			logprintf("parallelPlaylistRefresh =%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "bulk-timedmeta-report=", value) == 1)
		{
			gpGlobalConfig->enableBulkTimedMetaReport = (TriState)(value != 0);
			logprintf("bulk-timedmeta-report=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "useRetuneForUnpairedDiscontinuity=", value) == 1)
		{
			gpGlobalConfig->useRetuneForUnpairedDiscontinuity = (TriState)(value != 0);
			logprintf("useRetuneForUnpairedDiscontinuity=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "useRetuneForGstInternalError=", value) == 1)
		{
			gpGlobalConfig->useRetuneForGSTInternalError = (TriState)(value != 0);
			logprintf("useRetuneForGstInternalError=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "async-tune=", value) == 1)
		{
			gpGlobalConfig->mAsyncTuneConfig = (TriState)(value != 0);
			logprintf("async-tune=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "useWesterosSink=", value) == 1)
		{
			gpGlobalConfig->mWesterosSinkConfig = (TriState)(value != 0);
			logprintf("useWesterosSink=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "setLicenseCaching=", value) == 1)
		{
			gpGlobalConfig->licenseCaching = (TriState)(value != 0);
			logprintf("setLicenseCaching=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "propagateUriParameters=", value) == 1)
                {
			gpGlobalConfig->mPropagateUriParameters = (TriState)(value);
			logprintf("PropagateUriParameters = %d", (TriState) gpGlobalConfig->mPropagateUriParameters);
		}
		else if (ReadConfigNumericHelper(cfg, "pre-fetch-iframe-playlist=", value) == 1)
		{
			gpGlobalConfig->prefetchIframePlaylist = (value != 0);
			logprintf("pre-fetch-iframe-playlist=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "hls-av-sync-use-start-time=", value) == 1)
		{
			gpGlobalConfig->hlsAVTrackSyncUsingStartTime = (value != 0);
			logprintf("hls-av-sync-use-start-time=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "mpd-discontinuity-handling=", value) == 1)
		{
			gpGlobalConfig->mpdDiscontinuityHandling = (value != 0);
			logprintf("mpd-discontinuity-handling=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "mpd-discontinuity-handling-cdvr=", value) == 1)
		{
			gpGlobalConfig->mpdDiscontinuityHandlingCdvr = (value != 0);
			logprintf("mpd-discontinuity-handling-cdvr=%d", value);
		}
		else if(ReadConfigStringHelper(cfg, "ck-license-server-url=", (const char**)&gpGlobalConfig->ckLicenseServerURL))
		{
			logprintf("Clear Key license-server-url=%s", gpGlobalConfig->ckLicenseServerURL);
		}
		else if(ReadConfigStringHelper(cfg, "license-server-url=", (const char**)&gpGlobalConfig->licenseServerURL))
		{
			gpGlobalConfig->licenseServerLocalOverride = true;
			logprintf("license-server-url=%s", gpGlobalConfig->licenseServerURL);
		}
		else if(ReadConfigNumericHelper(cfg, "vod-trickplay-fps=", gpGlobalConfig->vodTrickplayFPS) == 1)
		{
			VALIDATE_INT("vod-trickplay-fps", gpGlobalConfig->vodTrickplayFPS, TRICKPLAY_NETWORK_PLAYBACK_FPS)
			if(gpGlobalConfig->vodTrickplayFPS != TRICKPLAY_NETWORK_PLAYBACK_FPS)
				gpGlobalConfig->vodTrickplayFPSLocalOverride = true;
			logprintf("vod-trickplay-fps=%d", gpGlobalConfig->vodTrickplayFPS);
		}
		else if(ReadConfigNumericHelper(cfg, "linear-trickplay-fps=", gpGlobalConfig->linearTrickplayFPS) == 1)
		{
			VALIDATE_INT("linear-trickplay-fps", gpGlobalConfig->linearTrickplayFPS, TRICKPLAY_TSB_PLAYBACK_FPS)
			if (gpGlobalConfig->linearTrickplayFPS != TRICKPLAY_TSB_PLAYBACK_FPS)
				gpGlobalConfig->linearTrickplayFPSLocalOverride = true;
			logprintf("linear-trickplay-fps=%d", gpGlobalConfig->linearTrickplayFPS);
		}
		else if (ReadConfigNumericHelper(cfg, "report-progress-interval=", gpGlobalConfig->reportProgressInterval) == 1)
		{
			// Progress report input in milliSec 
			VALIDATE_INT("report-progress-interval", gpGlobalConfig->reportProgressInterval, DEFAULT_REPORT_PROGRESS_INTERVAL)
			logprintf("report-progress-interval=%d", gpGlobalConfig->reportProgressInterval);
		}
		else if (ReadConfigStringHelper(cfg, "http-proxy=", (const char**)&gpGlobalConfig->httpProxy))
		{
			logprintf("http-proxy=%s", gpGlobalConfig->httpProxy);
		}
		else if (cfg.compare("force-http") == 0)
		{
			gpGlobalConfig->bForceHttp = !gpGlobalConfig->bForceHttp;
			logprintf("force-http: %s", gpGlobalConfig->bForceHttp ? "on" : "off");
		}
		else if (ReadConfigNumericHelper(cfg, "internal-retune=", value) == 1)
		{
			gpGlobalConfig->internalReTune = (value != 0);
			logprintf("internal-retune=%d", (int)value);
		}
		else if (ReadConfigNumericHelper(cfg, "gst-buffering-before-play=", value) == 1)
		{
			gpGlobalConfig->gstreamerBufferingBeforePlay = (value != 0);
			logprintf("gst-buffering-before-play=%d", (int)gpGlobalConfig->gstreamerBufferingBeforePlay);
		}
		else if (ReadConfigNumericHelper(cfg, "re-tune-on-buffering-timeout=", value) == 1)
		{
			gpGlobalConfig->reTuneOnBufferingTimeout = (value != 0);
			logprintf("re-tune-on-buffering-timeout=%d", (int)gpGlobalConfig->reTuneOnBufferingTimeout);
		}
		else if (ReadConfigNumericHelper(cfg, "iframe-default-bitrate=", gpGlobalConfig->iframeBitrate) == 1)
		{
			VALIDATE_LONG("iframe-default-bitrate",gpGlobalConfig->iframeBitrate, 0)
			logprintf("aamp iframe-default-bitrate: %ld", gpGlobalConfig->iframeBitrate);
		}
		else if (ReadConfigNumericHelper(cfg, "iframe-default-bitrate-4k=", gpGlobalConfig->iframeBitrate4K) == 1)
		{
			VALIDATE_LONG("iframe-default-bitrate-4k",gpGlobalConfig->iframeBitrate4K, 0)
			logprintf("aamp iframe-default-bitrate-4k: %ld", gpGlobalConfig->iframeBitrate4K);
		}
		else if (cfg.compare("aamp-audio-only-playback") == 0)
		{
			gpGlobalConfig->bAudioOnlyPlayback = true;
			logprintf("aamp-audio-only-playback is %s", gpGlobalConfig->bAudioOnlyPlayback ? "enabled" : "disabled");
		}
		else if (ReadConfigNumericHelper(cfg, "license-retry-wait-time=", gpGlobalConfig->licenseRetryWaitTime) == 1)
		{
			logprintf("license-retry-wait-time: %d", gpGlobalConfig->licenseRetryWaitTime);
		}
		else if (ReadConfigNumericHelper(cfg, "fragment-cache-length=", gpGlobalConfig->maxCachedFragmentsPerTrack) == 1)
		{
			VALIDATE_INT("fragment-cache-length", gpGlobalConfig->maxCachedFragmentsPerTrack, DEFAULT_CACHED_FRAGMENTS_PER_TRACK)
			logprintf("aamp fragment cache length: %d", gpGlobalConfig->maxCachedFragmentsPerTrack);
		}
		else if (ReadConfigNumericHelper(cfg, "pts-error-threshold=", gpGlobalConfig->ptsErrorThreshold) == 1)
		{
			VALIDATE_INT("pts-error-threshold", gpGlobalConfig->ptsErrorThreshold, MAX_PTS_ERRORS_THRESHOLD)
			logprintf("aamp pts-error-threshold: %d", gpGlobalConfig->ptsErrorThreshold);
		}
		else if (ReadConfigNumericHelper(cfg, "enable_setvideorectangle=", value) == 1)
		{
			gpGlobalConfig->mEnableRectPropertyCfg = (TriState)(value != 0);
			logprintf("AAMP Rectangle property for sink element: %d\n", value);
		}
		else if(ReadConfigNumericHelper(cfg, "max-playlist-cache=", gpGlobalConfig->gMaxPlaylistCacheSize) == 1)
		{
			// Read value in KB , convert it to bytes
			gpGlobalConfig->gMaxPlaylistCacheSize = gpGlobalConfig->gMaxPlaylistCacheSize * 1024;
			VALIDATE_INT("max-playlist-cache", gpGlobalConfig->gMaxPlaylistCacheSize, MAX_PLAYLIST_CACHE_SIZE);
			logprintf("aamp max-playlist-cache: %ld", gpGlobalConfig->gMaxPlaylistCacheSize);
		}
		else if(sscanf(cfg.c_str(), "dash-max-drm-sessions=%d", &gpGlobalConfig->dash_MaxDRMSessions) == 1)
		{
			// Read value in KB , convert it to bytes
			if(gpGlobalConfig->dash_MaxDRMSessions < MIN_DASH_DRM_SESSIONS || gpGlobalConfig->dash_MaxDRMSessions > MAX_DASH_DRM_SESSIONS)
			{
				logprintf("Out of range value for dash-max-drm-sessions, setting to %d;Expected Range (%d - %d)",
						MIN_DASH_DRM_SESSIONS, MIN_DASH_DRM_SESSIONS, MAX_DASH_DRM_SESSIONS);
				gpGlobalConfig->dash_MaxDRMSessions = MIN_DASH_DRM_SESSIONS;
			}
			logprintf("aamp dash-max-drm-sessions: %d", gpGlobalConfig->dash_MaxDRMSessions);
		}
		else if (ReadConfigStringHelper(cfg, "user-agent=", (const char**)&tmpValue))
		{
			if(tmpValue)
			{
				if(strlen(tmpValue) < AAMP_USER_AGENT_MAX_CONFIG_LEN)
				{
					logprintf("user-agent=%s", tmpValue);
					gpGlobalConfig->setBaseUserAgentString(tmpValue);
				}
				else
				{
					logprintf("user-agent len is more than %d , Hence Ignoring ", AAMP_USER_AGENT_MAX_CONFIG_LEN);
				}
 
				free(tmpValue);
				tmpValue = NULL;
			}
		}
		else if (ReadConfigNumericHelper(cfg, "wait-time-before-retry-http-5xx-ms=", gpGlobalConfig->waitTimeBeforeRetryHttp5xxMS) == 1)
		{
			VALIDATE_INT("wait-time-before-retry-http-5xx-ms", gpGlobalConfig->waitTimeBeforeRetryHttp5xxMS, DEFAULT_WAIT_TIME_BEFORE_RETRY_HTTP_5XX_MS);
			logprintf("aamp wait-time-before-retry-http-5xx-ms: %d", gpGlobalConfig->waitTimeBeforeRetryHttp5xxMS);
		}
		else if (ReadConfigNumericHelper(cfg, "preplaybuffercount=", gpGlobalConfig->preplaybuffercount) == 1)
		{
			VALIDATE_INT("preplaybuffercount", gpGlobalConfig->preplaybuffercount, 10);
			logprintf("preplaybuffercount : %d ",gpGlobalConfig->preplaybuffercount);
		}
		else if (ReadConfigNumericHelper(cfg, "sslverifypeer=", value) == 1)
		{
			gpGlobalConfig->sslVerifyPeer = (TriState)(value == 1);
			logprintf("ssl verify peer is %s", gpGlobalConfig->sslVerifyPeer? "disabled" : "enabled");
		}
		else if (ReadConfigNumericHelper(cfg, "curl-stall-timeout=", gpGlobalConfig->curlStallTimeout) == 1)
		{
			//Not calling VALIDATE_LONG since zero is supported
			logprintf("aamp curl-stall-timeout: %ld", gpGlobalConfig->curlStallTimeout);
		}
		else if (ReadConfigNumericHelper(cfg, "curl-download-start-timeout=", gpGlobalConfig->curlDownloadStartTimeout) == 1)
		{
			//Not calling VALIDATE_LONG since zero is supported
			logprintf("aamp curl-download-start-timeout: %ld", gpGlobalConfig->curlDownloadStartTimeout);
		}
		else if (ReadConfigNumericHelper(cfg, "discontinuity-timeout=", gpGlobalConfig->discontinuityTimeout) == 1)
		{
			//Not calling VALIDATE_LONG since zero is supported
			logprintf("aamp discontinuity-timeout: %ld", gpGlobalConfig->discontinuityTimeout);
		}
		else if (ReadConfigNumericHelper(cfg, "client-dai=", value) == 1)
		{
			gpGlobalConfig->enableClientDai = (value == 1);
			logprintf("Client side DAI: %s", gpGlobalConfig->enableClientDai ? "ON" : "OFF");
		}
		else if (ReadConfigNumericHelper(cfg, "ad-from-cdn-only=", value) == 1)
		{
			gpGlobalConfig->playAdFromCDN = (value == 1);
			logprintf("Ad playback from CDN only: %s", gpGlobalConfig->playAdFromCDN ? "ON" : "OFF");
		}
		else if (ReadConfigNumericHelper(cfg, "aamp-abr-threshold-size=", gpGlobalConfig->aampAbrThresholdSize) == 1)
                {
                        VALIDATE_INT("aamp-abr-threshold-size", gpGlobalConfig->aampAbrThresholdSize, DEFAULT_AAMP_ABR_THRESHOLD_SIZE);
                        logprintf("aamp aamp-abr-threshold-size: %d\n", gpGlobalConfig->aampAbrThresholdSize);
                }

		else if(ReadConfigStringHelper(cfg, "subtitle-language=", (const char**)&tmpValue))
		{
			if(tmpValue)
			{
				if(strlen(tmpValue) < MAX_LANGUAGE_TAG_LENGTH)
				{
					logprintf("subtitle-language=%s", tmpValue);
					gpGlobalConfig->mSubtitleLanguage = std::string(tmpValue);
				}
				else
				{
					logprintf("subtitle-language len is more than %d , Hence Ignoring ", MAX_LANGUAGE_TAG_LENGTH);
				}
				free(tmpValue);
				tmpValue = NULL;
			}
		}
		else if (ReadConfigNumericHelper(cfg, "reportbufferevent=", value) == 1)
		{
			gpGlobalConfig->reportBufferEvent = (value != 0);
			logprintf("reportbufferevent=%d", (int)gpGlobalConfig->reportBufferEvent);
		}
		else if (ReadConfigNumericHelper(cfg, "enable-tune-profiling=", value) == 1)
		{
			gpGlobalConfig->enableMicroEvents = (value!=0);
			logprintf( "enable-tune-profiling=%d", value );
		}
		else if (ReadConfigNumericHelper(cfg, "gst-position-query-enable=", value) == 1)
		{
			gpGlobalConfig->bPositionQueryEnabled = (value == 1);
			logprintf("Position query based progress events: %s", gpGlobalConfig->bPositionQueryEnabled ? "ON" : "OFF");
		}
		else if (ReadConfigNumericHelper(cfg, "use-matching-baseurl=", value) == 1)
		{
			gpGlobalConfig->useMatchingBaseUrl = (TriState) (value == 1);
			logprintf("use-matching-baseurl: %d", gpGlobalConfig->useMatchingBaseUrl);
		}
		else if (ReadConfigNumericHelper(cfg, "remove_Persistent=", gpGlobalConfig->aampRemovePersistent) == 1)
		{
			logprintf("remove_Persistent=%d", gpGlobalConfig->aampRemovePersistent);
		}
		else if (ReadConfigNumericHelper(cfg, "avgbwforABR=", value) == 1)
		{
			gpGlobalConfig->mUseAverageBWForABR= (TriState)(value != 0);
			logprintf("avgbwforABR=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "preCachePlaylistTime=", value) == 1)
		{	
			// time window in Minutes			
			gpGlobalConfig->mPreCacheTimeWindow= value;
			logprintf("preCachePlaylistTime=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "fragmentRetryLimit=", value) == 1)
		{
			gpGlobalConfig->rampdownLimit = value;
			logprintf("fragmentRetryLimit=%d", value);
		}
		else if (ReadConfigNumericHelper(cfg, "segmentInjectFailThreshold=", gpGlobalConfig->segInjectFailCount) == 1)
		{
			VALIDATE_INT("segmentInjectFailThreshold", gpGlobalConfig->segInjectFailCount, MAX_SEG_INJECT_FAIL_COUNT);
			logprintf("segmentInjectFailThreshold=%d", gpGlobalConfig->segInjectFailCount);
		}
		else if (ReadConfigNumericHelper(cfg, "drmDecryptFailThreshold=", gpGlobalConfig->drmDecryptFailCount) ==1)
		{
			VALIDATE_INT("drmDecryptFailThreshold", gpGlobalConfig->drmDecryptFailCount, MAX_SEG_DRM_DECRYPT_FAIL_COUNT);
			logprintf("drmDecryptFailThreshold=%d", gpGlobalConfig->drmDecryptFailCount);
		}
		else if (ReadConfigNumericHelper(cfg, "minBitrate=", gpGlobalConfig->minBitrate) ==1)
		{
			VALIDATE_LONG("minBitrate", gpGlobalConfig->minBitrate, 0);
			logprintf("minBitrate=%d", gpGlobalConfig->minBitrate);
		}
		else if (ReadConfigNumericHelper(cfg, "maxBitrate=", gpGlobalConfig->maxBitrate) ==1)
		{
			VALIDATE_LONG("maxBitrate", gpGlobalConfig->maxBitrate, LONG_MAX);
			logprintf("maxBitrate=%d", gpGlobalConfig->maxBitrate);
		}
		else if (ReadConfigNumericHelper(cfg, "initFragmentRetryCount=", gpGlobalConfig->initFragmentRetryCount) == 1)
		{
			logprintf("initFragmentRetryCount=%d", gpGlobalConfig->initFragmentRetryCount);
		}
		else if (ReadConfigNumericHelper(cfg, "enable-native-cc=", value) == 1)
		{
			gpGlobalConfig->nativeCCRendering = (value == 1);
			logprintf("Native CC rendering support: %s", gpGlobalConfig->nativeCCRendering ? "ON" : "OFF");
		}
		else if (cfg.compare("disableSubtec") == 0)
		{
			gpGlobalConfig->bEnableSubtec = false;
			logprintf("Subtec subtitles disabled");
		}
		else if (cfg.compare("webVttNative") == 0)
		{
			gpGlobalConfig->bWebVttNative = true;
			logprintf("Native WebVTT processing enabled");
		}
		else if (ReadConfigNumericHelper(cfg, "preferred-cea-708=", value) == 1)
		{
			gpGlobalConfig->preferredCEA708 = (value == 1) ? eTrueState : eFalseState;
			logprintf("CEA 708 is preferred format: %s", (gpGlobalConfig->preferredCEA708 == eTrueState) ? "TRUE" : "FALSE");
		}
		else if(ReadConfigNumericHelper(cfg, "initRampdownLimit=", value))
		{
			if (value >= 0){
				gpGlobalConfig->mInitRampdownLimit = value;
				logprintf("initRampdownLimit=%d", (int)gpGlobalConfig->mInitRampdownLimit);
			}
		}
		else if(ReadConfigNumericHelper(cfg, "maxTimeoutForSourceSetup=", gpGlobalConfig->mTimeoutForSourceSetup) == 1)
		{
			logprintf("Timeout for source setup = %ld", gpGlobalConfig->mTimeoutForSourceSetup);
		}
		else if(ReadConfigNumericHelper(cfg, "enableSeekableRange=", value))
		{
			gpGlobalConfig->mEnableSeekableRange = (TriState) (value == 1);
			logprintf("Seekable range reporting: %d", gpGlobalConfig->mEnableSeekableRange);
		}
		else if (cfg.at(0) == '*')
		{
			std::size_t pos = cfg.find_first_of(' ');
			if (pos != std::string::npos)
			{
				//Populate channel map from aamp.cfg
				// new wildcard matching for overrides - allows *HBO to remap any url including "HBO"
				logprintf("aamp override:\n%s\n", cfg.c_str());
				ChannelInfo channelInfo;
				std::stringstream iss(cfg.substr(1));
				std::string token;
				while (getline(iss, token, ' '))
				{
					if (token.compare(0,4,"http") == 0)
						channelInfo.uri = token;
					else
						channelInfo.name = token;
				}
				mChannelOverrideMap.push_back(channelInfo);
			}
		}
		else if (ReadConfigNumericHelper(cfg, "disableWifiCurlHeader=", value) == 1)
                {
                        gpGlobalConfig->wifiCurlHeaderEnabled = (value!=1);
                        logprintf("%s Wifi curl custom header",gpGlobalConfig->wifiCurlHeaderEnabled?"Enabled":"Disabled");
                }
		else if (ReadConfigNumericHelper(cfg, "disableMidFragmentSeek=", value) == 1)
		{
			gpGlobalConfig->midFragmentSeekEnabled = (value!=1);
			logprintf("%s Mid-Fragment Seek",gpGlobalConfig->midFragmentSeekEnabled?"Enabled":"Disabled");
		}
		else if (ReadConfigNumericHelper(cfg, "persistBitRateOverSeek=", value))
		{
			gpGlobalConfig->mPersistBitRateOverSeek = (TriState) (value == 1);
			logprintf("Persist ABR Profile over seek: %d", gpGlobalConfig->mPersistBitRateOverSeek);
		}
		else
		{
			std::size_t pos = cfg.find_first_of('=');
			std::string key = cfg.substr(0, pos);
			std::string value = cfg.substr(pos+1, cfg.size());
			gpGlobalConfig->unknownValues.insert(std::make_pair(key, value));
			logprintf("Added unknown key %s with value %s", key.c_str(), value.c_str());
		}
	}
}
// End of helper functions for loading configuration

// Curl callback functions

/**
 * @brief write callback to be used by CURL
 * @param ptr pointer to buffer containing the data
 * @param size size of the buffer
 * @param nmemb number of bytes
 * @param userdata CurlCallbackContext pointer
 * @retval size consumed or 0 if interrupted
 */
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t ret = 0;
	CurlCallbackContext *context = (CurlCallbackContext *)userdata;
	pthread_mutex_lock(&context->aamp->mLock);
	if (context->aamp->mDownloadsEnabled)
	{
		size_t numBytesForBlock = size*nmemb;
		aamp_AppendBytes(context->buffer, ptr, numBytesForBlock);
		ret = numBytesForBlock;
	}
	else
	{
		logprintf("write_callback - interrupted");
	}
	pthread_mutex_unlock(&context->aamp->mLock);
	return ret;
}

/**
 * @brief function to print header response during download failure and latency.
 * @param type current media type
 */
static void print_headerResponse(std::vector<std::string> &allResponseHeadersForErrorLogging, MediaType fileType)
{
	if (gpGlobalConfig->logging.curlHeader && (eMEDIATYPE_VIDEO == fileType || eMEDIATYPE_PLAYLIST_VIDEO == fileType))
	{
		int size = allResponseHeadersForErrorLogging.size();
		logprintf("################ Start :: Print Header response ################");
		for (int i=0; i < size; i++)
		{
			logprintf("* %s", allResponseHeadersForErrorLogging.at(i).c_str());
		}
		logprintf("################ End :: Print Header response ################");
	}

	allResponseHeadersForErrorLogging.clear();
}

/**
 * @brief callback invoked on http header by curl
 * @param ptr pointer to buffer containing the data
 * @param size size of the buffer
 * @param nmemb number of bytes
 * @param user_data  CurlCallbackContext pointer
 * @retval
 */
static size_t header_callback(const char *ptr, size_t size, size_t nmemb, void *user_data)
{
	CurlCallbackContext *context = static_cast<CurlCallbackContext *>(user_data);
	httpRespHeaderData *httpHeader = context->responseHeaderData;
	size_t len = nmemb * size;
	size_t startPos = 0;
	size_t endPos = len-2; // strip CRLF

	bool isBitrateHeader = false;
	bool isFogRecordingIdHeader = false;

	if( len<2 || ptr[endPos] != '\r' || ptr[endPos+1] != '\n' )
	{ // only proceed if this is a CRLF terminated curl header, as expected
		return len;
	}

	if (gpGlobalConfig->logging.curlHeader && ptr[0] &&
			(eMEDIATYPE_VIDEO == context->fileType || eMEDIATYPE_PLAYLIST_VIDEO == context->fileType))
	{
		std::string temp = std::string(ptr,endPos);
		context->allResponseHeadersForErrorLogging.push_back(temp);
	}

	// As per Hypertext Transfer Protocol ==> Field names are case-insensitive
	// HTTP/1.1 4.2 Message Headers : Each header field consists of a name followed by a colon (":") and the field value. Field names are case-insensitive
	if (STARTS_WITH_IGNORE_CASE(ptr, FOG_REASON_STRING))
	{
		httpHeader->type = eHTTPHEADERTYPE_FOG_REASON;
		startPos = STRLEN_LITERAL(FOG_REASON_STRING);
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, CURLHEADER_X_REASON))
	{
		httpHeader->type = eHTTPHEADERTYPE_XREASON;
		startPos = STRLEN_LITERAL(CURLHEADER_X_REASON);
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, FOG_ERROR_STRING))
	{
		httpHeader->type = eHTTPHEADERTYPE_FOG_ERROR;
		startPos = STRLEN_LITERAL(FOG_ERROR_STRING);
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, BITRATE_HEADER_STRING))
	{
		startPos = STRLEN_LITERAL(BITRATE_HEADER_STRING);
		isBitrateHeader = true;
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, SET_COOKIE_HEADER_STRING))
	{
		httpHeader->type = eHTTPHEADERTYPE_COOKIE;
		startPos = STRLEN_LITERAL(SET_COOKIE_HEADER_STRING);
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, LOCATION_HEADER_STRING))
	{
		httpHeader->type = eHTTPHEADERTYPE_EFF_LOCATION;
		startPos = STRLEN_LITERAL(LOCATION_HEADER_STRING);
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, FOG_RECORDING_ID_STRING))
	{
		startPos = STRLEN_LITERAL(FOG_RECORDING_ID_STRING);
		isFogRecordingIdHeader = true;
	}
	else if (STARTS_WITH_IGNORE_CASE(ptr, CONTENT_ENCODING_STRING ))
	{
		// Enabled IsEncoded as Content-Encoding header is present
		// The Content-Encoding entity header incidcates media is compressed
		context->downloadIsEncoded = true;
	}
	else if (0 == context->buffer->avail)
	{
		if (STARTS_WITH_IGNORE_CASE(ptr, CONTENTLENGTH_STRING))
		{
			int contentLengthStartPosition = STRLEN_LITERAL(CONTENTLENGTH_STRING);
			const char * contentLengthStr = ptr + contentLengthStartPosition;
			int contentLength = atoi(contentLengthStr);

			/*contentLength can be zero for redirects*/
			if (contentLength > 0)
			{
				/*Add 2 additional characters to take care of extra characters inserted by aamp_AppendNulTerminator*/
				aamp_Malloc(context->buffer, contentLength + 2);
			}
		}
	}
	
	if(startPos > 0)
	{
		while( endPos>startPos && ptr[endPos-1] == ' ' )
		{ // strip trailing whitespace
			endPos--;
		}
		while( startPos < endPos && ptr[startPos] == ' ')
		{ // strip leading whitespace
			startPos++;
		}

		if(isBitrateHeader)
		{
			const char * strBitrate = ptr + startPos;
			context->bitrate = atol(strBitrate);
			traceprintf("Parsed HTTP %s: %ld\n", isBitrateHeader? "Bitrate": "False", context->bitrate);
		}
		else if(isFogRecordingIdHeader)
		{
			context->aamp->mTsbRecordingId = string( ptr + startPos, endPos - startPos );
			AAMPLOG_TRACE("Parsed Fog-Id : %s", context->aamp->mTsbRecordingId.c_str());
		}
		else
		{
			httpHeader->data = string( ptr + startPos, endPos - startPos );
			if(httpHeader->type != eHTTPHEADERTYPE_EFF_LOCATION && httpHeader->type != eHTTPHEADERTYPE_FOG_ERROR)
			{ //Append delimiter ";"
				httpHeader->data += ';';
			}
		}

		if(gpGlobalConfig->logging.trace)
		{
			traceprintf("Parsed HTTP %s header: %s", httpHeader->type==eHTTPHEADERTYPE_COOKIE? "Cookie": "X-Reason", httpHeader->data.c_str());
		}
	}
	return len;
}

/**
 * @brief
 * @param clientp app-specific as optionally set with CURLOPT_PROGRESSDATA
 * @param dltotal total bytes expected to download
 * @param dlnow downloaded bytes so far
 * @param ultotal total bytes expected to upload
 * @param ulnow uploaded bytes so far
 * @retval
 */
static int progress_callback(
	void *clientp, // app-specific as optionally set with CURLOPT_PROGRESSDATA
	double dltotal, // total bytes expected to download
	double dlnow, // downloaded bytes so far
	double ultotal, // total bytes expected to upload
	double ulnow // uploaded bytes so far
	)
{
	CurlProgressCbContext *context = (CurlProgressCbContext *)clientp;
	int rc = 0;
	context->aamp->SyncBegin();
	if (!context->aamp->mDownloadsEnabled)
	{
		rc = -1; // CURLE_ABORTED_BY_CALLBACK
	}
	context->aamp->SyncEnd();
	if( rc==0 )
	{ // only proceed if not an aborted download
		if (dlnow > 0 && context->stallTimeout > 0)
		{
			if (context->downloadSize == -1)
			{ // first byte(s) downloaded
				context->downloadSize = dlnow;
				context->downloadUpdatedTime = NOW_STEADY_TS_MS;
			}
			else
			{
				if (dlnow == context->downloadSize)
				{ // no change in downloaded bytes - check time since last update to infer stall
					double timeElapsedSinceLastUpdate = (NOW_STEADY_TS_MS - context->downloadUpdatedTime) / 1000.0; //in secs
					if (timeElapsedSinceLastUpdate >= context->stallTimeout)
					{ // no change for at least <stallTimeout> seconds - consider download stalled and abort
						logprintf("Abort download as mid-download stall detected for %.2f seconds, download size:%.2f bytes", timeElapsedSinceLastUpdate, dlnow);
						context->abortReason = eCURL_ABORT_REASON_STALL_TIMEDOUT;
						rc = -1;
					}
				}
				else
				{ // received additional bytes - update state to track new size/time
					context->downloadSize = dlnow;
					context->downloadUpdatedTime = NOW_STEADY_TS_MS;
				}
			}
		}
		else if (dlnow == 0 && context->startTimeout > 0)
		{ // check to handle scenario where <startTimeout> seconds delay occurs without any bytes having been downloaded (stall at start)
			double timeElapsedInSec = (double)(NOW_STEADY_TS_MS - context->downloadStartTime) / 1000; //in secs  //CID:85922 - UNINTENDED_INTEGER_DIVISION
			if (timeElapsedInSec >= context->startTimeout)
			{
				logprintf("Abort download as no data received for %.2f seconds", timeElapsedInSec);
				context->abortReason = eCURL_ABORT_REASON_START_TIMEDOUT;
				rc = -1;
			}
		}
	}
	return rc;
}

static int eas_curl_debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp)
{
	(void)handle;
	(void)userp;
	if(type == CURLINFO_TEXT || type == CURLINFO_HEADER_IN)
	{ 
		//limit log spam to only TEXT and HEADER_IN
		size_t len = size;
		while( len>0 && data[len-1]<' ' ) len--;
		std::string printable(data,len);
		switch (type)
		{
		case CURLINFO_TEXT:
			logprintf("curl: %s", printable.c_str() );
			break;
		case CURLINFO_HEADER_IN:
			logprintf("curl header: %s", printable.c_str() );
			break;
		default:
			break; //CID:94999 - Resolve deadcode
		}
	}
	return 0;
}

/**
 * @brief
 * @param curl ptr to CURL instance
 * @param ssl_ctx SSL context used by CURL
 * @param user_ptr data pointer set as param to CURLOPT_SSL_CTX_DATA
 * @retval CURLcode CURLE_OK if no errors, otherwise corresponding CURL code
 */
CURLcode ssl_callback(CURL *curl, void *ssl_ctx, void *user_ptr)
{
	PrivateInstanceAAMP *context = (PrivateInstanceAAMP *)user_ptr;
	CURLcode rc = CURLE_OK;
	pthread_mutex_lock(&context->mLock);
	if (!context->mDownloadsEnabled)
	{
		rc = CURLE_ABORTED_BY_CALLBACK ; // CURLE_ABORTED_BY_CALLBACK
	}
	pthread_mutex_unlock(&context->mLock);
	return rc;
}

/**
 * @brief
 * @param curl ptr to CURL instance
 * @param data curl data lock
 * @param acess curl access lock
 * @param user_ptr CurlCallbackContext pointer
 * @retval void
 */
static void curl_lock_callback(CURL *curl, curl_lock_data data, curl_lock_access access, void *user_ptr)
{
	(void)access; /* unused */
	(void)user_ptr; /* unused */
	(void)curl; /* unused */
	(void)data; /* unused */
	pthread_mutex_lock(&gCurlInitMutex);
}

/**
 * @brief
 * @param curl ptr to CURL instance
 * @param data curl data lock
 * @param acess curl access lock
 * @param user_ptr CurlCallbackContext pointer
 * @retval void
 */
static void curl_unlock_callback(CURL *curl, curl_lock_data data, curl_lock_access access, void *user_ptr)
{
	(void)access; /* unused */
	(void)user_ptr; /* unused */
	(void)curl; /* unused */
	(void)data; /* unused */
	pthread_mutex_unlock(&gCurlInitMutex);
}

// End of curl callback functions

/**
 * @brief PrivateInstanceAAMP Constructor
 */
PrivateInstanceAAMP::PrivateInstanceAAMP() : mAbrBitrateData(), mLock(), mMutexAttr(),
	mpStreamAbstractionAAMP(NULL), mInitSuccess(false), mVideoFormat(FORMAT_INVALID), mAudioFormat(FORMAT_INVALID), mDownloadsDisabled(),
	mDownloadsEnabled(true), mStreamSink(NULL), profiler(), licenceFromManifest(false), previousAudioType(eAUDIO_UNKNOWN),
	mbDownloadsBlocked(false), streamerIsActive(false), mTSBEnabled(false), mIscDVR(false), mLiveOffset(AAMP_LIVE_OFFSET), mNewLiveOffsetflag(false),
	fragmentCollectorThreadID(0), seek_pos_seconds(-1), rate(0), pipeline_paused(false), mMaxLanguageCount(0), zoom_mode(VIDEO_ZOOM_FULL),
	video_muted(false), subtitles_muted(true), audio_volume(100), subscribedTags(), timedMetadata(), IsTuneTypeNew(false), trickStartUTCMS(-1),mLogTimetoTopProfile(true),
	playStartUTCMS(0), durationSeconds(0.0), culledSeconds(0.0), maxRefreshPlaylistIntervalSecs(DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS/1000), initialTuneTimeMs(0),
	mEventListener(NULL), mReportProgressPosn(0.0), mReportProgressTime(0), discardEnteringLiveEvt(false),
	mIsRetuneInProgress(false), mCondDiscontinuity(), mDiscontinuityTuneOperationId(0), mIsVSS(false),
	m_fd(-1), mIsLive(false), mTuneCompleted(false), mFirstTune(true), mfirstTuneFmt(-1), mTuneAttempts(0), mPlayerLoadTime(0),
	mState(eSTATE_RELEASED), mMediaFormat(eMEDIAFORMAT_HLS), mPersistedProfileIndex(0), mAvailableBandwidth(0),
	mDiscontinuityTuneOperationInProgress(false), mContentType(ContentType_UNKNOWN), mTunedEventPending(false),
	mSeekOperationInProgress(false), mPendingAsyncEvents(), mCustomHeaders(),
	mManifestUrl(""), mTunedManifestUrl(""), mServiceZone(), mVssVirtualStreamId(), mFogErrorString(""),
	mCurrentLanguageIndex(0), noExplicitUserLanguageSelection(true), languageSetByUser(false), preferredLanguagesString(), preferredLanguagesList(),
#ifdef SESSION_STATS
	mVideoEnd(NULL),
#endif
	mTimeToTopProfile(0),mTimeAtTopProfile(0),mPlaybackDuration(0),mTraceUUID(),
	mIsFirstRequestToFOG(false), mABREnabled(false), mUserRequestedBandwidth(0), mNetworkProxy(NULL), mLicenseProxy(NULL),mTuneType(eTUNETYPE_NEW_NORMAL)
	,mCdaiObject(NULL), mAdEventsQ(),mAdEventQMtx(), mAdPrevProgressTime(0), mAdCurOffset(0), mAdDuration(0), mAdProgressId("")
	,mBufUnderFlowStatus(false), mVideoBasePTS(0)
	,mCustomLicenseHeaders(), mIsIframeTrackPresent(false), mManifestTimeoutMs(-1), mNetworkTimeoutMs(-1)
	,mBulkTimedMetadata(false), reportMetadata(), mbPlayEnabled(true), mPlayerPreBuffered(false), mPlayerId(PLAYERID_CNTR++),mAampCacheHandler(new AampCacheHandler())
	,mAsyncTuneEnabled(false),
#if defined(REALTEKCE) || defined(AMLOGIC)	// Temporary till westerossink disable is rollbacked
	mWesterosSinkEnabled(true)
#else
	mWesterosSinkEnabled(false)
#endif
	,mEnableRectPropertyEnabled(true), waitforplaystart(), mLicenseCaching(true)
	,mTuneEventConfigLive(eTUNED_EVENT_ON_GST_PLAYING), mTuneEventConfigVod(eTUNED_EVENT_ON_GST_PLAYING)
	,mUseAvgBandwidthForABR(false), mParallelFetchPlaylistRefresh(true), mParallelFetchPlaylist(false), mDashParallelFragDownload(true)
	,mCurlShared(NULL)
	,mRampDownLimit(-1), mMinBitrate(0), mMaxBitrate(LONG_MAX), mSegInjectFailCount(MAX_SEG_INJECT_FAIL_COUNT), mDrmDecryptFailCount(MAX_SEG_DRM_DECRYPT_FAIL_COUNT)
	,mPlaylistTimeoutMs(-1)
	,mMutexPlaystart()
	,mDisableEC3(false),mDisableATMOS(false),mForceEC3(false)
#ifdef AAMP_HLS_DRM
    , fragmentCdmEncrypted(false) ,drmParserMutex(), aesCtrAttrDataList()
	, drmSessionThreadStarted(false), createDRMSessionThreadID(0)
#endif
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
	, mDRMSessionManager(NULL)
#endif
	,  mPreCachePlaylistThreadId(0), mPreCachePlaylistThreadFlag(false) , mPreCacheDnldList()
	, mPreCacheDnldTimeWindow(0), mReportProgressInterval(DEFAULT_REPORT_PROGRESS_INTERVAL), mParallelPlaylistFetchLock(), mAppName()
	, mABRBufferCheckEnabled(true), mNewAdBreakerEnabled(false), mProgressReportFromProcessDiscontinuity(false), mUseRetuneForUnpairedDiscontinuity(true)
	, prevPositionMiliseconds(-1), mInitFragmentRetryCount(-1), mPlaylistFetchFailError(0L),mAudioDecoderStreamSync(true)
	, mCurrentDrm(), mDrmInitData(), mMinInitialCacheSeconds(DEFAULT_MINIMUM_INIT_CACHE_SECONDS), mUseRetuneForGSTInternalError(true)
	, mLicenseServerUrls(), mFragmentCachingRequired(false), mFragmentCachingLock()
	, mPauseOnFirstVideoFrameDisp(false)
	, mPreferredAudioTrack(), mPreferredTextTrack(), mFirstVideoFrameDisplayedEnabled(false)
	, mSessionToken(), mCacheMaxSize(0)
	, midFragmentSeekCache(false)
	, mEnableSeekableRange(false), mReportVideoPTS(false)
	, mPreviousAudioType (FORMAT_INVALID)
	, mTsbRecordingId()
	, mPersistBitRateOverSeek(false)
	, mProgramDateTime (0)
	, mthumbIndexValue(0)
	, mManifestRefreshCount (0)
{
	LazilyLoadConfigIfNeeded();
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
	mDRMSessionManager = new AampDRMSessionManager();
#endif
	pthread_cond_init(&mDownloadsDisabled, NULL);
	strcpy(language,"en");
	iso639map_NormalizeLanguageCode( language, GetLangCodePreference() );
    
	memset(mSubLanguage, '\0', MAX_LANGUAGE_TAG_LENGTH);
	if (!gpGlobalConfig->mSubtitleLanguage.empty())
	{
		strncpy(mSubLanguage, gpGlobalConfig->mSubtitleLanguage.c_str(), MAX_LANGUAGE_TAG_LENGTH - 1);
	}
	pthread_mutexattr_init(&mMutexAttr);
	pthread_mutexattr_settype(&mMutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&mLock, &mMutexAttr);
	pthread_mutex_init(&mParallelPlaylistFetchLock, &mMutexAttr);
	pthread_mutex_init(&mFragmentCachingLock, &mMutexAttr);
	mCurlShared = curl_share_init();
	curl_share_setopt(mCurlShared, CURLSHOPT_LOCKFUNC, curl_lock_callback);
	curl_share_setopt(mCurlShared, CURLSHOPT_UNLOCKFUNC, curl_unlock_callback);
	curl_share_setopt(mCurlShared, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

	for (int i = 0; i < eCURLINSTANCE_MAX; i++)
	{
		curl[i] = NULL;
		//cookieHeaders[i].clear();
		httpRespHeaders[i].type = eHTTPHEADERTYPE_UNKNOWN;
		httpRespHeaders[i].data.clear();
		curlDLTimeout[i] = 0;
	}
	for (int i = 0; i < AAMP_MAX_NUM_EVENTS; i++)
	{
		mEventListeners[i] = NULL;
	}

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		mbTrackDownloadsBlocked[i] = false;
		mTrackInjectionBlocked[i] = false;
		lastUnderFlowTimeMs[i] = 0;
		mProcessingDiscontinuity[i] = false;
		mIsDiscontinuityIgnored[i] = false;
	}

	pthread_mutex_lock(&gMutex);
	gActivePrivAAMP_t gAAMPInstance = { this, false, 0 };
	gActivePrivAAMPs.push_back(gAAMPInstance);
	pthread_mutex_unlock(&gMutex);
	mPendingAsyncEvents.clear();

	if (gpGlobalConfig->wifiCurlHeaderEnabled) {
		if (true == IsActiveStreamingInterfaceWifi()) {
			mCustomHeaders["Wifi:"] = std::vector<std::string> { "1" };
			activeInterfaceWifi = true;
		}
		else
		{
			mCustomHeaders["Wifi:"] = std::vector<std::string> { "0" };
			activeInterfaceWifi = false;
		}
	}
	// Add Connection: Keep-Alive custom header - DELIA-26832
	mCustomHeaders["Connection:"] = std::vector<std::string> { "Keep-Alive" };
	pthread_cond_init(&mCondDiscontinuity, NULL);
	pthread_cond_init(&waitforplaystart, NULL);
	pthread_mutex_init(&mMutexPlaystart, NULL);
	SetAsyncTuneConfig(false);
	ConfigureWithLocalOptions();
        preferredLanguagesList.push_back("en");
#ifdef AAMP_HLS_DRM
	memset(&aesCtrAttrDataList, 0, sizeof(aesCtrAttrDataList));
	pthread_mutex_init(&drmParserMutex, NULL);
#endif
}

/**
 * @brief PrivateInstanceAAMP Destructor
 */
PrivateInstanceAAMP::~PrivateInstanceAAMP()
{
	pthread_mutex_lock(&gMutex);
	for (std::list<gActivePrivAAMP_t>::iterator iter = gActivePrivAAMPs.begin(); iter != gActivePrivAAMPs.end(); iter++)
	{
		if (this == iter->pAAMP)
		{
			gActivePrivAAMPs.erase(iter);
			break;
		}
	}
	pthread_mutex_unlock(&gMutex);

	pthread_mutex_lock(&mLock);
	for (int i = 0; i < AAMP_MAX_NUM_EVENTS; i++)
	{
		while (mEventListeners[i] != NULL)
		{
			ListenerData* pListener = mEventListeners[i];
			mEventListeners[i] = pListener->pNext;
			delete pListener;
		}
	}

	if (mNetworkProxy)
	{
		free(mNetworkProxy);
	}

	if (mLicenseProxy)
	{
		free(mLicenseProxy);
	}

#ifdef SESSION_STATS
	if (mVideoEnd)
	{
		delete mVideoEnd;
          	mVideoEnd = NULL;
	}
#endif
	pthread_mutex_unlock(&mLock);

	pthread_cond_destroy(&mDownloadsDisabled);
	pthread_cond_destroy(&mCondDiscontinuity);
	pthread_cond_destroy(&waitforplaystart);
	pthread_mutex_destroy(&mMutexPlaystart);
	pthread_mutex_destroy(&mLock);
	pthread_mutex_destroy(&mParallelPlaylistFetchLock);
	pthread_mutex_destroy(&mFragmentCachingLock);
#ifdef AAMP_HLS_DRM
	aesCtrAttrDataList.clear();
	pthread_mutex_destroy(&drmParserMutex);
#endif
	if (mAampCacheHandler)
	{
		delete mAampCacheHandler;
		mAampCacheHandler = NULL;
	}
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
	if (mDRMSessionManager)
	{
		delete mDRMSessionManager;
		mAampCacheHandler = NULL;
	}
#endif
	if(mCurlShared)
	{
		curl_share_cleanup(mCurlShared);
		mCurlShared = NULL;
        }
#ifdef IARM_MGR
	IARM_Bus_RemoveEventHandler("NET_SRV_MGR", IARM_BUS_NETWORK_MANAGER_EVENT_INTERFACE_IPADDRESS, getActiveInterfaceEventHandler);
#endif //IARM_MGR
}

/**
 * @brief Lock aamp mutex
 */
void PrivateInstanceAAMP::SyncBegin(void)
{
	pthread_mutex_lock(&mLock);
}

/**
 * @brief Unlock aamp mutex
 */
void PrivateInstanceAAMP::SyncEnd(void)
{
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief Report progress event to listeners
 */
void PrivateInstanceAAMP::ReportProgress(void)
{
	if (mDownloadsEnabled)
	{
		ReportAdProgress(sync);

		double position = GetPositionMilliseconds();
		double duration = durationSeconds * 1000.0;
		float speed = pipeline_paused ? 0 : rate;
		double start = -1;
		double end = -1;
		long long videoPTS = -1;
		double bufferedDuration = 0.0;

		// If tsb is not available for linear send -1  for start and end
		// so that xre detect this as tsbless playabck
		// Override above logic if mEnableSeekableRange is set, used by third-party apps
		if (!mEnableSeekableRange && (mContentType == ContentType_LINEAR && !mTSBEnabled))
		{
			start = -1;
			end = -1;
		}
		else
		{
			start = culledSeconds*1000.0;
			end = start + duration;
			if (position > end)
			{ // clamp end
				//logprintf("aamp clamp end");
				position = end;
			}
			else if (position < start)
			{ // clamp start
				//logprintf("aamp clamp start");
				position = start;
			}
		}

		if(mReportVideoPTS)
		{
				/*For HLS, tsprocessor.cpp removes the base PTS value and sends to gstreamer.
				**In order to report PTS of video currently being played out, we add the base PTS
				**to video PTS received from gstreamer
				*/
				/*For DASH,mVideoBasePTS value will be zero */
				videoPTS = mStreamSink->GetVideoPTS() + mVideoBasePTS;
		}

		if (mpStreamAbstractionAAMP)
		{
			bufferedDuration = mpStreamAbstractionAAMP->GetBufferedVideoDurationSec() * 1000.0;
		}

		ProgressEventPtr evt = std::make_shared<ProgressEvent>(duration, position, start, end, speed, videoPTS, bufferedDuration);
        
		if (gpGlobalConfig->logging.progress)
		{
			static int tick;
			if ((tick++ % 4) == 0)
			{
				logprintf("aamp pos: [%ld..%ld..%ld..%lld..%ld]",
					(long)(start / 1000),
					(long)(position / 1000),
					(long)(end / 1000),
					(long long) videoPTS,
					(long)(bufferedDuration / 1000) );
			}
		}
		mReportProgressPosn = position;
		SendEventSync(evt);
		mReportProgressTime = aamp_GetCurrentTimeMS();
	}
}

 /*
 * @brief Report Ad progress event to listeners
 *
 * Sending Ad progress percentage to JSPP
 */
void PrivateInstanceAAMP::ReportAdProgress(bool sync)
{
	if (mDownloadsEnabled && !mAdProgressId.empty())
	{
		long long curTime = NOW_STEADY_TS_MS;
		if (!pipeline_paused)
		{
			//Update the percentage only if the pipeline is in playing.
			mAdCurOffset += (uint32_t)(curTime - mAdPrevProgressTime);
			if(mAdCurOffset > mAdDuration) mAdCurOffset = mAdDuration;
		}
		mAdPrevProgressTime = curTime;

		AdPlacementEventPtr evt = std::make_shared<AdPlacementEvent>(AAMP_EVENT_AD_PLACEMENT_PROGRESS, mAdProgressId, (uint32_t)(mAdCurOffset * 100) / mAdDuration);
		if(sync)
		{
			SendEventSync(evt);
		}
		else
		{
			SendEventAsync(evt);
		}
	}
}


/**
 * @brief Update duration of stream.
 *
 * Called from fragmentcollector_hls::IndexPlaylist to update TSB duration
 *
 * @param[in] seconds Duration in seconds
 */
void PrivateInstanceAAMP::UpdateDuration(double seconds)
{
	AAMPLOG_INFO("aamp_UpdateDuration(%f)", seconds);
	durationSeconds = seconds;
}

/**
 * @brief Update culling state in case of TSB
 * @param culledSecs culled duration in seconds
 */
void PrivateInstanceAAMP::UpdateCullingState(double culledSecs)
{
	if (culledSecs == 0)
	{
		return;
	}

	if((!this->culledSeconds) && culledSecs)
	{
		logprintf("PrivateInstanceAAMP::%s - culling started, first value %f", __FUNCTION__, culledSecs);
	}

	this->culledSeconds += culledSecs;
	long long limitMs = (long long) std::round(this->culledSeconds * 1000.0);

	for (auto iter = timedMetadata.begin(); iter != timedMetadata.end(); )
	{
		// If the timed metadata has expired due to playlist refresh, remove it from local cache
		// For X-CONTENT-IDENTIFIER, -X-IDENTITY-ADS, X-MESSAGE_REF in DASH which has _timeMS as 0
		if (iter->_timeMS != 0 && iter->_timeMS < limitMs)
		{
			//logprintf("ERASE(limit:%lld) aamp_ReportTimedMetadata(%lld, '%s', '%s', nb)", limitMs,iter->_timeMS, iter->_name.c_str(), iter->_content.c_str());
			iter = timedMetadata.erase(iter);
		}
		else
		{
			iter++;
		}
	}

	// Check if we are paused and culled past paused playback position
	// AAMP internally caches fragments in sw and gst buffer, so we should be good here
	// Pipeline will be in Paused state when Lightning trickplay is done. During this state XRE will send the resume position to exit pause state .
	// Issue observed when culled position reaches the paused position during lightning trickplay and player resumes the playback with paused position as playback position ignoring XRE shown position.
	// Fix checks if the player is put into paused state with lighting mode(by checking last stored rate). 
  	// In this state player will not come out of Paused state, even if the culled position reaches paused position.
	if (pipeline_paused && mpStreamAbstractionAAMP && (rate != AAMP_RATE_FWD_4X) && (rate != AAMP_RATE_REW_4X))
	{
		double position = GetPositionMilliseconds() / 1000.0; // in seconds
		double minPlaylistPositionToResume = (position < maxRefreshPlaylistIntervalSecs) ? position : (position - maxRefreshPlaylistIntervalSecs);
		if (this->culledSeconds >= position)
		{
			logprintf("%s(): Resume playback since playlist start position(%f) has moved past paused position(%f) ", __FUNCTION__, this->culledSeconds, position);
			g_idle_add(PrivateInstanceAAMP_Resume, (gpointer)this);
		}
		else if (this->culledSeconds >= minPlaylistPositionToResume)
		{
			// Here there is a chance that paused position will be culled after next refresh playlist
			// AAMP internally caches fragments in sw bufffer after paused position, so we are at less risk
			// Make sure that culledSecs is within the limits of maxRefreshPlaylistIntervalSecs
			// This check helps us to avoid initial culling done by FOG after channel tune

			if (culledSecs <= maxRefreshPlaylistIntervalSecs)
			{
				logprintf("%s(): Resume playback since start position(%f) moved very close to minimum resume position(%f) ", __FUNCTION__, this->culledSeconds, minPlaylistPositionToResume);
				g_idle_add(PrivateInstanceAAMP_Resume, (gpointer)this);
			}
		}

	}
}

/**
 * @brief Add listener to aamp events
 * @param eventType type of event to subscribe
 * @param eventListener listener
 */
void PrivateInstanceAAMP::AddEventListener(AAMPEventType eventType, EventListener* eventListener)
{
	//logprintf("[AAMP_JS] %s(%d, %p)", __FUNCTION__, eventType, eventListener);
	if ((eventListener != NULL) && (eventType >= 0) && (eventType < AAMP_MAX_NUM_EVENTS))
	{
		ListenerData* pListener = new ListenerData;
		if (pListener)
		{
			//logprintf("[AAMP_JS] %s(%d, %p) new %p", __FUNCTION__, eventType, eventListener, pListener);
			pthread_mutex_lock(&mLock);
			pListener->eventListener = eventListener;
			pListener->pNext = mEventListeners[eventType];
			mEventListeners[eventType] = pListener;
			pthread_mutex_unlock(&mLock);
		}
	}
}


/**
 * @brief Remove listener to aamp events
 * @param eventType type of event to unsubscribe
 * @param eventListener listener
 */
void PrivateInstanceAAMP::RemoveEventListener(AAMPEventType eventType, EventListener* eventListener)
{
	if ((eventListener != NULL) && (eventType >= 0) && (eventType < AAMP_MAX_NUM_EVENTS))
	{
		pthread_mutex_lock(&mLock);
		ListenerData** ppLast = &mEventListeners[eventType];
		while (*ppLast != NULL)
		{
			ListenerData* pListener = *ppLast;
			if (pListener->eventListener == eventListener)
			{
				*ppLast = pListener->pNext;
				pthread_mutex_unlock(&mLock);
				AAMPLOG_INFO("[AAMP_JS] %s(%d, %p) delete %p", __FUNCTION__, eventType, eventListener, pListener);
				delete pListener;
				return;
			}
			ppLast = &(pListener->pNext);
		}
		pthread_mutex_unlock(&mLock);
	}
}

/**
 * @brief Handles DRM errors and sends events to application if required.
 * @param[in] event aamp event struck which holds the error details and error code(http, curl or secclient).
 * @param[in] isRetryEnabled drm retry enabled
 */
void PrivateInstanceAAMP::SendDrmErrorEvent(DrmMetaDataEventPtr event, bool isRetryEnabled)
{
	if (event)
	{
		AAMPTuneFailure tuneFailure = event->getFailure();
		long error_code = event->getResponseCode();
		bool isSecClientError = event->secclientError();

		if(AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN == tuneFailure || AAMP_TUNE_LICENCE_REQUEST_FAILED == tuneFailure)
		{
			char description[128] = {};

			if(AAMP_TUNE_LICENCE_REQUEST_FAILED == tuneFailure && error_code < 100)
			{
				if (isSecClientError)
				{
					snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "%s : Secclient Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
				}
				else
				{
					snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "%s : Curl Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
				}
			}
			else if (AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN == tuneFailure && eAUTHTOKEN_TOKEN_PARSE_ERROR == (AuthTokenErrors)error_code)
			{
				snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "%s : Access Token Parse Error", tuneFailureMap[tuneFailure].description);
			}
			else if(AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN == tuneFailure && eAUTHTOKEN_INVALID_STATUS_CODE == (AuthTokenErrors)error_code)
			{
				snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "%s : Invalid status code", tuneFailureMap[tuneFailure].description);
			}
			else
			{
				snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "%s : Http Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
			}
			SendErrorEvent(tuneFailure, description, isRetryEnabled);
		}
		else if(tuneFailure >= 0 && tuneFailure < AAMP_TUNE_FAILURE_UNKNOWN)
		{
			SendErrorEvent(tuneFailure, NULL, isRetryEnabled);
		}
		else
		{
			logprintf("%s:%d : Received unknown error event %d", __FUNCTION__, __LINE__, tuneFailure);
			SendErrorEvent(AAMP_TUNE_FAILURE_UNKNOWN);
		}
	}
}

/**
 * @brief Handles download errors and sends events to application if required.
 * @param tuneFailure Reason of error
 * @param error_code HTTP error code/ CURLcode
 */
void PrivateInstanceAAMP::SendDownloadErrorEvent(AAMPTuneFailure tuneFailure, long error_code)
{
	AAMPTuneFailure actualFailure = tuneFailure;

	if(tuneFailure >= 0 && tuneFailure < AAMP_TUNE_FAILURE_UNKNOWN)
	{
		char description[MAX_DESCRIPTION_SIZE] = {};
		if (((error_code >= PARTIAL_FILE_CONNECTIVITY_AAMP) && (error_code <= PARTIAL_FILE_START_STALL_TIMEOUT_AAMP)) || error_code == CURLE_OPERATION_TIMEDOUT)
		{
			switch(error_code)
			{
				case PARTIAL_FILE_DOWNLOAD_TIME_EXPIRED_AAMP:
						error_code = CURLE_PARTIAL_FILE;
				case CURLE_OPERATION_TIMEDOUT:
						snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Curl Error Code %ld, Download time expired", tuneFailureMap[tuneFailure].description, error_code);
						break;
				case PARTIAL_FILE_START_STALL_TIMEOUT_AAMP:
						snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Curl Error Code %d, Start/Stall timeout", tuneFailureMap[tuneFailure].description, CURLE_PARTIAL_FILE);
						break;
				case OPERATION_TIMEOUT_CONNECTIVITY_AAMP:
						snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Curl Error Code %d, Connectivity failure", tuneFailureMap[tuneFailure].description, CURLE_OPERATION_TIMEDOUT);
						break;
				case PARTIAL_FILE_CONNECTIVITY_AAMP:
						snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Curl Error Code %d, Connectivity failure", tuneFailureMap[tuneFailure].description, CURLE_PARTIAL_FILE);
						break;
			}
		}
		else if(error_code < 100)
		{
			snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Curl Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);  //CID:86441 - DC>STRING_BUFFER
		}
		else
		{
			snprintf(description,MAX_DESCRIPTION_SIZE, "%s : Http Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
			if (error_code == 404)
			{
				actualFailure = AAMP_TUNE_CONTENT_NOT_FOUND;
			}
		}
		if( IsTSBSupported() )
		{
			strcat(description, "(FOG)");
		}

		SendErrorEvent(actualFailure, description);
	}
	else
	{
		logprintf("%s:%d : Received unknown error event %d", __FUNCTION__, __LINE__, tuneFailure);
		SendErrorEvent(AAMP_TUNE_FAILURE_UNKNOWN);
	}
}

/**
 * @brief Sends Anomaly Error/warning messages
 *
 * @param[in] type - severity of message
 * @param[in] format - format string
 * args [in]  - multiple arguments based on format
 * @return void
 */
void PrivateInstanceAAMP::SendAnomalyEvent(AAMPAnomalyMessageType type, const char* format, ...)
{
	if(NULL != format)
	{
		va_list args;
		va_start(args, format);

		char msgData[MAX_ANOMALY_BUFF_SIZE];

		msgData[(MAX_ANOMALY_BUFF_SIZE-1)] = 0;
		vsnprintf(msgData, (MAX_ANOMALY_BUFF_SIZE-1), format, args);

		AnomalyReportEventPtr e = std::make_shared<AnomalyReportEvent>(type, msgData);

		AAMPLOG_INFO("Anomaly evt:%d msg:%s", e->getSeverity(), msgData);
		SendEventAsync(e);
		va_end(args);  //CID:82734 - VARAGAS
	}
}


/**
 * @brief Update playlist refresh interval
 * @param maxIntervalSecs refresh interval in seconds
 */
void PrivateInstanceAAMP::UpdateRefreshPlaylistInterval(float maxIntervalSecs)
{
	AAMPLOG_INFO("%s(): maxRefreshPlaylistIntervalSecs (%f)", __FUNCTION__, maxIntervalSecs);
	maxRefreshPlaylistIntervalSecs = maxIntervalSecs;
}


/**
 * @brief Sends UnderFlow Event messages
 *
 * @param[in] bufferingStopped- Flag to indicate buffering stopped (Underflow started true else false)
 * @return void
 */
void PrivateInstanceAAMP::SendBufferChangeEvent(bool bufferingStopped)
{
	BufferingChangedEventPtr e = std::make_shared<BufferingChangedEvent>(!bufferingStopped); /* False if Buffering End, True if Buffering Start*/

	SetBufUnderFlowStatus(bufferingStopped);
	AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d Sending Buffer Change event status (Buffering): %s", __FUNCTION__, __LINE__, (e->buffering() ? "Start": "End"));
	SendEventAsync(e);
}

/**
 * @brief To change the the gstreamer pipeline to pause/play
 *
 * @param[in] pause- true for pause and false for play
 * @param[in] forceStopGstreamerPreBuffering - true for disabling bufferinprogress
 * @return true on success
 */
bool PrivateInstanceAAMP::PausePipeline(bool pause, bool forceStopGstreamerPreBuffering)
{
	if (true != mStreamSink->Pause(pause, forceStopGstreamerPreBuffering))
	{
		return false;
	}
	pipeline_paused = pause;
	return true;
}

/**
 * @brief Handles errors and sends events to application if required.
 * For download failures, use SendDownloadErrorEvent instead.
 * @param tuneFailure Reason of error
 * @param description Optional description of error
 */
void PrivateInstanceAAMP::SendErrorEvent(AAMPTuneFailure tuneFailure, const char * description, bool isRetryEnabled)
{
	bool sendErrorEvent = false;
	pthread_mutex_lock(&mLock);
	if(mState != eSTATE_ERROR)
	{
		if(IsTSBSupported() && mState <= eSTATE_PREPARED)
		{
			// Send a TSB delete request when player is not tuned successfully.
			// If player is once tuned, retune happens with same content and player can reuse same TSB.
			std::string remoteUrl = "127.0.0.1:9080/tsb";
			long http_error = -1;
			ProcessCustomCurlRequest(remoteUrl, NULL, &http_error, eCURL_DELETE);
		}
		sendErrorEvent = true;
		mState = eSTATE_ERROR;
	}
	pthread_mutex_unlock(&mLock);
	if (sendErrorEvent)
	{
		int code;
		const char *errorDescription = NULL;
		DisableDownloads();
		if(tuneFailure >= 0 && tuneFailure < AAMP_TUNE_FAILURE_UNKNOWN)
		{
			if (tuneFailure == AAMP_TUNE_PLAYBACK_STALLED)
			{ // allow config override for stall detection error code
				code = gpGlobalConfig->stallErrorCode;
			}
			else
			{
				code = tuneFailureMap[tuneFailure].code;
			}
			if(description)
			{
				errorDescription = description;
			}
			else
			{
				errorDescription = tuneFailureMap[tuneFailure].description;
			}
		}
		else
		{
			code = tuneFailureMap[AAMP_TUNE_FAILURE_UNKNOWN].code;
			errorDescription = tuneFailureMap[AAMP_TUNE_FAILURE_UNKNOWN].description;
		}

		MediaErrorEventPtr e = std::make_shared<MediaErrorEvent>(tuneFailure, code, errorDescription, isRetryEnabled);
		SendAnomalyEvent(ANOMALY_ERROR, "Error[%d]:%s", tuneFailure, e->getDescription().c_str());
		if (!mAppName.empty())
		{
			logprintf("%s PLAYER[%d] APP: %s Sending error %s",(mbPlayEnabled?STRFGPLAYER:STRBGPLAYER), mPlayerId, mAppName.c_str(), e->getDescription().c_str());
		}
		else
		{
			logprintf("%s PLAYER[%d] Sending error %s",(mbPlayEnabled?STRFGPLAYER:STRBGPLAYER), mPlayerId, e->getDescription().c_str());
		}

		if (rate != AAMP_NORMAL_PLAY_RATE)
		{
			NotifySpeedChanged(AAMP_NORMAL_PLAY_RATE, false); // During trick play if the playback failed, send speed change event to XRE to reset its current speed rate.
		}

		SendEventAsync(e);
	}
	else
	{
		logprintf("PrivateInstanceAAMP::%s:%d Ignore error %d[%s]", __FUNCTION__, __LINE__, (int)tuneFailure, description);
	}

	mFogErrorString.clear();
}


/**
 * @brief Send event asynchronously to listeners
 * @param e event
 */
void PrivateInstanceAAMP::SendEventAsync(AAMPEventPtr e)
{
	AAMPEventType eventType = e->getType();
	if (mEventListener || mEventListeners[0] || mEventListeners[eventType])
	{
		AsyncEventDescriptor* aed = new AsyncEventDescriptor();
		aed->event = e;
		ScheduleEvent(aed);
		if(eventType != AAMP_EVENT_PROGRESS)
		{
			AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d event type  %d", __FUNCTION__, __LINE__, eventType);
		}
	}
	else
	{
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d Failed to send event type  %d", __FUNCTION__, __LINE__, eventType);
	}
}


/**
 * @brief Send event synchronously to listeners
 * @param e event
 */
void PrivateInstanceAAMP::SendEventSync(AAMPEventPtr e)
{
	AAMPEventType eventType = e->getType();
	if (eventType != AAMP_EVENT_PROGRESS)
	{
		if (eventType != AAMP_EVENT_STATE_CHANGED)
		{
			AAMPLOG_INFO("[AAMP_JS] %s(type=%d)", __FUNCTION__, eventType);
		}
		else
		{
			AAMPLOG_WARN("[AAMP_JS] %s(type=%d)(state=%d)", __FUNCTION__, eventType, std::dynamic_pointer_cast<StateChangedEvent>(e)->getState());
		}
	}

	//TODO protect mEventListener
	if (mEventListener)
	{
		mEventListener->SendEvent(e);
	}

	if ((eventType < 0) || (eventType >= AAMP_MAX_NUM_EVENTS))  //CID:81883 - Resolve OVER_RUN
		return;

	// Build list of registered event listeners.
	ListenerData* pList = NULL;
	pthread_mutex_lock(&mLock);
	ListenerData* pListener = mEventListeners[eventType];
	while (pListener != NULL)
	{
		ListenerData* pNew = new ListenerData;
		pNew->eventListener = pListener->eventListener;
		pNew->pNext = pList;
		pList = pNew;
		pListener = pListener->pNext;
	}
	pListener = mEventListeners[0];  // listeners registered for "all" event types
	while (pListener != NULL)
	{
		ListenerData* pNew = new ListenerData;
		pNew->eventListener = pListener->eventListener;
		pNew->pNext = pList;
		pList = pNew;
		pListener = pListener->pNext;
	}
	pthread_mutex_unlock(&mLock);

	// After releasing the lock, dispatch each of the registered listeners.
	// This allows event handlers to add/remove listeners for future events.
	while (pList != NULL)
	{
		ListenerData* pCurrent = pList;
		if (pCurrent->eventListener != NULL)
		{
			//logprintf("[AAMP_JS] %s(type=%d) listener=%p", __FUNCTION__, eventType, pCurrent->eventListener);
			pCurrent->eventListener->SendEvent(e);
		}
		pList = pCurrent->pNext;
		delete pCurrent;
	}
}

/**
 * @brief Notify bitrate change event to listeners
 * @param bitrate new bitrate
 * @param reason reason for bitrate change
 * @param width new width in pixels
 * @param height new height in pixels
 * @param GetBWIndex get bandwidth index - used for logging
 */
void PrivateInstanceAAMP::NotifyBitRateChangeEvent(int bitrate, BitrateChangeReason reason, int width, int height, double frameRate, double position, bool GetBWIndex)
{
	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_BITRATE_CHANGED])
	{
		AsyncEventDescriptor* e = new AsyncEventDescriptor();
		e->event = std::make_shared<BitrateChangeEvent>((int)aamp_GetCurrentTimeMS(), bitrate, BITRATEREASON2STRING(reason), width, height, frameRate, position);

		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		if(GetBWIndex && (mpStreamAbstractionAAMP != NULL))
		{
			logprintf("NotifyBitRateChangeEvent :: bitrate:%d desc:%s width:%d height:%d fps:%f position:%f IndexFromTopProfile: %d%s",
				bitrate, BITRATEREASON2STRING(reason), width, height, frameRate, position, mpStreamAbstractionAAMP->GetBWIndex(bitrate), (IsTSBSupported()? ", fog": " "));
		}
		else
		{
			logprintf("NotifyBitRateChangeEvent :: bitrate:%d desc:%s width:%d height:%d fps:%f position:%f %s",
				bitrate, BITRATEREASON2STRING(reason), width, height, frameRate, position, (IsTSBSupported()? ", fog": " "));
		}
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */

		ScheduleEvent(e);
	}
	else
	{
		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		if(GetBWIndex && (mpStreamAbstractionAAMP != NULL))
		{
			logprintf("NotifyBitRateChangeEvent ::NO LISTENERS bitrate:%d desc:%s width:%d height:%d, fps:%f position:%f IndexFromTopProfile: %d%s", 
				bitrate, BITRATEREASON2STRING(reason), width, height, frameRate, position, mpStreamAbstractionAAMP->GetBWIndex(bitrate), (IsTSBSupported()? ", fog": " "));
		}
		else
		{
			logprintf("NotifyBitRateChangeEvent ::NO LISTENERS bitrate:%d desc:%s width:%d height:%d fps:%f position:%f %s", 
				bitrate, BITRATEREASON2STRING(reason), width, height, frameRate, position, (IsTSBSupported()? ", fog": " "));
		}
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */
	}

	logprintf("BitrateChanged:%d", reason);
}


/**
 * @brief Notify rate change event to listeners
 * @param rate new speed
 * @param changeState true if state change to be done, false otherwise (default = true)
 */
void PrivateInstanceAAMP::NotifySpeedChanged(int rate, bool changeState)
{
	if (changeState)
	{
		if (rate == 0)
		{
			SetState(eSTATE_PAUSED);
		}
		else if (rate == AAMP_NORMAL_PLAY_RATE)
		{
			SetState(eSTATE_PLAYING);
		}
	}

#ifdef AAMP_CC_ENABLED
	if (gpGlobalConfig->nativeCCRendering)
	{
		if (rate == AAMP_NORMAL_PLAY_RATE)
		{
			AampCCManager::GetInstance()->SetTrickplayStatus(false);
		}
		else
		{
			AampCCManager::GetInstance()->SetTrickplayStatus(true);
		}
	}
#endif

	SendEventAsync(std::make_shared<SpeedChangedEvent>(rate));
}

/**
 * @brief Send DRM metadata event
 * @param e DRM metadata event
 */
void PrivateInstanceAAMP::SendDRMMetaData(DrmMetaDataEventPtr e)
{
        SendEventAsync(e);
        logprintf("SendDRMMetaData name = %s value = %x", e->getAccessStatus().c_str(), e->getAccessStatusValue());
}

/**
 * @brief Check if discontinuity processing is pending
 *
 * @retval true if discontinuity processing is pending
 */
bool PrivateInstanceAAMP::IsDiscontinuityProcessPending()
{
	bool vidDiscontinuity = (mVideoFormat != FORMAT_INVALID && mProcessingDiscontinuity[eMEDIATYPE_VIDEO]);
	bool audDiscontinuity = (mAudioFormat != FORMAT_INVALID && mProcessingDiscontinuity[eMEDIATYPE_AUDIO]);
	return (vidDiscontinuity || audDiscontinuity);
}

/**
 * @brief Process pending discontinuity and continue playback of stream after discontinuity
 *
 * @return true if pending discontinuity was processed successful, false if interrupted
 */
bool PrivateInstanceAAMP::ProcessPendingDiscontinuity()
{
	bool ret = true;
	SyncBegin();
	if (mDiscontinuityTuneOperationInProgress)
	{
		SyncEnd();
		logprintf("PrivateInstanceAAMP::%s:%d Discontinuity Tune Operation already in progress", __FUNCTION__, __LINE__);
		return ret; // true so that PrivateInstanceAAMP_ProcessDiscontinuity can cleanup properly
	}
	SyncEnd();

	if (!(mProcessingDiscontinuity[eMEDIATYPE_VIDEO] && mProcessingDiscontinuity[eMEDIATYPE_AUDIO]))
	{
		AAMPLOG_ERR("PrivateInstanceAAMP::%s:%d Discontinuity status of video - (%d) and audio - (%d)", __FUNCTION__, __LINE__, mProcessingDiscontinuity[eMEDIATYPE_VIDEO], mProcessingDiscontinuity[eMEDIATYPE_AUDIO]);
		return ret; // true so that PrivateInstanceAAMP_ProcessDiscontinuity can cleanup properly
	}

	SyncBegin();
	mDiscontinuityTuneOperationInProgress = true;
	SyncEnd();

	if (mProcessingDiscontinuity[eMEDIATYPE_AUDIO] && mProcessingDiscontinuity[eMEDIATYPE_VIDEO])
	{
		bool continueDiscontProcessing = true;
		logprintf("PrivateInstanceAAMP::%s:%d mProcessingDiscontinuity set", __FUNCTION__, __LINE__);
		// DELIA-46559, there is a chance that synchronous progress event sent will take some time to return back to AAMP
		// This can lead to discontinuity stall detection kicking in. So once we start discontinuity processing, reset the flags
		mProcessingDiscontinuity[eMEDIATYPE_VIDEO] = false;
		mProcessingDiscontinuity[eMEDIATYPE_AUDIO] = false;
		ResetTrackDiscontinuityIgnoredStatus();
		lastUnderFlowTimeMs[eMEDIATYPE_VIDEO] = 0;
		lastUnderFlowTimeMs[eMEDIATYPE_AUDIO] = 0;

		{
			double newPosition = GetPositionMilliseconds() / 1000.0;
			double injectedPosition = seek_pos_seconds + mpStreamAbstractionAAMP->GetLastInjectedFragmentPosition();
			AAMPLOG_WARN("PrivateInstanceAAMP::%s:%d last injected position:%f position calcualted: %f", __FUNCTION__, __LINE__, injectedPosition, newPosition);

			// Reset with injected position from StreamAbstractionAAMP. This ensures that any drift in
			// GStreamer position reporting is taken care of.
			// BCOM-4765: Set seek_pos_seconds to injected position only in case of westerossink. In cases with
			// brcmvideodecoder, we have noticed a drift of 500ms for HLS-TS assets (due to PTS restamping
			if (injectedPosition != 0 && (fabs(injectedPosition - newPosition) < 5.0) && mWesterosSinkEnabled)
			{
				seek_pos_seconds = injectedPosition;
			}
			else
			{
				seek_pos_seconds = newPosition;
			}
			AAMPLOG_WARN("PrivateInstanceAAMP::%s:%d Updated seek_pos_seconds:%f", __FUNCTION__, __LINE__, seek_pos_seconds);
		}
		trickStartUTCMS = -1;

		SyncBegin();
		mProgressReportFromProcessDiscontinuity = true;
		SyncEnd();

		// To notify app of discontinuity processing complete
		ReportProgress();

		// There is a chance some other operation maybe invoked from JS/App because of the above ReportProgress
		// Make sure we have still mDiscontinuityTuneOperationInProgress set
		SyncBegin();
		AAMPLOG_WARN("%s:%d Progress event sent as part of ProcessPendingDiscontinuity, mDiscontinuityTuneOperationInProgress:%d", __FUNCTION__, __LINE__, mDiscontinuityTuneOperationInProgress);
		mProgressReportFromProcessDiscontinuity = false;
		continueDiscontProcessing = mDiscontinuityTuneOperationInProgress;
		SyncEnd();

		if (continueDiscontProcessing)
		{
			mpStreamAbstractionAAMP->StopInjection();
#ifndef AAMP_STOP_SINK_ON_SEEK
			if (mMediaFormat != eMEDIAFORMAT_HLS_MP4) // Avoid calling flush for fmp4 playback.
			{
				mStreamSink->Flush(mpStreamAbstractionAAMP->GetFirstPTS(), rate);
			}
#else
			mStreamSink->Stop(true);
#endif
			mpStreamAbstractionAAMP->GetStreamFormat(mVideoFormat, mAudioFormat);
			mStreamSink->Configure(mVideoFormat, mAudioFormat, mpStreamAbstractionAAMP->GetESChangeStatus());
			mpStreamAbstractionAAMP->ResetESChangeStatus();
			mpStreamAbstractionAAMP->StartInjection();
			mStreamSink->Stream();
		}
		else
		{
			ret = false;
			AAMPLOG_WARN("PrivateInstanceAAMP::%s:%d mDiscontinuityTuneOperationInProgress was reset during operation, since other command received from app!", __FUNCTION__, __LINE__);
		}
	}

	if (ret)
	{
		SyncBegin();
		mDiscontinuityTuneOperationInProgress = false;
		SyncEnd();
	}

	return ret;
}

/**
 * @brief Process EOS from Sink and notify listeners if required
 */
void PrivateInstanceAAMP::NotifyEOSReached()
{
	logprintf("%s: Enter . processingDiscontinuity %d",__FUNCTION__, (mProcessingDiscontinuity[eMEDIATYPE_VIDEO] || mProcessingDiscontinuity[eMEDIATYPE_AUDIO]));
	if (!IsDiscontinuityProcessPending())
	{
		if (!mpStreamAbstractionAAMP->IsEOSReached())
		{
			AAMPLOG_ERR("%s: Bogus EOS event received from GStreamer, discarding it!", __FUNCTION__);
			return;
		}
		if (!IsLive() && rate > 0)
		{
			SetState(eSTATE_COMPLETE);
			SendEventAsync(std::make_shared<AAMPEventObject>(AAMP_EVENT_EOS));
			if (ContentType_EAS == mContentType) //Fix for DELIA-25590
			{
				mStreamSink->Stop(false);
			}
			SendAnomalyEvent(ANOMALY_TRACE, "Generating EOS event");
			return;
		}

		if (rate < 0)
		{
			seek_pos_seconds = culledSeconds;
			logprintf("%s:%d Updated seek_pos_seconds %f ", __FUNCTION__,__LINE__, seek_pos_seconds);
			rate = AAMP_NORMAL_PLAY_RATE;
			TuneHelper(eTUNETYPE_SEEK);
		}
		else
		{
			rate = AAMP_NORMAL_PLAY_RATE;
			TuneHelper(eTUNETYPE_SEEKTOLIVE);
		}

		NotifySpeedChanged(rate);
	}
	else
	{
		ProcessPendingDiscontinuity();
		DeliverAdEvents();
		logprintf("PrivateInstanceAAMP::%s:%d  EOS due to discontinuity handled", __FUNCTION__, __LINE__);
	}
}

/**
 * @brief Notify entering live event to listeners
 */
void PrivateInstanceAAMP::NotifyOnEnteringLive()
{
	if (discardEnteringLiveEvt)
	{
		return;
	}
	SendEventAsync(std::make_shared<AAMPEventObject>(AAMP_EVENT_ENTERING_LIVE));
}

/**
 * @brief Schedule asynchronous event
 * @param e event descriptor
 */
void PrivateInstanceAAMP::ScheduleEvent(AsyncEventDescriptor* e)
{
	//TODO protect mEventListener
	e->aamp = this;
	guint callbackID = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, SendAsynchronousEvent, e, AsyncEventDestroyNotify);
	SetCallbackAsPending(callbackID);
}

/**
 * @brief Send tune events to receiver
 *
 * @param[in] success - Tune status
 * @return void
 */
void PrivateInstanceAAMP::sendTuneMetrics(bool success)
{
	std::string eventsJSON;
	profiler.getTuneEventsJSON(eventsJSON, getStreamTypeString(),GetTunedManifestUrl(),success);
	SendMessage2Receiver(E_AAMP2Receiver_EVENTS,eventsJSON.c_str());

	//for now, avoid use of logprintf, to avoid potential truncation when URI in tune profiling or
	//extra events push us over limit
	AAMPLOG_WARN("tune-profiling: %s", eventsJSON.c_str());

	TuneProfilingEventPtr e = std::make_shared<TuneProfilingEvent>(eventsJSON);
	SendEventAsync(e);
}

/**
 * @brief Notify tune end for profiling/logging
 */
void PrivateInstanceAAMP::LogTuneComplete(void)
{
	bool success = true; // TODO
	int streamType = getStreamType();
	profiler.TuneEnd(success, mContentType, streamType, mFirstTune, mAppName,(mbPlayEnabled?STRFGPLAYER:STRBGPLAYER), mPlayerId, mPlayerPreBuffered);

	//update tunedManifestUrl if FOG was NOT used as manifestUrl might be updated with redirected url.
	if(!IsTSBSupported())
	{
		SetTunedManifestUrl(); /* Redirect URL in case on VOD */
	}

	if (!mTuneCompleted)
	{
		char classicTuneStr[AAMP_MAX_PIPE_DATA_SIZE];
		profiler.GetClassicTuneTimeInfo(success, mTuneAttempts, mfirstTuneFmt, mPlayerLoadTime, streamType, IsLive(), durationSeconds, classicTuneStr);
		SendMessage2Receiver(E_AAMP2Receiver_TUNETIME,classicTuneStr);
		if(gpGlobalConfig->enableMicroEvents) sendTuneMetrics(success);
		mTuneCompleted = true;
		mFirstTune = false;

		AAMPAnomalyMessageType eMsgType = AAMPAnomalyMessageType::ANOMALY_TRACE;
		if(mTuneAttempts > 1 )
		{
		    eMsgType = AAMPAnomalyMessageType::ANOMALY_WARNING;
		}
		std::string playbackType = GetContentTypString();

		if(mContentType == ContentType_LINEAR)
		{
			if(mTSBEnabled)
			{
				playbackType.append(":TSB=true");
			}
			else
			{
				playbackType.append(":TSB=false");
			}
		}

		SendAnomalyEvent(eMsgType, "Tune attempt#%d. %s:%s URL:%s", mTuneAttempts,playbackType.c_str(),getStreamTypeString().c_str(),GetTunedManifestUrl());
	}

	gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_WARN);
}

/**
 * @brief Notifies profiler that first frame is presented
 */
void PrivateInstanceAAMP::LogFirstFrame(void)
{
	profiler.ProfilePerformed(PROFILE_BUCKET_FIRST_FRAME);
}

/**
 * @brief Notifies profiler that player state from background to foreground i.e prebuffred
 */
void PrivateInstanceAAMP::LogPlayerPreBuffered(void)
{
       profiler.ProfilePerformed(PROFILE_BUCKET_PLAYER_PRE_BUFFERED);
}

/**
 * @brief Notifies profiler that drm initialization is complete
 */
void PrivateInstanceAAMP::LogDrmInitComplete(void)
{
	profiler.ProfileEnd(PROFILE_BUCKET_LA_TOTAL);
}

/**
 * @brief Notifies profiler that decryption has started
 * @param bucketType profiler bucket type
 */
void PrivateInstanceAAMP::LogDrmDecryptBegin(ProfilerBucketType bucketType)
{
	profiler.ProfileBegin(bucketType);
}

/**
 * @brief Notifies profiler that decryption has ended
 * @param bucketType profiler bucket type
 */
void PrivateInstanceAAMP::LogDrmDecryptEnd(ProfilerBucketType bucketType)
{
	profiler.ProfileEnd(bucketType);
}

/**
 * @brief Stop downloads of all tracks.
 * Used by aamp internally to manage states
 */
void PrivateInstanceAAMP::StopDownloads()
{
	traceprintf ("PrivateInstanceAAMP::%s", __FUNCTION__);
	if (!mbDownloadsBlocked)
	{
		pthread_mutex_lock(&mLock);
		mbDownloadsBlocked = true;
		pthread_mutex_unlock(&mLock);
	}
}

/**
 * @brief Resume downloads of all tracks.
 * Used by aamp internally to manage states
 */
void PrivateInstanceAAMP::ResumeDownloads()
{
	traceprintf ("PrivateInstanceAAMP::%s", __FUNCTION__);
	if (mbDownloadsBlocked)
	{
		pthread_mutex_lock(&mLock);
		mbDownloadsBlocked = false;
		//log_current_time("gstreamer-needs-data");
		pthread_mutex_unlock(&mLock);
	}
}

/**
 * @brief Stop downloads for a track.
 * Called from StreamSink to control flow
 * @param type media type of the track
 */
void PrivateInstanceAAMP::StopTrackDownloads(MediaType type)
{ // called from gstreamer main event loop
#ifdef AAMP_DEBUG_FETCH_INJECT
	if ((1 << type) & AAMP_DEBUG_FETCH_INJECT)
	{
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d", __FUNCTION__, (int) type);
	}
#endif
	if (!mbTrackDownloadsBlocked[type])
	{
		AAMPLOG_TRACE("gstreamer-enough-data from %s source", (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mbTrackDownloadsBlocked[type] = true;
		pthread_mutex_unlock(&mLock);
		NotifySinkBufferFull(type);
	}
	traceprintf ("PrivateInstanceAAMP::%s Enter. type = %d", __FUNCTION__, (int) type);
}

/**
 * @brief Resume downloads for a track.
 * Called from StreamSink to control flow
 * @param type media type of the track
 */
void PrivateInstanceAAMP::ResumeTrackDownloads(MediaType type)
{ // called from gstreamer main event loop
#ifdef AAMP_DEBUG_FETCH_INJECT
	if ((1 << type) & AAMP_DEBUG_FETCH_INJECT)
	{
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d", __FUNCTION__, (int) type);
	}
#endif
	if (mbTrackDownloadsBlocked[type])
	{
		AAMPLOG_TRACE("gstreamer-needs-data from %s source", (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mbTrackDownloadsBlocked[type] = false;
		//log_current_time("gstreamer-needs-data");
		pthread_mutex_unlock(&mLock);
	}
	traceprintf ("PrivateInstanceAAMP::%s Exit. type = %d", __FUNCTION__, (int) type);
}

/**
 * @brief block until gstreamer indicates pipeline wants more data
 * @param cb callback called periodically, if non-null
 * @param periodMs delay between callbacks
 * @param track track index
 */
void PrivateInstanceAAMP::BlockUntilGstreamerWantsData(void(*cb)(void), int periodMs, int track)
{ // called from FragmentCollector thread; blocks until gstreamer wants data
	traceprintf("PrivateInstanceAAMP::%s Enter. type = %d and downloads:%d", __FUNCTION__, track, mbTrackDownloadsBlocked[track]);
	int elapsedMs = 0;
	while (mbDownloadsBlocked || mbTrackDownloadsBlocked[track])
	{
		if (!mDownloadsEnabled || mTrackInjectionBlocked[track])
		{
			logprintf("PrivateInstanceAAMP::%s interrupted. mDownloadsEnabled:%d mTrackInjectionBlocked:%d", __FUNCTION__, mDownloadsEnabled, mTrackInjectionBlocked[track]);
			break;
		}
		if (cb && periodMs)
		{ // support for background tasks, i.e. refreshing manifest while gstreamer doesn't need additional data
			if (elapsedMs >= periodMs)
			{
				cb();
				elapsedMs -= periodMs;
			}
			elapsedMs += 10;
		}
		InterruptableMsSleep(10);
	}
	traceprintf("PrivateInstanceAAMP::%s Exit. type = %d", __FUNCTION__, track);
}

/**
 * @brief Initialize curl instances
 * @param startIdx start index
 * @param instanceCount count of instances
 */
void PrivateInstanceAAMP::CurlInit(AampCurlInstance startIdx, unsigned int instanceCount, const char *proxy)
{
	int instanceEnd = startIdx + instanceCount;
        long curlIPResolve;
	assert (instanceEnd <= eCURLINSTANCE_MAX);
        curlIPResolve = aamp_GetIPResolveValue();
	for (unsigned int i = startIdx; i < instanceEnd; i++)
	{
		if (!curl[i])
		{
			curl[i] = curl_easy_init();
			if (gpGlobalConfig->logging.curl)
			{
				curl_easy_setopt(curl[i], CURLOPT_VERBOSE, 1L);
			}
			curl_easy_setopt(curl[i], CURLOPT_NOSIGNAL, 1L);
			//curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback); // unused
			curl_easy_setopt(curl[i], CURLOPT_PROGRESSFUNCTION, progress_callback);
			curl_easy_setopt(curl[i], CURLOPT_HEADERFUNCTION, header_callback);
			curl_easy_setopt(curl[i], CURLOPT_WRITEFUNCTION, write_callback);
			curl_easy_setopt(curl[i], CURLOPT_TIMEOUT, DEFAULT_CURL_TIMEOUT);
			curl_easy_setopt(curl[i], CURLOPT_CONNECTTIMEOUT, DEFAULT_CURL_CONNECTTIMEOUT);
                        //Depending on ip mode supported by box, force curl to use that particular ip mode (DELIA-46626)
                        curl_easy_setopt(curl[i], CURLOPT_IPRESOLVE, curlIPResolve);
			curl_easy_setopt(curl[i], CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl[i], CURLOPT_NOPROGRESS, 0L); // enable progress meter (off by default)
			curl_easy_setopt(curl[i], CURLOPT_USERAGENT, gpGlobalConfig->pUserAgentString);
			curl_easy_setopt(curl[i], CURLOPT_ACCEPT_ENCODING, "");//Enable all the encoding formats supported by client
			curl_easy_setopt(curl[i], CURLOPT_SSL_CTX_FUNCTION, ssl_callback); //Check for downloads disabled in btw ssl handshake
			curl_easy_setopt(curl[i], CURLOPT_SSL_CTX_DATA, this);
			long dns_cache_timeout = 5*60;
			curl_easy_setopt(curl[i], CURLOPT_DNS_CACHE_TIMEOUT, dns_cache_timeout);
			curl_easy_setopt(curl[i], CURLOPT_SHARE, mCurlShared);

			curlDLTimeout[i] = DEFAULT_CURL_TIMEOUT * 1000;


			// dev override in cfg file takes priority to App Setting 
			if(gpGlobalConfig->httpProxy != NULL)
			{
				proxy = gpGlobalConfig->httpProxy;				
			}

			if (proxy != NULL)
			{
				/* use this proxy */
				curl_easy_setopt(curl[i], CURLOPT_PROXY, proxy);
				/* allow whatever auth the proxy speaks */
				curl_easy_setopt(curl[i], CURLOPT_PROXYAUTH, CURLAUTH_ANY);
			}

			if(ContentType_EAS == mContentType)
			{
				//enable verbose logs so we can debug field issues
				curl_easy_setopt(curl[i], CURLOPT_VERBOSE, 1);
				curl_easy_setopt(curl[i], CURLOPT_DEBUGFUNCTION, eas_curl_debug_callback);
				//set eas specific timeouts to handle faster cycling through bad hosts and faster total timeout
				curl_easy_setopt(curl[i], CURLOPT_TIMEOUT, EAS_CURL_TIMEOUT);
				curl_easy_setopt(curl[i], CURLOPT_CONNECTTIMEOUT, EAS_CURL_CONNECTTIMEOUT);

				curlDLTimeout[i] = EAS_CURL_TIMEOUT * 1000;

				//on ipv6 box force curl to use ipv6 mode only (DELIA-20209)
				struct stat tmpStat;
				bool isv6(::stat( "/tmp/estb_ipv6", &tmpStat) == 0);
				if(isv6)
					curl_easy_setopt(curl[i], CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
				logprintf("aamp eas curl config: timeout=%d, connecttimeout%d, ipv6=%d", EAS_CURL_TIMEOUT, EAS_CURL_CONNECTTIMEOUT, isv6);
			}
			//log_current_time("curl initialized");
		}
	}
}


/**
 * @brief Store language list of stream
 * @param langlist Array of languges
 */
void PrivateInstanceAAMP::StoreLanguageList(const std::set<std::string> &langlist)
{
	// store the language list
	int langCount = langlist.size();
	if (langCount > MAX_LANGUAGE_COUNT)
	{
		langCount = MAX_LANGUAGE_COUNT; //boundary check
	}
	mMaxLanguageCount = langCount;
	std::set<std::string>::const_iterator iter = langlist.begin();
	for (int cnt = 0; cnt < langCount; cnt++, iter++)
	{
		strncpy(mLanguageList[cnt], iter->c_str(), MAX_LANGUAGE_TAG_LENGTH);
		mLanguageList[cnt][MAX_LANGUAGE_TAG_LENGTH-1] = 0;
#ifdef SESSION_STATS
		if( this->mVideoEnd )
		{
			mVideoEnd->Setlanguage(VideoStatTrackType::STAT_AUDIO, (*iter), cnt+1);
		}
#endif
	}
}


/**
 * @brief Check if audio language is supported
 * @param checkLanguage language string to be checked
 * @retval true if supported, false if not supported
 */
bool PrivateInstanceAAMP::IsAudioLanguageSupported (const char *checkLanguage)
{
	bool retVal =false;
	for (int cnt=0; cnt < mMaxLanguageCount; cnt ++)
	{
		if(strncmp(mLanguageList[cnt], checkLanguage, MAX_LANGUAGE_TAG_LENGTH) == 0)
		{
			retVal = true;
			break;
		}
	}

	if(mMaxLanguageCount == 0)
	{
                logprintf("IsAudioLanguageSupported No Audio language stored !!!");
	}
	else if(!retVal)
	{
		logprintf("IsAudioLanguageSupported lang[%s] not available in list",checkLanguage);
	}
	return retVal;
}

/**
 * @brief Set curl timeout (CURLOPT_TIMEOUT)
 * @param timeout maximum time  in seconds curl request is allowed to take
 * @param instance index of instance to which timeout to be set
 */
void PrivateInstanceAAMP::SetCurlTimeout(long timeoutMS, AampCurlInstance instance)
{
	if(ContentType_EAS == mContentType)
		return;
	if(instance < eCURLINSTANCE_MAX && curl[instance])
	{
		curl_easy_setopt(curl[instance], CURLOPT_TIMEOUT_MS, timeoutMS);
		curlDLTimeout[instance] = timeoutMS;
	}
	else
	{
		logprintf("Failed to update timeout for curl instace %d",instance);
	}
}

/**
 * @brief Terminate curl instances
 * @param startIdx start index
 * @param instanceCount count of instances
 */
void PrivateInstanceAAMP::CurlTerm(AampCurlInstance startIdx, unsigned int instanceCount)
{
	int instanceEnd = startIdx + instanceCount;
	assert (instanceEnd <= eCURLINSTANCE_MAX);
	for (unsigned int i = startIdx; i < instanceEnd; i++)
	{
		if (curl[i])
		{
			curl_easy_cleanup(curl[i]);
			curl[i] = NULL;
			curlDLTimeout[i] = 0;
		}
	}
}

/**
 * @brief GetPlaylistCurlInstance - Function to return the curl instance for playlist download
 * Considers parallel download to decide the curl instance 
 * @param MediaType - Playlist type 
 * @param Init/Refresh - When playlist download is done 
 * @retval AampCurlInstance - Curl instance for playlist download
 */
AampCurlInstance PrivateInstanceAAMP::GetPlaylistCurlInstance(MediaType type, bool isInitialDownload)
{
	AampCurlInstance retType = eCURLINSTANCE_MANIFEST_PLAYLIST;
	bool indivCurlInstanceFlag = false;

	//DELIA-41646
	// logic behind this function :
	// a. This function gets called during Init and during Refresh of playlist .So need to decide who called
	// b. Based on the decision flag is considerd . mParallelFetchPlaylist for Init and mParallelFetchPlaylistRefresh
	//	  for refresh
	// c. If respective configuration is enabled , then associate separate curl for each track type
	// d. If parallel fetch is disabled , then single curl instance is used to fetch all playlist(eCURLINSTANCE_MANIFEST_PLAYLIST)

	indivCurlInstanceFlag = isInitialDownload ? mParallelFetchPlaylist : mParallelFetchPlaylistRefresh;
	if(indivCurlInstanceFlag)
	{
		switch(type)
		{
			case eMEDIATYPE_PLAYLIST_VIDEO:
				retType = eCURLINSTANCE_VIDEO;
				break;
			case eMEDIATYPE_PLAYLIST_AUDIO:
				retType = eCURLINSTANCE_AUDIO;
				break;
			case eMEDIATYPE_PLAYLIST_SUBTITLE:
				retType = eCURLINSTANCE_SUBTITLE;
				break;
			default:
				break;
		}
	}
	return retType;
}

/**
 * @brief called when tuning - reset artificially
 * low for quicker tune times
 * @param bitsPerSecond
 * @param trickPlay
 * @param profile
 */
void PrivateInstanceAAMP::ResetCurrentlyAvailableBandwidth(long bitsPerSecond , bool trickPlay,int profile)
{
	pthread_mutex_lock(&mLock);
	if (mAbrBitrateData.size())
	{
		mAbrBitrateData.erase(mAbrBitrateData.begin(),mAbrBitrateData.end());
	}
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief estimate currently available bandwidth, 
 * using most recently recorded 3 samples
 * @retval currently available bandwidth
 */
long PrivateInstanceAAMP::GetCurrentlyAvailableBandwidth(void)
{
	long avg = 0;
	long ret = -1;
	// 1. Check for any old bitrate beyond threshold time . remove those before calculation
	// 2. Sort and get median 
	// 3. if any outliers  , remove those entries based on a threshold value.
	// 4. Get the average of remaining data. 
	// 5. if no item in the list , return -1 . Caller to ignore bandwidth based processing
	
	std::vector< std::pair<long long,long> >::iterator bitrateIter;
	std::vector< long> tmpData;
	std::vector< long>::iterator tmpDataIter;
	long long presentTime = aamp_GetCurrentTimeMS();
	pthread_mutex_lock(&mLock);
	for (bitrateIter = mAbrBitrateData.begin(); bitrateIter != mAbrBitrateData.end();)
	{
		//logprintf("[%s][%d] Sz[%d] TimeCheck Pre[%lld] Sto[%lld] diff[%lld] bw[%ld] ",__FUNCTION__,__LINE__,mAbrBitrateData.size(),presentTime,(*bitrateIter).first,(presentTime - (*bitrateIter).first),(long)(*bitrateIter).second);
		if ((bitrateIter->first <= 0) || (presentTime - bitrateIter->first > gpGlobalConfig->abrCacheLife))
		{
			//logprintf("[%s][%d] Threadshold time reached , removing bitrate data ",__FUNCTION__,__LINE__);
			bitrateIter = mAbrBitrateData.erase(bitrateIter);
		}
		else
		{
			tmpData.push_back(bitrateIter->second);
			bitrateIter++;
		}
	}
	pthread_mutex_unlock(&mLock);

	if (tmpData.size())
	{	
		long medianbps=0;

		std::sort(tmpData.begin(),tmpData.end());
		if (tmpData.size() %2)
		{
			medianbps = tmpData.at(tmpData.size()/2);
		}
		else
		{
			long m1 = tmpData.at(tmpData.size()/2);
			long m2 = tmpData.at(tmpData.size()/2)+1;
			medianbps = (m1+m2)/2;
		} 
	
		long diffOutlier = 0;
		avg = 0;
		for (tmpDataIter = tmpData.begin();tmpDataIter != tmpData.end();)
		{
			diffOutlier = (*tmpDataIter) > medianbps ? (*tmpDataIter) - medianbps : medianbps - (*tmpDataIter);
			if (diffOutlier > gpGlobalConfig->abrOutlierDiffBytes)
			{
				//logprintf("[%s][%d] Outlier found[%ld]>[%ld] erasing ....",__FUNCTION__,__LINE__,diffOutlier,gpGlobalConfig->abrOutlierDiffBytes);
				tmpDataIter = tmpData.erase(tmpDataIter);
			}
			else
			{
				avg += (*tmpDataIter);
				tmpDataIter++;	
			}
		}
		if (tmpData.size())
		{
			//logprintf("[%s][%d] NwBW with newlogic size[%d] avg[%ld] ",__FUNCTION__,__LINE__,tmpData.size(), avg/tmpData.size());
			ret = (avg/tmpData.size());
			mAvailableBandwidth = ret;
		}	
		else
		{
			//logprintf("[%s][%d] No prior data available for abr , return -1 ",__FUNCTION__,__LINE__);
			ret = -1;
		}
	}
	else
	{
		//logprintf("[%s][%d] No data available for bitrate check , return -1 ",__FUNCTION__,__LINE__);
		ret = -1;
	}
	
	return ret;
}

/**
 * @brief Get MediaType as String
 */
const char* PrivateInstanceAAMP::MediaTypeString(MediaType fileType)
{
	switch(fileType)
	{
		case eMEDIATYPE_VIDEO:
		case eMEDIATYPE_INIT_VIDEO:
			return "VIDEO";
		case eMEDIATYPE_AUDIO:
		case eMEDIATYPE_INIT_AUDIO:
			return "AUDIO";
		case eMEDIATYPE_SUBTITLE:
		case eMEDIATYPE_INIT_SUBTITLE:
			return "SUBTITLE";
		case eMEDIATYPE_MANIFEST:
			return "MANIFEST";
		case eMEDIATYPE_LICENCE:
			return "LICENCE";
		case eMEDIATYPE_IFRAME:
			return "IFRAME";
		case eMEDIATYPE_PLAYLIST_VIDEO:
			return "PLAYLIST_VIDEO";
		case eMEDIATYPE_PLAYLIST_AUDIO:
			return "PLAYLIST_AUDIO";
		case eMEDIATYPE_PLAYLIST_SUBTITLE:
			return "PLAYLIST_SUBTITLE";
		default:
			return "Unknown";
	}
}

/**
 * @brief Fetch a file from CDN
 * @param remoteUrl url of the file
 * @param[out] buffer pointer to buffer abstraction
 * @param[out] effectiveUrl last effective URL
 * @param http_error error code in case of failure
 * @param range http range
 * @param curlInstance instance to be used to fetch
 * @param resetBuffer true to reset buffer before fetch
 * @param fileType media type of the file
 * @param fragmentDurationSeconds to know the current fragment length in case fragment fetch
 * @retval true if success
 */
bool PrivateInstanceAAMP::GetFile(std::string remoteUrl,struct GrowableBuffer *buffer, std::string& effectiveUrl, 
				long * http_error, double *downloadTime, const char *range, unsigned int curlInstance, 
				bool resetBuffer, MediaType fileType, long *bitrate, int * fogError,
				double fragmentDurationSeconds)
{
	MediaType simType = fileType; // remember the requested specific file type; fileType gets overridden later with simple VIDEO/AUDIO
	MediaTypeTelemetry mediaType = aamp_GetMediaTypeForTelemetry(fileType);
	long http_code = -1;
	double fileDownloadTime = 0;
	bool ret = false;
	int downloadAttempt = 0;
	int maxDownloadAttempt = 1;
	CURL* curl = this->curl[curlInstance];
	struct curl_slist* httpHeaders = NULL;
	CURLcode res = CURLE_OK;
	int fragmentDurationMs = (int)(fragmentDurationSeconds*1000);/*convert to MS */
	if (simType == eMEDIATYPE_INIT_VIDEO || simType == eMEDIATYPE_INIT_AUDIO)
	{
		maxDownloadAttempt += mInitFragmentRetryCount;
	}
	else
	{
		maxDownloadAttempt += DEFAULT_DOWNLOAD_RETRY_COUNT;
	}

	pthread_mutex_lock(&mLock);
	if (resetBuffer)
	{
		if(buffer->avail)
        	{
            		AAMPLOG_TRACE("%s:%d reset buffer %p avail %d", __FUNCTION__, __LINE__, buffer, (int)buffer->avail);
        	}	
		memset(buffer, 0x00, sizeof(*buffer));
	}
	if (mDownloadsEnabled)
	{
		int downloadTimeMS = 0;
		bool isDownloadStalled = false;
		CurlAbortReason abortReason = eCURL_ABORT_REASON_NONE;
		double connectTime = 0;
		pthread_mutex_unlock(&mLock);

		// append custom uri parameter with remoteUrl at the end before curl request if curlHeader logging enabled.
		if (gpGlobalConfig->logging.curlHeader && gpGlobalConfig->uriParameter && simType == eMEDIATYPE_MANIFEST )
		{
			if (remoteUrl.find("?") == std::string::npos)
			{
				gpGlobalConfig->uriParameter[0] = '?';
			}
			remoteUrl.append(gpGlobalConfig->uriParameter);
			//printf ("URL after appending uriParameter :: %s\n", remoteUrl.c_str());
		}

		AAMPLOG_INFO("aamp url:%d,%d,%d,%f,%s", mediaType, simType, curlInstance,fragmentDurationSeconds, remoteUrl.c_str());
		CurlCallbackContext context;
		if (curl)
		{
			curl_easy_setopt(curl, CURLOPT_URL, remoteUrl.c_str());

			context.aamp = this;
			context.buffer = buffer;
			context.responseHeaderData = &httpRespHeaders[curlInstance];
			context.fileType = simType;
			
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
			if(!gpGlobalConfig->sslVerifyPeer)
			{
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
			}
			else
			{
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
			}

			CurlProgressCbContext progressCtx;
			progressCtx.aamp = this;
			//Disable download stall detection checks for FOG playback done by JS PP
			if(simType == eMEDIATYPE_MANIFEST || simType == eMEDIATYPE_PLAYLIST_VIDEO || 
				simType == eMEDIATYPE_PLAYLIST_AUDIO || simType == eMEDIATYPE_PLAYLIST_SUBTITLE ||
				simType == eMEDIATYPE_PLAYLIST_IFRAME)
			{
				// For Manifest file : Set starttimeout to 0 ( no wait for first byte). Playlist/Manifest with DAI
				// contents take more time , hence to avoid frequent timeout, its set as 0
				progressCtx.startTimeout = 0;
			}
			else
			{
				// for Video/Audio segments , set the start timeout as configured by Application
				progressCtx.startTimeout = gpGlobalConfig->curlDownloadStartTimeout;
			}
			progressCtx.stallTimeout = gpGlobalConfig->curlStallTimeout;
                  
			// note: win32 curl lib doesn't support multi-part range
			curl_easy_setopt(curl, CURLOPT_RANGE, range);

			if ((httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_COOKIE) && (httpRespHeaders[curlInstance].data.length() > 0))
			{
				traceprintf("Appending cookie headers to HTTP request");
				//curl_easy_setopt(curl, CURLOPT_COOKIE, cookieHeaders[curlInstance].c_str());
				curl_easy_setopt(curl, CURLOPT_COOKIE, httpRespHeaders[curlInstance].data.c_str());
			}
			if (mCustomHeaders.size() > 0)
			{
				std::string customHeader;
				std::string headerValue;
				for (std::unordered_map<std::string, std::vector<std::string>>::iterator it = mCustomHeaders.begin();
									it != mCustomHeaders.end(); it++)
				{
					customHeader.clear();
					headerValue.clear();
					customHeader.insert(0, it->first);
					customHeader.push_back(' ');
					headerValue = it->second.at(0);
					if (it->first.compare("X-MoneyTrace:") == 0)
					{
						if (mTSBEnabled && !mIsFirstRequestToFOG)
						{
							continue;
						}
						char buf[512];
						memset(buf, '\0', 512);
						if (it->second.size() >= 2)
						{
							snprintf(buf, 512, "trace-id=%s;parent-id=%s;span-id=%lld",
									(const char*)it->second.at(0).c_str(),
									(const char*)it->second.at(1).c_str(),
									aamp_GetCurrentTimeMS());
						}
						else if (it->second.size() == 1)
						{
							snprintf(buf, 512, "trace-id=%s;parent-id=%lld;span-id=%lld",
									(const char*)it->second.at(0).c_str(),
									aamp_GetCurrentTimeMS(),
									aamp_GetCurrentTimeMS());
						}
						headerValue = buf;
					}
					if (it->first.compare("Wifi:") == 0)
					{
						if (true == activeInterfaceWifi)
						{
							headerValue = "1";
						}
						else
						{
							 headerValue = "0";
						}
					}
					customHeader.append(headerValue);
					httpHeaders = curl_slist_append(httpHeaders, customHeader.c_str());
				}

				if (gpGlobalConfig->logging.curlHeader && (eMEDIATYPE_VIDEO == simType || eMEDIATYPE_PLAYLIST_VIDEO == simType))
				{
					int size = gpGlobalConfig->customHeaderStr.size();
					for (int i=0; i < size; i++)
					{
						if (!gpGlobalConfig->customHeaderStr.at(i).empty())
						{
							//logprintf ("Custom Header Data: Index( %d ) Data( %s )", i, gpGlobalConfig->customHeaderStr.at(i).c_str());
							httpHeaders = curl_slist_append(httpHeaders, gpGlobalConfig->customHeaderStr.at(i).c_str());
						}
					}
				}

				if (httpHeaders != NULL)
				{
					curl_easy_setopt(curl, CURLOPT_HTTPHEADER, httpHeaders);
				}
			}

			while(downloadAttempt < maxDownloadAttempt)
			{
				progressCtx.downloadStartTime = NOW_STEADY_TS_MS;
				progressCtx.downloadUpdatedTime = -1;
				progressCtx.downloadSize = -1;
				progressCtx.abortReason = eCURL_ABORT_REASON_NONE;
				curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &progressCtx);
				if(buffer->ptr != NULL)
				{
					traceprintf("%s:%d reset length. buffer %p avail %d", __FUNCTION__, __LINE__, buffer, (int)buffer->avail);
					buffer->len = 0;
				}

				isDownloadStalled = false;
				abortReason = eCURL_ABORT_REASON_NONE;

				long long tStartTime = NOW_STEADY_TS_MS;
				CURLcode res = curl_easy_perform(curl); // synchronous; callbacks allow interruption

//				InterruptableMsSleep( 250 ); // this can be uncommented to locally induce extra per-download latency

				long long tEndTime = NOW_STEADY_TS_MS;
				downloadAttempt++;

				downloadTimeMS = (int)(tEndTime - tStartTime);
				bool loopAgain = false;
				if (res == CURLE_OK)
				{ // all data collected
					if( memcmp(remoteUrl.c_str(), "file:", 5) == 0 )
					{ // file uri scheme
						// libCurl does not provide CURLINFO_RESPONSE_CODE for 'file:' protocol.
						// Handle CURL_OK to http_code mapping here, other values handled below (see http_code = res).
						http_code = 200;
					}
					else
					{
						curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
					}
					char *effectiveUrlPtr = NULL;
					if (http_code != 200 && http_code != 204 && http_code != 206)
					{
						AAMP_LOG_NETWORK_ERROR (remoteUrl.c_str(), AAMPNetworkErrorHttp, (int)http_code, simType);
						print_headerResponse(context.allResponseHeadersForErrorLogging, simType);

						if((http_code >= 500 && http_code != 502) && downloadAttempt < maxDownloadAttempt)
						{
							InterruptableMsSleep(gpGlobalConfig->waitTimeBeforeRetryHttp5xxMS);
							logprintf("Download failed due to Server error. Retrying Attempt:%d!", downloadAttempt);
							loopAgain = true;
						}
					}
					if(http_code == 204)
					{
						if ( (httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_EFF_LOCATION) && (httpRespHeaders[curlInstance].data.length() > 0) )
						{
							logprintf("%s:%d Received Location header: '%s'",__FUNCTION__,__LINE__, httpRespHeaders[curlInstance].data.c_str());
							effectiveUrlPtr =  const_cast<char *>(httpRespHeaders[curlInstance].data.c_str());
						}
					}
					else
					{
						res = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrlPtr);
					}

					if ((http_code == 200) && (httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_FOG_ERROR) && (httpRespHeaders[curlInstance].data.length() > 0))
					{
						mFogErrorString.clear();
						mFogErrorString.assign(httpRespHeaders[curlInstance].data);
						logprintf("%s:%d Fog Error : '%s'",__FUNCTION__,__LINE__, mFogErrorString.c_str());
					}

					if(effectiveUrlPtr)
					{
						effectiveUrl.assign(effectiveUrlPtr);    //CID:81493 - Resolve Forward null
					}

					// check if redirected url is pointing to fog / local ip
					if(mIsFirstRequestToFOG)
					{
						if(mTsbRecordingId.empty())
						{
							AAMPLOG_INFO("TSB not avaialble from fog, playing from:%s ", effectiveUrl.c_str());
							mTSBEnabled = false;
						}
						// updating here because, tune request can be for fog but fog may redirect to cdn in some cases
						this->UpdateVideoEndTsbStatus(mTSBEnabled);
					}

					/*
					 * Latency should be printed in the case of successful download which exceeds the download threshold value,
					 * other than this case is assumed as network error and those will be logged with AAMP_LOG_NETWORK_ERROR.
					 */
					if (fragmentDurationSeconds != 0.0)
					{ 
						/*in case of fetch fragment this will be non zero value */
						if (downloadTimeMS > fragmentDurationMs )
						{
							AAMP_LOG_NETWORK_LATENCY (effectiveUrl.c_str(), downloadTimeMS, fragmentDurationMs, simType);
						}
					}
					else if (downloadTimeMS > FRAGMENT_DOWNLOAD_WARNING_THRESHOLD )
					{
						AAMP_LOG_NETWORK_LATENCY (effectiveUrl.c_str(), downloadTimeMS, FRAGMENT_DOWNLOAD_WARNING_THRESHOLD, simType);
						print_headerResponse(context.allResponseHeadersForErrorLogging, simType);
					}
				}
				else
				{
					long curlDownloadTimeoutMS = curlDLTimeout[curlInstance]; // curlDLTimeout is in msec
					//abortReason for progress_callback exit scenarios
					// curl sometimes exceeds the wait time by few milliseconds.Added buffer of 10msec
					isDownloadStalled = ((res == CURLE_OPERATION_TIMEDOUT || res == CURLE_PARTIAL_FILE ||
									(progressCtx.abortReason != eCURL_ABORT_REASON_NONE)) &&
									((downloadTimeMS-10) <= curlDownloadTimeoutMS));  //CID:100647 - No effect
					// set flag if download aborted with start/stall timeout.
					abortReason = progressCtx.abortReason;

					/* Curl 23 and 42 is not a real network error, so no need to log it here */
					//Log errors due to curl stall/start detection abort
					if (AAMP_IS_LOG_WORTHY_ERROR(res) || progressCtx.abortReason != eCURL_ABORT_REASON_NONE)
					{
						AAMP_LOG_NETWORK_ERROR (remoteUrl.c_str(), AAMPNetworkErrorCurl, (int)(progressCtx.abortReason == eCURL_ABORT_REASON_NONE ? res : CURLE_PARTIAL_FILE), simType);
						print_headerResponse(context.allResponseHeadersForErrorLogging, simType);
					}

					//Attempt retry for local playback since rampdown is disabled for FOG
					//Attempt retry for partial downloads, which have a higher chance to succeed
					if((res == CURLE_COULDNT_CONNECT || (res == CURLE_OPERATION_TIMEDOUT && mTSBEnabled) || isDownloadStalled) && downloadAttempt < maxDownloadAttempt)
					{
						if(mpStreamAbstractionAAMP)
						{
							if( simType == eMEDIATYPE_MANIFEST ||
								simType == eMEDIATYPE_AUDIO ||
							    simType == eMEDIATYPE_INIT_VIDEO ||
							    simType == eMEDIATYPE_PLAYLIST_AUDIO ||
							    simType == eMEDIATYPE_INIT_AUDIO )
							{ // always retry small, critical fragments on timeout
								loopAgain = true;
							}
							else
							{
								double buffervalue = mpStreamAbstractionAAMP->GetBufferedDuration();
								// buffer is -1 when sesssion not created . buffer is 0 when session created but playlist not downloaded
								if( buffervalue == -1.0 || buffervalue == 0 || buffervalue*1000 > (curlDownloadTimeoutMS + fragmentDurationMs))
								{
									// GetBuffer will return -1 if session is not created
									// Check if buffer is available and more than timeout interval then only reattempt
									// Not to retry download if there is no buffer left									
									loopAgain = true;
									if(simType == eMEDIATYPE_VIDEO)
									{
										if(buffer->len)
										{
											long downloadbps = ((long)(buffer->len / downloadTimeMS)*8000);
											long currentProfilebps	= mpStreamAbstractionAAMP->GetVideoBitrate();
											if(currentProfilebps - downloadbps >  BITRATE_ALLOWED_VARIATION_BAND)
												loopAgain = false;
										}
										curlDownloadTimeoutMS = mNetworkTimeoutMs;	
									}																		
								}
							}
						}						
						logprintf("Download failed due to curl timeout or isDownloadStalled:%d Retrying:%d Attempt:%d", isDownloadStalled, loopAgain, downloadAttempt);
					}

					/*
					* Assigning curl error to http_code, for sending the error code as
					* part of error event if required
					* We can distinguish curl error and http error based on value
					* curl errors are below 100 and http error starts from 100
					*/
					if( res == CURLE_FILE_COULDNT_READ_FILE )
					{
						http_code = 404; // translate file not found to URL not found
					}
					else
					{
						http_code = res;
					}
					#if 0
					if (isDownloadStalled)
					{
						AAMPLOG_INFO("Curl download stall detected - curl result:%d abortReason:%d downloadTimeMS:%lld curlTimeout:%ld", res, progressCtx.abortReason,
									downloadTimeMS, curlDownloadTimeoutMS);
						//To avoid updateBasedonFragmentCached being called on rampdown and to be discarded from ABR
						http_code = CURLE_PARTIAL_FILE;
					}
					#endif
				}

				if(gpGlobalConfig->enableMicroEvents && fileType != eMEDIATYPE_DEFAULT) //Unknown filetype
				{
					profiler.addtuneEvent(mediaType2Bucket(fileType),tStartTime,downloadTimeMS,(int)(http_code));
				}

				double total, connect, startTransfer, resolve, appConnect, preTransfer, redirect, dlSize;
				long reqSize;
				AAMP_LogLevel reqEndLogLevel = eLOGLEVEL_INFO;

				curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME , &total);
				curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
				connectTime = connect;
				fileDownloadTime = total;
				if(res != CURLE_OK || http_code == 0 || http_code >= 400 || total > 2.0 /*seconds*/)
				{
					reqEndLogLevel = eLOGLEVEL_WARN;
				}
				if (gpGlobalConfig->logging.isLogLevelAllowed(reqEndLogLevel))
				{
					double totalPerformRequest = (double)(downloadTimeMS)/1000;
					curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &resolve);
					curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appConnect);
					curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME, &preTransfer);
					curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &startTransfer);
					curl_easy_getinfo(curl, CURLINFO_REDIRECT_TIME, &redirect);
					curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &dlSize);
					curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE, &reqSize);

					std::string appName, timeoutClass;
					if (!mAppName.empty())
					{
						// append app name with class data
						appName = mAppName + ",";
					}
					if (CURLE_OPERATION_TIMEDOUT == res || CURLE_PARTIAL_FILE == res || CURLE_COULDNT_CONNECT == res)
					{
						// introduce  extra marker for connection status curl 7/18/28,
						// example 18(0) if connection failure with PARTIAL_FILE code
						timeoutClass = "(" + to_string(reqSize > 0) + ")";
					}
					AAMPLOG(reqEndLogLevel, "HttpRequestEnd: %s%d,%d,%ld%s,%2.4f,%2.4f,%2.4f,%2.4f,%2.4f,%2.4f,%2.4f,%2.4f,%g,%ld,%.500s",
						appName.c_str(), mediaType, simType, http_code, timeoutClass.c_str(), totalPerformRequest, total, connect, startTransfer, resolve, appConnect, preTransfer, redirect, dlSize, reqSize,
						((res == CURLE_OK) ? effectiveUrl.c_str() : remoteUrl.c_str())); // Effective URL could be different than remoteURL and it is updated only for CURLE_OK case
				}
				
				if(!loopAgain)
					break;
			}
		}

		if (http_code == 200 || http_code == 206 || http_code == CURLE_OPERATION_TIMEDOUT)
		{
			if (http_code == CURLE_OPERATION_TIMEDOUT && buffer->len > 0)
			{
				logprintf("Download timedout and obtained a partial buffer of size %d for a downloadTime=%d and isDownloadStalled:%d", buffer->len, downloadTimeMS, isDownloadStalled);
			}

			if (downloadTimeMS > 0 && fileType == eMEDIATYPE_VIDEO && CheckABREnabled())
			{
				if(buffer->len > gpGlobalConfig->aampAbrThresholdSize)
				{
					pthread_mutex_lock(&mLock);
					long downloadbps = ((long)(buffer->len / downloadTimeMS)*8000);
					long currentProfilebps  = mpStreamAbstractionAAMP->GetVideoBitrate();
					// extra coding to avoid picking lower profile

					if(downloadbps < currentProfilebps && fragmentDurationMs && downloadTimeMS < fragmentDurationMs/2)
					{
						downloadbps = currentProfilebps;
					}
					
					mAbrBitrateData.push_back(std::make_pair(aamp_GetCurrentTimeMS() ,downloadbps));
					//logprintf("CacheSz[%d]ConfigSz[%d] Storing Size [%d] bps[%ld]",mAbrBitrateData.size(),gpGlobalConfig->abrCacheLength, buffer->len, ((long)(buffer->len / downloadTimeMS)*8000));
					if(mAbrBitrateData.size() > gpGlobalConfig->abrCacheLength)
						mAbrBitrateData.erase(mAbrBitrateData.begin());
					pthread_mutex_unlock(&mLock);
				}
			}
		}
		if (http_code == 200 || http_code == 206)
		{
			if( gpGlobalConfig->harvestCountLimit > 0 )
			{
				logprintf("aamp harvestCountLimit: %d", gpGlobalConfig->harvestCountLimit);
				/* Avoid chance of overwriting , in case of manifest and playlist, name will be always same */
				if(fileType == eMEDIATYPE_MANIFEST || fileType == eMEDIATYPE_PLAYLIST_AUDIO 
				|| fileType == eMEDIATYPE_PLAYLIST_IFRAME || fileType == eMEDIATYPE_PLAYLIST_SUBTITLE || fileType == eMEDIATYPE_PLAYLIST_VIDEO )
				{
					mManifestRefreshCount++;
				}
				aamp_WriteFile(remoteUrl, buffer->ptr, buffer->len, fileType, mManifestRefreshCount);
			}
			double expectedContentLength = 0;
			if ((!context.downloadIsEncoded) && CURLE_OK==curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &expectedContentLength) && ((int)expectedContentLength>0) && ((int)expectedContentLength != (int)buffer->len))
			{
				//Note: For non-compressed data, Content-Length header and buffer size should be same. For gzipped data, 'Content-Length' will be <= deflated data.
				AAMPLOG_WARN("AAMP Content-Length=%d actual=%d", (int)expectedContentLength, (int)buffer->len);
				http_code       =       416; // Range Not Satisfiable
				ret             =       false; // redundant, but harmless
				if (buffer->ptr)
				{
					aamp_Free(&buffer->ptr);
				}
				memset(buffer, 0x00, sizeof(*buffer));
			}
			else
			{
				if(fileType == eMEDIATYPE_MANIFEST)
				{
					fileType = (MediaType)curlInstance;
				}
				else if (remoteUrl.find("iframe") != std::string::npos)
				{
					fileType = eMEDIATYPE_IFRAME;
				}
				ret = true;
			}
		}
		else
		{
			if (AAMP_IS_LOG_WORTHY_ERROR(res))
			{
				logprintf("BAD URL:%s", remoteUrl.c_str());
			}
			if (buffer->ptr)
			{
				aamp_Free(&buffer->ptr);
			}
			memset(buffer, 0x00, sizeof(*buffer));

			if (rate != 1.0)
			{
				fileType = eMEDIATYPE_IFRAME;
			}

			// dont generate anomaly reports for write and aborted errors
			// these are generated after trick play options,
			if( !(http_code == CURLE_ABORTED_BY_CALLBACK || http_code == CURLE_WRITE_ERROR || http_code == 204))
			{
				SendAnomalyEvent(ANOMALY_WARNING, "%s:%s,%s-%d url:%s", (mTSBEnabled ? "FOG" : "CDN"),
					MediaTypeString(fileType), (http_code < 100) ? "Curl" : "HTTP", http_code, remoteUrl.c_str());
			}
            
			if ( (httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_XREASON) && (httpRespHeaders[curlInstance].data.length() > 0) )
			{
				logprintf("Received X-Reason header from %s: '%s'", mTSBEnabled?"Fog":"CDN Server", httpRespHeaders[curlInstance].data.c_str());
				SendAnomalyEvent(ANOMALY_WARNING, "%s X-Reason:%s", mTSBEnabled ? "Fog" : "CDN", httpRespHeaders[curlInstance].data.c_str());
			}
			else if ( (httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_FOG_REASON) && (httpRespHeaders[curlInstance].data.length() > 0) )
			{
				//extract error and url used by fog to download content from cdn
				// it is part of fog-reason
				if(fogError)
				{
					std::regex errRegx("-(.*),");
					std::smatch match;
					if (std::regex_search(httpRespHeaders[curlInstance].data, match, errRegx) && match.size() > 1) {
						if (!match.str(1).empty())
						{
							*fogError = std::stoi(match.str(1));
							AAMPLOG_INFO("Received FOG-Reason fogError: '%d'", *fogError);
						}
					}
				}

				//	get failed url from fog reason and update effectiveUrl
				if(!effectiveUrl.empty())
				{
					std::regex fromRegx("from:(.*),");
					std::smatch match;

					if (std::regex_search(httpRespHeaders[curlInstance].data, match, fromRegx) && match.size() > 1) {
						if (!match.str(1).empty())
						{
							effectiveUrl.assign(match.str(1).c_str());
							AAMPLOG_INFO("Received FOG-Reason effectiveUrl: '%s'", effectiveUrl.c_str());
						}
					}
				}


                logprintf("Received FOG-Reason header: '%s'", httpRespHeaders[curlInstance].data.c_str());
                SendAnomalyEvent(ANOMALY_WARNING, "FOG-Reason:%s", httpRespHeaders[curlInstance].data.c_str());
            }
		}

		if (bitrate && (context.bitrate > 0))
		{
			AAMPLOG_INFO("Received getfile Bitrate : %ld", context.bitrate);
			*bitrate = context.bitrate;
		}

		if(simType == eMEDIATYPE_PLAYLIST_VIDEO || simType == eMEDIATYPE_PLAYLIST_AUDIO || simType == eMEDIATYPE_INIT_AUDIO || simType == eMEDIATYPE_INIT_VIDEO )
		{
			// send customized error code for curl 28 and 18 playlist/init fragment download failures
			if (connectTime == 0.0)
			{
				//curl connection is failure
				if(CURLE_PARTIAL_FILE == http_code)
				{
					http_code = PARTIAL_FILE_CONNECTIVITY_AAMP;
				}
				else if(CURLE_OPERATION_TIMEDOUT == http_code)
				{
					http_code = OPERATION_TIMEOUT_CONNECTIVITY_AAMP;
				}
			}
			else if(abortReason != eCURL_ABORT_REASON_NONE)
			{
				http_code = PARTIAL_FILE_START_STALL_TIMEOUT_AAMP;
			}
			else if (CURLE_PARTIAL_FILE == http_code)
			{
				// download time expired with partial file for playlists/init fragments
				http_code = PARTIAL_FILE_DOWNLOAD_TIME_EXPIRED_AAMP;
			}
		}
		pthread_mutex_lock(&mLock);
	}
	else
	{
		logprintf("downloads disabled");
	}
	pthread_mutex_unlock(&mLock);
	if (http_error)
	{
		*http_error = http_code;
		if(downloadTime)
		{
			*downloadTime = fileDownloadTime;
		}
	}
	if (httpHeaders != NULL)
	{
		curl_slist_free_all(httpHeaders);
	}
	if (mIsFirstRequestToFOG)
	{
		mIsFirstRequestToFOG = false;
	}

    if (gpGlobalConfig->useLinearSimulator)
	{
		// NEW! note that for simulated playlists, ideally we'd not bother re-downloading playlist above

		AAMPLOG_INFO("*** Simulated Linear URL: %s\n", remoteUrl.c_str()); // Log incoming request

		switch( simType )
		{
			case eMEDIATYPE_MANIFEST:
			{
				// Reset state after requesting main manifest
				if( full_playlist_video_ptr )
				{
					// Flush old cached video playlist

					free( full_playlist_video_ptr );
					full_playlist_video_ptr = NULL;
					full_playlist_video_len = 0;
				}

				if( full_playlist_audio_ptr )
				{
					// Flush old cached audio playlist

					free( full_playlist_audio_ptr );
					full_playlist_audio_ptr = NULL;
					full_playlist_audio_len = 0;
				}

				simulation_start = aamp_GetCurrentTimeMS();
			}
				break; /* eMEDIATYPE_MANIFEST */

			case eMEDIATYPE_PLAYLIST_AUDIO:
			{
				if( !full_playlist_audio_ptr )
				{
					// Cache the full vod audio playlist

					full_playlist_audio_len = buffer->len;
					full_playlist_audio_ptr = (char *)malloc(buffer->len);
					memcpy( full_playlist_audio_ptr, buffer->ptr, buffer->len );
				}

				SimulateLinearWindow( buffer, full_playlist_audio_ptr, full_playlist_audio_len );
			}
				break; /* eMEDIATYPE_PLAYLIST_AUDIO */

			case eMEDIATYPE_PLAYLIST_VIDEO:
			{
				if( !full_playlist_video_ptr )
				{
					// Cache the full vod video playlist

					full_playlist_video_len = buffer->len;
					full_playlist_video_ptr = (char *)malloc(buffer->len);
					memcpy( full_playlist_video_ptr, buffer->ptr, buffer->len );
				}

				SimulateLinearWindow( buffer, full_playlist_video_ptr, full_playlist_video_len );
			}
				break; /* eMEDIATYPE_PLAYLIST_VIDEO */

			default:
				break;
		}
	}

	return ret;
}

/**
 * @brief Download VideoEnd Session statistics from fog
 *
 * @param[out] buffer - Pointer to the output buffer
 * @return pointer to tsbSessionEnd data from fog
 */
char * PrivateInstanceAAMP::GetOnVideoEndSessionStatData()
{
	std::string remoteUrl = "127.0.0.1:9080/sessionstat";
	long http_error = -1;
	char* ret = NULL;
	if(!mTsbRecordingId.empty())
	{
		/* Request session statistics for current recording ID
		 *
		 * example request: 127.0.0.1:9080/sessionstat/<recordingID>
		 *
		 */
		remoteUrl.append("/");
		remoteUrl.append(mTsbRecordingId);
		GrowableBuffer data;
		if(ProcessCustomCurlRequest(remoteUrl, &data, &http_error))
		{
			// succesfully requested
			AAMPLOG_INFO("%s:%d curl request %s success", __FUNCTION__, __LINE__, remoteUrl.c_str());
			cJSON *root = cJSON_Parse(data.ptr);
			if (root == NULL)
			{
				const char *error_ptr = cJSON_GetErrorPtr();
				if (error_ptr != NULL)
				{
					AAMPLOG_ERR("%s:%d Invalid Json format: %s\n", __FUNCTION__, __LINE__, error_ptr);
				}
			}
			else
			{
				ret = cJSON_PrintUnformatted(root);
				cJSON_Delete(root);
			}

		}
		else
		{
			// Failure in request
			AAMPLOG_ERR("%s:%d curl request %s failed[%d]", __FUNCTION__, __LINE__, remoteUrl.c_str(), http_error);
		}

		if(data.ptr)
		{
			aamp_Free(&data.ptr);
		}
	}

	return ret;
}


/**
 * @brief Perform custom curl request
 *
 * @param[in] remoteUrl - File URL
 * @param[out] buffer - Pointer to the output buffer
 * @param[out] http_error - HTTP error code
 * @param[in] CurlRequest - request type
 * @return bool status
 */
bool PrivateInstanceAAMP::ProcessCustomCurlRequest(std::string& remoteUrl, GrowableBuffer* buffer, long *http_error, CurlRequest request)
{
	bool ret = false;
	CURLcode res;
	long httpCode = -1;
	CURL *curl = curl_easy_init();
	if(curl)
	{
		AAMPLOG_INFO("%s: %s, %d", __FUNCTION__, remoteUrl.c_str(), request);
		if(eCURL_GET == request)
		{
			CurlCallbackContext context(this, buffer);
			memset(buffer, 0x00, sizeof(*buffer));
			CurlProgressCbContext progressCtx(this, NOW_STEADY_TS_MS);

			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
			curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
			curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &progressCtx);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
		}
		else if(eCURL_DELETE == request)
		{
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
		}
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, DEFAULT_CURL_TIMEOUT);
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		if(!gpGlobalConfig->sslVerifyPeer){
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		}
		curl_easy_setopt(curl, CURLOPT_URL, remoteUrl.c_str());

		res = curl_easy_perform(curl);
		if (res == CURLE_OK)
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
			if ((httpCode == 204) || (httpCode == 200))
			{
				ret = true;
			}
			else
			{
				AAMPLOG_ERR("%s:%d Returned [%d]", __FUNCTION__, __LINE__, httpCode);
			}
		}
		else
		{
			AAMPLOG_ERR("%s::%d Failed to perform curl request, result:%d", __FUNCTION__, __LINE__, res);
		}

		if(httpCode > 0)
		{
			*http_error = httpCode;
		}

		curl_easy_cleanup(curl);
	}
	return ret;
}


/**
 * @brief Load AAMP configuration file
 */
void PrivateInstanceAAMP::LazilyLoadConfigIfNeeded(void)
{
	std::string cfgPath = "";
	if (!gpGlobalConfig)
	{
#ifdef AAMP_BUILD_INFO
		std::string tmpstr = MACRO_TO_STRING(AAMP_BUILD_INFO);
		logprintf(" AAMP_BUILD_INFO: %s",tmpstr.c_str());
#endif
		gpGlobalConfig = new GlobalConfigAAMP();
#ifdef IARM_MGR 
		logprintf("LazilyLoadConfigIfNeeded calling  GetTR181AAMPConfig  ");
		size_t iConfigLen = 0;
		char * cloudConf = GetTR181AAMPConfig("Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.Feature.AAMP_CFG.b64Config", iConfigLen);
		if(cloudConf && (iConfigLen > 0))
		{
			bool bCharCompliant = true;
			for (int i = 0; i < iConfigLen; i++)
			{
				if (!( cloudConf[i] == 0xD || cloudConf[i] == 0xA) && // ignore LF and CR chars
					((cloudConf[i] < 0x20) || (cloudConf[i] > 0x7E)))
				{
					bCharCompliant = false;
					logprintf("LazilyLoadConfigIfNeeded Non Compliant char[0x%X] found, Ignoring whole config  ",cloudConf[i]);
					break;
				}
			}

			if (bCharCompliant)
			{
				std::string strCfg(cloudConf,iConfigLen);
				std::istringstream iSteam(strCfg);
				std::string line;
				while (std::getline(iSteam, line)) {
				if (line.length() > 0)
				{
					//ProcessConfigEntry takes char * and line.c_str() returns const string hence copy of line is created
					logprintf("LazilyLoadConfigIfNeeded aamp-cmd:[%s]", line.c_str());
					ProcessConfigEntry(line);
				}
			}
		}
		free(cloudConf); // allocated by base64_Decode in GetTR181AAMPConfig
	}
#endif

#ifdef WIN32
		AampLogManager mLogManager;
		cfgPath.assign(mLogManager.getAampCfgPath());
#elif defined(__APPLE__)
		std::string cfgPath(getenv("HOME"));
		cfgPath += "/aamp.cfg";
#else

#ifdef AAMP_CPC
        // AAMP_ENABLE_OPT_OVERRIDE is only added for PROD builds.
        const char *env_aamp_enable_opt = getenv("AAMP_ENABLE_OPT_OVERRIDE");
#else
        const char *env_aamp_enable_opt = "true";
#endif

        if(env_aamp_enable_opt)
        {
            cfgPath = "/opt/aamp.cfg";
        }
#endif
		if (!cfgPath.empty())
		{
			std::ifstream f(cfgPath, std::ifstream::in | std::ifstream::binary);
			if (f.good())
			{
				logprintf("opened aamp.cfg");
				std::string buf;
				while (f.good())
				{
					std::getline(f, buf);
					ProcessConfigEntry(buf);
				}
				f.close();
			}
		}

		const char *env_aamp_force_aac = getenv("AAMP_FORCE_AAC");
		if(env_aamp_force_aac)
		{
			logprintf("AAMP_FORCE_AAC present: Changing preference to AAC over ATMOS & DD+");
			gpGlobalConfig->disableEC3 = eTrueState;
			gpGlobalConfig->disableATMOS = eTrueState;
		}

		const char *env_aamp_force_info = getenv("AAMP_FORCE_INFO");
		if(env_aamp_force_info)
		{
			logprintf("AAMP_FORCE_INFO present: Changing debug levels");
			gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_INFO);
			gpGlobalConfig->logging.debug = false;
			gpGlobalConfig->logging.info = true;
		}

		const char *env_aamp_force_debug = getenv("AAMP_FORCE_DEBUG");
		if(env_aamp_force_debug)
		{
			logprintf("AAMP_FORCE_DEBUG present: Changing debug levels");
			gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_TRACE);
			gpGlobalConfig->logging.debug = true;
			gpGlobalConfig->logging.info = false;
		}

		const char *env_aamp_min_init_cache = getenv("AAMP_MIN_INIT_CACHE");
		if(env_aamp_min_init_cache
				&& gpGlobalConfig->minInitialCacheSeconds == MINIMUM_INIT_CACHE_NOT_OVERRIDDEN)
		{
			int minInitCache = 0;
			if(sscanf(env_aamp_min_init_cache,"%d",&minInitCache)
					&& minInitCache >= 0)
			{
				logprintf("AAMP_MIN_INIT_CACHE present: Changing min initial cache to %d seconds",minInitCache);
				mMinInitialCacheSeconds = minInitCache;
			}
		}

		const char *env_enable_micro_events = getenv("TUNE_MICRO_EVENTS");
		if(env_enable_micro_events)
		{
			logprintf("TUNE_MICRO_EVENTS present: Enabling TUNE_MICRO_EVENTS.");
			gpGlobalConfig->enableMicroEvents = true;
		}

		const char *env_enable_cdai = getenv("CLIENT_SIDE_DAI");
		if(env_enable_cdai)
		{
			logprintf("CLIENT_SIDE_DAI present: Enabling CLIENT_SIDE_DAI.");
			gpGlobalConfig->enableClientDai = true;
		}

		const char *env_enable_westoros_sink = getenv("AAMP_ENABLE_WESTEROS_SINK");

		if(env_enable_westoros_sink)
		{
			int iValue = atoi(env_enable_westoros_sink);
			bool bValue = (strcasecmp(env_enable_westoros_sink,"true") == 0);

			logprintf("AAMP_ENABLE_WESTEROS_SINK present, Value = %d", (bValue ? bValue : (iValue ? iValue : 0)));

			if(iValue || bValue)
			{
				mWesterosSinkEnabled = true;
			}
		}

		if(gpGlobalConfig->minInitialCacheSeconds != MINIMUM_INIT_CACHE_NOT_OVERRIDDEN)
		{
			mMinInitialCacheSeconds = gpGlobalConfig->minInitialCacheSeconds;
		}
	}
}

/**
 * @brief Executes tear down sequence
 * @param newTune true if operation is a new tune
 */
void PrivateInstanceAAMP::TeardownStream(bool newTune)
{
	pthread_mutex_lock(&mLock);
	//Have to perfom this for trick and stop operations but avoid ad insertion related ones
	AAMPLOG_WARN("%s:%d mProgressReportFromProcessDiscontinuity:%d mDiscontinuityTuneOperationId:%d newTune:%d", __FUNCTION__, __LINE__, mProgressReportFromProcessDiscontinuity, mDiscontinuityTuneOperationId, newTune);
	if ((mDiscontinuityTuneOperationId != 0) && (!newTune || mState == eSTATE_IDLE))
	{
		bool waitForDiscontinuityProcessing = true;
		if (mProgressReportFromProcessDiscontinuity)
		{
			AAMPLOG_WARN("%s:%d TeardownStream invoked while mProgressReportFromProcessDiscontinuity and mDiscontinuityTuneOperationId[%d] set!", __FUNCTION__, __LINE__, mDiscontinuityTuneOperationId);
			guint callbackID = aamp_GetSourceID();
			if (callbackID != 0 && mDiscontinuityTuneOperationId == callbackID)
			{
				AAMPLOG_WARN("%s:%d TeardownStream idle callback id[%d] and mDiscontinuityTuneOperationId[%d] match. Ignore further discontinuity processing!", __FUNCTION__, __LINE__, callbackID, mDiscontinuityTuneOperationId);
				waitForDiscontinuityProcessing = false; // to avoid deadlock
				mDiscontinuityTuneOperationInProgress = false;
				mDiscontinuityTuneOperationId = 0;
			}
		}
		if (waitForDiscontinuityProcessing)
		{
			//wait for discont tune operation to finish before proceeding with stop
			if (mDiscontinuityTuneOperationInProgress)
			{
				pthread_cond_wait(&mCondDiscontinuity, &mLock);
			}
			else
			{
				g_source_remove(mDiscontinuityTuneOperationId);
				mDiscontinuityTuneOperationId = 0;
			}
		}
	}
	// Maybe mDiscontinuityTuneOperationId is 0, ProcessPendingDiscontinuity can be invoked from NotifyEOSReached too
	else if (mProgressReportFromProcessDiscontinuity)
	{
		AAMPLOG_WARN("%s:%d TeardownStream invoked while mProgressReportFromProcessDiscontinuity set!", __FUNCTION__, __LINE__);
		mDiscontinuityTuneOperationInProgress = false;
	}

	//reset discontinuity related flags
	mProcessingDiscontinuity[eMEDIATYPE_VIDEO] = false;
	mProcessingDiscontinuity[eMEDIATYPE_AUDIO] = false;
	ResetTrackDiscontinuityIgnoredStatus();
	pthread_mutex_unlock(&mLock);

	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->Stop(false);
		delete mpStreamAbstractionAAMP;
		mpStreamAbstractionAAMP = NULL;
	}

	pthread_mutex_lock(&mLock);
	mVideoFormat = FORMAT_INVALID;
	pthread_mutex_unlock(&mLock);
	if (streamerIsActive)
	{
#ifdef AAMP_STOP_SINK_ON_SEEK
		const bool forceStop = true;
		// Don't send event if nativeCCRendering is ON
		if (!gpGlobalConfig->nativeCCRendering)
		{
			CCHandleEventPtr event = std::make_shared<CCHandleEvent>(0);
			SendEventSync(event);
			logprintf("%s:%d Sent AAMP_EVENT_CC_HANDLE_RECEIVED with NULL handle", __FUNCTION__, __LINE__);
		}
#else
		const bool forceStop = false;
#endif
		if (!forceStop && ((!newTune && gpGlobalConfig->gAampDemuxHLSVideoTsTrack) || gpGlobalConfig->gPreservePipeline))
		{
			mStreamSink->Flush(0, rate);
		}
		else
		{
#ifdef AAMP_CC_ENABLED
			// Stop CC when pipeline is stopped/destroyed and if foreground instance
			if (gpGlobalConfig->nativeCCRendering && mbPlayEnabled)
			{
				AampCCManager::GetInstance()->Release();
			}
#endif
			mStreamSink->Stop(!newTune);
		}
	}
	else
	{
		for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
		{
			mbTrackDownloadsBlocked[iTrack] = true;
		}
		streamerIsActive = true;
	}
	mAdProgressId = "";
	std::queue<AAMPEventPtr> emptyEvQ;
	{
		std::lock_guard<std::mutex> lock(mAdEventQMtx);
		std::swap( mAdEventsQ, emptyEvQ );
	}
}

/**
 * @brief Setup pipe session with application
 */
bool PrivateInstanceAAMP::SetupPipeSession()
{
	bool retVal = false;
	if(m_fd != -1)
	{
		retVal = true; //Pipe exists
		return retVal;
	}
	if(mkfifo(strAAMPPipeName, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == -1)
	{
		if(errno == EEXIST)
		{
			// Pipe exists
			//logprintf("%s:CreatePipe: Pipe already exists",__FUNCTION__);
			retVal = true;
		}
		else
		{
			// Error
			logprintf("%s:CreatePipe: Failed to create named pipe %s for reading errno = %d (%s)",
				__FUNCTION__,strAAMPPipeName, errno, strerror(errno));
		}
	}
	else
	{
		// Success
		//logprintf("%s:CreatePipe: mkfifo succeeded",__FUNCTION__);
		retVal = true;
	}

	if(retVal)
	{
		// Open the named pipe for writing
		m_fd = open(strAAMPPipeName, O_WRONLY | O_NONBLOCK  );
		if (m_fd == -1)
		{
			// error
			logprintf("%s:OpenPipe: Failed to open named pipe %s for writing errno = %d (%s)",
				__FUNCTION__,strAAMPPipeName, errno, strerror(errno));
		}
		else
		{
			// Success
			retVal = true;
		}
	}
	return retVal;
}


/**
 * @brief Close pipe session with application
 */
void PrivateInstanceAAMP::ClosePipeSession()
{
	if(m_fd != -1)
	{
		close(m_fd);
		m_fd = -1;
	}
}
                            

/**
 * @brief Send message to application using pipe session
 * @param str message
 * @param nToWrite message size
 */
void PrivateInstanceAAMP::SendMessageOverPipe(const char *str,int nToWrite)
{
	if(m_fd != -1)
	{
		// Write the packet data to the pipe
		int nWritten =  write(m_fd, str, nToWrite);
		if(nWritten != nToWrite)
		{
			// Error
			logprintf("Error writing data written = %d, size = %d errno = %d (%s)",
				nWritten, nToWrite, errno, strerror(errno));
			if(errno == EPIPE)
			{
				// broken pipe, lets reset and open again when the pipe is avail
				ClosePipeSession();
			}
		}
	}
}

void PrivateInstanceAAMP::SendMessage2Receiver(AAMP2ReceiverMsgType type, const char *data)
{
#ifdef CREATE_PIPE_SESSION_TO_XRE
	if(SetupPipeSession())
	{
		int dataLen = strlen(data);
		int sizeToSend = AAMP2ReceiverMsgHdrSz + dataLen;
		std::vector<uint8_t> tmp(sizeToSend,0);
		AAMP2ReceiverMsg *msg = (AAMP2ReceiverMsg *)(tmp.data());
		msg->type = (unsigned int)type;
		msg->length = dataLen;
		memcpy(msg->data, data, dataLen);
		SendMessageOverPipe((char *)tmp.data(), sizeToSend);
	}
#else
	AAMPLOG_INFO("AAMP=>XRE: %s",data);
#endif
}

/**
 * @brief Common tune operations used on Tune, Seek, SetRate etc
 * @param tuneType type of tune
 */
void PrivateInstanceAAMP::TuneHelper(TuneType tuneType, bool seekWhilePaused)
{
	bool newTune;
	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		lastUnderFlowTimeMs[i] = 0;
	}
	LazilyLoadConfigIfNeeded();
	mFragmentCachingRequired = false;
	mPauseOnFirstVideoFrameDisp = false;
	mFirstVideoFrameDisplayedEnabled = false;

	if (tuneType == eTUNETYPE_SEEK || tuneType == eTUNETYPE_SEEKTOLIVE)
	{
		mSeekOperationInProgress = true;
	}

	if (eTUNETYPE_LAST == tuneType)
	{
		tuneType = mTuneType;
	}
	else
	{
		mTuneType = tuneType;
	}

	newTune = IsNewTune();

	// DELIA-39530 - Get position before pipeline is teared down
	if (eTUNETYPE_RETUNE == tuneType)
	{
		seek_pos_seconds = GetPositionMilliseconds()/1000;
	}

	TeardownStream(newTune|| (eTUNETYPE_RETUNE == tuneType));

	if (newTune)
	{

		// send previouse tune VideoEnd Metrics data
		// this is done here because events are cleared on stop and there is chance that event may not get sent
		// check for mEnableVideoEndEvent and call SendVideoEndEvent ,object mVideoEnd is created inside SendVideoEndEvent
		if(gpGlobalConfig->mEnableVideoEndEvent 
			&& (mTuneAttempts == 1)) // only for first attempt, dont send event when JSPP retunes. 
		{
			SendVideoEndEvent();
		}

		mTsbRecordingId.clear();
		// initialize defaults
		SetState(eSTATE_INITIALIZING);
		culledSeconds = 0;
		durationSeconds = 60 * 60; // 1 hour
		rate = AAMP_NORMAL_PLAY_RATE;
		playStartUTCMS = aamp_GetCurrentTimeMS();
		StoreLanguageList(std::set<std::string>());
		mTunedEventPending = true;
	}

	trickStartUTCMS = -1;

	double playlistSeekPos = seek_pos_seconds - culledSeconds;
	AAMPLOG_INFO("%s:%d playlistSeek : %f seek_pos_seconds:%f culledSeconds : %f ",__FUNCTION__,__LINE__,playlistSeekPos,seek_pos_seconds,culledSeconds);
	if (playlistSeekPos < 0)
	{
		playlistSeekPos = 0;
		seek_pos_seconds = culledSeconds;
		logprintf("%s:%d Updated seek_pos_seconds %f ",__FUNCTION__,__LINE__, seek_pos_seconds);
	}
	
	if (mMediaFormat == eMEDIAFORMAT_DASH)
	{
		#if defined (INTELCE)
		logprintf("Error: Dash playback not available\n");
		mInitSuccess = false;
		SendErrorEvent(AAMP_TUNE_UNSUPPORTED_STREAM_TYPE);
		return;
		#else
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_MPD(this, playlistSeekPos, rate);
		if (NULL == mCdaiObject)
		{
			mCdaiObject = new CDAIObjectMPD(this); // special version for DASH
		}
		#endif
	}
	else if (mMediaFormat == eMEDIAFORMAT_HLS || mMediaFormat == eMEDIAFORMAT_HLS_MP4)
	{ // m3u8
        	bool enableThrottle = true;
		if (!gpGlobalConfig->gThrottle)
        	{
			enableThrottle = false;
		}
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_HLS(this, playlistSeekPos, rate, enableThrottle);
		if(NULL == mCdaiObject)
		{
			mCdaiObject = new CDAIObject(this);    //Placeholder to reject the SetAlternateContents()
		}
	}
	else if (mMediaFormat == eMEDIAFORMAT_PROGRESSIVE)
	{
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_PROGRESSIVE(this, playlistSeekPos, rate);
		if (NULL == mCdaiObject)
		{
			mCdaiObject = new CDAIObject(this);    //Placeholder to reject the SetAlternateContents()
		}
	}
	else if (mMediaFormat == eMEDIAFORMAT_HDMI)
	{
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_HDMIIN(this, playlistSeekPos, rate);
		if (NULL == mCdaiObject)
		{
			mCdaiObject = new CDAIObject(this);    //Placeholder to reject the SetAlternateContents()
		}
	}
	else if (mMediaFormat == eMEDIAFORMAT_OTA)
	{
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_OTA(this, playlistSeekPos, rate);
		if (NULL == mCdaiObject)
		{
			mCdaiObject = new CDAIObject(this);    //Placeholder to reject the SetAlternateContents()
		}
	}
        else if (mMediaFormat == eMEDIAFORMAT_COMPOSITE)
        {
                mpStreamAbstractionAAMP = new StreamAbstractionAAMP_COMPOSITEIN(this, playlistSeekPos, rate);
                if (NULL == mCdaiObject)
                {
                        mCdaiObject = new CDAIObject(this);    //Placeholder to reject the SetAlternateContents()
                }
        }

	mInitSuccess = true;
	AAMPStatusType retVal;
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->SetCDAIObject(mCdaiObject);
		retVal = mpStreamAbstractionAAMP->Init(tuneType);
	}
	else
	{
		retVal = eAAMPSTATUS_GENERIC_ERROR;
	}

	if (retVal != eAAMPSTATUS_OK)
	{
		// Check if the seek position is beyond the duration
		if(retVal == eAAMPSTATUS_SEEK_RANGE_ERROR)
		{
			logprintf("mpStreamAbstractionAAMP Init Failed.Seek Position(%f) out of range(%lld)",mpStreamAbstractionAAMP->GetStreamPosition(),(GetDurationMs()/1000));
			NotifyEOSReached();
		}
		else
		{
			logprintf("mpStreamAbstractionAAMP Init Failed.Error(%d)",retVal);
			AAMPTuneFailure failReason = AAMP_TUNE_INIT_FAILED;
			switch(retVal)
			{
			case eAAMPSTATUS_MANIFEST_DOWNLOAD_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_MANIFEST_DNLD_ERROR; break;
			case eAAMPSTATUS_PLAYLIST_VIDEO_DOWNLOAD_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_PLAYLIST_VIDEO_DNLD_ERROR; break;
			case eAAMPSTATUS_PLAYLIST_AUDIO_DOWNLOAD_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_PLAYLIST_AUDIO_DNLD_ERROR; break;
			case eAAMPSTATUS_MANIFEST_CONTENT_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_MANIFEST_CONTENT_ERROR; break;
			case eAAMPSTATUS_MANIFEST_PARSE_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_MANIFEST_PARSE_ERROR; break;
			case eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR:
				failReason = AAMP_TUNE_INIT_FAILED_TRACK_SYNC_ERROR; break;
			case eAAMPSTATUS_UNSUPPORTED_DRM_ERROR:
				failReason = AAMP_TUNE_DRM_UNSUPPORTED; break;
			default :
				break;
			}

			if (failReason == AAMP_TUNE_INIT_FAILED_PLAYLIST_VIDEO_DNLD_ERROR || failReason == AAMP_TUNE_INIT_FAILED_PLAYLIST_AUDIO_DNLD_ERROR)
			{
				long http_error = mPlaylistFetchFailError;
				SendDownloadErrorEvent(failReason, http_error);
			}
			else
			{
				SendErrorEvent(failReason);
			}
		}
		mInitSuccess = false;
		return;
	}
	else
	{
		prevPositionMiliseconds = -1;
		double updatedSeekPosition = mpStreamAbstractionAAMP->GetStreamPosition();
		seek_pos_seconds = updatedSeekPosition + culledSeconds;
#ifndef AAMP_STOP_SINK_ON_SEEK
		logprintf("%s:%d Updated seek_pos_seconds %f culledSeconds :%f",__FUNCTION__,__LINE__, seek_pos_seconds,culledSeconds);
#endif
		mpStreamAbstractionAAMP->GetStreamFormat(mVideoFormat, mAudioFormat);
		AAMPLOG_INFO("TuneHelper : mVideoFormat %d, mAudioFormat %d", mVideoFormat, mAudioFormat);

		//Identify if HLS with mp4 fragments, to change media format
		if (mVideoFormat == FORMAT_ISO_BMFF && mMediaFormat == eMEDIAFORMAT_HLS)
		{
			mMediaFormat = eMEDIAFORMAT_HLS_MP4;
		}

		// Enable fragment initial caching. Retune not supported
		if(tuneType != eTUNETYPE_RETUNE
			&& mMinInitialCacheSeconds > 0
			&& rate == AAMP_NORMAL_PLAY_RATE
			&& mpStreamAbstractionAAMP->IsInitialCachingSupported())
		{
			mFirstVideoFrameDisplayedEnabled = true;
			mFragmentCachingRequired = true;
		}

		// Set Pause on First Video frame if seeking and requested
		if( mSeekOperationInProgress && seekWhilePaused )
		{
			mFirstVideoFrameDisplayedEnabled = true;
			mPauseOnFirstVideoFrameDisp = true;
		}

#ifndef AAMP_STOP_SINK_ON_SEEK
		if (mMediaFormat == eMEDIAFORMAT_HLS)
		{
			//Live adjust or syncTrack occurred, sent an updated flush event
			if ((!newTune && gpGlobalConfig->gAampDemuxHLSVideoTsTrack) || gpGlobalConfig->gPreservePipeline)
			{
				mStreamSink->Flush(mpStreamAbstractionAAMP->GetFirstPTS(), rate);
			}
		}
		else if (mMediaFormat == eMEDIAFORMAT_DASH)
		{
                        /*
                        commenting the Flush call with updatedSeekPosition as a work around for
                        Trick play freeze issues observed for rogers cDVR content (MBTROGERS-838)
                        @TODO Need to investigate and identify proper way to send Flush and segment 
                        events to avoid the freeze  
			if (!(newTune || (eTUNETYPE_RETUNE == tuneType)) && !IsTSBSupported())
			{
				mStreamSink->Flush(updatedSeekPosition, rate);
			}
			else
			{
				mStreamSink->Flush(0, rate);
			}
			*/
			mStreamSink->Flush(mpStreamAbstractionAAMP->GetFirstPTS(), rate);
		}
#endif
		mStreamSink->SetVideoZoom(zoom_mode);
		mStreamSink->SetVideoMute(video_muted);
		mStreamSink->SetAudioVolume(audio_volume);
		if (mbPlayEnabled)
		{
			mStreamSink->Configure(mVideoFormat, mAudioFormat, mpStreamAbstractionAAMP->GetESChangeStatus());
		}
		mpStreamAbstractionAAMP->ResetESChangeStatus();
		mpStreamAbstractionAAMP->Start();
		if (mbPlayEnabled)
			mStreamSink->Stream();
	}

	if (tuneType == eTUNETYPE_SEEK || tuneType == eTUNETYPE_SEEKTOLIVE)
	{
		mSeekOperationInProgress = false;
		// Pipeline is not configured if mbPlayEnabled is false, so not required
		if (mbPlayEnabled && pipeline_paused == true)
		{
			mStreamSink->Pause(true, false);
		}
	}

	if (newTune)
	{
		PrivAAMPState state;
		GetState(state);
		if((state != eSTATE_ERROR) && (mMediaFormat != eMEDIAFORMAT_OTA))
		{
			/*For OTA this event will be generated from StreamAbstractionAAMP_OTA*/
			SetState(eSTATE_PREPARED);
		}
	}
}

/**
 * @brief Tune to a URL.
 *
 * @param  mainManifestUrl - HTTP/HTTPS url to be played.
 * @param[in] autoPlay - Start playback immediately or not
 * @param  contentType - content Type.
 */
void PrivateInstanceAAMP::Tune(const char *mainManifestUrl, bool autoPlay, const char *contentType, bool bFirstAttempt, bool bFinalAttempt,const char *pTraceID,bool audioDecoderStreamSync)
{
	TuneType tuneType =  eTUNETYPE_NEW_NORMAL;
	gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_INFO);

	ConfigureNetworkTimeout();
	ConfigureManifestTimeout();
	ConfigurePlaylistTimeout();
	ConfigureParallelFetch();
	ConfigureDashParallelFragmentDownload();
	ConfigureBulkTimedMetadata();
	ConfigureRetuneForUnpairedDiscontinuity();
	ConfigureRetuneForGSTInternalError();
	ConfigureWesterosSink();
	ConfigureLicenseCaching();
	ConfigurePreCachePlaylist();
	ConfigureInitFragTimeoutRetryCount();
	mABREnabled = gpGlobalConfig->bEnableABR;
	mUserRequestedBandwidth = gpGlobalConfig->defaultBitrate;
	mLogTimetoTopProfile = true;
	// Reset mProgramDateTime to 0 , to avoid spill over to next tune if same session is 
	// reused 
	mProgramDateTime = 0;
	if(gpGlobalConfig->mUseAverageBWForABR != eUndefinedState)
	{
		mUseAvgBandwidthForABR = (bool)gpGlobalConfig->mUseAverageBWForABR;
	}

	if (gpGlobalConfig->sslVerifyPeer == eUndefinedState){
		/* Disable ssl verification by default */
		gpGlobalConfig->sslVerifyPeer = eFalseState;
		AAMPLOG_INFO("%s:%d : SSL Verification has not configured , default is False", __FUNCTION__,__LINE__);
	}

	//temporary hack for peacock
	if (STARTS_WITH_IGNORE_CASE(mAppName.c_str(), "peacock"))
	{
		if(NULL == mAampCacheHandler)
		{
			mAampCacheHandler = new AampCacheHandler();
		}
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
		if(NULL == mDRMSessionManager)
		{
			mDRMSessionManager = new AampDRMSessionManager();
		}
#endif
	}

	/* Reset counter in new tune */
	mManifestRefreshCount = 0;
	
	// For PreCaching of playlist , no max limit set as size will vary for each playlist length
	if(mCacheMaxSize != 0)
        {
                getAampCacheHandler()->SetMaxPlaylistCacheSize(mCacheMaxSize);
        }
	else if(mPreCacheDnldTimeWindow > 0)
	{
		getAampCacheHandler()->SetMaxPlaylistCacheSize(PLAYLIST_CACHE_SIZE_UNLIMITED);
	}

	mAudioDecoderStreamSync = audioDecoderStreamSync;
	if (NULL == mStreamSink)
	{
		mStreamSink = new AAMPGstPlayer(this);
	}
	/* Initialize gstreamer plugins with correct priority to co-exist with webkit plugins.
	 * Initial priority of aamp plugins is PRIMARY which is less than webkit plugins.
	 * if aamp->Tune is called, aamp plugins should be used, so set priority to a greater value
	 * than that of that of webkit plugins*/
	static bool gstPluginsInitialized = false;
	if (!gstPluginsInitialized)
	{
		gstPluginsInitialized = true;
		AAMPGstPlayer::InitializeAAMPGstreamerPlugins();
	}

	mbPlayEnabled = autoPlay;
	mPlayerPreBuffered = !autoPlay ;

	ResumeDownloads();

	if (!autoPlay)
	{
		pipeline_paused = true;
		logprintf("%s:%d - AutoPlay disabled; Just caching the stream now.\n",__FUNCTION__,__LINE__);
	}

	if (-1 != seek_pos_seconds)
	{
		logprintf("PrivateInstanceAAMP::%s:%d seek position already set, so eTUNETYPE_NEW_SEEK", __FUNCTION__, __LINE__);
		tuneType = eTUNETYPE_NEW_SEEK;
	}
	else
	{
		seek_pos_seconds = 0;
	}

	for(int i = 0; i < eCURLINSTANCE_MAX; i++)
	{
		//cookieHeaders[i].clear();
		httpRespHeaders[i].type = eHTTPHEADERTYPE_UNKNOWN;
		httpRespHeaders[i].data.clear();
	}

	const char *remapUrl = RemapManifestUrl(mainManifestUrl);
	if (remapUrl )
	{
		mainManifestUrl = remapUrl;
	}

	std::tie(mManifestUrl, mDrmInitData) = ExtractDrmInitData(mainManifestUrl);
	mMediaFormat = GetMediaFormatType(mainManifestUrl);
	
	mIsVSS = (strstr(mainManifestUrl, VSS_MARKER) || strstr(mainManifestUrl, VSS_MARKER_FOG));
	mTuneCompleted 	=	false;
	mTSBEnabled	= (mManifestUrl.find("tsb?") != std::string::npos);
	mPersistedProfileIndex	=	-1;
	mServiceZone.clear(); //clear the value if present
	mIsIframeTrackPresent = false;
	mCurrentDrm = nullptr;

	// DELIA-47965: Calling SetContentType without checking contentType != NULL, so that
	// mContentType will be reset to ContentType_UNKNOWN at the start of tune by default
	SetContentType(contentType);
	if (ContentType_CDVR == mContentType)
	{
		mIscDVR = true;
	}

#ifdef AAMP_CC_ENABLED
	if (eMEDIAFORMAT_OTA == mMediaFormat)
	{
		if (gpGlobalConfig->nativeCCRendering)
		{
			AampCCManager::GetInstance()->SetParentalControlStatus(false);
		}
	}
#endif
	if(!IsLiveAdjustRequired()) /* Ideally checking the content is either "ivod/cdvr" to adjust the liveoffset on trickplay. */
	{
		// DELIA-30843/DELIA-31379. for CDVR/IVod, offset is set to higher value
		// need to adjust the liveoffset on trickplay for ivod/cdvr with 30sec

		// Priority of setting:  (1) aampcfg (user override) (2) App Config (3) AAMP Default value 
		if(gpGlobalConfig->cdvrliveOffset != -1)
		{
			// if aamp cfg has override that will be set 		
			mLiveOffset	=	gpGlobalConfig->cdvrliveOffset;
		}
                else if(!mNewLiveOffsetflag)
		{
			// if App has not set the value , set it to default		
			mLiveOffset	=	AAMP_CDVR_LIVE_OFFSET;
		}
	}
	else
	{
		// will be used only for live
		// Priority of setting:  (1) aampcfg (user override) (2) App Config (3) AAMP Default value 
		if(gpGlobalConfig->liveOffset != -1)
		{
			// if aamp cfg has override that will be set 
			mLiveOffset	=	gpGlobalConfig->liveOffset;
		}
                else if(!mNewLiveOffsetflag)
		{
			// if App has not set the value , set it to default 
			mLiveOffset	=	AAMP_LIVE_OFFSET;
		}
	}

	if(bFirstAttempt)
	{
		mTuneAttempts = 1;	//Only the first attempt is xreInitiated.
		mPlayerLoadTime = NOW_STEADY_TS_MS;
	}
	else
	{
		mTuneAttempts++;
	}
	profiler.TuneBegin();
	ResetBufUnderFlowStatus();

	if( !remapUrl )
	{
		if (gpGlobalConfig->mapMPD && mMediaFormat == eMEDIAFORMAT_HLS && (mContentType != ContentType_EAS)) //Don't map, if it is dash and dont map if it is EAS
		{
			std::string hostName = aamp_getHostFromURL(mManifestUrl);
			if((hostName.find(gpGlobalConfig->mapMPD) != std::string::npos) || (mTSBEnabled && mManifestUrl.find(gpGlobalConfig->mapMPD) != std::string::npos))
			{
				replace(mManifestUrl, ".m3u8", ".mpd");
				mMediaFormat = eMEDIAFORMAT_DASH;
			}
		}
		else if (gpGlobalConfig->mapM3U8 && mMediaFormat == eMEDIAFORMAT_DASH)
		{
			std::string hostName = aamp_getHostFromURL(mManifestUrl);
			if((hostName.find(gpGlobalConfig->mapM3U8) != std::string::npos) || (mTSBEnabled && mManifestUrl.find(gpGlobalConfig->mapM3U8) != std::string::npos))
			{
				replace(mManifestUrl, ".mpd" , ".m3u8");
				mMediaFormat = eMEDIAFORMAT_HLS;
			}
		}
		//DELIA-47890 Fog can be disable by  having option fog=0 option in aamp.cfg,based on  that gpGlobalConfig->noFog is updated
		//Removed variable gpGlobalConfig->fogSupportsDash as it has similar usage
		if (gpGlobalConfig->noFog)
		{
			DeFog(mManifestUrl);
		}

		if (mForceEC3)
		{
			replace(mManifestUrl,".m3u8", "-eac3.m3u8");
		}
		if (mDisableEC3)
		{
			replace(mManifestUrl, "-eac3.m3u8", ".m3u8");
		}

		if(gpGlobalConfig->bForceHttp)
		{
			replace(mManifestUrl, "https://", "http://");
		}

		if (mManifestUrl.find("mpd")!= std::string::npos) // new - limit this option to linear content as part of DELIA-23975
		{
			replace(mManifestUrl, "-eac3.mpd", ".mpd");
		} // mpd
	} // !remap_url
 
	mIsFirstRequestToFOG = (mTSBEnabled == true);

	{
		char tuneStrPrefix[64];
		memset(tuneStrPrefix, '\0', sizeof(tuneStrPrefix));
		if (!mAppName.empty())
		{
			snprintf(tuneStrPrefix, sizeof(tuneStrPrefix), "%s PLAYER[%d] APP: %s",(mbPlayEnabled?STRFGPLAYER:STRBGPLAYER), mPlayerId, mAppName.c_str());
		}
		else
		{
			snprintf(tuneStrPrefix, sizeof(tuneStrPrefix), "%s PLAYER[%d]", (mbPlayEnabled?STRFGPLAYER:STRBGPLAYER), mPlayerId);
		}

		if(mManifestUrl.length() < MAX_URL_LOG_SIZE)
		{
			logprintf("%s aamp_tune: attempt: %d format: %s URL: %s\n", tuneStrPrefix, mTuneAttempts, mMediaFormatName[mMediaFormat], mManifestUrl.c_str());
		}
		else
		{
			logprintf("%s aamp_tune: attempt: %d format: %s URL: (BIG)\n", tuneStrPrefix, mTuneAttempts, mMediaFormatName[mMediaFormat]);
			printf("URL: %s\n", mManifestUrl.c_str());
		}
	}

	// this function uses mIsVSS and mTSBEnabled, hence it should be called after these variables are updated.
	ExtractServiceZone(mManifestUrl);

	SetTunedManifestUrl(mTSBEnabled);

	if(bFirstAttempt)
	{ // TODO: make mFirstTuneFormat of type MediaFormat
		mfirstTuneFmt = (int)mMediaFormat;
	}
	mCdaiObject = NULL;
	TuneHelper(tuneType);
	// do not change location of this set, it should be done after sending perviouse VideoEnd data which
	// is done in TuneHelper->SendVideoEndEvent function.
	if(pTraceID)
	{
		this->mTraceUUID = pTraceID;
	}
	else
	{
		this->mTraceUUID = "unknown";
	}
}

/**
 *   @brief Assign the correct mediaFormat by parsing the url
 *   @param[in]  manifest url
 */
MediaFormat PrivateInstanceAAMP::GetMediaFormatType(const char *url)
{
	MediaFormat rc = eMEDIAFORMAT_UNKNOWN;
	std::string urlStr(url); // for convenience, convert to std::string

#ifdef TRUST_LOCATOR_EXTENSION_IF_PRESENT // disable to exersize alternate path
	if( urlStr.rfind("hdmiin:",0)==0 )
	{
		rc = eMEDIAFORMAT_HDMI;
	}
        else if( urlStr.rfind("cvbsin:",0)==0 )
        {
                rc = eMEDIAFORMAT_COMPOSITE;
        }
	else if((urlStr.rfind("live:",0)==0) || (urlStr.rfind("tune:",0)==0)  || (urlStr.rfind("mr:",0)==0)) 
	{
		rc = eMEDIAFORMAT_OTA;
	}
	else if(urlStr.rfind("http://127.0.0.1", 0) == 0) // starts with localhost
	{ // where local host is used; inspect further to determine if this locator involves FOG

		size_t fogUrlStart = urlStr.find("recordedUrl=", 16); // search forward, skipping 16-char http://127.0.0.1

		if(fogUrlStart != std::string::npos)
		{ // definitely FOG - extension is inside recordedUrl URI parameter

			size_t fogUrlEnd = urlStr.find("&", fogUrlStart); // end of recordedUrl

			if(fogUrlEnd != std::string::npos)
			{
				if(urlStr.rfind("m3u8", fogUrlEnd) != std::string::npos)
				{
					rc = eMEDIAFORMAT_HLS;
				}
				else if(urlStr.rfind("mpd", fogUrlEnd)!=std::string::npos)
				{
					rc = eMEDIAFORMAT_DASH;
				}

				// should never get here with UNKNOWN format, but if we do, just fall through to normal locator scanning
			}
		}
	}
	
	if(rc == eMEDIAFORMAT_UNKNOWN)
	{ // do 'normal' (non-FOG) locator parsing

		size_t extensionEnd = urlStr.find("?"); // delimited for URI parameters, or end-of-string
		std::size_t extensionStart = urlStr.rfind(".", extensionEnd); // scan backwards to find final "."
		int extensionLength;

		if(extensionStart != std::string::npos)
		{ // found an extension
			if(extensionEnd == std::string::npos)
			{
				extensionEnd = urlStr.length();
			}

			extensionStart++; // skip past the "." - no reason to re-compare it

			extensionLength = (int)(extensionEnd - extensionStart); // bytes between "." and end of query delimiter/end of string

			if(extensionLength == 4 && urlStr.compare(extensionStart, extensionLength, "m3u8") == 0)
			{
				rc = eMEDIAFORMAT_HLS;
			}
			else if(extensionLength == 3)
			{
				if(urlStr.compare(extensionStart,extensionLength,"mpd") == 0)
				{
					rc = eMEDIAFORMAT_DASH;
				}
				else if(urlStr.compare(extensionStart,extensionLength,"mp3") == 0 || urlStr.compare(extensionStart,extensionLength,"mp4") == 0 ||
					urlStr.compare(extensionStart,extensionLength,"mkv") == 0)
				{
					rc = eMEDIAFORMAT_PROGRESSIVE;
				}
			}
		}
	}
#endif // TRUST_LOCATOR_EXTENSION_IF_PRESENT

	if(rc == eMEDIAFORMAT_UNKNOWN)
	{
		// no extension - sniff first few bytes of file to disambiguate
		struct GrowableBuffer sniffedBytes = {0, 0, 0};
		std::string effectiveUrl;
		long http_error;
		double downloadTime;
		long bitrate;
		int fogError;

		CurlInit(eCURLINSTANCE_MANIFEST_PLAYLIST, 1, GetNetworkProxy());

		bool gotManifest = GetFile(
							url,
							&sniffedBytes,
							effectiveUrl,
							&http_error,
							&downloadTime,
							"0-100", // download first few bytes only
							// TODO: ideally could use "0-6" for range but write_callback sometimes not called before curl returns http 206
							eCURLINSTANCE_MANIFEST_PLAYLIST,
							false,
							eMEDIATYPE_MANIFEST,
							&bitrate,
							&fogError,
							0.0);

		if(gotManifest)
		{
			if(sniffedBytes.len >= 7 && memcmp(sniffedBytes.ptr, "#EXTM3U8", 7) == 0)
			{
				rc = eMEDIAFORMAT_HLS;
			}
			else if((sniffedBytes.len >= 6 && memcmp(sniffedBytes.ptr, "<?xml ", 6) == 0) || // can start with xml
					 (sniffedBytes.len >= 5 && memcmp(sniffedBytes.ptr, "<MPD ", 5) == 0)) // or directly with mpd
			{ // note: legal to have whitespace before leading tag
				rc = eMEDIAFORMAT_DASH;
			}
			else
			{
				rc = eMEDIAFORMAT_PROGRESSIVE;
			}
		}
		aamp_Free(&sniffedBytes.ptr);
	}
	return rc;
}

/**
 *   @brief Check if AAMP is in stalled state after it pushed EOS to
 *   notify discontinuity
 *
 *   @param[in]  mediaType stream type
 */
void PrivateInstanceAAMP::CheckForDiscontinuityStall(MediaType mediaType)
{
	AAMPLOG_TRACE("%s:%d : Enter mediaType %d", __FUNCTION__, __LINE__, mediaType);
	if(!(mStreamSink->CheckForPTSChangeWithTimeout(gpGlobalConfig->discontinuityTimeout)))
	{
		AAMPLOG_INFO("%s:%d : No change in PTS for more than %ld ms, schedule retune!",__FUNCTION__, __LINE__, gpGlobalConfig->discontinuityTimeout);
		mProcessingDiscontinuity[eMEDIATYPE_VIDEO] = false;
		mProcessingDiscontinuity[eMEDIATYPE_AUDIO] = false;
		ResetTrackDiscontinuityIgnoredStatus();
		ScheduleRetune(eSTALL_AFTER_DISCONTINUITY, mediaType);
	}
	AAMPLOG_TRACE("%s:%d : Exit mediaType %d\n", __FUNCTION__, __LINE__, mediaType);
}

/**
 *   @brief return service zone, extracted from locator &sz URI parameter
 *   @param  url - stream url with vss service zone info as query string
 *   @return std::string
 */
void PrivateInstanceAAMP::ExtractServiceZone(std::string url)
{
	if(mIsVSS && !url.empty())
	{
		if(mTSBEnabled)
		{ // extract original locator from FOG recordedUrl URI parameter
			DeFog(url);
		}
		size_t vssStart = url.find(VSS_MARKER);
		if( vssStart != std::string::npos )
		{
			vssStart += VSS_MARKER_LEN; // skip "?sz="
			size_t vssLen = url.find('&',vssStart);
			if( vssLen != std::string::npos )
			{
				vssLen -= vssStart;
			}
			mServiceZone = url.substr(vssStart, vssLen );
			aamp_DecodeUrlParameter(mServiceZone); // DELIA-44703
		}
		else
		{
			AAMPLOG_ERR("PrivateInstanceAAMP::%s - ERROR: url does not have vss marker :%s ", __FUNCTION__,url.c_str());
		}
	}
}

std::string PrivateInstanceAAMP::GetContentTypString()
{
    std::string strRet;
    switch(mContentType)
    {
        case ContentType_CDVR :
        {
            strRet = "CDVR"; //cdvr
            break;
        }
        case ContentType_VOD :
        {
            strRet = "VOD"; //vod
            break;
        }    
        case ContentType_LINEAR :
        {
            strRet = "LINEAR"; //linear
            break;
        }    
        case ContentType_IVOD :
        {
            strRet = "IVOD"; //ivod
            break;
        }    
        case ContentType_EAS :
        {
            strRet ="EAS"; //eas
            break;
        }    
        case ContentType_CAMERA :
        {
            strRet = "XfinityHome"; //camera
            break;
        }    
        case ContentType_DVR :
        {
            strRet = "DVR"; //dvr
            break;
        }    
        case ContentType_MDVR :
        {
            strRet =  "MDVR" ; //mdvr
            break;
        }    
        case ContentType_IPDVR :
        {
            strRet ="IPDVR" ; //ipdvr
            break;
        }    
        case ContentType_PPV :
        {
            strRet =  "PPV"; //ppv
            break;
        }
        case ContentType_OTT :
        {
            strRet =  "OTT"; //ott
            break;
        }
        case ContentType_OTA :
        {
            strRet =  "OTA"; //ota
            break;
        }
        default:
        {
            strRet =  "Unknown";
            break;
        }
     }

    return strRet;
}

void PrivateInstanceAAMP::NotifySinkBufferFull(MediaType type)
{
	if(type != eMEDIATYPE_VIDEO)
		return;

	if(mpStreamAbstractionAAMP)
	{
		MediaTrack* video = mpStreamAbstractionAAMP->GetMediaTrack(eTRACK_VIDEO);
		if(video && video->enabled)
			video->OnSinkBufferFull();
	}
}

/**
 * @brief set a content type
 * @param[in] cType - content type 
 * @param[in] mainManifestUrl - main manifest URL
 * @retval none
 */
void PrivateInstanceAAMP::SetContentType(const char *cType)
{
	mContentType = ContentType_UNKNOWN; //default unknown
	if(NULL != cType)
	{
		std::string playbackMode = std::string(cType);
		if(playbackMode == "CDVR")
		{
			mContentType = ContentType_CDVR; //cdvr
		}
		else if(playbackMode == "VOD")
		{
			mContentType = ContentType_VOD; //vod
		}
		else if(playbackMode == "LINEAR_TV")
		{
			mContentType = ContentType_LINEAR; //linear
		}
		else if(playbackMode == "IVOD")
		{
			mContentType = ContentType_IVOD; //ivod
		}
		else if(playbackMode == "EAS")
		{
			mContentType = ContentType_EAS; //eas
		}
		else if(playbackMode == "xfinityhome")
		{
			mContentType = ContentType_CAMERA; //camera
		}
		else if(playbackMode == "DVR")
		{
			mContentType = ContentType_DVR; //dvr
		}
		else if(playbackMode == "MDVR")
		{
			mContentType = ContentType_MDVR; //mdvr
		}
		else if(playbackMode == "IPDVR")
		{
			mContentType = ContentType_IPDVR; //ipdvr
		}
		else if(playbackMode == "PPV")
		{
			mContentType = ContentType_PPV; //ppv
		}
		else if(playbackMode == "OTT")
		{
			mContentType = ContentType_OTT; //ott
		}
		else if(playbackMode == "OTA")
		{
			mContentType = ContentType_OTA; //ota
		}
		else if(playbackMode == "HDMI_IN")
		{
			mContentType = ContentType_HDMIIN; //ota
		}
		else if(playbackMode == "COMPOSITE_IN")
		{
			mContentType = ContentType_COMPOSITEIN; //ota
		}
	}
}

/**
 * @brief Get Content Type
 * @return ContentType
 */
ContentType PrivateInstanceAAMP::GetContentType() const
{
	return mContentType;
}

/**
 * @brief Extract DRM init data
 * @return url url
 * retval a tuple of url and DRM init data
 */
const std::tuple<std::string, std::string> PrivateInstanceAAMP::ExtractDrmInitData(const char *url)
{
	std::string urlStr(url);
	std::string drmInitDataStr;

	const size_t queryPos = urlStr.find("?");
	std::string modUrl;
	if (queryPos != std::string::npos)
	{
		// URL contains a query string. Strip off & decode the drmInitData (if present)
		modUrl = urlStr.substr(0, queryPos);
		const std::string parameterDefinition("drmInitData=");
		std::string parameter;
		std::stringstream querySs(urlStr.substr(queryPos + 1, std::string::npos));
		while (std::getline(querySs, parameter, '&'))
		{ // with each URI parameter
			if (parameter.rfind(parameterDefinition, 0) == 0)
			{ // found drmInitData URI parameter
				drmInitDataStr = parameter.substr(parameterDefinition.length());
				aamp_DecodeUrlParameter( drmInitDataStr );
			}
			else
			{ // filter out drmInitData; reintroduce all other URI parameters
				modUrl.append((queryPos == modUrl.length()) ? "?" : "&");
				modUrl.append(parameter);
			}
		}
		urlStr = modUrl;
	}
	return std::tuple<std::string, std::string>(urlStr, drmInitDataStr);
}

/**
 *   @brief Check if current stream is muxed
 *
 *   @return true if current stream is muxed
 */
bool PrivateInstanceAAMP::IsPlayEnabled()
{
	return mbPlayEnabled;
}

/**
 * @brief Soft-realease player.
 *
 */
void PrivateInstanceAAMP::detach()
{
	if(mpStreamAbstractionAAMP && mbPlayEnabled) //Player is running
	{
		AAMPLOG_WARN("%s:%d PLAYER[%d] Player %s=>%s and soft release.", __FUNCTION__, __LINE__, mPlayerId, STRFGPLAYER, STRBGPLAYER );
		pipeline_paused = true;
		mpStreamAbstractionAAMP->StopInjection();
#ifdef AAMP_CC_ENABLED
		// Stop CC when pipeline is stopped
		if (gpGlobalConfig->nativeCCRendering)
		{
			AampCCManager::GetInstance()->Release();
		}
#endif
		mStreamSink->Stop(true);
		mbPlayEnabled = false;
		mPlayerPreBuffered  = false;
	}
}

/**
 * @brief Get AampCacheHandler instance
 * @retval AampCacheHandler instance
 */
AampCacheHandler * PrivateInstanceAAMP::getAampCacheHandler()
{
	return mAampCacheHandler;
}

/**
 * @brief Set profile ramp down limit.
 * @param limit rampdown limit
 */
void PrivateInstanceAAMP::SetRampDownLimit(int limit)
{
	if (gpGlobalConfig->rampdownLimit >= 0)
	{
		mRampDownLimit = gpGlobalConfig->rampdownLimit;
		AAMPLOG_INFO("%s:%d Setting limit from configuration file: %d", __FUNCTION__, __LINE__, gpGlobalConfig->rampdownLimit);
	}
	else
	{
		if (limit >= 0)
		{
			AAMPLOG_INFO("%s:%d Setting Rampdown limit : %d", __FUNCTION__, __LINE__, limit);
			mRampDownLimit = limit;
		}
		else
		{
			AAMPLOG_WARN("%s:%d Invalid limit value %d", __FUNCTION__,__LINE__, limit);
		}
	}
}

/**
 * @brief Set minimum bitrate value.
 *
 */
void PrivateInstanceAAMP::SetMinimumBitrate(long bitrate)
{
	mMinBitrate = bitrate;
}

/**
 * @brief Set maximum bitrate value.
 */
void PrivateInstanceAAMP::SetMaximumBitrate(long bitrate)
{
	if (bitrate > 0)
	{
		mMaxBitrate = bitrate;
	}
}

/**
 * @brief Get maximum bitrate value.
 * @return maximum bitrate value
 */
long PrivateInstanceAAMP::GetMaximumBitrate()
{
	return mMaxBitrate;
}

/**
 * @brief Set maximum bitrate value.
 * @return minimum bitrate value
 */
long PrivateInstanceAAMP::GetMinimumBitrate()
{
	return mMinBitrate;
}

/**
 * @brief Fetch a file from CDN and update profiler
 * @param bucketType type of profiler bucket
 * @param fragmentUrl URL of the file
 * @param[out] len length of buffer
 * @param curlInstance instance to be used to fetch
 * @param range http range
 * @param fileType media type of the file
 * @retval buffer containing file, free using aamp_Free
 */
char *PrivateInstanceAAMP::LoadFragment(ProfilerBucketType bucketType, std::string fragmentUrl, std::string& effectiveUrl, size_t *len, unsigned int curlInstance, const char *range, long * http_code, double *downloadTime, MediaType fileType,int * fogError)
{
	profiler.ProfileBegin(bucketType);
	struct GrowableBuffer fragment = { 0, 0, 0 }; // TODO: leaks if thread killed
	if (!GetFile(fragmentUrl, &fragment, effectiveUrl, http_code, downloadTime, range, curlInstance, true, fileType,NULL,fogError))
	{
		profiler.ProfileError(bucketType, *http_code);
	}
	else
	{
		profiler.ProfileEnd(bucketType);
	}
	*len = fragment.len;
	return fragment.ptr;
}

/**
 * @brief Fetch a file from CDN and update profiler
 * @param bucketType type of profiler bucket
 * @param fragmentUrl URL of the file
 * @param[out] fragment pointer to buffer abstraction
 * @param curlInstance instance to be used to fetch
 * @param range http range
 * @param fileType media type of the file
 * @param http_code http code
 * @retval true on success, false on failure
 */
bool PrivateInstanceAAMP::LoadFragment(ProfilerBucketType bucketType, std::string fragmentUrl,std::string& effectiveUrl, struct GrowableBuffer *fragment, 
					unsigned int curlInstance, const char *range, MediaType fileType,long * http_code, double *downloadTime, long *bitrate,int * fogError, double fragmentDurationSeconds)
{
	bool ret = true;
	profiler.ProfileBegin(bucketType);
	if (!GetFile(fragmentUrl, fragment, effectiveUrl, http_code, downloadTime, range, curlInstance, false,fileType, bitrate, NULL, fragmentDurationSeconds))
	{
		ret = false;
		profiler.ProfileError(bucketType, *http_code);
	}
	else
	{
		profiler.ProfileEnd(bucketType);
	}
	return ret;
}

/**
 * @brief Push a media fragment to sink
 * @param mediaType type of buffer
 * @param ptr buffer containing fragment
 * @param len length of buffer
 * @param fragmentTime PTS of fragment in seconds
 * @param fragmentDuration duration of fragment in seconds
 */
void PrivateInstanceAAMP::PushFragment(MediaType mediaType, char *ptr, size_t len, double fragmentTime, double fragmentDuration)
{
	BlockUntilGstreamerWantsData(NULL, 0, 0);
	SyncBegin();
	mStreamSink->Send(mediaType, ptr, len, fragmentTime, fragmentTime, fragmentDuration);
	SyncEnd();
}

/**
 * @brief Push a media fragment to sink
 * @note Takes ownership of buffer
 * @param mediaType type of fragment
 * @param buffer contains data
 * @param fragmentTime PTS of fragment in seconds
 * @param fragmentDuration duration of fragment in seconds
 */
void PrivateInstanceAAMP::PushFragment(MediaType mediaType, GrowableBuffer* buffer, double fragmentTime, double fragmentDuration)
{
	BlockUntilGstreamerWantsData(NULL, 0, 0);
	SyncBegin();
	mStreamSink->Send(mediaType, buffer, fragmentTime, fragmentTime, fragmentDuration);
	SyncEnd();
}

/**
 * @brief Notifies EOS to sink
 * @param mediaType Type of media
 */
void PrivateInstanceAAMP::EndOfStreamReached(MediaType mediaType)
{
	if (mediaType != eMEDIATYPE_SUBTITLE)
	{
		SyncBegin();
		mStreamSink->EndOfStreamReached(mediaType);
		SyncEnd();

		// If EOS during Buffering, set Playing and let buffer to dry out
		// Sink is already unpaused by EndOfStreamReached()
		pthread_mutex_lock(&mFragmentCachingLock);
		mFragmentCachingRequired = false;
		PrivAAMPState state;
		GetState(state);
		if(state == eSTATE_BUFFERING)
		{
			if(mpStreamAbstractionAAMP)
			{
				mpStreamAbstractionAAMP->NotifyPlaybackPaused(false);
			}
			SetState(eSTATE_PLAYING);
		}
		pthread_mutex_unlock(&mFragmentCachingLock);
	}
}

/**
 * @brief Get seek base position
 * @retval seek base position
 */
double PrivateInstanceAAMP::GetSeekBase(void)
{
	return seek_pos_seconds;
}

/**
 *   @brief Get current drm
 *
 *   @return current drm
 */
std::shared_ptr<AampDrmHelper> PrivateInstanceAAMP::GetCurrentDRM(void)
{
	return mCurrentDrm;
}

/**
 *   @brief Set a preferred bitrate for video.
 *
 *   @param[in] preferred bitrate.
 */
void PrivateInstanceAAMP::SetVideoBitrate(long bitrate)
{
	if (bitrate == 0)
	{
		mABREnabled = true;
	}
	else
	{
		mABREnabled = false;
		mUserRequestedBandwidth = bitrate;
	}
}

/**
 *   @brief Get preferred bitrate for video.
 *
 *   @return preferred bitrate.
 */
long PrivateInstanceAAMP::GetVideoBitrate()
{
	return mUserRequestedBandwidth;
}

/**
 *   @brief Get available thumbnail tracks.
 *
 *   @return string of available thumbnail tracks.
 */
std::string PrivateInstanceAAMP::GetThumbnailTracks()
{
	std::string op;
	if(mpStreamAbstractionAAMP)
	{
		traceprintf("Entering PrivateInstanceAAMP::%s.",__FUNCTION__);
		std::vector<StreamInfo*> data = mpStreamAbstractionAAMP->GetAvailableThumbnailTracks();
		cJSON *root;
		cJSON *item;
		if(!data.empty())
		{
			root = cJSON_CreateArray();
			if(root)
			{
				for( int i = 0; i < data.size(); i++)
				{
					cJSON_AddItemToArray(root, item = cJSON_CreateObject());
					if(data[i]->bandwidthBitsPerSecond >= 0)
					{
						char buf[32];
						sprintf(buf,"%dx%d",data[i]->resolution.width,data[i]->resolution.height);
						cJSON_AddStringToObject(item,"RESOLUTION",buf);
						cJSON_AddNumberToObject(item,"BANDWIDTH",data[i]->bandwidthBitsPerSecond);
					}
				}
				char *jsonStr = cJSON_Print(root);
				if (jsonStr)
				{
					op.assign(jsonStr);
					free(jsonStr);
				}
				cJSON_Delete(root);
			}
		}
		traceprintf("In PrivateInstanceAAMP::%s, Json string:%s",__FUNCTION__,op.c_str());
	}
	return op;
}

/**
 *   @brief Get thumbnail data.
 *
 *   @return string thumbnail tile information.
 */
std::string PrivateInstanceAAMP::GetThumbnails(double tStart, double tEnd)
{
	std::string rc;
	if(mpStreamAbstractionAAMP)
	{
		std::string baseurl;
		int raw_w, raw_h, width, height;
		std::vector<ThumbnailData> datavec = mpStreamAbstractionAAMP->GetThumbnailRangeData(tStart, tEnd, &baseurl, &raw_w, &raw_h, &width, &height);
		if( !datavec.empty() )
		{
			cJSON *root = cJSON_CreateObject();
			if(!baseurl.empty())
			{
				cJSON_AddStringToObject(root,"baseUrl",baseurl.c_str());
			}
			if(raw_w > 0)
			{
				cJSON_AddNumberToObject(root,"raw_w",raw_w);
			}
			if(raw_h > 0)
			{
				cJSON_AddNumberToObject(root,"raw_h",raw_h);
			}
			cJSON_AddNumberToObject(root,"width",width);
			cJSON_AddNumberToObject(root,"height",height);

			cJSON *tile = cJSON_AddArrayToObject(root,"tile");
			for( const ThumbnailData &iter : datavec )
			{
				cJSON *item;
				cJSON_AddItemToArray(tile, item = cJSON_CreateObject() );
				if(!iter.url.empty())
				{
					cJSON_AddStringToObject(item,"url",iter.url.c_str());
				}
				cJSON_AddNumberToObject(item,"t",iter.t);
				cJSON_AddNumberToObject(item,"d",iter.d);
				cJSON_AddNumberToObject(item,"x",iter.x);
				cJSON_AddNumberToObject(item,"y",iter.y);
			}
			char *jsonStr = cJSON_Print(root);
			if( jsonStr )
			{
				rc.assign( jsonStr );
			}
			cJSON_Delete(root);
		}
	}
	return rc;
}

/**
 *   @brief To set the vod-tune-event according to the player.
 *
 *   @param[in] preferred tune event type
 */
void PrivateInstanceAAMP::SetTuneEventConfig( TunedEventConfig tuneEventType)
{
	if(gpGlobalConfig->tunedEventConfigVOD == eTUNED_EVENT_MAX)
	{
		mTuneEventConfigVod = tuneEventType;
	}
	else
	{
		mTuneEventConfigVod = gpGlobalConfig->tunedEventConfigVOD;
	}

	if(gpGlobalConfig->tunedEventConfigLive == eTUNED_EVENT_MAX)
	{
                mTuneEventConfigLive = tuneEventType;
	}
	else
	{
		mTuneEventConfigLive = gpGlobalConfig->tunedEventConfigLive;
	}
}

/**
 *   @brief Set Async Tune Configuration
 *   @param[in] bValue - true if async tune enabled
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetAsyncTuneConfig(bool bValue)
{
	if(gpGlobalConfig->mAsyncTuneConfig == eUndefinedState)
	{
		mAsyncTuneEnabled = bValue;
	}
	else
	{
		mAsyncTuneEnabled = (bool)gpGlobalConfig->mAsyncTuneConfig;
	}
	AAMPLOG_INFO("%s:%d Async Tune Config : %s ",__FUNCTION__,__LINE__,(mAsyncTuneEnabled)?"True":"False");
}

/**
 *   @brief Get Async Tune configuration
 *
 *   @return bool - true if config set 
 */
bool PrivateInstanceAAMP::GetAsyncTuneConfig()
{
        return mAsyncTuneEnabled;
}

/**
 *   @brief Set Matching BaseUrl Config Configuration
 *
 *   @param[in] bValue - true if Matching BaseUrl enabled
 *   @return void
 */
void PrivateInstanceAAMP::SetMatchingBaseUrlConfig(bool bValue)
{
	// app can set only if curr state is eUndefinedState
	if(eUndefinedState == gpGlobalConfig->useMatchingBaseUrl)
	{
		gpGlobalConfig->useMatchingBaseUrl = (TriState) bValue;
	}
	else
	{
		AAMPLOG_WARN("%s : Ignoring app setting[%d] already set value:%d", __FUNCTION__, bValue ,gpGlobalConfig->useMatchingBaseUrl );
	}
}

/**
 *   @brief to configure propagate URI parameters for fragment download
 *
 *   @param[in] bValue - true to enable
 *   @return void
 */

void PrivateInstanceAAMP::SetPropagateUriParameters(bool bValue)
{
	gpGlobalConfig->mPropagateUriParameters = (TriState)bValue;
	logprintf("%s:%d Propagate URIparameters : %s ",__FUNCTION__,__LINE__,(gpGlobalConfig->mPropagateUriParameters)?"True":"False");
}

/**
 *   @brief to disable SSL verify peer 
 *
 *   @param[in] bValue - true to enable the configuration
 *   @return void
 */

void PrivateInstanceAAMP::SetSslVerifyPeerConfig(bool bValue)
{
	if (gpGlobalConfig->sslVerifyPeer == eUndefinedState)
	{
		gpGlobalConfig->sslVerifyPeer = (TriState)bValue;
	}
	logprintf("%s:%d Disable Ssl Verify Peer : %s ",__FUNCTION__,__LINE__,(gpGlobalConfig->sslVerifyPeer)?"True":"False");
}


/**
 *   @brief Set Westeros sink Configuration
 *   @param[in] bValue - true if westeros sink enabled
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetWesterosSinkConfig(bool bValue)
{
	if(gpGlobalConfig->mWesterosSinkConfig == eUndefinedState)
	{
		mWesterosSinkEnabled = bValue;
	}
	else
	{
		mWesterosSinkEnabled = (bool)gpGlobalConfig->mWesterosSinkConfig;
	}
	AAMPLOG_INFO("%s:%d Westeros Sink Config : %s ",__FUNCTION__,__LINE__,(mWesterosSinkEnabled)?"True":"False");
}

/**
 *   @brief Set license caching
 *   @param[in] bValue - true/false to enable/disable license caching
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetLicenseCaching(bool bValue)
{
	if(gpGlobalConfig->licenseCaching == eUndefinedState)
	{
		mLicenseCaching = bValue;
	}
	else
	{
		mLicenseCaching = (bool)gpGlobalConfig->licenseCaching;
	}

	AAMPLOG_INFO("%s:%d License Caching is : %s ",__FUNCTION__, __LINE__, (mLicenseCaching ? "True" : "False"));
}

/**
 *   @brief Configure New ABR Enable/Disable
 *   @param[in] bValue - true if new ABR enabled
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetNewABRConfig(bool bValue)
{
	if(gpGlobalConfig->abrBufferCheckEnabled == eUndefinedState)
	{
		mABRBufferCheckEnabled = bValue;
	}
	else
	{
		mABRBufferCheckEnabled = (bool)gpGlobalConfig->abrBufferCheckEnabled;
	}
	AAMPLOG_INFO("%s:%d New ABR Config : %s ",__FUNCTION__,__LINE__,(mABRBufferCheckEnabled)?"True":"False");

	// temp code until its enabled in Peacock App - Remove it later.
	if(gpGlobalConfig->useNewDiscontinuity == eUndefinedState)
	{
		mNewAdBreakerEnabled = bValue;
		gpGlobalConfig->hlsAVTrackSyncUsingStartTime = bValue;	
	}
	else
	{
		mNewAdBreakerEnabled = (bool)gpGlobalConfig->useNewDiscontinuity;		
	}
	AAMPLOG_INFO("%s:%d New AdBreaker Config : %s ",__FUNCTION__,__LINE__,(mNewAdBreakerEnabled)?"True":"False");
}

/**
 *   @brief Configure New AdBreaker Enable/Disable
 *   @param[in] bValue - true if new ABR enabled
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetNewAdBreakerConfig(bool bValue)
{
	if(gpGlobalConfig->useNewDiscontinuity == eUndefinedState)
	{
		mNewAdBreakerEnabled = bValue;
		gpGlobalConfig->hlsAVTrackSyncUsingStartTime = bValue;
	}
	else
	{
		mNewAdBreakerEnabled = (bool)gpGlobalConfig->useNewDiscontinuity;
	}
	AAMPLOG_INFO("%s:%d New AdBreaker Config : %s ",__FUNCTION__,__LINE__,(mNewAdBreakerEnabled)?"True":"False");
}

/**
 *   @brief Set video rectangle.
 *
 *   @param  x - horizontal start position.
 *   @param  y - vertical start position.
 *   @param  w - width.
 *   @param  h - height.
 */
void PrivateInstanceAAMP::SetVideoRectangle(int x, int y, int w, int h)
{
	if (mpStreamAbstractionAAMP && ((mMediaFormat == eMEDIAFORMAT_OTA) || (mMediaFormat == eMEDIAFORMAT_HDMI) || (mMediaFormat == eMEDIAFORMAT_COMPOSITE)))
		mpStreamAbstractionAAMP->SetVideoRectangle(x, y, w, h);
	else
		mStreamSink->SetVideoRectangle(x, y, w, h);
}

/**
 *   @brief Set video zoom.
 *
 *   @param  zoom - zoom mode.
 */
void PrivateInstanceAAMP::SetVideoZoom(VideoZoomMode zoom)
{
	mStreamSink->SetVideoZoom(zoom);
}

/**
 *   @brief Enable/ Disable Video.
 *
 *   @param  muted - true to disable video, false to enable video.
 */
void PrivateInstanceAAMP::SetVideoMute(bool muted)
{
	mStreamSink->SetVideoMute(muted);
}

/**
 *   @brief Set Audio Volume.
 *
 *   @param  volume - Minimum 0, maximum 100.
 */
void PrivateInstanceAAMP::SetAudioVolume(int volume)
{
	mStreamSink->SetAudioVolume(volume);
}

/**
 * @brief abort ongoing downloads and returns error on future downloads
 * called while stopping fragment collector thread
 */
void PrivateInstanceAAMP::DisableDownloads(void)
{
	pthread_mutex_lock(&mLock);
	mDownloadsEnabled = false;
	pthread_cond_broadcast(&mDownloadsDisabled);
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief Check if downloads are enabled
 * @retval true if downloads are enabled
 */
bool PrivateInstanceAAMP::DownloadsAreEnabled(void)
{
	return mDownloadsEnabled; // needs mutex protection?
}

/**
 * @brief Enable downloads
 */
void PrivateInstanceAAMP::EnableDownloads()
{
	pthread_mutex_lock(&mLock);
	mDownloadsEnabled = true;
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief Sleep until timeout is reached or interrupted
 * @param timeInMs timeout in milliseconds
 */
void PrivateInstanceAAMP::InterruptableMsSleep(int timeInMs)
{
	if (timeInMs > 0)
	{
		struct timespec ts;
		int ret;
		ts = aamp_GetTimespec(timeInMs);
		pthread_mutex_lock(&mLock);
		if (mDownloadsEnabled)
		{
			ret = pthread_cond_timedwait(&mDownloadsDisabled, &mLock, &ts);
			if (0 == ret)
			{
				//logprintf("sleep interrupted!");
			}
#ifndef WIN32
			else if (ETIMEDOUT != ret)
			{
				logprintf("sleep - condition wait failed %s", strerror(ret));
			}
#endif
		}
		pthread_mutex_unlock(&mLock);
	}
}

/**
 * @brief Get stream duration
 * @retval duration is milliseconds
 */
long long PrivateInstanceAAMP::GetDurationMs()
{
	if (mMediaFormat == eMEDIAFORMAT_PROGRESSIVE)
	{
	 	long long ms = mStreamSink->GetDurationMilliseconds();
	 	durationSeconds = ms/1000.0;
	 	return ms;
	}
	else
	{
		return (long long)(durationSeconds*1000.0);
	}
}

/**
 * @brief Get current stream position
 * @retval current stream position in ms
 */
long long PrivateInstanceAAMP::GetPositionMs()
{
	return (prevPositionMiliseconds!=-1) ? prevPositionMiliseconds : GetPositionMilliseconds();
}

/**
 * @brief Get current stream position
 * @retval current stream position in ms
 */
long long PrivateInstanceAAMP::GetPositionMilliseconds()
{
	long long positionMiliseconds = seek_pos_seconds * 1000.0;
	if (trickStartUTCMS >= 0)
	{
		//DELIA-39530 - Audio only playback is un-tested. Hence disabled for now
		if (gpGlobalConfig->bPositionQueryEnabled && !gpGlobalConfig->bAudioOnlyPlayback)
		{
			positionMiliseconds += mStreamSink->GetPositionMilliseconds();
		}
		else
		{
			long long elapsedTime = aamp_GetCurrentTimeMS() - trickStartUTCMS;
			positionMiliseconds += (((elapsedTime > 1000) ? elapsedTime : 0) * rate);
		}

		if ((-1 != prevPositionMiliseconds) && (AAMP_NORMAL_PLAY_RATE == rate))
		{
			long long diff = positionMiliseconds - prevPositionMiliseconds;

			if ((diff > MAX_DIFF_BETWEEN_PTS_POS_MS) || (diff < 0))
			{
				AAMPLOG_WARN("%s:%d diff %lld prev-pos-ms %lld current-pos-ms %lld, restore prev-pos as current-pos!!", __FUNCTION__, __LINE__, diff, prevPositionMiliseconds, positionMiliseconds);
				positionMiliseconds = prevPositionMiliseconds;
			}
		}

		if (positionMiliseconds < 0)
		{
			AAMPLOG_WARN("%s : Correcting positionMiliseconds %lld to zero", __FUNCTION__, positionMiliseconds);
			positionMiliseconds = 0;
		}
		else if (mpStreamAbstractionAAMP)
		{
			if (!mIsLive)
			{
				long long durationMs  = GetDurationMs();
				if(positionMiliseconds > durationMs)
				{
					AAMPLOG_WARN("%s : Correcting positionMiliseconds %lld to duration %lld", __FUNCTION__, positionMiliseconds, durationMs);
					positionMiliseconds = durationMs;
				}
			}
			else
			{
				long long tsbEndMs = GetDurationMs() + (culledSeconds * 1000.0);
				if(positionMiliseconds > tsbEndMs)
				{
					AAMPLOG_WARN("%s : Correcting positionMiliseconds %lld to tsbEndMs %lld", __FUNCTION__, positionMiliseconds, tsbEndMs);
					positionMiliseconds = tsbEndMs;
				}
			}
		}
	}

	prevPositionMiliseconds = positionMiliseconds;
	return positionMiliseconds;
}

/**
 * @brief Sends media buffer to sink
 * @param mediaType type of media
 * @param ptr buffer containing media data
 * @param len length of buffer
 * @param fpts pts in seconds
 * @param fdts dts in seconds
 * @param fDuration duration of buffer
 */
void PrivateInstanceAAMP::SendStream(MediaType mediaType, const void *ptr, size_t len, double fpts, double fdts, double fDuration)
{
	profiler.ProfilePerformed(PROFILE_BUCKET_FIRST_BUFFER);
	mStreamSink->Send(mediaType, ptr, len, fpts, fdts, fDuration);
}

/**
 * @brief Sends media buffer to sink
 * @note  Ownership of buffer is transferred.
 * @param mediaType type of media
 * @param buffer - media data
 * @param fpts pts in seconds
 * @param fdts dts in seconds
 * @param fDuration duration of buffer
 */
void PrivateInstanceAAMP::SendStream(MediaType mediaType, GrowableBuffer* buffer, double fpts, double fdts, double fDuration)
{
	profiler.ProfilePerformed(PROFILE_BUCKET_FIRST_BUFFER);
	mStreamSink->Send(mediaType, buffer, fpts, fdts, fDuration);
}

/**
 * @brief Set stream sink
 * @param streamSink pointer of sink object
 */
void PrivateInstanceAAMP::SetStreamSink(StreamSink* streamSink)
{
	mStreamSink = streamSink;
}

/**
 * @brief Check if stream is live
 * @retval true if stream is live, false if not
 */
bool PrivateInstanceAAMP::IsLive()
{
	return mIsLive;
}

/**
 * @brief Stop playback and release resources.
 *
 */
void PrivateInstanceAAMP::Stop()
{
	pthread_mutex_lock(&gMutex);
	for (std::list<gActivePrivAAMP_t>::iterator iter = gActivePrivAAMPs.begin(); iter != gActivePrivAAMPs.end(); iter++)
	{
		if (this == iter->pAAMP)
		{
			if (iter->reTune && mIsRetuneInProgress)
			{
				// Wait for any ongoing re-tune operation to complete
				pthread_cond_wait(&gCond, &gMutex);
			}
			iter->reTune = false;
			break;
		}
	}
	pthread_mutex_unlock(&gMutex);

	DisableDownloads();
	// Stopping the playback, release all DRM context
	if (mpStreamAbstractionAAMP)
	{
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
		if (mDRMSessionManager)
		{
			mDRMSessionManager->setLicenseRequestAbort(true);
		}
#endif
		mpStreamAbstractionAAMP->Stop(true);
	}

	TeardownStream(true);
	pthread_mutex_lock(&mLock);
	if (mPendingAsyncEvents.size() > 0)
	{
		logprintf("PrivateInstanceAAMP::%s() - mPendingAsyncEvents.size - %d", __FUNCTION__, mPendingAsyncEvents.size());
		for (std::map<guint, bool>::iterator it = mPendingAsyncEvents.begin(); it != mPendingAsyncEvents.end(); it++)
		{
			if (it->first != 0)
			{
				if (it->second)
				{
					logprintf("PrivateInstanceAAMP::%s() - remove id - %d", __FUNCTION__, (int) it->first);
					g_source_remove(it->first);
				}
				else
				{
					logprintf("PrivateInstanceAAMP::%s() - Not removing id - %d as not pending", __FUNCTION__, (int) it->first);
				}
			}
		}
		mPendingAsyncEvents.clear();
	}
	if (timedMetadata.size() > 0)
	{
		timedMetadata.clear();
	}

	pthread_mutex_unlock(&mLock);
	seek_pos_seconds = -1;
	culledSeconds = 0;
	durationSeconds = 0;
	rate = 1;
	// Set the state to eSTATE_IDLE
	// directly setting state variable . Calling SetState will trigger event :(
	mState = eSTATE_IDLE;
  
	mSeekOperationInProgress = false;
	mMaxLanguageCount = 0; // reset language count
	mPreferredAudioTrack = AudioTrackInfo();
	mPreferredTextTrack = TextTrackInfo();
	// send signal to any thread waiting for play
	pthread_mutex_lock(&mMutexPlaystart);
	pthread_cond_broadcast(&waitforplaystart);
	pthread_mutex_unlock(&mMutexPlaystart);
	if(mPreCachePlaylistThreadFlag)
	{
		pthread_join(mPreCachePlaylistThreadId,NULL);
		mPreCachePlaylistThreadFlag=false;
		mPreCachePlaylistThreadId = 0;
	}
	getAampCacheHandler()->StopPlaylistCache();


	if (pipeline_paused)
	{
		pipeline_paused = false;
	}

	//temporary hack for peacock
	if (STARTS_WITH_IGNORE_CASE(mAppName.c_str(), "peacock"))
	{
		if (mAampCacheHandler)
		{
			delete mAampCacheHandler;
			mAampCacheHandler = NULL;
		}
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM)
		if (mDRMSessionManager)
		{
			delete mDRMSessionManager;
			mDRMSessionManager = NULL;
		}
#endif
	}

	if(NULL != mCdaiObject)
	{
		delete mCdaiObject;
		mCdaiObject = NULL;
	}

	/* Clear the session data*/
	if(!mSessionToken.empty()){
		mSessionToken.clear();
	}

	EnableDownloads();
}

/**
 * @brief SaveTimedMetadata Function to store Metadata for bulk reporting during Initialization 
 *
 */
void PrivateInstanceAAMP::SaveTimedMetadata(long long timeMilliseconds, const char* szName, const char* szContent, int nb, const char* id, double durationMS)
{
	std::string content(szContent, nb);
	reportMetadata.push_back(TimedMetadata(timeMilliseconds, std::string((szName == NULL) ? "" : szName), content, std::string((id == NULL) ? "" : id), durationMS));
}

/**
 * @brief ReportBulkTimedMetadata Function to send bulk timedMetadata in json format 
 *
 */
void PrivateInstanceAAMP::ReportBulkTimedMetadata()
{
	std::vector<TimedMetadata>::iterator iter;
	if(gpGlobalConfig->enableSubscribedTags && reportMetadata.size())
	{
		AAMPLOG_INFO("%s:%d Sending bulk Timed Metadata",__FUNCTION__,__LINE__);

		cJSON *root;
		cJSON *item;
		root = cJSON_CreateArray();
		if(root)
		{
			for (iter = reportMetadata.begin(); iter != reportMetadata.end(); iter++)
			{
				cJSON_AddItemToArray(root, item = cJSON_CreateObject());
				cJSON_AddStringToObject(item, "name", iter->_name.c_str());
				cJSON_AddStringToObject(item, "id", iter->_id.c_str());
				cJSON_AddNumberToObject(item, "timeMs", iter->_timeMS);
				cJSON_AddNumberToObject (item, "durationMs",iter->_durationMS);
				cJSON_AddStringToObject(item, "data", iter->_content.c_str());
			}

			char* bulkData = cJSON_PrintUnformatted(root);
			if(bulkData)
			{
				BulkTimedMetadataEventPtr eventData = std::make_shared<BulkTimedMetadataEvent>(std::string(bulkData));
				AAMPLOG_INFO("%s:%d:: Sending bulkTimedData", __FUNCTION__, __LINE__);
				if (gpGlobalConfig->logging.logMetadata)
				{
					printf("%s:%d:: bulkTimedData : %s\n", __FUNCTION__, __LINE__, bulkData);
				}
				// Sending BulkTimedMetaData event as synchronous event.
				// SCTE35 events are async events in TimedMetadata, and this event is sending only from HLS
				SendEventSync(eventData);
				free(bulkData);
			}
			cJSON_Delete(root);
		}
	}
}

/**
 * @brief Report TimedMetadata events
 * szName should be the tag name and szContent should be tag value, excluding delimiter ":"
 * @param timeMilliseconds time in milliseconds
 * @param szName name of metadata
 * @param szContent  metadata content
 * @param id - Identifier of the TimedMetadata
 * @param bSyncCall - Sync or Async Event
 * @param durationMS - Duration in milliseconds
 * @param nb unused
 */
void PrivateInstanceAAMP::ReportTimedMetadata(long long timeMilliseconds, const char *szName, const char *szContent,int nb, bool bSyncCall,const char *id, double durationMS)
{
	std::string content(szContent, nb);
	bool bFireEvent = false;

	// Check if timedMetadata was already reported
	std::vector<TimedMetadata>::iterator i;
	bool ignoreMetaAdd = false;
	for (i = timedMetadata.begin(); i != timedMetadata.end(); i++)
	{

		// Add a boundary check of 1 sec for rounding correction
		if ((timeMilliseconds >= i->_timeMS-1000 && timeMilliseconds <= i->_timeMS+1000 ) &&
			(i->_name.compare(szName) == 0)	&&
			(i->_content.compare(content) == 0))
		{
			// Already same exists , ignore
			ignoreMetaAdd = true;
			break;
		}

		if (i->_timeMS < timeMilliseconds)
		{
			// move to next entry
			continue;
		}
		else
		{
			// New entry in between saved entries.
			// i->_timeMS >= timeMilliseconds && no similar entry in table
			timedMetadata.insert(i, TimedMetadata(timeMilliseconds, szName, content, id, durationMS));
			bFireEvent = true;
			ignoreMetaAdd = true;
			break;
		}
	}

	if(!ignoreMetaAdd && i == timedMetadata.end())
	{
		// Comes here for
		// 1.No entry in the table
		// 2.Entries available which is only having time < NewMetatime
		timedMetadata.push_back(TimedMetadata(timeMilliseconds, szName, content, id, durationMS));
		bFireEvent = true;
	}

	if (bFireEvent)
	{
		//DELIA-40019: szContent should not contain any tag name and ":" delimiter. This is not checked in JS event listeners
		TimedMetadataEventPtr eventData = std::make_shared<TimedMetadataEvent>(((szName == NULL) ? "" : szName), ((id == NULL) ? "" : id), timeMilliseconds, durationMS, content);

		if (gpGlobalConfig->logging.logMetadata)
		{
			logprintf("aamp timedMetadata: [%ld] '%s'", (long)(timeMilliseconds), content.c_str());
		}


		if ((eventData->getName() == "SCTE35") || !bSyncCall )
		{
			SendEventAsync(eventData);
		}
		else
		{
			SendEventSync(eventData);
		}
	}
}

/**
 * @brief Notify first frame is displayed. Sends CC handle event to listeners.
 */
void PrivateInstanceAAMP::NotifyFirstFrameReceived()
{
	// If mFirstVideoFrameDisplayedEnabled, state will be changed in NotifyFirstVideoDisplayed()
	if(!mFirstVideoFrameDisplayedEnabled)
	{
		SetState(eSTATE_PLAYING);
	}
	pthread_mutex_lock(&mMutexPlaystart);
	pthread_cond_broadcast(&waitforplaystart);
	pthread_mutex_unlock(&mMutexPlaystart);

	TunedEventConfig tunedEventConfig = IsLive() ? mTuneEventConfigLive : mTuneEventConfigVod;
	if (eTUNED_EVENT_ON_GST_PLAYING == tunedEventConfig)
	{
		// This is an idle callback, so we can sent event synchronously
		if (SendTunedEvent())
		{
			logprintf("aamp: - sent tune event on Tune Completion.");
		}
	}
#ifdef AAMP_STOP_SINK_ON_SEEK
	/*Do not send event on trickplay as CC is not enabled*/
	if (AAMP_NORMAL_PLAY_RATE != rate)
	{
		logprintf("PrivateInstanceAAMP::%s:%d : not sending cc handle as rate = %f", __FUNCTION__, __LINE__, rate);
		return;
	}
#endif
	if (mStreamSink != NULL)
	{
#ifdef AAMP_CC_ENABLED
		if (gpGlobalConfig->nativeCCRendering)
		{
			AampCCManager::GetInstance()->Init((void *)mStreamSink->getCCDecoderHandle());
		}
		else
#endif
		{
			CCHandleEventPtr event = std::make_shared<CCHandleEvent>(mStreamSink->getCCDecoderHandle());
			SendEventSync(event);
		}
	}
}

/**
 * @brief Signal discontinuity of track.
 * Called from StreamAbstractionAAMP to signal discontinuity
 * @param track MediaType of the track
 * @param setDiscontinuityFlag if true then no need to call mStreamSink->Discontinuity(), set only the discontinuity processing flag.
 * @retval true if discontinuity is handled.
 */
bool PrivateInstanceAAMP::Discontinuity(MediaType track, bool setDiscontinuityFlag)
{
	bool ret;

	if (setDiscontinuityFlag)
	{
		ret = true;
	}
	else
	{
		SyncBegin();
		ret = mStreamSink->Discontinuity(track);
		SyncEnd();
	}

	if (ret)
	{
		mProcessingDiscontinuity[track] = true;
	}
	return ret;
}

void PrivateInstanceAAMP::UpdateSubtitleTimestamp()
{
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->UpdateSubtitleTimestamp();
	}
}

/**
 * @brief Schedules retune or discontinuity processing based on state.
 * @param errorType type of playback error
 * @param trackType media type
 */
void PrivateInstanceAAMP::ScheduleRetune(PlaybackErrorType errorType, MediaType trackType)
{
	if (AAMP_NORMAL_PLAY_RATE == rate && ContentType_EAS != mContentType)
	{
		PrivAAMPState state;
		GetState(state);
		if (((state != eSTATE_PLAYING) && (eGST_ERROR_VIDEO_BUFFERING != errorType)) || mSeekOperationInProgress)
		{
			logprintf("PrivateInstanceAAMP::%s:%d: Not processing reTune since state = %d, mSeekOperationInProgress = %d",
						__FUNCTION__, __LINE__, state, mSeekOperationInProgress);
			return;
		}

		/*If underflow is caused by a discontinuity processing, continue playback from discontinuity*/
		// If discontinuity process in progress, skip further processing
		// DELIA-46559 Since discontinuity flags are reset a bit earlier, additional checks added below to check if discontinuity processing in progress
		pthread_mutex_lock(&mLock);
		if (IsDiscontinuityProcessPending() || mDiscontinuityTuneOperationId != 0 || mDiscontinuityTuneOperationInProgress)
		{
			if (mDiscontinuityTuneOperationId != 0 || mDiscontinuityTuneOperationInProgress)
			{
				pthread_mutex_unlock(&mLock);
				logprintf("PrivateInstanceAAMP::%s:%d: Discontinuity Tune handler already spawned(%d) or inprogress(%d)",
					__FUNCTION__, __LINE__, mDiscontinuityTuneOperationId, mDiscontinuityTuneOperationInProgress);
				return;
			}
			mDiscontinuityTuneOperationId = g_idle_add(PrivateInstanceAAMP_ProcessDiscontinuity, (gpointer) this);
			pthread_mutex_unlock(&mLock);

			logprintf("PrivateInstanceAAMP::%s:%d: Underflow due to discontinuity handled", __FUNCTION__, __LINE__);
			return;
		}
		pthread_mutex_unlock(&mLock);

		if (mpStreamAbstractionAAMP && mpStreamAbstractionAAMP->IsStreamerStalled())
		{
			logprintf("PrivateInstanceAAMP::%s:%d: Ignore reTune due to playback stall", __FUNCTION__, __LINE__);
			return;
		}
		else if (!gpGlobalConfig->internalReTune)
		{
			logprintf("PrivateInstanceAAMP::%s:%d: Ignore reTune as disabled in configuration", __FUNCTION__, __LINE__);
			return;
		}

		if((gpGlobalConfig->reportBufferEvent) && (errorType == eGST_ERROR_UNDERFLOW) && (trackType == eMEDIATYPE_VIDEO))
		{
			SendBufferChangeEvent(true);  // Buffer state changed, buffer Under flow started
			if ( false == pipeline_paused )
			{
				if ( true != PausePipeline(true, true) )
				{
					AAMPLOG_ERR("%s(): Failed to pause the Pipeline", __FUNCTION__);
				}
			}
		}

		const char* errorString  =  (errorType == eGST_ERROR_PTS) ? "PTS ERROR" :
									(errorType == eGST_ERROR_UNDERFLOW) ? "Underflow" :
									(errorType == eSTALL_AFTER_DISCONTINUITY) ? "Stall After Discontinuity" :
									(errorType == eGST_ERROR_GST_PIPELINE_INTERNAL) ? "GstPipeline Internal Error" : "STARTTIME RESET";

		SendAnomalyEvent(ANOMALY_WARNING, "%s %s", (trackType == eMEDIATYPE_VIDEO ? "VIDEO" : "AUDIO"), errorString);
		bool activeAAMPFound = false;
		pthread_mutex_lock(&gMutex);
		for (std::list<gActivePrivAAMP_t>::iterator iter = gActivePrivAAMPs.begin(); iter != gActivePrivAAMPs.end(); iter++)
		{
			if (this == iter->pAAMP)
			{
				gActivePrivAAMP_t *gAAMPInstance = &(*iter);
				if (gAAMPInstance->reTune)
				{
					logprintf("PrivateInstanceAAMP::%s:%d: Already scheduled", __FUNCTION__, __LINE__);
				}
				else
				{

					if(eGST_ERROR_PTS == errorType || eGST_ERROR_UNDERFLOW == errorType)
					{
						long long now = aamp_GetCurrentTimeMS();
						long long lastErrorReportedTimeMs = lastUnderFlowTimeMs[trackType];
						if (lastErrorReportedTimeMs)
						{
							long long diffMs = (now - lastErrorReportedTimeMs);
							if (diffMs < AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS)
							{
								gAAMPInstance->numPtsErrors++;
								logprintf("PrivateInstanceAAMP::%s:%d: numPtsErrors %d, ptsErrorThreshold %d",
									__FUNCTION__, __LINE__, gAAMPInstance->numPtsErrors, gpGlobalConfig->ptsErrorThreshold);
								if (gAAMPInstance->numPtsErrors >= gpGlobalConfig->ptsErrorThreshold)
								{
									gAAMPInstance->numPtsErrors = 0;
									gAAMPInstance->reTune = true;
									logprintf("PrivateInstanceAAMP::%s:%d: Schedule Retune. diffMs %lld < threshold %lld",
										__FUNCTION__, __LINE__, diffMs, AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS);
									g_idle_add(PrivateInstanceAAMP_Retune, (gpointer)this);
								}
							}
							else
							{
								gAAMPInstance->numPtsErrors = 0;
								logprintf("PrivateInstanceAAMP::%s:%d: Not scheduling reTune since (diff %lld > threshold %lld) numPtsErrors %d, ptsErrorThreshold %d.",
									__FUNCTION__, __LINE__, diffMs, AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS,
									gAAMPInstance->numPtsErrors, gpGlobalConfig->ptsErrorThreshold);
							}
						}
						else
						{
							gAAMPInstance->numPtsErrors = 0;
							logprintf("PrivateInstanceAAMP::%s:%d: Not scheduling reTune since first %s.", __FUNCTION__, __LINE__, errorString);
						}
						lastUnderFlowTimeMs[trackType] = now;
					}
					else
					{
						logprintf("PrivateInstanceAAMP::%s:%d: Schedule Retune errorType %d error %s", __FUNCTION__, __LINE__, errorType, errorString);
						gAAMPInstance->reTune = true;
						g_idle_add(PrivateInstanceAAMP_Retune, (gpointer) this);
					}
				}
				activeAAMPFound = true;
				break;
			}
		}
		pthread_mutex_unlock(&gMutex);
		if (!activeAAMPFound)
		{
			logprintf("PrivateInstanceAAMP::%s:%d: %p not in Active AAMP list", __FUNCTION__, __LINE__, this);
		}
	}
}

/**
 * @brief Sets aamp state
 * @param state state to be set
 */
void PrivateInstanceAAMP::SetState(PrivAAMPState state)
{
	bool sentSync = true;

	if (mState == state)
	{ // noop
		return;
	}

	if (0 == aamp_GetSourceID())
	{
		sentSync = false;
	}

	if ( (state == eSTATE_PLAYING || state == eSTATE_BUFFERING || state == eSTATE_PAUSED)
		 && mState == eSTATE_SEEKING && (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_SEEKED]))
	{
		SeekedEventPtr event = std::make_shared<SeekedEvent>(GetPositionMilliseconds());
		if (sentSync)
		{
			SendEventSync(event);
		}
		else
		{
			SendEventAsync(event);
		}
	}

	pthread_mutex_lock(&mLock);
	mState = state;
	pthread_mutex_unlock(&mLock);

	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_STATE_CHANGED])
	{
		if (mState == eSTATE_PREPARING)
		{
			StateChangedEventPtr eventData = std::make_shared<StateChangedEvent>(eSTATE_INITIALIZED);
			if (sentSync)
			{
				SendEventSync(eventData);
			}
			else
			{
				SendEventAsync(eventData);
			}
		}

		StateChangedEventPtr eventData = std::make_shared<StateChangedEvent>(mState);
		if (sentSync)
		{
			SendEventSync(eventData);
		}
		else
		{
			SendEventAsync(eventData);
		}
	}
}

/**
 * @brief Get aamp state
 * @param[out] state current state of aamp
 */
void PrivateInstanceAAMP::GetState(PrivAAMPState& state)
{
	pthread_mutex_lock(&mLock);
	state = mState;
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief Add idle task
 * @note task shall return 0 to be removed, 1 to be repeated
 * @param task task function pointer
 * @param arg passed as parameter during idle task execution
 */
void PrivateInstanceAAMP::AddIdleTask(IdleTask task, void* arg)
{
	g_idle_add(task, (gpointer)arg);
}

/**
 * @brief Add high priority idle task
 *
 * @note task shall return 0 to be removed, 1 to be repeated
 *
 * @param[in] task task function pointer
 * @param[in] arg passed as parameter during idle task execution
 */
gint PrivateInstanceAAMP::AddHighIdleTask(IdleTask task, void* arg,DestroyTask dtask)
{
	gint callbackID = g_idle_add_full(G_PRIORITY_HIGH_IDLE, task, (gpointer)arg, dtask);
	return callbackID;
}

/**
 * @brief Check if first frame received or not
 * @retval true if the first frame received
 */
bool PrivateInstanceAAMP::IsFirstFrameReceived(void)
{
	return mStreamSink->IsFirstFrameReceived();
}

/**
 * @brief Check if sink cache is empty
 * @param mediaType type of track
 * @retval true if sink cache is empty
 */
bool PrivateInstanceAAMP::IsSinkCacheEmpty(MediaType mediaType)
{
	return mStreamSink->IsCacheEmpty(mediaType);
}

/**
 * @brief Notification on completing fragment caching
 */
void PrivateInstanceAAMP::NotifyFragmentCachingComplete()
{
	pthread_mutex_lock(&mFragmentCachingLock);
	mFragmentCachingRequired = false;
	mStreamSink->NotifyFragmentCachingComplete();
	PrivAAMPState state;
	GetState(state);
	if (state == eSTATE_BUFFERING)
	{
		if(mpStreamAbstractionAAMP)
		{
			mpStreamAbstractionAAMP->NotifyPlaybackPaused(false);
		}
		SetState(eSTATE_PLAYING);
	}
	pthread_mutex_unlock(&mFragmentCachingLock);
}

/**
 * @brief Send tuned event to listeners if required
 * @retval true if event is scheduled, false if discarded
 */
bool PrivateInstanceAAMP::SendTunedEvent(bool isSynchronous)
{
	bool ret = false;

	// Required for synchronising btw audio and video tracks in case of cdmidecryptor
	pthread_mutex_lock(&mLock);

	ret = mTunedEventPending;
	mTunedEventPending = false;

	pthread_mutex_unlock(&mLock);

	if(ret)
	{
		AAMPEventPtr ev = std::make_shared<AAMPEventObject>(AAMP_EVENT_TUNED);
		if (isSynchronous)
		{
			SendEventSync(ev);
		}
		else
		{
			SendEventAsync(ev);
		}
	}
	return ret;
}

/**
 *   @brief Send VideoEndEvent
 *
 *   @return success or failure
 */
bool PrivateInstanceAAMP::SendVideoEndEvent()
{
	bool ret = false;
#ifdef SESSION_STATS
	char * strVideoEndJson = NULL;
	// Required for protecting mVideoEnd object
	pthread_mutex_lock(&mLock);
	if(mVideoEnd)
	{
		//Update VideoEnd Data
		if(mTimeAtTopProfile > 0)
		{
			// Losing milisecons of data in conversion from double to long
			mVideoEnd->SetTimeAtTopProfile(mTimeAtTopProfile);
			mVideoEnd->SetTimeToTopProfile(mTimeToTopProfile);
		}
		mVideoEnd->SetTotalDuration(mPlaybackDuration);

		// re initialize for next tune collection
		mTimeToTopProfile = 0;
		mTimeAtTopProfile = 0;
		mPlaybackDuration = 0;
		mCurrentLanguageIndex = 0;

		//Memory of this string will be deleted after sending event by destructor of AsyncMetricsEventDescriptor
		if(mTSBEnabled)
		{
			char* data = GetOnVideoEndSessionStatData();
			if(data)
			{
				AAMPLOG_INFO("TsbSessionEnd:%s", data);
				strVideoEndJson = mVideoEnd->ToJsonString(data);
				aamp_Free(&data);
			}
		}
		else
		{
			strVideoEndJson = mVideoEnd->ToJsonString();
		}

		delete mVideoEnd;
	}
	
	mVideoEnd = new CVideoStat();
#ifdef USE_OPENCDM // AampOutputProtection is compiled when this  flag is enabled 
	//Collect Display resoluation and store in videoEndObject, TBD: If DisplayResolution changes during playback, its not taken care. not in scope for now. 
	int iDisplayWidth = 0 , iDisplayHeight = 0;
	AampOutputProtection::GetAampOutputProcectionInstance()->GetDisplayResolution(iDisplayWidth,iDisplayHeight);
	mVideoEnd->SetDisplayResolution(iDisplayWidth,iDisplayHeight);
#endif 
	pthread_mutex_unlock(&mLock);

	if(strVideoEndJson)
	{
		AAMPLOG_INFO("VideoEnd:%s", strVideoEndJson);
		MetricsDataEventPtr e = std::make_shared<MetricsDataEvent>(MetricsDataType::AAMP_DATA_VIDEO_END, this->mTraceUUID, strVideoEndJson);
		SendEventAsync(e);
		free(strVideoEndJson);
		ret = true;
	}
#endif
	return ret;
}

/**   @brief updates  profile Resolution to VideoStat object
 *
 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
 *   @param[in]  bitrate - bitrate ( bits per sec )
 *   @param[in]  width - Frame width
 *   @param[in]  Height - Frame Height
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndProfileResolution(MediaType mediaType, long bitrate, int width, int height)
{
#ifdef SESSION_STATS
	if(gpGlobalConfig->mEnableVideoEndEvent) // avoid mutex mLock lock if disabled.
	{
		pthread_mutex_lock(&mLock);
		if(mVideoEnd)
		{
			VideoStatTrackType trackType = VideoStatTrackType::STAT_VIDEO;
			if(mediaType == eMEDIATYPE_IFRAME)
			{
				trackType = VideoStatTrackType::STAT_IFRAME;
			}
			mVideoEnd->SetProfileResolution(trackType,bitrate,width,height);
		}
		pthread_mutex_unlock(&mLock);
	}
#endif
}

/**
 *   @brief updates download metrics to VideoStat object, this is used for VideoFragment as it takes duration for calcuation purpose.
 *
 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
 *   @param[in]  bitrate - bitrate ( bits per sec )
 *   @param[in]  curlOrHTTPErrorCode - download curl or http error
 *   @param[in]  strUrl :  URL in case of faulures
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double duration, double curlDownloadTime)
{
    UpdateVideoEndMetrics(mediaType, bitrate, curlOrHTTPCode, strUrl,duration,curlDownloadTime, false,false);
}

/**
 *   @brief updates time shift buffer status
 *
 *   @param[in]  btsbAvailable - true if TSB supported
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndTsbStatus(bool btsbAvailable)
{
#ifdef SESSION_STATS
	if(gpGlobalConfig->mEnableVideoEndEvent) // avoid mutex mLock lock if disabled.
	{
		pthread_mutex_lock(&mLock);
		if(mVideoEnd)
		{

			mVideoEnd->SetTsbStatus(btsbAvailable);
		}
		pthread_mutex_unlock(&mLock);
	}
#endif
}   

/**
 *   @brief updates download metrics to VideoStat object, this is used for VideoFragment as it takes duration for calcuation purpose.
 *
 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
 *   @param[in]  bitrate - bitrate ( bits per sec )
 *   @param[in]  curlOrHTTPErrorCode - download curl or http error
 *   @param[in]  strUrl :  URL in case of faulures
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double duration, double curlDownloadTime, bool keyChanged, bool isEncrypted)
{
#ifdef SESSION_STATS
	if(gpGlobalConfig->mEnableVideoEndEvent)
	{
		int audioIndex = 1;
		// ignore for write and aborted errors
		// these are generated after trick play options,
		if( curlOrHTTPCode > 0 &&  !(curlOrHTTPCode == CURLE_ABORTED_BY_CALLBACK || curlOrHTTPCode == CURLE_WRITE_ERROR) )
		{
			VideoStatDataType dataType = VideoStatDataType::VE_DATA_UNKNOWN;

			VideoStatTrackType trackType = VideoStatTrackType::STAT_UNKNOWN;
			VideoStatCountType eCountType = VideoStatCountType::COUNT_UNKNOWN;

		/*	COUNT_UNKNOWN,
			COUNT_LIC_TOTAL,
			COUNT_LIC_ENC_TO_CLR,
			COUNT_LIC_CLR_TO_ENC,
			COUNT_STALL,
		*/

			switch(mediaType)
			{
				case eMEDIATYPE_MANIFEST:
				{
					dataType = VideoStatDataType::VE_DATA_MANIFEST;
					trackType = VideoStatTrackType::STAT_MAIN;
				}
					break;

				case eMEDIATYPE_PLAYLIST_VIDEO:
				{
					dataType = VideoStatDataType::VE_DATA_MANIFEST;
					trackType = VideoStatTrackType::STAT_VIDEO;
				}
					break;

				case eMEDIATYPE_PLAYLIST_AUDIO:
				{
					dataType = VideoStatDataType::VE_DATA_MANIFEST;
					trackType = VideoStatTrackType::STAT_AUDIO;
					audioIndex += mCurrentLanguageIndex;
				}
					break;

				case eMEDIATYPE_PLAYLIST_IFRAME:
				{
					dataType = VideoStatDataType::VE_DATA_MANIFEST;
					trackType = STAT_IFRAME;
				}
					break;

				case eMEDIATYPE_VIDEO:
				{
					dataType = VideoStatDataType::VE_DATA_FRAGMENT;
					trackType = VideoStatTrackType::STAT_VIDEO;
					// always Video fragment will be from same thread so mutex required

					// !!!!!!!!!! To Do : Support this stats for Audio Only streams !!!!!!!!!!!!!!!!!!!!!
					//Is success
					if (((curlOrHTTPCode == 200) || (curlOrHTTPCode == 206))  && duration > 0)
					{
						if(mpStreamAbstractionAAMP->GetProfileCount())
						{
							long maxBitrateSupported = mpStreamAbstractionAAMP->GetMaxBitrate();
							if(maxBitrateSupported == bitrate)
							{
								mTimeAtTopProfile += duration;
	
							}
	
						}
						if(mTimeAtTopProfile == 0) // we havent achived top profile yet
						{
							mTimeToTopProfile += duration; // started at top profile
						}

						mPlaybackDuration += duration;
					}

				}
					break;
				case eMEDIATYPE_AUDIO:
				{
					dataType = VideoStatDataType::VE_DATA_FRAGMENT;
					trackType = VideoStatTrackType::STAT_AUDIO;
					audioIndex += mCurrentLanguageIndex;
				}
					break;
				case eMEDIATYPE_IFRAME:
				{
					dataType = VideoStatDataType::VE_DATA_FRAGMENT;
					trackType = VideoStatTrackType::STAT_IFRAME;
				}
					break;

				case eMEDIATYPE_INIT_IFRAME:
				{
					dataType = VideoStatDataType::VE_DATA_INIT_FRAGMENT;
					trackType = VideoStatTrackType::STAT_IFRAME;
				}
					break;

				case eMEDIATYPE_INIT_VIDEO:
				{
					dataType = VideoStatDataType::VE_DATA_INIT_FRAGMENT;
					trackType = VideoStatTrackType::STAT_VIDEO;
				}
					break;

				case eMEDIATYPE_INIT_AUDIO:
				{
					dataType = VideoStatDataType::VE_DATA_INIT_FRAGMENT;
					trackType = VideoStatTrackType::STAT_AUDIO;
					audioIndex += mCurrentLanguageIndex;
				}
					break;
				default:
					break;
			}


			// Required for protecting mVideoStat object
			if( dataType != VideoStatDataType::VE_DATA_UNKNOWN
					&& trackType != VideoStatTrackType::STAT_UNKNOWN)
			{
				pthread_mutex_lock(&mLock);
				if(mVideoEnd)
				{
					mVideoEnd->Increment_Data(dataType,trackType,bitrate,curlDownloadTime,curlOrHTTPCode,false,audioIndex);
					if((curlOrHTTPCode != 200) && (curlOrHTTPCode != 206) && strUrl.c_str())
					{
						//set failure url
						mVideoEnd->SetFailedFragmentUrl(trackType,bitrate,strUrl);
					}
					if(dataType == VideoStatDataType::VE_DATA_FRAGMENT)
					{
						mVideoEnd->Record_License_EncryptionStat(trackType,isEncrypted,keyChanged);
					}
				}
				pthread_mutex_unlock(&mLock);

			}
			else
			{
				AAMPLOG_INFO("PrivateInstanceAAMP::%s - Could Not update VideoEnd Event dataType:%d trackType:%d response:%d", __FUNCTION__,
						dataType,trackType,curlOrHTTPCode);
			}
		}
	}
#endif
}

/**
 *   @brief updates abr metrics to VideoStat object,
 *
 *   @param[in]  AAMPAbrInfo - abr info
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndMetrics(AAMPAbrInfo & info)
{
#ifdef SESSION_STATS
	if(gpGlobalConfig->mEnableVideoEndEvent)
	{
		//only for Ramp down case
		if(info.desiredProfileIndex < info.currentProfileIndex)
		{
			AAMPLOG_INFO("UpdateVideoEnd:abrinfo currIdx:%d desiredIdx:%d for:%d",  info.currentProfileIndex,info.desiredProfileIndex,info.abrCalledFor);

			if(info.abrCalledFor == AAMPAbrType::AAMPAbrBandwidthUpdate)
			{
				pthread_mutex_lock(&mLock);
				if(mVideoEnd)
				{
					mVideoEnd->Increment_NetworkDropCount();
				}
				pthread_mutex_unlock(&mLock);
			}
			else if (info.abrCalledFor == AAMPAbrType::AAMPAbrFragmentDownloadFailed
					|| info.abrCalledFor == AAMPAbrType::AAMPAbrFragmentDownloadFailed)
			{
				pthread_mutex_lock(&mLock);
				if(mVideoEnd)
				{
					mVideoEnd->Increment_ErrorDropCount();
				}
				pthread_mutex_unlock(&mLock);
			}
		}
	}
#endif
}

/**
 *   @brief updates download metrics to VideoEnd object,
 *
 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
 *   @param[in]  bitrate - bitrate
 *   @param[in]  curlOrHTTPErrorCode - download curl or http error
 *   @param[in]  strUrl :  URL in case of faulures
 *   @return void
 */
void PrivateInstanceAAMP::UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double curlDownloadTime)
{
	UpdateVideoEndMetrics(mediaType, bitrate, curlOrHTTPCode, strUrl,0,curlDownloadTime, false, false);
}

/**
 *   @brief Check if fragment caching is required
 *
 *   @return true if required or ongoing, false if not needed
 */
bool PrivateInstanceAAMP::IsFragmentCachingRequired()
{
	//Prevent enabling Fragment Caching during Seek While Pause
	return (!mPauseOnFirstVideoFrameDisp && mFragmentCachingRequired);
}

/**
 * @brief Get video display's width and height
 * @param width
 * @param height
 */
void PrivateInstanceAAMP::GetPlayerVideoSize(int &width, int &height)
{
	mStreamSink->GetVideoSize(width, height);
}

/**
 * @brief Set an idle callback to dispatched state
 * @param id Idle task Id
 */
void PrivateInstanceAAMP::SetCallbackAsDispatched(guint id)
{
	pthread_mutex_lock(&mLock);
	std::map<guint, bool>::iterator  itr = mPendingAsyncEvents.find(id);
	if(itr != mPendingAsyncEvents.end())
	{
		assert (itr->second);
		mPendingAsyncEvents.erase(itr);
	}
	else
	{
		logprintf("%s:%d id not in mPendingAsyncEvents, insert and mark as not pending", __FUNCTION__, __LINE__, id);
		mPendingAsyncEvents[id] = false;
	}
	pthread_mutex_unlock(&mLock);
}

/**
 * @brief Set an idle callback to pending state
 * @param id Idle task Id
 */
void PrivateInstanceAAMP::SetCallbackAsPending(guint id)
{
	pthread_mutex_lock(&mLock);
	std::map<guint, bool>::iterator  itr = mPendingAsyncEvents.find(id);
	if(itr != mPendingAsyncEvents.end())
	{
		assert (!itr->second);
		logprintf("%s:%d id already in mPendingAsyncEvents and completed, erase it", __FUNCTION__, __LINE__, id);
		mPendingAsyncEvents.erase(itr);
	}
	else
	{
		mPendingAsyncEvents[id] = true;
	}
	pthread_mutex_unlock(&mLock);
}

/**
 *   @brief Add/Remove a custom HTTP header and value.
 *
 *   @param  headerName - Name of custom HTTP header
 *   @param  headerValue - Value to be pased along with HTTP header.
 *   @param  isLicenseHeader - true, if header is to be used for a license request.
 */
void PrivateInstanceAAMP::AddCustomHTTPHeader(std::string headerName, std::vector<std::string> headerValue, bool isLicenseHeader)
{
	// Header name should be ending with :
	if(headerName.back() != ':')
	{
		headerName += ':';
	}

	if (isLicenseHeader)
	{
		if (headerValue.size() != 0)
		{
			mCustomLicenseHeaders[headerName] = headerValue;
		}
		else
		{
			mCustomLicenseHeaders.erase(headerName);
		}
	}
	else
	{
		if (headerValue.size() != 0)
		{
			mCustomHeaders[headerName] = headerValue;
		}
		else
		{
			mCustomHeaders.erase(headerName);
		}
	}
}

/**
 *   @brief Set License Server URL.
 *
 *   @param  url - URL of the server to be used for license requests
 *   @param  type - DRM Type(PR/WV) for which the server URL should be used, global by default
 */
void PrivateInstanceAAMP::SetLicenseServerURL(const char *url, DRMSystems type)
{
	// Local aamp.cfg config trumps JS PP config
	if (gpGlobalConfig->licenseServerLocalOverride)
	{
		AAMPLOG_INFO("PrivateInstanceAAMP::%s - aamp cfg overrides, so license url - %s for type - %d is ignored", __FUNCTION__, url, type);
		return;
	}

	if (type == eDRM_MAX_DRMSystems)
	{

		mLicenseServerUrls[eDRM_MAX_DRMSystems] = std::string(url);
	}
	else if (type == eDRM_PlayReady || type == eDRM_WideVine || type == eDRM_ClearKey)
	{
		mLicenseServerUrls[type] = std::string(url);
	}
	else
	{
		AAMPLOG_ERR("PrivateInstanceAAMP::%s - invalid drm type(%d) received.", __FUNCTION__, type);
		return;
	}

	AAMPLOG_INFO("PrivateInstanceAAMP::%s - set license url - %s for type - %d", __FUNCTION__, url, type);
}

/**
 *   @brief Indicates if session token has to be used with license request or not.
 *
 *   @param  isAnonymous - True if session token should be blank and false otherwise.
 */
void PrivateInstanceAAMP::SetAnonymousRequest(bool isAnonymous)
{
	gpGlobalConfig->licenseAnonymousRequest = isAnonymous;
}

/**
 *   @brief Indicates average BW to be used for ABR Profiling.
 *
 *   @param  useAvgBW - Flag for true / false
 */
void PrivateInstanceAAMP::SetAvgBWForABR(bool useAvgBW)
{
	mUseAvgBandwidthForABR = useAvgBW;
}

/**
 *   @brief Set Max TimeWindow for PreCaching Playlist
 *
 *   @param  maxTime - Time for PreCaching in Minutes
 */
void PrivateInstanceAAMP::SetPreCacheTimeWindow(int nTimeWindow)
{
	if(nTimeWindow > 0)
	{
		mPreCacheDnldTimeWindow = nTimeWindow;
		AAMPLOG_WARN("%s Playlist PreCaching enabled with timewindow:%d",__FUNCTION__,nTimeWindow);
	}
}

/**
 *   @brief Set VOD Trickplay FPS.
 *
 *   @param  vodTrickplayFPS - FPS to be used for VOD Trickplay
 */
void PrivateInstanceAAMP::SetVODTrickplayFPS(int vodTrickplayFPS)
{
	// Local aamp.cfg config trumps JS PP config
	if (gpGlobalConfig->vodTrickplayFPSLocalOverride)
	{
		return;
	}
	gpGlobalConfig->vodTrickplayFPS = vodTrickplayFPS;
	logprintf("PrivateInstanceAAMP::%s(), vodTrickplayFPS %d", __FUNCTION__, vodTrickplayFPS);
}

/**
 *   @brief Set Linear Trickplay FPS.
 *
 *   @param  linearTrickplayFPS - FPS to be used for Linear Trickplay
 */
void PrivateInstanceAAMP::SetLinearTrickplayFPS(int linearTrickplayFPS)
{
	// Local aamp.cfg config trumps JS PP config
	if (gpGlobalConfig->linearTrickplayFPSLocalOverride)
	{
		return;
	}
	gpGlobalConfig->linearTrickplayFPS = linearTrickplayFPS;
	logprintf("PrivateInstanceAAMP::%s(), linearTrickplayFPS %d", __FUNCTION__, linearTrickplayFPS);
}

/**
 *   @brief Set live offset [Sec]
 *
 *   @param SetLiveOffset - Live Offset
 */
void PrivateInstanceAAMP::SetLiveOffset(int liveoffset)
{
	if(liveoffset > 0 )
	{
		mLiveOffset = liveoffset;
		mNewLiveOffsetflag = true;
		logprintf("PrivateInstanceAAMP::%s(), liveoffset %d", __FUNCTION__, liveoffset);
	}
	else
	{
		logprintf("PrivateInstanceAAMP::%s(), liveoffset beyond limits %d", __FUNCTION__, liveoffset);
	}
}

/**
 *   @brief To set the error code to be used for playback stalled error.
 *
 *   @param  errorCode - error code for playback stall errors.
 */
void PrivateInstanceAAMP::SetStallErrorCode(int errorCode)
{
	gpGlobalConfig->stallErrorCode = errorCode;
}

/**
 *   @brief To set the timeout value to be used for playback stall detection.
 *
 *   @param  timeoutMS - timeout in milliseconds for playback stall detection.
 */
void PrivateInstanceAAMP::SetStallTimeout(int timeoutMS)
{
	gpGlobalConfig->stallTimeoutInMS = timeoutMS;
}

/**
 *   @brief To set the Playback Position reporting interval.
 *
 *   @param  reportIntervalMS - playback reporting interval in milliseconds.
 */
void PrivateInstanceAAMP::SetReportInterval(int reportIntervalMS)
{
	if(gpGlobalConfig->reportProgressInterval != 0)
	{
		mReportProgressInterval = gpGlobalConfig->reportProgressInterval;
	}
	else
	{
		mReportProgressInterval = reportIntervalMS;
	}
	AAMPLOG_WARN("%s Progress Interval configured %d",__FUNCTION__,mReportProgressInterval);		
}

/**
 *   @brief To set the max retry attempts for init frag curl timeout failures
 *
 *   @param  count - max attempt for timeout retry count
 */
void PrivateInstanceAAMP::SetInitFragTimeoutRetryCount(int count)
{
	if(-1 == gpGlobalConfig->initFragmentRetryCount)
	{
		mInitFragmentRetryCount = count;
		AAMPLOG_WARN("%s Init frag timeout retry count configured %d", __FUNCTION__, mInitFragmentRetryCount);
	}
}

/**
 * @brief Send stalled event to listeners
 */
void PrivateInstanceAAMP::SendStalledErrorEvent(bool isStalledBeforePlay)
{
	char* errorDesc = NULL;
	char description[MAX_ERROR_DESCRIPTION_LENGTH];
	memset(description, '\0', MAX_ERROR_DESCRIPTION_LENGTH);

	if (IsTSBSupported() && !mFogErrorString.empty())
	{
		snprintf(description, (MAX_ERROR_DESCRIPTION_LENGTH - 1), "%s", mFogErrorString.c_str());
		errorDesc = description;
	}
	else if (!isStalledBeforePlay)
	{
		snprintf(description, (MAX_ERROR_DESCRIPTION_LENGTH - 1), "Playback has been stalled for more than %d ms due to lack of new fragments", gpGlobalConfig->stallTimeoutInMS);
		errorDesc = description;
	}

	SendErrorEvent(AAMP_TUNE_PLAYBACK_STALLED, errorDesc);
}

/**
 * @brief Notifiy first buffer is processed
 */
void PrivateInstanceAAMP::NotifyFirstBufferProcessed()
{
	// If mFirstVideoFrameDisplayedEnabled, state will be changed in NotifyFirstVideoDisplayed()
	PrivAAMPState state;
	GetState(state);
	if (!mFirstVideoFrameDisplayedEnabled
			&& state == eSTATE_SEEKING)
	{
		SetState(eSTATE_PLAYING);
	}
	trickStartUTCMS = aamp_GetCurrentTimeMS();
	logprintf("%s:%d : seek pos %.3f", __FUNCTION__, __LINE__, seek_pos_seconds);
	UpdateSubtitleTimestamp();
}

/**
 * @brief Update audio language selection
 * @param lang string corresponding to language
 * @param checkBeforeOverwrite - if set will do additional checks before overwriting user setting
 */
void PrivateInstanceAAMP::UpdateAudioLanguageSelection(const char *lang, bool checkBeforeOverwrite)
{
	bool overwriteSetting = true;
	if (checkBeforeOverwrite)
	{
		// Check if the user provided language is present in the available language list
		// in which case this is just temporary and no need to overwrite user settings
		for (int cnt = 0; cnt < mMaxLanguageCount; cnt++)
		{
			if(strncmp(mLanguageList[cnt], language, MAX_LANGUAGE_TAG_LENGTH) == 0)
			{
				overwriteSetting = false;
				break;
			}
		}

	}

	if (overwriteSetting)
	{
		if (strncmp(language, lang, MAX_LANGUAGE_TAG_LENGTH) != 0)
		{
			AAMPLOG_WARN("%s:%d Update audio language from (%s) -> (%s)", __FUNCTION__, __LINE__, language, lang);
			strncpy(language, lang, MAX_LANGUAGE_TAG_LENGTH);
			language[MAX_LANGUAGE_TAG_LENGTH-1] = '\0';
		}
	}
	noExplicitUserLanguageSelection = false;

	for (int cnt=0; cnt < mMaxLanguageCount; cnt ++)
	{
		if(strncmp(mLanguageList[cnt],lang,MAX_LANGUAGE_TAG_LENGTH) == 0)
		{
			mCurrentLanguageIndex = cnt; // needed?
			break;
		}
	}
}

/**
 * @brief Update subtitle language selection
 * @param lang string corresponding to language
 */
void PrivateInstanceAAMP::UpdateSubtitleLanguageSelection(const char *lang)
{
	strncpy(mSubLanguage, lang, MAX_LANGUAGE_TAG_LENGTH);
	language[MAX_LANGUAGE_TAG_LENGTH-1] = '\0';
}

/**
 * @brief Get current stream type
 * @retval 10 - HLS/Clear
 * @retval 11 - HLS/Consec
 * @retval 12 - HLS/Access
 * @retval 13 - HLS/Vanilla AES
 * @retval 20 - DASH/Clear
 * @retval 21 - DASH/WV
 * @retval 22 - DASH/PR
 */
int PrivateInstanceAAMP::getStreamType()
{

	int type = 0;

	if(mMediaFormat == eMEDIAFORMAT_DASH)
	{
		type = 20;
	}
	else if( mMediaFormat == eMEDIAFORMAT_HLS)
	{
		type = 10;
	}
	else if (mMediaFormat == eMEDIAFORMAT_PROGRESSIVE)// eMEDIAFORMAT_PROGRESSIVE
	{
		type = 30;
	}
	else if (mMediaFormat == eMEDIAFORMAT_HLS_MP4)
	{
		type = 40;
	}
	else
	{
		type = 0;
	}

	if (mCurrentDrm != nullptr) {
		type += mCurrentDrm->getDrmCodecType();
	}
	return type;
}

#ifdef USE_SECCLIENT
/**
 * @brief GetMoneyTraceString - Extracts / Generates MoneyTrace string
 * @param[out] customHeader - Generated moneytrace is stored
 *
 * @retval None
*/
void PrivateInstanceAAMP::GetMoneyTraceString(std::string &customHeader) const
{
	char moneytracebuf[512];
	memset(moneytracebuf, 0, sizeof(moneytracebuf));

	if (mCustomHeaders.size() > 0)
	{
		for (std::unordered_map<std::string, std::vector<std::string>>::const_iterator it = mCustomHeaders.begin();
			it != mCustomHeaders.end(); it++)
		{
			if (it->first.compare("X-MoneyTrace:") == 0)
			{
				if (it->second.size() >= 2)
				{
					snprintf(moneytracebuf, sizeof(moneytracebuf), "trace-id=%s;parent-id=%s;span-id=%lld",
					(const char*)it->second.at(0).c_str(),
					(const char*)it->second.at(1).c_str(),
					aamp_GetCurrentTimeMS());
				}
				else if (it->second.size() == 1)
				{
					snprintf(moneytracebuf, sizeof(moneytracebuf), "trace-id=%s;parent-id=%lld;span-id=%lld",
						(const char*)it->second.at(0).c_str(),
						aamp_GetCurrentTimeMS(),
						aamp_GetCurrentTimeMS());
				}
				customHeader.append(moneytracebuf);
				break;
			}
		}
	}
	// No money trace is available in customheader from JS , create a new moneytrace locally
	if(customHeader.size() == 0)
	{
		// No Moneytrace info available in tune data 
		logprintf("No Moneytrace info available in tune request,need to generate one");
		uuid_t uuid;
		uuid_generate(uuid);
		char uuidstr[128];
		uuid_unparse(uuid, uuidstr);
		for (char *ptr = uuidstr; *ptr; ++ptr) {
			*ptr = tolower(*ptr);
		}
		snprintf(moneytracebuf,sizeof(moneytracebuf),"trace-id=%s;parent-id=%lld;span-id=%lld",uuidstr,aamp_GetCurrentTimeMS(),aamp_GetCurrentTimeMS());
		customHeader.append(moneytracebuf);
	}	
	AAMPLOG_TRACE("[GetMoneyTraceString] MoneyTrace[%s]",customHeader.c_str());
}
#endif /* USE_SECCLIENT */

/**
 * @brief Send tuned event if configured to sent after decryption
 */
void PrivateInstanceAAMP::NotifyFirstFragmentDecrypted()
{
	if(mTunedEventPending)
	{
		TunedEventConfig tunedEventConfig =  IsLive() ? mTuneEventConfigLive : mTuneEventConfigVod;
		if (eTUNED_EVENT_ON_FIRST_FRAGMENT_DECRYPTED == tunedEventConfig)
		{
			// For HLS - This is invoked by fetcher thread, so we have to sent asynchronously
			if (SendTunedEvent(false))
			{
				logprintf("aamp: %s - sent tune event after first fragment fetch and decrypt\n", mMediaFormatName[mMediaFormat]);
			}
		}
	}
}

/**
 *   @brief  Get PTS of first sample.
 *
 *   @return PTS of first sample
 */
double PrivateInstanceAAMP::GetFirstPTS()
{
	assert(NULL != mpStreamAbstractionAAMP);
	return mpStreamAbstractionAAMP->GetFirstPTS();
}

/**
 *   @brief Check if Live Adjust is required for current content. ( For "vod/ivod/ip-dvr/cdvr/eas", Live Adjust is not required ).
 *
 *   @return False if the content is either vod/ivod/cdvr/ip-dvr/eas
 */
bool PrivateInstanceAAMP::IsLiveAdjustRequired()
{
	bool retValue;

	switch (mContentType)
	{
		case ContentType_IVOD:
		case ContentType_VOD:
		case ContentType_CDVR:
		case ContentType_IPDVR:
		case ContentType_EAS:
			retValue = false;
			break;

		default:
			retValue = true;
			break;
	}
	return retValue;
}

/**
 *   @brief  Generate media metadata event based on args passed.
 *
 *   @param[in] durationMs - duration of playlist in milliseconds
 *   @param[in] langList - list of audio language available in asset
 *   @param[in] bitrateList - list of video bitrates available in asset
 *   @param[in] hasDrm - indicates if asset is encrypted/clear
 *   @param[in] isIframeTrackPresent - indicates if iframe tracks are available in asset
 */
void PrivateInstanceAAMP::SendMediaMetadataEvent(double durationMs, std::set<std::string>langList, std::vector<long> bitrateList, bool hasDrm, bool isIframeTrackPresent)
{
	std::vector<int> supportedPlaybackSpeeds { -64, -32, -16, -4, -1, 0, 1, 4, 16, 32, 64 };
	int langCount = 0;
	int bitrateCount = 0;
	int supportedSpeedCount = 0;
	int width  = 1280;
	int height = 720;

	GetPlayerVideoSize(width, height);

	std::string drmType = "NONE";
	std::shared_ptr<AampDrmHelper> helper = GetCurrentDRM();
	if (helper)
	{
		drmType = helper->friendlyName();
	}

	MediaMetadataEventPtr event = std::make_shared<MediaMetadataEvent>(durationMs, width, height, hasDrm, IsLive(), drmType);

	for (auto iter = langList.begin(); iter != langList.end(); iter++)
	{
		if (!iter->empty())
		{
			assert(iter->size() < MAX_LANGUAGE_TAG_LENGTH - 1);
			event->addLanguage((*iter));
		}
	}

	for (int i = 0; i < bitrateList.size(); i++)
	{
		event->addBitrate(bitrateList[i]);
	}

	//Iframe track present and hence playbackRate change is supported
	if (isIframeTrackPresent)
	{
		for(int i = 0; i < supportedPlaybackSpeeds.size(); i++)
		{
			event->addSupportedSpeed(supportedPlaybackSpeeds[i]);
		}
	}
	else
	{
		//Supports only pause and play
		event->addSupportedSpeed(0);
		event->addSupportedSpeed(1);
	}

	SendEventAsync(event);
}

/**
 *   @brief  Generate supported speeds changed event based on arg passed.
 *
 *   @param[in] isIframeTrackPresent - indicates if iframe tracks are available in asset
 */
void PrivateInstanceAAMP::SendSupportedSpeedsChangedEvent(bool isIframeTrackPresent)
{
	SupportedSpeedsChangedEventPtr event = std::make_shared<SupportedSpeedsChangedEvent>();
	std::vector<int> supportedPlaybackSpeeds { -64, -32, -16, -4, -1, 0, 1, 4, 16, 32, 64 };
	int supportedSpeedCount = 0;

	//Iframe track present and hence playbackRate change is supported
	if (isIframeTrackPresent)
	{
		for(int i = 0; i < supportedPlaybackSpeeds.size(); i++)
		{
			event->addSupportedSpeed(supportedPlaybackSpeeds[i]);;
		}
	}
	else
	{
		//Supports only pause and play
		event->addSupportedSpeed(0);
		event->addSupportedSpeed(1);
	}

	logprintf("aamp: sending supported speeds changed event with count %d", event->getSupportedSpeedCount());
	SendEventAsync(event);
}

/**
 *   @brief  Generate Blocked  event based on args passed.
 *
 *   @param[in] reason          - Blocked Reason
 */
void PrivateInstanceAAMP::SendBlockedEvent(const std::string & reason)
{
	BlockedEventPtr event = std::make_shared<BlockedEvent>(reason);
	SendEventAsync(event);
#ifdef AAMP_CC_ENABLED
	if (0 == reason.compare("SERVICE_PIN_LOCKED"))
	{
		if (gpGlobalConfig->nativeCCRendering)
		{
			AampCCManager::GetInstance()->SetParentalControlStatus(true);
		}
	}
#endif
}

/**
 *   @brief To set the initial bitrate value.
 *
 *   @param[in] initial bitrate to be selected
 */
void PrivateInstanceAAMP::SetInitialBitrate(long bitrate)
{
	if (bitrate > 0)
	{
		gpGlobalConfig->defaultBitrate = bitrate;
	}
}

/**
 *   @brief To set the initial bitrate value for 4K assets.
 *
 *   @param[in] initial bitrate to be selected for 4K assets
 */
void PrivateInstanceAAMP::SetInitialBitrate4K(long bitrate4K)
{
	if (bitrate4K > 0)
	{
		gpGlobalConfig->defaultBitrate4K = bitrate4K;
	}
}

/**
 *   @brief To set the network download timeout value.
 *
 *   @param[in] preferred timeout value
 */
void PrivateInstanceAAMP::SetNetworkTimeout(double timeout)
{
	if(timeout > 0)
	{
		mNetworkTimeoutMs = (long)CONVERT_SEC_TO_MS(timeout);
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d network timeout set to - %ld ms", __FUNCTION__, __LINE__, mNetworkTimeoutMs);
	}
}

/**
 *   @brief To set the network timeout to based on the priority
 *
 */
void PrivateInstanceAAMP::ConfigureNetworkTimeout()
{
	// If aamp.cfg has value , then set it as priority
	if(gpGlobalConfig->networkTimeoutMs != -1)
	{
		mNetworkTimeoutMs = gpGlobalConfig->networkTimeoutMs;
	}
	else if(mNetworkTimeoutMs == -1)
	{
		// if App has not set the value , then set default value 
		mNetworkTimeoutMs = (long)CONVERT_SEC_TO_MS(CURL_FRAGMENT_DL_TIMEOUT);
	}
}

/**
 *   @brief To set the manifest download timeout value.
 *
 *   @param[in] preferred timeout value
 */
void PrivateInstanceAAMP::SetManifestTimeout(double timeout)
{
	if (timeout > 0)
	{
		mManifestTimeoutMs = (long)CONVERT_SEC_TO_MS(timeout);
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d manifest timeout set to - %ld ms", __FUNCTION__, __LINE__, mManifestTimeoutMs);
	}
}

/**
 *   @brief To set the playlist download timeout value.
 *
 *   @param[in] preferred timeout value
 */
void PrivateInstanceAAMP::SetPlaylistTimeout(double timeout)
{
	if (timeout > 0)
	{
		mPlaylistTimeoutMs = (long)CONVERT_SEC_TO_MS(timeout);
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d Playlist timeout set to - %ld ms", __FUNCTION__, __LINE__, mPlaylistTimeoutMs);
	}
}

/**
 *   @brief To set the manifest timeout as per priority
 *
 */
void PrivateInstanceAAMP::ConfigureManifestTimeout()
{
	if(gpGlobalConfig->manifestTimeoutMs != -1)
	{
		mManifestTimeoutMs = gpGlobalConfig->manifestTimeoutMs;
	}
	else if(mManifestTimeoutMs == -1)
	{
		mManifestTimeoutMs = mNetworkTimeoutMs;
	}
}

/**
 *   @brief To set the playlist timeout as per priority
 *
 */
void PrivateInstanceAAMP::ConfigurePlaylistTimeout()
{
        if(gpGlobalConfig->playlistTimeoutMs != -1)
        {
                mPlaylistTimeoutMs = gpGlobalConfig->playlistTimeoutMs;
        }
        else if(mPlaylistTimeoutMs == -1)
        {
                mPlaylistTimeoutMs = mNetworkTimeoutMs;
        }
}

/**
 *   @brief To set DASH Parallel Download configuration for fragments
 *
 */
void PrivateInstanceAAMP::ConfigureDashParallelFragmentDownload()
{
	if(gpGlobalConfig->dashParallelFragDownload != eUndefinedState)
	{
		mDashParallelFragDownload = (bool)gpGlobalConfig->dashParallelFragDownload;
	}

	AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d DASH Paraller Frag DL Config [%d]", __FUNCTION__, __LINE__, mDashParallelFragDownload);
}

/**
 *   @brief To set Parallel Download configuration
 *
 */
void PrivateInstanceAAMP::ConfigureParallelFetch()
{
	// for VOD playlist fetch
	if(gpGlobalConfig->playlistsParallelFetch != eUndefinedState)
	{
		mParallelFetchPlaylist = (bool)gpGlobalConfig->playlistsParallelFetch;
	}

	// for linear playlist fetch
	if(gpGlobalConfig->parallelPlaylistRefresh  != eUndefinedState)
	{
		mParallelFetchPlaylistRefresh = (bool)gpGlobalConfig->parallelPlaylistRefresh ;
	}
}

/**
 *   @brief To set bulk timedMetadata reporting configuration
 *
 */
void PrivateInstanceAAMP::ConfigureBulkTimedMetadata()
{
        if(gpGlobalConfig->enableBulkTimedMetaReport != eUndefinedState)
        {
                mBulkTimedMetadata = (bool)gpGlobalConfig->enableBulkTimedMetaReport;
        }
}

/**
 *   @brief To set unpaired discontinuity retune configuration
 *
 */
void PrivateInstanceAAMP::ConfigureRetuneForUnpairedDiscontinuity()
{
    if(gpGlobalConfig->useRetuneForUnpairedDiscontinuity != eUndefinedState)
    {
            mUseRetuneForUnpairedDiscontinuity = (bool)gpGlobalConfig->useRetuneForUnpairedDiscontinuity;
    }
}

/**
 *   @brief To set retune configuration for gstpipeline internal data stream error.
 *
 */
void PrivateInstanceAAMP::ConfigureRetuneForGSTInternalError()
{
    if(gpGlobalConfig->useRetuneForGSTInternalError != eUndefinedState)
    {
            mUseRetuneForGSTInternalError = (bool)gpGlobalConfig->useRetuneForGSTInternalError;
    }
}

/**
 *   @brief Set unpaired discontinuity retune flag
 *   @param[in] bValue - true if unpaired discontinuity retune set
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetRetuneForUnpairedDiscontinuity(bool bValue)
{
    mUseRetuneForUnpairedDiscontinuity = bValue;
    AAMPLOG_INFO("%s:%d Retune For Unpaired Discontinuity Config from App : %d " ,__FUNCTION__,__LINE__,bValue);
}

/**
 *   @brief Set retune configuration for gstpipeline internal data stream error.
 *   @param[in] bValue - true if gst internal error retune set
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetRetuneForGSTInternalError(bool bValue)
{
	mUseRetuneForGSTInternalError = bValue;
	AAMPLOG_INFO("%s:%d GST Retune Config from App : %d", __FUNCTION__, __LINE__, bValue);
}

/**
 *   @brief Function to Configure PreCache Playlist functionality
 *
 */
void PrivateInstanceAAMP::ConfigurePreCachePlaylist()
{
	if(gpGlobalConfig->mPreCacheTimeWindow > 0)
	{
		mPreCacheDnldTimeWindow = gpGlobalConfig->mPreCacheTimeWindow;
	}
}

/**
 *   @brief Function to set the max retry attempts for init frag curl timeout failures
 *
 */
void PrivateInstanceAAMP::ConfigureInitFragTimeoutRetryCount()
{
	if(gpGlobalConfig->initFragmentRetryCount >= 0)
	{
		// given priority - if specified in /opt/aamp.cfg
		mInitFragmentRetryCount = gpGlobalConfig->initFragmentRetryCount;
	}
	else if (-1 == mInitFragmentRetryCount)
	{
		mInitFragmentRetryCount = DEFAULT_DOWNLOAD_RETRY_COUNT;
	}
}

/**
 *   @brief To set Westeros sink configuration
 *
 */
void PrivateInstanceAAMP::ConfigureWesterosSink()
{
	if (gpGlobalConfig->mWesterosSinkConfig != eUndefinedState)
	{
		mWesterosSinkEnabled = (bool)gpGlobalConfig->mWesterosSinkConfig;
	}

	AAMPLOG_WARN("%s Westeros Sink", mWesterosSinkEnabled ? "Enabling" : "Disabling");
}

/**
 *   @brief To set license caching config
 *
 */
void PrivateInstanceAAMP::ConfigureLicenseCaching()
{
	if (gpGlobalConfig->licenseCaching != eUndefinedState)
	{
		mLicenseCaching = (bool)gpGlobalConfig->licenseCaching;
	}

	if (!mLicenseCaching)
	{
		gpGlobalConfig->dash_MaxDRMSessions = 1; // By configuring 1 to max session will request a license every time when a new tune will be initiated.
		AAMPLOG_WARN("%s License Caching, MaxDRMSessions: %d", mLicenseCaching ? "Enabling" : "Disabling", gpGlobalConfig->dash_MaxDRMSessions);
	}
}

/**
 *   @brief To set the download buffer size value
 *
 *   @param[in] preferred download buffer size
 */
void PrivateInstanceAAMP::SetDownloadBufferSize(int bufferSize)
{
	if (bufferSize > 0)
	{
		gpGlobalConfig->maxCachedFragmentsPerTrack = bufferSize;
	}
}

/**
 *   @brief To check if tune operation completed
 *
 *   @retval true if completed
 */
bool PrivateInstanceAAMP::IsTuneCompleted()
{
	return mTuneCompleted;
}

/**
 *   @brief Set Preferred DRM.
 *
 *   @param[in] drmType - Preferred DRM type
 */
void PrivateInstanceAAMP::SetPreferredDRM(DRMSystems drmType)
{
	// if Preferred DRM is set using /opt/aamp.cfg or via RFC then
	// ignore this function setting
	if(gpGlobalConfig->isUsingLocalConfigForPreferredDRM)
	{
		AAMPLOG_INFO("%s:%d Ignoring Preferred drm: %d setting as localConfig for Preferred DRM is set to :%d", __FUNCTION__, __LINE__, drmType,gpGlobalConfig->preferredDrm);
	}
	else
	{
		AAMPLOG_INFO("%s:%d set Preferred drm: %d", __FUNCTION__, __LINE__, drmType);
		gpGlobalConfig->preferredDrm = drmType;
	}
}

/**
 *   @brief Get Preferred DRM.
 *
 *   @return Preferred DRM type
 */
DRMSystems PrivateInstanceAAMP::GetPreferredDRM()
{
	return gpGlobalConfig->preferredDrm;
}

/**
 *   @brief Set Stereo Only Playback.
 */
void PrivateInstanceAAMP::SetStereoOnlyPlayback(bool bValue)
{
	// If Stereo Only Mode is true, then disable DD+ and ATMOS (or) make if enable
	if(gpGlobalConfig->disableEC3 == eUndefinedState)
	{
		mDisableEC3 = bValue;
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d EC3 is : %s", __FUNCTION__, __LINE__, (bValue)? "Disabled" : "Enabled");
	}
	if(gpGlobalConfig->disableATMOS == eUndefinedState)
	{
		mDisableATMOS = bValue;
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d ATMOS is : %s", __FUNCTION__, __LINE__, (bValue)? "Disabled" : "Enabled");
	}
}

/**
 *   @brief Notification from the stream abstraction that a new SCTE35 event is found.
 *
 *   @param[in] Adbreak's unique identifier.
 *   @param[in] Break start time in milli seconds.
 *   @param[in] Break duration in milli seconds
 *   @param[in] SCTE35 binary object.
 */
void PrivateInstanceAAMP::FoundSCTE35(const std::string &adBreakId, uint64_t startMS, uint32_t breakdur, std::string &scte35)
{
	if(gpGlobalConfig->enableClientDai && !adBreakId.empty())
	{
		AAMPLOG_WARN("%s:%d [CDAI] Found Adbreak on period[%s] Duration[%d]", __FUNCTION__, __LINE__, adBreakId.c_str(), breakdur);
		std::string adId("");
		std::string url("");
		mCdaiObject->SetAlternateContents(adBreakId, adId, url, startMS, breakdur);	//A placeholder to avoid multiple scte35 event firing for the same adbreak
		ReportTimedMetadata(aamp_GetCurrentTimeMS(), "SCTE35", scte35.c_str(), scte35.size(), false, adBreakId.c_str(), breakdur);
	}
}

/**
 *   @brief Setting the alternate contents' (Ads/blackouts) URLs
 *
 *   @param[in] Adbreak's unique identifier.
 *   @param[in] Individual Ad's id
 *   @param[in] Ad URL
 */
void PrivateInstanceAAMP::SetAlternateContents(const std::string &adBreakId, const std::string &adId, const std::string &url)
{
	if(gpGlobalConfig->enableClientDai)
	{
		mCdaiObject->SetAlternateContents(adBreakId, adId, url);
	}
	else
	{
		AAMPLOG_WARN("%s:%d is called! CDAI not enabled!! Rejecting the promise.", __FUNCTION__, __LINE__);
		SendAdResolvedEvent(adId, false, 0, 0);
	}
}

/**
 *   @brief Send status of Ad manifest downloading & parsing
 *
 *   @param[in] Ad's unique identifier.
 *   @param[in] Manifest status (success/Failure)
 *   @param[in] Ad playback start time in milliseconds
 *   @param[in] Ad's duration in milliseconds
 */
void PrivateInstanceAAMP::SendAdResolvedEvent(const std::string &adId, bool status, uint64_t startMS, uint64_t durationMs)
{
	if (mDownloadsEnabled)	//Send it, only if Stop not called
	{
		AdResolvedEventPtr e = std::make_shared<AdResolvedEvent>(status, adId, startMS, durationMs);
		AAMPLOG_WARN("PrivateInstanceAAMP::%s():%d, [CDAI] Sent resolved status=%d for adId[%s]", __FUNCTION__, __LINE__, status, adId.c_str());
		SendEventAsync(e);
	}
}

/**
 *   @brief Deliver pending Ad events to JSPP
 */
void PrivateInstanceAAMP::DeliverAdEvents(bool immediate)
{
	std::lock_guard<std::mutex> lock(mAdEventQMtx);
	while (!mAdEventsQ.empty())
	{
		AAMPEventPtr e = mAdEventsQ.front();
		if(immediate)
		{
			SendEventAsync(e); 	//Need to send all events from gst idle thread.
		}
		else
		{
			SendEventSync(e);	//Already from gst idle thread
		}
		AAMPEventType evtType = e->getType();
		AAMPLOG_WARN("PrivateInstanceAAMP::%s():%d, [CDAI] Delivered AdEvent[%s] to JSPP.", __FUNCTION__, __LINE__, ADEVENT2STRING(evtType));
		if(AAMP_EVENT_AD_PLACEMENT_START == evtType)
		{
			AdPlacementEventPtr placementEvt = std::dynamic_pointer_cast<AdPlacementEvent>(e);
			mAdProgressId       = placementEvt->getAdId();
			mAdPrevProgressTime = NOW_STEADY_TS_MS;
			mAdCurOffset        = placementEvt->getOffset();
			mAdDuration         = placementEvt->getDuration();
		}
		else if(AAMP_EVENT_AD_PLACEMENT_END == evtType || AAMP_EVENT_AD_PLACEMENT_ERROR == evtType)
		{
			mAdProgressId = "";
		}
		mAdEventsQ.pop();
	}
}

/**
 *   @brief Send Ad reservation event
 *
 *   @param[in] type - Event type
 *   @param[in] adBreakId - Reservation Id
 *   @param[in] position - Event position in terms of channel's timeline
 *   @param[in] immediate - Send it immediate or not
 */
void PrivateInstanceAAMP::SendAdReservationEvent(AAMPEventType type, const std::string &adBreakId, uint64_t position, bool immediate)
{
	if(AAMP_EVENT_AD_RESERVATION_START == type || AAMP_EVENT_AD_RESERVATION_END == type)
	{
		AAMPLOG_INFO("PrivateInstanceAAMP::%s():%d, [CDAI] Pushed [%s] of adBreakId[%s] to Queue.", __FUNCTION__, __LINE__, ADEVENT2STRING(type), adBreakId.c_str());

		AdReservationEventPtr e = std::make_shared<AdReservationEvent>(type, adBreakId, position);

		{
			{
				std::lock_guard<std::mutex> lock(mAdEventQMtx);
				mAdEventsQ.push(e);
			}
			if(immediate)
			{
				//Despatch all ad events now
				DeliverAdEvents(true);
			}
		}
	}
}

/**
 *   @brief Send Ad placement event
 *
 *   @param[in] type - Event type
 *   @param[in] adId - Placement Id
 *   @param[in] position - Event position wrt to the corresponding adbreak start
 *   @param[in] adOffset - Offset point of the current ad
 *   @param[in] adDuration - Duration of the current ad
 *   @param[in] immediate - Send it immediate or not
 *   @param[in] error_code - Error code (in case of placment error)
 */
void PrivateInstanceAAMP::SendAdPlacementEvent(AAMPEventType type, const std::string &adId, uint32_t position, uint32_t adOffset, uint32_t adDuration, bool immediate, long error_code)
{
	if(AAMP_EVENT_AD_PLACEMENT_START <= type && AAMP_EVENT_AD_PLACEMENT_ERROR >= type)
	{
		AAMPLOG_INFO("PrivateInstanceAAMP::%s():%d, [CDAI] Pushed [%s] of adId[%s] to Queue.", __FUNCTION__, __LINE__, ADEVENT2STRING(type), adId.c_str());

		AdPlacementEventPtr e = std::make_shared<AdPlacementEvent>(type, adId, position, adOffset * 1000 /*MS*/, adDuration, error_code);

		{
			{
				std::lock_guard<std::mutex> lock(mAdEventQMtx);
				mAdEventsQ.push(e);
			}
			if(immediate)
			{
				//Despatch all ad events now
				DeliverAdEvents(true);
			}
		}
	}
}

/**
 *   @brief Get stream type
 *
 *   @retval stream type as string
 */
std::string PrivateInstanceAAMP::getStreamTypeString()
{
	std::string type = mMediaFormatName[mMediaFormat];

	if(mCurrentDrm != nullptr) //Incomplete Init won't be set the DRM
	{
		type += "/";
		type += mCurrentDrm->friendlyName();
	}
	else
	{
		type += "/Clear";
	}
	return type;
}

/**
 *   @brief Get the profile bucket for a media type
 *
 *   @param[in] fileType - media type
 *   @retval profile bucket for the media type
 */
ProfilerBucketType PrivateInstanceAAMP::mediaType2Bucket(MediaType fileType)
{
	ProfilerBucketType pbt;
	switch(fileType)
	{
		case eMEDIATYPE_VIDEO:
			pbt = PROFILE_BUCKET_FRAGMENT_VIDEO;
			break;
		case eMEDIATYPE_AUDIO:
			pbt = PROFILE_BUCKET_FRAGMENT_AUDIO;
			break;
		case eMEDIATYPE_SUBTITLE:
			pbt = PROFILE_BUCKET_FRAGMENT_SUBTITLE;
			break;
		case eMEDIATYPE_MANIFEST:
			pbt = PROFILE_BUCKET_MANIFEST;
			break;
		case eMEDIATYPE_INIT_VIDEO:
			pbt = PROFILE_BUCKET_INIT_VIDEO;
			break;
		case eMEDIATYPE_INIT_AUDIO:
			pbt = PROFILE_BUCKET_INIT_AUDIO;
			break;
		case eMEDIATYPE_INIT_SUBTITLE:
			pbt = PROFILE_BUCKET_INIT_SUBTITLE;
			break;
		case eMEDIATYPE_PLAYLIST_VIDEO:
			pbt = PROFILE_BUCKET_PLAYLIST_VIDEO;
			break;
		case eMEDIATYPE_PLAYLIST_AUDIO:
			pbt = PROFILE_BUCKET_PLAYLIST_AUDIO;
			break;
		case eMEDIATYPE_PLAYLIST_SUBTITLE:
			pbt = PROFILE_BUCKET_PLAYLIST_SUBTITLE;
			break;
		default:
			pbt = (ProfilerBucketType)fileType;
			break;
	}
	return pbt;
}

/**
 *   @brief Sets Recorded URL from Manifest received form XRE.
 *   @param[in] isrecordedUrl - flag to check for recordedurl in Manifest
 */
void PrivateInstanceAAMP::SetTunedManifestUrl(bool isrecordedUrl)
{
	mTunedManifestUrl.assign(mManifestUrl);
	traceprintf("%s::mManifestUrl: %s",__FUNCTION__,mManifestUrl.c_str());
	if(isrecordedUrl)
	{
		DeFog(mTunedManifestUrl);
		mTunedManifestUrl.replace(0,4,"_fog");
	}
	traceprintf("PrivateInstanceAAMP::%s, tunedManifestUrl:%s ", __FUNCTION__, mTunedManifestUrl.c_str());
}

/**
 *   @brief Gets Recorded URL from Manifest received form XRE.
 *   @param[out] manifestUrl - for VOD and recordedUrl for FOG enabled
 */
const char* PrivateInstanceAAMP::GetTunedManifestUrl()
{
	traceprintf("PrivateInstanceAAMP::%s, tunedManifestUrl:%s ", __FUNCTION__, mTunedManifestUrl.c_str());
	return mTunedManifestUrl.c_str();
}

/**
 *   @brief To set the network proxy
 *
 *   @param[in] network proxy to use
 */
void PrivateInstanceAAMP::SetNetworkProxy(const char * proxy)
{
	pthread_mutex_lock(&mLock);
	if(mNetworkProxy)
	{
		free(mNetworkProxy);
	}
	mNetworkProxy = strdup(proxy);
	pthread_mutex_unlock(&mLock);
}

/**
 *   @brief Get network proxy
 *
 *   @retval network proxy
 */
const char* PrivateInstanceAAMP::GetNetworkProxy() const
{
	return mNetworkProxy;
}

/**
 *   @brief To set the proxy for license request
 *
 *   @param[in] proxy to use for license request
 */
void PrivateInstanceAAMP::SetLicenseReqProxy(const char * licenseProxy)
{
	pthread_mutex_lock(&mLock);
	if(mLicenseProxy)
	{
		free(mLicenseProxy);
	}
	mLicenseProxy = strdup(licenseProxy);
	pthread_mutex_unlock(&mLock);
}

/**
 *   @brief Signal trick mode discontinuity to stream sink
 *
 *   @return void
 */
void PrivateInstanceAAMP::SignalTrickModeDiscontinuity()
{
	if (mStreamSink)
	{
		mStreamSink->SignalTrickModeDiscontinuity();
	}
}

/**
 *   @brief Check if current stream is muxed
 *
 *   @return true if current stream is muxed
 */
bool PrivateInstanceAAMP::IsMuxedStream()
{
	bool ret = false;
	if (mpStreamAbstractionAAMP)
	{
		ret = mpStreamAbstractionAAMP->IsMuxedStream();
	}
	return ret;
}

/**
 *   @brief To set the curl stall timeout value
 *
 *   @param[in] curl stall timeout
 */
void PrivateInstanceAAMP::SetDownloadStallTimeout(long stallTimeout)
{
	if (stallTimeout >= 0)
	{
		gpGlobalConfig->curlStallTimeout = stallTimeout;
	}
}

/**
 *   @brief To set the curl download start timeout value
 *
 *   @param[in] curl download start timeout
 */
void PrivateInstanceAAMP::SetDownloadStartTimeout(long startTimeout)
{
	if (startTimeout >= 0)
	{
		gpGlobalConfig->curlDownloadStartTimeout = startTimeout;
	}
}

/**
 * @brief Stop injection for a track.
 * Called from StopInjection
 *
 * @param[in] Media type
 * @return void
 */
void PrivateInstanceAAMP::StopTrackInjection(MediaType type)
{
#ifdef AAMP_DEBUG_FETCH_INJECT
	if ((1 << type) & AAMP_DEBUG_FETCH_INJECT)
	{
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d", __FUNCTION__, (int) type);
	}
#endif
	if (!mTrackInjectionBlocked[type])
	{
		AAMPLOG_TRACE("PrivateInstanceAAMP::%s for type %s", __FUNCTION__, (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mTrackInjectionBlocked[type] = true;
		pthread_mutex_unlock(&mLock);
	}
	traceprintf ("PrivateInstanceAAMP::%s Exit. type = %d", __FUNCTION__, (int) type);
}

/**
 * @brief Resume injection for a track.
 * Called from StartInjection
 *
 * @param[in] Media type
 * @return void
 */
void PrivateInstanceAAMP::ResumeTrackInjection(MediaType type)
{
#ifdef AAMP_DEBUG_FETCH_INJECT
	if ((1 << type) & AAMP_DEBUG_FETCH_INJECT)
	{
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d", __FUNCTION__, (int) type);
	}
#endif
	if (mTrackInjectionBlocked[type])
	{
		AAMPLOG_TRACE("PrivateInstanceAAMP::%s for type %s", __FUNCTION__, (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mTrackInjectionBlocked[type] = false;
		pthread_mutex_unlock(&mLock);
	}
	traceprintf ("PrivateInstanceAAMP::%s Exit. type = %d", __FUNCTION__, (int) type);
}

/**
 *   @brief Receives first video PTS of the current playback
 *
 *   @param[in]  pts - pts value
 *   @param[in]  timeScale - time scale (default 90000)
 */
void PrivateInstanceAAMP::NotifyFirstVideoPTS(unsigned long long pts, unsigned long timeScale)
{
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->NotifyFirstVideoPTS(pts, timeScale);
	}
}

/**
 *   @brief Notifies base PTS of the HLS video playback
 *
 *   @param[in]  pts - base pts value
 */
void PrivateInstanceAAMP::NotifyVideoBasePTS(unsigned long long basepts)
{
	mVideoBasePTS = basepts;
	AAMPLOG_INFO("mVideoBasePTS::%llu\n", mVideoBasePTS);
}

/**
 *   @brief To send webvtt cue as an event
 *
 *   @param[in]  cue - vtt cue object
 */
void PrivateInstanceAAMP::SendVTTCueDataAsEvent(VTTCue* cue)
{
	//This function is called from an idle handler and hence we call SendEventSync
	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_WEBVTT_CUE_DATA])
	{
		WebVttCueEventPtr ev = std::make_shared<WebVttCueEvent>(cue);
		SendEventSync(ev);
	}
}

/**
 *   @brief To check if subtitles are enabled
 *
 *   @return bool - true if subtitles are enabled
 */
bool PrivateInstanceAAMP::IsSubtitleEnabled(void)
{
	// Assumption being that enableSubtec and event listener will not be registered at the same time
	// in which case subtec gets priority over event listener
	return (gpGlobalConfig->bEnableSubtec || mEventListeners[AAMP_EVENT_WEBVTT_CUE_DATA] != NULL);
	//(!IsDashAsset() && (mEventListener || mEventListeners[AAMP_EVENT_WEBVTT_CUE_DATA]));
}

/**
 *   @brief To check if JavaScript cue listeners are registered
 *
 *   @return bool - true if listeners are registered
 */
bool PrivateInstanceAAMP::WebVTTCueListenersRegistered(void)
{
	return (mEventListeners[AAMP_EVENT_WEBVTT_CUE_DATA] != NULL);
}

/**
 *   @brief To get any custom license HTTP headers that was set by application
 *
 *   @param[out] headers - map of headers
 */
void PrivateInstanceAAMP::GetCustomLicenseHeaders(std::unordered_map<std::string, std::vector<std::string>>& customHeaders)
{
	customHeaders.insert(mCustomLicenseHeaders.begin(), mCustomLicenseHeaders.end());
}

/**
 *   @brief Set parallel playlist download config value.
 *   @param[in] bValue - true if a/v playlist to be downloaded in parallel
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetParallelPlaylistDL(bool bValue)
{
	mParallelFetchPlaylist = bValue;
	AAMPLOG_INFO("%s:%d Parallel playlist DL Config from App : %d " ,__FUNCTION__,__LINE__,bValue);
}

/**
 *   @brief Sends an ID3 metadata event.
 *
 *   @param[in] data pointer to ID3 metadata.
 *   @param[in] length length of ID3 metadata.
 */
void PrivateInstanceAAMP::SendId3MetadataEvent(std::vector<uint8_t> &data)
{
	ID3MetadataEventPtr e = std::make_shared<ID3MetadataEvent>(data);
	SendEventSync(e);
}

/**
 * @brief Gets the listener registration status of a given event
 * @param[in] eventType - type of the event to be checked
 *
 * @retval bool - True if an event listener for the event type exists
 */
bool PrivateInstanceAAMP::GetEventListenerStatus(AAMPEventType eventType)
{
	if (mEventListeners[eventType] != NULL)
	{
		return true;
	}
	return false;
}

/**
 *	 @brief Set parallel playlist download config value for linear .
 *	 @param[in] bValue - true if a/v playlist to be downloaded in parallel for linear
 *
 *	 @return void
 */
void PrivateInstanceAAMP::SetParallelPlaylistRefresh(bool bValue)
{
	mParallelFetchPlaylistRefresh = bValue;
	AAMPLOG_INFO("%s:%d Parallel playlist Refresh Fetch  Config from App : %d " ,__FUNCTION__,__LINE__,bValue);
}

/**
 *   @brief Set Bulk TimedMetadata reporting flag 
 *   @param[in] bValue - true if Application supports bulk reporting 
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetBulkTimedMetaReport(bool bValue)
{
        mBulkTimedMetadata = bValue;
        AAMPLOG_INFO("%s:%d Bulk TimedMetadata report Config from App : %d " ,__FUNCTION__,__LINE__,bValue);
}

/**
 *   @brief Sending a flushing seek to stream sink with given position
 *
 *   @param[in] position - position value to seek to
 *   @param[in] rate - playback rate
 *   @return void
 */
void PrivateInstanceAAMP::FlushStreamSink(double position, double rate)
{
#ifndef AAMP_STOP_SINK_ON_SEEK
	if (mStreamSink)
	{
		if(gpGlobalConfig->midFragmentSeekEnabled && position != 0 )
		{
			//RDK-26957 Adding midSeekPtsOffset to position value.
			//Enables us to seek to the desired position in the mp4 fragment.
			mStreamSink->SeekStreamSink(position + GetFirstPTS(), rate);
		}
		else
		{
			mStreamSink->SeekStreamSink(position, rate);
		}
	}
#endif
}

/**
 *   @brief PreCachePlaylistDownloadTask Thread function for PreCaching Playlist 
 *
 *   @return void
 */
void PrivateInstanceAAMP::PreCachePlaylistDownloadTask()
{
	// This is the thread function to download all the HLS Playlist in a 
	// differed manner
	int maxWindowforDownload = mPreCacheDnldTimeWindow * 60; // convert to seconds  
	int szPlaylistCount = mPreCacheDnldList.size();
	if(szPlaylistCount)
	{
		PrivAAMPState state;
		// First wait for Tune to complete to start this functionality
		pthread_mutex_lock(&mMutexPlaystart);
		pthread_cond_wait(&waitforplaystart, &mMutexPlaystart);
		pthread_mutex_unlock(&mMutexPlaystart);
		// May be Stop is called to release all resources .
		// Before download , check the state 
		GetState(state);
		// Check for state not IDLE also to avoid DELIA-46092
		if(state != eSTATE_RELEASED || state != eSTATE_IDLE)
		{
			CurlInit(eCURLINSTANCE_PLAYLISTPRECACHE, 1, GetNetworkProxy());
			SetCurlTimeout(mPlaylistTimeoutMs, eCURLINSTANCE_PLAYLISTPRECACHE);
			int sleepTimeBetweenDnld = (maxWindowforDownload / szPlaylistCount) * 1000; // time in milliSec 
			int idx = 0;
			do
			{
				InterruptableMsSleep(sleepTimeBetweenDnld);
				if(DownloadsAreEnabled())
				{
					// First check if the file is already in Cache
					PreCacheUrlStruct newelem = mPreCacheDnldList.at(idx);
					
					// check if url cached ,if not download
					if(getAampCacheHandler()->IsUrlCached(newelem.url)==false)
					{
						AAMPLOG_WARN("%s Downloading Playlist Type:%d for PreCaching:%s",__FUNCTION__,
							newelem.type, newelem.url.c_str());
						std::string playlistUrl;
						std::string playlistEffectiveUrl;
						GrowableBuffer playlistStore;
						long http_error;
						double downloadTime;
						if(GetFile(newelem.url, &playlistStore, playlistEffectiveUrl, &http_error, &downloadTime, NULL, eCURLINSTANCE_PLAYLISTPRECACHE, true, newelem.type))
						{
							// If successful download , then insert into Cache 
							getAampCacheHandler()->InsertToPlaylistCache(newelem.url, &playlistStore, playlistEffectiveUrl, false, newelem.type);
							aamp_Free(&playlistStore.ptr);
						}	
					}
					idx++;
				}
				else
				{
					// this can come here if trickplay is done or play started late
					if(state == eSTATE_SEEKING || state == eSTATE_PREPARED)
					{
						// wait for seek to complete 
						sleep(1);
					}
				}
				GetState(state);
			}while (idx < mPreCacheDnldList.size() && state != eSTATE_RELEASED && state != eSTATE_IDLE);
			mPreCacheDnldList.clear();
			CurlTerm(eCURLINSTANCE_PLAYLISTPRECACHE);
		}
	}
	AAMPLOG_WARN("%s End of PreCachePlaylistDownloadTask ", __FUNCTION__);
}

/**
 *   @brief SetPreCacheDownloadList - Function to assign the PreCaching file list
 *   @param[in] Playlist Download list  
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetPreCacheDownloadList(PreCacheUrlList &dnldListInput)
{
	mPreCacheDnldList = dnldListInput;
	if(mPreCacheDnldList.size())
	{
		AAMPLOG_WARN("%s:%d Got Playlist PreCache list of Size : %d", __FUNCTION__, __LINE__, mPreCacheDnldList.size());
	}
	
}

/**
 *   @brief Get available audio tracks.
 *
 *   @return std::string JSON formatted string of available audio tracks
 */
std::string PrivateInstanceAAMP::GetAvailableAudioTracks()
{
	std::string tracks;

	if (mpStreamAbstractionAAMP)
	{
		std::vector<AudioTrackInfo> trackInfo = mpStreamAbstractionAAMP->GetAvailableAudioTracks();
		if (!trackInfo.empty())
		{
			//Convert to JSON format
			cJSON *root;
			cJSON *item;
			root = cJSON_CreateArray();
			if(root)
			{
				for (auto iter = trackInfo.begin(); iter != trackInfo.end(); iter++)
				{
					cJSON_AddItemToArray(root, item = cJSON_CreateObject());
					if (!iter->name.empty())
					{
						cJSON_AddStringToObject(item, "name", iter->name.c_str());
					}
					if (!iter->language.empty())
					{
						cJSON_AddStringToObject(item, "language", iter->language.c_str());
					}
					if (!iter->codec.empty())
					{
						cJSON_AddStringToObject(item, "codec", iter->codec.c_str());
					}
					if (!iter->rendition.empty())
					{
						cJSON_AddStringToObject(item, "rendition", iter->rendition.c_str());
					}
					if (!iter->characteristics.empty())
					{
						cJSON_AddStringToObject(item, "characteristics", iter->characteristics.c_str());
					}
					if (iter->channels != 0)
					{
						cJSON_AddNumberToObject(item, "channels", iter->channels);
					}
					if (iter->bandwidth != -1)
					{
						cJSON_AddNumberToObject(item, "bandwidth", iter->bandwidth);
					}
				}
				char *jsonStr = cJSON_Print(root);
				if (jsonStr)
				{
					tracks.assign(jsonStr);
					free(jsonStr);
				}
				cJSON_Delete(root);
			}
		}
		else
		{
			AAMPLOG_ERR("PrivateInstanceAAMP::%s() %d No available audio track information!", __FUNCTION__, __LINE__);
		}
	}
	return tracks;
}

/**
 *   @brief Get available text tracks.
 *
 *   @return const char* JSON formatted string of available text tracks
 */
std::string PrivateInstanceAAMP::GetAvailableTextTracks()
{
	std::string tracks;

	if (mpStreamAbstractionAAMP)
	{
		std::vector<TextTrackInfo> trackInfo = mpStreamAbstractionAAMP->GetAvailableTextTracks();
		if (!trackInfo.empty())
		{
			//Convert to JSON format
			cJSON *root;
			cJSON *item;
			root = cJSON_CreateArray();
			if(root)
			{
				for (auto iter = trackInfo.begin(); iter != trackInfo.end(); iter++)
				{
					cJSON_AddItemToArray(root, item = cJSON_CreateObject());
					if (!iter->name.empty())
					{
						cJSON_AddStringToObject(item, "name", iter->name.c_str());
					}
					if (iter->isCC)
					{
						cJSON_AddStringToObject(item, "type", "CLOSED-CAPTIONS");
					}
					else
					{
						cJSON_AddStringToObject(item, "type", "SUBTITLES");
					}
					if (!iter->language.empty())
					{
						cJSON_AddStringToObject(item, "language", iter->language.c_str());
					}
					if (!iter->rendition.empty())
					{
						cJSON_AddStringToObject(item, "rendition", iter->rendition.c_str());
					}
					if (!iter->instreamId.empty())
					{
						cJSON_AddStringToObject(item, "instreamId", iter->instreamId.c_str());
					}
					if (!iter->characteristics.empty())
					{
						cJSON_AddStringToObject(item, "characteristics", iter->characteristics.c_str());
					}
					if (!iter->codec.empty())
					{
						cJSON_AddStringToObject(item, "codec", iter->codec.c_str());
					}
				}
				char *jsonStr = cJSON_Print(root);
				if (jsonStr)
				{
					tracks.assign(jsonStr);
					free(jsonStr);
				}
				cJSON_Delete(root);
			}
		}
		else
		{
			AAMPLOG_ERR("PrivateInstanceAAMP::%s() %d No available text track information!", __FUNCTION__, __LINE__);
		}
	}
	return tracks;
}

/*
 *   @brief Get the video window co-ordinates
 *
 *   @return current video co-ordinates in x,y,w,h format
 */
std::string PrivateInstanceAAMP::GetVideoRectangle()
{
	return mStreamSink->GetVideoRectangle();
}

/*
 *   @brief Set the application name which has created PlayerInstanceAAMP, for logging purposes
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetAppName(std::string name)
{
	mAppName = name;
}

/*
 *   @brief Get the application name
 *
 *   @return string application name
 */
std::string PrivateInstanceAAMP::GetAppName()
{
	return mAppName;
}

/*
 *   @brief Set DRM message event
 *
 *   @return payload message payload
 */
void PrivateInstanceAAMP::individualization(const std::string& payload)
{
	DrmMessageEventPtr event = std::make_shared<DrmMessageEvent>(payload);
	SendEventAsync(event);
}

/**
 *   @brief Set initial buffer duration in seconds
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetInitialBufferDuration(int durationSec)
{
	mMinInitialCacheSeconds = durationSec;
}

/**
 *   @brief Get current initial buffer duration in seconds
 *
 *   @return void
 */
int PrivateInstanceAAMP::GetInitialBufferDuration()
{
	return mMinInitialCacheSeconds;
}

/**
 *   @brief Check if First Video Frame Displayed Notification
 *          is required.
 *
 *   @return bool - true if required
 */
bool PrivateInstanceAAMP::IsFirstVideoFrameDisplayedRequired()
{
	return mFirstVideoFrameDisplayedEnabled;
}

/**
 *   @brief Notify First Video Frame was displayed
 *
 *   @return void
 */
void PrivateInstanceAAMP::NotifyFirstVideoFrameDisplayed()
{
	if(!mFirstVideoFrameDisplayedEnabled)
	{
		return;
	}

	mFirstVideoFrameDisplayedEnabled = false;

	// Seek While Paused - pause on first Video frame displayed
	if(mPauseOnFirstVideoFrameDisp)
	{
		mPauseOnFirstVideoFrameDisp = false;
		PrivAAMPState state;
		GetState(state);
		if(state != eSTATE_SEEKING)
		{
			return;
		}

		AAMPLOG_INFO("%s: Pausing Playback on First Frame Displayed", __FUNCTION__);
		if(mpStreamAbstractionAAMP)
		{
			mpStreamAbstractionAAMP->NotifyPlaybackPaused(true);
		}
		StopDownloads();
		if(PausePipeline(true, false))
		{
			SetState(eSTATE_PAUSED);
		}
		else
		{
			AAMPLOG_ERR("%s(): Failed to pause pipeline for first frame displayed!", __FUNCTION__);
		}
	}
	// Otherwise check for setting BUFFERING state
	else if(!SetStateBufferingIfRequired())
	{
		// If Buffering state was not needed, set PLAYING state
		SetState(eSTATE_PLAYING);
	}
}

/**
 *   @brief Set eSTATE_BUFFERING if required
 *
 *   @return bool - true if has been set
 */
bool PrivateInstanceAAMP::SetStateBufferingIfRequired()
{
	bool bufferingSet = false;

	pthread_mutex_lock(&mFragmentCachingLock);
	if(IsFragmentCachingRequired())
	{
		bufferingSet = true;
		PrivAAMPState state;
		GetState(state);
		if(state != eSTATE_BUFFERING)
		{
			if(mpStreamAbstractionAAMP)
			{
				mpStreamAbstractionAAMP->NotifyPlaybackPaused(true);
			}

			if(mStreamSink)
			{
				mStreamSink->NotifyFragmentCachingOngoing();
			}
			SetState(eSTATE_BUFFERING);
		}
	}
	pthread_mutex_unlock(&mFragmentCachingLock);

	return bufferingSet;
}

/**
 * @brief Check if track can inject data into GStreamer.
 * Called from MonitorBufferHealth
 *
 * @param[in] Media type
 * @return bool true if track can inject data, false otherwise
 */
bool PrivateInstanceAAMP::TrackDownloadsAreEnabled(MediaType type)
{
	bool ret = true;
	if (type > AAMP_TRACK_COUNT)
	{
		AAMPLOG_ERR("%s:%d type[%d] is un-supported, returning default as false!", __FUNCTION__, __LINE__, type);
		ret = false;
	}
	else
	{
		pthread_mutex_lock(&mLock);
		// If blocked, track downloads are disabled
		ret = !mbTrackDownloadsBlocked[type];
		pthread_mutex_unlock(&mLock);
	}
	return ret;
}

/**
 * @brief Stop buffering in AAMP and un-pause pipeline.
 * Called from MonitorBufferHealth
 *
 * @param[in] forceStop - stop buffering forcefully
 * @return void
 */
void PrivateInstanceAAMP::StopBuffering(bool forceStop)
{
	mStreamSink->StopBuffering(forceStop);
}

/**
 * @brief Get license server url for a drm type
 *
 * @param[in] type DRM type
 * @return license server url
 */
std::string PrivateInstanceAAMP::GetLicenseServerUrlForDrm(DRMSystems type)
{
	std::string url;
	auto it = mLicenseServerUrls.find(type);
	if (it != mLicenseServerUrls.end())
	{
		url = it->second;
	}
	else
	{
		// If url is not explicitly specified, check for generic one.
		// This might be set in cases of VIPER AAMP/JSController bindings
		it = mLicenseServerUrls.find(eDRM_MAX_DRMSystems);
		if (it != mLicenseServerUrls.end())
		{
			url = it->second;
		}
	}
	return url;
}

/**
 *   @brief Set audio track
 *
 *   @param[in] trackId index of audio track in available track list
 *   @return void
 */
void PrivateInstanceAAMP::SetAudioTrack(int trackId)
{
	if (mpStreamAbstractionAAMP)
	{
		std::vector<AudioTrackInfo> tracks = mpStreamAbstractionAAMP->GetAvailableAudioTracks();
		if (!tracks.empty() && (trackId >= 0 && trackId < tracks.size()))
		{
			SetPreferredAudioTrack(tracks[trackId]);
			// TODO: Confirm if required
			languageSetByUser = true;
			if (mMediaFormat == eMEDIAFORMAT_OTA)
			{
				mpStreamAbstractionAAMP->SetAudioTrack(trackId);
			}
			else
			{
				discardEnteringLiveEvt = true;

				seek_pos_seconds = GetPositionMilliseconds() / 1000.0;
				TeardownStream(false);
				TuneHelper(eTUNETYPE_SEEK);

				discardEnteringLiveEvt = false;
			}
		}
	}
}

/**
 *   @brief Get current audio track index
 *
 *   @return int - index of current audio track in available track list
 */
int PrivateInstanceAAMP::GetAudioTrack()
{
	int idx = -1;
	if (mpStreamAbstractionAAMP)
	{
		idx = mpStreamAbstractionAAMP->GetAudioTrack();
	}
	return idx;
}

#define MUTE_SUBTITLES_TRACKID (-1)

/**
 *   @brief Set text track
 *
 *   @param[in] trackId index of text track in available track list
 *   @return void
 */
void PrivateInstanceAAMP::SetTextTrack(int trackId)
{
	if (mpStreamAbstractionAAMP)
	{
		// Passing in -1 as the track ID mutes subs
		if (MUTE_SUBTITLES_TRACKID == trackId)
		{
			SetCCStatus(false);
			return;
		}

		std::vector<TextTrackInfo> tracks = mpStreamAbstractionAAMP->GetAvailableTextTracks();
		if (!tracks.empty() && (trackId >= 0 && trackId < tracks.size()))
		{
			TextTrackInfo track = tracks[trackId];
			// Check if CC / Subtitle track
			if (track.isCC)
			{
#ifdef AAMP_CC_ENABLED
				if (!track.instreamId.empty())
				{
					CCFormat format = eCLOSEDCAPTION_FORMAT_DEFAULT;
					// AampCCManager expects the CC type, ie 608 or 708
					// For DASH, there is a possibility that instreamId is just an integer so we infer rendition
					if (mMediaFormat == eMEDIAFORMAT_DASH && (std::isdigit(static_cast<unsigned char>(track.instreamId[0])) == 0) && !track.rendition.empty())
					{
						if (track.rendition.find("608") != std::string::npos)
						{
							format = eCLOSEDCAPTION_FORMAT_608;
						}
						else if (track.rendition.find("708") != std::string::npos)
						{
							format = eCLOSEDCAPTION_FORMAT_708;
						}
					}

					// preferredCEA708 overrides whatever we infer from track. USE WITH CAUTION
					if (gpGlobalConfig->preferredCEA708 != eUndefinedState)
					{
						if (gpGlobalConfig->preferredCEA708 == eTrueState)
						{
							format = eCLOSEDCAPTION_FORMAT_708;
						}
						else
						{
							format = eCLOSEDCAPTION_FORMAT_608;
						}
						AAMPLOG_WARN("PrivateInstanceAAMP::%s %d CC format override present, override format to: %d", __FUNCTION__, __LINE__, format);
					}
					AampCCManager::GetInstance()->SetTrack(track.instreamId, format);
				}
				else
				{
					AAMPLOG_ERR("PrivateInstanceAAMP::%s %d Track number/instreamId is empty, skip operation", __FUNCTION__, __LINE__);
				}
#endif
			}
			else
			{
				//Unmute subtitles
				SetCCStatus(true);

				//TODO: Effective handling between subtitle and CC tracks
				int textTrack = mpStreamAbstractionAAMP->GetTextTrack();
				AAMPLOG_WARN("GetPreferredTextTrack %d trackId %d", textTrack, trackId);
				if (trackId != textTrack)
				{
					SetPreferredTextTrack(track);
					discardEnteringLiveEvt = true;

					//Performs a full retune (seek to current position)
					//This ensures that the subtitles are correctly synced
					//for both VOD and LIVE but is quite clunky
					//Smooth switching without pipeline restart is to follow
					seek_pos_seconds = GetPositionMilliseconds() / 1000.0;
					TeardownStream(false);
					TuneHelper(eTUNETYPE_SEEK);

					discardEnteringLiveEvt = false;
				}
			}
		}
	}
}

/**
 *   @brief Switch the subtitle track following a change to the 
 * 			preferredTextTrack
 *
 *   @return void
 */

void PrivateInstanceAAMP::RefreshSubtitles()
{
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->RefreshSubtitles();
	}
}

/**
 *   @brief Get current text track index
 *
 *   @return int - index of current text track in available track list
 */
int PrivateInstanceAAMP::GetTextTrack()
{
	int idx = -1;
#ifdef AAMP_CC_ENABLED
	if (AampCCManager::GetInstance()->GetStatus() && mpStreamAbstractionAAMP)
	{
		std::string trackId = AampCCManager::GetInstance()->GetTrack();
		if (!trackId.empty())
		{
			std::vector<TextTrackInfo> tracks = mpStreamAbstractionAAMP->GetAvailableTextTracks();
			for (auto it = tracks.begin(); it != tracks.end(); it++)
			{
				if (it->instreamId == trackId)
				{
					idx = std::distance(tracks.begin(), it);
				}
			}
		}
	}
#endif
	if (mpStreamAbstractionAAMP && idx == -1 && !subtitles_muted)
	{
		idx = mpStreamAbstractionAAMP->GetTextTrack();
	}
	return idx;
}

/**
 *   @brief Set CC visibility on/off
 *
 *   @param[in] enabled true for CC on, false otherwise
 *   @return void
 */
void PrivateInstanceAAMP::SetCCStatus(bool enabled)
{
#ifdef AAMP_CC_ENABLED
	AampCCManager::GetInstance()->SetStatus(enabled);
#endif

	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->MuteSubtitles(!enabled);
	}
	subtitles_muted = !enabled;
}

/**
 *   @brief Function to notify available audio tracks changed
 *
 *   @return void
 */
void PrivateInstanceAAMP::NotifyAudioTracksChanged()
{
	SendEventAsync(std::make_shared<AAMPEventObject>(AAMP_EVENT_AUDIO_TRACKS_CHANGED));
}

/**
 *   @brief Function to notify available text tracks changed
 *
 *   @return void
 */
void PrivateInstanceAAMP::NotifyTextTracksChanged()
{
	SendEventAsync(std::make_shared<AAMPEventObject>(AAMP_EVENT_TEXT_TRACKS_CHANGED));
}

/**
 *   @brief Set style options for text track rendering
 *
 *   @param[in] options - JSON formatted style options
 *   @return void
 */
void PrivateInstanceAAMP::SetTextStyle(const std::string &options)
{
	//TODO: This can be later extended to subtitle rendering
	// Right now, API is not available for subtitle
#ifdef AAMP_CC_ENABLED
	AampCCManager::GetInstance()->SetStyle(options);
#endif
}

/**
 *   @brief Get style options for text track rendering
 *
 *   @return std::string - JSON formatted style options
 */
std::string PrivateInstanceAAMP::GetTextStyle()
{
	//TODO: This can be later extended to subtitle rendering
	// Right now, API is not available for subtitle
#ifdef AAMP_CC_ENABLED
	return AampCCManager::GetInstance()->GetStyle();
#else
	return std::string();
#endif
}

/**
 *   @brief Check if any active PrivateInstanceAAMP available
 *
 *   @return bool true if available
 */
bool PrivateInstanceAAMP::IsActiveInstancePresent()
{
	return !gActivePrivAAMPs.empty();
}

/**
 * @brief Set profile ramp down limit.
 *
 */
void PrivateInstanceAAMP::SetInitRampdownLimit(int limit)
{
	if (gpGlobalConfig->mInitRampdownLimit >= 0)
	{
		AAMPLOG_INFO("%s:%d Setting Init Rampdown limit from configuration file: %d", __FUNCTION__, __LINE__, gpGlobalConfig->mInitRampdownLimit);
	}
	else
	{
		if (limit >= 0)
		{
			AAMPLOG_INFO("%s:%d Setting Init Rampdown limit : %d", __FUNCTION__, __LINE__, limit);
			gpGlobalConfig->mInitRampdownLimit = limit;
		}
		else
		{
			AAMPLOG_WARN("%s:%d Invalid Init Rampdown limit value %d", __FUNCTION__,__LINE__, limit);
		}
	}
}

/**
 *   @brief Set the session Token for player
 *
 *   @param[in] string - sessionToken
 *   @return void
 */
void PrivateInstanceAAMP::SetSessionToken(std::string &sessionToken)
{
	if (sessionToken.empty())
	{
		AAMPLOG_ERR("%s:%d Failed to set session Token : empty", __FUNCTION__, __LINE__);
	}
	else
	{
		AAMPLOG_INFO("%s:%d Setting Session Token", __FUNCTION__, __LINE__);	
		mSessionToken = sessionToken;
	}
	return;
}

/**
 *   @brief Set discontinuity ignored flag for given track
 *
 *   @return void
 */
void PrivateInstanceAAMP::SetTrackDiscontinuityIgnoredStatus(MediaType track)
{
	mIsDiscontinuityIgnored[track] = true;
}

/**
 *   @brief Check whether the given track discontinuity ignored earlier.
 *
 *   @return true - if the discontinuity already ignored.
 */
bool PrivateInstanceAAMP::IsDiscontinuityIgnoredForOtherTrack(MediaType track)
{
	return (mIsDiscontinuityIgnored[track]);
}

/**
 *   @brief Reset discontinuity ignored flag for audio and video tracks
 *
 *   @return void
 */
void PrivateInstanceAAMP::ResetTrackDiscontinuityIgnoredStatus(void)
{
	mIsDiscontinuityIgnored[eTRACK_VIDEO] = false;
	mIsDiscontinuityIgnored[eTRACK_AUDIO] = false;
}

/**
 *   @brief Set stream format for audio/video tracks
 *
 *   @param[in] videoFormat - video stream format
 *   @param[in] audioFormat - audio stream format
 *   @return void
 */

void PrivateInstanceAAMP::SetStreamFormat(StreamOutputFormat videoFormat, StreamOutputFormat audioFormat)
{
	bool reconfigure = false;
	AAMPLOG_WARN("%s:%d Got format - videoFormat %d and audioFormat %d", __FUNCTION__, __LINE__, videoFormat, audioFormat);

	// 1. Modified Configure() not to recreate all playbins if there is a change in track's format.
	// 2. For a demuxed scenario, this function will be called twice for each audio and video, so double the trouble.
	// Hence call Configure() only for following scenarios to reduce the overhead,
	// i.e FORMAT_INVALID to any KNOWN/FORMAT_UNKNOWN, FORMAT_UNKNOWN to any KNOWN and any FORMAT_KNOWN to FORMAT_KNOWN if it's not same.
	// Truth table
	// mVideFormat   videoFormat  reconfigure
	// *		  INVALID	false
	// INVALID        INVALID       false
	// INVALID        UNKNOWN       true
	// INVALID	  KNOWN         true
	// UNKNOWN	  INVALID	false
	// UNKNOWN        UNKNOWN       false
	// UNKNOWN	  KNOWN		true
	// KNOWN	  INVALID	false
	// KNOWN          UNKNOWN	false
	// KNOWN          KNOWN         true if format changes, false if same
	if (videoFormat != FORMAT_INVALID && mVideoFormat != videoFormat && (videoFormat != FORMAT_UNKNOWN || mVideoFormat == FORMAT_INVALID))
	{
		reconfigure = true;
		mVideoFormat = videoFormat;
	}
	if (audioFormat != FORMAT_INVALID && mAudioFormat != audioFormat && (audioFormat != FORMAT_UNKNOWN || mAudioFormat == FORMAT_INVALID))
	{
		reconfigure = true;
		mAudioFormat = audioFormat;
	}

	if (reconfigure)
	{
		// Configure pipeline as TSProcessor might have detected the actual stream type
		// or even presence of audio
		mStreamSink->Configure(mVideoFormat, mAudioFormat, false);
	}
}

/**
 *   @brief Set video rectangle property
 *
 *   @param[in] video rectangle property
 */
void PrivateInstanceAAMP::EnableVideoRectangle(bool rectProperty)
{
	//  Value can be set only if local override is not available.Local setting takes preference
	if(gpGlobalConfig->mEnableRectPropertyCfg == eUndefinedState)
	{
		// Ideally video rectangle property should be enabled.
		// There exists a scenario with westeros where the compositor handles scaling in parallel
		// and scaling via rectangle property again messes up the co-ordinates.
		// So disable video rectangle property only for those use-case.
		if (rectProperty == false)
		{
			if (mWesterosSinkEnabled)
			{
				mEnableRectPropertyEnabled = rectProperty;
			}
			else
			{
				AAMPLOG_WARN("%s:%d Skipping the configuration value[%d], since westerossink is disabled", __FUNCTION__, __LINE__, rectProperty);
			}
		}
		else
		{
			mEnableRectPropertyEnabled = rectProperty;
		}
	}
	else
	{
		mEnableRectPropertyEnabled = (bool)gpGlobalConfig->mEnableRectPropertyCfg;
	}
}

/**
 *   @brief Enable seekable range values in progress event
 *
 *   @param[in] enabled - true if enabled
 */
void PrivateInstanceAAMP::EnableSeekableRange(bool enabled)
{
	if(gpGlobalConfig->mEnableSeekableRange == eUndefinedState)
	{
		mEnableSeekableRange = enabled;
	}
	else
	{
		mEnableSeekableRange = (bool)gpGlobalConfig->mEnableSeekableRange;
	}
}

/**
*   @brief Disable Content Restrictions - unlock
*   @param[in] grace - seconds from current time, grace period, grace = -1 will allow an unlimited grace period
*   @param[in] time - seconds from current time,time till which the channel need to be kept unlocked
*   @param[in] eventChange - disable restriction handling till next program event boundary
*
*   @return void
*/
void PrivateInstanceAAMP::DisableContentRestrictions(long grace, long time, bool eventChange)
{
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->DisableContentRestrictions(grace, time, eventChange);
#ifdef AAMP_CC_ENABLED
		if (gpGlobalConfig->nativeCCRendering)
		{
			AampCCManager::GetInstance()->SetParentalControlStatus(false);
		}
#endif
	}
}

/**
*   @brief Enable Content Restrictions - lock
*   @return void
*/
void PrivateInstanceAAMP::EnableContentRestrictions()
{
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->EnableContentRestrictions();
	}
}

/**
 *   @brief Enable video PTS reporting in progress event
 *
 *   @param[in] enabled - true if enabled
 */
void PrivateInstanceAAMP::SetReportVideoPTS(bool enabled)
{
	if(gpGlobalConfig->bReportVideoPTS == eUndefinedState)
	{
		mReportVideoPTS = enabled;
	}
	else
	{
		mReportVideoPTS = (bool)gpGlobalConfig->bReportVideoPTS;
	}
}

void PrivateInstanceAAMP::ConfigureWithLocalOptions()
{
	if(gpGlobalConfig->rampdownLimit >= 0)
	{
		mRampDownLimit = gpGlobalConfig->rampdownLimit;
	}
	if(gpGlobalConfig->minBitrate > 0)
	{
		mMinBitrate = gpGlobalConfig->minBitrate;
	}
	if(gpGlobalConfig->maxBitrate > 0)
	{
		mMaxBitrate = gpGlobalConfig->maxBitrate;
	}
	if(gpGlobalConfig->segInjectFailCount > 0)
	{
		mSegInjectFailCount = gpGlobalConfig->segInjectFailCount;
	}
	if(gpGlobalConfig->drmDecryptFailCount > 0)
	{
		mDrmDecryptFailCount = gpGlobalConfig->drmDecryptFailCount;
	}
	if(gpGlobalConfig->abrBufferCheckEnabled != eUndefinedState)
	{
		mABRBufferCheckEnabled = (bool)gpGlobalConfig->abrBufferCheckEnabled;
	}
	if(gpGlobalConfig->useNewDiscontinuity != eUndefinedState)
	{
		mNewAdBreakerEnabled = (bool)gpGlobalConfig->useNewDiscontinuity;
	}
	if (gpGlobalConfig->ckLicenseServerURL != NULL)
	{
		mLicenseServerUrls[eDRM_ClearKey] = std::string(gpGlobalConfig->ckLicenseServerURL);
	}
	if (gpGlobalConfig->licenseServerURL != NULL)
	{
		mLicenseServerUrls[eDRM_MAX_DRMSystems] = std::string(gpGlobalConfig->licenseServerURL);
	}
	if(gpGlobalConfig->disableEC3 != eUndefinedState)
	{
		mDisableEC3 = (bool)gpGlobalConfig->disableEC3;
	}
	if(gpGlobalConfig->disableATMOS != eUndefinedState)
	{
		mDisableATMOS = (bool)gpGlobalConfig->disableATMOS;
	}
	if(gpGlobalConfig->forceEC3 != eUndefinedState)
	{
		mForceEC3 = (bool)gpGlobalConfig->forceEC3;
	}
	if(gpGlobalConfig->gMaxPlaylistCacheSize != 0)
	{
		mCacheMaxSize = gpGlobalConfig->gMaxPlaylistCacheSize ;
	}
	if (gpGlobalConfig->mWesterosSinkConfig != eUndefinedState)
	{
		mWesterosSinkEnabled = (bool)gpGlobalConfig->mWesterosSinkConfig;
	}
	if (gpGlobalConfig->mEnableRectPropertyCfg != eUndefinedState)
	{
		mEnableRectPropertyEnabled = (bool)gpGlobalConfig->mEnableRectPropertyCfg;
	}
	if(gpGlobalConfig->tunedEventConfigVOD != eTUNED_EVENT_MAX)
	{
		mTuneEventConfigVod = gpGlobalConfig->tunedEventConfigVOD;
	}
	if(gpGlobalConfig->tunedEventConfigLive != eTUNED_EVENT_MAX)
	{
		mTuneEventConfigLive = gpGlobalConfig->tunedEventConfigLive;
	}
	if(gpGlobalConfig->mEnableSeekableRange != eUndefinedState)
	{
		mEnableSeekableRange = gpGlobalConfig->mEnableSeekableRange;
	}
	if(gpGlobalConfig->bReportVideoPTS != eUndefinedState)
	{
		mReportVideoPTS = gpGlobalConfig->bReportVideoPTS;
	}
	if(gpGlobalConfig->mPersistBitRateOverSeek != eUndefinedState)
	{
		mPersistBitRateOverSeek = gpGlobalConfig->mPersistBitRateOverSeek;
	}

}
/**
 * @brief Set Maximum Cache Size for playlist store
 *
 */
void PrivateInstanceAAMP::SetMaxPlaylistCacheSize(int cacheSize)
{
	mCacheMaxSize = cacheSize * 1024 ;
}

/**
 *   @brief Enable/disable configuration to persist ABR profile over SAP/Seek
 *
 *   @param[in] value - To enable/disable configuration
 *   @return void
 */
void PrivateInstanceAAMP::PersistBitRateOverSeek(bool value)
{
	if(gpGlobalConfig->mPersistBitRateOverSeek == eUndefinedState)
	{
		mPersistBitRateOverSeek = value;
	}
	else
	{
		mPersistBitRateOverSeek = (bool)gpGlobalConfig->mPersistBitRateOverSeek;
	}
}

/**
 * EOF
 */
