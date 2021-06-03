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
* @file isobmffbox.cpp
* @brief Source file for ISO Base Media File Format Boxes
*/

#include "isobmffbox.h"
#include "AampConfig.h"
#include <stddef.h>

/**
 * @brief Utility function to read 8 bytes from a buffer
 *
 * @param[in] buf - buffer pointer
 * @return bytes read from buffer
 */
uint64_t ReadUint64(uint8_t *buf)
{
	uint64_t val = READ_U32(buf);
	val = (val<<32) | (uint32_t)READ_U32(buf);
	return val;
}

/**
 * @brief Utility function to write 8 bytes to a buffer
 *
 * @param[in] dst - buffer pointer
 * @param[in] value - value to write
 * @return void
 */
void WriteUint64(uint8_t *dst, uint64_t val)
{
	uint32_t msw = (uint32_t)(val>>32);
	WRITE_U32(dst, msw); dst+=4;
	WRITE_U32(dst, val);
}

/**
 * @brief Box constructor
 *
 * @param[in] sz - box size
 * @param[in] btype - box type
 */
Box::Box(uint32_t sz, const char btype[4]) : offset(0), size(sz), type{}
{
	memcpy(type,btype,4);
}

/**
 * @brief Set box's offset from the beginning of the buffer
 *
 * @param[in] os - offset
 * @return void
 */
void Box::setOffset(uint32_t os)
{
	offset = os;
}

/**
 * @brief Get box offset
 *
 * @return offset of box
 */
uint32_t Box::getOffset()
{
	return offset;
}

/**
 * @brief To check if box has any child boxes
 *
 * @return true if this box has other boxes as children
 */
bool Box::hasChildren()
{
	return false;
}

/**
 * @brief Get children of this box
 *
 * @return array of child boxes
 */
const std::vector<Box*> *Box::getChildren()
{
	return NULL;
}

/**
 * @brief Get box size
 *
 * @return box size
 */
uint32_t Box::getSize()
{
	return size;
}

/**
 * @brief Get box type
 *
 * @return box type
 */
const char *Box::getType()
{
	return type;
}

/**
 * @brief Get box type
 *
 * @return box type
 */
const char* Box::getBoxType()
{
    if ((!IS_TYPE(type, MOOV)) ||
        (!IS_TYPE(type, MDIA)) ||
        (!IS_TYPE(type, MOOF)) ||
        (!IS_TYPE(type, TRAF)) ||
        (!IS_TYPE(type, TFDT)) ||
        (!IS_TYPE(type, MVHD)) ||
        (!IS_TYPE(type, MDHD)) ||
        (!IS_TYPE(type, TFDT)) ||
        (!IS_TYPE(type, FTYP)) ||
        (!IS_TYPE(type, STYP)) ||
        (!IS_TYPE(type, SIDX)) ||
        (!IS_TYPE(type, PRFT)) ||
        (!IS_TYPE(type, MDAT)))
    {
        return "UKWN";
    }
    return type;
}


/**
 * @brief Static function to construct a Box object
 *
 * @param[in] hdr - pointer to box
 * @param[in] maxSz - box size
 * @return newly constructed Box object
 */
