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
 * @file aampcli.cpp
 * @brief Stand alone AAMP player with command line interface.
 */

#include <stdio.h>
#include <errno.h>
#include <list>
#include <string.h>
#include <string>
#include <gst/gst.h>
#include <priv_aamp.h>
#include <main_aamp.h>
#include "../StreamAbstractionAAMP.h"

#ifdef IARM_MGR
#include "host.hpp"
#include "manager.hpp"
#include "libIBus.h"
#include "libIBusDaemon.h"
#endif

#ifdef __APPLE__
#import <cocoa_window.h>
#endif
#define MAX_BUFFER_LENGTH 4096

#define CC_OPTION_1 "{\"penItalicized\":false,\"textEdgeStyle\":\"none\",\"textEdgeColor\":\"black\",\"penSize\":\"small\",\"windowFillColor\":\"black\",\"fontStyle\":\"default\",\"textForegroundColor\":\"white\",\"windowFillOpacity\":\"transparent\",\"textForegroundOpacity\":\"solid\",\"textBackgroundColor\":\"black\",\"textBackgroundOpacity\":\"solid\",\"windowBorderEdgeStyle\":\"none\",\"windowBorderEdgeColor\":\"black\",\"penUnderline\":false}"
#define CC_OPTION_2 "{\"penItalicized\":false,\"textEdgeStyle\":\"none\",\"textEdgeColor\":\"yellow\",\"penSize\":\"small\",\"windowFillColor\":\"black\",\"fontStyle\":\"default\",\"textForegroundColor\":\"yellow\",\"windowFillOpacity\":\"transparent\",\"textForegroundOpacity\":\"solid\",\"textBackgroundColor\":\"cyan\",\"textBackgroundOpacity\":\"solid\",\"windowBorderEdgeStyle\":\"none\",\"windowBorderEdgeColor\":\"black\",\"penUnderline\":true}"
#define CC_OPTION_3 "{\"penItalicized\":false,\"textEdgeStyle\":\"none\",\"textEdgeColor\":\"black\",\"penSize\":\"large\",\"windowFillColor\":\"blue\",\"fontStyle\":\"default\",\"textForegroundColor\":\"red\",\"windowFillOpacity\":\"transparent\",\"textForegroundOpacity\":\"solid\",\"textBackgroundColor\":\"white\",\"textBackgroundOpacity\":\"solid\",\"windowBorderEdgeStyle\":\"none\",\"windowBorderEdgeColor\":\"black\",\"penUnderline\":false}"
static PlayerInstanceAAMP *mSingleton = NULL;
static PlayerInstanceAAMP *mBackgroundPlayer = NULL;
static GMainLoop *AAMPGstPlayerMainLoop = NULL;

/**
 * @struct VirtualChannelInfo
 * @brief Holds information of a virtual channel
 */
struct VirtualChannelInfo
{
	VirtualChannelInfo() : channelNumber(0), name(), uri()
	{
	}
	int channelNumber;
	std::string name;
	std::string uri;
};

/**
 * @enum AAMPGetTypes
 * @brief Define the enum values of get types
 */
typedef enum {
	eAAMP_GET_CurrentAudioLan = 1,
	eAAMP_GET_CurrentDrm,
	eAAMP_GET_PlaybackPosition,
	eAAMP_GET_PlaybackDuration,
	eAAMP_GET_VideoBitrate,
	eAAMP_GET_AudioBitrate,
	eAAMP_GET_AudioVolume,
	eAAMP_GET_PlaybackRate,
	eAAMP_GET_VideoBitrates,
	eAAMP_GET_AudioBitrates,
	eAAMP_GET_CurrentPreferredLanguages,
	eAAMP_GET_AvailableAudioTracks,
	eAAMP_GET_AvailableTextTracks,
	eAAMP_GET_AudioTrack,
	eAAMP_GET_TextTrack,
	eAAMP_GET_ThumbnailConfig,
	eAAMP_GET_ThumbnailData
}AAMPGetTypes;

/**
 * @enum AAMPSetTypes
 * @brief Define the enum values of set types
 */
typedef enum{
	eAAMP_SET_RateAndSeek = 1,
	eAAMP_SET_VideoRectangle,
	eAAMP_SET_VideoZoom,
	eAAMP_SET_VideoMute,
	eAAMP_SET_AudioVolume,
	eAAMP_SET_Language,
	eAAMP_SET_SubscribedTags,
	eAAMP_SET_LicenseServerUrl,
	eAAMP_SET_AnonymouseRequest,
	eAAMP_SET_VodTrickplayFps,
	eAAMP_SET_LinearTrickplayFps,
	eAAMP_SET_LiveOffset,
	eAAMP_SET_StallErrorCode,
	eAAMP_SET_StallTimeout,
	eAAMP_SET_ReportInterval,
	eAAMP_SET_VideoBitarate,
	eAAMP_SET_InitialBitrate,
	eAAMP_SET_InitialBitrate4k,
	eAAMP_SET_NetworkTimeout,
	eAAMP_SET_ManifestTimeout,
	eAAMP_SET_DownloadBufferSize,
	eAAMP_SET_PreferredDrm,
	eAAMP_SET_StereoOnlyPlayback,
	eAAMP_SET_AlternateContent,
	eAAMP_SET_NetworkProxy,
	eAAMP_SET_LicenseReqProxy,
	eAAMP_SET_DownloadStallTimeout,
	eAAMP_SET_DownloadStartTimeout,
	eAAMP_SET_PreferredSubtitleLang,
	eAAMP_SET_ParallelPlaylistDL,
	eAAMP_SET_PreferredLanguages,
	eAAMP_SET_RampDownLimit,
	eAAMP_SET_InitRampdownLimit,
	eAAMP_SET_MinimumBitrate,
	eAAMP_SET_MaximumBitrate,
	eAAMP_SET_MaximumSegmentInjFailCount,
	eAAMP_SET_MaximumDrmDecryptFailCount,
	eAAMP_SET_RegisterForID3MetadataEvents,
	eAAMP_SET_InitialBufferDuration,
	eAAMP_SET_AudioTrack,
	eAAMP_SET_TextTrack,
	eAAMP_SET_CCStatus,
	eAAMP_SET_CCStyle,
	eAAMP_SET_LanguageFormat,
	eAAMP_SET_PropagateUriParam,
	eAAMP_SET_ThumbnailTrack,
	eAAMP_SET_PausedBehavior
}AAMPSetTypes;

static std::list<VirtualChannelInfo> mVirtualChannelMap;

/**
 * @brief Thread to run mainloop (for standalone mode)
 * @param[in] arg user_data
 * @retval void pointer
 */
