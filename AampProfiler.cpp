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
 * @file AampProfiler.cpp
 * @brief ProfileEventAAMP class impl
 */

#include "AampProfiler.h"
#include "AampConstants.h"
#include "AampUtils.h"
#include "AampConfig.h"

#include <algorithm>

#define MAX std::max

/**
 * @brief ProfileEventAAMP Constructor
 */
ProfileEventAAMP::ProfileEventAAMP():
	tuneStartMonotonicBase(0), tuneStartBaseUTCMS(0), bandwidthBitsPerSecondVideo(0),
        bandwidthBitsPerSecondAudio(0), drmErrorCode(0), enabled(false), xreTimeBuckets(), tuneEventList(),
	tuneEventListMtx(), mTuneFailBucketType(PROFILE_BUCKET_MANIFEST), mTuneFailErrorCode(0),bEnableMicroEvent(false)
{

}

/**
 * @brief Record a new tune time event.
 *
 * @param[in] pbt - Profiler bucket type
 * @param[in] start - Start time
 * @param[in] dur - Duration
 * @param[in] res - Event result
 * @return void
 */
void ProfileEventAAMP::addtuneEvent(ProfilerBucketType pbt, unsigned int start, unsigned int dur, int res)
{
	if (pbt >= PROFILE_BUCKET_TYPE_COUNT)
	{
		AAMPLOG_WARN("%s:%d bucketId=%d > PROFILE_BUCKET_TYPE_COUNT. How did it happen?", __FUNCTION__, __LINE__, pbt);
		return;
	}

	// DELIA-41285: don't exclude any pre-tune download activity
	if (!buckets[PROFILE_BUCKET_FIRST_FRAME].complete)
	{
		std::lock_guard<std::mutex> lock(tuneEventListMtx);
		tuneEventList.emplace_back(pbt,(start - tuneStartMonotonicBase),dur,res);
	}
}

/**
 * @brief Get tune time events in JSON format
 *
 * @param[out] outSS - Output JSON string
 * @param[in] streamType - Stream type
 * @param[in] url - Tune URL
 * @param[in] success - Tune success/failure
 * @return void
 */
void ProfileEventAAMP::getTuneEventsJSON(std::string &outStr, const std::string &streamType, const char *url, bool success)
{
	bool siblingEvent = false;
	unsigned int tEndTime = NOW_STEADY_TS_MS;
	unsigned int td = tEndTime - tuneStartMonotonicBase;
	size_t end = 0;

	std::string temlUrl = url;
	end = temlUrl.find("?");

	if (end != std::string::npos)
	{
		temlUrl = temlUrl.substr(0, end);
	}

	char outPtr[512];
	memset(outPtr, '\0', 512);

	snprintf(outPtr, 512, "{\"s\":%lld,\"td\":%d,\"st\":\"%s\",\"u\":\"%s\",\"tf\":{\"i\":%d,\"er\":%d},\"r\":%d,\"v\":[",tuneStartBaseUTCMS, td, streamType.c_str(), temlUrl.c_str(), mTuneFailBucketType, mTuneFailErrorCode, (success ? 1 : 0));

	outStr.append(outPtr);

	std::lock_guard<std::mutex> lock(tuneEventListMtx);
	for(auto &te:tuneEventList)
	{
		if(siblingEvent)
		{
			outStr.append(",");
		}
		char eventPtr[256];
		memset(eventPtr, '\0', 256);
		snprintf(eventPtr, 256, "{\"i\":%d,\"b\":%d,\"d\":%d,\"o\":%d}", te.id, te.start, te.duration, te.result);
		outStr.append(eventPtr);

		siblingEvent = true;
	}
	outStr.append("]}");

	tuneEventList.clear();
	mTuneFailErrorCode = 0;
	mTuneFailBucketType = PROFILE_BUCKET_MANIFEST;
}

/**
 * @brief Profiler method to perform tune begin related operations.
 *
 * @return void
 */