Box* Box::constructBox(uint8_t *hdr, uint32_t maxSz)
{
    uint32_t size = 0;
    uint8_t type[5];
    if(maxSz < 4)
    {
        AAMPLOG_WARN("Box data < 4 bytes. Can't determine Size & Type\n");
        return new Box(maxSz, (const char *)"UKWN");
    }
    else if(maxSz >= 4 && maxSz < 8)
    {
        AAMPLOG_WARN("Box Size between >4 but <8 bytes. Can't determine Type\n");
        size = READ_U32(hdr);
        return new Box(size, (const char *)"UKWN");
    }
    else
    {
        size = READ_U32(hdr);
        READ_U8(type, hdr, 4);
        type[4] = '\0';
    }

	if (size > maxSz)
	{
#ifdef AAMP_DEBUG_BOX_CONSTRUCT
		AAMPLOG_WARN("Box[%s] Size error:size[%u] > maxSz[%u]\n",type, size, maxSz);
#endif
	}
	else if (IS_TYPE(type, MOOV))
	{
		return GenericContainerBox::constructContainer(size, MOOV, hdr);
	}
	else if (IS_TYPE(type, TRAK))
	{
		return GenericContainerBox::constructContainer(size, TRAK, hdr);
	}
	else if (IS_TYPE(type, MDIA))
	{
		return GenericContainerBox::constructContainer(size, MDIA, hdr);
	}
	else if (IS_TYPE(type, MOOF))
	{
		return GenericContainerBox::constructContainer(size, MOOF, hdr);
	}
	else if (IS_TYPE(type, TRAF))
	{
		return GenericContainerBox::constructContainer(size, TRAF, hdr);
	}
    else if (IS_TYPE(type, TFHD))
    {
        return TfhdBox::constructTfhdBox(size,  hdr);
    }
	else if (IS_TYPE(type, TFDT))
	{
		return TfdtBox::constructTfdtBox(size,  hdr);
	}
	else if (IS_TYPE(type, TRUN))
	{
		return TrunBox::constructTrunBox(size,  hdr);
	}
	else if (IS_TYPE(type, MVHD))
	{
		return MvhdBox::constructMvhdBox(size,  hdr);
	}
	else if (IS_TYPE(type, MDHD))
	{
		return MdhdBox::constructMdhdBox(size,  hdr);
	}

	return new Box(size, (const char *)type);
}

/**
 * @brief GenericContainerBox constructor
 *
 * @param[in] sz - box size
 * @param[in] btype - box type
 */
GenericContainerBox::GenericContainerBox(uint32_t sz, const char btype[4]) : Box(sz, btype), children()
{

}

/**
 * @brief GenericContainerBox destructor
 */
GenericContainerBox::~GenericContainerBox()
{
	for (size_t i = 0; i < children.size(); i++)
	{
		delete children.at(i);
	}
	children.clear();
}

/**
 * @brief Add a box as a child box
 *
 * @param[in] box - child box object
 * @return void
 */
void GenericContainerBox::addChildren(Box *box)
{
	children.push_back(box);
}

/**
 * @brief To check if box has any child boxes
 *
 * @return true if this box has other boxes as children
 */
bool GenericContainerBox::hasChildren()
{
	return true;
}

/**
 * @brief Get children of this box
 *
 * @return array of child boxes
 */
const std::vector<Box*> *GenericContainerBox::getChildren()
{
	return &children;
}

/**
 * @brief Static function to construct a GenericContainerBox object
 *
 * @param[in] sz - box size
 * @param[in] btype - box type
 * @param[in] ptr - pointer to box
 * @return newly constructed GenericContainerBox object
 */
GenericContainerBox* GenericContainerBox::constructContainer(uint32_t sz, const char btype[4], uint8_t *ptr)
{
	GenericContainerBox *cbox = new GenericContainerBox(sz, btype);
	uint32_t curOffset = sizeof(uint32_t) + sizeof(uint32_t); //Sizes of size & type fields
	while (curOffset < sz)
	{
		Box *box = Box::constructBox(ptr, sz-curOffset);
		box->setOffset(curOffset);
		cbox->addChildren(box);
		curOffset += box->getSize();
		ptr += box->getSize();
	}
	return cbox;
}

/**
 * @brief FullBox constructor
 *
 * @param[in] sz - box size
 * @param[in] btype - box type
 * @param[in] ver - version value
 * @param[in] f - flag value
 */
FullBox::FullBox(uint32_t sz, const char btype[4], uint8_t ver, uint32_t f) : Box(sz, btype), version(ver), flags(f)
{

}

/**
 * @brief MvhdBox constructor
 *
 * @param[in] sz - box size
 * @param[in] tScale - TimeScale value
 */
MvhdBox::MvhdBox(uint32_t sz, uint32_t tScale) : FullBox(sz, Box::MVHD, 0, 0), timeScale(tScale)
{

}

