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
 * @file ThunderAccess.cpp
 * @brief wrapper class for accessing thunder plugins
 */
#include "priv_aamp.h"
#include <core/core.h>
#include <websocket/websocket.h>
#include <securityagent/SecurityTokenUtil.h>
#include <ThunderAccess.h>

using namespace std;
using namespace WPEFramework;

#define SERVER_DETAILS  "127.0.0.1:9998"
#define MAX_LENGTH 1024
#define THUNDER_RPC_TIMEOUT 5000 

/**
 *  * @brief Structure to save the Thunder security token details
 **/
typedef struct ThunderSecurity
{
    std::string securityToken;
    int tokenStatus;
    bool tokenQueried;
}ThunderSecurityData;

ThunderSecurityData gSecurityData;
/**
 *   @brief  ThunderAccessAAMP constructor
 *   @note   Security tocken accusition, controller object creation
 *   @param  NA
 *   @retval NA
 *   @retval NA
 */
ThunderAccessAAMP::ThunderAccessAAMP(std::string callsign)
                 : remoteObject(NULL),
                   controllerObject(NULL),
                   pluginCallsign(callsign)
{
    AAMPLOG_INFO( "[ThunderAccessAAMP]Inside %s ", __FUNCTION__ );

    uint32_t status = Core::ERROR_NONE;

    Core::SystemInfo::SetEnvironment(_T("THUNDER_ACCESS"), (_T(SERVER_DETAILS)));

    if(!gSecurityData.tokenQueried)
    {
        unsigned char buffer[MAX_LENGTH] = {0};
        gSecurityData.tokenStatus = GetSecurityToken(MAX_LENGTH,buffer);
        if(gSecurityData.tokenStatus > 0){
            AAMPLOG_INFO( "[ThunderAccessAAMP] %s : GetSecurityToken success", __FUNCTION__ );
            string sToken = (char*)buffer;
            gSecurityData.securityToken = "token=" + sToken;
        }
        gSecurityData.tokenQueried = true;

        //AAMPLOG_WARN( "[ThunderAccessAAMP] %s : securityToken : %s tokenStatus : %d tokenQueried : %s", __FUNCTION__, gSecurityData.securityToken.c_str(), gSecurityData.tokenStatus, ((gSecurityData.tokenQueried)?"true":"false"));
    }

    if (NULL == controllerObject) {
        /*Passing empty string instead of Controller callsign.This is assumed as controller plugin.*/
        if(gSecurityData.tokenStatus > 0){
            controllerObject = new JSONRPC::LinkType<Core::JSON::IElement>(_T(""), _T(""), false, gSecurityData.securityToken);
        }
        else{
            controllerObject = new JSONRPC::LinkType<Core::JSON::IElement>(_T(""));
        }

        if (NULL == controllerObject) {
            AAMPLOG_WARN( "[ThunderAccessAAMP] %s : Controller object creation failed", __FUNCTION__ );
        } else {
            AAMPLOG_INFO( "[ThunderAccessAAMP] %s : Controller object creation success", __FUNCTION__ );
        }
    }

    if(gSecurityData.tokenStatus > 0){
        remoteObject = new JSONRPC::LinkType<Core::JSON::IElement>(_T(pluginCallsign), _T(""), false, gSecurityData.securityToken);
    }
    else{
        remoteObject = new JSONRPC::LinkType<Core::JSON::IElement>(_T(pluginCallsign), _T(""));
    }
    if (NULL == remoteObject) {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : %s Client initialization failed", __FUNCTION__, pluginCallsign.c_str());
    } else {
        AAMPLOG_INFO( "[ThunderAccessAAMP] %s : %s Client initialization success", __FUNCTION__, pluginCallsign.c_str());
    }
}

/**
 *   @brief  ThunderAccessAAMP destructor
 *   @note   clean up
 *   @param  NA
 *   @retval NA
 *   @retval NA
 */
ThunderAccessAAMP::~ThunderAccessAAMP()
{
    if(NULL != controllerObject)
    {
        delete controllerObject;
        controllerObject = NULL;
    }
    if(NULL != remoteObject)
    {
        delete remoteObject;
        remoteObject = NULL;
    }
}

/**
 *   @brief  ActivatePlugin
 *   @note   Plugin activation and Remote object creation
 *   @param  Plugin Callsign
 *   @retval true on success
 *   @retval false on failure
 */