void ProfileEventAAMP::TuneBegin(void)
{ // start tune
	memset(buckets, 0, sizeof(buckets));
	tuneStartBaseUTCMS = NOW_SYSTEM_TS_MS;
	tuneStartMonotonicBase = NOW_STEADY_TS_MS;
	bandwidthBitsPerSecondVideo = 0;
	bandwidthBitsPerSecondAudio = 0;
	drmErrorCode = 0;
	enabled = true;
	mTuneFailBucketType = PROFILE_BUCKET_MANIFEST;
	mTuneFailErrorCode = 0;
	tuneEventList.clear();
}

/**
 * @brief Logging performance metrics after successful tune completion. Metrics starts with IP_AAMP_TUNETIME
 *
 * <h4>Format of IP_AAMP_TUNETIME:</h4>
 * version,	// version for this protocol, initially zero<br>
 * build,		// incremented when there are significant player changes/optimizations<br>
 * tunestartUtcMs,	// when tune logically started from AAMP perspective<br>
 * <br>
 * ManifestDownloadStartTime,  // offset in milliseconds from tunestart when main manifest begins download<br>
 * ManifestDownloadTotalTime,  // time (ms) taken for main manifest download, relative to ManifestDownloadStartTime<br>
 * ManifestDownloadFailCount,  // if >0 ManifestDownloadTotalTime spans multiple download attempts<br>
 * <br>
 * PlaylistDownloadStartTime,  // offset in milliseconds from tunestart when playlist subManifest begins download<br>
 * PlaylistDownloadTotalTime,  // time (ms) taken for playlist subManifest download, relative to PlaylistDownloadStartTime<br>
 * PlaylistDownloadFailCount,  // if >0 otherwise PlaylistDownloadTotalTime spans multiple download attempts<br>
 * <br>
 * InitFragmentDownloadStartTime, // offset in milliseconds from tunestart when init fragment begins download<br>
 * InitFragmentDownloadTotalTime, // time (ms) taken for fragment download, relative to InitFragmentDownloadStartTime<br>
 * InitFragmentDownloadFailCount, // if >0 InitFragmentDownloadTotalTime spans multiple download attempts<br>
 * <br>
 * Fragment1DownloadStartTime, // offset in milliseconds from tunestart when fragment begins download<br>
 * Fragment1DownloadTotalTime, // time (ms) taken for fragment download, relative to Fragment1DownloadStartTime<br>
 * Fragment1DownloadFailCount, // if >0 Fragment1DownloadTotalTime spans multiple download attempts<br>
 * Fragment1Bandwidth,	    	// intrinsic bitrate of downloaded fragment<br>
 * <br>
 * drmLicenseRequestStart,	    // offset in milliseconds from tunestart<br>
 * drmLicenseRequestTotalTime, // time (ms) for license acquisition relative to drmLicenseRequestStart<br>
 * drmFailErrorCode,           // nonzero if drm license acquisition failed during tuning<br>
 * <br>
 * LAPreProcDuration,	    	// License acquisition pre-processing duration in ms<br>
 * LANetworkDuration, 			// License acquisition network duration in ms<br>
 * LAPostProcDuration,         // License acquisition post-processing duration in ms<br>
 * <br>
 * VideoDecryptDuration,		// Video fragment decrypt duration in ms<br>
 * AudioDecryptDuration,		// Audio fragment decrypt duration in ms<br>
 * <br>
 * gstStart,	// offset in ms from tunestart when pipeline creation/setup begins<br>
 * gstFirstFrame,  // offset in ms from tunestart when first frame of video is decoded/presented<br>
 * <br>
 * contentType, 	//Playback Mode. Values: CDVR, VOD, LINEAR, IVOD, EAS, CAMERA, DVR, MDVR, IPDVR, PPV<br>
 * streamType, 	//Stream Type. Values: 10-HLS/Clear, 11-HLS/Consec, 12-HLS/Access, 13-HLS/Vanilla AES, 20-DASH/Clear, 21-DASH/WV, 22-DASH/PR<br>
 * firstTune		//First tune after reboot/crash<br>
 * Prebuffered		//If the Player was in preBuffer(BG) mode)<br>
 * PreBufferedTime		//Player spend Time in BG<br>
 * @param[in] success - Tune status
 * @param[in] contentType - Content Type. Eg: LINEAR, VOD, etc
 * @param[in] streamType - Stream Type. Eg: HLS, DASH, etc
 * @param[in] firstTune - Is it a first tune after reboot/crash.
 * @return void
 */
