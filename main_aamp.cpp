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
 * @file main_aamp.cpp
 * @brief Advanced Adaptive Media Player (AAMP)
 */

#include <sys/time.h>
#ifndef DISABLE_DASH
#include "fragmentcollector_mpd.h"
#endif
#include "fragmentcollector_hls.h"
#include "_base64.h"
#include "base16.h"
#include "aampgstplayer.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "drm.h"
#include <unistd.h>
#include "priv_aamp.h"
#include <signal.h>
#include <semaphore.h>
#include <glib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <vector>
#include <list>
#ifdef AAMP_CC_ENABLED
#include "ccDataReader.h"
#include "vlCCConstants.h"
#include "cc_util.h"
#include "vlGfxScreen.h"
#endif
#include <uuid/uuid.h>
static const char* strAAMPPipeName = "/tmp/ipc_aamp";
#ifdef WIN32
#include "conio.h"
#else
#include <termios.h>
#include <errno.h>

#ifdef IARM_MGR
#include "host.hpp"
#include "manager.hpp"
#include "libIBus.h"
#include "libIBusDaemon.h"

#include <hostIf_tr69ReqHandler.h>
#include <sstream>

/**
 * @brief
 * @param paramName
 * @param iConfigLen
 * @retval
 */
char * GetTR181AAMPConfig(const char * paramName, size_t & iConfigLen);
#endif


/**
 * @brief get a character for console
 * @retval user input character
 */
int getch(void)
{ // for linux
	struct termios oldattr, newattr;
	int ch;
	tcgetattr(STDIN_FILENO, &oldattr);
	newattr = oldattr;
	newattr.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
	ch = getchar();
	tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
	return ch;
}
#endif

#define ARRAY_SIZE(A) ((int)(sizeof(A)/sizeof(A[0])))

/**
 * @struct AsyncEventDescriptor
 * @brief Used in asynchronous event notification logic
 */
struct AsyncEventDescriptor
{
	AAMPEvent event;
	PrivateInstanceAAMP* aamp;
};

static TuneFailureMap tuneFailureMap[] =
{
	{AAMP_TUNE_INIT_FAILED, 10, "AAMP: init failed"}, //"Fragmentcollector initialization failed"
	{AAMP_TUNE_MANIFEST_REQ_FAILED, 10, "AAMP: Manifest Download failed"}, //"Playlist refresh failed"
	{AAMP_TUNE_AUTHORISATION_FAILURE, 40, "AAMP: Authorization failure"},
	{AAMP_TUNE_FRAGMENT_DOWNLOAD_FAILURE, 10, "AAMP: fragment download failures"},
	{AAMP_TUNE_INIT_FRAGMENT_DOWNLOAD_FAILURE, 10, "AAMP: init fragment download failed"},
	{AAMP_TUNE_UNTRACKED_DRM_ERROR, 50, "AAMP: DRM error untracked error"},
	{AAMP_TUNE_DRM_INIT_FAILED, 50, "AAMP: DRM Initialization Failed"},
	{AAMP_TUNE_DRM_DATA_BIND_FAILED, 50, "AAMP: InitData-DRM Binding Failed"},
	{AAMP_TUNE_DRM_CHALLENGE_FAILED, 50, "AAMP: DRM License Challenge Generation Failed"},
	{AAMP_TUNE_LICENCE_TIMEOUT, 50, "AAMP: DRM License Request Timed out"},
	{AAMP_TUNE_LICENCE_REQUEST_FAILED, 50, "AAMP: DRM License Request Failed"},
	{AAMP_TUNE_INVALID_DRM_KEY, 50, "AAMP: Invalid Key Error, from DRM"},
	{AAMP_TUNE_UNSUPPORTED_STREAM_TYPE, 50, "AAMP: Unsupported Stream Type"}, //"Unable to determine stream type for DRM Init"
	{AAMP_TUNE_FAILED_TO_GET_KEYID, 50, "AAMP: Failed to parse key id from PSSH"},
	{AAMP_TUNE_FAILED_TO_GET_ACCESS_TOKEN, 50, "AAMP: Failed to get access token from Auth Service"},
	{AAMP_TUNE_CORRUPT_DRM_DATA, 51, "AAMP: DRM failure due to Corrupt DRM files"},
	{AAMP_TUNE_DRM_DECRYPT_FAILED, 50, "AAMP: DRM Decryption Failed for Fragments"},
	{AAMP_TUNE_GST_PIPELINE_ERROR, 80, "AAMP: Error from gstreamer pipeline"},
	{AAMP_TUNE_PLAYBACK_STALLED, 7600, "AAMP: Playback was stalled due to lack of new fragments"},
	{AAMP_TUNE_CONTENT_NOT_FOUND, 20, "AAMP: Resource was not found at the URL(HTTP 404)"},
	{AAMP_TUNE_DRM_KEY_UPDATE_FAILED, 50, "AAMP: Failed to process DRM key"},
	{AAMP_TUNE_DEVICE_NOT_PROVISIONED, 52, "AAMP: Device not provisioned"},
	{AAMP_TUNE_FAILURE_UNKNOWN, 100, "AAMP: Unknown Failure"}
};

/**
 * @struct ChannelInfo 
 * @brief Holds information of a channel
 */
struct ChannelInfo
{
	int channelNumber;
	std::string name;
	std::string uri;
};

static std::list<ChannelInfo> mChannelMap;

#ifdef STANDALONE_AAMP
static PlayerInstanceAAMP *mSingleton;
#endif

GlobalConfigAAMP *gpGlobalConfig;

#define LOCAL_HOST_IP       "127.0.0.1"
#define STR_PROXY_BUFF_SIZE  64
#define AAMP_MAX_SIMULTANEOUS_INSTANCES 2
#define AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS (20*1000LL)

#define VALIDATE_INT(param_name, param_value, default_value)        \
    if ((param_value <= 0) || (param_value > INT_MAX))  { \
        logprintf("%s(): Parameter '%s' not within INTEGER limit. Using default value instead.\n", __FUNCTION__, param_name); \
        param_value = default_value; \
    }

#define VALIDATE_LONG(param_name, param_value, default_value)        \
    if ((param_value <= 0) || (param_value > LONG_MAX))  { \
        logprintf("%s(): Parameter '%s' not within LONG INTEGER limit. Using default value instead.\n", __FUNCTION__, param_name); \
        param_value = default_value; \
    }

static struct
{
	PrivateInstanceAAMP* pAAMP;
	bool reTune;
	int numPtsErrors;
} gActivePrivAAMPs[AAMP_MAX_SIMULTANEOUS_INSTANCES] = { { NULL, false, 0 }, { NULL, false, 0 } };

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gCond = PTHREAD_COND_INITIALIZER;

#ifdef AAMP_CC_ENABLED

//Required for now, since we use config params to switch CC on/off

/**
 * @brief Checks if CC enabled in config params
 * @retval true if CC is enabled
 */
bool aamp_IsCCEnabled(void)
{
	if (gpGlobalConfig)
	{
		return gpGlobalConfig->aamp_GetCCStatus();
	}

	return false;
}


/**
 * @brief Initialize CC resource. Rendering is disabled by default
 * @param handle decoder handle
 * @retval 0 on sucess, -1 on failure
 */
int aamp_CCStart(void *handle)
{
	int ret = -1;
	static bool initStatus = false;
	if (!initStatus)
	{
		vlGfxInit(0);
		ret = vlMpeosCCManagerInit();
		if (ret != 0)
		{
			return ret;
		}
		initStatus = true;
	}

	if (handle == NULL)
	{
		return -1;
	}

	media_closeCaptionStart((void *)handle);

	//set CC off initially
	ret = ccSetCCState(CCStatus_OFF, 0);

	return ret;
}


/**
 * @brief Destroy CC resource
 */
void aamp_CCStop(void)
{
	media_closeCaptionStop();
}


/**
 * @brief Start CC Rendering
 * @retval 0 on success
 */
int aamp_CCShow(void)
{
	return ccSetCCState(CCStatus_ON, 0);
}


/**
 * @brief Stop CC Rendering
 * @retval 0 on success
 */
int aamp_CCHide(void)
{
	return ccSetCCState(CCStatus_OFF, 0);
}
#endif // AAMP_CC_ENABLED

/**
 * @brief Get ID of DRM system
 * @param drmSystem drm system
 * @retval ID of the DRM system, empty string if not supported
 */
const char * GetDrmSystemID(DRMSystems drmSystem)
{
	switch(drmSystem)
	{
	case eDRM_WideVine:
		return "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
	case eDRM_PlayReady:
		return "9a04f079-9840-4286-ab92-e65be0885f95";
	case eDRM_CONSEC_agnostic:
		return "afbcb50e-bf74-3d13-be8f-13930c783962";
	}
	return "";
}

/**
 * @brief Get name of DRM system
 * @param drmSystem drm system
 * @retval Name of the DRM system, empty string if not supported
 */
const char * GetDrmSystemName(DRMSystems drmSystem)
{
	switch(drmSystem)
	{
	case eDRM_WideVine:
		return "WideWine";
	case eDRM_PlayReady:
		return "PlayReady";
	case eDRM_CONSEC_agnostic:
		return "Consec Agnostic";
	case eDRM_Adobe_Access:
		return "Adobe Access";
	case eDRM_Vanilla_AES:
		return "Vanilla AES";
	}
	return "";
}

/**
 * @brief Get current time stamp
 * @retval current clock time as milliseconds
 */
long long aamp_GetCurrentTimeMS(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return (long long)(t.tv_sec*1e3 + t.tv_usec*1e-3);
}


/**
 * @brief Report progress event to listeners
 */
void PrivateInstanceAAMP::ReportProgress(void)
{
	//if (mPlayerState.durationMilliseconds > 0)
	if (mDownloadsEnabled)
	{
		AAMPEvent eventData;
		eventData.type = AAMP_EVENT_PROGRESS;
		if (!mPlayingAd)
		{
			eventData.data.progress.positionMiliseconds = (seek_pos_seconds) * 1000.0;
			if (!pipeline_paused && trickStartUTCMS >= 0)
			{
				long long elapsedTime = aamp_GetCurrentTimeMS() - trickStartUTCMS;
				eventData.data.progress.positionMiliseconds += elapsedTime * rate;
				// note, using StreamSink::GetPositionMilliseconds() instead of elapsedTime
				// would likely be more accurate, but would need to be tested to accomodate
				// and compensate for FF/REW play rates
			}
		}
		else
		{
			eventData.data.progress.positionMiliseconds = mAdPosition * 1000.0;
		}
		eventData.data.progress.durationMiliseconds = durationSeconds*1000.0;

		//If tsb is not available for linear send -1  for start and end
		// so that xre detect this as tsbless playabck
		if( mContentType == ContentType_LINEAR && !mTSBEnabled)
		{
            eventData.data.progress.startMiliseconds = -1;
            eventData.data.progress.endMiliseconds = -1;
		}
		else
		{
		    eventData.data.progress.startMiliseconds = culledSeconds*1000.0;
		    eventData.data.progress.endMiliseconds = eventData.data.progress.startMiliseconds + eventData.data.progress.durationMiliseconds;
		}

		eventData.data.progress.playbackSpeed = pipeline_paused ? 0 : rate;

		if (eventData.data.progress.positionMiliseconds > eventData.data.progress.endMiliseconds)
		{ // clamp end
			//logprintf("aamp clamp end\n");
			eventData.data.progress.positionMiliseconds = eventData.data.progress.endMiliseconds;
		}
		else if (eventData.data.progress.positionMiliseconds < eventData.data.progress.startMiliseconds)
		{ // clamp start
			//logprintf("aamp clamp start\n");
			eventData.data.progress.positionMiliseconds = eventData.data.progress.startMiliseconds;
		}

		if (gpGlobalConfig->logging.progress)
		{
			static int tick;
			if ((tick++ & 3) == 0)
			{
				logprintf("aamp pos: [%ld..%ld..%ld]\n",
					(long)(eventData.data.progress.startMiliseconds / 1000),
					(long)(eventData.data.progress.positionMiliseconds / 1000),
					(long)(eventData.data.progress.endMiliseconds / 1000));
			}
		}
		mReportProgressPosn = eventData.data.progress.positionMiliseconds;
		SendEventSync(eventData);
		mReportProgressTime = aamp_GetCurrentTimeMS();
	}
}

/**
* @brief called from fragmentcollector_hls::IndexPlaylist to update TSB duration
*/

/**
 * @brief Update duration of stream
 * @param seconds duration in seconds
 */
void PrivateInstanceAAMP::UpdateDuration(double seconds)
{
	if(!mPlayingAd)
	{
		AAMPLOG_INFO("aamp_UpdateDuration(%f)\n", seconds);
		durationSeconds = seconds;
	}
}


/**
 * @brief Idle task to resume aamp
 * @param ptr pointer to PrivateInstanceAAMP object
 * @retval G_SOURCE_REMOVE
 */
static gboolean PrivateInstanceAAMP_Resume(gpointer ptr)
{
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP* )ptr;
	aamp->NotifyFirstBufferProcessed();
	if (aamp->pipeline_paused)
	{
		if (aamp->rate == 1.0)
		{
			aamp->mStreamSink->Pause(false);
			aamp->pipeline_paused = false;
		}
		else
		{
			aamp->rate = 1.0;
			aamp->pipeline_paused = false;
			aamp->TuneHelper(eTUNETYPE_SEEK);
		}
		aamp->ResumeDownloads();
		aamp->NotifySpeedChanged(aamp->rate);
	}
	return G_SOURCE_REMOVE;
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
		logprintf("PrivateInstanceAAMP::%s - culling started, first value %f\n", __FUNCTION__, culledSecs);
	}
	this->culledSeconds += culledSecs;

	// Check if we are paused and culled past paused playback position
	// AAMP internally caches fragments in sw and gst buffer, so we should be good here
	if (pipeline_paused && mpStreamAbstractionAAMP)
	{
		double minPlaylistPositionToResume = (seek_pos_seconds < maxRefreshPlaylistIntervalSecs) ? seek_pos_seconds : (seek_pos_seconds - maxRefreshPlaylistIntervalSecs);
		if (this->culledSeconds >= seek_pos_seconds)
		{
			logprintf("%s(): Resume playback since playlist start position(%f) has moved past paused position(%f) \n", __FUNCTION__, this->culledSeconds, seek_pos_seconds);
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
				logprintf("%s(): Resume playback since start position(%f) moved very close to minimum resume position(%f) \n", __FUNCTION__, this->culledSeconds, minPlaylistPositionToResume);
				g_idle_add(PrivateInstanceAAMP_Resume, (gpointer)this);
			}
		}

	}
}


/**
 * @brief Update playlist refresh interval
 * @param maxIntervalSecs refresh interval in seconds
 */
void PrivateInstanceAAMP::UpdateRefreshPlaylistInterval(float maxIntervalSecs)
{
	AAMPLOG_INFO("%s(): maxRefreshPlaylistIntervalSecs (%f)\n", __FUNCTION__, maxIntervalSecs);
	maxRefreshPlaylistIntervalSecs = maxIntervalSecs;
}


/**
 * @brief Add listener to aamp events
 * @param eventType type of event to subscribe
 * @param eventListener listener
 */
void PrivateInstanceAAMP::AddEventListener(AAMPEventType eventType, AAMPEventListener* eventListener)
{
	//logprintf("[AAMP_JS] %s(%d, %p)\n", __FUNCTION__, eventType, eventListener);
	if ((eventListener != NULL) && (eventType >= 0) && (eventType < AAMP_MAX_NUM_EVENTS))
	{
		ListenerData* pListener = new ListenerData;
		if (pListener)
		{
			//logprintf("[AAMP_JS] %s(%d, %p) new %p\n", __FUNCTION__, eventType, eventListener, pListener);
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
void PrivateInstanceAAMP::RemoveEventListener(AAMPEventType eventType, AAMPEventListener* eventListener)
{
	logprintf("[AAMP_JS] %s(%d, %p)\n", __FUNCTION__, eventType, eventListener);
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
				logprintf("[AAMP_JS] %s(%d, %p) delete %p\n", __FUNCTION__, eventType, eventListener, pListener);
				delete pListener;
				return;
			}
			ppLast = &(pListener->pNext);
		}
		pthread_mutex_unlock(&mLock);
	}
}


/**
 * @brief Handles download errors and sends events to application if required.
 * @param tuneFailure Reason of error
 * @param error_code HTTP error code/ CURLcode
 */
void PrivateInstanceAAMP::SendDownloadErrorEvent(AAMPTuneFailure tuneFailure,long error_code)
{
	AAMPTuneFailure actualFailure = tuneFailure;

	if(tuneFailure >= 0 && tuneFailure < AAMP_TUNE_FAILURE_UNKNOWN)
	{
		char description[128] = {};
		if(error_code < 100)
		{
			sprintf(description, "%s : Curl Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
		}
		else
		{
			sprintf(description, "%s : Http Error Code %ld", tuneFailureMap[tuneFailure].description, error_code);
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
		logprintf("%s:%d : Received unknown error event %d\n", __FUNCTION__, __LINE__, tuneFailure);
		SendErrorEvent(AAMP_TUNE_FAILURE_UNKNOWN);
	}
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
		sendErrorEvent = true;
		mState = eSTATE_ERROR;
	}
	pthread_mutex_unlock(&mLock);
	if (sendErrorEvent)
	{
		AAMPEvent e;
		e.type = AAMP_EVENT_TUNE_FAILED;
		e.data.mediaError.shouldRetry = isRetryEnabled;
		const char *errorDescription = NULL;
		DisableDownloads();
		if(tuneFailure >= 0 && tuneFailure < AAMP_TUNE_FAILURE_UNKNOWN)
		{
			if (tuneFailure == AAMP_TUNE_PLAYBACK_STALLED)
			{ // allow config override for stall detection error code
				e.data.mediaError.code = gpGlobalConfig->stallErrorCode;
			}
			else
			{
				e.data.mediaError.code = tuneFailureMap[tuneFailure].code;
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
			e.data.mediaError.code = tuneFailureMap[AAMP_TUNE_FAILURE_UNKNOWN].code;
			errorDescription = tuneFailureMap[AAMP_TUNE_FAILURE_UNKNOWN].description;
		}

		strncpy(e.data.mediaError.description, errorDescription, MAX_ERROR_DESCRIPTION_LENGTH);
		e.data.mediaError.description[MAX_ERROR_DESCRIPTION_LENGTH - 1] = '\0';
		logprintf("Sending error %s \n",e.data.mediaError.description);
		SendEventAsync(e);
	}
	else
	{
		logprintf("PrivateInstanceAAMP::%s:%d Ignore error %d[%s]\n", __FUNCTION__, __LINE__, (int)tuneFailure, description);
	}
}


/**
 * @brief Send event asynchronously to listeners
 * @param e event
 */
void PrivateInstanceAAMP::SendEventAsync(const AAMPEvent &e)
{
	if (mEventListener || mEventListeners[0] || mEventListeners[e.type])
	{
		AsyncEventDescriptor* aed = new AsyncEventDescriptor();
		aed->event = e;
		ScheduleEvent(aed);
		if(e.type != AAMP_EVENT_PROGRESS)
			AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d event type  %d\n", __FUNCTION__, __LINE__,e.type);
	}
}


/**
 * @brief Send event synchronously to listeners
 * @param e event
 */
void PrivateInstanceAAMP::SendEventSync(const AAMPEvent &e)
{
	if(e.type != AAMP_EVENT_PROGRESS)
		AAMPLOG_INFO("[AAMP_JS] %s(type=%d)\n", __FUNCTION__, e.type);

	//TODO protect mEventListener
	if (mEventListener)
	{
		mEventListener->Event(e);
	}

	AAMPEventType eventType = e.type;
	if ((eventType < 0) && (eventType >= AAMP_MAX_NUM_EVENTS))
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
			//logprintf("[AAMP_JS] %s(type=%d) listener=%p\n", __FUNCTION__, eventType, pCurrent->eventListener);
			pCurrent->eventListener->Event(e);
		}
		pList = pCurrent->pNext;
		delete pCurrent;
	}
}

/**
 * @brief Notify bitrate change event to listeners
 * @param bitrate new bitrate
 * @param description description of rate change
 * @param width new width in pixels
 * @param height new height in pixels
 * @param GetBWIndex get bandwidth index - used for logging
 */
void PrivateInstanceAAMP::NotifyBitRateChangeEvent(int bitrate ,const char *description ,int width ,int height, bool GetBWIndex)
{
	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_BITRATE_CHANGED])
	{
		AsyncEventDescriptor* e = new AsyncEventDescriptor();
		e->event.type = AAMP_EVENT_BITRATE_CHANGED;
		e->event.data.bitrateChanged.time               =       (int)aamp_GetCurrentTimeMS();
		e->event.data.bitrateChanged.bitrate            =       bitrate;
		strncpy(e->event.data.bitrateChanged.description,description,sizeof(e->event.data.bitrateChanged.description));
		e->event.data.bitrateChanged.width              =       width;
		e->event.data.bitrateChanged.height             =       height;

		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		if(GetBWIndex && (mpStreamAbstractionAAMP != NULL))
		{
			logprintf("NotifyBitRateChangeEvent :: bitrate:%d desc:%s width:%d height:%d, IndexFromTopProfile: %d%s\n",bitrate,description,width,height, mpStreamAbstractionAAMP->GetBWIndex(bitrate), (IsTSBSupported()? ", fog": " "));
		}
		else
		{
			logprintf("NotifyBitRateChangeEvent :: bitrate:%d desc:%s width:%d height:%d%s\n",bitrate,description,width,height, (IsTSBSupported()? ", fog": " "));
		}
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */

		ScheduleEvent(e);
	}
	else
	{
		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		if(GetBWIndex && (mpStreamAbstractionAAMP != NULL))
		{
			logprintf("NotifyBitRateChangeEvent ::NO LISTENERS bitrate:%d desc:%s width:%d height:%d, IndexFromTopProfile: %d%s\n",bitrate,description,width,height, mpStreamAbstractionAAMP->GetBWIndex(bitrate), (IsTSBSupported()? ", fog": " "));
		}
		else
		{
			logprintf("NotifyBitRateChangeEvent ::NO LISTENERS bitrate:%d desc:%s width:%d height:%d%s\n",bitrate,description,width,height, (IsTSBSupported()? ", fog": " "));
		}
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */
	}
}


/**
 * @brief Notify rate change event to listeners
 * @param rate new speed
 */
void PrivateInstanceAAMP::NotifySpeedChanged(float rate)
{
	if (rate == 0)
	{
		SetState(eSTATE_PAUSED);
	}
	else if (rate == 1.0)
	{
		SetState(eSTATE_PLAYING);
	}

	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_SPEED_CHANGED])
	{
		AsyncEventDescriptor* e = new AsyncEventDescriptor();
		e->event.type = AAMP_EVENT_SPEED_CHANGED;
		e->event.data.speedChanged.rate = rate;
		ScheduleEvent(e);
	}
}


void PrivateInstanceAAMP::SendDRMMetaData(const AAMPEvent &e)
{

        SendEventSync(e);
        logprintf("SendDRMMetaData name = %s value = %x\n",e.data.dash_drmmetadata.accessStatus,e.data.dash_drmmetadata.accessStatus_value);
}




/**
 * @brief
 * @param ptr
 * @retval
 */
