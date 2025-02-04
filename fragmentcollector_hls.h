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
 *  @file  fragmentcollector_hls.cpp
 *  @brief This file handles HLS Streaming functionality for AAMP player	
 *
 *  @section DESCRIPTION
 *  
 *  This file handles HLS Streaming functionality for AAMP player. Class/structures 
 *	required for hls fragment collector is defined here.  
 *  Major functionalities include 
 *	a) Manifest / fragment collector and trick play handling
 *	b) DRM Initialization / Key acquisition
 *  c) Decrypt and inject fragments for playback
 *  d) Synchronize audio/video tracks .
 *
 */
#ifndef FRAGMENTCOLLECTOR_HLS_H
#define FRAGMENTCOLLECTOR_HLS_H

#include "StreamAbstractionAAMP.h"
#include "tsprocessor.h"
#include "drm.h"


#define MAX_PROFILE 128 // TODO: remove limitation
#define FOG_FRAG_BW_IDENTIFIER "bandwidth-"
#define FOG_FRAG_BW_IDENTIFIER_LEN 10
#define FOG_FRAG_BW_DELIMITER "-"
#define CHAR_CR 0x0d // '\r'
#define CHAR_LF 0x0a // '\n'
#define BOOLSTR(boolValue) (boolValue?"true":"false")
#define PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS (0.1f)
#define MAX_MANIFEST_DOWNLOAD_RETRY 3
#define MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (6*1000)
#define MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (500) // 500mSec
#define DRM_IV_LEN 16
#define AAMP_AUDIO_FORMAT_MAP_LEN 7
#define AAMP_VIDEO_FORMAT_MAP_LEN 3



/**
* \struct	HlsStreamInfo
* \brief	HlsStreamInfo structure for stream related information 
*/
typedef struct HlsStreamInfo: public StreamInfo
{ // #EXT-X-STREAM-INFs
	long program_id;	/**< Program Id */
	const char *audio;	/**< Audio */
	const char *codecs;	/**< Codec String */
	const char *uri;	/**< URI Information */

	// rarely present
	long averageBandwidth;	/**< Average Bandwidth */
	double frameRate;		/**< Frame Rate */
	const char *closedCaptions;	/**< CC if present */
	const char *subtitles;	/**< Subtitles */
} HlsStreamInfo;

/**
* \struct	MediaInfo
* \brief	MediaInfo structure for Media related information 
*/
typedef struct MediaInfo
{ // #EXT-X-MEDIA
	MediaType type;			/**< Media Type */
	const char *group_id;	/**< Group ID */
	const char *name;		/**< Name of Media */
	const char *language;	/**< Language */
	bool autoselect;		/**< AutoSelect */
	bool isDefault;			/**< IsDefault */
	const char *uri;		/**< URI Information */

	// rarely present
	int channels;			/**< Channel */
	const char *instreamID;	/**< StreamID */
	bool forced;			/**< Forced Flag */
} MediaInfo;

/**
*	\struct	IndexNode
* 	\brief	IndexNode structure for Node/DRM Index
*/
struct IndexNode
{
	double completionTimeSecondsFromStart;	/**< Time of index from start */
	const char *pFragmentInfo;				/**< Fragment Information pointer */
	int drmMetadataIdx;						/**< DRM Index for Fragment */
};

/**
 * \class TrackState
 * \brief State Machine for each Media Track
 *
 * This class is meant to handle each media track of stream
 */
class TrackState : public MediaTrack
{
public:
	/// Constructor
	TrackState(TrackType type, class StreamAbstractionAAMP_HLS* parent, PrivateInstanceAAMP* aamp, const char* name);
	/// Destructor
	~TrackState();
	/// Start Fragment downloader and Injector thread  
	void Start();
	/// Reset and Stop Collector and Injector thread 
	void Stop();
	/// Fragment Collector thread execution function
	void RunFetchLoop();
	/// Function to parse playlist file and update data structures 
	double IndexPlaylist();
	/// Function to handle Profile change after ABR  
	void ABRProfileChanged(void);
	/// Function to get next fragment URI for download 
	char *GetNextFragmentUriFromPlaylist();
	/// Function to update IV value from DRM information 
	void UpdateDrmIV(const char *ptr);
	/// Function to update SHA1 ID from DRM information
	void UpdateDrmCMSha1Hash(const char *ptr);
	/// Function to set the DRM Metadata into Adobe DRM Layer 
	void SetDrmContextUnlocked();
	/// Function to decrypt the fragment data 
	DrmReturn DrmDecrypt(CachedFragment* cachedFragment, ProfilerBucketType bucketType);
	/// Function to fetch the Playlist file
	void FetchPlaylist();
	/// Process Drm Metadata after indexing
	void UpdateDrmMetadata();
	/// Start deferred DRM license acquisition
	void StartDeferredDrmLicenseAcquisition();
	/**
	 * @brief Get period information of next fragment
	 *
	 * @param[out] periodIdx Index of the period in which next fragment belongs
	 * @param[out] offsetFromPeriodStart Offset from start position of the period
	 */
	void GetNextFragmentPeriodInfo(int &periodIdx, double &offsetFromPeriodStart);

