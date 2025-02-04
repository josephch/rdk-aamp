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
 * @file aamplogging.cpp
 * @brief AAMP logging mechanisum source file
 */

#include "priv_aamp.h"

#ifndef WIN32
#ifdef USE_SYSLOG_HELPER_PRINT
#include "syslog_helper_ifc.h"
#endif
#ifdef USE_SYSTEMD_JOURNAL_PRINT
#include <systemd/sd-journal.h>
#endif
#endif

/**
 * @brief Max bebug log buffer size
 */
#define MAX_DEBUG_LOG_BUFF_SIZE 1024

/**
 * @brief Log file directory index - To support dynamic directory configuration for aamp logging
 */
char gLogDirectory[] = "c:/tmp/aamp.log";

/*-----------------------------------------------------------------------------------------------------*/

/**
 * @brief To check the given log level is allowed to print mechanism
 * @param[in] chkLevel - log level
 * @retval true if the log level allowed for print mechanism
 */
bool AampLogManager::isLogLevelAllowed(AAMP_LogLevel chkLevel)
{
	return (chkLevel>=aampLoglevel);
}

/**
 * @brief Set the log level for print mechanism
 * @param[in] newLevel - log level new value
 * @retuen void
 */
void AampLogManager::setLogLevel(AAMP_LogLevel newLevel)
{
	if(!info && !debug)
		aampLoglevel = newLevel;
}

/**
 * @brief Set the simulator log file directory index.
 */
void AampLogManager::setLogDirectory(char driveName) {
  gLogDirectory[0] = driveName;
}

/**
 * @brief Print the network latency level logging for triage purpose
 * @param[in] url - content url
 * @param[in] downloadTime - download time of the fragment or manifest
 * @param[in] downloadThresholdTimeoutMs - specified download threshold time out value
 * @retuen void
 */
void AampLogManager::LogNetworkLatency(const char* url, int downloadTime, int downloadThresholdTimeoutMs)
{
	std::string contentType;
	std::string location;
	std::string symptom;

	ParseContentUrl(url, contentType, location, symptom);

	logprintf ("AAMPLogNetworkLatency RDK-10043 downloadTime=%d downloadThreshold=%d type='%s' location='%s' symptom='%s' url='%s'\n",
		downloadTime, downloadThresholdTimeoutMs, contentType.c_str(), location.c_str(), symptom.c_str(), url);
}

/**
 * @brief Print the network error level logging for triage purpose
 * @param[in] url - content url
 * @param[in] errorType - it can be http or curl errors
 * @param[in] errorCode - it can be http error or curl error code
 * @retuen void
 */
void AampLogManager::LogNetworkError(const char* url, AAMPNetworkErrorType errorType, int errorCode)
{
	std::string contentType;
	std::string location;
	std::string symptom;

	ParseContentUrl(url, contentType, location, symptom);

	switch(errorType)
	{
		case AAMPNetworkErrorHttp:
		{
			if(errorCode >= 400)
			{
				logprintf("AAMPLogNetworkError RDK-10044 error='http error %d' type='%s' location='%s' symptom='%s' url='%s'\n",
					errorCode, contentType.c_str(), location.c_str(), symptom.c_str(), url );
			}
		}
			break; /*AAMPNetworkErrorHttp*/

		case AAMPNetworkErrorTimeout:
		{
			if(errorCode > 0)
			{
				logprintf("AAMPLogNetworkError RDK-10044 error='timeout %d' type='%s' location='%s' symptom='%s' url='%s'\n",
					errorCode, contentType.c_str(), location.c_str(), symptom.c_str(), url );
			}
		}
			break; /*AAMPNetworkErrorTimeout*/

		case AAMPNetworkErrorCurl:
		{
			if(errorCode > 0)
			{
				logprintf("AAMPLogNetworkError RDK-10044 error='curl error %d' type='%s' location='%s' symptom='%s' url='%s'\n",
					errorCode, contentType.c_str(), location.c_str(), symptom.c_str(), url );
			}
		}
			break; /*AAMPNetworkErrorCurl*/
	}
}

/**
 * @brief To get the issue symptom based on the error type for triage purpose
 * @param[in] url - content url
 * @param[out] contentType - it could be a manifest or other audio/video/iframe tracks
 * @param[out] location - server location
 * @param[out] symptom - issue exhibiting scenario for error case
 * @retuen void
 */
void AampLogManager::ParseContentUrl(const char* url, std::string& contentType, std::string& location, std::string& symptom)
{
	contentType="unknown";
	location="unknown";
	symptom="unknown";

	if(strstr(url,".m3u8") || strstr(url,".mpd"))
	{
		if(strstr(url,"-bandwidth-"))
		{
			contentType = "sub manifest";
			symptom = "freeze/buffering";
		}
		else
		{
			contentType = "main manifest";
			symptom = "video fails to start, has delayed start or freezes/buffers";
		}
	}
	else if(strstr(url,".ts") || strstr(url,".mp4"))
	{
		if(strstr(url, "-header"))
		{
			contentType = "sub manifest";
			symptom = "freeze/buffering";
		}
		if(strstr(url,"-iframe"))
		{
			contentType = "iframe";
			symptom = "trickplay ends or freezes";
		}
		else if(strstr(url,"-muxed"))
		{
			contentType = "muxed segment";
			symptom = "freeze/buffering";
		}
		else if(strstr(url,"-video"))
		{
			contentType = "video segment";
			symptom = "freeze/buffering";
		}
		else if(strstr(url,"-audio"))
		{
			contentType = "audio segment";
			symptom = "freeze/buffering";
		}
	}

	if(strstr(url,"//mm."))
	{
		location = "manifest manipulator";
	}
	else if(strstr(url,"//odol"))
	{
		location = "edge cache";
	}
}