void ProfileEventAAMP::TuneEnd(bool success, ContentType contentType, int streamType, bool firstTune, std::string appName, std::string playerActiveMode, int playerId, bool playerPreBuffered)
{
	if(!enabled )
	{
		return;
	}
	enabled = false;
	unsigned int licenseAcqNWTime = bucketDuration(PROFILE_BUCKET_LA_NETWORK);
	if(licenseAcqNWTime == 0) licenseAcqNWTime = bucketDuration(PROFILE_BUCKET_LA_TOTAL); //A HACK for HLS

	char tuneTimeStrPrefix[64];
	memset(tuneTimeStrPrefix, '\0', sizeof(tuneTimeStrPrefix));
	if (!appName.empty())
	{
		snprintf(tuneTimeStrPrefix, sizeof(tuneTimeStrPrefix), "%s PLAYER[%d] APP: %s IP_AAMP_TUNETIME", playerActiveMode.c_str(),playerId,appName.c_str());
	}
	else
	{
		snprintf(tuneTimeStrPrefix, sizeof(tuneTimeStrPrefix), "%s PLAYER[%d] IP_AAMP_TUNETIME", playerActiveMode.c_str(),playerId);
	}

	AAMPLOG_WARN("%s:%d,%d,%lld," // prefix, version, build, tuneStartBaseUTCMS
		"%d,%d,%d," 	// main manifest (start,total,err)
		"%d,%d,%d," 	// video playlist (start,total,err)
		"%d,%d,%d," 	// audio playlist (start,total,err)

		"%d,%d,%d," 	// video init-segment (start,total,err)
		"%d,%d,%d," 	// audio init-segment (start,total,err)

		"%d,%d,%d,%ld," 	// video fragment (start,total,err, bitrate)
		"%d,%d,%d,%ld," 	// audio fragment (start,total,err, bitrate)

		"%d,%d,%d," 	// licenseAcqStart, licenseAcqTotal, drmFailErrorCode
		"%d,%d,%d," 	// LAPreProcDuration, LANetworkDuration, LAPostProcDuration

		"%d,%d," 		// VideoDecryptDuration, AudioDecryptDuration
		"%d,%d," 		// gstPlayStartTime, gstFirstFrameTime
		"%d,%d,%d," 		// contentType, streamType, firstTune
                "%d,%d",                // If Player was in prebufferd mode, time spent in prebufferd(BG) mode
		// TODO: settop type, flags, isFOGEnabled, isDDPlus, isDemuxed, assetDurationMs

		tuneTimeStrPrefix,
		4, // version for this protocol, initially zero
		0, // build - incremented when there are significant player changes/optimizations
		tuneStartBaseUTCMS, // when tune logically started from AAMP perspective

		buckets[PROFILE_BUCKET_MANIFEST].tStart, bucketDuration(PROFILE_BUCKET_MANIFEST), buckets[PROFILE_BUCKET_MANIFEST].errorCount,
		buckets[PROFILE_BUCKET_PLAYLIST_VIDEO].tStart, bucketDuration(PROFILE_BUCKET_PLAYLIST_VIDEO), buckets[PROFILE_BUCKET_PLAYLIST_VIDEO].errorCount,
		buckets[PROFILE_BUCKET_PLAYLIST_AUDIO].tStart, bucketDuration(PROFILE_BUCKET_PLAYLIST_AUDIO), buckets[PROFILE_BUCKET_PLAYLIST_AUDIO].errorCount,

		buckets[PROFILE_BUCKET_INIT_VIDEO].tStart, bucketDuration(PROFILE_BUCKET_INIT_VIDEO), buckets[PROFILE_BUCKET_INIT_VIDEO].errorCount,
		buckets[PROFILE_BUCKET_INIT_AUDIO].tStart, bucketDuration(PROFILE_BUCKET_INIT_AUDIO), buckets[PROFILE_BUCKET_INIT_AUDIO].errorCount,

		buckets[PROFILE_BUCKET_FRAGMENT_VIDEO].tStart, bucketDuration(PROFILE_BUCKET_FRAGMENT_VIDEO), buckets[PROFILE_BUCKET_FRAGMENT_VIDEO].errorCount,bandwidthBitsPerSecondVideo,
		buckets[PROFILE_BUCKET_FRAGMENT_AUDIO].tStart, bucketDuration(PROFILE_BUCKET_FRAGMENT_AUDIO), buckets[PROFILE_BUCKET_FRAGMENT_AUDIO].errorCount,bandwidthBitsPerSecondAudio,

		buckets[PROFILE_BUCKET_LA_TOTAL].tStart, bucketDuration(PROFILE_BUCKET_LA_TOTAL), drmErrorCode,
		bucketDuration(PROFILE_BUCKET_LA_PREPROC), licenseAcqNWTime, bucketDuration(PROFILE_BUCKET_LA_POSTPROC),
		bucketDuration(PROFILE_BUCKET_DECRYPT_VIDEO),bucketDuration(PROFILE_BUCKET_DECRYPT_AUDIO),

		buckets[PROFILE_BUCKET_FIRST_BUFFER].tStart, // gstPlaying: offset in ms from tunestart when pipeline first fed data
		playerPreBuffered ? buckets[PROFILE_BUCKET_FIRST_FRAME].tStart - buckets[PROFILE_BUCKET_PLAYER_PRE_BUFFERED].tStart : buckets[PROFILE_BUCKET_FIRST_FRAME].tStart,  // gstFirstFrame: offset in ms from tunestart when first frame of video is decoded/presented
		contentType, streamType, firstTune,
		playerPreBuffered,playerPreBuffered ? buckets[PROFILE_BUCKET_PLAYER_PRE_BUFFERED].tStart : 0
		);
}