/**
 * @brief MvhdBox constructor
 *
 * @param[in] fbox - box object
 * @param[in] tScale - TimeScale value
 */
MvhdBox::MvhdBox(FullBox &fbox, uint32_t tScale) : FullBox(fbox), timeScale(tScale)
{

}

/**
 * @brief Set TimeScale value
 *
 * @param[in] tScale - TimeScale value
 * @return void
 */
void MvhdBox::setTimeScale(uint32_t tScale)
{
	timeScale = tScale;
}

/**
 * @brief Get TimeScale value
 *
 * @return TimeScale value
 */
uint32_t MvhdBox::getTimeScale()
{
	return timeScale;
}

/**
 * @brief Static function to construct a MvhdBox object
 *
 * @param[in] sz - box size
 * @param[in] ptr - pointer to box
 * @return newly constructed MvhdBox object
 */
MvhdBox* MvhdBox::constructMvhdBox(uint32_t sz, uint8_t *ptr)
{
	uint8_t version = READ_VERSION(ptr);
	uint32_t flags  = READ_FLAGS(ptr);
	uint64_t tScale;

	uint32_t skip = sizeof(uint32_t)*2;
	if (1 == version)
	{
		//Skipping creation_time &modification_time
		skip = sizeof(uint64_t)*2;
	}
	ptr += skip;

	tScale = READ_U32(ptr);

	FullBox fbox(sz, Box::MVHD, version, flags);
	return new MvhdBox(fbox, tScale);
}

/**
 * @brief MdhdBox constructor
 *
 * @param[in] sz - box size
 * @param[in] tScale - TimeScale value
 */
MdhdBox::MdhdBox(uint32_t sz, uint32_t tScale) : FullBox(sz, Box::MDHD, 0, 0), timeScale(tScale)
{

}

/**
 * @brief MdhdBox constructor
 *
 * @param[in] fbox - box object
 * @param[in] tScale - TimeScale value
 */
MdhdBox::MdhdBox(FullBox &fbox, uint32_t tScale) : FullBox(fbox), timeScale(tScale)
{

}

/**
 * @brief Set TimeScale value
 *
 * @param[in] tScale - TimeScale value
 * @return void
 */
void MdhdBox::setTimeScale(uint32_t tScale)
{
	timeScale = tScale;
}

/**
 * @brief Get TimeScale value
 *
 * @return TimeScale value
 */
uint32_t MdhdBox::getTimeScale()
{
	return timeScale;
}

/**
 * @brief Static function to construct a MdhdBox object
 *
 * @param[in] sz - box size
 * @param[in] ptr - pointer to box
 * @return newly constructed MdhdBox object
 */
MdhdBox* MdhdBox::constructMdhdBox(uint32_t sz, uint8_t *ptr)
{
	uint8_t version = READ_VERSION(ptr);
	uint32_t flags  = READ_FLAGS(ptr);
	uint64_t tScale;

	uint32_t skip = sizeof(uint32_t)*2;
	if (1 == version)
	{
		//Skipping creation_time &modification_time
		skip = sizeof(uint64_t)*2;
	}
	ptr += skip;

	tScale = READ_U32(ptr);

	FullBox fbox(sz, Box::MDHD, version, flags);
	return new MdhdBox(fbox, tScale);
}

/**
 * @brief TfdtBox constructor
 *
 * @param[in] sz - box size
 * @param[in] mdt - BaseMediaDecodeTime value
 */
TfdtBox::TfdtBox(uint32_t sz, uint64_t mdt) : FullBox(sz, Box::TFDT, 0, 0), baseMDT(mdt)
{

}

/**
 * @brief TfdtBox constructor
 *
 * @param[in] fbox - box object
 * @param[in] mdt - BaseMediaDecodeTime value
 */
TfdtBox::TfdtBox(FullBox &fbox, uint64_t mdt) : FullBox(fbox), baseMDT(mdt)
{

}

/**
 * @brief Set BaseMediaDecodeTime value
 *
 * @param[in] mdt - BaseMediaDecodeTime value
 * @return void
 */