	/**
	 * @brief Get start position of the period corresponding to the index.
	 *
	 * @param[in] periodIdx Index of period
	 * @return Start position of the period
	 */
	double GetPeriodStartPosition(int periodIdx);

	/**
	 * @brief Get total number of periods in playlist based on discontinuity
	 *
	 * @return Number of periods in playlist
	 */
	int GetNumberOfPeriods();

private:
	/// Function to get fragment URI based on Index 
	char *GetFragmentUriFromIndex();
	/// Function to flush all the downloads done 
	void FlushIndex();
	/// Function to Fetch the fragment and inject for playback 
	void FetchFragment();
	/// Helper function fetch the fragments 
	bool FetchFragmentHelper(long &http_error, bool &decryption_error);
	/// Function to redownload playlist after refresh interval .
	void RefreshPlaylist(void);
	/// Function to get Context pointer
	StreamAbstractionAAMP* GetContext();
	/// Function to inject fragment decrypted fragment
	void InjectFragmentInternal(CachedFragment* cachedFragment, bool &fragmentDiscarded);
	/// Function to find the media sequence after refresh for continuity
	char *FindMediaForSequenceNumber();

public:
	char effectiveUrl[MAX_URI_LENGTH]; 		/**< uri associated with downloaded playlist (takes into account 302 redirect) */
	char playlistUrl[MAX_URI_LENGTH]; 		/**< uri associated with downloaded playlist */
	GrowableBuffer playlist; 				/**< downloaded playlist contents */
		
	GrowableBuffer index; 			/**< packed IndexNode records for associated playlist */
	int indexCount; 				/**< number of indexed fragments in currently indexed playlist */
	int currentIdx; 				/**< index for currently-presenting fragment used during FF/REW (-1 if undefined) */
	char fragmentURIFromIndex[MAX_URI_LENGTH]; /**< storage for uri generated by GetFragmentUriFromIndex */
	long long indexFirstMediaSequenceNumber; /**< first media sequence number from indexed manifest */

	char *fragmentURI; /**< pointer (into playlist) to URI of current fragment-of-interest */
	long long lastPlaylistDownloadTimeMS; /**< UTC time at which playlist was downloaded */
	int byteRangeLength; /**< state for \#EXT-X-BYTERANGE fragments */
	int byteRangeOffset; /**< state for \#EXT-X-BYTERANGE fragments */

	long long nextMediaSequenceNumber; /**< media sequence number following current fragment-of-interest */
	double playlistPosition; /**< playlist-relative time of most recent fragment-of-interest; -1 if undefined */
	double playTarget; /**< initially relative seek time (seconds) based on playlist window, but updated as a play_target */

	double targetDurationSeconds; /**< copy of \#EXT-X-TARGETDURATION to manage playlist refresh frequency */

	StreamOutputFormat streamOutputFormat; /**< type of data encoded in each fragment */
	TSProcessor* playContext; /**< state for s/w demuxer / pts/pcr restamper module */
	struct timeval startTimeForPlaylistSync; /**< used for time-based track synchronization when switching between playlists */
	double playTargetOffset; /**< For correcting timestamps of streams with audio and video tracks */
	bool discontinuity; /**< Set when discontinuity is found in track*/
	StreamAbstractionAAMP_HLS* context; /**< To get  settings common across tracks*/
	bool fragmentEncrypted; /**< In DAI, ad fragments can be clear. Set if current fragment is encrypted*/
	struct DrmInfo mDrmInfo;	/**< Structure variable to hold Drm Information */
	char* mCMSha1Hash;	/**< variable to store ShaID*/
	long long mDrmTimeStamp;	/**< variable to store Drm Time Stamp */
	int mDrmMetaDataIndexPosition;	/**< Variable to store Drm Meta data Index position*/
	GrowableBuffer mDrmMetaDataIndex;  /**< DrmMetadata records for associated playlist */
	int mDrmMetaDataIndexCount; /**< number of DrmMetadata records in currently indexed playlist */
private:
	bool refreshPlaylist;	/**< bool flag to indicate if playlist refresh required or not */
	pthread_t fragmentCollectorThreadID;	/**< Thread Id for Fragment  collector Thread */
	bool fragmentCollectorThreadStarted;	/**< Flag indicating if fragment collector thread started or not*/
	int manifestDLFailCount;				/**< Manifest Download fail count for retry*/
	std::map<int, double> mPeriodPositionIndex;  /**< period start position mapping of associated playlist */
	bool firstIndexDone;                    /**< Indicates if first indexing is done*/
	HlsDrmBase* mDrm;                       /**< DRM decrypt context*/
};