static void* AAMPGstPlayer_StreamThread(void *arg);
static bool initialized = false;
GThread *aampMainLoopThread = NULL;


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
 * @brief check if the char array is having numbers only
 * @param s
 * @retval true or false
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



/**
 * @brief Thread to run mainloop (for standalone mode)
 * @param[in] arg user_data
 * @retval void pointer
 */
static void* AAMPGstPlayer_StreamThread(void *arg)
{
	if (AAMPGstPlayerMainLoop)
	{
		g_main_loop_run(AAMPGstPlayerMainLoop); // blocks
		logprintf("AAMPGstPlayer_StreamThread: exited main event loop");
	}
	g_main_loop_unref(AAMPGstPlayerMainLoop);
	AAMPGstPlayerMainLoop = NULL;
	return NULL;
}

/**
 * @brief To initialize Gstreamer and start mainloop (for standalone mode)
 * @param[in] argc number of arguments
 * @param[in] argv array of arguments
 */
void InitPlayerLoop(int argc, char **argv)
{
	if (!initialized)
	{
		initialized = true;
		gst_init(&argc, &argv);
		AAMPGstPlayerMainLoop = g_main_loop_new(NULL, FALSE);
		aampMainLoopThread = g_thread_new("AAMPGstPlayerLoop", &AAMPGstPlayer_StreamThread, NULL );
	}
}

/**
 * @brief Stop mainloop execution (for standalone mode)
 */
void TermPlayerLoop()
{
	if(AAMPGstPlayerMainLoop)
	{
		g_main_loop_quit(AAMPGstPlayerMainLoop);
		g_thread_join(aampMainLoopThread);
		gst_deinit ();
		logprintf("%s(): Exit", __FUNCTION__);
	}
}

/**
 * @brief Show help menu with aamp command line interface
 */
static void ShowHelp(void)
{
	int i = 0;
	if (!mVirtualChannelMap.empty())
	{
		logprintf("aampcli.cfg virtual channel map:");

		for (std::list<VirtualChannelInfo>::iterator it = mVirtualChannelMap.begin(); it != mVirtualChannelMap.end(); ++it, ++i)
		{
			VirtualChannelInfo &pChannelInfo = *it;
			const char *channelName = pChannelInfo.name.c_str();
			printf("%4d: %s", pChannelInfo.channelNumber, channelName );
			if ((i % 4) == 3 )
			{ // four virtual channels per row
				printf("\n");
			}
			else
			{
				size_t len = strlen(channelName);
				while( len<20 )
				{ // pad each column to 20 characters, for clean layout
					printf( " " );
					len++;
				}
			}
		}
		printf("\n\n");
	}
	logprintf( "Commands:" );
	logprintf( "<channelNumber>               // Play selected channel from guide");
	logprintf( "<url>                         // Play arbitrary stream");
	logprintf( "pause play stop status flush  // Playback options");
	logprintf( "sf, ff<x> rw<y>               // Trickmodes (x <= 128. y <= 128)");
	logprintf( "cache <url>/<channel>         // Cache a channel in the background");
	logprintf( "toggle                        // Toggle the background channel & foreground channel");
	logprintf( "stopb                         // Stop background channel.");
	logprintf( "+ -                           // Change profile");
	logprintf( "bps <x>                       // set bitrate ");
	logprintf( "sap                           // Use SAP track (if avail)");
	logprintf( "seek <seconds>                // Specify start time within manifest");
	logprintf( "live                          // Seek to live point");
	logprintf( "underflow                     // Simulate underflow" );
	logprintf( "retune                        // schedule retune" );
	logprintf( "reset                         // delete player instance and create a new one" );
	logprintf( "get help                      // Show help of get command" );
	logprintf( "set help                      // Show help of set command" );
	logprintf( "exit                          // Exit from application" );
	logprintf( "help                          // Show this list again" );
}

/**
 * @brief Display Help menu for set
 * @param none
 */
void ShowHelpGet(){
	printf("******************************************************************************************\n");
	printf("*   get <command> [<arguments>]\n");
	printf("*   Usage of Commands, and arguments expected\n");
	printf("******************************************************************************************\n");
	printf("get 1                 // Get Current audio language\n");
	printf("get 2                 // Get Current DRM\n");
	printf("get 3                 // Get Current Playback position\n");
	printf("get 4                 // Get Playback Duration\n");
	printf("get 5                 // Get current video bitrate\n");
	printf("get 6                 // Get current Audio bitrate\n");
	printf("get 7                 // Get current Audio volume\n");
	printf("get 8                 // Get Current Playback rate\n");
	printf("get 9                 // Get Video bitrates supported\n");
	printf("get 10                // Get Audio bitrates supported\n");
	printf("get 11                // Get Current preferred languages\n");
	printf("get 12                // Get Available Audio Tracks\n");
	printf("get 13                // Get Available Text Tracks\n");
	printf("get 14                // Get Audio Track\n");
	printf("get 15                // Get Text Track\n");
	printf("get 16                // Get Available ThumbnailTracks\n");
	printf("get 17                // Get Thumbnail timerange data(int startpos, int endpos)\n");
	printf("****************************************************************************\n");
}

/**
 * @brief Display Help menu for set
 * @param none
 */