static gboolean PrivateInstanceAAMP_ProcessDiscontinuity(gpointer ptr)
{
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP*) ptr;

	if (!g_source_is_destroyed(g_main_current_source()))
	{
		aamp->ProcessPendingDiscontinuity();

		aamp->SyncBegin();
		aamp->mDiscontinuityTuneOperationId = 0;
		pthread_cond_signal(&aamp->mCondDiscontinuity);
		aamp->SyncEnd();
	}
	return G_SOURCE_REMOVE;
}


/**
 * @brief Check if discontinuity processing is pending
 * @retval true if discontinuity processing is pending
 */
bool PrivateInstanceAAMP::IsDiscontinuityProcessPending()
{
	bool ret = false;
	if (mProcessingDiscontinuity || mProcessingAdInsertion || mPlayingAd)
	{
		ret = true;
	}
	return ret;
}


/**
 * @brief Process pending discontinuity and continue playback of stream after discontinuity
 */
void PrivateInstanceAAMP::ProcessPendingDiscontinuity()
{
	SyncBegin();
	if (mDiscontinuityTuneOperationInProgress)
	{
		SyncEnd();
		logprintf("PrivateInstanceAAMP::%s:%d Discontinuity Tune Operation already in progress\n", __FUNCTION__, __LINE__);
		return;
	}
	SyncEnd();

	if (!(mProcessingDiscontinuity || mProcessingAdInsertion || mPlayingAd))
	{
		return;
	}

	SyncBegin();
	mDiscontinuityTuneOperationInProgress = true;
	SyncEnd();

	if (mProcessingDiscontinuity)
	{
		logprintf("PrivateInstanceAAMP::%s:%d mProcessingDiscontinuity set\n", __FUNCTION__, __LINE__);
		lastUnderFlowTimeMs[eMEDIATYPE_VIDEO] = 0;
		lastUnderFlowTimeMs[eMEDIATYPE_AUDIO] = 0;
		mpStreamAbstractionAAMP->Stop(false);
#ifndef AAMP_STOP_SINK_ON_SEEK
		mStreamSink->Flush(mpStreamAbstractionAAMP->GetFirstPTS(), rate);
#else
		mStreamSink->Stop(true);
#endif
		mpStreamAbstractionAAMP->GetStreamFormat(mFormat, mAudioFormat);
		mStreamSink->Configure(mFormat, mAudioFormat, false);
		mpStreamAbstractionAAMP->Start();
		mStreamSink->Stream();
		mProcessingDiscontinuity = false;
	}
	else
	{
		if (mProcessingAdInsertion)
		{
			logprintf("PrivateInstanceAAMP::%s:%d mProcessingAdInsertion set\n", __FUNCTION__, __LINE__);
			mProcessingAdInsertion = false;
			if(mAdUrl[0])
			{
				mPlayingAd = true;
				logprintf("PrivateInstanceAAMP::%s:%d  Play ad from start\n", __FUNCTION__, __LINE__);
				TuneHelper(eTUNETYPE_NEW_NORMAL);
			}
			else
			{
				logprintf("PrivateInstanceAAMP::%s:%d  invalid ad url\n", __FUNCTION__, __LINE__);
			}
		}
		else if (mPlayingAd)
		{
			logprintf("PrivateInstanceAAMP::%s:%d  Completed ad playback - seek to ad-position\n", __FUNCTION__, __LINE__);
			mPlayingAd = false;
			seek_pos_seconds = mAdPosition;
			TuneHelper(eTUNETYPE_NEW_SEEK);
		}
	}

	SyncBegin();
	mDiscontinuityTuneOperationInProgress = false;
	SyncEnd();
}

/**
 * @brief Process EOS from Sink and notify listeners if required
 */
void PrivateInstanceAAMP::NotifyEOSReached()
{
	logprintf("%s: Enter . processingDiscontinuity %d\n",__FUNCTION__, mProcessingDiscontinuity);
	if (!IsLive() && rate > 0 && (!mProcessingDiscontinuity) && (!mProcessingAdInsertion) &&(!mPlayingAd))
	{
		SetState(eSTATE_COMPLETE);
		SendEventAsync(AAMP_EVENT_EOS);
		if (ContentType_EAS == mContentType) //Fix for DELIA-25590
		{
			mStreamSink->Stop(false);
		}
		return;
	}
	if (!IsDiscontinuityProcessPending())
	{
		if (rate < 0)
		{
			seek_pos_seconds = culledSeconds;
			logprintf("%s:%d Updated seek_pos_seconds %f \n", __FUNCTION__,__LINE__, seek_pos_seconds);
			rate = 1.0;
			TuneHelper(eTUNETYPE_SEEK);
		}
		else
		{
			rate = 1.0;
			TuneHelper(eTUNETYPE_SEEKTOLIVE);
		}

		NotifySpeedChanged(rate);
	}
	else
	{
		ProcessPendingDiscontinuity();
		logprintf("PrivateInstanceAAMP::%s:%d  EOS due to discontinuity handled\n", __FUNCTION__, __LINE__);
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
	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_ENTERING_LIVE])
	{
		SendEventAsync(AAMP_EVENT_ENTERING_LIVE);
	}
}

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
	if(e->event.type != AAMP_EVENT_PROGRESS)
		AAMPLOG_INFO("PrivateInstanceAAMP::%s:%d event type  %d\n", __FUNCTION__, __LINE__,e->event.type);
	//Get current idle handler's id
	gint callbackID = g_source_get_id(g_main_current_source());
	e->aamp->SetCallbackAsDispatched(callbackID);

	e->aamp->SendEventSync(e->event);
	return G_SOURCE_REMOVE;
}


/**
 * @brief Schedule asynchronous event
 * @param e event descriptor
 */
void PrivateInstanceAAMP::ScheduleEvent(AsyncEventDescriptor* e)
{
	//TODO protect mEventListener
	e->aamp = this;
	gint callbackID = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, SendAsynchronousEvent, e, AsyncEventDestroyNotify);
	SetCallbackAsPending(callbackID);
}


/**
 * @brief Notify tune end for profiling/logging
 */
void PrivateInstanceAAMP::LogTuneComplete(void)
{
	bool success = true; // TODO
	int streamType = getStreamType();
	profiler.TuneEnd(success, mContentType, streamType, mFirstTune);

	if (!mTuneCompleted)
	{
		char classicTuneStr[AAMP_MAX_PIPE_DATA_SIZE];
		profiler.GetClassicTuneTimeInfo(success, mTuneAttempts, mfirstTuneFmt, mPlayerLoadTime, streamType, IsLive(), durationSeconds, classicTuneStr);
#ifndef STANDALONE_AAMP
		SetupPipeSession();
		SendMessageOverPipe((const char *) classicTuneStr, (int) strlen(classicTuneStr));
		ClosePipeSession();
#endif
		mTuneCompleted = true;
		mFirstTune = false;
		TunedEventConfig tunedEventConfig = IsLive() ? gpGlobalConfig->tunedEventConfigLive : gpGlobalConfig->tunedEventConfigVOD;
		if (eTUNED_EVENT_ON_GST_PLAYING == tunedEventConfig)
		{
			if (SendTunedEvent())
			{
				logprintf("aamp: - sent TUNED event on Tune Completion.\n");
			}
		}
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
 * @brief Log errors.
 * @param msg Error message
 */
void aamp_Error(const char *msg)
{
	logprintf("aamp ERROR: %s\n", msg);
	//exit(1);
}


/**
 * @brief Free memory allocated by aamp_Malloc
 * @param[in][out] pptr Pointer to allocated memory
 */
void aamp_Free(char **pptr)
{
	void *ptr = *pptr;
	if (ptr)
	{
		g_free(ptr);
		*pptr = NULL;
	}
}


/**
 * @brief Stop downloads of all tracks.
 * Used by aamp internally to manage states
 */
void PrivateInstanceAAMP::StopDownloads()
{
	traceprintf ("PrivateInstanceAAMP::%s\n", __FUNCTION__);
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
	traceprintf ("PrivateInstanceAAMP::%s\n", __FUNCTION__);
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
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d\n", __FUNCTION__, (int) type);
	}
#endif
	if (!mbTrackDownloadsBlocked[type])
	{
		AAMPLOG_TRACE("gstreamer-enough-data from %s source\n", (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mbTrackDownloadsBlocked[type] = true;
		pthread_mutex_unlock(&mLock);
	}
	traceprintf ("PrivateInstanceAAMP::%s Enter. type = %d\n", __FUNCTION__, (int) type);
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
		logprintf ("PrivateInstanceAAMP::%s Enter. type = %d\n", __FUNCTION__, (int) type);
	}
#endif
	if (mbTrackDownloadsBlocked[type])
	{
		AAMPLOG_TRACE("gstreamer-needs-data from %s source\n", (type == eMEDIATYPE_AUDIO) ? "audio" : "video");
		pthread_mutex_lock(&mLock);
		mbTrackDownloadsBlocked[type] = false;
		//log_current_time("gstreamer-needs-data");
		pthread_mutex_unlock(&mLock);
	}
	traceprintf ("PrivateInstanceAAMP::%s Exit. type = %d\n", __FUNCTION__, (int) type);
}

/**
 * @brief block until gstreamer indicates pipeline wants more data
 * @param cb callback called periodically, if non-null
 * @param periodMs delay between callbacks
 * @param track track index
 */
void PrivateInstanceAAMP::BlockUntilGstreamerWantsData(void(*cb)(void), int periodMs, int track)
{ // called from FragmentCollector thread; blocks until gstreamer wants data
	traceprintf( "PrivateInstanceAAMP::%s Enter. type = %d\n", __FUNCTION__, track);
	int elapsedMs = 0;
	while (mbDownloadsBlocked || mbTrackDownloadsBlocked[track])
	{
		if (!mDownloadsEnabled)
		{
			logprintf("PrivateInstanceAAMP::%s interrupted\n", __FUNCTION__);
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
	traceprintf ("PrivateInstanceAAMP::%s Exit. type = %d\n", __FUNCTION__, track);
}


/**
 * @brief Allocate memory to growable buffer
 * @param buffer growable buffer
 * @param len size of memory to be allocated
 */
void aamp_Malloc(struct GrowableBuffer *buffer, size_t len)
{
	assert(!buffer->ptr && !buffer->avail );
	buffer->ptr = (char *)g_malloc(len);
	buffer->avail = len;
}


/**
 * @brief Append data to buffer
 * @param buffer Growable buffer object pointer
 * @param ptr Buffer to append
 * @param len Buffer size
 */
void aamp_AppendBytes(struct GrowableBuffer *buffer, const void *ptr, size_t len)
{
	size_t required = buffer->len + len;
	if (required > buffer->avail)
	{
		if(buffer->avail > (128*1024))
		{
			AAMPLOG_INFO("%s:%d realloc. buf %p avail %d required %d\n", __FUNCTION__, __LINE__, buffer, (int)buffer->avail, (int)required);
		}
		buffer->avail = required * 2; // grow generously to minimize realloc overhead
		char *ptr = (char *)g_realloc(buffer->ptr, buffer->avail);
		assert(ptr);
		if (ptr)
		{
			if (buffer->ptr == NULL)
			{ // first alloc (not a realloc)
			}
			buffer->ptr = ptr;
		}
	}
	if (buffer->ptr)
	{
		memcpy(&buffer->ptr[buffer->len], ptr, len);
		buffer->len = required;
	}
}

/**
 * @struct WriteContext
 * @brief context during curl write callback
 */
struct WriteContext
{
	PrivateInstanceAAMP *aamp;
	GrowableBuffer *buffer;
};

/**
 * @brief write callback to be used by CURL
 * @param ptr pointer to buffer containing the data
 * @param size size of the buffer
 * @param nmemb number of bytes
 * @param userdata WriteContext pointer
 * @retval size consumed or 0 if interrupted
 */
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t ret = 0;
	struct WriteContext *context = (struct WriteContext *)userdata;
	pthread_mutex_lock(&context->aamp->mLock);
	if (context->aamp->mDownloadsEnabled)
	{
		size_t numBytesForBlock = size*nmemb;
		aamp_AppendBytes(context->buffer, ptr, numBytesForBlock);
		ret = numBytesForBlock;
	}
	else
	{
		logprintf("write_callback - interrupted\n");
	}
	pthread_mutex_unlock(&context->aamp->mLock);
	return ret;
}


/**
 * @brief
 * @param ptr
 * @param size
 * @param nmemb
 * @param user_data
 * @retval
 */
static size_t header_callback(void *ptr, size_t size, size_t nmemb, void *user_data)
{
	//std::string *httpHeaders = static_cast<std::string *>(user_data);
	httpRespHeaderData *httpHeader = static_cast<httpRespHeaderData *>(user_data);
	size_t len = nmemb * size;
	int startPos = 0;
	int endPos = 0;

	std::string header((const char *)ptr, 0, len);

	if (std::string::npos != header.find("X-Reason:"))
	{
		httpHeader->type = eHTTPHEADERTYPE_XREASON;
		logprintf("%s:%d %s\n", __FUNCTION__, __LINE__, header.c_str());
		startPos = header.find("X-Reason:") + strlen("X-Reason:");
		endPos = header.length() - 1;
	}
	else if (std::string::npos != header.find("Set-Cookie:"))
	{
		httpHeader->type = eHTTPHEADERTYPE_COOKIE;
		startPos = header.find("Set-Cookie:") + strlen("Set-Cookie:");
		endPos = header.length() - 1;
	}

	if((startPos > 0) && (endPos > 0))
	{
		//Find the first character after the http header name
		while ((header[startPos] == ' ') && (startPos <= header.length()))
		{
			startPos++;
		}
		while ((header[endPos] == '\r' || header[endPos] == '\n' || header[endPos] == ';') && (endPos >= 0))
		{
			endPos--;
		}
		httpHeader->data = header.substr(startPos, (endPos - startPos + 1));

		//Append a delimiter ";"
		httpHeader->data += ';';

		traceprintf("Parsed HTTP %s header: %s\n", httpHeader->type==eHTTPHEADERTYPE_COOKIE? "Cookie": "X-Reason", httpHeader->data.c_str());
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
	PrivateInstanceAAMP *context = (PrivateInstanceAAMP *)clientp;
	int rc = 0;
	pthread_mutex_lock(&context->mLock);
	if (!context->mDownloadsEnabled)
	{
		rc = -1; // CURLE_ABORTED_BY_CALLBACK
	}
	pthread_mutex_unlock(&context->mLock);
	return rc;
}

static int eas_curl_debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp)
{
	(void)handle;
	(void)userp;
	(void)size;

	//remove unwanted trailing line feeds from log
	for(int i = (int)size-1; i >= 0; i--)
	{
	    if(data[i] == '\n' || data[i] == '\r')
		data[i] = '\0';
	    else
		break;
	}

	//limit log spam to only TEXT and HEADER_IN
	switch (type) {
		case CURLINFO_TEXT:
		logprintf("curl: %s\n", data);
		break;
		case CURLINFO_HEADER_IN:
		logprintf("curl header: %s\n", data);
		break;
	    default:
		break;
	}
	return 0;
}

/**
 * @brief Initialize curl instances
 * @param startIdx start index
 * @param instanceCount count of instances
 */
void PrivateInstanceAAMP::CurlInit(int startIdx, unsigned int instanceCount)
{
	int instanceEnd = startIdx + instanceCount;
	assert (instanceEnd <= MAX_CURL_INSTANCE_COUNT);
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
			curl_easy_setopt(curl[i], CURLOPT_PROGRESSDATA, this);
			curl_easy_setopt(curl[i], CURLOPT_PROGRESSFUNCTION, progress_callback);
			curl_easy_setopt(curl[i], CURLOPT_HEADERFUNCTION, header_callback);
			//curl_easy_setopt(curl[i], CURLOPT_HEADERDATA, &cookieHeaders[i]);
			curl_easy_setopt(curl[i], CURLOPT_HEADERDATA, &httpRespHeaders[i]);
			curl_easy_setopt(curl[i], CURLOPT_WRITEFUNCTION, write_callback);
			curl_easy_setopt(curl[i], CURLOPT_TIMEOUT, DEFAULT_CURL_TIMEOUT);
			curl_easy_setopt(curl[i], CURLOPT_CONNECTTIMEOUT, DEFAULT_CURL_CONNECTTIMEOUT);
			curl_easy_setopt(curl[i], CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
			curl_easy_setopt(curl[i], CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl[i], CURLOPT_NOPROGRESS, 0L); // enable progress meter (off by default)
			curl_easy_setopt(curl[i], CURLOPT_USERAGENT, "AAMP/1.0.0");
			curl_easy_setopt(curl[i], CURLOPT_ACCEPT_ENCODING, "");//Enable all the encoding formats supported by client
			if (gpGlobalConfig->httpProxy)
			{
				char proxyStr[STR_PROXY_BUFF_SIZE];
				memset((void*)proxyStr, 0, STR_PROXY_BUFF_SIZE);
				snprintf(proxyStr, (STR_PROXY_BUFF_SIZE - 1), "http://%s", gpGlobalConfig->httpProxy);

				/* use this proxy */
				curl_easy_setopt(curl[i], CURLOPT_PROXY, proxyStr);
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
				//on ipv6 box force curl to use ipv6 mode only (DELIA-20209)
				struct stat tmpStat;
				bool isv6(::stat( "/tmp/estb_ipv6", &tmpStat) == 0);
				if(isv6)
					curl_easy_setopt(curl[i], CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
				logprintf("aamp eas curl config: timeout=%d, connecttimeout%d, ipv6=%d\n", EAS_CURL_TIMEOUT, EAS_CURL_CONNECTTIMEOUT, isv6);
			}
			//log_current_time("curl initialized");
		}
	}
}

/**
 * @brief Store language list of stream
 * @param maxLangCount count of language item to be stored
 * @param langlist Array of languges
 */
void PrivateInstanceAAMP::StoreLanguageList(int maxLangCount , char langlist[][MAX_LANGUAGE_TAG_LENGTH])
{
	// store the language list
	if (maxLangCount > MAX_LANGUAGE_COUNT)
	{
		maxLangCount = MAX_LANGUAGE_COUNT; //boundary check
	}
	mMaxLanguageCount = maxLangCount;
	for (int cnt=0; cnt < maxLangCount; cnt ++)
	{
		strncpy(mLanguageList[cnt],langlist[cnt],MAX_LANGUAGE_TAG_LENGTH);
		mLanguageList[cnt][MAX_LANGUAGE_TAG_LENGTH-1] = 0;
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
		if(strncmp(mLanguageList[cnt],checkLanguage,MAX_LANGUAGE_TAG_LENGTH) == 0)
		{
			retVal = true;
			break;
		}
	}

	if(mMaxLanguageCount == 0)
	{
                logprintf("IsAudioLanguageSupported No Audio language stored !!!\n");
	}
	else if(!retVal)
	{
		logprintf("IsAudioLanguageSupported lang[%s] not available in list\n",checkLanguage);
	}
	return retVal;
}


/**
 * @brief Set curl timeout (CURLOPT_TIMEOUT)
 * @param timeout maximum time  in seconds curl request is allowed to take
 * @param instance index of instance to which timeout to be set
 */
void PrivateInstanceAAMP::SetCurlTimeout(long timeout, unsigned int instance)
{
	if(ContentType_EAS == mContentType)
		return;
	if(instance < MAX_CURL_INSTANCE_COUNT && curl[instance])
	{
		curl_easy_setopt(curl[instance], CURLOPT_TIMEOUT, timeout);
	}
	else
	{
		logprintf("Failed to update timeout for curl instace %d\n",instance);
	}
}


/**
 * @brief Terminate curl instances
 * @param startIdx start index
 * @param instanceCount count of instances
 */
void PrivateInstanceAAMP::CurlTerm(int startIdx, unsigned int instanceCount)
{
	int instanceEnd = startIdx + instanceCount;
	assert (instanceEnd <= MAX_CURL_INSTANCE_COUNT);
	for (unsigned int i = startIdx; i < instanceEnd; i++)
	{
		if (curl[i])
		{
			curl_easy_cleanup(curl[i]);
			curl[i] = NULL;
		}
	}
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
	if (mAbrBitrateData.size())
	{
		mAbrBitrateData.erase(mAbrBitrateData.begin(),mAbrBitrateData.end());
	}
	AAMPLOG_WARN("ABRMonitor-Reset::{\"Reason\":\"%s\",\"Bandwidth\":%ld,\"Profile\":%d}\n",(trickPlay)?"TrickPlay":"Tune",bitsPerSecond,profile);
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
	for (bitrateIter = mAbrBitrateData.begin(); bitrateIter != mAbrBitrateData.end();)
	{
		//logprintf("[%s][%d] Sz[%d] TimeCheck Pre[%lld] Sto[%lld] diff[%lld] bw[%ld] \n",__FUNCTION__,__LINE__,mAbrBitrateData.size(),presentTime,(*bitrateIter).first,(presentTime - (*bitrateIter).first),(long)(*bitrateIter).second);
		if ((bitrateIter->first <= 0) || (presentTime - bitrateIter->first > gpGlobalConfig->abrCacheLife))
		{
			//logprintf("[%s][%d] Threadshold time reached , removing bitrate data \n",__FUNCTION__,__LINE__);
			bitrateIter = mAbrBitrateData.erase(bitrateIter);
		}
		else
		{
			tmpData.push_back(bitrateIter->second);
			bitrateIter++;
		}
	}

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
				//logprintf("[%s][%d] Outlier found[%ld]>[%ld] erasing ....\n",__FUNCTION__,__LINE__,diffOutlier,gpGlobalConfig->abrOutlierDiffBytes);
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
			//logprintf("[%s][%d] NwBW with newlogic size[%d] avg[%ld] \n",__FUNCTION__,__LINE__,tmpData.size(), avg/tmpData.size());
			ret = (avg/tmpData.size());
			mAvailableBandwidth = ret;
		}	
		else
		{
			//logprintf("[%s][%d] No prior data available for abr , return -1 \n",__FUNCTION__,__LINE__);
			ret = -1;
		}
	}
	else
	{
		//logprintf("[%s][%d] No data available for bitrate check , return -1 \n",__FUNCTION__,__LINE__);
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
			return "VIDEO";
		case eMEDIATYPE_AUDIO:
			return "AUDIO";
		case eMEDIATYPE_MANIFEST:
			return "MANIFEST";
		case eMEDIATYPE_LICENCE:
			return "LICENCE";
		case eMEDIATYPE_IFRAME:
			return "IFRAME";
		default:
			return "";
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
 * @retval true if success
 */
bool PrivateInstanceAAMP::GetFile(const char *remoteUrl, struct GrowableBuffer *buffer, char effectiveUrl[MAX_URI_LENGTH], long * http_error, const char *range, unsigned int curlInstance, bool resetBuffer, MediaType fileType)
{
	long http_code = -1;
	bool ret = false;
	int downloadAttempt = 0;
	CURL* curl = this->curl[curlInstance];
	struct curl_slist* httpHeaders = NULL;
	CURLcode res = CURLE_OK;

	// temporarily increase timeout for manifest download - these files (especially for VOD) can be large and slow to download
	//bool modifyDownloadTimeout = (!mIsLocalPlayback && fileType == eMEDIATYPE_MANIFEST);

	pthread_mutex_lock(&mLock);
	if (resetBuffer)
	{
		if(buffer->avail)
        	{
            		AAMPLOG_TRACE("%s:%d reset buffer %p avail %d\n", __FUNCTION__, __LINE__, buffer, (int)buffer->avail);
        	}	
		memset(buffer, 0x00, sizeof(*buffer));
	}
	if (mDownloadsEnabled)
	{
		long long downloadTimeMS = 0;
		pthread_mutex_unlock(&mLock);
		AAMPLOG_INFO("aamp url: %s\n", remoteUrl);

		if (curl)
		{
			curl_easy_setopt(curl, CURLOPT_URL, remoteUrl);
			struct WriteContext context;
			context.aamp = this;
			context.buffer = buffer;
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

			// note: win32 curl lib doesn't support multi-part range
			curl_easy_setopt(curl, CURLOPT_RANGE, range);

			/*
			if (modifyDownloadTimeout)
			{
				curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_MANIFEST_DL_TIMEOUT);
			}
			*/

			if ((httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_COOKIE) && (httpRespHeaders[curlInstance].data.length() > 0))
			{
				traceprintf("Appending cookie headers to HTTP request\n");
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
						if (mIsLocalPlayback && !mIsFirstRequestToFOG)
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
							snprintf(buf, 512, "trace-id=%s;parent-id=%u;span-id=%lld",
									(const char*)it->second.at(0).c_str(),
									aamp_GetCurrentTimeMS(),
									aamp_GetCurrentTimeMS());
						}
						headerValue = buf;
					}
					customHeader.append(headerValue);
					httpHeaders = curl_slist_append(httpHeaders, customHeader.c_str());
				}
				if (httpHeaders != NULL)
				{
					curl_easy_setopt(curl, CURLOPT_HTTPHEADER, httpHeaders);
				}
			}

			while(downloadAttempt < 2)
			{
				if(buffer->ptr != NULL)
				{
					traceprintf("%s:%d reset length. buffer %p avail %d\n", __FUNCTION__, __LINE__, buffer, (int)buffer->avail);
					buffer->len = 0;
				}

				std::chrono::steady_clock::time_point tStartTime = std::chrono::steady_clock::now();
				res = curl_easy_perform(curl); // synchronous; callbacks allow interruption
				std::chrono::steady_clock::time_point tEndTime = std::chrono::steady_clock::now();
				downloadAttempt++;

				downloadTimeMS = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(tEndTime - tStartTime).count());

				if (res == CURLE_OK)
				{ // all data collected
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
					if (http_code != 200 && http_code != 206)
					{
#if 0 /* Commented since the same is supported via AAMP_LOG_NETWORK_ERROR */
						logprintf("HTTP RESPONSE CODE: %ld\n", http_code);
#else
						AAMP_LOG_NETWORK_ERROR (remoteUrl, AAMPNetworkErrorHttp, (int)http_code);
#endif /* 0 */
						if((500 == http_code || 503 == http_code) && downloadAttempt < 2)
						{
							logprintf("Download failed due to Server error. Retrying!\n");
							continue;
						}
					}
					char *effectiveUrlPtr = NULL;
					res = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrlPtr);
					strncpy(effectiveUrl, effectiveUrlPtr, MAX_URI_LENGTH-1);
					effectiveUrl[MAX_URI_LENGTH-1] = '\0';

					// check if redirected url is pointing to fog / local ip
					if(mIsFirstRequestToFOG)
					{
					    if( strstr(effectiveUrl,LOCAL_HOST_IP) == NULL )
					    {
					        // oops, TSB is not working, we got redirected away from fog
					        mIsLocalPlayback = false;
					        mTSBEnabled = false;
					        logprintf("NO_TSB_AVAILABLE playing from:%s \n", effectiveUrl);
					    }
					}

					/*
					 * Latency should be printed in the case of successful download which exceeds the download threshold value,
					 * other than this case is assumed as network error and those will be logged with AAMP_LOG_NETWORK_ERROR.
					 */
					if (downloadTimeMS > FRAGMENT_DOWNLOAD_WARNING_THRESHOLD )
					{
						AAMP_LOG_NETWORK_LATENCY (effectiveUrl, downloadTimeMS, FRAGMENT_DOWNLOAD_WARNING_THRESHOLD );
					}
				}
				else
				{
#if 0 /* Commented since the same is supported via AAMP_LOG_NETWORK_ERROR */
					logprintf("CURL error: %d\n", res);
#else
					AAMP_LOG_NETWORK_ERROR (remoteUrl, AAMPNetworkErrorCurl, (int)res);
#endif /* 0 */
					//Attempt retry for local playback since rampdown is disabled for FOG
					if((res == CURLE_COULDNT_CONNECT || (res == CURLE_OPERATION_TIMEDOUT && (mIsLocalPlayback || fileType == eMEDIATYPE_MANIFEST))) && downloadAttempt < 2)
					{
						logprintf("Download failed due to curl connect timeout. Retrying!\n");
						continue;
					}
					/*
					* Assigning curl error to http_code, for sending the error code as
					* part of error event if required
					* We can distinguish curl error and http error based on value
					*curl errors are below 100 and http error starts from 100
					*/
					http_code = res;
				}

				double total, connect, startTransfer, resolve, appConnect, preTransfer, redirect, dlSize;
				long reqSize;
				AAMP_LogLevel reqEndLogLevel = eLOGLEVEL_INFO;

				curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME , &total);
				if(res != CURLE_OK || http_code == 0 || http_code >= 400 || total > 2.0 /*seconds*/)
				{
					reqEndLogLevel = eLOGLEVEL_WARN;
				}
				if (gpGlobalConfig->logging.isLogLevelAllowed(reqEndLogLevel))
				{
					double totalPerformRequest = (double)(std::chrono::duration_cast<std::chrono::microseconds>(tEndTime - tStartTime).count())/1000000;
					curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &resolve);
					curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
					curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appConnect);
					curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME, &preTransfer);
					curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &startTransfer);
					curl_easy_getinfo(curl, CURLINFO_REDIRECT_TIME, &redirect);
					curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &dlSize);
					curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE, &reqSize);
					AAMPLOG(reqEndLogLevel, "HttpRequestEnd: {\"url\":\"%.500s\",\"curlTime\":%2.4f,\"times\":{\"total\":%2.4f,\"connect\":%2.4f,\"startTransfer\":%2.4f,\"resolve\":%2.4f,\"appConnect\":%2.4f,\"preTransfer\":%2.4f,\"redirect\":%2.4f,\"dlSz\":%g,\"ulSz\":%ld},\"responseCode\":%ld}\n",
						remoteUrl,
						totalPerformRequest,
						total, connect, startTransfer, resolve, appConnect, preTransfer, redirect, dlSize, reqSize, http_code);
				}
				break;
			}

			/*
			if (modifyDownloadTimeout)
			{
				curl_easy_setopt(curl, CURLOPT_TIMEOUT, gpGlobalConfig->fragmentDLTimeout);
			}
			*/

		}

		if (http_code == 200 || http_code == 206 || http_code == CURLE_OPERATION_TIMEDOUT)
		{
			if (http_code == CURLE_OPERATION_TIMEDOUT)
			{
				logprintf("Download timedout and obtained a partial buffer of size %d for a downloadTime=%u\n", buffer->len, downloadTimeMS);
			}
			if (downloadTimeMS > 0 && fileType == eMEDIATYPE_VIDEO && gpGlobalConfig->bEnableABR && (buffer->len > AAMP_ABR_THRESHOLD_SIZE || (http_code == CURLE_OPERATION_TIMEDOUT && buffer->len > 0)))
			{
				{
					long long currTime = aamp_GetCurrentTimeMS();
					mAbrBitrateData.push_back(std::make_pair(currTime ,((long)(buffer->len / downloadTimeMS)*8000)));
					//logprintf("CacheSz[%d]ConfigSz[%d] Storing Size [%d] bps[%ld]\n",mAbrBitrateData.size(),gpGlobalConfig->abrCacheLength, buffer->len, ((long)(buffer->len / downloadTimeMS)*8000));
					if(mAbrBitrateData.size() > gpGlobalConfig->abrCacheLength)
						mAbrBitrateData.erase(mAbrBitrateData.begin());
				}
			}
		}
		if (http_code == 200 || http_code == 206)
		{
#ifdef SAVE_DOWNLOADS_TO_DISK
			const char *fname = remoteUrl;
			for (;;)
			{
				const char *next = strchr(fname, '/');
				if (next)
				{
					next++;
					fname = next;
				}
				else
				{
					break;
				}
			}
			char path[1024];
			snprintf(path,sizeof(path),"C:/Users/pstrof200/Downloads/%s", fname);
			FILE *f = fopen(path, "wb");
			fwrite(buffer->ptr, 1, buffer->len, f);
			fclose(f);
#endif
			double expectedContentLength = 0;
			if (CURLE_OK==curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &expectedContentLength) && ((int)expectedContentLength>0) && ((int)expectedContentLength > (int)buffer->len))
			{
				//Note: For non-compressed data, Content-Length header and buffer size should be same. For gzipped data, 'Content-Length' will be <= deflated data.
				logprintf("AAMP content length mismatch expected %d got %d\n",(int)expectedContentLength, (int)buffer->len);
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
				else if (strstr(remoteUrl,"iframe"))
				{
					fileType = eMEDIATYPE_IFRAME;
				}

				if((downloadTimeMS > FRAGMENT_DOWNLOAD_WARNING_THRESHOLD) || (gpGlobalConfig->logging.latencyLogging[fileType] == true))
				{
					long long SequenceNo = GetSeqenceNumberfromURL(remoteUrl);
					logprintf("aampabr#T:%s,s:%lld,d:%lld,sz:%d,r:%ld,cerr:%d,hcode:%ld,n:%lld,estr:%ld,url:%s",MediaTypeString(fileType),(aamp_GetCurrentTimeMS()-downloadTimeMS),downloadTimeMS,int(buffer->len),mpStreamAbstractionAAMP->GetCurProfIdxBW(),res,http_code,SequenceNo,GetCurrentlyAvailableBandwidth(),remoteUrl);
				}
				ret             =       true;
			}
		}
		else
		{
			logprintf("BAD URL:%s\n", remoteUrl);
			if (buffer->ptr)
			{
				aamp_Free(&buffer->ptr);
			}
			memset(buffer, 0x00, sizeof(*buffer));

			if ( (httpRespHeaders[curlInstance].type == eHTTPHEADERTYPE_XREASON) && (httpRespHeaders[curlInstance].data.length() > 0) )
			{
				logprintf("Received X-Reason header from %s: '%s'", mTSBEnabled?"Fog":"CDN Server", httpRespHeaders[curlInstance].data.c_str());
			}
		}

		pthread_mutex_lock(&mLock);
	}
	else
	{
		logprintf("downloads disabled\n");
	}
	pthread_mutex_unlock(&mLock);
	if (http_error)
	{
		*http_error = http_code;
	}
	if (httpHeaders != NULL)
	{
		curl_slist_free_all(httpHeaders);
	}
	if (mIsFirstRequestToFOG)
	{
		mIsFirstRequestToFOG = false;
	}
	return ret;
}


