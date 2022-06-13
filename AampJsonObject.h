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
#ifndef _AAMP_JSON_OBJECT_H
#define _AAMP_JSON_OBJECT_H

#include <string>
#include <vector>

#include <cjson/cJSON.h>

/*
 * Utility class to construct a JSON string
 */
class AampJsonObject {
public:
	AampJsonObject();
	AampJsonObject(const std::string& jsonStr);
	~AampJsonObject();
	AampJsonObject(const AampJsonObject&) = delete;
	AampJsonObject& operator=(const AampJsonObject&) = delete;

	enum ENCODING
	{
		ENCODING_STRING,	 // Bytes encoded as a string as-is
		ENCODING_BASE64,     // Bytes base64 encoded to a string
		ENCODING_BASE64_URL  // Bytes base64url encoded to a string
	};

	/**
	 *  @brief Add a string value
	 *
	 *  @param name name for the value
	 *  @param value string value to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, const std::string& value, const ENCODING encoding = ENCODING_STRING);

	/**
	 *  @brief Add a vector of string values as a JSON array
	 *
	 *  @param name name for the array
	 *  @param values vector of strings to add as an array
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, const std::vector<std::string>& values);

	/**
	 *  @brief Add the provided bytes after encoding in the specified encoding
	 *
	 *  @param name name for the value
	 *  @param values vector of bytes to add in the specified encoding
	 *  @param encoding how to encode the byte array
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, const std::vector<uint8_t>& values, const ENCODING encoding = ENCODING_STRING);

	/**
	 *  @brief Add a vector of #AampJsonObject as a JSON array
	 *
	 *  @param name name for the array
	 *  @param values vector of #AampJsonObject to add as an array
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, std::vector<AampJsonObject*>& values);
	
	/**
	 *  @brief Get a vector of String from a JSON array
	 *
	 *  @param name name for the array
	 *  @param[out] values String Array
	 *  @return true if successfully added, false otherwise
	 */
	bool get(const std::string& name, std::vector<std::string>& values);
	
	/**
	 *  @brief Get a int value from a JSON data
	 *
	 *  @param name name for the property
	 *  @param[out] values int value
	 *  @return true if successfully added, false otherwise
	 */
	bool get(const std::string& name, int& value);
	
	/**
	 * @brief Get a string value
	 *
	 * @param name name of the property to retrieve
	 * @param value string to populate with the retrieved value
	 * @return true if successfully retrieved value, false otherwise
	 */
	bool get(const std::string& name, std::string& value);

	/**
	 * @brief Get a string value as a vector of bytes
	 *
	 * @param name name of the property to retrieve
	 * @param values vector to populate with the retrieved value
	 * @param encoding the encoding of the string, used to determine how to decode the content into the vector
	 * @return true if successfully retrieved and decoded value, false otherwise
	 */
	bool get(const std::string& name, std::vector<uint8_t>& values, const ENCODING encoding = ENCODING_STRING);

	/**
	 * @fn get
	 * @brief Get the AampJson object from json data within the Json data
	 *
	 * @param name Name of the internal json data
	 * @param value[out] reference Object which return as json object inside json data.
	 * @return true if successfully retrieved and decoded value, false otherwise
	 */
	bool get(const std::string& name, AampJsonObject &value);

	/**
	 *  @brief Print the constructed JSON to a string
	 *
	 *  @return JSON string
	 */
	std::string print();


	/**
	 *  @brief Print the constructed JSON to a string
	 *
	 *  @return JSON string
	*/
	std::string print_UnFormatted();

	/**
	 *  @brief Print the constructed JSON into the provided vector
	 *
	 *  @param[out] data - vector to output printed JSON to
	 *  @return	 void.
	 */
	void print(std::vector<uint8_t>& data);
	/**
	 *  @brief Add a bool value
	 *
	 *  @param name name for the value
	 *  @param value bool to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, bool value);	
	/**
	 *  @brief Add a int value
	 *
	 *  @param name name for the value
	 *  @param value int to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, int value);
	
	/**
	 *  @brief Add a double value
	 *
	 *  @param name name for the value
	 *  @param value double to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, double value);
	
	/**
	 *  @brief Add a long value
	 *
	 *  @param name name for the value
	 *  @param value long to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, long value);
	
	/**
	 *  @brief Add a long value
	 *
	 *  @param name name for the value
	 *  @param value long to add
	 *  @return true if successfully added, false otherwise
	 */
	bool add(const std::string& name, cJSON *value);

	/**
	 * @fn isArray
	 * 
	 * @brief Check whether the value is Array or not
	 * @return true if it is Array or false
	 */
	bool isArray(const std::string& name);

	/**
	 * @fn isString
	 * 
	 * @brief Check whether the value is String or not
	 * @return true if it is String or false
	 */
	bool isString(const std::string& name);

	/**
	 * @fn isNumber
	 * 
	 * @brief Check whether the value is Number or not
	 * @return true if it is Number or false
	 */
	bool isNumber(const std::string& name);

	/**
	 * @fn isObject
	 * 
	 * @brief Check whether the value is Object or not
	 * @return true if it is Object or false
	 */
	bool isObject(const std::string& name);

private:
	bool set(AampJsonObject *parent, cJSON *object);
	bool add(const std::string& name);
	cJSON *mJsonObj;
	AampJsonObject *mParent;
};

class AampJsonParseException : public std::exception
{
public:
	virtual const char* what() const throw()
	{
		return "Failed to parse JSON string";
	}
};

#endif //_AAMP_JSON_OBJECT_H