void ShowHelpSet(){
	printf("******************************************************************************************\n");
	printf("*   set <command> [<arguments>] \n");
	printf("*   Usage of Commands with arguemnts expected in ()\n");
	printf("******************************************************************************************\n");
	printf("set 1 <x> <y>         // Set Rate and Seek (int x=rate, double y=seconds)\n");
	printf("set 2 <x> <y> <w> <h> // Set Video Rectangle (int x,y,w,h)\n");
	printf("set 3 <x>             // Set Video zoom  ( x = 1 fo full, x = 0 for normal)\n");
	printf("set 4 <x>             // Set Video Mute ( x = 1  - Mute , x = 0 - Unmute)\n");
	printf("set 5 <x>             // Set Audio Volume (int x=volume)\n");
	printf("set 6 <x>             // Set Language (string x=lang)\n");
	printf("set 7                 // Set Subscribed Tag - dummy\n");
	printf("set 8 <x>             // Set License Server URL (String x=url)\n");
	printf("set 9 <x>             // Set Anonymous Request  (int x=0/1)\n");
	printf("set 10 <x>            // Set VOD Trickplay FPS (int x=trickPlayFPS)\n");
	printf("set 11 <x>            // Set Linear Trickplay FPS (int x=trickPlayFPS)\n");
	printf("set 12 <x>            // Set Live offset (int x=offset)\n");
	printf("set 13 <x>            // Set Stall error code (int x=errorCode)\n");
	printf("set 14 <x>            // Set Stall timeout (int x=timeout)\n");
	printf("set 15 <x>            // Set Report Interval (int x=interval)\n");
	printf("set 16 <x>            // Set Video Bitrate (long x=bitrate)\n");
	printf("set 17 <x>            // Set Initial Bitrate (long x = bitrate)\n");
	printf("set 18 <x>            // Set Initial Bitrate 4K (long x = bitrate4k)\n");
	printf("set 19 <x>            // Set Network Timeout (long x = timeout in ms)\n");
	printf("set 20 <x>            // Set Manifest Timeout (long x = timeout in ms)\n");
	printf("set 21 <x>            // Set Download Buffer Size (int x = bufferSize)\n");
	printf("set 22 <x>            // Set Preferred DRM (int x=1-WV, 2-PR, 4-Access, 5-AES 6-ClearKey)\n");
	printf("set 23 <x>            // Set Stereo only playback (x=1/0)\n");
	printf("set 24                // Set Alternate Contents - dummy ()\n");
	printf("set 25 <x>            // Set Set Network Proxy (string x = url)\n");
	printf("set 26 <x>            // Set License Request Proxy (string x=url)\n");
	printf("set 27 <x>            // Set Download Stall timeout (long x=timeout)\n");
	printf("set 28 <x>            // Set Download Start timeout (long x=timeout)\n");
	printf("set 29 <x>            // Set Preferred Subtitle language (string x = lang)\n");
	printf("set 30 <x>            // Set Parallel Playlist download (x=0/1)\n");
	printf("set 31 <x>            // Set Preferred languages (string \"lang1, lang2, ...\")\n");
	printf("set 32 <x>            // Set number of Ramp Down limit during the playback (x = number)\n");
	printf("set 33 <x>            // Set number of Initial Ramp Down limit prior to the playback (x = number)\n");
	printf("set 34 <x>            // Set Minimum bitrate (x = bitrate)\n");
	printf("set 35 <x>            // Set Maximum bitrate (x = bitrate)\n");
	printf("set 36 <x>            // Set Maximum segment injection fail count (int x = count)\n");
	printf("set 37 <x>            // Set Maximum DRM Decryption fail count(int x = count)\n");
	printf("set 38 <x>            // Set Listen for ID3_METADATA events (x = 1 - add listener, x = 0 - remove)\n");
	printf("set 39 <y> <y>        // Set Language Format (x = preferredFormat(0-3), y = useRole(0/1))\n");
	printf("set 40 <x>            // Set Initial Buffer Duration (int x = Duration in sec)\n");
	printf("set 41 <x>            // Set Audio track (int x = track index)\n");
	printf("set 42 <x>            // Set Text track (int x = track index)\n");
	printf("set 43 <x>            // Set CC status (x = 0/1)\n");
	printf("set 44 <x>            // Set a predefined CC style option (x = 1/2/3)\n");
	printf("set 45 <x>            // Set propagate uri parameters: (int x = 0 to disable)\n");
	printf("set 46 <x>            // Set Thumbnail Track (int x = Thumbnail Index)\n");
	printf("set 47 <x>            // Set Paused behavior (int x (0-3) options -\"autoplay defer\",\"autoplay immediate\",\"live defer\",\"live immediate\"\n");
	printf("******************************************************************************************\n");
}

#define LOG_CLI_EVENTS
#ifdef LOG_CLI_EVENTS
static class PlayerInstanceAAMP *mpPlayerInstanceAAMP;

/**
 * @class myAAMPEventListener
 * @brief
 */
class myAAMPEventListener :public AAMPEventObjectListener
{
public:

	/**
	 * @brief Implementation of event callback
	 * @param e Event
	 */
	void Event(const AAMPEventPtr& e)
	{
		switch (e->getType())
		{
		case AAMP_EVENT_STATE_CHANGED:
			{
				StateChangedEventPtr ev = std::dynamic_pointer_cast<StateChangedEvent>(e);
				logprintf("AAMP_EVENT_STATE_CHANGED: %d", ev->getState());
				break;
			}
		case AAMP_EVENT_SEEKED:
			{
				SeekedEventPtr ev = std::dynamic_pointer_cast<SeekedEvent>(e);
				logprintf("AAMP_EVENT_SEEKED: new positionMs %f", ev->getPosition());
				break;
			}
		case AAMP_EVENT_MEDIA_METADATA:
			{
				MediaMetadataEventPtr ev = std::dynamic_pointer_cast<MediaMetadataEvent>(e);
				std::vector<std::string> languages = ev->getLanguages();
				int langCount = ev->getLanguagesCount();
				logprintf("AAMP_EVENT_MEDIA_METADATA\n" );
				for (int i = 0; i < langCount; i++)
				{
					logprintf("language: %s\n", languages[i].c_str());
				}
				break;
			}
		case AAMP_EVENT_TUNED:
			logprintf("AAMP_EVENT_TUNED");
			break;
		case AAMP_EVENT_TUNE_FAILED:
			logprintf("AAMP_EVENT_TUNE_FAILED");
			break;
		case AAMP_EVENT_SPEED_CHANGED:
			logprintf("AAMP_EVENT_SPEED_CHANGED");
			break;
		case AAMP_EVENT_DRM_METADATA:
                        logprintf("AAMP_DRM_FAILED");
                        break;
		case AAMP_EVENT_EOS:
			logprintf("AAMP_EVENT_EOS");
			break;
		case AAMP_EVENT_PLAYLIST_INDEXED:
			logprintf("AAMP_EVENT_PLAYLIST_INDEXED");
			break;
		case AAMP_EVENT_PROGRESS:
			//logprintf("AAMP_EVENT_PROGRESS");
			break;
		case AAMP_EVENT_CC_HANDLE_RECEIVED:
			logprintf("AAMP_EVENT_CC_HANDLE_RECEIVED");
			break;
		case AAMP_EVENT_BITRATE_CHANGED:
			logprintf("AAMP_EVENT_BITRATE_CHANGED");
			break;
		case AAMP_EVENT_AUDIO_TRACKS_CHANGED:
			logprintf("AAMP_EVENT_AUDIO_TRACKS_CHANGED");
			break;
		case AAMP_EVENT_ID3_METADATA:
			{
				logprintf("AAMP_EVENT_ID3_METADATA");
				ID3MetadataEventPtr ev = std::dynamic_pointer_cast<ID3MetadataEvent>(e);
				std::vector<uint8_t> metadata = ev->getMetadata();
				int len = ev->getMetadataSize();
				logprintf("ID3 payload, length %d bytes:", len);
				printf("\t");
				for (int i = 0; i < len; i++)
				{
					printf("%c", metadata[i]);
				}
				printf("\n");
				break;
			}
		default:
			break;
		}
	}
}; // myAAMPEventListener

static class myAAMPEventListener *myEventListener;
#endif // LOG_CLI_EVENTS