/**
 * @brief Append nul character to buffer
 * @param buffer buffer in which nul to be append
 */
void aamp_AppendNulTerminator(struct GrowableBuffer *buffer)
{
	char zeros[2] = { 0, 0 }; // append two bytes, to distinguish between internal inserted 0x00's and a final 0x00 0x00
	aamp_AppendBytes(buffer, zeros, 2);
}



#if 0
/**
* @brief return pointer to start and end of substring, if found
*/
static const char *FindUriAttr(const char *uri, const char *attrName, const char **pfin)
{
	const char *attrValue = NULL;
	const char *qsStart = strchr(uri, '?');
	if (qsStart)
	{
		attrValue = strstr(qsStart, attrName);
		if (attrValue)
		{
			attrValue += strlen(attrName);
			assert(*attrValue == '=');
			attrValue++;
			const char *fin = strchr(attrValue, '&');
			if (!fin)
			{
				fin = attrValue + strlen(attrValue);
			}
			*pfin = fin;
		}
	}
	return attrValue;
}


/**
 * @brief
 * @param ptr
 * @retval
 */
static int ReadDecimalFourDigit(const char *ptr)
{
	return
		(ptr[0] - '0') * 1000 +
		(ptr[1] - '0') * 100 +
		(ptr[2] - '0') * 10 +
		(ptr[3] - '0');
}

/**
 * @brief
 * @param ptr
 * @retval
 */
static int ReadDecimalTwoDigit(const char *ptr)
{
	return
		(ptr[0] - '0') * 10 +
		(ptr[1] - '0');
}

/**
 * @struct DateTime
 * @brief
 */
struct DateTime
{
	int YYYY;
	int MM;
	int DD;
	int hh;
	int mm;
	float ss;
	int Z;
};

/**
 * @brief Parse ISO8601 time
 * @param[out] datetime Parsed output
 * @param[in] ptr ISO8601 C string
 */
static void ParseISO8601(struct DateTime *datetime, const char *ptr)
{
	datetime->YYYY = ReadDecimalFourDigit(ptr);
	ptr += 4;
	if (*ptr == '-') ptr++;

	datetime->MM = ReadDecimalTwoDigit(ptr);
	ptr += 2;
	if (*ptr == '-') ptr++;

	datetime->DD = ReadDecimalTwoDigit(ptr);
	ptr += 2;
	if (*ptr == 'T') ptr++;

	datetime->hh = ReadDecimalTwoDigit(ptr);
	ptr += 2;
	if (*ptr == ':') ptr++;

	datetime->mm = ReadDecimalTwoDigit(ptr);
	ptr += 2;
	if (*ptr == ':') ptr++;

	datetime->ss = (float)ReadDecimalTwoDigit(ptr);
	ptr += 2;
	if (*ptr == '.' || *ptr == ',')
	{
		ptr++;
		int denominator = 1;
		int numerator = 0;
		while (*ptr >= '0' && *ptr <= '9')
		{
			numerator = (numerator * 10) + (*ptr - '0');
			denominator *= 10;
			ptr++;
		}
		datetime->ss += numerator / (float)denominator;
	}

	char zone = *ptr++;
	if (zone != 'Z')
	{
		datetime->Z = 600 * (ptr[0] - '0'); // hour, tens
		datetime->Z += 60 * (ptr[1] - '0'); // hour, ones
		ptr += 2;
		if (*ptr == ':') ptr++;
		if (*ptr >= '0'&& *ptr <= '9')
		{
			datetime->Z += 10 * (ptr[0] - '0'); // seconds, tens
			datetime->Z += (ptr[1] - '0'); // seconds, ones
			ptr += 2;
		}
		if (zone == '-')
		{
			datetime->Z = -datetime->Z;
		}
	}
	else
	{
		datetime->Z = 0;
	}
}
#endif


/**
 * @brief Resolve URL from base and uri
 * @param[out] dst Destination buffer
 * @param base Base URL
 * @param uri manifest/ fragment uri
 */
void aamp_ResolveURL(char *dst, const char *base, const char *uri)
{
	if (memcmp(uri, "http://", 7) != 0 && memcmp(uri, "https://", 8) != 0) // explicit endpoint - needed for DAI playlist
	{
		strcpy(dst, base);

		if (uri[0] == '/')
		{ // absolute path; preserve only endpoint http://<endpoint>:<port>/
			dst = strstr(dst, "://");
			assert(dst);
			if (dst)
			{
				dst = strchr(dst + 3, '/');
			}
		}
		else
		{ // relative path; include base directory
			dst = strchr(dst, '/');
			assert(dst);
			for (;;)
			{
				char *next = strchr(dst + 1, '/');
				if (!next)
				{
					break;
				}
				dst = next;
			}
			dst++;
		}

		strcpy(dst, uri);

		if (strchr(uri, '?') == 0)//if uri doesn't already have url parameters, then copy from the parents(if they exist)
		{
			const char* params = strchr(base, '?');
			if (params)
				strcat(dst, params);
		}
	}
	else
		strcpy(dst, uri);
}

/**
 * @brief
 * @param url
 * @retval
 */
std::string aamp_getHostFromURL(char *url)
{
    std::string host = "comcast.net";
    int delimCnt = 0;
    char *ptr = url;
    char *hostStPtr = NULL;
    char *hostEndPtr = NULL;
    while(*ptr != '\0'){
        if(*ptr == '/')
        {
            delimCnt++;
            if(delimCnt == 2) hostStPtr=ptr+1;
            if(delimCnt == 3)
            {
                hostEndPtr=ptr;
                break;
            }
        }
        ptr++;
    }
    if((hostStPtr != hostEndPtr) && (hostStPtr != NULL) && (hostEndPtr != NULL))
    {
        host = std::string(hostStPtr,hostEndPtr-hostStPtr);
    }
    return host;
}

#ifdef STANDALONE_AAMP

/**
 * @brief Show help menu with aamp command line interface
 */
static void ShowHelp(void)
{
	int i = 0;

	if (!mChannelMap.empty())
	{
		logprintf("\nChannel Map from aamp.cfg\n*************************\n");

		for (std::list<ChannelInfo>::iterator it = mChannelMap.begin(); it != mChannelMap.end(); ++it, ++i)
		{
			ChannelInfo &pChannelInfo = *it;
			logprintf("%4d: %s", pChannelInfo.channelNumber, pChannelInfo.name.c_str());
			if ((i % 4) == 3)
			{
				logprintf("\n");
			}
			else
			{
				logprintf("\t");
			}
		}
	}

	logprintf("\n\nList of Commands\n****************\n");
	logprintf("<channelNumber> // Play selected channel from guide\n");
	logprintf("<url> // Play arbitrary stream\n");
	logprintf("info gst trace curl progress // Logging toggles\n");
	logprintf("pause play stop status flush // Playback options\n");
	logprintf("sf, ff<x> rw<y> // Trickmodes (x- 16, 32. y- 4, 8, 16, 32)\n");
	logprintf("+ - // Change profile\n");
	logprintf("sap // Use SAP track (if avail)\n");
	logprintf("seek <seconds> // Specify start time within manifest\n");
	logprintf("live // Seek to live point\n");
	logprintf("underflow // Simulate underflow\n");
	logprintf("help // Show this list again\n");
	logprintf("exit // Exit from application\n");
}
#endif


/**
 * @brief
 * @param s
 * @retval
 */
static bool isNumber(const char *s)
{
	if (*s)
	{
		if (*s == '-')
		{ // skip leading minus
			s++;
		}
		for (;;)
		{
			if (*s >= '0' && *s <= '9')
			{
				s++;
				continue;
			}
			if (*s == 0x00)
			{
				return true;
			}
			break;
		}
	}
	return false;
}

#define MAX_OVERRIDE 10

/**
 * @brief
 * @param mainManifestUrl
 * @retval
 */
static const char *RemapManifestUrl(const char *mainManifestUrl)
{
#ifndef STANDALONE_AAMP
	if (!mChannelMap.empty())
	{
		for (std::list<ChannelInfo>::iterator it = mChannelMap.begin(); it != mChannelMap.end(); ++it)
		{
			ChannelInfo &pChannelInfo = *it;
			if (strstr(mainManifestUrl, pChannelInfo.name.c_str()))
			{
				logprintf("override!\n");
				return pChannelInfo.uri.c_str();
			}
		}
	}
#endif
	return NULL;
}

#ifdef IARM_MGR
//Enable below line while merging https://gerrit.teamccp.com/#/c/171105/ (
//XRE-12586 - Move aamp recipes from meta-rdk-video-comcast to meta-rdk-video)
//#ifdef IARM_MGR

/**
 * @brief
 * @param paramName
 * @param iConfigLen
 * @retval
 */
char *  GetTR181AAMPConfig(const char * paramName, size_t & iConfigLen)
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
                
                logprintf("GetTR181AAMPConfig: Got:%s En-Len:%d Dec-len:%d\n",strforLog.c_str(),param.paramLen,iConfigLen);
            }
            else
            {
                logprintf("GetTR181AAMPConfig: Not a string param type=%d or Invalid len:%d \n",param.paramtype, param.paramLen);
            }
        }
    }
    else
    {
        logprintf("GetTR181AAMPConfig: Failed to retrieve value result=%d\n",result);
    }
    return strConfig;
}
#endif


/**
 * @brief trim a string
 * @param[in][out] cmd Buffer containing string
 */
static void trim(char **cmd)
{
	std::string src = *cmd;
	size_t first = src.find_first_not_of(' ');
	if (first != std::string::npos)
	{
		size_t last = src.find_last_not_of(" \r\n");
		std::string dst = src.substr(first, (last - first + 1));
		strncpy(*cmd, (char*)dst.c_str(), dst.size());
		(*cmd)[dst.size()] = '\0';
	}
}

/**
* @brief helper function to avoid dependency on unsafe sscanf while reading strings
* @param bufPtr pointer to CString buffer to scan
* @param prefixPtr - prefix string to match in bufPtr
* @param valueCopyPtr receives allocated copy of string following prefix (skipping delimiting whitesace) if prefix found
* @retval 0 if prefix not present or error
* @retval 1 if string extracted/copied to valueCopyPtr
*/
static int ReadConfigStringHelper(const char *bufPtr, const char *prefixPtr, const char **valueCopyPtr)
{
    int rc = 0;
    size_t prefixLen = strlen(prefixPtr);
    size_t bufLen = strlen(bufPtr);
    if (bufLen > prefixLen && memcmp(bufPtr, prefixPtr, prefixLen) == 0)
    {
        bufPtr += prefixLen;
        while (*bufPtr == ' ')
        { // skip any whitespace
            bufPtr++;
        }
        *valueCopyPtr = strdup(bufPtr);
        if (*valueCopyPtr)
        {
            rc = 1;
        }
    }
    return rc;
}

/**
 * @brief Process command
 * @param cmd command
 * @param usingCLI true if using aamp command line interface
 */
