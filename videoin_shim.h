/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 RDK Management
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
 * @file videoin_shim.h
 * @brief shim for dispatching UVE Video input playback
 */

#ifndef VIDEOIN_SHIM_H_
#define VIDEOIN_SHIM_H_

#include "StreamAbstractionAAMP.h"
#include <string>
#include <stdint.h>
#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
#include "Module.h"
#include <core/core.h>
#include "ThunderAccess.h"
#endif
using namespace std;

/**
 * @class StreamAbstractionAAMP_VIDEOIN
 * @brief Fragment collector for MPEG DASH
 */
class StreamAbstractionAAMP_VIDEOIN : public StreamAbstractionAAMP
{
public:
	
    /**
     * @brief StreamAbstractionAAMP_VIDEOIN Constructor
     * @param aamp pointer to PrivateInstanceAAMP object associated with player
     * @param seekpos Seek position
     * @param rate playback rate
     */
    StreamAbstractionAAMP_VIDEOIN(const std::string name, const std::string callSign, AampLogManager *logObj, class PrivateInstanceAAMP *aamp,double seekpos, float rate);
    /**
     * @brief StreamAbstractionAAMP_VIDEOIN Destructor
     */
    ~StreamAbstractionAAMP_VIDEOIN();
    /**     
     * @brief Copy constructor disabled
     *
     */
    StreamAbstractionAAMP_VIDEOIN(const StreamAbstractionAAMP_VIDEOIN&) = delete;
    /**
     * @brief assignment operator disabled
     *
     */
    StreamAbstractionAAMP_VIDEOIN& operator=(const StreamAbstractionAAMP_VIDEOIN&) = delete;
    /**
     * @brief Stub implementation
     */
    void DumpProfiles(void) override;
    /**
     *   @brief  Starts streaming.
     */
    void Start() override;
    /**
     *   @brief Stops streaming.
     */	
    void Stop(bool clearChannelData) override;
    /**
     * @brief SetVideoRectangle sets the position coordinates (x,y) & size (w,h)
     * 
     * @param[in] x,y - position coordinates of video rectangle
     * @param[in] w,h - width & height of video rectangle
     */
    void SetVideoRectangle(int x, int y, int w, int h) override;
    /**
     *   @brief  Initialize a newly created object.
     *   @note   To be implemented by sub classes
     *   @param  tuneType to set type of object.
     *   @retval eAAMPSTATUS_OK
     */
    AAMPStatusType Init(TuneType tuneType) override;
    /**
     * @brief Get output format of stream.
     *
     * @param[out]  primaryOutputFormat - format of primary track
     * @param[out]  audioOutputFormat - format of audio track
     * @param[out]  auxAudioOutputFormat - format of aux track
     */
    void GetStreamFormat(StreamOutputFormat &primaryOutputFormat, StreamOutputFormat &audioOutputFormat, StreamOutputFormat &auxAudioOutputFormat) override;
    /**
     * @brief Get current stream position.
     *
     * @retval current position of stream. 
     */
    double GetStreamPosition() override;
    /**
     *   @brief Return MediaTrack of requested type
     *
     *   @param[in] type - track type
     *   @retval MediaTrack pointer.
     */
    MediaTrack* GetMediaTrack(TrackType type) override;
    /**
     *   @brief  Get PTS of first sample.
     *
     *   @retval PTS of first sample
     */
    double GetFirstPTS() override;
    /**
     *   @brief  Get Start time PTS of first sample. 
     *   
     *   @retval start time of first sample
     */
    double GetStartTimeOfFirstPTS() override;
    double GetBufferedDuration() override;
    bool IsInitialCachingSupported() override;
    /**
     * @brief Get index of profile corresponds to bandwidth
     * @param[in] bitrate Bitrate to lookup profile
     * @retval profile index
     */
    int GetBWIndex(long bitrate) override;
    /**
     * @brief To get the available video bitrates.
     * @return available video bitrates
     */
    std::vector<long> GetVideoBitrates(void) override;
    /**
     * @brief To get the available audio bitrates.
     * @return available audio bitrates
     */
    std::vector<long> GetAudioBitrates(void) override;
    /**
     * @brief Gets Max Bitrate avialable for current playback.
     * @return long MAX video bitrates
     */
    long GetMaxBitrate(void) override;
    /**
     *   @brief  Stops injecting fragments to StreamSink.
     */
    void StopInjection(void) override;
    /**
     *   @brief  Start injecting fragments to StreamSink.
     */
    void StartInjection(void) override;
    void SeekPosUpdate(double) { };
protected:
    /**
     *   @brief Get stream information of a profile from subclass.
     *
     *   @param[in]  idx - profile index.
     *   @retval stream information corresponding to index.
     */
    StreamInfo* GetStreamInfo(int idx) override;
    AAMPStatusType InitHelper(TuneType tuneType);
    /**
     *   @brief  calls start on video in specified by port and method name
     */
    void StartHelper(int port, const std::string & methodName);
    /**
     *   @brief  Stops streaming.
     */
    void StopHelper(const std::string & methodName) ;
    bool mTuned;

#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
	 
    /**
     *   @brief  Registers  Event to input plugin and to mRegisteredEvents list for later use.
     *   @param[in] eventName : Event name
     *  @param[in] functionHandler : Event funciton pointer
     */
    void RegisterEvent (string eventName, std::function<void(const WPEFramework::Core::JSON::VariantContainer&)> functionHandler);
    /**
     *   @brief  Registers all Events to input plugin
     */
    void RegisterAllEvents ();
#endif

private:
#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
    ThunderAccessAAMP thunderAccessObj;
    ThunderAccessAAMP thunderRDKShellObj;
    
    /**
     *   @brief  Gets  onSignalChanged and translates into aamp events
     */
    void OnInputStatusChanged(const JsonObject& parameters);
    /** 
     *  @brief  Gets  onSignalChanged and translates into aamp events
     */
    void OnSignalChanged(const JsonObject& parameters);
#endif
    bool GetScreenResolution(int & screenWidth, int & screenHeight);
    int videoInputPort;
    std::string mName; // Used for logging
    std::list<std::string> mRegisteredEvents;
};

#endif // VIDEOIN_SHIM_H_
/**
 * @}
 */

