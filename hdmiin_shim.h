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
 * @file hdmiin_shim.h
 * @brief shim for dispatching UVE HDMI input playback
 */

#ifndef HDMIIN_SHIM_H_
#define HDMIIN_SHIM_H_

#include "videoin_shim.h"
#include <string>
#include <stdint.h>
#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
#include <core/core.h>
#include "ThunderAccess.h"
#endif
using namespace std;

/**
 * @class StreamAbstractionAAMP_HDMIIN
 * @brief Fragment collector for MPEG DASH
 */
class StreamAbstractionAAMP_HDMIIN : public StreamAbstractionAAMP_VIDEOIN
{
public:
    /**
     * @fn StreamAbstractionAAMP_HDMIIN
     * @param aamp pointer to PrivateInstanceAAMP object associated with player
     * @param seekpos Seek position
     * @param rate playback rate
     */	
    StreamAbstractionAAMP_HDMIIN(AampLogManager *logObj, class PrivateInstanceAAMP *aamp,double seekpos, float rate);
    /**
     * @fn ~StreamAbstractionAAMP_HDMIIN 
     */    
    ~StreamAbstractionAAMP_HDMIIN();
    /**
     * @brief Copy constructor disabled
     *
     */
    StreamAbstractionAAMP_HDMIIN(const StreamAbstractionAAMP_HDMIIN&) = delete;
    /**
     * @brief assignment operator disabled
     *
     */
    StreamAbstractionAAMP_HDMIIN& operator=(const StreamAbstractionAAMP_HDMIIN&) = delete;
    /**
     *   @fn Init
     *   @param  tuneType to set type of object.
     *   @retval eAAMPSTATUS_OK
     */
    AAMPStatusType Init(TuneType tuneType) override;
    /**
     *   @fn Start
     */
    void Start() override;
    /**
     *   @fn Stop
     */
    void Stop(bool clearChannelData) override;
    /**
     * @fn GetAvailableVideoTracks
     */
    std::vector<StreamInfo*> GetAvailableVideoTracks(void) override;
    /**
     * @fn GetAvailableThumbnailTracks
     */
    std::vector<StreamInfo*> GetAvailableThumbnailTracks(void) override;
    /**
     * @fn SetThumbnailTrack
     */
    bool SetThumbnailTrack(int) override;
    /**
     * @fn GetThumbnailRangeData
     */
    std::vector<ThumbnailData> GetThumbnailRangeData(double,double, std::string*, int*, int*, int*, int*) override;
private:
#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
    /*Event Handler*/
    /**
     *   @fn OnVideoStreamInfoUpdate
     */
    void OnVideoStreamInfoUpdate(const JsonObject& parameters);
#endif
};

#endif // HDMIIN_SHIM_H_
/**
 * @}
 */
 