void ProcessCommand(char *cmd, bool usingCLI)
{
	if (cmd[0] != '#')
	{ // ignore comments

		//Removing unnecessary spaces and newlines
		trim(&cmd);

		double seconds = 0;

#ifdef STANDALONE_AAMP
	bool done = false;
	int rate = 0;
		if (usingCLI)
		{
			done = true;
			if (cmd[0] == 0)
			{
				if (mSingleton->aamp->mpStreamAbstractionAAMP)
				{
					mSingleton->aamp->mpStreamAbstractionAAMP->DumpProfiles();
				}
				logprintf("current bitrate ~= %ld\n", mSingleton->aamp->GetCurrentlyAvailableBandwidth());
			}
			else if (strcmp(cmd, "help") == 0)
			{
				ShowHelp();
			}
			else if (memcmp(cmd, "http", 4) == 0)
			{
				mSingleton->Tune(cmd);
			}
			else if (isNumber(cmd))
			{
				int channelNumber = atoi(cmd);
				logprintf("channel number: %d\n", channelNumber);
				for (std::list<ChannelInfo>::iterator it = mChannelMap.begin(); it != mChannelMap.end(); ++it)
				{
					ChannelInfo &channelInfo = *it;
					if(channelInfo.channelNumber == channelNumber)
					{
						mSingleton->Tune(channelInfo.uri.c_str());
						break;
					}
				}
			}
			else if (sscanf(cmd, "seek %lf", &seconds) == 1)
			{
				mSingleton->Seek(seconds);
			}
			else if (strcmp(cmd, "sf") == 0)
			{
				mSingleton->SetRate(0.5);
			}
			else if (sscanf(cmd, "ff%d", &rate) == 1)
			{
				if (rate != 4 && rate != 16 && rate != 32)
				{
					logprintf("Speed not supported.\n");
				}
				else
					mSingleton->SetRate((float)rate);
			}
			else if (strcmp(cmd, "play") == 0)
			{
				mSingleton->SetRate(1);
			}
			else if (strcmp(cmd, "pause") == 0)
			{
				mSingleton->SetRate(0);
			}
			else if (sscanf(cmd, "rw%d", &rate) == 1)
			{
				if ((rate < 4 || rate > 32) || (rate % 4))
				{
					logprintf("Speed not supported.\n");
				}
				else
					mSingleton->SetRate((float)(-rate));
			}
			else if (strcmp(cmd, "flush") == 0)
			{
				mSingleton->aamp->mStreamSink->Flush();
			}
			else if (strcmp(cmd, "stop") == 0)
			{
				mSingleton->Stop();
			}
			else if (strcmp(cmd, "underflow") == 0)
			{
				mSingleton->aamp->ScheduleRetune(eGST_ERROR_PTS, eMEDIATYPE_VIDEO);
			}
			else if (strcmp(cmd, "status") == 0)
			{
				mSingleton->aamp->mStreamSink->DumpStatus();
			}
			else if (strcmp(cmd, "live") == 0)
			{
				mSingleton->SeekToLive();
			}
			else if (strcmp(cmd, "exit") == 0)
			{
				mSingleton->Stop();
				delete mSingleton;
				mChannelMap.clear();
				exit(0);
			}
			else if (memcmp(cmd, "rect", 4) == 0)
			{
				int x, y, w, h;
				if (sscanf(cmd, "rect %d %d %d %d", &x, &y, &w, &h) == 4)
				{
					mSingleton->SetVideoRectangle(x, y, w, h);
				}
			}
			else if (memcmp(cmd, "zoom", 4) == 0)
			{
				int zoom;
				if (sscanf(cmd, "zoom %d", &zoom) == 1)
				{
					if (zoom)
					{
						logprintf("Set zoom to full\n", zoom);
						mSingleton->SetVideoZoom(VIDEO_ZOOM_FULL);
					}
					else
					{
						logprintf("Set zoom to none\n", zoom);
						mSingleton->SetVideoZoom(VIDEO_ZOOM_NONE);
					}
				}
			}
			else if (strcmp(cmd, "sap") == 0)
			{
				gpGlobalConfig->SAP = !gpGlobalConfig->SAP;
				logprintf("SAP %s\n", gpGlobalConfig->SAP ? "on" : "off");
				if (gpGlobalConfig->SAP)
				{
					mSingleton->SetLanguage("es");
				}
				else
				{
					mSingleton->SetLanguage("en");
				}
			}
			else
			{
				done = false;
			}
		}

		if (!done)
#endif
		{
			int value;
			char strValue[1024];
			if (sscanf(cmd, "map-mpd=%d\n", &gpGlobalConfig->mapMPD) == 1)
			{
				logprintf("map-mpd=%d\n", gpGlobalConfig->mapMPD);
			}
			else if (sscanf(cmd, "fog-dash=%d\n", &value) == 1)
			{
				gpGlobalConfig->fogSupportsDash = (value != 0);
				logprintf("fog-dash=%d\n", value);
			}
			else if (sscanf(cmd, "fog=%d\n", &value) == 1)
			{
				gpGlobalConfig->noFog = (value==0);
				logprintf("fog=%d\n", value);
			}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
			else if (sscanf(cmd, "harvest=%d", &gpGlobalConfig->harvest) == 1)
			{
				logprintf("harvest=%d\n", gpGlobalConfig->harvest);
			}
#endif
			else if (sscanf(cmd, "forceEC3=%d", &gpGlobalConfig->forceEC3) == 1)
			{
				logprintf("forceEC3=%d\n", gpGlobalConfig->forceEC3);
			}
			else if (sscanf(cmd, "disableEC3=%d", &gpGlobalConfig->disableEC3) == 1)
			{
				logprintf("disableEC3=%d\n", gpGlobalConfig->disableEC3);
			}
			else if (sscanf(cmd, "disableATMOS=%d", &gpGlobalConfig->disableATMOS) == 1)
			{
				logprintf("disableATMOS=%d\n", gpGlobalConfig->disableATMOS);
			}
			else if (sscanf(cmd, "live-offset=%d", &gpGlobalConfig->liveOffset) == 1)
			{
                            VALIDATE_INT("live-offset", gpGlobalConfig->liveOffset, AAMP_LIVE_OFFSET)
                            logprintf("live-offset=%d\n", gpGlobalConfig->liveOffset);
			}
			else if (sscanf(cmd, "cdvrlive-offset=%d", &gpGlobalConfig->cdvrliveOffset) == 1)
			{
				VALIDATE_INT("cdvrlive-offset", gpGlobalConfig->cdvrliveOffset, AAMP_CDVR_LIVE_OFFSET)
				logprintf("cdvrlive-offset=%d\n", gpGlobalConfig->cdvrliveOffset);
			}
			else if (sscanf(cmd, "ad-position=%d", &gpGlobalConfig->adPositionSec) == 1)
			{
				VALIDATE_INT("ad-position", gpGlobalConfig->adPositionSec, 0)
				logprintf("ad-position=%d\n", gpGlobalConfig->adPositionSec);
			}
			else if (ReadConfigStringHelper(cmd, "ad-url=", &gpGlobalConfig->adURL))
			{
				logprintf("ad-url=%s\n", gpGlobalConfig->adURL);
			}
			else if (sscanf(cmd, "disablePlaylistIndexEvent=%d", &gpGlobalConfig->disablePlaylistIndexEvent) == 1)
			{
				logprintf("disablePlaylistIndexEvent=%d\n", gpGlobalConfig->disablePlaylistIndexEvent);
			}
			else if (strcmp(cmd, "enableSubscribedTags") == 0)
			{
				gpGlobalConfig->enableSubscribedTags = true;
				logprintf("enableSubscribedTags set\n");
			}
			else if (strcmp(cmd, "disableSubscribedTags") == 0)
			{
				gpGlobalConfig->enableSubscribedTags = false;
				logprintf("disableSubscribedTags set\n");
			}
			else if (sscanf(cmd, "enableSubscribedTags=%d", &gpGlobalConfig->enableSubscribedTags) == 1)
			{
				logprintf("enableSubscribedTags=%d\n", gpGlobalConfig->enableSubscribedTags);
			}
			else if (sscanf(cmd, "fragmentDLTimeout=%ld", &gpGlobalConfig->fragmentDLTimeout) == 1)
			{
				VALIDATE_LONG("fragmentDLTimeout", gpGlobalConfig->fragmentDLTimeout, CURL_FRAGMENT_DL_TIMEOUT)
				logprintf("fragmentDLTimeout=%ld\n", gpGlobalConfig->fragmentDLTimeout);
			}
#ifdef AAMP_CC_ENABLED
			else if (strcmp(cmd, "cc") == 0)
			{
				gpGlobalConfig->bEnableCC = !gpGlobalConfig->bEnableCC;
				logprintf("CC Status %s\n", gpGlobalConfig->bEnableCC ? "on" : "off");
			}
#endif
			else if (strcmp(cmd, "dash-ignore-base-url-if-slash") == 0)
			{
				gpGlobalConfig->dashIgnoreBaseURLIfSlash = true;
				logprintf("dash-ignore-base-url-if-slash set\n");
			}
			else if (strcmp(cmd, "license-anonymous-request") == 0)
			{
				gpGlobalConfig->licenseAnonymousRequest = true;
				logprintf("license-anonymous-request set\n");
			}
			else if ((strcmp(cmd, "info") == 0) && (!gpGlobalConfig->logging.debug))
			{
				gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_INFO);
				gpGlobalConfig->logging.info = true;
				logprintf("info logging %s\n", gpGlobalConfig->logging.info ? "on" : "off");
			}
			else if (strcmp(cmd, "gst") == 0)
			{
				gpGlobalConfig->logging.gst = !gpGlobalConfig->logging.gst;
				logprintf("gst logging %s\n", gpGlobalConfig->logging.gst ? "on" : "off");
			}
			else if (strcmp(cmd, "progress") == 0)
			{
				gpGlobalConfig->logging.progress = !gpGlobalConfig->logging.progress;
				logprintf("progress logging %s\n", gpGlobalConfig->logging.progress ? "on" : "off");
			}
			else if (strcmp(cmd, "debug") == 0)
			{
				gpGlobalConfig->logging.info = false;
				gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_TRACE);
				gpGlobalConfig->logging.debug = true;
				logprintf("debug logging %s\n", gpGlobalConfig->logging.debug ? "on" : "off");
			}
			else if (strcmp(cmd, "trace") == 0)
			{
				gpGlobalConfig->logging.trace = !gpGlobalConfig->logging.trace;
				logprintf("trace logging %s\n", gpGlobalConfig->logging.trace ? "on" : "off");
			}
			else if (strcmp(cmd, "curl") == 0)
			{
				gpGlobalConfig->logging.curl = !gpGlobalConfig->logging.curl;
				logprintf("curl logging %s\n", gpGlobalConfig->logging.curl ? "on" : "off");
			}
			else if (sscanf(cmd, "default-bitrate=%ld", &gpGlobalConfig->defaultBitrate) == 1)
			{
				VALIDATE_LONG("default-bitrate",gpGlobalConfig->defaultBitrate, DEFAULT_INIT_BITRATE)
				logprintf("aamp default-bitrate: %ld\n", gpGlobalConfig->defaultBitrate);
			}
			else if (sscanf(cmd, "default-bitrate-4k=%ld", &gpGlobalConfig->defaultBitrate4K) == 1)
			{
				VALIDATE_LONG("default-bitrate-4k", gpGlobalConfig->defaultBitrate4K, DEFAULT_INIT_BITRATE_4K)
				logprintf("aamp default-bitrate-4k: %ld\n", gpGlobalConfig->defaultBitrate4K);
			}
			else if (strcmp(cmd, "abr") == 0)
			{
				gpGlobalConfig->bEnableABR = !gpGlobalConfig->bEnableABR;
				logprintf("abr %s\n", gpGlobalConfig->bEnableABR ? "on" : "off");
			}
			else if (sscanf(cmd, "abr-cache-life=%d", &gpGlobalConfig->abrCacheLife) == 1)
			{
				gpGlobalConfig->abrCacheLife *= 1000;
				logprintf("aamp abr cache lifetime: %ldmsec\n", gpGlobalConfig->abrCacheLife);
			}
			else if (sscanf(cmd, "abr-cache-length=%ld", &gpGlobalConfig->abrCacheLength) == 1)
                        {
                                VALIDATE_INT("abr-cache-length", gpGlobalConfig->abrCacheLength, DEFAULT_ABR_CACHE_LENGTH)
                                logprintf("aamp abr cache length: %ld\n", gpGlobalConfig->abrCacheLength);
                        }
			else if (sscanf(cmd, "abr-cache-outlier=%ld", &gpGlobalConfig->abrOutlierDiffBytes) == 1)
                        {
                                VALIDATE_LONG("abr-cache-outlier", gpGlobalConfig->abrOutlierDiffBytes, DEFAULT_ABR_OUTLIER)
                                logprintf("aamp abr outlier in bytes: %ld\n", gpGlobalConfig->abrOutlierDiffBytes);
                        }
			else if (sscanf(cmd, "abr-skip-duration=%ld", &gpGlobalConfig->abrSkipDuration) == 1)
                        {
                                VALIDATE_INT("abr-skip-duration",gpGlobalConfig->abrSkipDuration, DEFAULT_ABR_SKIP_DURATION)
                                logprintf("aamp abr skip duration: %d\n", gpGlobalConfig->abrSkipDuration);
                        }
			else if (sscanf(cmd, "abr-nw-consistency=%ld", &gpGlobalConfig->abrNwConsistency) == 1)
                        {
                                VALIDATE_LONG("abr-nw-consistency", gpGlobalConfig->abrNwConsistency, DEFAULT_ABR_NW_CONSISTENCY_CNT)
                                logprintf("aamp abr NetworkConsistencyCnt: %d\n", gpGlobalConfig->abrNwConsistency);
                        }
			else if (sscanf(cmd, "flush=%d", &gpGlobalConfig->gPreservePipeline) == 1)
			{
				logprintf("aamp flush=%d\n", gpGlobalConfig->gPreservePipeline);
			}
			else if (sscanf(cmd, "demux-hls-audio-track=%d", &gpGlobalConfig->gAampDemuxHLSAudioTsTrack) == 1)
			{ // default 1, set to 0 for hw demux audio ts track
				logprintf("demux-hls-audio-track=%d\n", gpGlobalConfig->gAampDemuxHLSAudioTsTrack);
			}
			else if (sscanf(cmd, "demux-hls-video-track=%d", &gpGlobalConfig->gAampDemuxHLSVideoTsTrack) == 1)
			{ // default 1, set to 0 for hw demux video ts track
				logprintf("demux-hls-video-track=%d\n", gpGlobalConfig->gAampDemuxHLSVideoTsTrack);
			}
			else if (sscanf(cmd, "demux-hls-video-track-tm=%d", &gpGlobalConfig->demuxHLSVideoTsTrackTM) == 1)
			{ // default 0, set to 1 to demux video ts track during trickmodes
				logprintf("demux-hls-video-track-tm=%d\n", gpGlobalConfig->demuxHLSVideoTsTrackTM);
			}
			else if (sscanf(cmd, "demuxed-audio-before-video=%d", &gpGlobalConfig->demuxedAudioBeforeVideo) == 1)
			{ // default 0, set to 1 to send audio es before video in case of s/w demux.
				logprintf("demuxed-audio-before-video=%d\n", gpGlobalConfig->demuxedAudioBeforeVideo);
			}
			else if (sscanf(cmd, "throttle=%d", &gpGlobalConfig->gThrottle) == 1)
			{ // default is true; used with restamping
				// ?
				logprintf("aamp throttle=%d\n", gpGlobalConfig->gThrottle);
			}
			else if (sscanf(cmd, "min-vod-cache=%d", &gpGlobalConfig->minVODCacheSeconds) == 1)
			{ // override for VOD cache
				VALIDATE_INT("min-vod-cache", gpGlobalConfig->minVODCacheSeconds, DEFAULT_MINIMUM_CACHE_VOD_SECONDS)
				logprintf("min-vod-cache=%d\n", gpGlobalConfig->minVODCacheSeconds);
			}
			else if (sscanf(cmd, "buffer-health-monitor-delay=%d", &gpGlobalConfig->bufferHealthMonitorDelay) == 1)
			{ // override for buffer health monitor delay after tune/ seek
				VALIDATE_INT("buffer-health-monitor-delay", gpGlobalConfig->bufferHealthMonitorDelay, DEFAULT_BUFFER_HEALTH_MONITOR_DELAY)
				logprintf("buffer-health-monitor-delay=%d\n", gpGlobalConfig->bufferHealthMonitorDelay);
			}
			else if (sscanf(cmd, "buffer-health-monitor-interval=%d", &gpGlobalConfig->bufferHealthMonitorInterval) == 1)
			{ // override for buffer health monitor interval
				VALIDATE_INT("buffer-health-monitor-interval", gpGlobalConfig->bufferHealthMonitorInterval, DEFAULT_BUFFER_HEALTH_MONITOR_INTERVAL)
				logprintf("buffer-health-monitor-interval=%d\n", gpGlobalConfig->bufferHealthMonitorInterval);
			}
			else if (sscanf(cmd, "preferred-drm=%d", &value) == 1)
			{ // override for preferred drm value
				if(value <= eDRM_NONE || value > eDRM_PlayReady)
				{
					logprintf("preferred-drm=%d is unsupported\n", value);
				}
				else
				{
					gpGlobalConfig->preferredDrm = (DRMSystems) value;
				}
				logprintf("preferred-drm=%s\n", GetDrmSystemName(gpGlobalConfig->preferredDrm));
			}
			else if (sscanf(cmd, "live-tune-event-playlist-indexed=%d", &value) == 1)
			{ // default is 0; set 1 for sending tuned event after playlist indexing - for live
				logprintf("live-tune-event-playlist-indexed=%d\n", value);
				if (value)
				{
					gpGlobalConfig->tunedEventConfigLive = eTUNED_EVENT_ON_PLAYLIST_INDEXED;
				}
			}
			else if (sscanf(cmd, "live-tune-event-first-fragment-decrypted=%d", &value) == 1)
			{ // default is 0; set 1 for sending tuned event after first fragment decrypt - for live
				logprintf("live-tune-event-first-fragment-decrypted=%d\n", value);
				if (value)
				{
					gpGlobalConfig->tunedEventConfigLive = eTUNED_EVENT_ON_FIRST_FRAGMENT_DECRYPTED;
				}
			}
			else if (sscanf(cmd, "vod-tune-event-playlist-indexed=%d", &value) == 1)
                        { // default is 0; set 1 for sending tuned event after playlist indexing - for vod
                                logprintf("vod-tune-event-playlist-indexed=%d\n", value);
                                if (value)
                                {
                                        gpGlobalConfig->tunedEventConfigVOD = eTUNED_EVENT_ON_PLAYLIST_INDEXED;
                                }
                        }
                        else if (sscanf(cmd, "vod-tune-event-first-fragment-decrypted=%d", &value) == 1)
                        { // default is 0; set 1 for sending tuned event after first fragment decrypt - for vod
                                logprintf("vod-tune-event-first-fragment-decrypted=%d\n", value);
                                if (value)
                                {
                                        gpGlobalConfig->tunedEventConfigVOD = eTUNED_EVENT_ON_FIRST_FRAGMENT_DECRYPTED;
                                }
                        }
			else if (sscanf(cmd, "playlists-parallel-fetch=%d\n", &value) == 1)
			{
				gpGlobalConfig->playlistsParallelFetch = (value != 0);
				logprintf("playlists-parallel-fetch=%d\n", value);
			}
			else if (sscanf(cmd, "pre-fetch-iframe-playlist=%d\n", &value) == 1)
			{
				gpGlobalConfig->prefetchIframePlaylist = (value != 0);
				logprintf("pre-fetch-iframe-playlist=%d\n", value);
			}
			else if (sscanf(cmd, "hls-av-sync-use-start-time=%d\n", &value) == 1)
			{
				gpGlobalConfig->hlsAVTrackSyncUsingStartTime = (value != 0);
				logprintf("hls-av-sync-use-start-time=%d\n", value);
			}
			else if (sscanf(cmd, "mpd-discontinuity-handling=%d\n", &value) == 1)
			{
				gpGlobalConfig->mpdDiscontinuityHandling = (value != 0);
				logprintf("mpd-discontinuity-handling=%d\n", value);
			}
			else if (sscanf(cmd, "mpd-discontinuity-handling-cdvr=%d\n", &value) == 1)
			{
				gpGlobalConfig->mpdDiscontinuityHandlingCdvr = (value != 0);
				logprintf("mpd-discontinuity-handling-cdvr=%d\n", value);
			}
			else if(ReadConfigStringHelper(cmd, "license-server-url=", (const char**)&gpGlobalConfig->licenseServerURL))
			{
				gpGlobalConfig->licenseServerLocalOverride = true;
				logprintf("license-server-url=%s\n", gpGlobalConfig->licenseServerURL);
			}
			else if(sscanf(cmd, "vod-trickplay-fps=%d\n", &gpGlobalConfig->vodTrickplayFPS) == 1)
			{
				VALIDATE_INT("vod-trickplay-fps", gpGlobalConfig->vodTrickplayFPS, TRICKPLAY_NETWORK_PLAYBACK_FPS)
				if(gpGlobalConfig->vodTrickplayFPS != TRICKPLAY_NETWORK_PLAYBACK_FPS)
					gpGlobalConfig->vodTrickplayFPSLocalOverride = true;

				logprintf("vod-trickplay-fps=%d\n", gpGlobalConfig->vodTrickplayFPS);
			}
			else if(sscanf(cmd, "linear-trickplay-fps=%d\n", &gpGlobalConfig->linearTrickplayFPS) == 1)
			{
				VALIDATE_INT("linear-trickplay-fps", gpGlobalConfig->linearTrickplayFPS, TRICKPLAY_TSB_PLAYBACK_FPS)
				if (gpGlobalConfig->linearTrickplayFPS != TRICKPLAY_TSB_PLAYBACK_FPS)
					gpGlobalConfig->linearTrickplayFPSLocalOverride = true;

				logprintf("linear-trickplay-fps=%d\n", gpGlobalConfig->linearTrickplayFPS);
			}
			else if (sscanf(cmd, "report-progress-interval=%d\n", &gpGlobalConfig->reportProgressInterval) == 1)
			{
				VALIDATE_INT("report-progress-interval", gpGlobalConfig->reportProgressInterval, DEFAULT_REPORT_PROGRESS_INTERVAL)
				logprintf("report-progress-interval=%d\n", gpGlobalConfig->reportProgressInterval);
			}
			else if (ReadConfigStringHelper(cmd, "http-proxy=", &gpGlobalConfig->httpProxy))
			{
				logprintf("http-proxy=%s\n", gpGlobalConfig->httpProxy);
			}
			else if (strcmp(cmd, "force-http") == 0)
			{
				gpGlobalConfig->bForceHttp = !gpGlobalConfig->bForceHttp;
				logprintf("force-http: %s\n", gpGlobalConfig->bForceHttp ? "on" : "off");
			}
			else if (sscanf(cmd, "internal-retune=%d\n", &value) == 1)
			{
				gpGlobalConfig->internalReTune = (value != 0);
				logprintf("internal-retune=%d\n", (int)value);
			}
			else if (sscanf(cmd, "gst-buffering-before-play=%d\n", &value) == 1)
			{
				gpGlobalConfig->gstreamerBufferingBeforePlay = (value != 0);
				logprintf("gst-buffering-before-play=%d\n", (int)gpGlobalConfig->gstreamerBufferingBeforePlay);
			}
			else if (strcmp(cmd, "audioLatencyLogging") == 0)
			{
				gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_AUDIO] = true;
				logprintf("audioLatencyLogging is %s\n", gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_AUDIO]? "enabled" : "disabled");
			}
			else if (strcmp(cmd, "videoLatencyLogging") == 0)
			{
				gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_VIDEO] = true;
				logprintf("videoLatencyLogging is %s\n", gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_VIDEO]? "enabled" : "disabled");
			}
			else if (strcmp(cmd, "manifestLatencyLogging") == 0)
			{
				gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_MANIFEST] = true;
				logprintf("manifestLatencyLogging is %s\n", gpGlobalConfig->logging.latencyLogging[eMEDIATYPE_MANIFEST]? "enabled" : "disabled");
			}
			else if (sscanf(cmd, "iframe-default-bitrate=%ld", &gpGlobalConfig->iframeBitrate) == 1)
			{
				VALIDATE_LONG("iframe-default-bitrate",gpGlobalConfig->iframeBitrate, 0)
				logprintf("aamp iframe-default-bitrate: %ld\n", gpGlobalConfig->iframeBitrate);
			}
			else if (sscanf(cmd, "iframe-default-bitrate-4k=%ld", &gpGlobalConfig->iframeBitrate4K) == 1)
			{
				VALIDATE_LONG("iframe-default-bitrate-4k",gpGlobalConfig->iframeBitrate4K, 0)
				logprintf("aamp iframe-default-bitrate-4k: %ld\n", gpGlobalConfig->iframeBitrate4K);
			}
            else if (strcmp(cmd, "aamp-audio-only-playback") == 0)
            {
                gpGlobalConfig->bAudioOnlyPlayback = true;
                logprintf("aamp-audio-only-playback is %s\n", gpGlobalConfig->bAudioOnlyPlayback ? "enabled" : "disabled");
            }
			else if (sscanf(cmd, "license-retry-wait-time=%d", &gpGlobalConfig->licenseRetryWaitTime) == 1)
			{
				logprintf("license-retry-wait-time: %d\n", gpGlobalConfig->licenseRetryWaitTime);
			}
			else if (sscanf(cmd, "fragment-cache-length=%d", &gpGlobalConfig->maxCachedFragmentsPerTrack) == 1)
			{
				VALIDATE_INT("fragment-cache-length", gpGlobalConfig->maxCachedFragmentsPerTrack, DEFAULT_CACHED_FRAGMENTS_PER_TRACK)
				logprintf("aamp fragment cache length: %d\n", gpGlobalConfig->maxCachedFragmentsPerTrack);
			}
			else if (sscanf(cmd, "pts-error-threshold=%d", &gpGlobalConfig->ptsErrorThreshold) == 1)
			{
				VALIDATE_INT("pts-error-threshold", gpGlobalConfig->ptsErrorThreshold, MAX_PTS_ERRORS_THRESHOLD)
				logprintf("aamp pts-error-threshold: %d\n", gpGlobalConfig->ptsErrorThreshold);
			}
			else if (mChannelMap.size() < MAX_OVERRIDE && !usingCLI)
			{
				if (cmd[0] == '*')
				{
					char *delim = strchr(cmd, ' ');
					if (delim)
					{
						//Populate channel map from aamp.cfg
#ifndef STANDALONE_AAMP
						// new wildcard matching for overrides - allows *HBO to remap any url including "HBO"
						logprintf("aamp override:\n%s\n", cmd);
#endif
						ChannelInfo channelInfo;
						char *channelStr = &cmd[1];
						char *token = strtok(channelStr, " ");
						while (token != NULL)
						{
							if (isNumber(token))
								channelInfo.channelNumber = atoi(token);
							else if (memcmp(token, "http", 4) == 0)
								channelInfo.uri = token;
							else
								channelInfo.name = token;

							token = strtok(NULL, " ");
						}
						mChannelMap.push_back(channelInfo);
					}
				}
			}
		}
	}
}


