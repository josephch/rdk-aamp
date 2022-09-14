#include <iostream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <memory>

#include "AampDRMutils.h"
#include "AampConfig.h"
#include "priv_aamp.h"
#include "aampgstplayer.h"

#include "CppUTest/TestHarness.h"
#include "CppUTestExt/MockSupport.h"

//Enable the define below to get AAMP logging out when running tests
//#define ENABLE_LOGGING
#define TEST_LOG_LEVEL eLOGLEVEL_TRACE

std::shared_ptr<AampConfig> gGlobalConfig;
AampConfig *gpGlobalConfig;
AampLogManager *mLogObj = NULL;

static std::unordered_map<std::string, std::vector<std::string>> fCustomHeaders;

void MockAampReset(void)
{
	gGlobalConfig = std::make_shared<AampConfig>();

	gpGlobalConfig = gGlobalConfig.get();
}

PrivateInstanceAAMP::PrivateInstanceAAMP(AampConfig *config) : mConfig(config), mIsFakeTune(false) {}
PrivateInstanceAAMP::~PrivateInstanceAAMP() {}

void PrivateInstanceAAMP::GetCustomLicenseHeaders(std::unordered_map<std::string, std::vector<std::string>>& customHeaders)
{
	customHeaders = fCustomHeaders;
}

void PrivateInstanceAAMP::SendDrmErrorEvent(DrmMetaDataEventPtr event, bool isRetryEnabled)
{
}

void PrivateInstanceAAMP::SendDRMMetaData(DrmMetaDataEventPtr e)
{
}

void PrivateInstanceAAMP::individualization(const std::string& payload)
{
	mock("Aamp").actualCall("individualization").withStringParameter("payload", payload.c_str());
}

void PrivateInstanceAAMP::SendEvent(AAMPEventPtr eventData, AAMPEventMode eventMode)
{
}

void PrivateInstanceAAMP::SetState(PrivAAMPState state)
{
}

std::string PrivateInstanceAAMP::GetLicenseReqProxy()
{
	return std::string();
}

std::string PrivateInstanceAAMP::GetLicenseServerUrlForDrm(DRMSystems type)
{
	std::string url;
	if (type == eDRM_PlayReady)
	{
		GETCONFIGVALUE_PRIV(eAAMPConfig_PRLicenseServerUrl,url);
	}
	else if (type == eDRM_WideVine)
	{
		GETCONFIGVALUE_PRIV(eAAMPConfig_WVLicenseServerUrl,url);
	}
	else if (type == eDRM_ClearKey)
	{
		GETCONFIGVALUE_PRIV(eAAMPConfig_CKLicenseServerUrl,url);
	}

	if(url.empty())
	{
		GETCONFIGVALUE_PRIV(eAAMPConfig_LicenseServerUrl,url);
	}
	return url;
}

std::string PrivateInstanceAAMP::GetLicenseCustomData()
{
	return std::string();
}

bool PrivateInstanceAAMP::IsEventListenerAvailable(AAMPEventType eventType)
{
	return false;
}

std::string PrivateInstanceAAMP::GetAppName()
{
	return std::string();
}

int PrivateInstanceAAMP::HandleSSLProgressCallback ( void *clientp, double dltotal, double dlnow, double ultotal, double ulnow )
{
	return 0;
}

size_t PrivateInstanceAAMP::HandleSSLHeaderCallback ( const char *ptr, size_t size, size_t nmemb, void* userdata )
{
	return 0;
}

size_t PrivateInstanceAAMP::HandleSSLWriteCallback ( char *ptr, size_t size, size_t nmemb, void* userdata )
{
	return 0;
}

#ifdef USE_SECCLIENT
void PrivateInstanceAAMP::GetMoneyTraceString(std::string &customHeader) const
{
}
#endif

bool AAMPGstPlayer::IsCodecSupported(const std::string &codecName)
{
	return true;
}

void aamp_Free(struct GrowableBuffer *buffer)
{
	if (buffer && buffer->ptr)
	{
		g_free(buffer->ptr);
		buffer->ptr  = NULL;
	}
}

void aamp_AppendBytes(struct GrowableBuffer *buffer, const void *ptr, size_t len)
{
	size_t required = buffer->len + len;
	if (required > buffer->avail)
	{
		// For encoded contents, step by step increment 512KB => 1MB => 2MB => ..
		// In other cases, double the buffer based on availability and requirement.
		buffer->avail = ((buffer->avail * 2) > required) ? (buffer->avail * 2) : (required * 2);
		char *ptr = (char *)g_realloc(buffer->ptr, buffer->avail);
		assert(ptr);
		if (ptr)
		{
			if (buffer->ptr == NULL)
			{ // first alloc (not a realloc)
			}
			buffer->ptr = ptr;
		}
	}
	if (buffer->ptr)
	{
		memcpy(&buffer->ptr[buffer->len], ptr, len);
		buffer->len = required;
	}
}

bool AampLogManager::isLogLevelAllowed(AAMP_LogLevel chkLevel)
{
	return chkLevel >= TEST_LOG_LEVEL;
}

std::string AampLogManager::getHexDebugStr(const std::vector<uint8_t>& data)
{
	std::ostringstream hexSs;
	hexSs << "0x";
	hexSs << std::hex << std::uppercase << std::setfill('0');
	std::for_each(data.cbegin(), data.cend(), [&](int c) { hexSs << std::setw(2) << c; });
	return hexSs.str();
}

void AampLogManager::setLogLevel(AAMP_LogLevel newLevel)
{
}

void logprintf(const char *format, ...)
{
#ifdef ENABLE_LOGGING
	int len = 0;
	va_list args;
	va_start(args, format);

	char gDebugPrintBuffer[MAX_DEBUG_LOG_BUFF_SIZE];
	len = sprintf(gDebugPrintBuffer, "[AAMP-PLAYER]");
	vsnprintf(gDebugPrintBuffer+len, MAX_DEBUG_LOG_BUFF_SIZE-len, format, args);
	gDebugPrintBuffer[(MAX_DEBUG_LOG_BUFF_SIZE-1)] = 0;

	std::cout << gDebugPrintBuffer << std::endl;

	va_end(args);
#endif
}

void logprintf_new(int playerId, const char* levelstr, const char* file, int line, const char *format, ...)
{
#ifdef ENABLE_LOGGING
	int len = 0;
	va_list args;
	va_start(args, format);

	char gDebugPrintBuffer[MAX_DEBUG_LOG_BUFF_SIZE];
	len = sprintf(gDebugPrintBuffer, "[AAMP-PLAYER][%d][%s][%s][%d]", playerId, levelstr, file, line);
	vsnprintf(gDebugPrintBuffer+len, MAX_DEBUG_LOG_BUFF_SIZE-len, format, args);
	gDebugPrintBuffer[(MAX_DEBUG_LOG_BUFF_SIZE-1)] = 0;

	std::cout << gDebugPrintBuffer << std::endl;

	va_end(args);
#endif
}

void DumpBlob(const unsigned char *ptr, size_t len)
{
}