/**
 * @brief Parse config entries for aamp-cli, and update gpGlobalConfig params
 *        based on the config.
 * @param cfg config to process
 */
static void ProcessCLIConfEntry(char *cfg)
{
	trim(&cfg);
	if (cfg[0] == '*')
	{
			char *delim = strchr(cfg, ' ');
			if (delim)
			{
				//Populate channel map from aampcli.cfg
				VirtualChannelInfo channelInfo;
				channelInfo.channelNumber = INT_MIN;
				char *channelStr = &cfg[1];
				char *token = strtok(channelStr, " ");
				while (token != NULL)
				{
					if (isNumber(token))
						channelInfo.channelNumber = atoi(token);
					else if (
							 memcmp(token, "udp:", 4)==0 ||
							 memcmp(token, "http:", 5) == 0 ||
							 memcmp(token, "https:", 6) == 0 ||
							 memcmp(token, "hdmiin:", 7) == 0 ||
							 memcmp(token, "live:", 5) == 0)
					{
						channelInfo.uri = token;
					}
					else
						channelInfo.name = token;
					token = strtok(NULL, " ");
				}
				if (!channelInfo.uri.empty())
				{
					if (INT_MIN == channelInfo.channelNumber)
					{
						channelInfo.channelNumber = mVirtualChannelMap.size() + 1;
					}
					if (channelInfo.name.empty())
					{
						channelInfo.name = "CH" + std::to_string(channelInfo.channelNumber);
					}
					bool duplicate = false;
					for (std::list<VirtualChannelInfo>::iterator it = mVirtualChannelMap.begin(); it != mVirtualChannelMap.end(); ++it)
					{
						VirtualChannelInfo &existingChannelInfo = *it;
						if(channelInfo.channelNumber == existingChannelInfo.channelNumber )
						{
							duplicate = true;
							break;
						}
					}
					if( duplicate )
					{
						//printf( "duplicate entry for channel %d\n", channelInfo.channelNumber );
					}
					else
					{
						mVirtualChannelMap.push_back(channelInfo);
					}
				}
				else
				{
					logprintf("%s(): Could not parse uri of %s", __FUNCTION__, cfg);
				}
			}
	}
}

inline void StopCachedChannel()
{
	if(mBackgroundPlayer)
	{
		mBackgroundPlayer->Stop();
		delete mBackgroundPlayer;
		mBackgroundPlayer = NULL;
	}
}

void CacheChannel(const char *url)
{
	StopCachedChannel();
	mBackgroundPlayer = new PlayerInstanceAAMP(
#ifdef RENDER_FRAMES_IN_APP_CONTEXT
			NULL
			,updateYUVFrame
#endif
			);
#ifdef LOG_CLI_EVENTS
	mBackgroundPlayer->RegisterEvents(myEventListener);
#endif
	mBackgroundPlayer->Tune(url, false);
}

/**
 * @brief Process command
 * @param cmd command
 */