/**
 * @brief Method converting the AAMP style tune performance data to IP_EX_TUNETIME style data
 *
 * @param[in] success - Tune status
 * @param[in] tuneRetries - Number of tune attempts
 * @param[in] playerLoadTime - Time at which the first tune request reached the AAMP player
 * @param[in] streamType - Type of stream. eg: HLS, DASH, etc
 * @param[in] isLive  - Live channel or not
 * @param[in] durationinSec - Asset duration in seconds
 * @param[out] TuneTimeInfoStr - Formatted output string
 * @return void
 */
void ProfileEventAAMP::GetClassicTuneTimeInfo(bool success, int tuneRetries, int firstTuneType, long long playerLoadTime, int streamType, bool isLive,unsigned int durationinSec, char *TuneTimeInfoStr)
{
	// Prepare String for Classic TuneTime data
	// Note: Certain buckets won't be available; will take the tFinish of the previous bucket as the start & finish those buckets.
	xreTimeBuckets[TuneTimeBeginLoad]               =       tuneStartMonotonicBase ;
	xreTimeBuckets[TuneTimePrepareToPlay]           =       tuneStartMonotonicBase + buckets[PROFILE_BUCKET_MANIFEST].tFinish;
	xreTimeBuckets[TuneTimePlay]                    =       tuneStartMonotonicBase + MAX(buckets[PROFILE_BUCKET_MANIFEST].tFinish, MAX(buckets[PROFILE_BUCKET_PLAYLIST_VIDEO].tFinish, buckets[PROFILE_BUCKET_PLAYLIST_AUDIO].tFinish));
	xreTimeBuckets[TuneTimeDrmReady]                =       MAX(xreTimeBuckets[TuneTimePlay], (tuneStartMonotonicBase +  buckets[PROFILE_BUCKET_LA_TOTAL].tFinish));
	long long fragmentReadyTime                     =       tuneStartMonotonicBase + MAX(buckets[PROFILE_BUCKET_FRAGMENT_VIDEO].tFinish, buckets[PROFILE_BUCKET_FRAGMENT_AUDIO].tFinish);
	xreTimeBuckets[TuneTimeStartStream]             =       MAX(xreTimeBuckets[TuneTimeDrmReady],fragmentReadyTime);
	xreTimeBuckets[TuneTimeStreaming]               =       tuneStartMonotonicBase + buckets[PROFILE_BUCKET_FIRST_FRAME].tStart;

	unsigned int failRetryBucketTime                =       tuneStartMonotonicBase - playerLoadTime;
	unsigned int prepareToPlayBucketTime            =       (unsigned int)(xreTimeBuckets[TuneTimePrepareToPlay] - xreTimeBuckets[TuneTimeBeginLoad]);
	unsigned int playBucketTime                     =       (unsigned int)(xreTimeBuckets[TuneTimePlay]- xreTimeBuckets[TuneTimePrepareToPlay]);
	unsigned int fragmentBucketTime                 =       (unsigned int)(fragmentReadyTime - xreTimeBuckets[TuneTimePlay]) ;
	unsigned int decoderStreamingBucketTime         =       xreTimeBuckets[TuneTimeStreaming] - xreTimeBuckets[TuneTimeStartStream];
	/*Note: 'Drm Ready' to 'decrypt start' gap is not covered in any of the buckets.*/

	unsigned int manifestTotal      =       bucketDuration(PROFILE_BUCKET_MANIFEST);
	unsigned int profilesTotal      =       effectiveBucketTime(PROFILE_BUCKET_PLAYLIST_VIDEO, PROFILE_BUCKET_PLAYLIST_AUDIO);
	unsigned int initFragmentTotal  =       effectiveBucketTime(PROFILE_BUCKET_INIT_VIDEO, PROFILE_BUCKET_INIT_AUDIO);
	unsigned int fragmentTotal      =       effectiveBucketTime(PROFILE_BUCKET_FRAGMENT_VIDEO, PROFILE_BUCKET_FRAGMENT_AUDIO);
	// DrmReadyBucketTime is licenseTotal, time taken for complete license acquisition
	// licenseNWTime is the time taken for network request.
	unsigned int licenseTotal       =       bucketDuration(PROFILE_BUCKET_LA_TOTAL);
	unsigned int licenseNWTime      =       bucketDuration(PROFILE_BUCKET_LA_NETWORK);
	if(licenseNWTime == 0)
	{
		licenseNWTime = licenseTotal;  //A HACK for HLS
	}

	// Total Network Time
	unsigned int networkTime = manifestTotal + profilesTotal + initFragmentTotal + fragmentTotal + licenseNWTime;

	snprintf(TuneTimeInfoStr,AAMP_MAX_PIPE_DATA_SIZE,"%d,%lld,%d,%d," //totalNetworkTime, playerLoadTime , failRetryBucketTime, prepareToPlayBucketTime,
			"%d,%d,%d,"                                             //playBucketTime ,licenseTotal , decoderStreamingBucketTime
			"%d,%d,%d,%d,"                                          // manifestTotal,profilesTotal,fragmentTotal,effectiveFragmentDLTime
			"%d,%d,%d,%d,"                                          // licenseNWTime,success,durationinMilliSec,isLive
			"%lld,%lld,%lld,"                                       // TuneTimeBeginLoad,TuneTimePrepareToPlay,TuneTimePlay,
			"%lld,%lld,%lld,"                                       //TuneTimeDrmReady,TuneTimeStartStream,TuneTimeStreaming
			"%d,%d,%d,%lld",                                             //streamType, tuneRetries, TuneType, TuneCompleteTime(UTC MSec)
			networkTime,playerLoadTime, failRetryBucketTime, prepareToPlayBucketTime,playBucketTime,licenseTotal,decoderStreamingBucketTime,
			manifestTotal,profilesTotal,(initFragmentTotal + fragmentTotal),fragmentBucketTime, licenseNWTime,success,durationinSec*1000,isLive,
			xreTimeBuckets[TuneTimeBeginLoad],xreTimeBuckets[TuneTimePrepareToPlay],xreTimeBuckets[TuneTimePlay] ,xreTimeBuckets[TuneTimeDrmReady],
			xreTimeBuckets[TuneTimeStartStream],xreTimeBuckets[TuneTimeStreaming],streamType,tuneRetries,firstTuneType,(long long)NOW_SYSTEM_TS_MS
	);
#ifndef CREATE_PIPE_SESSION_TO_XRE
	AAMPLOG_WARN("AAMP=>XRE: %s", TuneTimeInfoStr);
#endif
}

