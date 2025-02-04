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
 * @file drm.h
 * @brief AVE DRM helper declarations
 */

#ifndef DRM_H
#define DRM_H

#include <stddef.h> // for size_t
#include "HlsDrmBase.h"

#define MAX_DRM_CONTEXT 6
#define DRM_SHA1_HASH_LEN 40

#ifdef AVE_DRM
#include "ave-adapter/MyFlashAccessAdapter.h"
#else
/**
 * @enum DrmMethod
 * @brief AVE drm method
 */
typedef enum
{
	eMETHOD_NONE,
	eMETHOD_AES_128, /// encrypted using Advanced Encryption Standard 128-bit key and PKCS7 padding
} DrmMethod;

/**
 * @struct DrmMetadata
 * @brief AVE drm metadata extracted from EXT-X-FAXS-CM
 */
struct DrmMetadata
{ // from EXT-X-FAXS-CM
	unsigned char * metadataPtr;
	size_t metadataSize;
};

/**
 * @struct DrmInfo
 * @brief DRM information required to decrypt
 */
struct DrmInfo
{ // from EXT-X-KEY
	DrmMethod method;
	bool useFirst16BytesAsIV;
	unsigned char *iv; // [16]
	char *uri;
//	unsigned char *CMSha1Hash;// [20]; // unused
//	unsigned char *encryptedRotationKey;
};
#endif /*AVE_DRM*/


/**
 * @class AveDrm
 * @brief Adobe AVE DRM management
 */
class AveDrm : public HlsDrmBase
{
public:
	AveDrm();
	DrmReturn SetMetaData(class PrivateInstanceAAMP *aamp, void* metadata);
	DrmReturn SetDecryptInfo(PrivateInstanceAAMP *aamp, const struct DrmInfo *drmInfo);
	DrmReturn Decrypt(ProfilerBucketType bucketType, void *encryptedDataPtr, size_t encryptedDataLen, int timeInMs);
	void Release();
	void CancelKeyWait();
	void RestoreKeyState();
	void SetState(DRMState state);
	DRMState mDrmState;
private:
	PrivateInstanceAAMP *mpAamp;
	class MyFlashAccessAdapter *m_pDrmAdapter;
	class TheDRMListener *m_pDrmListner;
	DRMState mPrevDrmState;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
};

/**
* @struct	DrmMetadataNode
* @brief	DrmMetadataNode structure for DRM Metadata/Hash storage
*/
struct DrmMetadataNode
{
	DrmMetadata metaData;
	char* sha1Hash;
};

/**
* @class	AveDrmManager
* @brief	Manages AveDrm instances and provide functions for license acquisition and rotation.
* 			Methods are not multi-thread safe. Caller is responsible for synchronization.
*/
class AveDrmManager
{
public:
	static void ResetAll();
	static void CancelKeyWaitAll();
	static void ReleaseAll();
	static void RestoreKeyStateAll();
	static void SetMetadata(PrivateInstanceAAMP *aamp, DrmMetadataNode *metaDataNode);
	static void PrintSha1Hash( char* sha1Hash);
	static AveDrm* GetAveDrm(char* sha1Hash);
	static int GetNewMetadataIndex(DrmMetadataNode* drmMetadataIdx, int drmMetadataCount);
private:
	AveDrmManager();
	void Reset();
	char mSha1Hash[DRM_SHA1_HASH_LEN];
	DrmMetadata mDrmMetadata;
	AveDrm* mDrm;
	bool mDrmContexSet;
	static AveDrmManager sAveDrmManager[MAX_DRM_CONTEXT];
	static int sAveDrmManagerCount;
};

#endif // DRM_H