static void ProcessCliCommand(char *cmd)
{
	double seconds = 0;
	int rate = 0;
	char lang[MAX_LANGUAGE_TAG_LENGTH];
	char cacheUrl[200];
	int keepPaused = 0;
	trim(&cmd);
	if (cmd[0] == 0)
	{
		if (mSingleton->aamp->mpStreamAbstractionAAMP)
		{
			mSingleton->aamp->mpStreamAbstractionAAMP->DumpProfiles();
		}
		logprintf("current bitrate ~= %ld", mSingleton->aamp->GetCurrentlyAvailableBandwidth());
	}
	else if (strcmp(cmd, "help") == 0)
	{
		ShowHelp();
	}
	else if (memcmp(cmd, "http", 4) == 0)
	{
		mSingleton->Tune(cmd);
	}
	else if (memcmp(cmd, "live", 4) == 0)
	{
		mSingleton->Tune(cmd);
	}
	else if (memcmp(cmd, "hdmiin", 6) == 0)
	{
		mSingleton->Tune(cmd);
	}
	else if (isNumber(cmd))
	{
		int channelNumber = atoi(cmd);
		logprintf("channel number: %d", channelNumber);
		for (std::list<VirtualChannelInfo>::iterator it = mVirtualChannelMap.begin(); it != mVirtualChannelMap.end(); ++it)
		{
			VirtualChannelInfo &channelInfo = *it;
			if(channelInfo.channelNumber == channelNumber)
			{
			//	logprintf("Found %d tuning to %s",channelInfo.channelNumber, channelInfo.uri.c_str());
				mSingleton->Tune(channelInfo.uri.c_str());
				break;
			}
		}
	}
	else if (sscanf(cmd, "cache %s", cacheUrl) == 1)
	{
		if (memcmp(cacheUrl, "http", 4) ==0)
		{
			CacheChannel(cacheUrl);
		}
		else
		{
			int channelNumber = atoi(cacheUrl);
			logprintf("channel number: %d", channelNumber);
			for (std::list<VirtualChannelInfo>::iterator it = mVirtualChannelMap.begin(); it != mVirtualChannelMap.end(); ++it)
			{
				VirtualChannelInfo &channelInfo = *it;
				if(channelInfo.channelNumber == channelNumber)
				{
					CacheChannel(channelInfo.uri.c_str());
					break;
				}
			}
		}
	}
	else if (strcmp(cmd, "toggle") == 0)
	{
		if(mBackgroundPlayer && mSingleton)
		{
			mSingleton->detach();
			mBackgroundPlayer->SetRate(AAMP_NORMAL_PLAY_RATE);

			PlayerInstanceAAMP *tmpPlayer = mSingleton;
			mSingleton = mBackgroundPlayer;
			mBackgroundPlayer = tmpPlayer;
			StopCachedChannel();
		}
	}
	else if (strcmp(cmd, "stopb") == 0)
	{
		StopCachedChannel();
	}
	else if (sscanf(cmd, "seek %lf %d", &seconds, &keepPaused) >= 1)
	{
		mSingleton->Seek(seconds, (keepPaused==1) );
		keepPaused = 0;
	}
	else if (strcmp(cmd, "sf") == 0)
	{
		mSingleton->SetRate((int)0.5);
	}
	else if (sscanf(cmd, "ff%d", &rate) == 1)
	{
		if (rate != 4 && rate != 16 && rate != 32)
		{
			logprintf("Speed not supported.");
		}
		else
		{
			mSingleton->SetRate((float)rate);
		}
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
			logprintf("Speed not supported.");
		}
		else
		{
			mSingleton->SetRate((float)(-rate));
		}
	}
	else if (sscanf(cmd, "bps %d", &rate) == 1)
	{
		logprintf("Set video bitrate %d.", rate);
		mSingleton->SetVideoBitrate(rate);
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
		mSingleton->aamp->ScheduleRetune(eGST_ERROR_UNDERFLOW, eMEDIATYPE_VIDEO);
	}
	else if (strcmp(cmd, "retune") == 0)
	{
		mSingleton->aamp->ScheduleRetune(eDASH_ERROR_STARTTIME_RESET, eMEDIATYPE_VIDEO);
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
		if (mBackgroundPlayer)
			delete mBackgroundPlayer;
		mVirtualChannelMap.clear();
		TermPlayerLoop();
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
				logprintf("Set zoom to full");
				mSingleton->SetVideoZoom(VIDEO_ZOOM_FULL);
			}
			else
			{
				logprintf("Set zoom to none");
				mSingleton->SetVideoZoom(VIDEO_ZOOM_NONE);
			}
		}
	}
	else if( sscanf(cmd, "sap %s",lang ) )
	{
		size_t len = strlen(lang);
		logprintf("aamp cli sap called for language %s\n",lang);
		if( len>0 )
		{
			mSingleton->SetLanguage( lang );
		}
		else
		{
			logprintf( "GetCurrentAudioLanguage: '%s'\n", mSingleton->GetCurrentAudioLanguage() );
		}
	}
	else if( strcmp(cmd,"getplaybackrate") == 0 )
	{
		logprintf("Playback Rate: %d\n", mSingleton->GetPlaybackRate());
	}
	else if (memcmp(cmd, "islive", 6) == 0 )
	{
		logprintf(" VIDEO IS %s ", 
		(mSingleton->IsLive() == true )? "LIVE": "NOT LIVE");
	}
	else if (memcmp(cmd, "customheader", 12) == 0 )
	{
		//Dummy implimenations
		std::vector<std::string> headerValue;
		logprintf("customheader Command is %s " , cmd); 
		mSingleton->AddCustomHTTPHeader("", headerValue, false);
	}
	else if (memcmp(cmd, "reset", 5) == 0 )
	{
		logprintf(" Reset mSingleton instance %p ", mSingleton);
		mSingleton->Stop();

		delete mSingleton;
		mSingleton = NULL;

		mSingleton = new PlayerInstanceAAMP(
#ifdef RENDER_FRAMES_IN_APP_CONTEXT
				NULL
				,updateYUVFrame
#endif
				);

#ifdef LOG_CLI_EVENTS
		myEventListener = new myAAMPEventListener();
		mSingleton->RegisterEvents(myEventListener);
#endif
		logprintf(" New mSingleton instance %p ", mSingleton);

	}
	else if (memcmp(cmd, "set", 3) == 0 )
	{
		char help[8];
		int opt;
		if (sscanf(cmd, "set %d", &opt) == 1){
			switch(opt){
				case eAAMP_SET_RateAndSeek:
                                {
					int rate;
					double ralatineTuneTime;
					logprintf("Matchde Command eAAMP_SET_RateAndSeek - %s ", cmd);
					if (sscanf(cmd, "set %d %d %lf", &opt, &rate, &ralatineTuneTime ) == 3){
						mSingleton->SetRateAndSeek(rate, ralatineTuneTime);
					}
					break;
                                }
				case eAAMP_SET_VideoRectangle:
                                {
                                        int x,y,w,h;
					logprintf("Matchde Command eAAMP_SET_VideoRectangle - %s ", cmd);
					if (sscanf(cmd, "set %d %d %d %d %d", &opt, &x, &y, &w, &h) == 5){
						mSingleton->SetVideoRectangle(x,y,w,h);
					}
					break;
                                }
				case eAAMP_SET_VideoZoom:
                                {
					int videoZoom;
					logprintf("Matchde Command eAAMP_SET_VideoZoom - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &videoZoom) == 2){
						mSingleton->SetVideoZoom((videoZoom > 0 )? VIDEO_ZOOM_FULL : VIDEO_ZOOM_NONE );
					}
					break;
                                }

				case eAAMP_SET_VideoMute:
                                {
					int videoMute;
					logprintf("Matchde Command eAAMP_SET_VideoMute - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &videoMute) == 2){
						mSingleton->SetVideoMute((videoMute == 1 )? true : false );
					}
					break;	
                                }

				case eAAMP_SET_AudioVolume:
                                {
                                        int vol;
					logprintf("Matchde Command eAAMP_SET_AudioVolume - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &vol) == 2){
						mSingleton->SetAudioVolume(vol);
					}
					break;
                                }

				case eAAMP_SET_Language:
                                {
					char lang[12];
					logprintf("Matchde Command eAAMP_SET_Language - %s ", cmd);
					if (sscanf(cmd, "set %d %s", &opt, lang) == 2){
						mSingleton->SetLanguage(lang);
					}
					break;
                                }
				case eAAMP_SET_SubscribedTags:
			        {
                                        //Dummy implimentation
					std::vector<std::string> subscribedTags;
					logprintf("Matchde Command eAAMP_SET_SubscribedTags - %s ", cmd);
					mSingleton->SetSubscribedTags(subscribedTags);
					break;
                                }
				case eAAMP_SET_LicenseServerUrl:
                                {
                                        char lisenceUrl[1024];
					int drmType;
					logprintf("Matchde Command eAAMP_SET_LicenseServerUrl - %s ", cmd);
					if (sscanf(cmd, "set %d %s %d", &opt, lisenceUrl, &drmType) == 3){
						mSingleton->SetLicenseServerURL(lisenceUrl, 
						(drmType == eDRM_PlayReady)?eDRM_PlayReady:eDRM_WideVine);
					}
					break;
                                }
				case eAAMP_SET_AnonymouseRequest:
                                {
                                        int isAnonym;
					logprintf("Matchde Command eAAMP_SET_AnonymouseRequest - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &isAnonym) == 2){
						mSingleton->SetAnonymousRequest((isAnonym == 1)?true:false);
					}
					break;
                                }
				case eAAMP_SET_VodTrickplayFps:
                                {
                                        int vodTFps;
					logprintf("Matchde Command eAAMP_SET_VodTrickplayFps - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &vodTFps) == 2){
						mSingleton->SetVODTrickplayFPS(vodTFps);
					}
					break;
                                }
				case eAAMP_SET_LinearTrickplayFps:
                                {
                                        int linearTFps;
					logprintf("Matchde Command eAAMP_SET_LinearTrickplayFps - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &linearTFps) == 2){
						mSingleton->SetLinearTrickplayFPS(linearTFps);
					}
					break;
                                }
				case eAAMP_SET_LiveOffset:
                                {
                                        int liveOffset;
					logprintf("Matchde Command eAAMP_SET_LiveOffset - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &liveOffset) == 2){
						mSingleton->SetLiveOffset(liveOffset);
					}
					break;
                                }
				case eAAMP_SET_StallErrorCode:
                                {
                                        int stallErrorCode;
					logprintf("Matchde Command eAAMP_SET_StallErrorCode - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &stallErrorCode) == 2){
						mSingleton->SetStallErrorCode(stallErrorCode);
					}
					break;
                                }
				case eAAMP_SET_StallTimeout:
                                {
                                        int stallTimeout;
					logprintf("Matchde Command eAAMP_SET_StallTimeout - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &stallTimeout) == 2){
						mSingleton->SetStallTimeout(stallTimeout);
					}
					break;
                                }

				case eAAMP_SET_ReportInterval:
                                {
                                        int reportInterval;
					logprintf("Matchde Command eAAMP_SET_ReportInterval - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &reportInterval) == 2){
						mSingleton->SetReportInterval(reportInterval);
					}
					break;
                                }
				case eAAMP_SET_VideoBitarate:
                                {
                                        long videoBitarate;
					logprintf("Matchde Command eAAMP_SET_VideoBitarate - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &videoBitarate) == 2){
						mSingleton->SetVideoBitrate(videoBitarate);
					}
					break;
                                }
                                case eAAMP_SET_InitialBitrate:
                                {
                                        long initialBitrate;
					logprintf("Matchde Command eAAMP_SET_InitialBitrate - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &initialBitrate) == 2){
						mSingleton->SetInitialBitrate(initialBitrate);
					}
					break;
                                }
				case eAAMP_SET_InitialBitrate4k:
                                {
                                        long initialBitrate4k;
					logprintf("Matchde Command eAAMP_SET_InitialBitrate4k - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &initialBitrate4k) == 2){
						mSingleton->SetInitialBitrate4K(initialBitrate4k);
					}
					break;
                                }

				case eAAMP_SET_NetworkTimeout:
                                {
                                        double networkTimeout;
					logprintf("Matchde Command eAAMP_SET_NetworkTimeout - %s ", cmd);
					if (sscanf(cmd, "set %d %lf", &opt, &networkTimeout) == 2){
						mSingleton->SetNetworkTimeout(networkTimeout);
					}
					break;
                                }
				case eAAMP_SET_ManifestTimeout:
                                {
                                        double manifestTimeout;
					logprintf("Matchde Command eAAMP_SET_ManifestTimeout - %s ", cmd);
					if (sscanf(cmd, "set %d %lf", &opt, &manifestTimeout) == 2){
						mSingleton->SetManifestTimeout(manifestTimeout);
					}
					break;
                                }

				case eAAMP_SET_DownloadBufferSize:
                                {
                                        int downloadBufferSize;
					logprintf("Matchde Command eAAMP_SET_DownloadBufferSize - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &downloadBufferSize) == 2){
						mSingleton->SetDownloadBufferSize(downloadBufferSize);
					}
					break;
                                }

				case eAAMP_SET_PreferredDrm:
                                {
                                        int preferredDrm;
					logprintf("Matchde Command eAAMP_SET_PreferredDrm - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &preferredDrm) == 2){
						mSingleton->SetPreferredDRM((DRMSystems)preferredDrm);
					}
					break;
                                }

				case eAAMP_SET_StereoOnlyPlayback:
                                {
                                        int stereoOnlyPlayback;
					logprintf("Matchde Command eAAMP_SET_StereoOnlyPlayback - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &stereoOnlyPlayback) == 2){
						mSingleton->SetStereoOnlyPlayback(
							(stereoOnlyPlayback == 1 )? true:false);
					}
					break;
                                }

				case eAAMP_SET_AlternateContent:
                                {
                                        //Dummy implimentation
					std::string adBrkId = "";
					std::string adId = "";
					std::string url = "";
					logprintf("Matchde Command eAAMP_SET_AlternateContent - %s ", cmd);
					mSingleton->SetAlternateContents(adBrkId, adId, url);
					break;
                                }

				case eAAMP_SET_NetworkProxy:
                                {
                                        char networkProxy[128];
					logprintf("Matchde Command eAAMP_SET_NetworkProxy - %s ", cmd);
					if (sscanf(cmd, "set %d %s", &opt, networkProxy) == 2){
						mSingleton->SetNetworkProxy(networkProxy);
					}
					break;
                                }
				case eAAMP_SET_LicenseReqProxy:
                                {
                                        char licenseReqProxy[128];
					logprintf("Matchde Command eAAMP_SET_LicenseReqProxy - %s ", cmd);
					if (sscanf(cmd, "set %d %s", &opt, licenseReqProxy) == 2){
						mSingleton->SetLicenseReqProxy(licenseReqProxy);
					}
					break;
                                }
				case eAAMP_SET_DownloadStallTimeout:
                                {
                                        long downloadStallTimeout;
					logprintf("Matchde Command eAAMP_SET_DownloadStallTimeout - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &downloadStallTimeout) == 2){
						mSingleton->SetDownloadStallTimeout(downloadStallTimeout);
					}
					break;
                                }
				case eAAMP_SET_DownloadStartTimeout:
                                {
                                        long downloadStartTimeout;
					logprintf("Matchde Command eAAMP_SET_DownloadStartTimeout - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &downloadStartTimeout) == 2){
						mSingleton->SetDownloadStartTimeout(downloadStartTimeout);
					}
					break;
                                }

				case eAAMP_SET_PreferredSubtitleLang:
                                {
					char preferredSubtitleLang[12];
                                        logprintf("Matchde Command eAAMP_SET_PreferredSubtitleLang - %s ", cmd);
					if (sscanf(cmd, "set %d %s", &opt, preferredSubtitleLang) == 2){
						mSingleton->SetPreferredSubtitleLanguage(preferredSubtitleLang);
					}
					break;
                                }
				
				case eAAMP_SET_ParallelPlaylistDL:
                                {
					int parallelPlaylistDL;
					logprintf("Matchde Command eAAMP_SET_ParallelPlaylistDL - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &parallelPlaylistDL) == 2){
						mSingleton->SetParallelPlaylistDL( (parallelPlaylistDL == 1)? true:false );
					}
					break;
                                }
				case eAAMP_SET_PreferredLanguages:
				{
					logprintf("Matched Command eAAMP_SET_PreferredLanguages - %s ", cmd);
					const char* listStart = NULL;
					const char* listEnd = NULL;
					if((listStart = strchr(cmd, '"')) == NULL
							|| ( strlen(listStart+1) && (listEnd = strchr(listStart+1, '"')) == NULL) )
					{
						logprintf("preferred languages string has to be wrapped with \" \" ie \"eng, ger\"");
						break;
					}

					std::string preferredLanguages(listStart+1, listEnd-listStart-1);
					if(!preferredLanguages.empty())
						mSingleton->SetPreferredLanguages(preferredLanguages.c_str());
					else
						mSingleton->SetPreferredLanguages(NULL);
					break;
				}
				case eAAMP_SET_InitRampdownLimit:
                                {
					int rampDownLimit;
					logprintf("Matched Command eAAMP_SET_InitRampdownLimit - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &rampDownLimit) == 2){
						mSingleton->SetInitRampdownLimit(rampDownLimit);
					}
					break;
                                }
				case eAAMP_SET_RampDownLimit:
                                {
					int rampDownLimit;
					logprintf("Matched Command eAAMP_SET_RampDownLimit - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &rampDownLimit) == 2){
						mSingleton->SetRampDownLimit(rampDownLimit);
					}
					break;
                                }

				case eAAMP_SET_MinimumBitrate:
                                {
					long minBitrate;
					logprintf("Matched Command eAAMP_SET_MinimumBitrate - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &minBitrate) == 2){
						mSingleton->SetMinimumBitrate(minBitrate);
					}
					break;
                                }

				case eAAMP_SET_MaximumBitrate:
                                {
					long maxBitrate;
					logprintf("Matched Command eAAMP_SET_MaximumBitrate - %s ", cmd);
					if (sscanf(cmd, "set %d %ld", &opt, &maxBitrate) == 2){
						mSingleton->SetMaximumBitrate(maxBitrate);
					}
					break;
                                }

				case eAAMP_SET_MaximumSegmentInjFailCount:
                                {
					int failCount;
					logprintf("Matched Command eAAMP_SET_MaximumSegmentInjFailCount - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &failCount) == 2){
						mSingleton->SetSegmentInjectFailCount(failCount);
					}
					break;
                                }

				case eAAMP_SET_MaximumDrmDecryptFailCount:
                                {
					int failCount;
					logprintf("Matched Command eAAMP_SET_MaximumDrmDecryptFailCount - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &failCount) == 2){
						mSingleton->SetSegmentDecryptFailCount(failCount);
					}
					break;
                                }

				case eAAMP_SET_RegisterForID3MetadataEvents:
                                {
					int id3MetadataEventsEnabled;
					logprintf("Matched Command eAAMP_SET_RegisterForID3MetadataEvents - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &id3MetadataEventsEnabled) == 2){
						if (id3MetadataEventsEnabled)
						{
							mSingleton->AddEventListener(AAMP_EVENT_ID3_METADATA, myEventListener);
						}
						else
						{
							mSingleton->RemoveEventListener(AAMP_EVENT_ID3_METADATA, myEventListener);
						}

					}
					break;
                                }
				case eAAMP_SET_InitialBufferDuration:
				{
					int duration;
					logprintf("Matched Command eAAMP_SET_InitialBufferDuration - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &duration) == 2)
					{
						mSingleton->SetInitialBufferDuration(duration);
					}
					break;
				}
				case eAAMP_SET_AudioTrack:
				{
					int track;
					logprintf("Matched Command eAAMP_SET_AudioTrack - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &track) == 2)
					{
						mSingleton->SetAudioTrack(track);
					}
					break;
				}
				case eAAMP_SET_TextTrack:
				{
					int track;
					logprintf("Matched Command eAAMP_SET_TextTrack - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &track) == 2)
					{
						mSingleton->SetTextTrack(track);
					}
					break;
				}
				case eAAMP_SET_CCStatus:
				{
					int status;
					logprintf("Matched Command eAAMP_SET_CCStatus - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &status) == 2)
					{
						mSingleton->SetCCStatus(status == 1);
					}
					break;
				}
				case eAAMP_SET_CCStyle:
				{
					int value;
					logprintf("Matched Command eAAMP_SET_CCStyle - %s ", cmd);
					if (sscanf(cmd, "set %d %d", &opt, &value) == 2)
					{
						std::string options;
						switch (value)
						{
							case 1:
								options = std::string(CC_OPTION_1);
								break;
							case 2:
								options = std::string(CC_OPTION_2);
								break;
							case 3:
								options = std::string(CC_OPTION_3);
								break;
							default:
								logprintf("Invalid option passed. skipping!");
								break;
						}
						if (!options.empty())
						{
							mSingleton->SetTextStyle(options);
						}
					}
					break;
				}
				case eAAMP_SET_LanguageFormat:
				{
					LangCodePreference preference;
					bool useRole;
					if (sscanf(cmd, "set %d %d %d", &opt, &preference, &useRole) == 3)
					{
						mSingleton->SetLanguageFormat(preference, useRole);
					}
					break;
                                }
				case eAAMP_SET_PropagateUriParam:
                                {
                                        int propagateUriParam;
                                        logprintf("Matched Command eAAMP_SET_PropogateUriParam - %s ", cmd);
                                        if (sscanf(cmd, "set %d %d", &opt, &propagateUriParam) == 2){
                                                mSingleton->SetPropagateUriParameters((bool) propagateUriParam);
                                        }
                                        break;
                                }
				case eAAMP_SET_ThumbnailTrack:
				{
					printf("[AAMPCLI] Matched Command eAAMP_SET_ThumbnailTrack - %s\n",cmd);
					sscanf(cmd, "set %d %d", &opt, &rate);
					printf("[AAMPCLI] Setting ThumbnailTrack : %s\n",mSingleton->SetThumbnailTrack(rate)?"Success":"Failure");;
					break;
				}
				case eAAMP_SET_PausedBehavior:
				{
					char behavior[24];
					printf("[AAMPCLI] Matched Command eAAMP_SET_PausedBehavior - %s\n", cmd);
					if(sscanf(cmd, "set %d %d", &opt, &rate) == 2)
					{
						mSingleton->SetPausedBehavior(rate);
					}
					break;
				}
				default:
					logprintf("Invalid set command %d\n", opt);
			}

		}
		else if (sscanf(cmd, "set %s", help) == 1)
		{
			if(0 == strncmp("help", help, 4))
			{
				ShowHelpSet();
			}else
			{
				logprintf("Invalid usage of set operations %s", help);
			}
		}
		else
		{
			logprintf("Invalid set command = %s", cmd);
		}
	}
	else if (memcmp(cmd, "get", 3) == 0 )
	{
		char help[8];
		int opt, value1, value2;
		if (sscanf(cmd, "get %d", &opt) == 1){
			switch(opt){
				case eAAMP_GET_ThumbnailConfig:
					printf("[AAMPCLI] GETTING AVAILABLE THUMBNAIL TRACKS: %s\n", mSingleton->GetAvailableThumbnailTracks().c_str() );
					break;
				case eAAMP_GET_ThumbnailData:
					sscanf(cmd, "get %d %d %d",&opt, &value1, &value2);
					printf("[AAMPCLI] GETTING THUMBNAIL TIME RANGE DATA for duration(%d,%d): %s\n",value1,value2,mSingleton->GetThumbnails(value1, value2).c_str());
					break;
				case eAAMP_GET_AudioTrack:
					logprintf( "CURRENT AUDIO TRACK: %d", mSingleton->GetAudioTrack() );
					break;
				case eAAMP_GET_TextTrack:
					logprintf( "CURRENT TEXT TRACK: %d", mSingleton->GetTextTrack() );
					break;
				case eAAMP_GET_AvailableAudioTracks:
					logprintf( "AVAILABLE AUDIO TRACKS: %s", mSingleton->GetAvailableAudioTracks().c_str() );
					break;
				case eAAMP_GET_AvailableTextTracks:
					logprintf( "AVAILABLE TEXT TRACKS: %s", mSingleton->GetAvailableTextTracks().c_str() );
					break;

				case eAAMP_GET_CurrentAudioLan:
					logprintf(" CURRRENT AUDIO LANGUAGE = %s ",
					mSingleton->GetCurrentAudioLanguage());
					break;

				case eAAMP_GET_CurrentDrm:
					logprintf(" CURRRENT DRM  = %s ",
					mSingleton->GetCurrentDRM());
					break;

				case eAAMP_GET_PlaybackPosition:
					logprintf(" PLAYBACK POSITION = %lf ",
					mSingleton->GetPlaybackPosition());
					break;

				case eAAMP_GET_PlaybackDuration:
					logprintf(" PLAYBACK DURATION = %lf ",
					mSingleton->GetPlaybackDuration());
					break;

				case eAAMP_GET_VideoBitrate:
					logprintf(" VIDEO BITRATE = %ld ",
					mSingleton->GetVideoBitrate());
					break;

				case eAAMP_GET_AudioBitrate:
					logprintf(" AUDIO BITRATE = %ld ",
					mSingleton->GetAudioBitrate());
					break;

				case eAAMP_GET_AudioVolume:
					logprintf(" AUDIO VOULUME = %d ",
					mSingleton->GetAudioVolume());
					break;
				
				case eAAMP_GET_PlaybackRate:
					logprintf(" PLAYBACK RATE = %d ",
					mSingleton->GetPlaybackRate());
					break;

				case eAAMP_GET_VideoBitrates:
                                {
					std::vector<long int> videoBitrates;
					printf("[AAMP-PLAYER] VIDEO BITRATES = [ ");
                                        videoBitrates = mSingleton->GetVideoBitrates();
					for(int i=0; i < videoBitrates.size(); i++){
						printf("%ld, ", videoBitrates[i]);
					}
					printf(" ] \n");
					break;
                                }

				case eAAMP_GET_AudioBitrates:
                                {
					std::vector<long int> audioBitrates;
					printf("[AAMP-PLAYER] AUDIO BITRATES = [ ");
                                        audioBitrates = mSingleton->GetAudioBitrates();
					for(int i=0; i < audioBitrates.size(); i++){
						printf("%ld, ", audioBitrates[i]);
					}
					printf(" ] \n");
					break;
			        }	
				case eAAMP_GET_CurrentPreferredLanguages:
				{
					const char *prefferedLanguages = mSingleton->GetPreferredLanguages();
					logprintf(" PREFERRED LANGUAGES = \"%s\" ", prefferedLanguages? prefferedLanguages : "<NULL>");
					break;
				}

				default:
					logprintf("Invalid get command %d\n", opt);
			}

		}
		else if (sscanf(cmd, "get %s", help) == 1)
		{
			if(0 == strncmp("help", help, 4)){
				ShowHelpGet();
			}else
			{
				logprintf("Invalid usage of get operations %s", help);
			}
		}
		else
		{
			logprintf("Invalid get command = %s", cmd);
		}
	}
}

static void * run_command(void* param)
{
    char cmd[MAX_BUFFER_LENGTH];
    ShowHelp();
    char *ret = NULL;
    std::string* startUrl = (std::string*) param;
    if ((startUrl != nullptr) && (startUrl->size() > 0)) {
	    strncpy(cmd, startUrl->c_str(), MAX_BUFFER_LENGTH-1);
	    ProcessCliCommand(cmd);
    }
    do
    {
        logprintf("aamp-cli> ");
        if((ret = fgets(cmd, sizeof(cmd), stdin))!=NULL)
            ProcessCliCommand(cmd);
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
		logprintf("device::Manager::Initialize() succeeded");

	}
	catch (...)
	{
		logprintf("device::Manager::Initialize() failed");
	}
#endif
	char driveName = (*argv)[0];
	AampLogManager mLogManager;
	AampLogManager::disableLogRedirection = true;
	ABRManager mAbrManager;
	std::string startUrl;
	if (argc > 1) {
		startUrl = argv[1];
	}
	/* Set log directory path for AAMP and ABR Manager */
	mLogManager.setLogAndCfgDirectory(driveName);
	mAbrManager.setLogDirectory(driveName);

	logprintf("**************************************************************************");
	logprintf("** ADVANCED ADAPTIVE MICRO PLAYER (AAMP) - COMMAND LINE INTERFACE (CLI) **");
	logprintf("**************************************************************************");

	InitPlayerLoop(0,NULL);

	mSingleton = new PlayerInstanceAAMP();
#ifdef LOG_CLI_EVENTS
	myEventListener = new myAAMPEventListener();
	mSingleton->RegisterEvents(myEventListener);
#endif
	//std::string name = "testApp";
	//mSingleton->SetAppName(name);

#ifdef WIN32
	FILE *f = fopen(mLogManager.getAampCliCfgPath(), "rb");
#elif defined(__APPLE__)
	std::string cfgPath(getenv("HOME"));
	cfgPath += "/aampcli.cfg";
	FILE *f = fopen(cfgPath.c_str(), "rb");
#else
	FILE *f = fopen("/opt/aampcli.cfg", "rb");
#endif
	if (f)
	{
		logprintf("opened aampcli.cfg");
		char buf[MAX_BUFFER_LENGTH];
		while (fgets(buf, sizeof(buf), f))
		{
			ProcessCLIConfEntry(buf);
		}
		fclose(f);
	}

#ifdef __APPLE__
    pthread_t cmdThreadId;
    if(pthread_create(&cmdThreadId,NULL,run_command,&startUrl) != 0)
	{
		logprintf("Failed at create pthread errno = %d", errno);  //CID:83593 - checked return
	}

    createAndRunCocoaWindow();
	if(mBackgroundPlayer)
	{
		mBackgroundPlayer->Stop();
		delete mBackgroundPlayer;
	}
    mSingleton->Stop();
    delete mSingleton;
#else
    run_command(NULL);
#endif
}




