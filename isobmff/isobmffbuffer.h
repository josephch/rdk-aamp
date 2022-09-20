/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2019 RDK Management
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
* @file isobmffbuffer.h
* @brief Header file for ISO Base Media File Format Buffer
*/

#ifndef __ISOBMFFBUFFER_H__
#define __ISOBMFFBUFFER_H__

#include "isobmffbox.h"
#include <stddef.h>
#include <vector>
#include <string>
#include <cstdint>
#include "AampLogManager.h"

/**
 * @class IsoBmffBuffer
 * @brief Class for ISO BMFF Buffer
 */
class IsoBmffBuffer
{
private:
	std::vector<Box*> boxes;	//ISOBMFF boxes of associated buffer
	uint8_t *buffer;
	size_t bufSize;
	Box* chunkedBox; //will hold one element only
	size_t mdatCount;
	AampLogManager *mLogObj;
	/**
	 * @fn getFirstPTSInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @param[out] pts - pts value
	 * @return true if parse was successful. false otherwise
	 */
	bool getFirstPTSInternal(const std::vector<Box*> *boxes, uint64_t &pts);
        
	/**
	 * @fn getTrackIdInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @param[out] track_id - track_id
	 * @return true if parse was successful. false otherwise
	 */
	bool getTrackIdInternal (const std::vector<Box*> *boxes, uint32_t &track_id);

	/**
	 * @fn getTimeScaleInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @param[out] timeScale - TimeScale value
	 * @param[out] foundMdhd - flag indicates if MDHD box was seen
	 * @return true if parse was successful. false otherwise
	 */
	bool getTimeScaleInternal(const std::vector<Box*> *boxes, uint32_t &timeScale, bool &foundMdhd);

	/**
	 * @fn printBoxesInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @return void
	 */
	void printBoxesInternal(const std::vector<Box*> *boxes);

        /**
         * @fn parseBoxInternal
         *
     	 * @param[in] boxes - ISOBMFF boxes
     	 * @param[in] name - box name to get
     	 * @param[out] buf - mdat buffer pointer
     	 * @param[out] size - size of mdat buffer
     	 * @return bool
     	 */
	bool parseBoxInternal(const std::vector<Box*> *boxes, const char *name, uint8_t *buf, size_t &size);

        /**
     	 * @fn getBoxSizeInternal
     	 *
     	 * @param[in] boxes - ISOBMFF boxes
     	 * @param[in] name - box name to get
     	 * @param[out] size - size of mdat buffer
     	 * @return bool
     	 */
	bool getBoxSizeInternal(const std::vector<Box*> *boxes, const char *name, size_t &size);

    	/**
    	 * @fn getBoxesInternal
    	 *
    	 * @param[in] boxes - ISOBMFF boxes
    	 * @param[in] name - box name to get
    	 * @param[out] pBoxes - size of mdat buffer
    	 * @return bool
    	 */
    	bool getBoxesInternal(const std::vector<Box*> *boxes, const char *name, std::vector<Box*> *pBoxes);

public:
	/**
	 * @brief IsoBmffBuffer constructor
	 */
	IsoBmffBuffer(AampLogManager *logObj=NULL): mLogObj(logObj), boxes(), buffer(NULL), bufSize(0), chunkedBox(NULL), mdatCount(0)
	{

	}

	/**
	 * @fn ~IsoBmffBuffer
	 */
	~IsoBmffBuffer();

	IsoBmffBuffer(const IsoBmffBuffer&) = delete;
	IsoBmffBuffer& operator=(const IsoBmffBuffer&) = delete;

	/**
	 * @fn setBuffer
	 *
	 * @param[in] buf - buffer pointer
	 * @param[in] sz - buffer size
	 * @return void
	 */
	void setBuffer(uint8_t *buf, size_t sz);

	/**
	 * @fn parseBuffer
	 *  @param[in] correctBoxSize - flag to indicate if box size needs to be corrected
	 * @param[in] newTrackId - new track id to overwrite the existing track id, when value is -1, it will not override
	 * @return true if parse was successful. false otherwise
	 */
	bool parseBuffer(bool correctBoxSize = false, int newTrackId = -1);

	/**
	 * @fn restampPTS
	 *
	 * @param[in] offset - pts offset
	 * @param[in] basePts - base pts
	 * @param[in] segment - buffer pointer
	 * @param[in] bufSz - buffer size
	 * @return void
	 */
	void restampPTS(uint64_t offset, uint64_t basePts, uint8_t *segment, uint32_t bufSz);

	/**
	 * @fn getFirstPTS
	 *
	 * @param[out] pts - pts value
	 * @return true if parse was successful. false otherwise
	 */
	bool getFirstPTS(uint64_t &pts);
	
	/**
	 * @fn getTrack_id
	 *
	 * @param[out] track_id - track-id
	 * @return true if parse was successful. false otherwise
	 */
	bool getTrack_id(uint32_t &track_id);

	/**
	 * @fn PrintPTS
	 * @return tvoid
	 */
	void PrintPTS(void);

	/**
	 * @fn getTimeScale
	 *
	 * @param[out] timeScale - TimeScale value
	 * @return true if parse was successful. false otherwise
	 */
	bool getTimeScale(uint32_t &timeScale);

