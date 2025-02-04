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
 * @file jsmediaplayer.h
 * @brief JavaScript bindings for AAMPMediaPlayer
 */


#ifndef __AAMP_JSMEDIAPLAYER_H__
#define __AAMP_JSMEDIAPLAYER_H__

#include <JavaScriptCore/JavaScript.h>
#include <map>
#include "main_aamp.h"

#define AAMP_UNIFIED_VIDEO_ENGINE_VERSION "0.5"

/**
 * @struct AAMPMediaPlayer_JS
 * @brief Private data structure of AAMPMediaPlayer JS object
 */
struct AAMPMediaPlayer_JS {
	JSGlobalContextRef _ctx;
	PlayerInstanceAAMP* _aamp;

	std::multimap<AAMPEventType, void*> _listeners;
};

#endif /* __AAMP_JSMEDIAPLAYER_H__ */