/**
 * @brief Load AAMP configuration file
 */
void PrivateInstanceAAMP::LazilyLoadConfigIfNeeded(void)
{
	if (!gpGlobalConfig)
	{
		gpGlobalConfig = new GlobalConfigAAMP();
#ifdef IARM_MGR 
        logprintf("LazilyLoadConfigIfNeeded calling  GetTR181AAMPConfig  \n");
        size_t iConfigLen = 0;
        char *  cloudConf = GetTR181AAMPConfig("Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.Feature.AAMP_CFG.b64Config", iConfigLen);
        if(cloudConf && (iConfigLen > 0))
        {
            bool bCharCompliant = true;
            for (int i = 0; i < iConfigLen; i++)
            {
                if (!( cloudConf[i] == 0xD || cloudConf[i] == 0xA) && // ignore LF and CR chars
                    ((cloudConf[i] < 0x20) || (cloudConf[i] > 0x7E)))
                {
                    bCharCompliant = false;
                    logprintf("LazilyLoadConfigIfNeeded Non Compliant char[0x%X] found, Ignoring whole config  \n",cloudConf[i]);
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
                        //ProcessCommand takes char * and line.c_str() returns const string hence copy of line is created, 
                        char * cstrCmd = (char *)malloc(line.length() + 1);
                        if (cstrCmd)
                        {
                            strcpy(cstrCmd, line.c_str());
                            logprintf("LazilyLoadConfigIfNeeded aamp-cmd:[%s]\n", cstrCmd);
                            ProcessCommand(cstrCmd, false);
                            free(cstrCmd);
                        }
                    }
                }
            }
            free(cloudConf); // allocated by base64_Decode in GetTR181AAMPConfig
        }
#endif

#ifdef WIN32
		FILE *f = fopen("c:/tmp/aamp.cfg", "rb");
#elif defined(__APPLE__)
		std::string cfgPath(getenv("HOME"));
		cfgPath += "/aamp.cfg";
		FILE *f = fopen(cfgPath.c_str(), "rb");
#else
		FILE *f = fopen("/opt/aamp.cfg", "rb");
#endif
		if (f)
		{
			logprintf("opened aamp.cfg\n");
			char buf[MAX_URI_LENGTH * 2];
			while (fgets(buf, sizeof(buf), f))
			{
				ProcessCommand(buf, false);
			}
			fclose(f);
		}

		const char *env_aamp_force_aac = getenv("AAMP_FORCE_AAC");
		if(env_aamp_force_aac)
		{
			logprintf("AAMP_FORCE_AAC present: Changing preference to AAC over ATMOS & DD+\n");
			gpGlobalConfig->disableEC3 = 1;
			gpGlobalConfig->disableATMOS = 1;
		}

		const char *env_aamp_min_vod_cache = getenv("AAMP_MIN_VOD_CACHE");
		if(env_aamp_min_vod_cache)
		{
			int minVodCache = 0;
			if(sscanf(env_aamp_min_vod_cache,"%d",&minVodCache))
			{
				logprintf("AAMP_MIN_VOD_CACHE present: Changing min vod cache to %d seconds\n",minVodCache);
				gpGlobalConfig->minVODCacheSeconds = minVodCache;
			}
		}
	}
}

#ifdef STANDALONE_AAMP

#ifdef FOG_HAMMER_TEST

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
 * @brief Main function of AAMP command line interface
 * @param argc
 * @param argv
 * @retval 0 on success
 */
int main(int argc, char **argv)
{
	logprintf("%s\n", argv[0]);
	PlayerInstanceAAMP *playerInstance = new PlayerInstanceAAMP();
	mSingleton = playerInstance; // HACK
	ShowHelp();
	static int chan = 201;
	for (;;)
	{
#if 1
		unsigned r = (unsigned)rand();
		int delay = (r % 100) * 20000 / 100; // leave tuned up to 20 seoonds
		switch (chan)
		{
		case 201: chan = 202; break;
		case 202: chan = 203; break;
		case 203: chan = 204; break;
		case 204: chan = 201; break;
		default:
			exit(0);
			break;
		case AAMP_EVENT_TIMED_METADATA:
			logprintf("AAMP_EVENT_TIMED_METADATA\n");
			break;
		}
		char buf[32];
		sprintf(buf,sizeof(buf), "%d\n", chan);
		logprintf("\n\n***chan=%d*** delay=%dms***", chan, delay);
		ProcessCommand(buf, 1);
		mssleep(delay); // hammertest
#else
		char buf[32];
		sprintf(buf, "3\n");
		ProcessCommand(buf, 1);
		//aamp_ResumeDownloads();

		//		mssleep(1000);
		//	sprintf(buf, "3\n");
		//ProcessCommand(buf, 1);
		//aamp_ResumeDownloads();

		for (;;)
		{
			mssleep(5000); // hammertest
		}
#endif
	}
}
#else

//#define LOG_CLI_EVENTS
#ifdef LOG_CLI_EVENTS
static class PlayerInstanceAAMP *mpPlayerInstanceAAMP;

/**
 * @class myAAMPEventListener
 * @brief
 */
class myAAMPEventListener :public AAMPEventListener
{
public:

	/**
	 * @brief Implementation of event callback
	 * @param e Event
	 */
	void Event(const AAMPEvent & e)
	{
		switch (e.type)
		{
		case AAMP_EVENT_TUNED:
			logprintf("AAMP_EVENT_TUNED\n");
			break;
		case AAMP_EVENT_TUNE_FAILED:
			logprintf("AAMP_EVENT_TUNE_FAILED\n");
			break;
		case AAMP_EVENT_SPEED_CHANGED:
			logprintf("AAMP_EVENT_SPEED_CHANGED\n");
			break;
		case AAMP_EVENT_DRM_METADATA:
                        logprintf("AAMP_DRM_FAILED\n");
                        break;
		case AAMP_EVENT_EOS:
			logprintf("AAMP_EVENT_EOS\n");
			break;
		case AAMP_EVENT_PLAYLIST_INDEXED:
			logprintf("AAMP_EVENT_PLAYLIST_INDEXED\n");
			break;
		case AAMP_EVENT_PROGRESS:
			//			logprintf("AAMP_EVENT_PROGRESS\n");
			break;
		case AAMP_EVENT_CC_HANDLE_RECEIVED:
			logprintf("AAMP_EVENT_CC_HANDLE_RECEIVED\n");
			break;
		case AAMP_EVENT_BITRATE_CHANGED:
			logprintf("AAMP_EVENT_BITRATE_CHANGED\n");
			break;
		}
	}
}; // myAAMPEventListener 

static class myAAMPEventListener *myEventListener;
#endif

/**
 * @brief
 * @param arg
 * @retval
 */
static void * run_commnds(void *arg)
{
    char cmd[MAX_URI_LENGTH * 2];
    char *ret = NULL;
    ShowHelp();
    do
    {
        logprintf("aamp-cli>");
        if((ret = fgets(cmd, sizeof(cmd), stdin))!=NULL)
            ProcessCommand(cmd, true);
    } while (ret != NULL);

    return NULL;
}


/**
 * @brief
 * @param argc
 * @param argv
 * @retval
 */
int main(int argc, char **argv)
{

#ifdef IARM_MGR
	char Init_Str[] = "aamp-cli";
	IARM_Bus_Init(Init_Str);
	IARM_Bus_Connect();
	try
	{
		device::Manager::Initialize();
		logprintf("device::Manager::Initialize() succeeded\n");

	}
	catch (...)
	{
		logprintf("device::Manager::Initialize() failed\n");
	}
#endif
	char driveName = (*argv)[0];
	AampLogManager mLogManager;
	ABRManager mAbrManager;

	/* Set log directory path for AAMP and ABR Manager */
	mLogManager.setLogDirectory(driveName);
	mAbrManager.setLogDirectory(driveName);

	logprintf("**************************************************************************\n");
	logprintf("** ADVANCED ADAPTIVE MICRO PLAYER (AAMP) - COMMAND LINE INTERFACE (CLI) **\n");
	logprintf("**************************************************************************\n");
	PlayerInstanceAAMP *playerInstance = new PlayerInstanceAAMP();
	mSingleton = playerInstance; // HACK
#ifdef LOG_CLI_EVENTS
	myEventListener = new myAAMPEventListener();
	playerInstance->RegisterEvents(myEventListener);
#endif

	ShowHelp();

	char cmd[MAX_URI_LENGTH * 2];

	char *ret = NULL;
	do
	{
		logprintf("aamp-cli> ");
		if((ret = fgets(cmd, sizeof(cmd), stdin))!=NULL)
			ProcessCommand(cmd, true);
	} while (ret != NULL);
}
#endif // FOG_HAMMER_TEST
#endif // STANDALONE_AAMP


/**
 * @brief Executes tear down sequence
 * @param newTune true if operation is a new tune
 */
void PrivateInstanceAAMP::TeardownStream(bool newTune)
{
	pthread_mutex_lock(&mLock);
	//Have to perfom this for trick and stop operations but avoid ad insertion related ones
	if ((mDiscontinuityTuneOperationId != 0) && (!newTune || mState == eSTATE_IDLE))
	{
		//wait for discont tune operation to finish before proceeding with stop
		if (mDiscontinuityTuneOperationInProgress)
		{
			pthread_cond_wait(&mCondDiscontinuity, &mLock);
		}
		else
		{
			//reset discontinuity related flags
			mProcessingDiscontinuity = mProcessingAdInsertion =  false;
			g_source_remove(mDiscontinuityTuneOperationId);
			mDiscontinuityTuneOperationId = 0;
		}
	}
	pthread_mutex_unlock(&mLock);

	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->Stop(false);
		delete mpStreamAbstractionAAMP;
		mpStreamAbstractionAAMP = NULL;
	}

	pthread_mutex_lock(&mLock);
	mFormat = FORMAT_INVALID;
	pthread_mutex_unlock(&mLock);
	if (streamerIsActive)
	{
#ifdef AAMP_STOP_SINK_ON_SEEK
		const bool forceStop = true;
		AAMPEvent event;
		event.type = AAMP_EVENT_CC_HANDLE_RECEIVED;
		event.data.ccHandle.handle = 0;
		traceprintf("%s:%d Sending AAMP_EVENT_CC_HANDLE_RECEIVED with NULL handle\n",__FUNCTION__, __LINE__);
		SendEventSync(event);
		logprintf("%s:%d Sent AAMP_EVENT_CC_HANDLE_RECEIVED with NULL handle\n",__FUNCTION__, __LINE__);
#else
		const bool forceStop = false;
#endif
		if (!forceStop && ((!newTune && gpGlobalConfig->gAampDemuxHLSVideoTsTrack) || gpGlobalConfig->gPreservePipeline))
		{
			mStreamSink->Flush(0, rate);
		}
		else
		{
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
}


/**
 *   @brief Constructor.
 *
 *   @param  streamSink - custom stream sink, NULL for default.
 */
PlayerInstanceAAMP::PlayerInstanceAAMP(StreamSink* streamSink)
{
#ifndef STANDALONE_AAMP
	const char* szJSLib = "libaamp.so";
	mJSBinding_DL = dlopen(szJSLib, RTLD_GLOBAL | RTLD_LAZY);
	logprintf("[AAMP_JS] dlopen(\"%s\")=%p\n", szJSLib, mJSBinding_DL);
#endif

	aamp = new PrivateInstanceAAMP();
	mInternalStreamSink = NULL;
	if (NULL == streamSink)
	{
		mInternalStreamSink = new AAMPGstPlayer(aamp);
		streamSink = mInternalStreamSink;
	}
	aamp->SetStreamSink(streamSink);

	/*Test ad insertion*/
	if(gpGlobalConfig->adURL)
	{
		logprintf("Schedule ad insertion. url %s pos %d\n", gpGlobalConfig->adURL, gpGlobalConfig->adPositionSec);
		InsertAd(gpGlobalConfig->adURL, gpGlobalConfig->adPositionSec);
	}
}


/**
 * @brief PlayerInstanceAAMP Destructor
 */
PlayerInstanceAAMP::~PlayerInstanceAAMP()
{
	if (aamp)
	{
		aamp->Stop();
		delete aamp;
	}
	if (mInternalStreamSink)
	{
		delete mInternalStreamSink;
	}
#ifndef STANDALONE_AAMP
	if (mJSBinding_DL)
	{
		logprintf("[AAMP_JS] dlclose(%p)\n", mJSBinding_DL);
		dlclose(mJSBinding_DL);
	}
#endif
}



/**
 * @brief Setup pipe session with application
 */
void PrivateInstanceAAMP::SetupPipeSession()
{
       bool retVal = false;
       m_fd = -1;
        if(mkfifo(strAAMPPipeName, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == -1) {
                if(errno == EEXIST) {
                // Pipe exists
                        //logprintf("%s:CreatePipe: Pipe already exists",__FUNCTION__);
                        retVal = true;
                }
                else {
                // Error
                	logprintf("%s:CreatePipe: Failed to create named pipe %s for reading errno = %d (%s)\n",
                        	__FUNCTION__,strAAMPPipeName, errno, strerror(errno));
                }
                }
                else {
                        // Success
                        //logprintf("%s:CreatePipe: mkfifo succeeded",__FUNCTION__);
                        retVal = true;
                }

                if(retVal)
                {
                        // Open the named pipe for writing
                        m_fd = open(strAAMPPipeName, O_WRONLY | O_NONBLOCK  );
                        if (m_fd == -1) {
                                // error
                                logprintf("%s:OpenPipe: Failed to open named pipe %s for writing errno = %d (%s)",
                                                __FUNCTION__,strAAMPPipeName, errno, strerror(errno));
                        }
                        else {
                                // Success
                                //logprintf("%s:OpenPipe: Success, created/opened named pipe %s for reading",__FUNCTION__, strAAMPPipeName);
                                retVal = true;
                        }

                }
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
            if(nWritten != nToWrite) {
                // Error
                logprintf("Error writing data written = %d, size = %d errno = %d (%s)\n",
                        nWritten, nToWrite, errno, strerror(errno));
                if(errno == EPIPE) {
                    // broken pipe, lets reset and open again when the pipe is avail
                    ClosePipeSession();
                }
            }
        }
}


/**
 * @brief Stop playback and release resources.
 *
 */
void PlayerInstanceAAMP::Stop(void)
{
	PrivAAMPState state;
	aamp->GetState(state);

	//state will be eSTATE_IDLE or eSTATE_RELEASED, right after an init or post-processing of a Stop call
	if (state == eSTATE_IDLE || state == eSTATE_RELEASED)
	{
		logprintf("aamp_stop ignored since already at eSTATE_IDLE\n");
		return;
	}
	aamp->SetState(eSTATE_IDLE);

	logprintf("aamp_stop\n");
	pthread_mutex_lock(&gMutex);
	for (int i = 0; i < AAMP_MAX_SIMULTANEOUS_INSTANCES; i++)
	{
		if (aamp == gActivePrivAAMPs[i].pAAMP)
		{
			if (gActivePrivAAMPs[i].reTune && aamp->mIsRetuneInProgress)
			{
				// Wait for any ongoing re-tune operation to complete
				pthread_cond_wait(&gCond, &gMutex);
			}
			gActivePrivAAMPs[i].reTune = false;
		}
	}
	pthread_mutex_unlock(&gMutex);
	AAMPLOG_INFO("Stopping Playback at Position '%lld'.\n", aamp->GetPositionMs());
	aamp->Stop();
}


/**
 * @brief de-fog playback URL to play directly from CDN instead of fog
 * @param[in][out] dst Buffer containing URL
 */
static void DeFog(char *dst)
{
	const char *src = strstr(dst, "&recordedUrl=");
	if (src)
	{
		src += 13;
		for (;;)
		{
			char c = *src++;
			if (c == '%')
			{
				size_t len;
				unsigned char *tmp = base16_Decode(src, 2, &len);
				if (tmp)
				{
					*dst++ = tmp[0];
					free(tmp);
				}
				src += 2;
			}
			else if (c == 0 || c == '&')
			{
				*dst++ = 0x00;
				break;
			}
			else
			{
				*dst++ = c;
			}
		}
	}
}


/**
 * @brief
 * @param string
 * @param existingSubStringToReplace
 * @param replacementString
 * @retval
 */
static bool replace_cstring( char *string, const char *existingSubStringToReplace, const char *replacementString )
{
	char *insertionPtr = strstr(string, existingSubStringToReplace);
	if (insertionPtr)
	{
		size_t charsToInsert = strlen(replacementString);
		size_t charsToRemove = strlen(existingSubStringToReplace);
		size_t charsToKeep = strlen(&insertionPtr[charsToRemove]);
		memmove(&insertionPtr[charsToInsert], &insertionPtr[charsToRemove], charsToKeep);
          	insertionPtr[charsToInsert+charsToKeep] = 0x00;
		memcpy(insertionPtr, replacementString, charsToInsert);
		return true;
	}
	return false;
}

/**
 * @brief Common tune operations used on Tune, Seek, SetRate etc
 * @param tuneType type of tune
 */
void PrivateInstanceAAMP::TuneHelper(TuneType tuneType)
{
	bool newTune;
	lastUnderFlowTimeMs[eMEDIATYPE_VIDEO] = 0;
	lastUnderFlowTimeMs[eMEDIATYPE_AUDIO] = 0;
	LazilyLoadConfigIfNeeded();

	if (tuneType == eTUNETYPE_SEEK || tuneType == eTUNETYPE_SEEKTOLIVE)
	{
		mSeekOperationInProgress = true;
	}

	if (eTUNETYPE_LAST == tuneType)
	{
		tuneType = lastTuneType;
	}
	else
	{
		lastTuneType = tuneType;
	}

	newTune = ((eTUNETYPE_NEW_NORMAL == tuneType) || (eTUNETYPE_NEW_SEEK == tuneType));

	TeardownStream(newTune|| (eTUNETYPE_RETUNE == tuneType));

	if (eTUNETYPE_RETUNE == tuneType)
	{
		seek_pos_seconds = GetPositionMs()/1000;
	}


	if (newTune)
	{ // initialize defaults
		if (!mPlayingAd)
		{
			SetState(eSTATE_INITIALIZING);
		}
		culledSeconds = 0;
		durationSeconds = 60 * 60; // 1 hour
		rate = 1.0;
		playStartUTCMS = aamp_GetCurrentTimeMS();
		StoreLanguageList(0,NULL);
		mTunedEventPending = true;
	}

	trickStartUTCMS = -1;

	double playlistSeekPos = seek_pos_seconds - culledSeconds;
	if (playlistSeekPos < 0)
	{
		playlistSeekPos = 0;
		seek_pos_seconds = culledSeconds;
		logprintf("%s:%d Updated seek_pos_seconds %f \n",__FUNCTION__,__LINE__, seek_pos_seconds);
	}

	if (mIsDash)
	{ // mpd
#ifndef DISABLE_DASH
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_MPD(this, playlistSeekPos, rate);
#else
		logprintf("DISABLE_DASH set - Dash playback not available\n");
#endif
	}
	else
	{ // m3u8
		bool enableThrottle = true;
		if (!gpGlobalConfig->gThrottle)
		{
			enableThrottle = false;
		}
		mpStreamAbstractionAAMP = new StreamAbstractionAAMP_HLS(this, playlistSeekPos, rate, enableThrottle);
	}
	AAMPStatusType retVal = mpStreamAbstractionAAMP->Init(tuneType);
	if (retVal != eAAMPSTATUS_OK)
	{
		// Check if the seek position is beyond the duration
		if(retVal == eAAMPSTATUS_SEEK_RANGE_ERROR)
		{
			logprintf("mpStreamAbstractionAAMP Init Failed.Seek Position(%f) out of range(%lld)\n",mpStreamAbstractionAAMP->GetStreamPosition(),(GetDurationMs()/1000));
			NotifyEOSReached();
		}
		else
		{
			logprintf("mpStreamAbstractionAAMP Init Failed.Error(%d)\n",retVal);
			SendErrorEvent(AAMP_TUNE_INIT_FAILED);
			//event.data.mediaError.description = "kECFileNotFound (90)";
			//event.data.mediaError.playerRecoveryEnabled = false;
		}
		return;
	}
	else
	{
		double updatedSeekPosition = mpStreamAbstractionAAMP->GetStreamPosition();
		seek_pos_seconds = updatedSeekPosition + culledSeconds;
#ifndef AAMP_STOP_SINK_ON_SEEK
		logprintf("%s:%d Updated seek_pos_seconds %f \n",__FUNCTION__,__LINE__, seek_pos_seconds);
		if (!mIsDash)
		{
			//Live adjust or syncTrack occurred, sent an updated flush event
			if ((!newTune && gpGlobalConfig->gAampDemuxHLSVideoTsTrack) || gpGlobalConfig->gPreservePipeline)
			{
				mStreamSink->Flush(mpStreamAbstractionAAMP->GetFirstPTS(), rate);
			}
		}
		else
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

		mpStreamAbstractionAAMP->GetStreamFormat(mFormat, mAudioFormat);
		AAMPLOG_INFO("TuneHelper : mFormat %d, mAudioFormat %d\n", mFormat, mAudioFormat);
		mStreamSink->SetVideoZoom(zoom_mode);
		mStreamSink->SetVideoMute(video_muted);
		mStreamSink->SetAudioVolume(audio_volume);
		mStreamSink->Configure(mFormat, mAudioFormat, mpStreamAbstractionAAMP->GetESChangeStatus());
		mpStreamAbstractionAAMP->ResetESChangeStatus();
		if( !mPlayingAd && mAdPosition > seek_pos_seconds)
		{
			mpStreamAbstractionAAMP->SetEndPos(mAdPosition);
		}
		mpStreamAbstractionAAMP->Start();
		mStreamSink->Stream();
	}

	if (tuneType == eTUNETYPE_SEEK || tuneType == eTUNETYPE_SEEKTOLIVE)
	{
		mSeekOperationInProgress = false;
		if (pipeline_paused == true)
		{
			mStreamSink->Pause(true);
		}
	}

	if (newTune && !mPlayingAd)
	{
		SetState(eSTATE_PREPARED);
	}
}


/**
 * @brief Tune to a URL.
 *
 * @param  mainManifestUrl - HTTP/HTTPS url to be played.
 * @param  contentType - content Type.
 */
void PlayerInstanceAAMP::Tune(const char *mainManifestUrl, const char *contentType, bool bFirstAttempt, bool bFinalAttempt)
{
	PrivAAMPState state;
	aamp->GetState(state);
	if (state == eSTATE_RELEASED)
	{
		aamp->SetState(eSTATE_IDLE); //To send the IDLE status event for first channel tune after bootup
	}
	aamp->Tune(mainManifestUrl, contentType, bFirstAttempt, bFinalAttempt);
}


/**
 * @brief Tune to a URL.
 *
 * @param  mainManifestUrl - HTTP/HTTPS url to be played.
 * @param  contentType - content Type.
 */
void PrivateInstanceAAMP::Tune(const char *mainManifestUrl, const char *contentType, bool bFirstAttempt, bool bFinalAttempt)
{
	AAMPLOG_TRACE("aamp_tune: original URL: %s\n", mainManifestUrl);

	TuneType tuneType =  eTUNETYPE_NEW_NORMAL;
	gpGlobalConfig->logging.setLogLevel(eLOGLEVEL_INFO);
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

	if (pipeline_paused)
	{
		// resume downloads and clear paused flag. state change will be done
		// on streamSink configuration.
		pipeline_paused = false;
		ResumeDownloads();
	}

	if (-1 != seek_pos_seconds)
	{
		logprintf("PrivateInstanceAAMP::%s:%d seek position already set, so eTUNETYPE_NEW_SEEK\n", __FUNCTION__, __LINE__);
		tuneType = eTUNETYPE_NEW_SEEK;
	}
	else
	{
		seek_pos_seconds = 0;
	}

	for(int i = 0; i < MAX_CURL_INSTANCE_COUNT; i++)
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

	strncpy(manifestUrl, mainManifestUrl, MAX_URI_LENGTH);
	manifestUrl[MAX_URI_LENGTH-1] = '\0';
	
	mIsDash = !strstr(mainManifestUrl, "m3u8");
	mTuneCompleted 	=	false;
	mTSBEnabled	=	false;
	mIscDVR = strstr(mainManifestUrl, "cdvr-");
	mIsLocalPlayback = (aamp_getHostFromURL(manifestUrl).find(LOCAL_HOST_IP) != std::string::npos);
	mPersistedProfileIndex	=	-1;
	mCurrentDrm = eDRM_NONE;
	
	SetContentType(mainManifestUrl, contentType);
	if(IsVodOrCdvrAsset())
	{
		// DELIA-30843/DELIA-31379 . for CDVR/IVod , offset is set to higher value 	
		// need to adjust the liveoffset on trickplay for ivod/cdvr with 30sec 
		if(!mNewLiveOffsetflag)
		{
			mLiveOffset	=	gpGlobalConfig->cdvrliveOffset;
		}
	}	
	else
	{
		// will be used only for live 
		if(!mNewLiveOffsetflag)
		{
			mLiveOffset	=	gpGlobalConfig->liveOffset;
		}
	}
	logprintf("mLiveOffset: %d", mLiveOffset);

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

	if( !remapUrl )
	{
		if (gpGlobalConfig->mapMPD && !mIsDash && (mContentType != ContentType_EAS)) //Don't map, if it is dash and dont map if it is EAS
		{
			mIsDash = true;
			if (!gpGlobalConfig->fogSupportsDash )
			{
				DeFog(manifestUrl);
			}

			bool urlReplaced = false;

			switch(gpGlobalConfig->mapMPD)
			{
				case 1: 		//Simply change m3u8 to mpd
					urlReplaced = true;
					break;
				case 2:
					urlReplaced |= (replace_cstring(manifestUrl, "col-jitp2.xcr", "col-jitp2-samsung.top") ||
					                replace_cstring(manifestUrl, "linear-nat-pil-red", "coam-tvil-pil")    ||
					                replace_cstring(manifestUrl, "linear-nat-pil", "coam-tvil-pil"));
					break;
				case 3:			//Setting all national channels' FQDN to "ctv-nat-slivel4lb-vip.cmc.co.ndcwest.comcast.net"
					if(strstr(manifestUrl,"-nat-"))
					{
						std::string hostName = aamp_getHostFromURL(manifestUrl);
						urlReplaced |= replace_cstring(manifestUrl, hostName.c_str(), "ctv-nat-slivel4lb-vip.cmc.co.ndcwest.comcast.net");
					}
					else
					{
						urlReplaced |= replace_cstring(manifestUrl, "col-jitp2.xcr", "col-jitp2-samsung.top");
					}
					break;
				default:
					//Let fall back
					break;
			}

			if(!urlReplaced)
			{
				//Fall back channel
				strcpy(manifestUrl, "http://ccr.coam-tvil-pil.xcr.comcast.net/FNCHD_HD_NAT_16756_0_5884597068415311163.mpd");
			}

			replace_cstring(manifestUrl, ".m3u8", ".mpd");
		}
		
		if (gpGlobalConfig->noFog)
		{
			DeFog(manifestUrl);
		}
	
		if (gpGlobalConfig->forceEC3)
		{
			replace_cstring(manifestUrl,".m3u8", "-eac3.m3u8");
		}
		if (gpGlobalConfig->disableEC3 && strstr(manifestUrl,"tsb?") ) // new - limit this option to linear content as part of DELIA-23975
		{
			replace_cstring(manifestUrl, "-eac3.m3u8", ".m3u8");
		}

		if(gpGlobalConfig->bForceHttp)
		{
			replace_cstring(manifestUrl, "https://", "http://");
		}

		if (strstr(manifestUrl,"mpd") ) // new - limit this option to linear content as part of DELIA-23975
		{
			replace_cstring(manifestUrl, "-eac3.mpd", ".mpd");
		} // mpd
	} // !remap_url
  
	if (strstr(manifestUrl,"tsb?"))
	{
		mTSBEnabled = true;
	}
	mIsFirstRequestToFOG = (mIsLocalPlayback == true);
	logprintf("aamp_tune: attempt: %d format: %s URL: %s\n", mTuneAttempts, mIsDash?"DASH":"HLS" ,manifestUrl);

	if(bFirstAttempt)
	{
		mfirstTuneFmt = mIsDash?1:0;
	}
	TuneHelper(tuneType);
}

void PrivateInstanceAAMP::SetContentType(const char *mainManifestUrl, const char *cType)
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
		else if(strstr(mainManifestUrl,"xfinityhome"))
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
	}
	logprintf("Detected ContentType %d (%s)\n",mContentType,cType?cType:"UNKNOWN");
}