	/**
	 * @fn destroyBoxes
	 *
	 * @return void
	 */
	void destroyBoxes();

	/**
	 * @fn printBoxes
	 *
	 * @return void
	 */
	void printBoxes();

	/**
	 * @fn isInitSegment
	 *
	 * @return true if buffer is an initialization segment. false otherwise
	 */
	bool isInitSegment();

	/**
 	 * @fn parseMdatBox
	 * @param[out] buf - mdat buffer pointer
	 * @param[out] size - size of mdat buffer
	 * @return true if mdat buffer is available. false otherwise
	 */
	bool parseMdatBox(uint8_t *buf, size_t &size);

	/**
	 * @fn getMdatBoxSize
	 * @param[out] size - size of mdat buffer
	 * @return true if buffer size available. false otherwise
	 */
	bool getMdatBoxSize(size_t &size);

	/**
	 * @fn getEMSGData
	 *
	 * @param[out] message - messageData from EMSG
	 * @param[out] messageLen - messageLen
	 * @param[out] schemeIdUri - schemeIdUri
	 * @param[out] value - value of Id3
	 * @param[out] presTime - Presentation time
	 * @param[out] timeScale - timeScale of ID3 metadata
	 * @param[out] eventDuration - eventDuration value
	 * @param[out] id - ID of metadata
	 * @return true if parse was successful. false otherwise
	 */
	bool getEMSGData(uint8_t* &message, uint32_t &messageLen, uint8_t* &schemeIdUri, uint8_t* &value, uint64_t &presTime, uint32_t &timeScale, uint32_t &eventDuration, uint32_t &id);

	/**
	 * @fn getEMSGInfoInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @param[out] message - messageData pointer
	 * @param[out] messageLen - messageLen value
	 * @param[out] schemeIdUri - schemeIdUri
	 * @param[out] value - value of Id3
	 * @param[out] presTime - Presentation time
	 * @param[out] timeScale - timeScale of ID3 metadata
	 * @param[out] eventDuration - eventDuration value
	 * @param[out] id - ID of metadata
	 * @param[out] foundEmsg - flag indicates if EMSG box was seen
	 * @return true if parse was successful. false otherwise
	 */
	bool getEMSGInfoInternal(const std::vector<Box*> *boxes, uint8_t* &message, uint32_t &messageLen, uint8_t* &schemeIdUri, uint8_t* &value, uint64_t &presTime, uint32_t &timeScale, uint32_t &eventDuration, uint32_t &id, bool &foundEmsg);
	
	/**
	 * @fn getMdatBoxCount
	 * @param[out] count - mdat box count
	 * @return true if mdat count available. false otherwise
	 */
	bool getMdatBoxCount(size_t &count);

	/**
 	 * @fn printMdatBoxes
	 *
	 * @return void
	 */
	void printMdatBoxes();

	/**
	 * @fn getTypeOfBoxes
	 * @param[in] name - box name to get
	 * @param[out] stBoxes - List of box handles of a type in a parsed buffer
	 * @return true if Box found. false otherwise
	 */
	bool getTypeOfBoxes(const char *name, std::vector<Box*> &stBoxes);

	/**
	 * @fn getChunkedfBox
	 *
	 * @return Box handle if Chunk box found in a parsed buffer. NULL otherwise
	 */
	Box* getChunkedfBox() const;

	/**
	 * @fn getParsedBoxes
	 *
	 * @return Box handle list if Box found at index given. NULL otherwise
	 */
	std::vector<Box*> *getParsedBoxes();

	/**
	 * @fn getBox
	 * @param[in] name - box name to get
	 * @param[out] index - index of box in a parsed buffer
	 * @return Box handle if Box found at index given. NULL otherwise
	 */
	Box* getBox(const char *name, size_t &index);

	/**
	 * @fn getBoxAtIndex
	 * @param[out] index - index of box in a parsed buffer
	 * @return Box handle if Box found at index given. NULL otherwise
	 */
	Box* getBoxAtIndex(size_t index);

	/**
	 * @fn printPTSInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @return void
	 */
	void printPTSInternal(const std::vector<Box*> *boxes);

	/**
	 * @fn getSampleDuration
	 *
	 * @param[in] box - ISOBMFF box
	 * @param[in] fduration -  duration to get
	 * @return void
	 */
	void getSampleDuration(Box *box, uint64_t &fduration);

	/**
	 * @fn getSampleDurationInernal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @return uint64_t - duration  value
	 */
	uint64_t getSampleDurationInernal(const std::vector<Box*> *boxes);

	/**
	 * @fn getPts
	 *
	 * @param[in] box - ISOBMFF box
	 * @param[in] fpts -  PTS to get
	 * @return void
	 */
	void getPts(Box *box, uint64_t &fpts);

	/**
 	 * @fn getPtsInternal
	 *
	 * @param[in] boxes - ISOBMFF boxes
	 * @return uint64_t - PTS value
	 */
	uint64_t getPtsInternal(const std::vector<Box*> *boxes);
};


#endif /* __ISOBMFFBUFFER_H__ */