bool ThunderAccessAAMP::ActivatePlugin()
{
    bool ret = true;
    JsonObject result;
    JsonObject controlParam;
    std::string response;
    uint32_t status = Core::ERROR_NONE;

    if (NULL != controllerObject) {
        controlParam["callsign"] = pluginCallsign;
        status = controllerObject->Invoke<JsonObject, JsonObject>(THUNDER_RPC_TIMEOUT, _T("activate"), controlParam, result);
        if (Core::ERROR_NONE == status){
            result.ToString(response);
            AAMPLOG_INFO( "[ThunderAccessAAMP] %s plugin Activated. Response : %s ", pluginCallsign.c_str(), response.c_str());
        }
        else
        {
            AAMPLOG_WARN( "[ThunderAccessAAMP] %s plugin Activation failed with error status : %u ", pluginCallsign.c_str(), status);
            ret = false;
        }
    } else {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : Controller Object NULL ", __FUNCTION__);
        ret = false;
    }

    return ret;
}

/**
 *   @brief  subscribeEvent
 *   @note   Subscribe event data for the specific plugin
 *   @param  Event name, Event handler
 *   @retval true on success
 *   @retval false on failure
 */
/*To Do: Only JSON Object can be used as parameter now*/
bool ThunderAccessAAMP::SubscribeEvent (string eventName, std::function<void(const WPEFramework::Core::JSON::VariantContainer&)> functionHandler)
{
    bool ret = true;
    uint32_t status = Core::ERROR_NONE;
    if (NULL != remoteObject) {
        status = remoteObject->Subscribe<JsonObject>(THUNDER_RPC_TIMEOUT, _T(eventName), functionHandler);
        if (Core::ERROR_NONE == status) {
            AAMPLOG_INFO( "[ThunderAccessAAMP] %s : Subscribed to : %s", __FUNCTION__, eventName.c_str());
        } else {
            AAMPLOG_WARN( "[ThunderAccessAAMP] %s : Subscription failed for : %s with error status %u", __FUNCTION__, eventName.c_str(), status);
            ret = false;
        }
    } else {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : remoteObject not created for the plugin!", __FUNCTION__ );
        ret = false;
    }
    return ret;
}

/**
 *   @brief  unSubscribeEvent
 *   @note   unSubscribe event data for the specific plugin
 *   @param  Event name
 *   @retval true on success
 *   @retval false on failure
 */
/*To Do: Only JSON Object can be used as parameter now*/
bool ThunderAccessAAMP::UnSubscribeEvent (string eventName)
{
    bool ret = true;
    if (NULL != remoteObject) {
        remoteObject->Unsubscribe(THUNDER_RPC_TIMEOUT, _T(eventName));
        AAMPLOG_INFO( "[ThunderAccessAAMP] %s : UnSubscribed : %s event", __FUNCTION__, eventName.c_str());
    } else {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : remoteObject not created for the plugin!", __FUNCTION__ );
        ret = false;
    }
    return ret;
}

/**
 *   @brief  invokeJSONRPC
 *   @note   Invoke JSONRPC call for the plugin
 *   @param  Plugin method, reference to input pram and result
 *   @retval true on success
 *   @retval false on failure
 */
bool ThunderAccessAAMP::InvokeJSONRPC(std::string method, const JsonObject &param, JsonObject &result)
{
    bool ret = true;
    std::string response;
    uint32_t status = Core::ERROR_NONE;

    if(NULL == remoteObject)
    {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : client not initialized! ", __FUNCTION__ );
        return false;
    }
    status = remoteObject->Invoke<JsonObject, JsonObject>(THUNDER_RPC_TIMEOUT, _T(method), param, result);
    if (Core::ERROR_NONE == status)
    {
        if (result["success"].Boolean()) {
            result.ToString(response);
            AAMPLOG_INFO( "[ThunderAccessAAMP] %s success! Response : %s", method.c_str() , response.c_str());
        }
        else
        {
            result.ToString(response);
            AAMPLOG_WARN( "[ThunderAccessAAMP] %s call failed! Response : %s", method.c_str() , response.c_str());
            ret = false;
        }
    }
    else
    {
        AAMPLOG_WARN( "[ThunderAccessAAMP] %s : invoke failed with error status %u", method.c_str(), status);
        ret = false;
    }
    return ret;
}
