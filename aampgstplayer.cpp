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
 * @file aampgstplayer.cpp
 * @brief Gstreamer based player impl for AAMP
 */


#include "aampgstplayer.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h> // for sprintf
#include "priv_aamp.h"
#include <pthread.h>
#include <atomic>

#ifdef AAMP_MPD_DRM
#include "aampoutputprotection.h"
#endif


/**
 * @enum GstPlayFlags 
 * @brief Enum of configuration flags used by playbin
 */
typedef enum {
	GST_PLAY_FLAG_VIDEO = (1 << 0), // 0x001
	GST_PLAY_FLAG_AUDIO = (1 << 1), // 0x002
	GST_PLAY_FLAG_TEXT = (1 << 2), // 0x004
	GST_PLAY_FLAG_VIS = (1 << 3), // 0x008
	GST_PLAY_FLAG_SOFT_VOLUME = (1 << 4), // 0x010
	GST_PLAY_FLAG_NATIVE_AUDIO = (1 << 5), // 0x020
	GST_PLAY_FLAG_NATIVE_VIDEO = (1 << 6), // 0x040
	GST_PLAY_FLAG_DOWNLOAD = (1 << 7), // 0x080
	GST_PLAY_FLAG_BUFFERING = (1 << 8), // 0x100
	GST_PLAY_FLAG_DEINTERLACE = (1 << 9), // 0x200
	GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10) // 0x400
} GstPlayFlags;

//#define SUPPORT_MULTI_AUDIO
#define GST_ELEMENT_GET_STATE_RETRY_CNT_MAX 5

/*Playersinkbin events*/
#define GSTPLAYERSINKBIN_EVENT_HAVE_VIDEO 0x01
#define GSTPLAYERSINKBIN_EVENT_HAVE_AUDIO 0x02
#define GSTPLAYERSINKBIN_EVENT_FIRST_VIDEO_FRAME 0x03
#define GSTPLAYERSINKBIN_EVENT_FIRST_AUDIO_FRAME 0x04
#define GSTPLAYERSINKBIN_EVENT_ERROR_VIDEO_UNDERFLOW 0x06
#define GSTPLAYERSINKBIN_EVENT_ERROR_AUDIO_UNDERFLOW 0x07
#define GSTPLAYERSINKBIN_EVENT_ERROR_VIDEO_PTS 0x08
#define GSTPLAYERSINKBIN_EVENT_ERROR_AUDIO_PTS 0x09

#define USE_IDLE_LOOP_FOR_PROGRESS_REPORTING

#ifdef INTELCE
#define INPUT_GAIN_DB_MUTE  (gdouble)-145
#define INPUT_GAIN_DB_UNMUTE  (gdouble)0
#define DEFAULT_VIDEO_RECTANGLE "0,0,0,0"
#else
#define DEFAULT_VIDEO_RECTANGLE "0,0,1280,720"
#endif
#define DEFAULT_BUFFERING_LOW_PERCENT 2 // for 2M buffer, 2Mbps bitrate, 2% is around 160ms
#define DEFAULT_BUFFERING_TO_MS 10 // interval to check buffer fullness

/* Magic number (random value) used to validate struct AAMPGstPlayerPriv */
#define AAMP_GST_PLAYER_PRIVATE_MAGIC 0x8C19B86A

/**
 * @struct media_stream
 * @brief Holds stream(A/V) specific variables.
 */
struct media_stream
{
	GstElement *sinkbin;
	GstElement *source;
	StreamOutputFormat format;
	gboolean using_playersinkbin;
	bool flush;
	bool resetPosition;
	bool bufferUnderrun;
	bool eosReached;
};

/**
 * @struct AAMPGstPlayerPriv
 * @brief Holds private variables of AAMPGstPlayer
 */
struct AAMPGstPlayerPriv
{
	uint32_t magicNumber; // magicNumber used to validate that AAMPGstPlayerPriv has not been destroyed
	bool gstPropsDirty; //Flag used to check if gst props need to be set at start.
	media_stream stream[AAMP_TRACK_COUNT];
	GstElement *pipeline; //GstPipeline used for playback.
	GstBus *bus; //Bus for receiving GstEvents from pipeline.
	int current_rate; 
	guint64 total_bytes;
	gint n_audio; //Number of audio tracks.
	gint current_audio; //Offset of current audio track.
#ifdef USE_IDLE_LOOP_FOR_PROGRESS_REPORTING
	guint firstProgressCallbackIdleTaskId; //ID of idle handler created for notifying first progress event.
	std::atomic<bool> firstProgressCallbackIdleTaskPending; //Set if any first progress callback is pending.
	guint periodicProgressCallbackIdleTaskId; //ID of timed handler created for notifying progress events.
#endif
	GstElement *video_dec; //Video decoder used by pipeline.
	GstElement *video_sink; //Video sink used by pipeline.
	GstElement *audio_sink; //Audio sink used by pipeline.
#ifdef INTELCE_USE_VIDRENDSINK
	GstElement *video_pproc; //Video element used by pipeline.(only for Intel).
#endif

	float rate; //Current playback rate.
	VideoZoomMode zoom; //Video-zoom setting.
	bool videoMuted; //Video mute status.
	bool audioMuted; //Audio mute status.
	double audioVolume; //Audio volume.
	guint eosCallbackIdleTaskId; //ID of idle handler created for notifying EOS event.
	std::atomic<bool> eosCallbackIdleTaskPending; //Set if any eos callback is pending.
	bool firstFrameReceived; //Flag that denotes if first frame was notified.
	char videoRectangle[32]; //Video-rectangle co-ordinates in format x,y,w,h.
	bool pendingPlayState; //Flag that denotes if set pipeline to PLAYING state is pending.
	bool decoderHandleNotified; //Flag that denotes if decoder handle was notified.
	guint firstFrameCallbackIdleTaskId; //ID of idle handler created for notifying first frame event.
	GstEvent *protectionEvent; //GstEvent holding the pssi data to be sent downstream.
	std::atomic<bool> firstFrameCallbackIdleTaskPending; //Set if any first frame callback is pending.
	bool using_westerossink; //true if westros sink is used as video sink
	guint busWatchId;
	std::atomic<bool> eosSignalled; /** Indicates if EOS has signaled */
	gboolean buffering_enabled; // enable buffering based on multiqueue
	gboolean buffering_in_progress; // buffering is in progress
	GstState buffering_target_state; // the target state after buffering
	gint buffering_low_percent; // the low percent of bufferering before starts playing
	gboolean buffering_capable; // indicates if multiqueue is available in pipeline to perform buffering
#ifdef INTELCE
	bool keepLastFrame; //Keep last frame over next pipeline delete/ create cycle
#endif
};

#ifdef STANDALONE_AAMP
static GMainLoop *AAMPGstPlayerMainLoop = NULL; //GMainLoop instance for event management in AAMP standalone mode.
#endif

static const char* GstPluginNamePR = "aampplayreadydecryptor";
static const char* GstPluginNameWV = "aampwidevinedecryptor";

/**
 * @brief Called from the mainloop when a message is available on the bus
 * @param[in] bus the GstBus that sent the message
 * @param[in] msg the GstMessage
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval FALSE if the event source should be removed.
 */
static gboolean bus_message(GstBus * bus, GstMessage * msg, AAMPGstPlayer * _this);

/**
 * @brief Invoked synchronously when a message is available on the bus
 * @param[in] bus the GstBus that sent the message
 * @param[in] msg the GstMessage
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval FALSE if the event source should be removed.
 */
static GstBusSyncReply bus_sync_handler(GstBus * bus, GstMessage * msg, AAMPGstPlayer * _this);


/**
 * @brief AAMPGstPlayer Constructor
 * @param[in] aamp pointer to PrivateInstanceAAMP object associated with player
 */
AAMPGstPlayer::AAMPGstPlayer(PrivateInstanceAAMP *aamp)
{
	privateContext = (AAMPGstPlayerPriv *)malloc(sizeof(*privateContext));
	memset(privateContext, 0, sizeof(*privateContext));
	privateContext->magicNumber = (AAMP_GST_PLAYER_PRIVATE_MAGIC ^ ((uint64_t)privateContext));
	privateContext->audioVolume = 1.0;
	privateContext->gstPropsDirty = true; //Have to set audioVolume on gst startup
	privateContext->using_westerossink = false;
	if (getenv("PLAYERSINKBIN_USE_WESTEROSSINK"))
		privateContext->using_westerossink = true;
	this->aamp = aamp;
#ifdef STANDALONE_AAMP
	Init(0, NULL);
#endif

	CreatePipeline();
	privateContext->rate = 1.0;
	strcpy(privateContext->videoRectangle, DEFAULT_VIDEO_RECTANGLE);
}


/**
 * @brief AAMPGstPlayer Destructor
 */
AAMPGstPlayer::~AAMPGstPlayer()
{
	DestroyPipeline();
	privateContext->magicNumber = 0x0;
	free(privateContext);
#ifdef STANDALONE_AAMP
	Term();
#endif
}


/**
 * @brief Analyze stream info from the GstPipeline
 * @param[in] _this pointer to AAMPGstPlayer instance
 */
static void analyze_streams(AAMPGstPlayer *_this)
{
#ifdef SUPPORT_MULTI_AUDIO
	GstElement *sinkbin = _this->privateContext->stream[eMEDIATYPE_VIDEO].sinkbin;

	g_object_get(sinkbin, "n-audio", &_this->privateContext->n_audio, NULL);
	g_print("audio:\n");
	for (gint i = 0; i < _this->privateContext->n_audio; i++)
	{
		GstTagList *tags = NULL;
		g_signal_emit_by_name(sinkbin, "get-audio-tags", i, &tags);
		if (tags)
		{
			gchar *str;
			guint rate;

			g_print("audio stream %d:\n", i);
			if (gst_tag_list_get_string(tags, GST_TAG_AUDIO_CODEC, &str)) {
				g_print("  codec: %s\n", str);
				g_free(str);
			}
			if (gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &str)) {
				g_print("  language: %s\n", str);
				g_free(str);
			}
			if (gst_tag_list_get_uint(tags, GST_TAG_BITRATE, &rate)) {
				g_print("  bitrate: %d\n", rate);
			}
			gst_tag_list_free(tags);
		}
	}
	g_object_get(sinkbin, "current-audio", &_this->privateContext->current_audio, NULL);
#endif
}


/**
 * @brief Callback for appsrc "need-data" signal
 * @param[in] source pointer to appsrc instance triggering "need-data" signal
 * @param[in] size size of data required
 * @param[in] _this pointer to AAMPGstPlayer instance associated with the playback
 */
static void need_data(GstElement *source, guint size, AAMPGstPlayer * _this)
{
	if (source == _this->privateContext->stream[eMEDIATYPE_AUDIO].source)
	{
		_this->aamp->ResumeTrackDownloads(eMEDIATYPE_AUDIO); // signal fragment downloader thread
	}
        else
	{
		_this->aamp->ResumeTrackDownloads(eMEDIATYPE_VIDEO); // signal fragment downloader thread
	}
}


/**
 * @brief Callback for appsrc "enough-data" signal
 * @param[in] source pointer to appsrc instance triggering "enough-data" signal
 * @param[in] _this pointer to AAMPGstPlayer instance associated with the playback
 */
static void enough_data(GstElement *source, AAMPGstPlayer * _this)
{
	if (source == _this->privateContext->stream[eMEDIATYPE_AUDIO].source)
	{
		_this->aamp->StopTrackDownloads(eMEDIATYPE_AUDIO); // signal fragment downloader thread
	}
        else
	{
		_this->aamp->StopTrackDownloads(eMEDIATYPE_VIDEO); // signal fragment downloader thread
	}
}


/**
 * @brief Callback for appsrc "seek-data" signal
 * @param[in] src pointer to appsrc instance triggering "seek-data" signal 
 * @param[in] offset seek position offset
 * @param[in] _this pointer to AAMPGstPlayer instance associated with the playback
 */
static gboolean  appsrc_seek  (GstAppSrc *src, guint64 offset, AAMPGstPlayer * _this)
{
#ifdef TRACE
	logprintf("appsrc %p seek-signal - offset %" G_GUINT64_FORMAT "\n", src, offset);
#endif
	return TRUE;
}


/**
 * @brief Initialize properties/callback of appsrc
 * @param[in] _this pointer to AAMPGstPlayer instance associated with the playback
 * @param[in] source pointer to appsrc instance to be initialized
 * @param[in] mediaType stream type
 */
static void InitializeSource( AAMPGstPlayer *_this,GObject *source, MediaType mediaType = eMEDIATYPE_VIDEO )
{
	g_signal_connect(source, "need-data", G_CALLBACK(need_data), _this);
	g_signal_connect(source, "enough-data", G_CALLBACK(enough_data), _this);
	g_signal_connect(source, "seek-data", G_CALLBACK(appsrc_seek), _this);
	gst_app_src_set_stream_type(GST_APP_SRC(source), GST_APP_STREAM_TYPE_SEEKABLE);
	if (eMEDIATYPE_VIDEO == mediaType )
	{
#ifdef USE_SAGE_SVP
		g_object_set(source, "max-bytes", 4194304 * 3, NULL); // 4096k * 3
#else
		g_object_set(source, "max-bytes", (guint64)4194304, NULL); // 4096k
#endif
	}
	else
	{
#ifdef USE_SAGE_SVP
		g_object_set(source, "max-bytes", 512000 * 3, NULL); // 512k * 3 for audio
#else
		g_object_set(source, "max-bytes", (guint64)512000, NULL); // 512k for audio
#endif
	}
	g_object_set(source, "min-percent", 50, NULL);
	g_object_set(source, "format", GST_FORMAT_TIME, NULL);
}


/**
 * @brief Parse format to generate GstCaps
 * @param[in] format stream format to generate caps
 * @retval GstCaps for the input format
 */