/**
 *   @brief Register event handler.
 *
 *   @param  eventListener - pointer to implementation of AAMPEventListener to receive events.
 */
void PlayerInstanceAAMP::RegisterEvents(AAMPEventListener* eventListener)
{
	aamp->RegisterEvents(eventListener);
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

//http://q-cdn4-1-cg17-linear-7151e001.movetv.com/17202/qa/live/Cartoon_Network/b099cab8f2c511e6bacc0025b551a120/video/vid06/0000007dd.m4s
//Request for stream b099cab8f2c511e6bacc0025b551a120 segment 0x7dd is beyond the stream end(0x1b7; limit 0x1b8)

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
char *PrivateInstanceAAMP::LoadFragment(ProfilerBucketType bucketType, const char *fragmentUrl, size_t *len, unsigned int curlInstance, const char *range, MediaType fileType)
{
	profiler.ProfileBegin(bucketType);
	char effectiveUrl[MAX_URI_LENGTH];
	struct GrowableBuffer fragment = { 0, 0, 0 }; // TODO: leaks if thread killed
	if (!GetFile(fragmentUrl, &fragment, effectiveUrl, NULL, range, curlInstance, true, fileType))
	{
		profiler.ProfileError(bucketType);
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
bool PrivateInstanceAAMP::LoadFragment(ProfilerBucketType bucketType, const char *fragmentUrl, struct GrowableBuffer *fragment, unsigned int curlInstance, const char *range, MediaType fileType, long * http_code)
{
	bool ret = true;
	profiler.ProfileBegin(bucketType);
	char effectiveUrl[MAX_URI_LENGTH];
	if (!GetFile(fragmentUrl, fragment, effectiveUrl, http_code, range, curlInstance, false, fileType))
	{
		ret = false;
		profiler.ProfileError(bucketType);
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
	SyncBegin();
	mStreamSink->EndOfStreamReached(mediaType);
	SyncEnd();
}


/**
 * @brief Notifies EndTime to sink, used for client DAI
 * @param mediaType Type of media
 */
void PrivateInstanceAAMP::EndTimeReached(MediaType mediaType)
{
	SyncBegin();
	mProcessingAdInsertion = true;
	mStreamSink->EndOfStreamReached(mediaType);
	SyncEnd();
}


/**
 * @brief Insert ad at position
 * @param url URL of ad asset
 * @param positionSeconds position at which ad to be inserted
 */
void PrivateInstanceAAMP::InsertAd(const char *url, double positionSeconds)
{
	if (url)
	{
		strncpy(mAdUrl, url, MAX_URI_LENGTH - 1);
		mAdUrl[MAX_URI_LENGTH - 1] = 0;
		mAdPosition = positionSeconds;
	}
	else
	{
		mAdUrl[0] = 0;
		mAdPosition = 0;
	}
}


/**
 *   @brief Schedule insertion of ad at given position.
 *
 *   @param  url - HTTP/HTTPS url of the ad
 *   @param  positionSeconds - position at which ad shall be inserted
 */
void PlayerInstanceAAMP::InsertAd(const char *url, double positionSeconds)
{
	aamp->InsertAd(url, positionSeconds);
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
 *   @brief Set playback rate.
 *
 *   @param  rate - Rate of playback.
 *   @param  overshootcorrection - overshoot correction in milliseconds.
 */
void PlayerInstanceAAMP::SetRate(float rate ,int overshootcorrection)
{
	if (aamp->mpStreamAbstractionAAMP)
	{
		if (rate > 0 && aamp->IsLive() && aamp->mpStreamAbstractionAAMP->IsStreamerAtLivePoint() && aamp->rate >=1.0)
		{
			logprintf("%s(): Already at logical live point, hence skipping operation\n", __FUNCTION__);
			aamp->NotifyOnEnteringLive();
			return;
		}


		//DELIA-30274  -- Get the trick play to a closer position 
		//Logic adapted 
		// XRE gives fixed overshoot position , not suited for aamp . So ignoring overshoot correction value 
			// instead use last reported posn vs the time player get play command
		// a. During trickplay , last XRE reported position is stored in aamp->mReportProgressPosn
					/// and last reported time is stored in aamp->mReportProgressTime
		// b. Calculate the time delta  from last reported time
		// c. Using this diff , calculate the best/nearest match position (works out 70-80%)
		// d. If time delta is < 100ms ,still last video fragment rendering is not removed ,but position updated very recently
			// So switch last displayed position - NewPosn -= Posn - ((aamp->rate/4)*1000) 
		// e. If time delta is > 950ms , possibility of next frame to come by the time play event is processed . 
			//So go to next fragment which might get displayed
		// f. If none of above ,maintain the last displayed position .
		// 
		// h. TODO (again trial n error) - for 3x/4x , within 1sec there might multiple frame displayed . Can use timedelta to calculate some more near,to be tried

		int  timeDeltaFromProgReport = (aamp_GetCurrentTimeMS() - aamp->mReportProgressTime);
		// when switching from trick to play mode only 
		if(aamp->rate && rate==1.0)
		{
			if(timeDeltaFromProgReport > 950) // diff > 950 mSec
			{
				// increment by 1x trickplay frame , next possible displayed frame
				aamp->seek_pos_seconds = (aamp->mReportProgressPosn+(aamp->rate*1000))/1000;
			}
			else if(timeDeltaFromProgReport > 100) // diff > 100 mSec
			{
				// Get the last shown frame itself 
				aamp->seek_pos_seconds = aamp->mReportProgressPosn/1000;
			}
			else
			{
				// Go little back to last shown frame 
				aamp->seek_pos_seconds = (aamp->mReportProgressPosn-(aamp->rate*1000))/1000;
			}
		}
		else
		{
			// Coming out of pause mode(aamp->rate=0) or when going into pause mode (rate=0)
			// Show the last position 
			aamp->seek_pos_seconds = aamp->GetPositionMs()/1000;
		}

		aamp->trickStartUTCMS = -1;

		logprintf("aamp_SetRate(%f)overshoot(%d) ProgressReportDelta:(%d) ", rate,overshootcorrection,timeDeltaFromProgReport);
		logprintf("aamp_SetRate Adj position: %f\n", aamp->seek_pos_seconds); // current position relative to tune time
		logprintf("aamp_SetRate rate(%f)->(%f)\n", aamp->rate,rate);
		logprintf("aamp_SetRate cur pipeline: %s\n", aamp->pipeline_paused ? "paused" : "playing");

		if (rate == aamp->rate)
		{ // no change in desired play rate
			if (aamp->pipeline_paused && rate != 0)
			{ // but need to unpause pipeline
				AAMPLOG_INFO("Resuming Playback at Position '%lld'.\n", aamp->GetPositionMs());
				aamp->mpStreamAbstractionAAMP->NotifyPlaybackPaused(false);
				aamp->mStreamSink->Pause(false);
				aamp->NotifyFirstBufferProcessed(); //required since buffers are already cached in paused state
				aamp->pipeline_paused = false;
				aamp->ResumeDownloads();
			}
		}
		else if (rate == 0)
		{
			if (!aamp->pipeline_paused)
			{
				AAMPLOG_INFO("Pausing Playback at Position '%lld'.\n", aamp->GetPositionMs());
				aamp->mpStreamAbstractionAAMP->NotifyPlaybackPaused(true);
				aamp->StopDownloads();
				aamp->mStreamSink->Pause(true);
				aamp->pipeline_paused = true;
			}
		}
		else
		{
			aamp->rate = rate;
			aamp->pipeline_paused = false;
			aamp->ResumeDownloads();
			aamp->TuneHelper(eTUNETYPE_SEEK); // this unpauses pipeline as side effect
		}

		aamp->NotifySpeedChanged(aamp->pipeline_paused ? 0 : aamp->rate);
	}
	else
	{
		aamp->rate = rate;
	}
}


/**
 *   @brief Seek to a time.
 *
 *   @param  secondsRelativeToTuneTime - Seek position for VOD,
 *           relative position from first tune command.
 */
void PlayerInstanceAAMP::Seek(double secondsRelativeToTuneTime)
{
	bool sentSpeedChangedEv = false;
	bool isSeekToLive = false;
	TuneType tuneType = eTUNETYPE_SEEK;

	if (secondsRelativeToTuneTime == AAMP_SEEK_TO_LIVE_POSITION)
	{
		isSeekToLive = true;
		tuneType = eTUNETYPE_SEEKTOLIVE;
	}

	logprintf("aamp_Seek(%f) and seekToLive(%d)\n", secondsRelativeToTuneTime, isSeekToLive);

	if (isSeekToLive && !aamp->IsLive())
	{
		logprintf("%s:%d - Not live, skipping seekToLive\n",__FUNCTION__,__LINE__);
		return;
	}

	if (aamp->IsLive() && aamp->mpStreamAbstractionAAMP->IsStreamerAtLivePoint())
	{
		double currPositionSecs = aamp->GetPositionMs() / 1000.00;
		if (isSeekToLive || secondsRelativeToTuneTime >= currPositionSecs)
		{
			logprintf("%s():Already at live point, skipping operation since requested position(%f) >= currPosition(%f) or seekToLive(%d)\n", __FUNCTION__, secondsRelativeToTuneTime, currPositionSecs, isSeekToLive);
			aamp->NotifyOnEnteringLive();
			return;
		}
	}

	if (aamp->pipeline_paused)
	{
		// resume downloads and clear paused flag. state change will be done
		// on streamSink configuration.
		logprintf("%s(): paused state, so resume downloads\n", __FUNCTION__);
		aamp->pipeline_paused = false;
		aamp->ResumeDownloads();
		sentSpeedChangedEv = true;
	}

	if (tuneType == eTUNETYPE_SEEK)
	{
		aamp->seek_pos_seconds = secondsRelativeToTuneTime;
	}
	if (aamp->rate != 1.0)
	{
		aamp->rate = 1.0;
		sentSpeedChangedEv = true;
	}
	if (aamp->mpStreamAbstractionAAMP)
	{ // for seek while streaming
		aamp->SetState(eSTATE_SEEKING);
		aamp->TuneHelper(tuneType);
		if (sentSpeedChangedEv)
		{
			aamp->NotifySpeedChanged(aamp->rate);
		}
		else
		{
			aamp->SetState(eSTATE_PLAYING);
		}
	}
}


/**
 *   @brief Seek to live point.
 */
void PlayerInstanceAAMP::SeekToLive()
{
	Seek(AAMP_SEEK_TO_LIVE_POSITION);
}


/**
 *   @brief Seek to a time and playback with a new rate.
 *
 *   @param  rate - Rate of playback.
 *   @param  secondsRelativeToTuneTime - Seek position for VOD,
 *           relative position from first tune command.
 */
void PlayerInstanceAAMP::SetRateAndSeek(float rate, double secondsRelativeToTuneTime)
{
	logprintf("aamp_SetRateAndSeek(%f)(%f)\n", rate, secondsRelativeToTuneTime);
	aamp->TeardownStream(false);
	aamp->seek_pos_seconds = secondsRelativeToTuneTime;
	aamp->rate = rate;
	aamp->TuneHelper(eTUNETYPE_SEEK);
}


/**
 *   @brief Set video rectangle.
 *
 *   @param  x - horizontal start position.
 *   @param  y - vertical start position.
 *   @param  w - width.
 *   @param  h - height.
 */
void PlayerInstanceAAMP::SetVideoRectangle(int x, int y, int w, int h)
{
	aamp->SetVideoRectangle(x, y, w, h);
}


/**
 *   @brief Set video zoom.
 *
 *   @param  zoom - zoom mode.
 */
void PlayerInstanceAAMP::SetVideoZoom(VideoZoomMode zoom)
{
	aamp->zoom_mode = zoom;
	if (aamp->mpStreamAbstractionAAMP)
		aamp->SetVideoZoom(zoom);
}


/**
 *   @brief Enable/ Disable Video.
 *
 *   @param  muted - true to disable video, false to enable video.
 */
void PlayerInstanceAAMP::SetVideoMute(bool muted)
{
	aamp->video_muted = muted;
	if (aamp->mpStreamAbstractionAAMP)
		aamp->SetVideoMute(muted);
}


/**
 *   @brief Set Audio Volume.
 *
 *   @param  volume - Minimum 0, maximum 100.
 */
void PlayerInstanceAAMP::SetAudioVolume(int volume)
{
	aamp->audio_volume = volume;
	if (aamp->mpStreamAbstractionAAMP)
		aamp->SetAudioVolume(volume);
}


/**
 *   @brief Set Audio language.
 *
 *   @param  language - Language of audio track.
 */
void PlayerInstanceAAMP::SetLanguage(const char* language)
{
	logprintf("aamp_SetLanguage(%s)->(%s)\n",aamp->language, language);

        if (strncmp(language, aamp->language, MAX_LANGUAGE_TAG_LENGTH) == 0)
                return;

	PrivAAMPState state;
	aamp->GetState(state);
	// There is no active playback session, save the language for later
	if (state == eSTATE_IDLE)
	{
		aamp->UpdateAudioLanguageSelection(language);
		logprintf("aamp_SetLanguage(%s) Language set prior to tune start\n", language);
		return;
	}

	// check if language is supported in manifest languagelist
	if((aamp->IsAudioLanguageSupported(language)) || (!aamp->mMaxLanguageCount))
	{
		aamp->UpdateAudioLanguageSelection(language);
		logprintf("aamp_SetLanguage(%s) Language set\n", language);
		if (aamp->mpStreamAbstractionAAMP)
		{
			logprintf("aamp_SetLanguage(%s) retuning\n", language);

			aamp->discardEnteringLiveEvt = true;

			aamp->seek_pos_seconds = aamp->GetPositionMs()/1000.0;
			aamp->TeardownStream(false);
			aamp->TuneHelper(eTUNETYPE_SEEK);

			aamp->discardEnteringLiveEvt = false;
		}
	}
	else
		logprintf("aamp_SetLanguage(%s) not supported in manifest\n", language);

}


/**
 *   @brief Set array of subscribed tags.
 *
 *   @param  subscribedTags - Array of subscribed tags.
 */
void PlayerInstanceAAMP::SetSubscribedTags(std::vector<std::string> subscribedTags)
{
	logprintf("aamp_SetSubscribedTags()\n");

	aamp->subscribedTags = subscribedTags;

	for (int i=0; i < aamp->subscribedTags.size(); i++) {
	        logprintf("    subscribedTags[%d] = '%s'\n", i, subscribedTags.at(i).data());
	}
}

#ifndef STANDALONE_AAMP

/**
 *   @brief Load AAMP JS object in the specified JS context.
 *
 *   @param  context - JS context.
 */
void PlayerInstanceAAMP::LoadJS(void* context)
{
	logprintf("[AAMP_JS] %s(%p)\n", __FUNCTION__, context);
	if (mJSBinding_DL) {
		void(*loadJS)(void*, void*);
		const char* szLoadJS = "aamp_LoadJS";
		loadJS = (void(*)(void*, void*))dlsym(mJSBinding_DL, szLoadJS);
		if (loadJS) {
			logprintf("[AAMP_JS] %s() dlsym(%p, \"%s\")=%p\n", __FUNCTION__, mJSBinding_DL, szLoadJS, loadJS);
			loadJS(context, this);
		}
	}
}


/**
 *   @brief Unoad AAMP JS object in the specified JS context.
 *
 *   @param  context - JS context.
 */
void PlayerInstanceAAMP::UnloadJS(void* context)
{
	logprintf("[AAMP_JS] %s(%p)\n", __FUNCTION__, context);
	if (mJSBinding_DL) {
		void(*unloadJS)(void*);
		const char* szUnloadJS = "aamp_UnloadJS";
		unloadJS = (void(*)(void*))dlsym(mJSBinding_DL, szUnloadJS);
		if (unloadJS) {
			logprintf("[AAMP_JS] %s() dlsym(%p, \"%s\")=%p\n", __FUNCTION__, mJSBinding_DL, szUnloadJS, unloadJS);
			unloadJS(context);
		}
	}
}
#endif


/**
 *   @brief Support multiple listeners for multiple event type
 *
 *   @param  eventType - type of event.
 *   @param  eventListener - listener for the eventType.
 */
void PlayerInstanceAAMP::AddEventListener(AAMPEventType eventType, AAMPEventListener* eventListener)
{
	aamp->AddEventListener(eventType, eventListener);
}


/**
 *   @brief Remove event listener for eventType.
 *
 *   @param  eventType - type of event.
 *   @param  eventListener - listener to be removed for the eventType.
 */
void PlayerInstanceAAMP::RemoveEventListener(AAMPEventType eventType, AAMPEventListener* eventListener)
{
	aamp->RemoveEventListener(eventType, eventListener);
}


/**
 *   @brief To check playlist type.
 *
 *   @return bool - True if live content, false otherwise
 */
bool PlayerInstanceAAMP::IsLive()
{
	PrivAAMPState state;
	aamp->GetState(state);
	if (state == eSTATE_ERROR)
	{
		logprintf("IsLive is ignored since the player is at eSTATE_ERROR\n");
		return false;
	}
	else
	{
		return aamp->IsLive();
	}
}


/**
 *   @brief Get current audio language.
 *
 *   @return current audio language
 */
char* PlayerInstanceAAMP::GetCurrentAudioLanguage(void)
{
	return aamp->language;
}


/**
 *   @brief Add/Remove a custom HTTP header and value.
 *
 *   @param  headerName - Name of custom HTTP header
 *   @param  headerValue - Value to be passed along with HTTP header.
 */
void PlayerInstanceAAMP::AddCustomHTTPHeader(std::string headerName, std::vector<std::string> headerValue)
{
	aamp->AddCustomHTTPHeader(headerName, headerValue);
}

/**
 *   @brief Set License Server URL.
 *
 *   @param  url - URL of the server to be used for license requests
 *   @param  type - DRM Type(PR/WV) for which the server URL should be used, global by default
 */
void PlayerInstanceAAMP::SetLicenseServerURL(const char *url, DRMSystems type)
{
	aamp->SetLicenseServerURL(url, type);
}


/**
 *   @brief Indicates if session token has to be used with license request or not.
 *
 *   @param  isAnonymous - True if session token should be blank and false otherwise.
 */
void PlayerInstanceAAMP::SetAnonymousRequest(bool isAnonymous)
{
	aamp->SetAnonymousRequest(isAnonymous);
}


/**
 *   @brief Set VOD Trickplay FPS.
 *
 *   @param  vodTrickplayFPS - FPS to be used for VOD Trickplay
 */
void PlayerInstanceAAMP::SetVODTrickplayFPS(int vodTrickplayFPS)
{
	aamp->SetVODTrickplayFPS(vodTrickplayFPS);
}


/**
 *   @brief Set Linear Trickplay FPS.
 *
 *   @param  linearTrickplayFPS - FPS to be used for Linear Trickplay
 */
void PlayerInstanceAAMP::SetLinearTrickplayFPS(int linearTrickplayFPS)
{
	aamp->SetLinearTrickplayFPS(linearTrickplayFPS);
}

/**
 *   @brief Set Live Offset.
 *
 *   @param  liveoffset- Live Offset
 */
void PlayerInstanceAAMP::SetLiveOffset(int liveoffset)
{
	aamp->SetLiveOffset(liveoffset);
}


/**
 *   @brief To set the error code to be used for playback stalled error.
 *
 *   @param  errorCode - error code for playback stall errors.
 */
void PlayerInstanceAAMP::SetStallErrorCode(int errorCode)
{
	aamp->SetStallErrorCode(errorCode);
}


/**
 *   @brief To set the timeout value to be used for playback stall detection.
 *
 *   @param  timeoutMS - timeout in milliseconds for playback stall detection.
 */
void PlayerInstanceAAMP::SetStallTimeout(int timeoutMS)
{
	aamp->SetStallTimeout(timeoutMS);
}


/**
 *   @brief Set report interval duration
 *
 *   @param  reportIntervalMS - report interval duration in MS
 */
void PlayerInstanceAAMP::SetReportInterval(int reportIntervalMS)
{
	aamp->SetReportInterval(reportIntervalMS);
}


/**
 *   @brief To get the current playback position.
 *
 *   @ret current playback position in seconds
 */
double PlayerInstanceAAMP::GetPlaybackPosition()
{
	return (aamp->GetPositionMs() / 1000.00);
}


/**
*   @brief To get the current asset's duration.
*
*   @ret duration in seconds
*/
double PlayerInstanceAAMP::GetPlaybackDuration()
{
	return (aamp->GetDurationMs() / 1000.00);
}


/**
 *   @brief To get the current AAMP state.
 *
 *   @ret current AAMP state
 */
PrivAAMPState PlayerInstanceAAMP::GetState(void)
{
	PrivAAMPState currentState;
	aamp->GetState(currentState);
	return currentState;
}


/**
 *   @brief To get the bitrate of current video profile.
 *
 *   @ret bitrate of video profile
 */
long PlayerInstanceAAMP::GetVideoBitrate(void)
{
	long bitrate = 0;
	if (aamp->mpStreamAbstractionAAMP)
	{
		bitrate = aamp->mpStreamAbstractionAAMP->GetVideoBitrate();
	}
	return bitrate;
}


/**
 *   @brief To set a preferred bitrate for video profile.
 *
 *   @param[in] preferred bitrate for video profile
 */
void PlayerInstanceAAMP::SetVideoBitrate(long bitrate)
{
	if (aamp->mpStreamAbstractionAAMP)
	{
		//Switch off ABR and set bitrate
		aamp->mpStreamAbstractionAAMP->SetVideoBitrate(bitrate);
	}
}


/**
 *   @brief To get the bitrate of current audio profile.
 *
 *   @ret bitrate of audio profile
 */
long PlayerInstanceAAMP::GetAudioBitrate(void)
{
	long bitrate = 0;
	if (aamp->mpStreamAbstractionAAMP)
	{
		bitrate = aamp->mpStreamAbstractionAAMP->GetAudioBitrate();
	}
	return bitrate;
}


/**
 *   @brief To set a preferred bitrate for audio profile.
 *
 *   @param[in] preferred bitrate for audio profile
 */
void PlayerInstanceAAMP::SetAudioBitrate(long bitrate)
{
	//no-op for now
}


/**
 *   @brief To get the current audio volume.
 *
 *   @ret audio volume
 */
int PlayerInstanceAAMP::GetAudioVolume(void)
{
	return aamp->audio_volume;
}


/**
 *   @brief To get the current playback rate.
 *
 *   @ret current playback rate
 */
float PlayerInstanceAAMP::GetPlaybackRate(void)
{
	return aamp->rate;
}


/**
 *   @brief To get the available video bitrates.
 *
 *   @ret available video bitrates
 */
std::vector<long> PlayerInstanceAAMP::GetVideoBitrates(void)
{
	if (aamp->mpStreamAbstractionAAMP)
	{
		return aamp->mpStreamAbstractionAAMP->GetVideoBitrates();
	}
	return std::vector<long>();
}


/**
 *   @brief To get the available audio bitrates.
 *
 *   @ret available audio bitrates
 */
std::vector<long> PlayerInstanceAAMP::GetAudioBitrates(void)
{
	if (aamp->mpStreamAbstractionAAMP)
	{
		return aamp->mpStreamAbstractionAAMP->GetAudioBitrates();
	}
	return std::vector<long>();
}


/**
 *   @brief To set the initial bitrate value.
 *
 *   @param[in] initial bitrate to be selected
 */
void PlayerInstanceAAMP::SetInitialBitrate(long bitrate)
{
	aamp->SetInitialBitrate(bitrate);
}


/**
 *   @brief To set the initial bitrate value for 4K assets.
 *
 *   @param[in] initial bitrate to be selected for 4K assets
 */
void PlayerInstanceAAMP::SetInitialBitrate4K(long bitrate4K)
{
	aamp->SetInitialBitrate4K(bitrate4K);
}


/**
 *   @brief To set the network download timeout value.
 *
 *   @param[in] preferred timeout value
 */
void PlayerInstanceAAMP::SetNetworkTimeout(int timeout)
{
	aamp->SetNetworkTimeout(timeout);
}


/**
 *   @brief To set the download buffer size value
 *
 *   @param[in] preferred download buffer size
 */
void PlayerInstanceAAMP::SetDownloadBufferSize(int bufferSize)
{
	aamp->SetDownloadBufferSize(bufferSize);
}


/**
 *   @brief Set preferred DRM.
 *
 *   @param[in] drmType - preferred DRM type
 */
void PlayerInstanceAAMP::SetPreferredDRM(DRMSystems drmType)
{
	aamp->SetPreferredDRM(drmType);
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
#if 0 // #ifdef WIN32 (needed workaround?)
	while (timeInMs > 0)
	{
		if (aamp_DownloadsAreEnabled())
		{
			struct timespec req, rem;
			int sliceMs = 10;
			req.tv_sec = sliceMs / 1000;
			req.tv_nsec = (sliceMs % 1000) * 1000000;
			nanosleep(&req, &rem);
			timeInMs -= sliceMs;
		}
		else
		{
			logprintf("interrupted!\n");
			break;
		}
	}
#else
	if (timeInMs > 0)
	{
		struct timespec ts;
		struct timeval tv;
		int ret;
		gettimeofday(&tv, NULL);
		ts.tv_sec = time(NULL) + timeInMs / 1000;
		ts.tv_nsec = (long)(tv.tv_usec * 1000 + 1000 * 1000 * (timeInMs % 1000));
		ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
		ts.tv_nsec %= (1000 * 1000 * 1000);
		pthread_mutex_lock(&mLock);
		if (mDownloadsEnabled)
		{
			ret = pthread_cond_timedwait(&mDownloadsDisabled, &mLock, &ts);
			if (0 == ret)
			{
				logprintf("sleep interrupted!\n");
			}
#ifndef WIN32
			else if (ETIMEDOUT != ret)
			{
				logprintf("sleep - condition wait failed %s\n", strerror(ret));
			}
#endif
		}
		pthread_mutex_unlock(&mLock);
	}
#endif
}


/**
 * @brief Get stream duration
 * @retval duration is milliseconds
 */
long long PrivateInstanceAAMP::GetDurationMs()
{
	return (long long)(durationSeconds*1000.0);
}


/**
 * @brief Get current stream position
 * @retval current stream position in ms
 */
long long PrivateInstanceAAMP::GetPositionMs()
{
	long long positionMiliseconds = (seek_pos_seconds)* 1000.0;
	if (!pipeline_paused && trickStartUTCMS >= 0)
	{
		long long elapsedTime = aamp_GetCurrentTimeMS() - trickStartUTCMS;
		positionMiliseconds += elapsedTime*rate;

		if (positionMiliseconds < 0)
		{
			logprintf("%s : Correcting positionMiliseconds %lld to zero\n", __FUNCTION__, positionMiliseconds);
			positionMiliseconds = 0;
		}
		else if (mpStreamAbstractionAAMP)
		{
			if (!mpStreamAbstractionAAMP->IsLive())
			{
				long long durationMs  = GetDurationMs();
				if(positionMiliseconds > durationMs)
				{
					logprintf("%s : Correcting positionMiliseconds %lld to duration %lld\n", __FUNCTION__, positionMiliseconds, durationMs);
					positionMiliseconds = durationMs;
				}
			}
			else
			{
				long long tsbEndMs = GetDurationMs() + (culledSeconds * 1000.0);
				if(positionMiliseconds > tsbEndMs)
				{
					logprintf("%s : Correcting positionMiliseconds %lld to duration %lld\n", __FUNCTION__, positionMiliseconds, tsbEndMs);
					positionMiliseconds = tsbEndMs;
				}
			}
		}
		// note, using mStreamerInterface->GetPositionMilliseconds() instead of elapsedTime
		// would likely be more accurate, but would need to be tested to accomodate
		// and compensate for FF/REW play rates
	}
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
	bool ret = false;
	if (mpStreamAbstractionAAMP)
	{
		ret = mpStreamAbstractionAAMP->IsLive();
	}
	return ret;
}


/**
 * @brief Stop playback and release resources.
 *
 */
void PrivateInstanceAAMP::Stop()
{
	// Stopping the playback, release all DRM context
	if (mpStreamAbstractionAAMP)
	{
		mpStreamAbstractionAAMP->Stop(true);
	}

	TeardownStream(true);
	pthread_mutex_lock(&mLock);
	if (mPendingAsyncEvents.size() > 0)
	{
		logprintf("PrivateInstanceAAMP::%s() - mPendingAsyncEvents.size - %d\n", __FUNCTION__, mPendingAsyncEvents.size());
		for (std::map<gint, bool>::iterator it = mPendingAsyncEvents.begin(); it != mPendingAsyncEvents.end(); it++)
		{
			if (it->first != 0)
			{
				if (it->second)
				{
					logprintf("PrivateInstanceAAMP::%s() - remove id - %d\n", __FUNCTION__, (int) it->first);
					g_source_remove(it->first);
				}
				else
				{
					logprintf("PrivateInstanceAAMP::%s() - Not removing id - %d as not pending\n", __FUNCTION__, (int) it->first);
				}
			}
		}
		mPendingAsyncEvents.clear();
	}
	if (timedMetadata.size() > 0)
	{
		logprintf("PrivateInstanceAAMP::%s() - timedMetadata.size - %d\n", __FUNCTION__, timedMetadata.size());
		timedMetadata.clear();
	}
	pthread_mutex_unlock(&mLock);
	seek_pos_seconds = -1;
	culledSeconds = 0;
	durationSeconds = 0;
	rate = 1;
	mPlayingAd = false;
	ClearPlaylistCache();
	mEnableCache = true;
	mSeekOperationInProgress = false;
	mMaxLanguageCount = 0; // reset language count
}



/**
 * @brief Report TimedMetadata events
 * @param timeMilliseconds time in milliseconds
 * @param szName name of metadata
 * @param szContent  metadata content
 * @param nb unused
 */
void PrivateInstanceAAMP::ReportTimedMetadata(double timeMilliseconds, const char* szName, const char* szContent, int nb)
{
	std::string content(szContent, nb);
	bool bFireEvent = false;

	// Check if timedMetadata was already reported
	std::vector<TimedMetadata>::iterator i;
	for (i=timedMetadata.begin(); i != timedMetadata.end(); i++)
	{
		if (i->_timeMS < timeMilliseconds)
			continue;

		// Does an entry already exist?
		if ((i->_timeMS == timeMilliseconds) && (i->_name.compare(szName) == 0))
		{
			if (i->_content.compare(content) == 0)
			{
				//logprintf("aamp_ReportTimedMetadata(%ld, '%s', '%s', nb) DUPLICATE\n", (long)timeMilliseconds, szName, content.data(), nb);
			} else {
				//logprintf("aamp_ReportTimedMetadata(%ld, '%s', '%s', nb) REPLACE\n", (long)timeMilliseconds, szName, content.data(), nb);
				i->_content = content;
				bFireEvent = true;
			}
			break;
		}

		if (i->_timeMS > timeMilliseconds)
		{
			//logprintf("aamp_ReportTimedMetadata(%ld, '%s', '%s', nb) INSERT\n", (long)timeMilliseconds, szName, content.data(), nb);
			timedMetadata.insert(i, TimedMetadata(timeMilliseconds, szName, content));
			bFireEvent = true;
			break;
		}
	}

	if (i == timedMetadata.end())
	{
		//logprintf("aamp_ReportTimedMetadata(%ld, '%s', '%s', nb) APPEND\n", (long)timeMilliseconds, szName, content.data(), nb);
		timedMetadata.push_back(TimedMetadata(timeMilliseconds, szName, content));
		bFireEvent = true;
	}

	if (bFireEvent)
	{
		AAMPEvent eventData;
		eventData.type = AAMP_EVENT_TIMED_METADATA;
		eventData.data.timedMetadata.timeMilliseconds = timeMilliseconds;
		eventData.data.timedMetadata.szName = (szName == NULL) ? "" : szName;
		eventData.data.timedMetadata.szContent = content.data();

		if (gpGlobalConfig->logging.progress)
		{
			logprintf("aamp timedMetadata: [%ld] '%s'\n",
				(long)(eventData.data.timedMetadata.timeMilliseconds),
				eventData.data.timedMetadata.szContent);
		}
		SendEventSync(eventData);
	}
}

#ifdef AAMP_HARVEST_SUPPORT_ENABLED

/**
 * @brief Check if harvest is required
 * @param modifyCount true to decrement harvest value
 * @retval true if harvest is required
 */
bool PrivateInstanceAAMP::HarvestFragments(bool modifyCount)
{
	if (gpGlobalConfig->harvest)
	{
		logprintf("aamp harvest: %d\n", gpGlobalConfig->harvest);
		if(modifyCount)
		{
			gpGlobalConfig->harvest--;
			if(!gpGlobalConfig->harvest)
			{
				logprintf("gpGlobalConfig->harvest zero, no more harvesting\n");
			}
		}
		return true;
	}
	return false;
}
#endif

/**
 * @brief Notify first frame is displayed. Sends CC handle event to listeners.
 */
void PrivateInstanceAAMP::NotifyFirstFrameReceived()
{
	SetState(eSTATE_PLAYING);
#ifdef AAMP_STOP_SINK_ON_SEEK
	/*Do not send event on trickplay as CC is not enabled*/
	if (1.0 != rate)
	{
		logprintf("PrivateInstanceAAMP::%s:%d : not sending cc handle as rate = %f\n", __FUNCTION__, __LINE__, rate);
		return;
	}
#endif
	if (mStreamSink != NULL)
	{
		AAMPEvent event;
		event.type = AAMP_EVENT_CC_HANDLE_RECEIVED;
		event.data.ccHandle.handle = mStreamSink->getCCDecoderHandle();
		SendEventSync(event);
	}
}

/**
 * @brief Signal discontinuity of track.
 * Called from StreamAbstractionAAMP to signal discontinuity
 * @param track MediaType of the track
 * @retval true if discontinuity is handled.
 */
bool PrivateInstanceAAMP::Discontinuity(MediaType track)
{
	bool ret;
	SyncBegin();
	ret = mStreamSink->Discontinuity(track);
	SyncEnd();
	if (ret)
	{
		mProcessingDiscontinuity = true;
	}
	return ret;
}


/**
 * @brief
 * @param ptr
 * @retval
 */
static gboolean PrivateInstanceAAMP_Retune(gpointer ptr)
{
	PrivateInstanceAAMP* aamp = (PrivateInstanceAAMP*) ptr;
	bool activeAAMPFound = false;
	bool reTune = false;
	int i;
	pthread_mutex_lock(&gMutex);
	for ( i = 0; i < AAMP_MAX_SIMULTANEOUS_INSTANCES; i++)
	{
		if (aamp == gActivePrivAAMPs[i].pAAMP)
		{
			activeAAMPFound = true;
			reTune = gActivePrivAAMPs[i].reTune;
			break;
		}
	}
	if (!activeAAMPFound)
	{
		logprintf("PrivateInstanceAAMP::%s : %p not in Active AAMP list\n", __FUNCTION__, aamp);
	}
	else if (!reTune)
	{
		logprintf("PrivateInstanceAAMP::%s : %p reTune flag not set\n", __FUNCTION__, aamp);
	}
	else
	{
		aamp->mIsRetuneInProgress = true;
		pthread_mutex_unlock(&gMutex);

		aamp->TuneHelper(eTUNETYPE_RETUNE);

		pthread_mutex_lock(&gMutex);
		aamp->mIsRetuneInProgress = false;
		gActivePrivAAMPs[i].reTune = false;
		pthread_cond_signal(&gCond);
	}
	pthread_mutex_unlock(&gMutex);
	return G_SOURCE_REMOVE;
}


/**
 * @brief Schedules retune or discontinuity processing based on state.
 * @param errorType type of playback error
 * @param trackType media type
 */
void PrivateInstanceAAMP::ScheduleRetune(PlaybackErrorType errorType, MediaType trackType)
{
	if (1.0 == rate && ContentType_EAS != mContentType)
	{
		PrivAAMPState state;
		GetState(state);
		if (state != eSTATE_PLAYING || mSeekOperationInProgress)
		{
			logprintf("PrivateInstanceAAMP::%s : Not processing reTune since state = %d, mSeekOperationInProgress = %d\n",
						__FUNCTION__, state, mSeekOperationInProgress);
			return;
		}

		/*If underflow is caused by a discontinuity processing, continue playback from discontinuity*/
		if (IsDiscontinuityProcessPending())
		{
			pthread_mutex_lock(&mLock);
			if (mDiscontinuityTuneOperationId != 0 || mDiscontinuityTuneOperationInProgress)
			{
				pthread_mutex_unlock(&mLock);
				logprintf("PrivateInstanceAAMP::%s:%d Discontinuity Tune handler already spawned(%d) or inprogress(%d)\n",
					__FUNCTION__, __LINE__, mDiscontinuityTuneOperationId, mDiscontinuityTuneOperationInProgress);
				return;
			}
			mDiscontinuityTuneOperationId = g_idle_add(PrivateInstanceAAMP_ProcessDiscontinuity, (gpointer) this);
			pthread_mutex_unlock(&mLock);

			logprintf("PrivateInstanceAAMP::%s:%d  Underflow due to discontinuity handled\n", __FUNCTION__, __LINE__);
			return;
		}
		else if (!gpGlobalConfig->internalReTune)
		{
			logprintf("PrivateInstanceAAMP::%s : Ignore reTune as disabled in configuration\n", __FUNCTION__);
			return;
		}
		bool activeAAMPFound = false;
		pthread_mutex_lock(&gMutex);
		for (int i = 0; i < AAMP_MAX_SIMULTANEOUS_INSTANCES; i++)
		{
			if (this == gActivePrivAAMPs[i].pAAMP)
			{
				if (gActivePrivAAMPs[i].reTune)
				{
					logprintf("PrivateInstanceAAMP::%s : Already scheduled\n", __FUNCTION__);
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
								gActivePrivAAMPs[i].numPtsErrors++;
								logprintf("PrivateInstanceAAMP::%s : numPtsErrors %lld,  ptsErrorThreshold %lld\n", __FUNCTION__, gActivePrivAAMPs[i].numPtsErrors, gpGlobalConfig->ptsErrorThreshold);
								if (gActivePrivAAMPs[i].numPtsErrors >= gpGlobalConfig->ptsErrorThreshold)
								{
									gActivePrivAAMPs[i].numPtsErrors = 0;
									gActivePrivAAMPs[i].reTune = true;
									logprintf("PrivateInstanceAAMP::%s : Schedule Retune. diffMs %lld < threshold %lld\n", __FUNCTION__, diffMs, AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS);
									g_idle_add(PrivateInstanceAAMP_Retune, (gpointer)this);
								}
							}
							else
							{
								gActivePrivAAMPs[i].numPtsErrors = 0;
								logprintf("PrivateInstanceAAMP::%s : Not scheduling reTune since (diff %lld > threshold %lld).\n",
										__FUNCTION__, diffMs, AAMP_MAX_TIME_BW_UNDERFLOWS_TO_TRIGGER_RETUNE_MS, gActivePrivAAMPs[i].numPtsErrors, gpGlobalConfig->ptsErrorThreshold);
							}
						}
						else
						{
							gActivePrivAAMPs[i].numPtsErrors = 0;
							logprintf("PrivateInstanceAAMP::%s : Not scheduling reTune since first underflow.\n", __FUNCTION__);
						}
						lastUnderFlowTimeMs[trackType] = now;
					}
					else if(eDASH_ERROR_STARTTIME_RESET == errorType)
					{
						logprintf("PrivateInstanceAAMP::%s : Schedule Retune to handle start time reset.\n", __FUNCTION__);
						gActivePrivAAMPs[i].reTune = true;
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
			logprintf("PrivateInstanceAAMP::%s : %p not in Active AAMP list\n", __FUNCTION__, this);
		}
	}
}


/**
 * @brief PrivateInstanceAAMP Constructor
 */
PrivateInstanceAAMP::PrivateInstanceAAMP()
{
	LazilyLoadConfigIfNeeded();
	mpStreamAbstractionAAMP = NULL;
	mFormat = FORMAT_INVALID;
	mAudioFormat = FORMAT_INVALID;
	pthread_cond_init(&mDownloadsDisabled, NULL);
	mDownloadsEnabled = true;
	mStreamSink = NULL;
	mbDownloadsBlocked = false;
	streamerIsActive = false;
	seek_pos_seconds = -1;
	rate = 0;
	strcpy(language,"en");
	mMaxLanguageCount = 0;
	mPersistedProfileIndex	= 0;
	mTSBEnabled     =       false;
	mIscDVR = false;
	mLiveOffset = AAMP_LIVE_OFFSET;
	mNewLiveOffsetflag = false;
	zoom_mode = VIDEO_ZOOM_FULL;
	pipeline_paused = false;
	trickStartUTCMS = -1;
	playStartUTCMS = 0;
	durationSeconds = 0.0;
	mReportProgressPosn = 0.0;
	mReportProgressTime=0;
	culledSeconds = 0.0;
	maxRefreshPlaylistIntervalSecs = DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS / 1000;
	initialTuneTimeMs = 0;
	lastTuneType = eTUNETYPE_NEW_NORMAL;
	fragmentCollectorThreadID = 0;
	audio_volume = 100;
	m_fd = -1;
	mTuneCompleted = false;
	mFirstTune = true;
	mState = eSTATE_RELEASED;
	mProcessingDiscontinuity = false;
	mDiscontinuityTuneOperationInProgress = false;
	mProcessingAdInsertion = false;
	mSeekOperationInProgress = false;
	lastUnderFlowTimeMs[eMEDIATYPE_VIDEO] = 0;
	lastUnderFlowTimeMs[eMEDIATYPE_AUDIO] = 0;
	mAvailableBandwidth = 0;
	mCurrentDrm = eDRM_NONE;
	pthread_mutexattr_init(&mMutexAttr);
	pthread_mutexattr_settype(&mMutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&mLock, &mMutexAttr);

	for (int i = 0; i < MAX_CURL_INSTANCE_COUNT; i++)
	{
		curl[i] = NULL;
		//cookieHeaders[i].clear();
		httpRespHeaders[i].type = eHTTPHEADERTYPE_UNKNOWN;
		httpRespHeaders[i].data.clear();
	}
	mEventListener = NULL;
	for (int i = 0; i < AAMP_MAX_NUM_EVENTS; i++)
	{
		mEventListeners[i] = NULL;
	}

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		mbTrackDownloadsBlocked[i] = false;
	}

	pthread_mutex_lock(&gMutex);
	for (int i = 0; i < AAMP_MAX_SIMULTANEOUS_INSTANCES; i++)
	{
		if (NULL == gActivePrivAAMPs[i].pAAMP)
		{
			gActivePrivAAMPs[i].pAAMP = this;
			gActivePrivAAMPs[i].reTune = false;
			break;
		}
	}
	mIsRetuneInProgress = false;
	pthread_mutex_unlock(&gMutex);
	discardEnteringLiveEvt = false;
	licenceFromManifest = false;
	mPlayingAd = false;
	mAdPosition = 0;
	mAdUrl[0] = 0;
	mTunedEventPending = false;
	mPendingAsyncEvents.clear();

	// Add Connection: Keep-Alive custom header - DELIA-26832
	mCustomHeaders["Connection:"] = std::vector<std::string> { "Keep-Alive" };
	mIsFirstRequestToFOG = false;
	mEnableCache = true;
	mDiscontinuityTuneOperationId = 0;
	pthread_cond_init(&mCondDiscontinuity, NULL);

    /* START: Added As Part of DELIA-28363 and DELIA-28247 */
    IsTuneTypeNew = false;
    /* END: Added As Part of DELIA-28363 and DELIA-28247 */

	mIsLocalPlayback = false;
	previousAudioType = eAUDIO_UNKNOWN;
}


/**
 * @brief PrivateInstanceAAMP Destructor
 */
PrivateInstanceAAMP::~PrivateInstanceAAMP()
{
	pthread_mutex_lock(&gMutex);
	for (int i = 0; i < AAMP_MAX_SIMULTANEOUS_INSTANCES; i++)
	{
		if (this == gActivePrivAAMPs[i].pAAMP)
		{
			gActivePrivAAMPs[i].pAAMP = NULL;
		}
	}
	pthread_mutex_unlock(&gMutex);

	for (int i = 0; i < AAMP_MAX_NUM_EVENTS; i++)
	{
		while (mEventListeners[i] != NULL)
		{
			ListenerData* pListener = mEventListeners[i];
			mEventListeners[i] = pListener->pNext;
			delete pListener;
		}
	}
	pthread_cond_destroy(&mDownloadsDisabled);
	pthread_cond_destroy(&mCondDiscontinuity);
	pthread_mutex_destroy(&mLock);
}


/**
 * @brief Sets aamp state
 * @param state state to be set
 */
void PrivateInstanceAAMP::SetState(PrivAAMPState state)
{
	if (mState == state)
	{
		return;
	}

	pthread_mutex_lock(&mLock);
	mState = state;
	pthread_mutex_unlock(&mLock);

	if (mEventListener || mEventListeners[0] || mEventListeners[AAMP_EVENT_STATE_CHANGED])
	{
		if (mState == eSTATE_PREPARING)
		{
			AAMPEvent eventData;
			eventData.type = AAMP_EVENT_STATE_CHANGED;
			eventData.data.stateChanged.state = eSTATE_INITIALIZED;
			SendEventSync(eventData);
		}

		AAMPEvent eventData;
		eventData.type = AAMP_EVENT_STATE_CHANGED;
		eventData.data.stateChanged.state = mState;
		SendEventSync(eventData);
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
	mStreamSink->NotifyFragmentCachingComplete();
}


/**
 * @brief Send tuned event to listeners if required
 * @retval true if event is scheduled, false if discarded
 */
bool PrivateInstanceAAMP::SendTunedEvent()
{
	bool ret = false;

	// Required for synchronising btw audio and video tracks in case of cdmidecryptor
	pthread_mutex_lock(&mLock);

	ret = mTunedEventPending;
	mTunedEventPending = false;

	pthread_mutex_unlock(&mLock);

	if(ret)
	{
		SendEventAsync(AAMP_EVENT_TUNED);
	}
	return ret;
}


/**
 * @brief Check if fragment Buffering is required before playing.
 *
 * @retval true if buffering is required.
 */
bool PrivateInstanceAAMP::IsFragmentBufferingRequired()
{
	return mpStreamAbstractionAAMP->IsFragmentBufferingRequired();
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
void PrivateInstanceAAMP::SetCallbackAsDispatched(gint id)
{
	pthread_mutex_lock(&mLock);
	std::map<gint, bool>::iterator  itr = mPendingAsyncEvents.find(id);
	if(itr != mPendingAsyncEvents.end())
	{
		assert (itr->second);
		mPendingAsyncEvents.erase(itr);
	}
	else
	{
		logprintf("%s:%d id not in mPendingAsyncEvents, insert and mark as not pending\n", __FUNCTION__, __LINE__, id);
		mPendingAsyncEvents[id] = false;
	}
	pthread_mutex_unlock(&mLock);
}


/**
 * @brief Set an idle callback to pending state
 * @param id Idle task Id
 */
void PrivateInstanceAAMP::SetCallbackAsPending(gint id)
{
	pthread_mutex_lock(&mLock);
	std::map<gint, bool>::iterator  itr = mPendingAsyncEvents.find(id);
	if(itr != mPendingAsyncEvents.end())
	{
		assert (!itr->second);
		logprintf("%s:%d id already in mPendingAsyncEvents and completed, erase it\n", __FUNCTION__, __LINE__, id);
		mPendingAsyncEvents.erase(itr);
	}
	else
	{
		mPendingAsyncEvents[id] = true;
	}	pthread_mutex_unlock(&mLock);
}


/**
 *   @brief Add/Remove a custom HTTP header and value.
 *
 *   @param  headerName - Name of custom HTTP header
 *   @param  headerValue - Value to be pased along with HTTP header.
 */
void PrivateInstanceAAMP::AddCustomHTTPHeader(std::string headerName, std::vector<std::string> headerValue)
{
	// Header name should be ending with :
	if(headerName.back() != ':')
	{
		headerName += ':';
	}

	if (headerValue.size() != 0)
	{
		mCustomHeaders[headerName] = headerValue;
	}
	else
	{
		mCustomHeaders.erase(headerName);
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
	char **serverUrl = &(gpGlobalConfig->licenseServerURL);
	if (type == eDRM_MAX_DRMSystems)
	{
		// Local aamp.cfg config trumps JS PP config
		if (gpGlobalConfig->licenseServerLocalOverride)
		{
			return;
		}
	}
	else if (type == eDRM_PlayReady)
	{
		serverUrl = &(gpGlobalConfig->prLicenseServerURL);
	}
	else if (type == eDRM_WideVine)
	{
		serverUrl = &(gpGlobalConfig->wvLicenseServerURL);
	}
	else
	{
		AAMPLOG_ERR("PrivateInstanceAAMP::%s - invalid drm type received.\n", __FUNCTION__);
		return;
	}

	AAMPLOG_INFO("PrivateInstanceAAMP::%s - set license url - %s for type - %d\n", __FUNCTION__, url, type);
	if (*serverUrl != NULL)
	{
		free(*serverUrl);
	}
	*serverUrl = strdup(url);
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
	logprintf("PrivateInstanceAAMP::%s(), vodTrickplayFPS %d\n", __FUNCTION__, vodTrickplayFPS);
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
	logprintf("PrivateInstanceAAMP::%s(), linearTrickplayFPS %d\n", __FUNCTION__, linearTrickplayFPS);
}


/**
 *   @brief Set live offset [Sec]
 *
 *   @param SetLiveOffset - Live Offset
 */
void PrivateInstanceAAMP::SetLiveOffset(int liveoffset)
{
	mLiveOffset = liveoffset;
	mNewLiveOffsetflag = true;
	logprintf("PrivateInstanceAAMP::%s(), liveoffset %d\n", __FUNCTION__, liveoffset);
}


/**
 * @brief Insert playlist to playlist cache
 * @param url URL corresponding to playlist
 * @param buffer Contains the playlist
 * @param effectiveUrl Effective URL of playlist
 */
void PrivateInstanceAAMP::InsertToPlaylistCache(const std::string url, const GrowableBuffer* buffer, const char* effectiveUrl)
{
	GrowableBuffer* buf = NULL;
	char* eUrl;
	std::unordered_map<std::string, std::pair<GrowableBuffer*, char*>>::iterator  it = mPlaylistCache.find(url);
	if (it != mPlaylistCache.end())
	{
		std::pair<GrowableBuffer*,char*> p = it->second;
		buf = p.first;
		eUrl = p.second;
		buf->len = 0;
		aamp_AppendBytes(buf, buffer->ptr, buffer->len );
		strncpy(eUrl, effectiveUrl, MAX_URI_LENGTH);
		logprintf("PrivateInstanceAAMP::%s:%d : replaced available entry\n", __FUNCTION__, __LINE__);
	}
	else
	{
		buf = new GrowableBuffer();
		memset (buf, 0, sizeof(GrowableBuffer));
		aamp_AppendBytes(buf, buffer->ptr, buffer->len );
		eUrl = (char*) malloc(MAX_URI_LENGTH);
		strncpy(eUrl, effectiveUrl, MAX_URI_LENGTH);
		mPlaylistCache[url] = std::pair<GrowableBuffer*, char*>(buf, eUrl);
		traceprintf("PrivateInstanceAAMP::%s:%d : Inserted. url %s\n", __FUNCTION__, __LINE__, url.c_str());
	}
}


/**
 * @brief Retrieve playlist from playlist cache
 * @param url URL corresponding to playlist
 * @param[out] buffer Output buffer containing playlist
 * @param[out] effectiveUrl effective URL of retrieved playlist
 * @retval true if playlist is successfully retrieved.
 */
bool PrivateInstanceAAMP::RetrieveFromPlaylistCache(const std::string url, GrowableBuffer* buffer, char effectiveUrl[])
{
	GrowableBuffer* buf = NULL;
	bool ret;
	char* eUrl;
	std::unordered_map<std::string, std::pair<GrowableBuffer*, char*>>::iterator  it = mPlaylistCache.find(url);
	if (it != mPlaylistCache.end())
	{
		std::pair<GrowableBuffer*, char*> p = it->second;
		buf = p.first;
		eUrl = p.second;
		buffer->len = 0;
		aamp_AppendBytes(buffer, buf->ptr, buf->len );
		strncpy(effectiveUrl,eUrl, MAX_URI_LENGTH);
		traceprintf("PrivateInstanceAAMP::%s:%d : url %s found\n", __FUNCTION__, __LINE__, url.c_str());
		ret = true;
	}
	else
	{
		traceprintf("PrivateInstanceAAMP::%s:%d : url %s not found\n", __FUNCTION__, __LINE__, url.c_str());
		ret = false;
	}
	return ret;
}


/**
 * @brief Clear playlist cache
 */
void PrivateInstanceAAMP::ClearPlaylistCache()
{
	GrowableBuffer* buf = NULL;
	char* eUrl;
	if(mPlaylistCache.size() > 2)
	{
		logprintf("PrivateInstanceAAMP::%s:%d : cache size %d\n", __FUNCTION__, __LINE__, (int)mPlaylistCache.size());
	}
	for (std::unordered_map<std::string, std::pair<GrowableBuffer*, char*>>::iterator  it = mPlaylistCache.begin();
						it != mPlaylistCache.end(); it++)
	{
		std::pair<GrowableBuffer*,char*> p = it->second;
		buf = p.first;
		eUrl = p.second;
		aamp_Free(&buf->ptr);
		free(eUrl);
		delete buf;
	}
	mPlaylistCache.clear();
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

void PrivateInstanceAAMP::SetReportInterval(int reportIntervalMS)
{
	gpGlobalConfig->reportProgressInterval = reportIntervalMS;
}

/**
 * @brief Send stalled event to listeners
 */
void PrivateInstanceAAMP::SendStalledErrorEvent()
{
	char description[MAX_ERROR_DESCRIPTION_LENGTH];
	memset(description, '\0', MAX_ERROR_DESCRIPTION_LENGTH);
	snprintf(description, MAX_ERROR_DESCRIPTION_LENGTH - 1, "Playback has been stalled for more than %d ms", gpGlobalConfig->stallTimeoutInMS);
	SendErrorEvent(AAMP_TUNE_PLAYBACK_STALLED, description);
}

/**
 * @brief Notifiy first buffer is processed
 */
void PrivateInstanceAAMP::NotifyFirstBufferProcessed()
{
	trickStartUTCMS = aamp_GetCurrentTimeMS();
}


/**
 * @brief Update audio language selection
 * @param lang string corresponding to language
 */
void PrivateInstanceAAMP::UpdateAudioLanguageSelection(const char *lang)
{
	strncpy(language, lang, MAX_LANGUAGE_TAG_LENGTH);
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

	int type = 10; //HLS

	if(mIsDash){
		type = 20;
	}

	switch(mCurrentDrm)
	{
		case eDRM_WideVine:
		case eDRM_CONSEC_agnostic:
			type += 1;	// 11 or 21
			break;
		case eDRM_PlayReady:
		case eDRM_Adobe_Access:
			type += 2;	// 12 or 22
			break;
		case eDRM_Vanilla_AES:
			type += 3;	// 13
			break;
		default:
			break; //Clear
	}
	return type;
}

#ifdef AAMP_MPD_DRM
/**
 * @brief GetMoneyTraceString - Extracts / Generates MoneyTrace string 
 * @param[out] customHeader - Generated moneytrace is stored  
 *
 * @retval None
*/
void PrivateInstanceAAMP::GetMoneyTraceString(std::string &customHeader)
{
	char moneytracebuf[512];
	memset(moneytracebuf, 0, sizeof(moneytracebuf));

	if (mCustomHeaders.size() > 0)
	{
		for (std::unordered_map<std::string, std::vector<std::string>>::iterator it = mCustomHeaders.begin();
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
					snprintf(moneytracebuf, sizeof(moneytracebuf), "trace-id=%s;parent-id=%u;span-id=%lld",
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
		logprintf("No Moneytrace info available in tune request,need to generate one\n");
		uuid_t uuid;
		uuid_generate(uuid);
		char uuidstr[128];
		uuid_unparse(uuid, uuidstr);
		for (char *ptr = uuidstr; *ptr; ++ptr) {
			*ptr = tolower(*ptr);
		}
		snprintf(moneytracebuf,sizeof(moneytracebuf),"trace-id=%s;parent-id=%u;span-id=%lld",uuidstr,aamp_GetCurrentTimeMS(),aamp_GetCurrentTimeMS());
		customHeader.append(moneytracebuf);
	}	
	AAMPLOG_TRACE("[GetMoneyTraceString] MoneyTrace[%s]\n",customHeader.c_str());
}
#endif /* AAMP_MPD_DRM */

/**
 * @brief Send tuned event if configured to sent after decryption
 */
void PrivateInstanceAAMP::NotifyFirstFragmentDecrypted()
{
	if(mTunedEventPending)
	{
		TunedEventConfig tunedEventConfig =  IsLive() ? gpGlobalConfig->tunedEventConfigLive : gpGlobalConfig->tunedEventConfigVOD;
		if (eTUNED_EVENT_ON_FIRST_FRAGMENT_DECRYPTED == tunedEventConfig)
		{
			if (SendTunedEvent())
			{
				logprintf("aamp: %s - sent tune event after first fragment fetch and decrypt\n", mIsDash ? "mpd" : "hls");
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
 *   @brief  Check if asset is vod/ivod/cdvr.
 *
 *   @return true if asset is either vod/ivod/cdvr
 */
bool PrivateInstanceAAMP::IsVodOrCdvrAsset()
{
	return (mContentType == ContentType_IVOD || mContentType == ContentType_VOD || mContentType == ContentType_CDVR || mContentType == ContentType_IPDVR);
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
	AAMPEvent event;
	std::vector<int> supportedPlaybackSpeeds { -64, -32, -16, -4, -1, 0, 1, 4, 16, 32, 64 };
	int langCount = 0;
	int bitrateCount = 0;
	int supportedSpeedCount = 0;

	event.type = AAMP_EVENT_MEDIA_METADATA;
	event.data.metadata.durationMiliseconds = durationMs;
	memset(event.data.metadata.bitrates, 0, sizeof(event.data.metadata.bitrates));
	memset(event.data.metadata.supportedSpeeds, 0, sizeof(event.data.metadata.supportedSpeeds));

	for (std::set<std::string>::iterator iter = langList.begin();
			(iter != langList.end() && langCount < MAX_LANGUAGE_COUNT) ; iter++)
	{
		std::string langEntry = *iter;
		if (!langEntry.empty())
		{
			strncpy(event.data.metadata.languages[langCount], langEntry.c_str(), MAX_LANGUAGE_TAG_LENGTH);
			event.data.metadata.languages[langCount][MAX_LANGUAGE_TAG_LENGTH-1] = 0;
			langCount++;
		}
	}
	event.data.metadata.languageCount = langCount;
	StoreLanguageList(langCount, event.data.metadata.languages);

	for (int i = 0; (i < bitrateList.size() && bitrateCount < MAX_BITRATE_COUNT); i++)
	{
		event.data.metadata.bitrates[bitrateCount++] = bitrateList[i];
	}
	event.data.metadata.bitrateCount = bitrateCount;
	event.data.metadata.width = 1280;
	event.data.metadata.height = 720;
	GetPlayerVideoSize(event.data.metadata.width, event.data.metadata.height);
	event.data.metadata.hasDrm = hasDrm;

	//Iframe track present and hence playbackRate change is supported
	if (isIframeTrackPresent)
	{
		for(int i = 0; i < supportedPlaybackSpeeds.size() && supportedSpeedCount < MAX_SUPPORTED_SPEED_COUNT; i++)
		{
			event.data.metadata.supportedSpeeds[supportedSpeedCount++] = supportedPlaybackSpeeds[i];
		}
	}
	else
	{
		//Supports only pause and play
		event.data.metadata.supportedSpeeds[supportedSpeedCount++] = 0;
		event.data.metadata.supportedSpeeds[supportedSpeedCount++] = 1;
	}
	event.data.metadata.supportedSpeedCount = supportedSpeedCount;

	logprintf("aamp: sending metadata event and duration update %f\n", ((double)durationMs)/1000);
	SendEventAsync(event);
}

/**
 *   @brief  Generate supported speeds changed event based on arg passed.
 *
 *   @param[in] isIframeTrackPresent - indicates if iframe tracks are available in asset
 */
void PrivateInstanceAAMP::SendSupportedSpeedsChangedEvent(bool isIframeTrackPresent)
{
	AAMPEvent event;
	std::vector<int> supportedPlaybackSpeeds { -64, -32, -16, -4, -1, 0, 1, 4, 16, 32, 64 };
	int supportedSpeedCount = 0;

	event.type = AAMP_EVENT_SPEEDS_CHANGED;
	//Iframe track present and hence playbackRate change is supported
	if (isIframeTrackPresent)
	{
		for(int i = 0; i < supportedPlaybackSpeeds.size() && supportedSpeedCount < MAX_SUPPORTED_SPEED_COUNT; i++)
		{
			event.data.speedsChanged.supportedSpeeds[supportedSpeedCount++] = supportedPlaybackSpeeds[i];
		}
	}
	else
	{
		//Supports only pause and play
		event.data.speedsChanged.supportedSpeeds[supportedSpeedCount++] = 0;
		event.data.speedsChanged.supportedSpeeds[supportedSpeedCount++] = 1;
	}
	event.data.speedsChanged.supportedSpeedCount = supportedSpeedCount;

	logprintf("aamp: sending supported speeds changed event with count %d\n", supportedSpeedCount);
	SendEventAsync(event);
}


/**
 *   @brief  Get Sequence Number from URL
 *
 *   @param[in] fragmentUrl fragment Url
 *   @returns Sequence Number if found in fragment Url else 0
 */
long long PrivateInstanceAAMP::GetSeqenceNumberfromURL(const char *fragmentUrl)
{

	long long seqNumber = 0;
	const char *pos = strstr(fragmentUrl, "-frag-");
	const char *ptr;
	if (pos)
	{
		seqNumber = atoll(pos + 6);
	}
	else if (ptr = strstr(fragmentUrl, ".seg"))
	{
		if( (ptr-fragmentUrl >= 5) && (memcmp(ptr - 5, "-init", 5) == 0))
		{
			seqNumber = -1;
		}
		else
		{
			while (ptr > fragmentUrl)
			{
				if (*ptr == '/')
				{
					break;
				}
				ptr--;
			}
			seqNumber = atoll(ptr + 1);
		}
	}
	else if ((strstr(fragmentUrl, ".mpd")) || (strstr(fragmentUrl, ".m3u8")))
	{
		seqNumber = -1;
	}
	return seqNumber;
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
void PrivateInstanceAAMP::SetNetworkTimeout(int timeout)
{
	if (timeout > 0)
	{
		gpGlobalConfig->fragmentDLTimeout = timeout;
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
 *   @brief Set Preferred DRM.
 *
 *   @param[in] drmType - Preferred DRM type
 */
void PrivateInstanceAAMP::SetPreferredDRM(DRMSystems drmType)
{
	AAMPLOG_INFO("%s:%d set preferred drm: %d\n", __FUNCTION__, __LINE__, drmType);
	gpGlobalConfig->preferredDrm = drmType;
}