/**
 * @brief Marking the beginning of a bucket
 *
 * @param[in] type - Bucket type
 * @return void
 */
void ProfileEventAAMP::ProfileBegin(ProfilerBucketType type)
{
	struct ProfilerBucket *bucket = &buckets[type];
	if (!bucket->complete && (0==bucket->tStart))	//No other Begin should record before the End
	{
		bucket->tStart = NOW_STEADY_TS_MS - tuneStartMonotonicBase;
		bucket->tFinish = bucket->tStart;
	}
}

/**
 * @brief Marking error while executing a bucket
 *
 * @param[in] type - Bucket type
 * @param[in] result - Error code
 * @return void
 */
void ProfileEventAAMP::ProfileError(ProfilerBucketType type, int result)
{
	struct ProfilerBucket *bucket = &buckets[type];
	if (!bucket->complete && !(0==bucket->tStart))
	{
		SetTuneFailCode(result, type);
		bucket->errorCount++;
		if (bEnableMicroEvent && (type == PROFILE_BUCKET_DECRYPT_VIDEO || type == PROFILE_BUCKET_DECRYPT_AUDIO
					 || type == PROFILE_BUCKET_LA_TOTAL || type == PROFILE_BUCKET_LA_NETWORK))
		{
			long long start = bucket->tStart + tuneStartMonotonicBase;
			addtuneEvent(type, start, (unsigned int)(NOW_STEADY_TS_MS - start), result);
		}
	}
}