static GstCaps* GetGstCaps(StreamOutputFormat format)
{
	GstCaps * caps = NULL;
	switch (format)
	{
		case FORMAT_MPEGTS:
			caps = gst_caps_new_simple ("video/mpegts",
					"systemstream", G_TYPE_BOOLEAN, TRUE,
					"packetsize", G_TYPE_INT, 188, NULL);
			break;
		case FORMAT_ISO_BMFF:
			caps = gst_caps_new_simple("video/quicktime", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_AAC:
			caps = gst_caps_new_simple ("audio/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"stream-format", G_TYPE_STRING, "adts", NULL);
			break;
		case FORMAT_AUDIO_ES_AC3:
			caps = gst_caps_new_simple ("audio/ac3", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_ATMOS:
			// Todo :: a) Test with all platforms if atmos works 
			//	   b) Test to see if x-eac3 config is enough for atmos stream.
			//	 	if x-eac3 is enough then both switch cases can be combined
			caps = gst_caps_new_simple ("audio/x-eac3", NULL, NULL);
                        break;
		case FORMAT_AUDIO_ES_EC3:
			caps = gst_caps_new_simple ("audio/x-eac3", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_H264:
#ifdef INTELCE
			caps = gst_caps_new_simple ("video/x-h264",
					"stream-format", G_TYPE_STRING, "avc",
					"width", G_TYPE_INT, 1920,
					"height", G_TYPE_INT, 1080,
					NULL);
#else
			caps = gst_caps_new_simple ("video/x-h264", NULL, NULL);
#endif
			break;
		case FORMAT_VIDEO_ES_HEVC:
			caps = gst_caps_new_simple ("video/x-h265", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_MPEG2:
			caps = gst_caps_new_simple ("video/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
		case FORMAT_INVALID:
		case FORMAT_NONE:
		default:
			logprintf("Unsupported format %d\n", format);
			break;
	}
	return caps;
}


/**
 * @brief Callback when source is added by playbin
 * @param[in] object a GstObject
 * @param[in] orig the object that originated the signal
 * @param[in] pspec the property that changed
 * @param[in] _this pointer to AAMPGstPlayer instance associated with the playback
 */
static void found_source(GObject * object, GObject * orig, GParamSpec * pspec, AAMPGstPlayer * _this )
{
	logprintf("AAMPGstPlayer: found_source\n");
	MediaType mediaType;
	media_stream *stream;
	GstCaps * caps;
	if (object == G_OBJECT(_this->privateContext->stream[eMEDIATYPE_VIDEO].sinkbin))
	{
		logprintf("Found source for bin1\n");
		mediaType = eMEDIATYPE_VIDEO;
	}
	else
	{
		logprintf("Found source for bin2\n");
		mediaType = eMEDIATYPE_AUDIO;
	}
	stream = &_this->privateContext->stream[mediaType];
	g_object_get(orig, pspec->name, &stream->source, NULL);
	InitializeSource(_this, G_OBJECT(stream->source), mediaType);
	caps = GetGstCaps(stream->format);
	gst_app_src_set_caps(GST_APP_SRC(stream->source), caps);
	gst_caps_unref(caps);
}

#ifdef DEBUG_GST_MESSAGE_TAG

/**
 * @brief Calls this function for each tag inside the tag list
 * @param[in] list the GstTagList
 * @param[in] tag a name of a tag in list
 * @param[in] user_data user data provided when registering callback
 */
static void print_tag( const GstTagList * list, const gchar * tag, gpointer user_data)
{
	guint count = gst_tag_list_get_tag_size(list, tag);
	for ( guint i = 0; i < count; i++)
	{
		gchar *str = NULL;
		if (gst_tag_get_type(tag) == G_TYPE_STRING)
		{
			if (!gst_tag_list_get_string_index(list, tag, i, &str))
			{
				g_assert_not_reached();
			}
		}
		else
		{
			str = g_strdup_value_contents(gst_tag_list_get_value_index(list, tag, i));
		}
		if (i == 0)
		{
			g_print("  %15s: %s\n", gst_tag_get_nick(tag), str);
		}
		else
		{
			g_print("                 : %s\n", str);
		}
		g_free(str);
	}
}
#endif


/**
 * @brief Idle callback to notify first frame rendered event
 * @param[in] user_data pointer to AAMPGstPlayer instance
 * @retval G_SOURCE_REMOVE, if the source should be removed
 */
static gboolean IdleCallbackOnFirstFrame(gpointer user_data)
{
        AAMPGstPlayer *_this = (AAMPGstPlayer *)user_data;
        _this->aamp->NotifyFirstFrameReceived();
        _this->privateContext->firstFrameCallbackIdleTaskPending = false;
        _this->privateContext->firstFrameCallbackIdleTaskId = 0;
        return G_SOURCE_REMOVE;
}


/**
 * @brief Idle callback to notify end-of-stream event
 * @param[in] user_data pointer to AAMPGstPlayer instance
 * @retval G_SOURCE_REMOVE, if the source should be removed
 */
static gboolean IdleCallbackOnEOS(gpointer user_data)
{
	AAMPGstPlayer *_this = (AAMPGstPlayer *)user_data;
	_this->privateContext->eosCallbackIdleTaskPending = false;
	logprintf("%s:%d  eosCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, _this->privateContext->eosCallbackIdleTaskId);
	_this->aamp->NotifyEOSReached();
	_this->privateContext->eosCallbackIdleTaskId = 0;
	return G_SOURCE_REMOVE;
}

#ifdef USE_IDLE_LOOP_FOR_PROGRESS_REPORTING


/**
 * @brief Timer's callback to notify playback progress event
 * @param[in] user_data pointer to AAMPGstPlayer instance
 * @retval G_SOURCE_REMOVE, if the source should be removed
 */
static gboolean ProgressCallbackOnTimeout(gpointer user_data)
{
	AAMPGstPlayer *_this = (AAMPGstPlayer *)user_data;
	_this->aamp->ReportProgress();
	traceprintf("%s:%d current %d, stored %d \n", __FUNCTION__, __LINE__, g_source_get_id(g_main_current_source()), _this->privateContext->periodicProgressCallbackIdleTaskId);
	return G_SOURCE_CONTINUE;
}


/**
 * @brief Idle callback to start progress notifier timer
 * @param[in] user_data pointer to AAMPGstPlayer instance
 * @retval G_SOURCE_REMOVE, if the source should be removed
 */
static gboolean IdleCallback(gpointer user_data)
{
	AAMPGstPlayer *_this = (AAMPGstPlayer *)user_data;
	_this->aamp->ReportProgress();
	_this->privateContext->firstProgressCallbackIdleTaskPending = false;
	_this->privateContext->firstProgressCallbackIdleTaskId = 0;
	_this->privateContext->periodicProgressCallbackIdleTaskId = g_timeout_add(gpGlobalConfig->reportProgressInterval, ProgressCallbackOnTimeout, user_data);
	logprintf("%s:%d current %d, periodicProgressCallbackIdleTaskId %d \n", __FUNCTION__, __LINE__, g_source_get_id(g_main_current_source()), _this->privateContext->periodicProgressCallbackIdleTaskId);
	return G_SOURCE_REMOVE;
}
#endif

/**
 * @brief Notify first Audio and Video frame through an idle function to make the playersinkbin halding same as normal(playbin) playback.
 * @param[in] type media type of the frame which is decoded, either audio or video.
 */
void AAMPGstPlayer::NotifyFirstFrame(MediaType type)
{
	if(!privateContext->firstFrameReceived)
	{
		privateContext->firstFrameReceived = true;
		aamp->LogFirstFrame();
		aamp->LogTuneComplete();
	}

	if (eMEDIATYPE_VIDEO == type)
	{
		if (!privateContext->decoderHandleNotified)
		{
			privateContext->decoderHandleNotified = true;
			privateContext->firstFrameCallbackIdleTaskPending = true;
			privateContext->firstFrameCallbackIdleTaskId = g_idle_add(IdleCallbackOnFirstFrame, this);
			if (!privateContext->firstFrameCallbackIdleTaskPending)
			{
				logprintf("%s:%d firstFrameCallbackIdleTask already finished, reset id\n", __FUNCTION__, __LINE__);
				privateContext->firstFrameCallbackIdleTaskId = 0;
			}
		}
#ifdef USE_IDLE_LOOP_FOR_PROGRESS_REPORTING
		if (privateContext->firstProgressCallbackIdleTaskId == 0)
		{
			privateContext->firstProgressCallbackIdleTaskPending = true;
			privateContext->firstProgressCallbackIdleTaskId = g_idle_add(IdleCallback, this);
			if (!privateContext->firstProgressCallbackIdleTaskPending)
			{
				logprintf("%s:%d firstProgressCallbackIdleTask already finished, reset id\n", __FUNCTION__, __LINE__);
				privateContext->firstProgressCallbackIdleTaskId = 0;
			}
		}
#endif
	}
}

/**
 * @brief Callback invoked after first video frame decoded
 * @param[in] object pointer to element raising the callback
 * @param[in] arg0 number of arguments
 * @param[in] arg1 array of arguments
 * @param[in] _this pointer to AAMPGstPlayer instance
 */
static void AAMPGstPlayer_OnVideoFirstFrameBrcmVidDecoder(GstElement* object, guint arg0, gpointer arg1,
	AAMPGstPlayer * _this)

{
	logprintf("AAMPGstPlayer_OnVideoFirstFrameBrcmVidDecoder. got First Video Frame\n");
	_this->NotifyFirstFrame(eMEDIATYPE_VIDEO);

}

/**
 * @brief Callback invoked after first audio buffer decoded
 * @param[in] object pointer to element raising the callback
 * @param[in] arg0 number of arguments
 * @param[in] arg1 array of arguments
 * @param[in] _this pointer to AAMPGstPlayer instance
 */
static void AAMPGstPlayer_OnAudioFirstFrameBrcmAudDecoder(GstElement* object, guint arg0, gpointer arg1,
        AAMPGstPlayer * _this)
{
	logprintf("AAMPGstPlayer_OnAudioFirstFrameBrcmAudDecoder. got First Audio Frame\n");
	_this->NotifyFirstFrame(eMEDIATYPE_AUDIO);
}

/**
 * @brief Check if gstreamer element is video decoder
 * @param[in] name Name of the element
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval TRUE if element name is that of the decoder
 */
bool AAMPGstPlayer_isVideoDecoder(const char* name, AAMPGstPlayer * _this)
{
	return	(!_this->privateContext->using_westerossink && memcmp(name, "brcmvideodecoder", 16) == 0) || 
			( _this->privateContext->using_westerossink && memcmp(name, "westerossink", 12) == 0);
}

/**
 * @brief Check if gstreamer element is video sink
 * @param[in] name Name of the element
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval TRUE if element name is that of video sink
 */
bool AAMPGstPlayer_isVideoSink(const char* name, AAMPGstPlayer * _this)
{
	return	(!_this->privateContext->using_westerossink && memcmp(name, "brcmvideosink", 13) == 0) || // brcmvideosink0, brcmvideosink1, ...
			( _this->privateContext->using_westerossink && memcmp(name, "westerossink", 12) == 0);
}

/**
 * @brief Check if gstreamer element is audio decoder
 * @param[in] name Name of the element
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval TRUE if element name is that of audio decoder
 */
bool AAMPGstPlayer_isVideoOrAudioDecoder(const char* name, AAMPGstPlayer * _this)
{
	return	(!_this->privateContext->using_westerossink && !_this->privateContext->stream[eMEDIATYPE_VIDEO].using_playersinkbin &&
			(memcmp(name, "brcmvideodecoder", 16) == 0 || memcmp(name, "brcmaudiodecoder", 16) == 0)) || 
			(_this->privateContext->using_westerossink && memcmp(name, "westerossink", 12) == 0);
}

/**
 * @brief Callback invoked when facing an underflow
 * @param[in] object pointer to element raising the callback
 * @param[in] arg0 number of arguments
 * @param[in] arg1 array of arguments
 * @param[in] _this pointer to AAMPGstPlayer instance
 */
static void AAMPGstPlayer_OnGstBufferUnderflowCb(GstElement* object, guint arg0, gpointer arg1,
        AAMPGstPlayer * _this)
{
	//TODO - Handle underflow
	MediaType type;
	logprintf("## %s() : Got Underflow message from %s ##\n", __FUNCTION__, GST_ELEMENT_NAME(object));
	if (AAMPGstPlayer_isVideoDecoder(GST_ELEMENT_NAME(object), _this))
	{
		type = eMEDIATYPE_VIDEO;
	}
	else if (memcmp(GST_ELEMENT_NAME(object), "brcmaudiodecoder", 16) == 0)
	{
		type = eMEDIATYPE_AUDIO;
	}

	// Validate that _this->privateContext is valid and has not been freed.
	if ((_this->privateContext == NULL) ||
	    (_this->privateContext->magicNumber != (AAMP_GST_PLAYER_PRIVATE_MAGIC ^ ((uint64_t)(_this->privateContext)))))
	{
		// Return immediately if the player private context is no longer valid.  Fix crash DELIA-31456
		logprintf("## %s() : Ignore Underflow, the player context is no longer valid ##\n", __FUNCTION__);
		return;
	}

	_this->privateContext->stream[type].bufferUnderrun = true;
	if (_this->privateContext->stream[type].eosReached)
	{
		if (_this->privateContext->rate > 0)
		{
			_this->NotifyEOS();
		}
		else
		{
			_this->aamp->ScheduleRetune(eGST_ERROR_UNDERFLOW, type);
		}
		_this->privateContext->stream[type].eosReached = false;
	}
	else
	{
		if (!_this->privateContext->buffering_enabled || !_this->privateContext->buffering_capable)
		{
			_this->aamp->ScheduleRetune(eGST_ERROR_UNDERFLOW, type);
		}
		else
		{
			gst_element_set_state (_this->privateContext->pipeline, GST_STATE_PAUSED);
		}
	}
}

/**
 * @brief Callback invoked a PTS error is encountered
 * @param[in] object pointer to element raising the callback
 * @param[in] arg0 number of arguments
 * @param[in] arg1 array of arguments
 * @param[in] _this pointer to AAMPGstPlayer instance
 */
static void AAMPGstPlayer_OnGstPtsErrorCb(GstElement* object, guint arg0, gpointer arg1,
        AAMPGstPlayer * _this)
{
	logprintf("## %s() : Got PTS error message from %s ##\n", __FUNCTION__, GST_ELEMENT_NAME(object));
	if (AAMPGstPlayer_isVideoDecoder(GST_ELEMENT_NAME(object), _this))
	{
		_this->aamp->ScheduleRetune(eGST_ERROR_PTS, eMEDIATYPE_VIDEO);
	}
	else if (memcmp(GST_ELEMENT_NAME(object), "brcmaudiodecoder", 16) == 0)
	{
		_this->aamp->ScheduleRetune(eGST_ERROR_PTS, eMEDIATYPE_AUDIO);
	}
}



static gboolean buffering_timeout (gpointer data)
{
	AAMPGstPlayer * _this = (AAMPGstPlayer *) data;
	GstQuery *query;
	gboolean busy;
	gint percent;
	gint64 estimated_total;
	gint64 position, duration;
	guint64 play_left;

	if (_this->privateContext->buffering_enabled)
	{
		query = gst_query_new_buffering (GST_FORMAT_TIME);
		if (!gst_element_query (_this->privateContext->pipeline, query))
		{
			return TRUE;
		}

		gst_query_parse_buffering_percent (query, &busy, &percent);

		/* we are buffering or the estimated download time is bigger than the
		 * remaining playback time. We keep buffering.
		 */
		_this->privateContext->buffering_in_progress = (busy || percent < _this->privateContext->buffering_low_percent);
		if (!_this->privateContext->buffering_in_progress)
		{
			_this->privateContext->buffering_enabled = false;
			logprintf("%s:%d Set pipeline state to %s\n", __FUNCTION__, __LINE__, gst_element_state_get_name(_this->privateContext->buffering_target_state));
			gst_element_set_state (_this->privateContext->pipeline, _this->privateContext->buffering_target_state);
		}
	}
	return _this->privateContext->buffering_in_progress;
}

/**
 * @brief Called from the mainloop when a message is available on the bus
 * @param[in] bus the GstBus that sent the message
 * @param[in] msg the GstMessage
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval FALSE if the event source should be removed.
 */
static gboolean bus_message(GstBus * bus, GstMessage * msg, AAMPGstPlayer * _this)
{
	GError *error;
	gchar *dbg_info;
	bool isPlaybinStateChangeEvent;

	switch (GST_MESSAGE_TYPE(msg))
	{ // see https://developer.gnome.org/gstreamer/stable/gstreamer-GstMessage.html#GstMessage
	case GST_MESSAGE_ERROR:
		gst_message_parse_error(msg, &error, &dbg_info);
		g_printerr("GST_MESSAGE_ERROR %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
		char errorDesc[MAX_ERROR_DESCRIPTION_LENGTH];
		memset(errorDesc, '\0', MAX_ERROR_DESCRIPTION_LENGTH);
		strncpy(errorDesc, "GstPipeline Error:", 18);
		strncat(errorDesc, error->message, MAX_ERROR_DESCRIPTION_LENGTH - 18 - 1);
		if (strstr(error->message, "video decode error") != NULL ||
			 strstr(error->message, "HDCP Authentication Failure") != NULL)
		{
			_this->aamp->SendErrorEvent(AAMP_TUNE_GST_PIPELINE_ERROR, errorDesc, false);
		}
		else
		{
			_this->aamp->SendErrorEvent(AAMP_TUNE_GST_PIPELINE_ERROR, errorDesc);
		}
		g_printerr("Debug Info: %s\n", (dbg_info) ? dbg_info : "none");
		g_clear_error(&error);
		g_free(dbg_info);
		break;

	case GST_MESSAGE_WARNING:
		gst_message_parse_warning(msg, &error, &dbg_info);
		g_printerr("GST_MESSAGE_WARNING %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
		g_printerr("Debug Info: %s\n", (dbg_info) ? dbg_info : "none");
		g_clear_error(&error);
		g_free(dbg_info);
		break;
		
	case GST_MESSAGE_EOS:
		/**
		 * pipeline event: end-of-stream reached
		 * application may perform flushing seek to resume playback
		 */
		logprintf("GST_MESSAGE_EOS\n");
		_this->NotifyEOS();
		break;

	case GST_MESSAGE_STATE_CHANGED:
		GstState old_state, new_state, pending_state;
		gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

		isPlaybinStateChangeEvent = (GST_MESSAGE_SRC(msg) == GST_OBJECT(_this->privateContext->pipeline));

		if (gpGlobalConfig->logging.gst || isPlaybinStateChangeEvent)
		{
			logprintf("%s %s -> %s (pending %s)\n",
				GST_OBJECT_NAME(msg->src),
				gst_element_state_get_name(old_state),
				gst_element_state_get_name(new_state),
				gst_element_state_get_name(pending_state));
			if (isPlaybinStateChangeEvent && new_state == GST_STATE_PLAYING)
			{
#if defined(INTELCE) || (defined(STANDALONE_AAMP) && defined(__APPLE__))
				if(!_this->privateContext->firstFrameReceived)
				{
					_this->privateContext->firstFrameReceived = true;
					_this->aamp->LogFirstFrame();
					_this->aamp->LogTuneComplete();
				}
				_this->aamp->NotifyFirstFrameReceived();
#endif

				//Pipeline has moved into playing state and no buffering active, reset buffering flag
				if (_this->privateContext->buffering_enabled && !_this->privateContext->buffering_capable)
				{
					_this->privateContext->buffering_enabled = false;
				}

#if defined(USE_IDLE_LOOP_FOR_PROGRESS_REPORTING) && (defined(INTELCE))
				//Note: Progress event should be sent after the decoderAvailable event only.
				//BRCM platform sends progress event after AAMPGstPlayer_OnVideoFirstFrameBrcmVidDecoder.
				if (_this->privateContext->firstProgressCallbackIdleTaskId == 0)
				{
					_this->privateContext->firstProgressCallbackIdleTaskId = g_idle_add(IdleCallback, _this);
				}
#endif
				analyze_streams(_this);

				if (gpGlobalConfig->logging.gst )
				{
					GST_DEBUG_BIN_TO_DOT_FILE((GstBin *)_this->privateContext->pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "myplayer");
					// output graph to .dot format which can be visualized with Graphviz tool if:
					// gstreamer is configured with --gst-enable-gst-debug
					// and "gst" is enabled in aamp.cfg
					// and environment variable GST_DEBUG_DUMP_DOT_DIR is set to a basepath(e.g. /opt).
				}
			}
		}
		if ((!_this->privateContext->stream[eMEDIATYPE_VIDEO].using_playersinkbin) && (_this->privateContext->gstPropsDirty))
		{
#ifndef INTELCE
			if (new_state == GST_STATE_PAUSED && old_state == GST_STATE_READY)
			{
				if (AAMPGstPlayer_isVideoSink(GST_OBJECT_NAME(msg->src), _this))
				{ // video scaling patch
					/*
					brcmvideosink doesn't sets the rectangle property correct by default
					gst-inspect-1.0 brcmvideosink
					g_object_get(_this->privateContext->pipeline, "video-sink", &videoSink, NULL); - reports NULL
					note: alternate "window-set" works as well
					*/
					_this->privateContext->video_sink = (GstElement *) msg->src;
					logprintf("AAMPGstPlayer setting rectangle, video mute and zoom\n");
					g_object_set(msg->src, "rectangle", _this->privateContext->videoRectangle, NULL);
					g_object_set(msg->src, "zoom-mode", VIDEO_ZOOM_FULL == _this->privateContext->zoom ? 0 : 1, NULL);
					g_object_set(msg->src, "show-video-window", !_this->privateContext->videoMuted, NULL);
					g_object_set(msg->src, "enable-reject-preroll", FALSE, NULL);
				}
				else if (memcmp(GST_OBJECT_NAME(msg->src), "brcmaudiosink", 13) == 0)
				{
					_this->privateContext->audio_sink = (GstElement *) msg->src;

					_this->setVolumeOrMuteUnMute();

					g_object_set(msg->src, "enable_reject_preroll", FALSE, NULL);
				}
				else if (strstr(GST_OBJECT_NAME(msg->src), "brcmaudiodecoder"))
				{
					GstElement * audio_dec = (GstElement *) msg->src;
					// this reduces amount of data in the fifo, which is flushed/lost when transition from expert to normal modes
					g_object_set(msg->src, "limit_buffering", 1, NULL);
					logprintf("Found brcmaudiodecoder, limiting audio decoder buffering");
				}

				StreamOutputFormat audFormat = _this->privateContext->stream[eMEDIATYPE_AUDIO].format;

				if ((audFormat == FORMAT_NONE || _this->privateContext->audio_sink != NULL) &&
					(_this->privateContext->video_sink != NULL))
				{
					_this->privateContext->gstPropsDirty = false;
				}
			}
#endif
		}
		if (AAMPGstPlayer_isVideoOrAudioDecoder(GST_OBJECT_NAME(msg->src), _this))
		{
#ifdef AAMP_MPD_DRM
			// This is the video decoder, send this to the output protection module
			// so it can get the source width/height
			if (AAMPGstPlayer_isVideoDecoder(GST_OBJECT_NAME(msg->src), _this))
			{
				logprintf("AAMPGstPlayer Found --> brcmvideodecoder = %p\n", msg->src);
				if(AampOutputProtection::IsAampOutputProcectionInstanceActive())
				{
					AampOutputProtection *pInstance = AampOutputProtection::GetAampOutputProcectionInstance();
					pInstance->setGstElement((GstElement *)(msg->src));
					pInstance->Release();
				}
			}
#endif
			if (old_state == GST_STATE_NULL && new_state == GST_STATE_READY)
			{
				g_signal_connect(msg->src, "buffer-underflow-callback",
					G_CALLBACK(AAMPGstPlayer_OnGstBufferUnderflowCb), _this);
				g_signal_connect(msg->src, "pts-error-callback",
					G_CALLBACK(AAMPGstPlayer_OnGstPtsErrorCb), _this);
			}
		}

		if (_this->privateContext->buffering_enabled)
		{
			if (memcmp(GST_OBJECT_NAME(msg->src), "multiqueue", 10) == 0)
			{
				GstElement * parent = NULL;
				parent = (GstElement *)gst_element_get_parent(msg->src);
				parent = (GstElement *)gst_element_get_parent(parent);
				parent = (GstElement *)gst_element_get_parent(parent); // playerbin
				char *n1 = GST_OBJECT_NAME(parent);
				char *n2 = GST_OBJECT_NAME(_this->privateContext->stream[eMEDIATYPE_VIDEO].sinkbin);

				if (memcmp(n1, n2, strlen(n1)) == 0)
				{
					gboolean b_use_buffering;
					g_object_get(msg->src, "use-buffering", &b_use_buffering, NULL);

					if (b_use_buffering == 0)
					{
						logprintf("%s set use-buffering to 1, old/new state %d/%d, \n", GST_ELEMENT_NAME(msg->src),
								old_state, new_state);
						g_object_set(msg->src, "use-buffering", 1, NULL);
						_this->privateContext->buffering_capable = true;
					}
				}
			}
		}
		break;

	case GST_MESSAGE_ASYNC_DONE:
		{

			if (_this->privateContext->buffering_enabled)
			{
				if (!_this->privateContext->buffering_in_progress)
				{
					logprintf("%s:%d Set pipeline state to %s\n", __FUNCTION__, __LINE__, gst_element_state_get_name(_this->privateContext->buffering_target_state));
					gst_element_set_state (_this->privateContext->pipeline, _this->privateContext->buffering_target_state);
				}
				else
				{
					g_timeout_add (DEFAULT_BUFFERING_TO_MS, buffering_timeout, _this);
				}
			}
		}
		break;

	case GST_MESSAGE_BUFFERING:
		{
			gint percent;
			gst_message_parse_buffering (msg, &percent);
			if (_this->privateContext->buffering_enabled)
			{
				if (percent < _this->privateContext->buffering_low_percent)
				{
					if (!_this->privateContext->buffering_in_progress)
					{
						_this->privateContext->buffering_in_progress = true;
						if (_this->privateContext->buffering_target_state == GST_STATE_PLAYING)
						{
							/* we were not buffering but PLAYING, PAUSE  the pipeline. */
							gst_element_set_state (_this->privateContext->pipeline, GST_STATE_PAUSED);
						}
						logprintf("%s : eBuffering %d percent\n", GST_ELEMENT_NAME(_this->privateContext->pipeline), percent);
					}
				}
				else
				{
					/* stop buffering, to simplify, only do buffering at the beginning of stream*/
					g_object_set(msg->src, "use-buffering", 0, NULL);
					_this->privateContext->buffering_enabled = false;
					gst_element_set_state (_this->privateContext->pipeline, GST_STATE_PLAYING);
				}
			}
		}
		break;

	case GST_MESSAGE_TAG:
#ifdef DEBUG_GST_MESSAGE_TAG
		logprintf("GST_MESSAGE_TAG\n");
		{
			GstTagList *tags = NULL;
			gst_message_parse_tag(msg, &tags);
			gst_tag_list_foreach(tags, print_tag, NULL);
			gst_tag_list_free(tags);
		}
#endif
		break;

	case GST_MESSAGE_QOS:
	{
		gboolean live;
		guint64 running_time;
		guint64 stream_time;
		guint64 timestamp;
		guint64 duration;
		gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);
		break;
	}

	case GST_MESSAGE_CLOCK_LOST:
		logprintf("GST_MESSAGE_CLOCK_LOST\n");
		// get new clock - needed?
		gst_element_set_state(_this->privateContext->pipeline, GST_STATE_PAUSED);
		gst_element_set_state(_this->privateContext->pipeline, GST_STATE_PLAYING);
		break;

#ifdef TRACE
	case GST_MESSAGE_RESET_TIME:
		GstClockTime running_time;
		gst_message_parse_reset_time (msg, &running_time);
		printf("GST_MESSAGE_RESET_TIME %llu\n", (unsigned long long)running_time);
		break;
#endif

#ifdef USE_GST1
	case GST_MESSAGE_NEED_CONTEXT:

		/*
		 * Code to avoid logs flooding with NEED-CONTEXT message for DRM systems
		 */
		/*
		const gchar* contextType;
		gst_message_parse_context_type(msg, &contextType);
		if (!g_strcmp0(contextType, "drm-preferred-decryption-system-id"))
		{
			logprintf("Setting Playready context\n");
			GstContext* context = gst_context_new("drm-preferred-decryption-system-id", FALSE);
			GstStructure* contextStructure = gst_context_writable_structure(context);
			gst_structure_set(contextStructure, "decryption-system-id", G_TYPE_STRING, "9a04f079-9840-4286-ab92-e65be0885f95", NULL);
			gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
		}
		*/
		break;
#endif

	case GST_MESSAGE_STREAM_STATUS:
	case GST_MESSAGE_ELEMENT: // can be used to collect pts, dts, pid
	case GST_MESSAGE_DURATION:
	case GST_MESSAGE_LATENCY:
	case GST_MESSAGE_NEW_CLOCK:
		break;

	default:
		logprintf("msg type: %s\n", gst_message_type_get_name(msg->type));
		break;
	}
	return TRUE;
}


/**
 * @brief Invoked synchronously when a message is available on the bus
 * @param[in] bus the GstBus that sent the message
 * @param[in] msg the GstMessage
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @retval FALSE if the event source should be removed.
 */
static GstBusSyncReply bus_sync_handler(GstBus * bus, GstMessage * msg, AAMPGstPlayer * _this)
{
	switch(GST_MESSAGE_TYPE(msg))
	{
	case GST_MESSAGE_STATE_CHANGED:
		GstState old_state, new_state;
		gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
		if (old_state == GST_STATE_NULL && new_state == GST_STATE_READY)
		{
#ifndef INTELCE
			if (AAMPGstPlayer_isVideoOrAudioDecoder(GST_OBJECT_NAME(msg->src), _this))
			{
				if (AAMPGstPlayer_isVideoDecoder(GST_OBJECT_NAME(msg->src), _this))
				{
					_this->privateContext->video_dec = (GstElement *) msg->src;
					g_signal_connect(_this->privateContext->video_dec, "first-video-frame-callback",
									G_CALLBACK(AAMPGstPlayer_OnVideoFirstFrameBrcmVidDecoder), _this);
					if(!_this->privateContext->using_westerossink)
					{
						g_object_set(msg->src, "report_decode_errors", TRUE, NULL);
					}
				}
				else
				{
					g_signal_connect(msg->src, "first-audio-frame-callback",
									G_CALLBACK(AAMPGstPlayer_OnAudioFirstFrameBrcmAudDecoder), _this);
				}
			}
#else
			if (memcmp(GST_OBJECT_NAME(msg->src), "ismdgstaudiosink", 16) == 0)
			{
				_this->privateContext->audio_sink = (GstElement *) msg->src;

				logprintf("AAMPGstPlayer setting audio-sync\n");
				g_object_set(msg->src, "sync", TRUE, NULL);

				_this->setVolumeOrMuteUnMute();
			}
			else
			{
#ifndef INTELCE_USE_VIDRENDSINK
				if (memcmp(GST_OBJECT_NAME(msg->src), "ismdgstvidsink", 14) == 0)
#else
				if (memcmp(GST_OBJECT_NAME(msg->src), "ismdgstvidrendsink", 18) == 0)
#endif
				{
					AAMPGstPlayerPriv *privateContext = _this->privateContext;
					privateContext->video_sink = (GstElement *) msg->src;
					logprintf("AAMPGstPlayer setting stop-keep-frame %d\n", (int)(privateContext->keepLastFrame));
					g_object_set(msg->src, "stop-keep-frame", privateContext->keepLastFrame, NULL);
#if defined(INTELCE) && !defined(INTELCE_USE_VIDRENDSINK)
					logprintf("AAMPGstPlayer setting rectangle %s\n", privateContext->videoRectangle);
					g_object_set(msg->src, "rectangle", privateContext->videoRectangle, NULL);
					logprintf("AAMPGstPlayer setting zoom %s\n", (VIDEO_ZOOM_FULL == privateContext->zoom) ? "FULL" : "NONE");
					g_object_set(msg->src, "scale-mode", (VIDEO_ZOOM_FULL == privateContext->zoom) ? 0 : 3, NULL);
#endif
					logprintf("AAMPGstPlayer setting video mute %d\n", privateContext->videoMuted);
					g_object_set(msg->src, "mute", privateContext->videoMuted, NULL);
				}
				else if (memcmp(GST_OBJECT_NAME(msg->src), "ismdgsth264viddec", 17) == 0)
				{
					_this->privateContext->video_dec = (GstElement *) msg->src;
				}
#ifdef INTELCE_USE_VIDRENDSINK
				else if (memcmp(GST_OBJECT_NAME(msg->src), "ismdgstvidpproc", 15) == 0)
				{
					_this->privateContext->video_pproc = (GstElement *) msg->src;
					logprintf("AAMPGstPlayer setting rectangle %s\n", _this->privateContext->videoRectangle);
					g_object_set(msg->src, "rectangle", _this->privateContext->videoRectangle, NULL);
					logprintf("AAMPGstPlayer setting zoom %d\n", _this->privateContext->zoom);
					g_object_set(msg->src, "scale-mode", (VIDEO_ZOOM_FULL == _this->privateContext->zoom) ? 0 : 3, NULL);
				}
#endif
			}
#endif
			/*This block is added to share the PrivateInstanceAAMP object
			  with PlayReadyDecryptor Plugin, for tune time profiling

			  AAMP is added as a property of playready plugin
			*/
			if(memcmp(GST_OBJECT_NAME(msg->src), GstPluginNamePR, strlen(GstPluginNamePR)) == 0 ||
			   memcmp(GST_OBJECT_NAME(msg->src), GstPluginNameWV, strlen(GstPluginNameWV)) == 0)
			{
				logprintf("AAMPGstPlayer setting aamp instance for %s decryptor\n", GST_OBJECT_NAME(msg->src));
				GValue val = { 0, };
				g_value_init(&val, G_TYPE_POINTER);
				g_value_set_pointer(&val, (gpointer) _this->aamp);
				g_object_set_property(G_OBJECT(msg->src), "aamp",&val);
			}
		}
		break;
#ifdef USE_GST1
	case GST_MESSAGE_NEED_CONTEXT:
		
		/*
		 * Code to avoid logs flooding with NEED-CONTEXT message for DRM systems
		 */
		const gchar* contextType;
		gst_message_parse_context_type(msg, &contextType);
		if (!g_strcmp0(contextType, "drm-preferred-decryption-system-id"))
		{
			logprintf("Setting %s as preferred drm\n",GetDrmSystemName((DRMSystems)gpGlobalConfig->preferredDrm));
			GstContext* context = gst_context_new("drm-preferred-decryption-system-id", FALSE);
			GstStructure* contextStructure = gst_context_writable_structure(context);
			gst_structure_set(contextStructure, "decryption-system-id", G_TYPE_STRING, GetDrmSystemID((DRMSystems)gpGlobalConfig->preferredDrm),  NULL);
			gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
		}

		break;
#endif

	default:
		break;
	}

	return GST_BUS_PASS;
}

#ifdef STANDALONE_AAMP

/**
 * @brief Thread to run mainloop (for standalone mode)
 * @param[in] arg user_data
 * @retval void pointer
 */
static void* AAMPGstPlayer_StreamThread(void *arg);
bool AAMPGstPlayer::initialized = false;
GThread *aampMainLoopThread = NULL;

/**
 * @brief To initialize Gstreamer and start mainloop (for standalone mode)
 * @param[in] argc number of arguments
 * @param[in] argv array of arguments
 */
void AAMPGstPlayer::Init(int argc, char **argv)
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
void AAMPGstPlayer::Term()
{
	if(AAMPGstPlayerMainLoop)
	{
		g_main_loop_quit(AAMPGstPlayerMainLoop);
		g_thread_join(aampMainLoopThread);
		gst_deinit ();
		logprintf("%s(): Exit\n", __FUNCTION__);
	}
}
#endif


/**
 * @brief Create a new Gstreamer pipeline
 */
bool AAMPGstPlayer::CreatePipeline()
{
	bool ret = false;
	logprintf("%s(): Creating gstreamer pipeline\n", __FUNCTION__);

	if (privateContext->pipeline || privateContext->bus)
	{
		DestroyPipeline();
	}

	privateContext->pipeline = gst_pipeline_new("AAMPGstPlayerPipeline");
	if (privateContext->pipeline)
	{
		privateContext->bus = gst_pipeline_get_bus(GST_PIPELINE(privateContext->pipeline));
		if (privateContext->bus)
		{
			privateContext->busWatchId = gst_bus_add_watch(privateContext->bus, (GstBusFunc) bus_message, this);
#ifdef USE_GST1
			gst_bus_set_sync_handler(privateContext->bus, (GstBusSyncHandler) bus_sync_handler, this, NULL);
#else
			gst_bus_set_sync_handler(privateContext->bus, (GstBusSyncHandler) bus_sync_handler, this);
#endif
			privateContext->buffering_enabled = gpGlobalConfig->gstreamerBufferingBeforePlay;
			privateContext->buffering_low_percent = DEFAULT_BUFFERING_LOW_PERCENT;
			privateContext->buffering_in_progress = false;
			privateContext->buffering_target_state = GST_STATE_NULL;
			privateContext->buffering_capable = false;
			logprintf("%s buffering_enabled %u, low percent %d\n", GST_ELEMENT_NAME(privateContext->pipeline),
					privateContext->buffering_enabled, privateContext->buffering_low_percent);

			ret = true;
		}
		else
		{
			logprintf("AAMPGstPlayer - gst_pipeline_get_bus failed\n");
		}
	}
	else
	{
		logprintf("AAMPGstPlayer - gst_pipeline_new failed\n");
	}

	return ret;
}


/**
 * @brief Cleanup an existing Gstreamer pipeline and associated resources
 */
void AAMPGstPlayer::DestroyPipeline()
{
	if (privateContext->pipeline)
	{
		gst_object_unref(privateContext->pipeline);
		privateContext->pipeline = NULL;
	}
	if (privateContext->busWatchId != 0)
	{
		g_source_remove(privateContext->busWatchId);
		privateContext->busWatchId = 0;
	}
	if (privateContext->bus)
	{
		gst_object_unref(privateContext->bus);
		privateContext->bus = NULL;
	}

    //video decoder handle will change with new pipeline
    privateContext->decoderHandleNotified = false;

	logprintf("%s(): Destroying gstreamer pipeline\n", __FUNCTION__);
}


/**
 * @brief Retrieve the video decoder handle from pipeline
 * @retval the decoder handle
 */
unsigned long AAMPGstPlayer::getCCDecoderHandle()
{
	gpointer dec_handle = NULL;
	if (this->privateContext->stream[eMEDIATYPE_VIDEO].using_playersinkbin && this->privateContext->stream[eMEDIATYPE_VIDEO].sinkbin != NULL)
	{
		logprintf("Querying playersinkbin for handle\n");
		g_object_get(this->privateContext->stream[eMEDIATYPE_VIDEO].sinkbin, "video-decode-handle", &dec_handle, NULL);
	}
	else if(this->privateContext->video_dec != NULL)
	{
		logprintf("Querying video decoder for handle\n");
#ifndef INTELCE
		g_object_get(this->privateContext->video_dec, "videodecoder", &dec_handle, NULL);
#else
		g_object_get(privateContext->video_dec, "decode-handle", &dec_handle, NULL);
#endif
	}
	logprintf("video decoder handle received %p\n", dec_handle);
	return (unsigned long)dec_handle;
}

/**
 * @brief Generate a protection event
 * @param[in] protSystemId keysystem to be used
 * @param[in] initData DRM initialization data
 * @param[in] initDataSize DRM initialization data size
 */
void AAMPGstPlayer::QueueProtectionEvent(const char *protSystemId, const void *initData, size_t initDataSize)
{
#ifdef AAMP_MPD_DRM
  	GstBuffer *pssi;

	logprintf("queueing protection event for keysystem: %s initdata size: %d\n", protSystemId, initDataSize);

	pssi = gst_buffer_new_wrapped(g_memdup (initData, initDataSize), initDataSize);
    
	privateContext->protectionEvent = gst_event_new_protection (protSystemId, pssi, "dash/mpd");

	gst_buffer_unref (pssi);
#endif
}

/**
 * @brief Cleanup generated protection event
 */
void AAMPGstPlayer::ClearProtectionEvent()
{
	if(privateContext->protectionEvent)
	{
		logprintf("%s removing protection event! \n", __FUNCTION__);
		gst_event_unref (privateContext->protectionEvent);
		privateContext->protectionEvent = NULL;
	}
}

/**
 * @brief Callback for receiving playersinkbin gstreamer events
 * @param[in] playersinkbin instance of playersinkbin
 * @param[in] status event name
 * @param[in] arg user data (pointer to AAMPGstPlayer instance)
 */
static void AAMPGstPlayer_PlayersinkbinCB(GstElement * playersinkbin, gint status,  void* arg)
{
	AAMPGstPlayer *_this = (AAMPGstPlayer *)arg;
	switch (status)
	{
		case GSTPLAYERSINKBIN_EVENT_HAVE_VIDEO:
			GST_INFO("got Video PES.\n");
			break;
		case GSTPLAYERSINKBIN_EVENT_HAVE_AUDIO:
			GST_INFO("got Audio PES\n");
			break;
		case GSTPLAYERSINKBIN_EVENT_FIRST_VIDEO_FRAME:
			GST_INFO("got First Video Frame\n");
			_this->NotifyFirstFrame(eMEDIATYPE_VIDEO);
			break;
		case GSTPLAYERSINKBIN_EVENT_FIRST_AUDIO_FRAME:
			GST_INFO("got First Audio Sample\n");
			_this->NotifyFirstFrame(eMEDIATYPE_AUDIO);
			break;
		case GSTPLAYERSINKBIN_EVENT_ERROR_VIDEO_UNDERFLOW:
			//TODO - Handle underflow
			logprintf("## %s() : Got Underflow message from video pipeline ##\n", __FUNCTION__);
			break;
		case GSTPLAYERSINKBIN_EVENT_ERROR_AUDIO_UNDERFLOW:
			//TODO - Handle underflow
			logprintf("## %s() : Got Underflow message from audio pipeline ##\n", __FUNCTION__);
			break;
		case GSTPLAYERSINKBIN_EVENT_ERROR_VIDEO_PTS:
			//TODO - Handle PTS error
			logprintf("## %s() : Got PTS error message from video pipeline ##\n", __FUNCTION__);
			break;
		case GSTPLAYERSINKBIN_EVENT_ERROR_AUDIO_PTS:
			//TODO - Handle PTS error
			logprintf("## %s() : Got PTS error message from audio pipeline ##\n", __FUNCTION__);
			break;
		default:
			GST_INFO("%s status = 0x%x (Unknown)\n", __FUNCTION__, status);
			break;
	}
}


/**
 * @brief Create an appsrc element for a particular format
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @param[in] format data format for setting src pad caps
 * @retval pointer to appsrc instance
 */
static GstElement* AAMPGstPlayer_GetAppSrc(AAMPGstPlayer *_this, StreamOutputFormat format)
{
	GstElement *source;
	GstCaps * caps;
	source = gst_element_factory_make("appsrc", NULL);
	if (NULL == source)
	{
		logprintf("AAMPGstPlayer_SetupStream Cannot create source\n");
		return NULL;
	}
	InitializeSource( _this, G_OBJECT(source) );

	caps = GetGstCaps(format);
	gst_app_src_set_caps(GST_APP_SRC(source), caps);
	gst_caps_unref(caps);
	return source;
}


/**
 * @brief Cleanup resources and flags for a particular stream type
 * @param[in] mediaType stream type
 */
void AAMPGstPlayer::TearDownStream(MediaType mediaType)
{
	media_stream* stream = &privateContext->stream[mediaType];
	stream->bufferUnderrun = false;
	stream->eosReached = false;
	if ((stream->format != FORMAT_INVALID) && (stream->format != FORMAT_NONE))
	{
		logprintf("AAMPGstPlayer::TearDownStream: mediaType %d \n", (int)mediaType);
		if (privateContext->pipeline)
		{
			/* set the playbin state to NULL before detach it */
			if (stream->sinkbin && (GST_STATE_CHANGE_FAILURE == gst_element_set_state(GST_ELEMENT(stream->sinkbin), GST_STATE_NULL)))
			{
				logprintf("AAMPGstPlayer::TearDownStream: Failed to set NULL state for sinkbin\n");
			}

			if (stream->sinkbin && (!gst_bin_remove(GST_BIN(privateContext->pipeline), GST_ELEMENT(stream->sinkbin))))
			{
				logprintf("AAMPGstPlayer::TearDownStream:  Unable to remove sinkbin from pipeline\n");
			}
			if (stream->using_playersinkbin)
			{
				if (!gst_bin_remove(GST_BIN(privateContext->pipeline), GST_ELEMENT(stream->source)))
				{
					logprintf("AAMPGstPlayer::TearDownStream:  Unable to remove source from pipeline\n");
				}
			}
		}
		stream->format = FORMAT_INVALID;
		stream->sinkbin = NULL;
		stream->source = NULL;
	}
	if (mediaType == eMEDIATYPE_VIDEO)
	{
		privateContext->video_dec = NULL;
#if !defined(INTELCE) || defined(INTELCE_USE_VIDRENDSINK)
		privateContext->video_sink = NULL;
#endif

#ifdef INTELCE_USE_VIDRENDSINK
		privateContext->video_pproc = NULL;
#endif
	}
	else if (mediaType == eMEDIATYPE_AUDIO)
	{
		privateContext->audio_sink = NULL;
	}
	logprintf("AAMPGstPlayer::TearDownStream:  exit mediaType = %d\n", mediaType);
}


/**
 * @brief Setup pipeline for a particular stream type
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @param[in] streamId stream type
 * @retval 0, if setup successfully. -1, for failure
 */
static int AAMPGstPlayer_SetupStream(AAMPGstPlayer *_this, int streamId)
{
	media_stream* stream = &_this->privateContext->stream[streamId];

	if (!stream->using_playersinkbin)
	{
#ifdef USE_GST1
		logprintf("AAMPGstPlayer_SetupStream - using playbin\n");
		stream->sinkbin = gst_element_factory_make("playbin", NULL);
		if (_this->privateContext->using_westerossink)
		{
			logprintf("AAMPGstPlayer_SetupStream - using westerossink\n");
			GstElement* vidsink = gst_element_factory_make("westerossink", NULL);
			g_object_set(stream->sinkbin, "video-sink", vidsink, NULL);
        }
#else
		logprintf("AAMPGstPlayer_SetupStream - using playbin2\n");
		stream->sinkbin = gst_element_factory_make("playbin2", NULL);
#endif
#if defined(INTELCE) && !defined(INTELCE_USE_VIDRENDSINK)
		if (eMEDIATYPE_VIDEO == streamId)
		{
			logprintf("%s:%d - using ismd_vidsink\n", __FUNCTION__, __LINE__);
			GstElement* vidsink = _this->privateContext->video_sink;
			if(NULL == vidsink)
			{
				vidsink = gst_element_factory_make("ismd_vidsink", NULL);
				if(!vidsink)
				{
					logprintf("%s:%d - Could not create ismd_vidsink element\n", __FUNCTION__, __LINE__);
				}
				else
				{
					_this->privateContext->video_sink = GST_ELEMENT(gst_object_ref( vidsink));
				}
			}
			else
			{
				logprintf("%s:%d Reusing existing vidsink element\n", __FUNCTION__, __LINE__);
			}
			logprintf("%s:%d Set video-sink %p to playbin %p\n", __FUNCTION__, __LINE__, vidsink, stream->sinkbin);
			g_object_set(stream->sinkbin, "video-sink", vidsink, NULL);
		}
#endif
		gst_bin_add(GST_BIN(_this->privateContext->pipeline), stream->sinkbin);
		gint flags;
		g_object_get(stream->sinkbin, "flags", &flags, NULL);
		logprintf("playbin flags1: 0x%x\n", flags); // 0x617 on settop
		flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_NATIVE_AUDIO | GST_PLAY_FLAG_NATIVE_VIDEO;
		g_object_set(stream->sinkbin, "flags", flags, NULL); // needed?
		g_object_set(stream->sinkbin, "uri", "appsrc://", NULL);
		g_signal_connect(stream->sinkbin, "deep-notify::source", G_CALLBACK(found_source), _this);
		gst_element_sync_state_with_parent(stream->sinkbin);
		_this->privateContext->gstPropsDirty = true;
	}
	else
	{
		stream->source = AAMPGstPlayer_GetAppSrc(_this,stream->format);
		gst_bin_add(GST_BIN(_this->privateContext->pipeline), stream->source);
		gst_element_sync_state_with_parent(stream->source);
		stream->sinkbin = gst_element_factory_make("playersinkbin", NULL);
		if (NULL == stream->sinkbin)
		{
			logprintf("AAMPGstPlayer_SetupStream Cannot create sink\n");
			return -1;
		}
		g_signal_connect(stream->sinkbin, "event-callback", G_CALLBACK(AAMPGstPlayer_PlayersinkbinCB), _this);
		gst_bin_add(GST_BIN(_this->privateContext->pipeline), stream->sinkbin);
		gst_element_link(stream->source, stream->sinkbin);
		gst_element_sync_state_with_parent(stream->sinkbin);

		logprintf("AAMPGstPlayer_SetupStream:  Created playersinkbin. Setting rectangle\n");
		g_object_set(stream->sinkbin, "rectangle",  _this->privateContext->videoRectangle, NULL);
		g_object_set(stream->sinkbin, "zoom", _this->privateContext->zoom, NULL);
		g_object_set(stream->sinkbin, "video-mute", _this->privateContext->videoMuted, NULL);
		g_object_set(stream->sinkbin, "volume", _this->privateContext->audioVolume, NULL);
		_this->privateContext->gstPropsDirty = false;
	}
	return 0;
}


/**
 * @brief Send any pending/cached events to pipeline
 * @param[in] privateContext pointer to AAMPGstPlayerPriv instance
 * @param[in] mediaType stream type
 * @param[in] pts PTS of next buffer
 */
static void AAMPGstPlayer_SendPendingEvents(PrivateInstanceAAMP *aamp, AAMPGstPlayerPriv *privateContext, MediaType mediaType, GstClockTime pts)
{
	media_stream* stream = &privateContext->stream[mediaType];
	GstPad* sourceEleSrcPad = gst_element_get_static_pad(GST_ELEMENT(stream->source), "src");
	if(stream->flush)
	{
		gboolean ret = gst_pad_push_event(sourceEleSrcPad, gst_event_new_flush_start());
		if (!ret) logprintf("%s: flush start error\n", __FUNCTION__);
#ifdef USE_GST1
		GstEvent* event = gst_event_new_flush_stop(FALSE);
#else
		GstEvent* event = gst_event_new_flush_stop();
#endif
		ret = gst_pad_push_event(sourceEleSrcPad, event);
		if (!ret) logprintf("%s: flush stop error\n", __FUNCTION__);
		stream->flush = false;
	}
#ifdef USE_GST1
	GstSegment segment;
	gst_segment_init(&segment, GST_FORMAT_TIME);
	segment.start = pts;
	segment.position = 0;
	segment.rate = 1.0;
#ifdef INTELCE
	segment.applied_rate = 1.0;
#else
	segment.applied_rate = privateContext->rate;
#endif
	logprintf("Sending segment event for mediaType[%d]. start %" G_GUINT64_FORMAT " stop %" G_GUINT64_FORMAT" rate %f applied_rate %f\n", mediaType, segment.start, segment.stop, segment.rate, segment.applied_rate);
	GstEvent* event = gst_event_new_segment (&segment);
#else
	GstEvent* event = gst_event_new_new_segment (FALSE, 1.0, GST_FORMAT_TIME, pts, GST_CLOCK_TIME_NONE, 0);
#endif
	if (!gst_pad_push_event(sourceEleSrcPad, event))
	{
		 logprintf("%s: gst_pad_push_event segment error\n", __FUNCTION__);
	}

	if (stream->format == FORMAT_ISO_BMFF)
	{
		gboolean enableOverride;
#ifdef INTELCE
		enableOverride = TRUE;
#else
		enableOverride = (privateContext->rate != 1.0);
#endif
		GstStructure * eventStruct = gst_structure_new("aamp_override", "enable", G_TYPE_BOOLEAN, enableOverride, "rate", G_TYPE_FLOAT, privateContext->rate,"aampplayer",G_TYPE_BOOLEAN,TRUE, NULL);
#ifdef INTELCE
		if ((privateContext->rate == 1.0))
		{
			guint64 basePTS = aamp->GetFirstPTS() * GST_SECOND;
			logprintf("%s: Set override event's basePTS [ %" G_GUINT64_FORMAT "]\n", __FUNCTION__, basePTS);
			gst_structure_set (eventStruct, "basePTS", G_TYPE_UINT64, basePTS, NULL);
		}
#endif
		if (!gst_pad_push_event(sourceEleSrcPad, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, eventStruct)))
		{
			logprintf("%s: Error on sending rate override event\n", __FUNCTION__);
		}

		if(privateContext->protectionEvent)
		{
			logprintf("%s pushing protection event! mediatype: %d\n", __FUNCTION__, mediaType);
			if (!gst_pad_push_event(sourceEleSrcPad, gst_event_ref(privateContext->protectionEvent)))
			{
				logprintf("%s push protection event failed!\n", __FUNCTION__);
			}
		}
	}
#ifdef INTELCE
	if (!gst_pad_push_event(sourceEleSrcPad, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, gst_structure_new("discard-segment-event-with-zero-start", "enable", G_TYPE_BOOLEAN, TRUE, NULL))))
	{
		logprintf("%s: Error on sending discard-segment-event-with-zero-start custom event\n", __FUNCTION__);
	}
#endif

	gst_object_unref(sourceEleSrcPad);
	stream->resetPosition = false;
	stream->flush = false;
}


/**
 * @brief Inject buffer of a stream type to its pipeline
 * @param[in] mediaType stream type
 * @param[in] ptr buffer pointer
 * @param[in] len0 length of buffer
 * @param[in] fpts PTS of buffer (in sec)
 * @param[in] fdts DTS of buffer (in sec)
 * @param[in] fDuration duration of buffer (in sec)
 */
void AAMPGstPlayer::Send(MediaType mediaType, const void *ptr, size_t len0, double fpts, double fdts, double fDuration)
{
#define MAX_BYTES_TO_SEND (128*1024)
	GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
	GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
	GstClockTime duration = (GstClockTime)(fDuration * 1000000000LL);
	gboolean discontinuity = FALSE;
	size_t maxBytes;
	GstFlowReturn ret;

	if (privateContext->stream[eMEDIATYPE_VIDEO].format == FORMAT_ISO_BMFF)
	{
		//For mpeg-dash, sent the entire fragment.
		maxBytes = len0;
	}
	else
	{
		//For Dash, if using playersinkbin, broadcom plugins has buffer size limitation.
		maxBytes = MAX_BYTES_TO_SEND;
	}
#ifdef TRACE_VID_PTS
	if (mediaType == eMEDIATYPE_VIDEO && privateContext->rate != 1.0)
	{
		logprintf("AAMPGstPlayer %s : rate %f fpts %f pts %llu pipeline->stream_time %lu ", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", privateContext->rate, fpts, (unsigned long long)pts, GST_PIPELINE(this->privateContext->pipeline)->stream_time);
		GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(this->privateContext->pipeline));
		if (clock)
		{
			GstClockTime curr = gst_clock_get_time(clock);
			logprintf("  clock time %lu diff %lu (%f sec)", curr, pts-curr, (float)(pts-curr)/GST_SECOND);
			gst_object_unref(clock);
		}
		logprintf("\n");
	}
#endif

#ifdef DUMP_STREAM
	static FILE* fp = NULL;
	if (!fp)
	{
		fp = fopen("AAMPGstPlayerdump.ts", "w");
	}
	fwrite(ptr, 1, len0, fp );
#endif
	if(privateContext->stream[mediaType].resetPosition)
	{
		AAMPGstPlayer_SendPendingEvents(aamp, privateContext, mediaType, pts);
		discontinuity = TRUE;
	}

	while (aamp->DownloadsAreEnabled())
	{
		size_t len = len0;
		if (len > maxBytes)
		{
			len = maxBytes;
		}
		GstBuffer *buffer = gst_buffer_new_and_alloc((guint)len);
		if (discontinuity )
		{
			GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
			discontinuity = FALSE;
		}
#ifdef USE_GST1
		GstMapInfo map;
		gst_buffer_map(buffer, &map, GST_MAP_WRITE);
		memcpy(map.data, ptr, len);
		gst_buffer_unmap(buffer, &map);
		GST_BUFFER_PTS(buffer) = pts;
		GST_BUFFER_DTS(buffer) = dts;
		//GST_BUFFER_DURATION(buffer) = duration;
#else
		memcpy(GST_BUFFER_DATA(buffer), ptr, len);
		GST_BUFFER_TIMESTAMP(buffer) = pts;
		GST_BUFFER_DURATION(buffer) = duration;
#endif
		ret = gst_app_src_push_buffer(GST_APP_SRC(privateContext->stream[mediaType].source), buffer);
		if (ret != GST_FLOW_OK)
		{
			logprintf("gst_app_src_push_buffer error: %d[%s] mediaType %d\n", ret, gst_flow_get_name (ret), (int)mediaType);
			assert(false);
		}
		else if (privateContext->stream[mediaType].bufferUnderrun)
		{
			privateContext->stream[mediaType].bufferUnderrun = false;
		}
		ptr = len + (unsigned char *)ptr;
		len0 -= len;
		if (len0 == 0)
		{
			break;
		}
	}
}


/**
 * @brief Inject buffer of a stream type to its pipeline
 * @param[in] mediaType stream type
 * @param[in] pBuffer buffer as GrowableBuffer pointer
 * @param[in] fpts PTS of buffer (in sec)
 * @param[in] fdts DTS of buffer (in sec)
 * @param[in] fDuration duration of buffer (in sec)
 */
void AAMPGstPlayer::Send(MediaType mediaType, GrowableBuffer* pBuffer, double fpts, double fdts, double fDuration)
{
	GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
	GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
	GstClockTime duration = (GstClockTime)(fDuration * 1000000000LL);
	gboolean discontinuity = FALSE;

#ifdef TRACE_VID_PTS
	if (mediaType == eMEDIATYPE_VIDEO && privateContext->rate != 1.0)
	{
		logprintf("AAMPGstPlayer %s : rate %f fpts %f pts %llu pipeline->stream_time %lu ", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", privateContext->rate, fpts, (unsigned long long)pts, GST_PIPELINE(this->privateContext->pipeline)->stream_time);
		GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(this->privateContext->pipeline));
		if (clock)
		{
			GstClockTime curr = gst_clock_get_time(clock);
			logprintf("  clock time %lu diff %lu (%f sec)", curr, pts-curr, (float)(pts-curr)/GST_SECOND);
			gst_object_unref(clock);
		}
		logprintf("\n");
	}
#endif

#ifdef DUMP_STREAM
	static FILE* fp = NULL;
	if (!fp)
	{
		fp = fopen("AAMPGstPlayerdump.ts", "w");
	}
	fwrite(pBuffer->ptr , 1, pBuffer->len, fp );
#endif
	if(privateContext->stream[mediaType].resetPosition)
	{
		AAMPGstPlayer_SendPendingEvents(aamp, privateContext, mediaType, pts);
		discontinuity = TRUE;
	}

#ifdef USE_GST1
	GstBuffer* buffer = gst_buffer_new_wrapped (pBuffer->ptr ,pBuffer->len);
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = dts;
#else
	GstBuffer* buffer = gst_buffer_new();
	GST_BUFFER_SIZE (buffer) = pBuffer->len;
	GST_BUFFER_MALLOCDATA (buffer) = (guint8*)pBuffer->ptr;
	GST_BUFFER_DATA (buffer) = GST_BUFFER_MALLOCDATA (buffer);
	GST_BUFFER_TIMESTAMP(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
#endif

	GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(privateContext->stream[mediaType].source), buffer);
	if (ret != GST_FLOW_OK)
	{
		logprintf("gst_app_src_push_buffer error: %d[%s] mediaType %d\n", ret, gst_flow_get_name (ret), (int)mediaType);
		assert(false);
	}
	else if (privateContext->stream[mediaType].bufferUnderrun)
	{
		privateContext->stream[mediaType].bufferUnderrun = false;
	}

	/*Since ownership of buffer is given to gstreamer, reset pBuffer */
	memset(pBuffer, 0x00, sizeof(GrowableBuffer));
}

#ifdef STANDALONE_AAMP

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
		logprintf("AAMPGstPlayer_StreamThread: exited main event loop\n");
	}
	g_main_loop_unref(AAMPGstPlayerMainLoop);
	AAMPGstPlayerMainLoop = NULL;
	return NULL;
}
#endif



/**
 * @brief To start playback
 */
void AAMPGstPlayer::Stream()
{
}


/**
 * @brief Configure pipeline based on A/V formats
 * @param[in] format video format
 * @param[in] audioFormat audio format
 */
void AAMPGstPlayer::Configure(StreamOutputFormat format, StreamOutputFormat audioFormat, bool bESChangeStatus)
{
	logprintf("AAMPGstPlayer::%s %d > format %d audioFormat %d\n", __FUNCTION__, __LINE__, format, audioFormat);
	StreamOutputFormat newFormat[AAMP_TRACK_COUNT];
	newFormat[eMEDIATYPE_VIDEO] = format;
	newFormat[eMEDIATYPE_AUDIO] = audioFormat;

#ifdef AAMP_STOP_SINK_ON_SEEK
	privateContext->rate = aamp->rate;
#endif

	if (privateContext->pipeline == NULL || privateContext->bus == NULL)
	{
		CreatePipeline();
	}

	bool configureStream = false;

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		media_stream *stream = &privateContext->stream[i];
		if (stream->format != newFormat[i])
		{
			if ((newFormat[i] != FORMAT_INVALID) && (newFormat[i] != FORMAT_NONE))
			{
				logprintf("AAMPGstPlayer::%s %d > Closing stream %d old format = %d, new format = %d\n",
								__FUNCTION__, __LINE__, i, stream->format, newFormat[i]);
				configureStream = true;
			}
		}

		/* Force configure the bin for mid stream audio type change */
		if (!configureStream && bESChangeStatus && (eMEDIATYPE_AUDIO == i))
		{
			logprintf("AAMPGstPlayer::%s %d > AudioType Changed. Force configure pipeline\n", __FUNCTION__, __LINE__);
			configureStream = true;
		}

		stream->resetPosition = true;
		stream->eosReached = false;
	}

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		media_stream *stream = &privateContext->stream[i];
		if (configureStream && (newFormat[i] != FORMAT_INVALID) && (newFormat[i] != FORMAT_NONE))
		{
			TearDownStream((MediaType) i);
			stream->format = newFormat[i];
	#ifdef USE_PLAYERSINKBIN
			if (FORMAT_MPEGTS == stream->format )
			{
				logprintf("AAMPGstPlayer::%s %d > - using playersinkbin, track = %d\n", __FUNCTION__, __LINE__, i);
				stream->using_playersinkbin = TRUE;
			}
			else
	#endif
			{
				stream->using_playersinkbin = FALSE;
			}
			if (0 != AAMPGstPlayer_SetupStream(this, (MediaType) i))
			{
				logprintf("AAMPGstPlayer::%s %d > track %d failed\n", __FUNCTION__, __LINE__, i);
				return;
			}
		}
	}
	if(aamp->IsFragmentBufferingRequired())
	{
		if (gst_element_set_state(this->privateContext->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
		{
			logprintf("AAMPGstPlayer::%s %d > GST_STATE_PAUSED failed\n", __FUNCTION__, __LINE__);
		}
		privateContext->pendingPlayState = true;
	}
	else
	{
		if (this->privateContext->buffering_enabled)
		{
			if (gst_element_set_state(this->privateContext->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
			{
				logprintf("AAMPGstPlayer_Configure GST_STATE_PLAYING failed\n");
			}
			this->privateContext->buffering_target_state = GST_STATE_PLAYING;
			privateContext->pendingPlayState = false;
		}
		else
		{
			if (gst_element_set_state(this->privateContext->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
			{
				logprintf("AAMPGstPlayer::%s %d > GST_STATE_PLAYING failed\n", __FUNCTION__, __LINE__);
			}
			privateContext->pendingPlayState = false;
		}
	}
	privateContext->eosSignalled = false;
#ifdef TRACE
	logprintf("exiting AAMPGstPlayer::%s\n", __FUNCTION__);
#endif
}


/**
 * @brief To signal EOS to a particular appsrc instance
 * @param[in] source pointer to appsrc instance
 */
static void AAMPGstPlayer_SignalEOS(GstElement *source )
{
	if (source )
	{
		GstFlowReturn ret;
		g_signal_emit_by_name(source, "end-of-stream", &ret);
		if (ret != GST_FLOW_OK)
		{
			logprintf("gst_app_src_push_buffer  error: %d", ret);
		}
	}
}


/**
 * @brief Starts processing EOS for a particular stream type
 * @param[in] type stream type
 */
void AAMPGstPlayer::EndOfStreamReached(MediaType type)
{
	logprintf("entering AAMPGstPlayer_EndOfStreamReached type %d\n", (int)type);

	media_stream *stream = &privateContext->stream[type];
	stream->eosReached = true;
	if (stream->format != FORMAT_NONE && stream->resetPosition == true && stream->flush == true)
	{
		logprintf("%s(): EOS received as first buffer \n", __FUNCTION__);
		NotifyEOS();
	}
	else
	{
		NotifyFragmentCachingComplete();
		AAMPGstPlayer_SignalEOS(stream->source);
		/*For trickmodes, give EOS to audio source*/
		if (1.0 != privateContext->rate)
		{
			AAMPGstPlayer_SignalEOS(privateContext->stream[eMEDIATYPE_AUDIO].source);
		}
	}
}


/**
 * @brief Stop playback and any idle handlers active at the time
 * @param[in] keepLastFrame denotes if last video frame should be kept
 */
void AAMPGstPlayer::Stop(bool keepLastFrame)
{
	logprintf("entering AAMPGstPlayer_Stop keepLastFrame %d\n", keepLastFrame);
#ifdef INTELCE
	if (privateContext->video_sink)
	{
		privateContext->keepLastFrame = keepLastFrame;
		g_object_set(privateContext->video_sink,  "stop-keep-frame", keepLastFrame, NULL);
#if !defined(INTELCE_USE_VIDRENDSINK)
		if  (!keepLastFrame)
		{
			gst_object_unref(privateContext->video_sink);
			privateContext->video_sink = NULL;
		}
		else
		{
			g_object_set(privateContext->video_sink,  "reuse-vidrend", keepLastFrame, NULL);
		}
#endif
	}
#endif
	if(!keepLastFrame)
	{
		privateContext->firstFrameReceived = false;
	}
#ifdef USE_IDLE_LOOP_FOR_PROGRESS_REPORTING
	if (privateContext->firstProgressCallbackIdleTaskPending)
	{
		logprintf("AAMPGstPlayer::%s %d > Remove firstProgressCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->firstProgressCallbackIdleTaskId);
		g_source_remove(privateContext->firstProgressCallbackIdleTaskId);
		privateContext->firstProgressCallbackIdleTaskPending = false;
		privateContext->firstProgressCallbackIdleTaskId = 0;
	}
	if (this->privateContext->periodicProgressCallbackIdleTaskId)
	{
		logprintf("AAMPGstPlayer::%s %d > Remove periodicProgressCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->periodicProgressCallbackIdleTaskId);
		g_source_remove(privateContext->periodicProgressCallbackIdleTaskId);
		privateContext->periodicProgressCallbackIdleTaskId = 0;
	}
#endif
	if (this->privateContext->eosCallbackIdleTaskPending)
	{
		logprintf("AAMPGstPlayer::%s %d > Remove eosCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->eosCallbackIdleTaskId);
		g_source_remove(privateContext->eosCallbackIdleTaskId);
		privateContext->eosCallbackIdleTaskPending = false;
		privateContext->eosCallbackIdleTaskId = 0;
	}
	if (this->privateContext->firstFrameCallbackIdleTaskPending)
	{
		logprintf("AAMPGstPlayer::%s %d > Remove firstFrameCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->firstFrameCallbackIdleTaskId);
		g_source_remove(privateContext->firstFrameCallbackIdleTaskId);
		privateContext->firstFrameCallbackIdleTaskPending = false;
		privateContext->firstFrameCallbackIdleTaskId = 0;
	}
	if (this->privateContext->pipeline)
	{
		GstState current;
		GstState pending;
		if(GST_STATE_CHANGE_FAILURE == gst_element_get_state(privateContext->pipeline, &current, &pending, 0))
		{
			logprintf("AAMPGstPlayer::%s: Pipeline is in FAILURE state : current %s  pending %s\n", __FUNCTION__,gst_element_state_get_name(current), gst_element_state_get_name(pending));
		}
		gst_element_set_state(this->privateContext->pipeline, GST_STATE_NULL);
		logprintf("AAMPGstPlayer::%s: Pipeline state set to null\n", __FUNCTION__);
	}
#ifdef AAMP_MPD_DRM
	if(AampOutputProtection::IsAampOutputProcectionInstanceActive())
	{
		AampOutputProtection *pInstance = AampOutputProtection::GetAampOutputProcectionInstance();
		pInstance->setGstElement((GstElement *)(NULL));
		pInstance->Release();
	}
#endif
	TearDownStream(eMEDIATYPE_VIDEO);
	TearDownStream(eMEDIATYPE_AUDIO);
	DestroyPipeline();
	privateContext->rate = 1.0;
	logprintf("exiting AAMPGstPlayer_Stop\n");
}


/**
 * @brief Log the various info related to playback
 */
void AAMPGstPlayer::DumpStatus(void)
{
	GstElement *source = this->privateContext->stream[eMEDIATYPE_VIDEO].source;
	gboolean rcBool;
	guint64 rcUint64;
	gint64 rcInt64;
	GstFormat rcFormat;

	//https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gst-plugins-base-plugins/html/gst-plugins-base-plugins-appsrc.html
	
	rcBool = 0;
	g_object_get(source, "block", &rcBool, NULL);
	logprintf("\tblock=%d\n", (int)rcBool); // 0

	rcBool = 0;
	g_object_get(source, "emit-signals", &rcBool, NULL);
	logprintf("\temit-signals=%d\n", (int)rcBool); // 1

	rcFormat = (GstFormat)0;
	g_object_get(source, "format", &rcFormat, NULL);
	logprintf("\tformat=%d\n", (int)rcFormat); // 2
	
	rcBool = 0;
	g_object_get(source, "is-live", &rcBool, NULL);
	logprintf("\tis-live=%d\n", (int)rcBool); // 0
	
	rcUint64 = 0;
	g_object_get(source, "max-bytes", &rcUint64, NULL);
	logprintf("\tmax-bytes=%d\n", (int)rcUint64); // 200000
	
	rcInt64 = 0;
	g_object_get(source, "max-latency", &rcInt64, NULL);
	logprintf("\tmax-latency=%d\n", (int)rcInt64); // -1

	rcInt64 = 0;
	g_object_get(source, "min-latency", &rcInt64, NULL);
	logprintf("\tmin-latency=%d\n", (int)rcInt64); // -1

	rcInt64 = 0;
	g_object_get(source, "size", &rcInt64, NULL);
	logprintf("\tsize=%d\n", (int)rcInt64); // -1

	gint64 pos, len;
	GstFormat format = GST_FORMAT_TIME;
#ifdef USE_GST1
	if (gst_element_query_position(privateContext->pipeline, format, &pos) &&
		gst_element_query_duration(privateContext->pipeline, format, &len))
#else
	if (gst_element_query_position(privateContext->pipeline, &format, &pos) &&
		gst_element_query_duration(privateContext->pipeline, &format, &len))
#endif
	{
		logprintf("Position: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT "\r",
			GST_TIME_ARGS(pos), GST_TIME_ARGS(len));
	}
}


/**
 * @brief Validate pipeline state transition within a max timeout
 * @param[in] _this pointer to AAMPGstPlayer instance
 * @param[in] stateToValidate state to be validated
 * @param[in] msTimeOut max timeout in MS
 * @retval Current pipeline state
 */
static GstState validateStateWithMsTimeout( AAMPGstPlayer *_this, GstState stateToValidate, guint msTimeOut)
{
	GstState gst_current;
	GstState gst_pending;
	float timeout = 100.0;
	gint gstGetStateCnt = GST_ELEMENT_GET_STATE_RETRY_CNT_MAX;

	do
	{
		if ((GST_STATE_CHANGE_SUCCESS
				== gst_element_get_state(_this->privateContext->pipeline, &gst_current, &gst_pending, timeout * GST_MSECOND))
				&& (gst_current == stateToValidate))
		{
			GST_WARNING(
					"validateStateWithMsTimeout - PIPELINE gst_element_get_state - SUCCESS : State = %d, Pending = %d",
					gst_current, gst_pending);
			return gst_current;
		}
		g_usleep (msTimeOut * 1000); // Let pipeline safely transition to required state
	}
	while ((gst_current != stateToValidate) && (gstGetStateCnt-- != 0));

	logprintf("validateStateWithMsTimeout - PIPELINE gst_element_get_state - FAILURE : State = %d, Pending = %d\n",
			gst_current, gst_pending);
	return gst_current;
}


/**
 * @brief Flush the buffers in pipeline
 */
void AAMPGstPlayer::Flush(void)
{
	if (privateContext->pipeline)
	{
		PauseAndFlush(false);
	}
}


/**
 * @brief Pause pipeline and flush 
 * @param playAfterFlush denotes if it should be set to playing at the end
 */
void AAMPGstPlayer::PauseAndFlush(bool playAfterFlush)
{
	aamp->SyncBegin();
	logprintf("Entering AAMPGstPlayer::PauseAndFlush() pipeline state %s\n",
			gst_element_state_get_name(GST_STATE(privateContext->pipeline)));
	GstStateChangeReturn rc;
	GstState stateBeforeFlush = GST_STATE_PAUSED;
#ifndef USE_PLAYERSINKBIN
	/*On pc, tsdemux requires null transition*/
	stateBeforeFlush = GST_STATE_NULL;
#endif
	rc = gst_element_set_state(this->privateContext->pipeline, stateBeforeFlush);
	if (GST_STATE_CHANGE_ASYNC == rc)
	{
		if (GST_STATE_PAUSED != validateStateWithMsTimeout(this,GST_STATE_PAUSED, 50))
		{
			logprintf("AAMPGstPlayer_Flush - validateStateWithMsTimeout - FAILED GstState %d\n", GST_STATE_PAUSED);
		}
	}
	else if (GST_STATE_CHANGE_SUCCESS != rc)
	{
		logprintf("AAMPGstPlayer_Flush - gst_element_set_state - FAILED rc %d\n", rc);
	}
#ifdef USE_GST1
	gboolean ret = gst_element_send_event( GST_ELEMENT(privateContext->pipeline), gst_event_new_flush_start());
	if (!ret) logprintf("AAMPGstPlayer_Flush: flush start error\n");
	ret = gst_element_send_event(GST_ELEMENT(privateContext->pipeline), gst_event_new_flush_stop(TRUE));
	if (!ret) logprintf("AAMPGstPlayer_Flush: flush stop error\n");
#else
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		media_stream *stream = &this->privateContext->stream[iTrack];
		if (stream->source)
		{
			GstPad* sourceEleSrcPad = gst_element_get_static_pad(stream->source, "src");
			gboolean ret = gst_pad_push_event(sourceEleSrcPad, gst_event_new_flush_start());
			if (!ret) logprintf("AAMPGstPlayer_Flush: flush start error\n");

			ret = gst_pad_push_event(sourceEleSrcPad, gst_event_new_flush_stop());
			if (!ret) logprintf("AAMPGstPlayer_Flush: flush stop error\n");

			gst_object_unref(sourceEleSrcPad);
		}
	}
#endif
	if (playAfterFlush)
	{
		rc = gst_element_set_state(this->privateContext->pipeline, GST_STATE_PLAYING);

		if (GST_STATE_CHANGE_ASYNC == rc)
		{
#ifdef AAMP_WAIT_FOR_PLAYING_STATE
			if (GST_STATE_PLAYING != validateStateWithMsTimeout( GST_STATE_PLAYING, 50))
			{
				logprintf("AAMPGstPlayer_Flush - validateStateWithMsTimeout - FAILED GstState %d\n",
						GST_STATE_PLAYING);
			}
#endif
		}
		else if (GST_STATE_CHANGE_SUCCESS != rc)
		{
			logprintf("AAMPGstPlayer_Flush - gst_element_set_state - FAILED rc %d\n", rc);
		}
	}
	this->privateContext->total_bytes = 0;
	privateContext->pendingPlayState = false;
	//privateContext->total_duration = 0;
	logprintf("exiting AAMPGstPlayer_FlushEvent\n");
	aamp->SyncEnd();
}


/**
 * @brief Select a particular audio track (for playbin)
 * @param[in] index index of audio track to be selected
 */
void AAMPGstPlayer::SelectAudio(int index)
{
#ifdef SUPPORT_MULTI_AUDIO
	if (index >= 0 && index < appContext->n_audio)
	{
		privateContext->current_audio = index;
		g_object_set(privateContext->stream[eMEDIATYPE_VIDEO].sinkbin, "current-audio", privateContext->current_audio, NULL);
	}
#endif
}


/**
 * @brief Get playback position in MS
 * @retval playback position in MS
 */
long AAMPGstPlayer::GetPositionMilliseconds(void)
{
	long rc = 0;
	gint64 pos, len;
	GstFormat format = GST_FORMAT_TIME;
	if (privateContext->pipeline == NULL)
	{
		logprintf("%s(): Pipeline is NULL\n", __FUNCTION__);
		return rc;
	}
#ifdef USE_GST1
	if (gst_element_query_position(privateContext->pipeline, format, &pos) &&
		gst_element_query_duration(privateContext->pipeline, format, &len))
#else
	if (gst_element_query_position(privateContext->pipeline, &format, &pos) &&
		gst_element_query_duration(privateContext->pipeline, &format, &len))
#endif
	{
		rc = pos / 1e6;
	}
	return rc;
}


/**
 * @brief To pause/play pipeline
 * @param[in] pause flag to pause/play the pipeline
 */
void AAMPGstPlayer::Pause( bool pause )
{
	aamp->SyncBegin();
	logprintf("entering AAMPGstPlayer_Pause\n");
	if (privateContext->pipeline == NULL)
	{
		logprintf("%s(): Pipeline is NULL\n", __FUNCTION__);
		return;
	}
	GstState nextState = pause ? GST_STATE_PAUSED : GST_STATE_PLAYING;
	gst_element_set_state(this->privateContext->pipeline, nextState);
	privateContext->buffering_target_state = nextState;

#if 0
	GstStateChangeReturn rc;
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		media_stream *stream = &privateContext->stream[iTrack];
		if (stream->source)
		{
			rc = gst_element_set_state(privateContext->stream->sinkbin, GST_STATE_PAUSED);
		}
	}
#endif
	aamp->SyncEnd();
}


/**
 * @brief Set video display rectangle co-ordinates
 * @param[in] x x co-ordinate of display rectangle
 * @param[in] y y co-ordinate of display rectangle
 * @param[in] w width of display rectangle
 * @param[in] h height of display rectangle
 */
void AAMPGstPlayer::SetVideoRectangle(int x, int y, int w, int h)
{
	media_stream *stream = &privateContext->stream[eMEDIATYPE_VIDEO];
	sprintf(privateContext->videoRectangle, "%d,%d,%d,%d", x,y,w,h);
	logprintf("SetVideoRectangle :: Rect %s, using_playersinkbin = %d, video_sink =%p\n",
			privateContext->videoRectangle, stream->using_playersinkbin, privateContext->video_sink);
	if (stream->using_playersinkbin)
	{
		g_object_set(stream->sinkbin, "rectangle", privateContext->videoRectangle, NULL);
	}
#ifndef INTELCE
	else if (privateContext->video_sink)
	{
		g_object_set(privateContext->video_sink, "rectangle", privateContext->videoRectangle, NULL);
	}
#else
#if defined(INTELCE_USE_VIDRENDSINK)
	else if (privateContext->video_pproc)
	{
		g_object_set(privateContext->video_pproc, "rectangle", privateContext->videoRectangle, NULL);
	}
#else
	else if (privateContext->video_sink)
	{
		g_object_set(privateContext->video_sink, "rectangle", privateContext->videoRectangle, NULL);
	}
#endif
#endif
	else
	{
		logprintf("SetVideoRectangle :: Scaling not possible at this time\n");
		privateContext->gstPropsDirty = true;
	}
}


/**
 * @brief Set video zoom
 * @param[in] zoom zoom setting to be set
 */
void AAMPGstPlayer::SetVideoZoom(VideoZoomMode zoom)
{
	media_stream *stream = &privateContext->stream[eMEDIATYPE_VIDEO];
	AAMPLOG_INFO("SetVideoZoom :: ZoomMode %d, using_playersinkbin = %d, video_sink =%p\n",
			zoom, stream->using_playersinkbin, privateContext->video_sink);

	privateContext->zoom = zoom;
	if (stream->using_playersinkbin && stream->sinkbin)
	{
		g_object_set(stream->sinkbin, "zoom", zoom, NULL);
	}
#ifndef INTELCE
	else if (privateContext->video_sink)
	{
		g_object_set(privateContext->video_sink, "zoom-mode", VIDEO_ZOOM_FULL == zoom ? 0 : 1, NULL);
	}
#elif defined(INTELCE_USE_VIDRENDSINK)
	else if (privateContext->video_pproc)
	{
		g_object_set(privateContext->video_pproc, "scale-mode", VIDEO_ZOOM_FULL == zoom ? 0 : 3, NULL);
	}
#else
	else if (privateContext->video_sink)
	{
		g_object_set(privateContext->video_sink, "scale-mode", VIDEO_ZOOM_FULL == zoom ? 0 : 3, NULL);
	}
#endif
	else
	{
		privateContext->gstPropsDirty = true;
	}
}


/**
 * @brief Set video mute
 * @param[in] muted true to mute video otherwise false
 */
void AAMPGstPlayer::SetVideoMute(bool muted)
{
	media_stream *stream = &privateContext->stream[eMEDIATYPE_VIDEO];
	AAMPLOG_INFO("%s: muted %d, using_playersinkbin = %d, video_sink =%p\n", __FUNCTION__, muted, stream->using_playersinkbin, privateContext->video_sink);

	privateContext->videoMuted = muted;
	if (stream->using_playersinkbin && stream->sinkbin)
	{
		g_object_set(stream->sinkbin, "video-mute", privateContext->videoMuted, NULL);
	}
	else if (privateContext->video_sink)
	{
#ifndef INTELCE
		g_object_set(privateContext->video_sink, "show-video-window", !privateContext->videoMuted, NULL);
#else
		g_object_set(privateContext->video_sink, "mute", privateContext->videoMuted, NULL);
#endif
	}
	else
	{
		privateContext->gstPropsDirty = true;
	}
}


/**
 * @brief Set audio volume
 * @param[in] volume audio volume value (0-100)
 */
void AAMPGstPlayer::SetAudioVolume(int volume)
{
	privateContext->audioVolume = volume / 100.0;

	setVolumeOrMuteUnMute();
}


/**
 * @brief Set audio volume or mute
 * @note set privateContext->audioVolume before calling this function
 */
void AAMPGstPlayer::setVolumeOrMuteUnMute(void)
{
	GstElement *gSource = NULL;
	char *propertyName = NULL;
	media_stream *stream = &privateContext->stream[eMEDIATYPE_VIDEO];

	AAMPLOG_INFO("AAMPGstPlayer::%s() %d > volume = %f, using_playersinkbin = %d, audio_sink = %p\n", __FUNCTION__, __LINE__, privateContext->audioVolume, stream->using_playersinkbin, privateContext->audio_sink);

	if (stream->using_playersinkbin && stream->sinkbin)
	{
		gSource = stream->sinkbin;
		propertyName = (char*)"audio-mute";
	}
	else if (privateContext->audio_sink)
	{
		gSource = privateContext->audio_sink;
		propertyName = (char*)"mute";
	}
	else
	{
		privateContext->gstPropsDirty = true;
		return; /* Return here if the sinkbin or audio_sink is not valid, no need to proceed further */
	}

	/* Muting the audio decoder in general to avoid audio passthrough in expert mode for locked channel */
	if (0 == privateContext->audioVolume)
	{
		logprintf("AAMPGstPlayer::%s() %d > Audio Muted\n", __FUNCTION__, __LINE__);
#ifdef INTELCE
		if (!stream->using_playersinkbin)
		{
			logprintf("AAMPGstPlayer::%s() %d > Setting input-gain to %f\n", __FUNCTION__, __LINE__, INPUT_GAIN_DB_MUTE);
			g_object_set(privateContext->audio_sink, "input-gain", INPUT_GAIN_DB_MUTE, NULL);
		}
		else
#endif
		{
			g_object_set(gSource, propertyName, true, NULL);
		}
		privateContext->audioMuted = true;
	}
	else
	{
		if (privateContext->audioMuted)
		{
			logprintf("AAMPGstPlayer::%s() %d > Audio Unmuted after a Mute\n", __FUNCTION__, __LINE__);
#ifdef INTELCE
			if (!stream->using_playersinkbin)
			{
				logprintf("AAMPGstPlayer::%s() %d > Setting input-gain to %f\n", __FUNCTION__, __LINE__, INPUT_GAIN_DB_UNMUTE);
				g_object_set(privateContext->audio_sink, "input-gain", INPUT_GAIN_DB_UNMUTE, NULL);
			}
			else
#endif
			{
				g_object_set(gSource, propertyName, false, NULL);
			}
			privateContext->audioMuted = false;
		}
		
		logprintf("AAMPGstPlayer::%s %d > Setting Volume %f\n",	__FUNCTION__, __LINE__, privateContext->audioVolume);
		g_object_set(gSource, "volume", privateContext->audioVolume, NULL);
	}
}


/**
 * @brief Flush cached GstBuffers and set seek position & rate
 * @param[in] position playback seek position
 * @param[in] rate playback rate
 */
void AAMPGstPlayer::Flush(double position, float rate)
{
	media_stream *stream = &privateContext->stream[eMEDIATYPE_VIDEO];
	privateContext->rate = rate;
	privateContext->stream[eMEDIATYPE_VIDEO].bufferUnderrun = false;
	privateContext->stream[eMEDIATYPE_AUDIO].bufferUnderrun = false;

	if (privateContext->eosCallbackIdleTaskPending)
	{
		logprintf("AAMPGstPlayer::%s %d > Remove eosCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->eosCallbackIdleTaskId);
		g_source_remove(privateContext->eosCallbackIdleTaskId);
		privateContext->eosCallbackIdleTaskId = 0;
		privateContext->eosCallbackIdleTaskPending = false;
	}

	if (stream->using_playersinkbin)
	{
		Flush();
	}
	else
	{
		if (privateContext->pipeline == NULL)
		{
			logprintf("%s(): Pipeline is NULL\n", __FUNCTION__);
			return;
		}

		//Check if pipeline is in playing/paused state. If not flush doesn't work
		GstState current, pending;
		gst_element_get_state(privateContext->pipeline, &current, &pending, 100 * GST_MSECOND);
		if (current != GST_STATE_PLAYING && current != GST_STATE_PAUSED)
		{
			logprintf("%s(): Pipeline is not in playing/paused state, hence resetting it\n", __FUNCTION__);
			Stop(true);
			return;
		}
		else
		{
			logprintf("%s(): Pipeline is in %s state position %f\n", __FUNCTION__, gst_element_state_get_name(current), position);
		}
		for (int i = 0; i < AAMP_TRACK_COUNT; i++)
		{
			privateContext->stream[i].resetPosition = true;
			privateContext->stream[i].flush = true;
		}
		AAMPLOG_INFO("TestStreamer::%s - pipeline flush seek - start = %f\n", __FUNCTION__, position);
		if (!gst_element_seek(privateContext->pipeline, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
				position * GST_SECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE))
		{
			logprintf("Seek failed\n");
		}
	}
	privateContext->eosSignalled = false;
}


/**
 * @brief Process discontinuity for a stream type
 * @param type stream type
 * @retval true if discontinuity processed
 */
bool AAMPGstPlayer::Discontinuity(MediaType type)
{
	bool ret = false;
	logprintf("Entering AAMPGstPlayer::%s type %d\n", __FUNCTION__, (int)type);
	media_stream *stream = &privateContext->stream[type];
	/*Handle discontinuity only if atleast one buffer is pushed*/
	if (stream->format != FORMAT_NONE && stream->resetPosition == true)
	{
		logprintf("%s(): Discontinuity received before first buffer - ignoring\n", __FUNCTION__);
	}
	else
	{
		traceprintf("%s(): stream->format %d, stream->resetPosition %d, stream->flush %d\n", __FUNCTION__,stream->format , stream->resetPosition, stream->flush);
		AAMPGstPlayer_SignalEOS(stream->source);
		ret = true;
	}
	return ret;
}


/**
 * @brief Check if cache empty for a media type
 * @param[in] mediaType stream type
 * @retval true if cache empty
 */
bool AAMPGstPlayer::IsCacheEmpty(MediaType mediaType)
{
	int ret = true;
#ifdef USE_GST1
	media_stream *stream = &privateContext->stream[mediaType];
	if (stream->source)
	{
		guint64 cacheLevel = gst_app_src_get_current_level_bytes (GST_APP_SRC(stream->source));
		if(0 == cacheLevel)
		{
			// Changed from logprintf to traceprintf, to avoid log flooding (seen on xi3 and xid).
			// We're seeing this logged frequently during live linear playback, despite no user-facing problem.
			traceprintf("AAMPGstPlayer::%s():%d Cache level empty\n", __FUNCTION__, __LINE__);

			if (stream->bufferUnderrun == true)
			{
				logprintf("AAMPGstPlayer::%s():%d Stream(%d) had received buffer underrun signal previously\n", __FUNCTION__, __LINE__, mediaType);
				return true;
			}
		}
		else
		{
			traceprintf("AAMPGstPlayer::%s():%d Cache level  %" G_GUINT64_FORMAT "\n", __FUNCTION__, __LINE__, cacheLevel);
			ret = false;
		}
	}
#endif
	return ret;
}

/**
 * @brief Set pipeline to PLAYING state once fragment caching is complete
 */
void AAMPGstPlayer::NotifyFragmentCachingComplete()
{
	if(privateContext->pendingPlayState)
	{
		logprintf("AAMPGstPlayer::%s():%d Setting pipeline to PLAYING state \n", __FUNCTION__, __LINE__);
		if (gst_element_set_state(privateContext->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		{
			logprintf("AAMPGstPlayer_Configure GST_STATE_PLAYING failed\n");
		}
		privateContext->pendingPlayState = false;
	}
	else
	{
		logprintf("AAMPGstPlayer::%s():%d No pending PLAYING state\n", __FUNCTION__, __LINE__);
	}
}


/**
 * @brief Get video display's width and height
 * @param[in] width video width
 * @param[in] height video height
 */
void AAMPGstPlayer::GetVideoSize(int &width, int &height)
{
	int x, y, w, h;
	sscanf(privateContext->videoRectangle, "%d,%d,%d,%d", &x, &y, &w, &h);
	if (w > 0 && h > 0)
	{
		width = w;
		height = h;
	}
}


/**
 * @brief Increase the rank of AAMP decryptor plugins
 */
void AAMPGstPlayer::InitializeAAMPGstreamerPlugins()
{
#ifdef AAMP_MPD_DRM
	GstRegistry* registry = gst_registry_get();

	GstPluginFeature* pluginFeature = gst_registry_lookup_feature(registry, GstPluginNamePR);

	if (pluginFeature == NULL)
	{
		logprintf("AAMPGstPlayer::%s():%d %s plugin feature not available; reloading aamp plugin\n", __FUNCTION__, __LINE__, GstPluginNamePR);
		GstPlugin * plugin = gst_plugin_load_by_name ("aamp");
		if(plugin)
		{
			gst_object_unref(plugin);
		}
		pluginFeature = gst_registry_lookup_feature(registry, GstPluginNamePR);
		if(pluginFeature == NULL)
			logprintf("AAMPGstPlayer::%s():%d %s plugin feature not available\n", __FUNCTION__, __LINE__, GstPluginNamePR);
	}
	if(pluginFeature)
	{
		gst_registry_remove_feature (registry, pluginFeature);//Added as a work around to handle DELIA-31716
		gst_registry_add_feature (registry, pluginFeature);


		logprintf("AAMPGstPlayer::%s():%d %s plugin priority set to GST_RANK_PRIMARY + 111\n", __FUNCTION__, __LINE__, GstPluginNamePR);
		gst_plugin_feature_set_rank(pluginFeature, GST_RANK_PRIMARY + 111);
		gst_object_unref(pluginFeature);
	}

	pluginFeature = gst_registry_lookup_feature(registry, GstPluginNameWV);

	if (pluginFeature == NULL)
	{
		logprintf("AAMPGstPlayer::%s():%d %s plugin feature not available\n", __FUNCTION__, __LINE__, GstPluginNameWV);
	}
	else
	{
		logprintf("AAMPGstPlayer::%s():%d %s plugin priority set to GST_RANK_PRIMARY + 111\n", __FUNCTION__, __LINE__, GstPluginNameWV);
		gst_plugin_feature_set_rank(pluginFeature, GST_RANK_PRIMARY + 111);
		gst_object_unref(pluginFeature);
	}
#endif
}


/**
 * @brief Notify EOS to core aamp asynchronously if required.
 * @note Used internally by AAMPGstPlayer
 */
void AAMPGstPlayer::NotifyEOS()
{
	if (!privateContext->eosSignalled)
	{
		if (!privateContext->eosCallbackIdleTaskPending)
		{
			privateContext->eosCallbackIdleTaskPending = true;
			privateContext->eosCallbackIdleTaskId = g_idle_add(IdleCallbackOnEOS, this);
			if (!privateContext->eosCallbackIdleTaskPending)
			{
				logprintf("%s:%d eosCallbackIdleTask already finished, reset id\n", __FUNCTION__, __LINE__);
				privateContext->eosCallbackIdleTaskId = 0;
			}
			else
			{
				logprintf("%s:%d eosCallbackIdleTask scheduled, eosCallbackIdleTaskId %d\n", __FUNCTION__, __LINE__, privateContext->eosCallbackIdleTaskId);
			}
		}
		else
		{
			logprintf("%s()%d: IdleCallbackOnEOS already registered previously, hence skip!\n", __FUNCTION__, __LINE__);
		}
		privateContext->eosSignalled = true;
	}
	else
	{
		logprintf("%s()%d: EOS already signaled, hence skip!\n", __FUNCTION__, __LINE__);
	}
}