void TfdtBox::setBaseMDT(uint64_t mdt)
{
	baseMDT = mdt;
}

/**
 * @brief Get BaseMediaDecodeTime value
 *
 * @return BaseMediaDecodeTime value
 */
uint64_t TfdtBox::getBaseMDT()
{
	return baseMDT;
}

/**
 * @brief Static function to construct a TfdtBox object
 *
 * @param[in] sz - box size
 * @param[in] ptr - pointer to box
 * @return newly constructed TfdtBox object
 */
TfdtBox* TfdtBox::constructTfdtBox(uint32_t sz, uint8_t *ptr)
{
	uint8_t version = READ_VERSION(ptr);
	uint32_t flags  = READ_FLAGS(ptr);
	uint64_t mdt ;

	if (1 == version)
	{
		mdt = READ_BMDT64(ptr);
	}
	else
	{
		mdt = (uint32_t)READ_U32(ptr);
	}
	FullBox fbox(sz, Box::TFDT, version, flags);
	return new TfdtBox(fbox, mdt);
}

/**
 * @brief TrunBox constructor
 *
 * @param[in] sz - box size
 * @param[in] mdt - sampleDuration value
 */
TrunBox::TrunBox(uint32_t sz, uint64_t sampleDuration) : FullBox(sz, Box::TRUN, 0, 0), duration(sampleDuration)
{
}

/**
 * @brief TrunBox constructor
 *
 * @param[in] fbox - box object
 * @param[in] mdt - BaseMediaDecodeTime value
 */
TrunBox::TrunBox(FullBox &fbox, uint64_t sampleDuration) : FullBox(fbox), duration(sampleDuration)
{
}

/**
 * @brief Set SampleDuration value
 *
 * @param[in] sampleDuration - Sample Duration value
 * @return void
 */
void TrunBox::setSampleDuration(uint64_t sampleDuration)
{
    duration = sampleDuration;
}

/**
 * @brief Get sampleDuration value
 *
 * @return sampleDuration value
 */
uint64_t TrunBox::getSampleDuration()
{
    return duration;
}

/**
 * @brief Static function to construct a TrunBox object
 *
 * @param[in] sz - box size
 * @param[in] ptr - pointer to box
 * @return newly constructed TrunBox object
 */
TrunBox* TrunBox::constructTrunBox(uint32_t sz, uint8_t *ptr)
{
	const uint32_t TRUN_FLAG_DATA_OFFSET_PRESENT                    = 0x0001;
	const uint32_t TRUN_FLAG_FIRST_SAMPLE_FLAGS_PRESENT             = 0x0004;
	const uint32_t TRUN_FLAG_SAMPLE_DURATION_PRESENT                = 0x0100;
	const uint32_t TRUN_FLAG_SAMPLE_SIZE_PRESENT                    = 0x0200;
	const uint32_t TRUN_FLAG_SAMPLE_FLAGS_PRESENT                   = 0x0400;
	const uint32_t TRUN_FLAG_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT = 0x0800;

	uint8_t version = READ_VERSION(ptr);
	uint32_t flags  = READ_FLAGS(ptr);
	uint64_t sampleDuration = 0;//1001000; //fix-Me
	uint32_t sample_count = 0;
	uint32_t sample_duration = 0;
	uint32_t sample_size = 0;
	uint32_t sample_flags = 0;
	uint32_t sample_composition_time_offset = 0;
	uint32_t discard;
	uint32_t totalSampleDuration = 0;

	uint32_t record_fields_count = 0;

	// count the number of bits set to 1 in the second byte of the flags
	for (unsigned int i=0; i<8; i++)
	{
		if (flags & (1<<(i+8))) ++record_fields_count;
	}

	sample_count = READ_U32(ptr);

	discard = READ_U32(ptr);

	for (unsigned int i=0; i<sample_count; i++)
	{
		if (flags & TRUN_FLAG_SAMPLE_DURATION_PRESENT)
		{
			sample_duration = READ_U32(ptr);
			totalSampleDuration += sample_duration;
		}
		if (flags & TRUN_FLAG_SAMPLE_SIZE_PRESENT)
		{
			sample_size = READ_U32(ptr);
		}
		if (flags & TRUN_FLAG_SAMPLE_FLAGS_PRESENT)
		{
			sample_flags = READ_U32(ptr);
		}
		if (flags & TRUN_FLAG_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT)
		{
			sample_composition_time_offset = READ_U32(ptr);
		}
	}
	FullBox fbox(sz, Box::TRUN, version, flags);
	return new TrunBox(fbox, totalSampleDuration);
}