class StreamAbstractionAAMP_HLS;
class PrivateInstanceAAMP;
/**
 * \class StreamAbstractionAAMP_HLS
 *
 * \brief HLS Stream handler class 
 *
 * This class is meant to handle download of HLS manifest and interface play controls
 */
class StreamAbstractionAAMP_HLS : public StreamAbstractionAAMP
{
public:
	/// Function to to handle parse and indexing of individual tracks 
	double IndexPlaylist(TrackState *trackState);
	/// Constructor 
	StreamAbstractionAAMP_HLS(class PrivateInstanceAAMP *aamp,double seekpos, float rate, bool enableThrottle);
	/// Destructor 
	~StreamAbstractionAAMP_HLS();
	/// Function to log all video/audio profiles 
	void DumpProfiles(void);
	//void SetRate(float rate, double seek_pos );
	/// Function to start processing of all tracks with stream 
	void Start();
	/// Function to handle stop processing of all tracks within stream
	void Stop(bool clearChannelData);
	/// Function to return if stream is Live or VOD 
	bool IsLive();
	/// Function to initialize member variables,download main manifest and parse  
	AAMPStatusType Init(TuneType tuneType);
	/// Function to get stream format 
	void GetStreamFormat(StreamOutputFormat &primaryOutputFormat, StreamOutputFormat &audioOutputFormat);
	/// Function to return current playing position of stream 
	double GetStreamPosition() { return seekPosition; }
	/// Function to return first PTS 
	double GetFirstPTS();
	/// Function to return the MediaTrack instance for the media type input 
	MediaTrack* GetMediaTrack(TrackType type);
	/// Function to return Bandwidth index for the bitrate value 
	int GetBWIndex(long bitrate);
	/// Function to get available video bitrates.
	std::vector<long> GetVideoBitrates(void);
	/// Function to get available audio bitrates.
	std::vector<long> GetAudioBitrates(void);
//private:
	// TODO: following really should be private, but need to be accessible from callbacks
	
	TrackState* trackState[AAMP_TRACK_COUNT];		/**< array to store all tracks of a stream */
	float rate;										/**< Rate of playback  */
	float maxIntervalBtwPlaylistUpdateMs;			/**< Interval between playlist update */

	PlaylistType playlistType;						/**< Playlist Type */
	bool hasEndListTag;								/**< Flag indicating if End list is present or not */

	GrowableBuffer mainManifest;					/**< Main manifest buffer holder */

	bool allowsCache;								/**< Flag indicating if playlist needs to be cached or not */

	HlsStreamInfo streamInfo[MAX_PROFILE];			/**< Array to store multiple stream information */

	int mediaCount;									/**< Number of media in the stream */
	MediaInfo mediaInfo[MAX_PROFILE];				/**< Array to store multiple media within stream */

	double seekPosition;							/**< Seek position for playback */
	int mTrickPlayFPS;								/**< Trick play frames per stream */
	bool enableThrottle;							/**< Flag indicating throttle enable/disable */
	bool firstFragmentDecrypted;					/**< Flag indicating if first fragment is decrypted for stream */
	bool mStartTimestampZero;						/**< Flag indicating if timestamp to start is zero or not (No audio stream) */
	bool newTune;									/**< Flag to indicate new tune  */
	/// Function to parse Main manifest 
	void ParseMainManifest(char *ptr);
	/// Function to get playlist URI for the track type 
	const char *GetPlaylistURI(TrackType trackType, StreamOutputFormat* format = NULL);
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
	/// Function to locally store the download files for debug purpose 
	void HarvestFile(const char * url, GrowableBuffer* buffer, bool isFragment, const char* prefix = NULL);
#endif
	int lastSelectedProfileIndex; 	/**< Variable  to restore in case of playlist download failure */ 
protected:
	/// Function to get StreamInfo stucture based on the index input
	StreamInfo* GetStreamInfo(int idx){ return &streamInfo[idx];}
private:
	/// Function to Synchronize timing of Audio /Video for live streams 
	AAMPStatusType SyncTracks( double trackDuration[]);
	/// Function to Synchronize timing of Audio /Video for Vod streams 
	void SyncVODTracks();
	
	int segDLFailCount;						/**< Segment Download fail count */
	int segDrmDecryptFailCount;				/**< Segment Decrypt fail count */
};

#endif // FRAGMENTCOLLECTOR_HLS_H