/**
 * @brief Marking the end of a bucket
 *
 * @param[in] type - Bucket type
 * @return void
 */
void ProfileEventAAMP::ProfileEnd(ProfilerBucketType type)
{
	struct ProfilerBucket *bucket = &buckets[type];
	if (!bucket->complete && !(0==bucket->tStart))
	{
		bucket->tFinish = NOW_STEADY_TS_MS - tuneStartMonotonicBase;
		if(bEnableMicroEvent && (type == PROFILE_BUCKET_DECRYPT_VIDEO || type == PROFILE_BUCKET_DECRYPT_AUDIO
					 || type == PROFILE_BUCKET_LA_TOTAL || type == PROFILE_BUCKET_LA_NETWORK))
		{
			long long start = bucket->tStart + tuneStartMonotonicBase;
				addtuneEvent(type, start, (unsigned int)(bucket->tFinish - bucket->tStart), 200);
		}
		/*
		static const char *bucketName[PROFILE_BUCKET_TYPE_COUNT] =
		{
		"manifest",
		"playlist",
		"fragment",
		"key",
		"decrypt"
		"first-frame"
		};

		logprintf("aamp %7d (+%6d): %s",
		bucket->tStart,
		bucket->tFinish - bucket->tStart,
		bucketName[type]);
		*/
		bucket->complete = true;
	}
}

/**
 * @brief Method to mark the end of a bucket, for which beginning is not marked
 *
 * @param[in] type - Bucket type
 * @return void
 */
void ProfileEventAAMP::ProfilePerformed(ProfilerBucketType type)
{
	ProfileBegin(type);
	buckets[type].complete = true;
}

/**
 * @brief Method to set Failure code and Bucket Type used for microevents
 *
 * @param[in] type - tune Fail Code
 * @param[in] type - Bucket type
 * @return void
 */
void ProfileEventAAMP::SetTuneFailCode(int tuneFailCode, ProfilerBucketType failBucketType)
{
	if(!mTuneFailErrorCode)
	{
		AAMPLOG_INFO("%s:%d Tune Fail: ProfilerBucketType: %d, tuneFailCode: %d", __FUNCTION__, __LINE__, failBucketType, tuneFailCode);
		mTuneFailErrorCode = tuneFailCode;
		mTuneFailBucketType = failBucketType;
	}
}