/**
 * @brief Print the DRM error level logging for triage purpose
 * @param[in] major - drm major error code
 * @param[in] minor - drm minor error code
 * @retuen void
 */
void AampLogManager::LogDRMError(int major, int minor)
{
	std::string description;

	switch(major)
	{
		case 3307: /* Internal errors */
		{
			if(minor == 268435462)
			{
				description = "Missing drm keys. Files are missing from /opt/drm. This could happen if socprovisioning fails to pull keys from fkps. This could also happen with a new box type that isn't registered with fkps. Check the /opt/logs/socprov.log for error. Contact ComSec for help.";
			}
			else if(minor == 570425352)
			{
				description = "Stale cache data. There is bad data in adobe cache at /opt/persistent/adobe. This can happen if the cache isn't cleared by /lib/rdk/cleanAdobe.sh after either an FKPS key update or a firmware update. This should not be happening in the field. For engineers, they can try a factory reset to fix the problem.";
			}
			else if(minor == 1000022)
			{
				description = "Local cache directory not readable. The Receiver running as non-root cannot access and read the adobe cache at /opt/persistent/adobe. This can happen if /lib/rdk/prepareChrootEnv.sh fails to set that folders privileges. Run ls -l /opt/persistent and check the access rights. Contact the SI team for help. Also see jira XRE-6687";
			}
		}
			break; /* 3307 */

		case 3321: /* Individualization errors */
		{
			if(minor == 102)
			{
				description = "Invalid signiture request on the Adobe individualization request. Expired certs can cause this, so the first course of action is to verify if the certs, temp baked in or production fkps, have not expired. See table at bottom of https://www.teamccp.com/confluence/display/CPE/Certificate+Library.";
			}
			else if(minor == 10100)
			{
				description = "Unknown Device class error from the Adobe individualization server. The drm certs may be been distributed to comcast security team for inclusion in fkps, but Adobe has not yet added the device info to their indi server.";
			}
			else if(minor == 1107296357)
			{
				description = "Failed to connect to individualization server. This can happen if the network goes down. This can also happen if bad proxy settings exist in /opt/xreproxy.conf. Check the receiver.log for the last HttpRequestBegin before the error occurs and check the host name in the url, then check your proxy conf";
			}

			if(minor == 1000595) /* RequiredSPINotAvailable */
			{
				/* This error doesn't tell us anything useful but points to some other underlying issue.
				 * Don't report this error. Ensure a triage log is being create for the underlying issue 
				 */

				return;
			}
		}
			break; /* 3321 */

		case 3322: /* Device binding failure */
		{
			description = "Device binding failure. DRM data cached by the player at /opt/persistent/adobe, may be corrupt, missing, or innaccesible due to file permision. Please check this folder. A factory reset may be required to fix this and force a re-individualization of the box to reset that data.";
		}
			break; /* 3322 */

		case 3328:
		{
			if(minor == 1003532)
			{
				description = "Potential server issue. This could happen if drm keys are missing or bad. To attempt a quick fix: Back up /opt/drm and /opt/persistent/adobe, perform a factory reset, and see if that fixes the issue. Reach out to ComSec team for help diagnosing the error.";
			}
		}
			break; /* 3328 */

		case 3329: /* Application errors (our consec errors) */
		{
			description = "Comcast license server error response. This could happen for various reasons: bad cache data, bad session token, any license related issue. To attempt a quick fix: Back up /opt/drm and /opt/persistent/adobe, perform a factory reset, and see if that fixes the issue. Reach out to ComSec team for help diagnosing the error.";
		}
			break; /* 3329 */

		case 3338: /* Unknown connection type */
		{
			description = "Unknown connection type. Rare issue related to output protection code not being implemented on certain branches or core or for new socs. See STBI-6542 for details. Reach out to Receiver IP-Video team for help.";
		}
			break; /* 3338 */
	}

	if(description.empty())
	{
		description = "Unrecognized error. Please report this to the STB IP-Video team.";
	}

	logprintf("AAMPLogDRMError RDK-10041 error=%d.%d description='%s'\n", major, minor, description.c_str());
}

/**
 * @brief Print logs to console / log file
 * @param[in] format - printf style string
 * @retuen void
 */
void logprintf(const char *format, ...)
{
	va_list args;
	va_start(args, format);

	char gDebugPrintBuffer[MAX_DEBUG_LOG_BUFF_SIZE];
	vsnprintf(gDebugPrintBuffer, MAX_DEBUG_LOG_BUFF_SIZE, format, args);
	gDebugPrintBuffer[(MAX_DEBUG_LOG_BUFF_SIZE-1)] = 0;

	va_end(args);

#if (!defined STANDALONE_AAMP) && (defined (USE_SYSTEMD_JOURNAL_PRINT) || defined (USE_SYSLOG_HELPER_PRINT))
#ifdef USE_SYSTEMD_JOURNAL_PRINT
	sd_journal_print(LOG_NOTICE, "%s", gDebugPrintBuffer);
#else
	send_logs_to_syslog(gDebugPrintBuffer);
#endif

#else  //USE_SYSTEMD_JOURNAL_PRINT
#ifdef WIN32
	static bool init;

	FILE *f = fopen(gLogDirectory, (init ? "a" : "w"));
	if (f)
	{
		init = true;
		fputs(gDebugPrintBuffer, f);
		fclose(f);
	}

	printf("%s", gDebugPrintBuffer);
#else
	struct timeval t;
	gettimeofday(&t, NULL);
	printf("%ld:%3ld : %s", (long int)t.tv_sec, (long int)t.tv_usec / 1000, gDebugPrintBuffer);
#endif
#endif
}