TfhdBox::TfhdBox(uint32_t sz, uint64_t default_duration) : FullBox(sz, Box::TRUN, 0, 0), duration(default_duration)
{

}

TfhdBox::TfhdBox(FullBox &fbox, uint64_t default_duration) : FullBox(fbox), duration(default_duration)
{

}

void TfhdBox::setSampleDuration(uint64_t default_duration)
{
    duration = default_duration;
}

uint64_t TfhdBox::getSampleDuration()
{
    return duration;
}

/**
 * @brief Static function to construct a TfhdBox object
 *
 * @param[in] sz - box size
 * @param[in] ptr - pointer to box
 * @return newly constructed TfhdBox object
 */
TfhdBox* TfhdBox::constructTfhdBox(uint32_t sz, uint8_t *ptr)
{
    uint8_t version = READ_VERSION(ptr); //8
    uint32_t flags  = READ_FLAGS(ptr); //24

    const uint32_t TFHD_FLAG_BASE_DATA_OFFSET_PRESENT            = 0x00001;
    const uint32_t TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT    = 0x00002;
    const uint32_t TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT     = 0x00008;
    const uint32_t TFHD_FLAG_DEFAULT_SAMPLE_SIZE_PRESENT         = 0x00010;
    const uint32_t TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT        = 0x00020;
    const uint32_t TFHD_FLAG_DURATION_IS_EMPTY                   = 0x10000;
    const uint32_t TFHD_FLAG_DEFAULT_BASE_IS_MOOF                = 0x20000;

    uint32_t TrackId;
    uint32_t BaseDataOffset;
    uint32_t SampleDescriptionIndex;
    uint32_t DefaultSampleDuration;
    uint32_t DefaultSampleSize;
    uint32_t DefaultSampleFlags;

    TrackId = READ_U32(ptr);
    if (flags & TFHD_FLAG_BASE_DATA_OFFSET_PRESENT) {
        BaseDataOffset = READ_64(ptr);
    } else {
        BaseDataOffset = 0;
    }
    if (flags & TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT) {
        SampleDescriptionIndex = READ_U32(ptr);
    } else {
        SampleDescriptionIndex = 1;
    }
    if (flags & TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT) {
        DefaultSampleDuration = READ_U32(ptr);
    } else {
        DefaultSampleDuration = 0;
    }
    if (flags & TFHD_FLAG_DEFAULT_SAMPLE_SIZE_PRESENT) {
        DefaultSampleSize = READ_U32(ptr);
    } else {
        DefaultSampleSize = 0;
    }
    if (flags & TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT) {
        DefaultSampleFlags = READ_U32(ptr);
    } else {
        DefaultSampleFlags = 0;
    }

//    logprintf("TFHD constructTfhdBox TrackId: %d",TrackId );
//    logprintf("TFHD constructTfhdBox BaseDataOffset: %d",BaseDataOffset );
//    logprintf("TFHD constructTfhdBox SampleDescriptionIndex: %d",SampleDescriptionIndex);
//    logprintf("TFHD constructTfhdBox DefaultSampleDuration: %d",DefaultSampleDuration );
//    logprintf("TFHD constructTfhdBox DefaultSampleSize: %d",DefaultSampleSize );
//    logprintf("TFHD constructTfhdBox DefaultSampleFlags: %d",DefaultSampleFlags );

    FullBox fbox(sz, Box::TFHD, version, flags);
    return new TfhdBox(fbox, DefaultSampleDuration);
}
