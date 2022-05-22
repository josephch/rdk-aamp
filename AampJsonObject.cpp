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
#include <vector>

#include "AampJsonObject.h"
#include "AampUtils.h"
#include "_base64.h"

AampJsonObject::AampJsonObject() : mParent(NULL), mJsonObj()
{
	mJsonObj = cJSON_CreateObject();
}

AampJsonObject::AampJsonObject(const std::string& jsonStr) : mParent(NULL), mJsonObj()
{
	mJsonObj = cJSON_Parse(jsonStr.c_str());

	if (!mJsonObj)
	{
		throw AampJsonParseException();
	}
}

AampJsonObject::~AampJsonObject()
{
	if (!mParent)
	{
		cJSON_Delete(mJsonObj);
	}
}

bool AampJsonObject::add(const std::string& name, const std::string& value, const ENCODING encoding)
{
	bool res = false;

	if (encoding == ENCODING_STRING)
	{
		res = add(name, cJSON_CreateString(value.c_str()));
	}
	else
	{
		res = add(name, std::vector<uint8_t>(value.begin(), value.end()), encoding);
	}

	return res;
}

bool AampJsonObject::add(const std::string& name, const std::vector<std::string>& values)
{
	std::vector<const char*> cstrings;
	for (auto value : values)
	{
		cstrings.push_back(value.c_str());
	}
	return add(name, cJSON_CreateStringArray(&cstrings[0], values.size()));
}

bool AampJsonObject::add(const std::string& name, const std::vector<uint8_t>& values, const ENCODING encoding)
{
	bool res = false;

	switch (encoding)
	{
		case ENCODING_STRING:
		{
			std::string strValue(values.begin(), values.end());
			res = add(name, cJSON_CreateString(strValue.c_str()));
		}
		break;

		case ENCODING_BASE64:
		{
			const char *encodedResponse = base64_Encode((const unsigned char*)&values[0], values.size());
			if (encodedResponse != NULL)
			{
				res = add(name, cJSON_CreateString(encodedResponse));
				free((void*)encodedResponse);
			}
		}
		break;

		case ENCODING_BASE64_URL:
		{
			const char *encodedResponse = aamp_Base64_URL_Encode((const unsigned char*)&values[0], values.size());
			if (encodedResponse != NULL)
			{
				res = add(name, cJSON_CreateString(encodedResponse));
				free((void*)encodedResponse);
			}
		}
		break;

		default:
			/* Unsupported encoding format */
			break;
	}

	return res;
}

bool AampJsonObject::add(const std::string& name, std::vector<AampJsonObject*>& values)
{
	cJSON *arr = cJSON_CreateArray();
	for (auto& obj : values)
	{
		cJSON_AddItemToArray(arr, obj->mJsonObj);
		obj->mParent = this;
	}
	return add(name, arr);
}

bool AampJsonObject::add(const std::string& name, cJSON *value)
{
	if (NULL == value)
	{
		return false;
	}
	cJSON_AddItemToObject(mJsonObj, name.c_str(), value);
	return true;
}


bool AampJsonObject::add(const std::string& name, bool value)
{
	cJSON_AddItemToObject(mJsonObj, name.c_str(), cJSON_CreateBool(value));
	return true;
}

bool AampJsonObject::add(const std::string& name, int value)
{
	cJSON_AddItemToObject(mJsonObj, name.c_str(), cJSON_CreateNumber(value));
	return true;
}

bool AampJsonObject::add(const std::string& name, double value)
{
	cJSON_AddItemToObject(mJsonObj, name.c_str(), cJSON_CreateNumber(value));
	return true;
}

bool AampJsonObject::add(const std::string& name, long value)
{
	cJSON_AddItemToObject(mJsonObj, name.c_str(), cJSON_CreateNumber(value));
	return true;
}

bool AampJsonObject::get(const std::string& name, std::string& value)
{
	cJSON *strObj = cJSON_GetObjectItem(mJsonObj, name.c_str());

	if (strObj)
	{
		char *strValue = cJSON_GetStringValue(strObj);
		if (strValue)
		{
			value = strValue;
			return true;
		}
	}
	return false;
}

bool AampJsonObject::get(const std::string& name, std::vector<uint8_t>& values, const ENCODING encoding)
{
	bool res = false;
	std::string strValue;

	if (get(name, strValue))
	{
		values.clear();

		switch (encoding)
		{
			case ENCODING_STRING:
			{
				values.insert(values.begin(), strValue.begin(), strValue.end());
			}
			break;

			case ENCODING_BASE64:
			{
				size_t decodedSize = 0;
				const unsigned char *decodedResponse = base64_Decode(strValue.c_str(), &decodedSize, strValue.length());
				if (decodedResponse != NULL)
				{
					values.insert(values.begin(), decodedResponse, decodedResponse + decodedSize);
					res = true;
				}
			}
			break;

			case ENCODING_BASE64_URL:
			{
				size_t decodedSize = 0;
				const unsigned char *decodedResponse = aamp_Base64_URL_Decode(strValue.c_str(), &decodedSize, strValue.length());
				if (decodedResponse != NULL)
				{
					values.insert(values.begin(), decodedResponse, decodedResponse + decodedSize);
					res = true;
				}
			}
			break;

			default:
				/* Unsupported encoding format */
				break;
		}
	}
	return res;
}

std::string AampJsonObject::print()
{
	char *jsonString = cJSON_Print(mJsonObj);
	if (NULL != jsonString)
	{
		std::string retStr(jsonString);
		cJSON_free(jsonString);
		return retStr;
	}
	return "";
}

std::string AampJsonObject::print_UnFormatted()
{
	char *jsonString = cJSON_PrintUnformatted(mJsonObj);
	if (NULL != jsonString)
	{
		std::string retStr(jsonString);
		cJSON_free(jsonString);
		return retStr;
	}
	return "";
}

void AampJsonObject::print(std::vector<uint8_t>& data)
{
	std::string jsonOutputStr = print();
	(void)data.insert(data.begin(), jsonOutputStr.begin(), jsonOutputStr.end());
}
