/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 RDK Management
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
 * @file AampcliProcessCommand.h
 * @brief AampcliProcessCommand header file
 */

#ifndef AAMPCLIPROCESSCOMMAND_H
#define AAMPCLIPROCESSCOMMAND_H

#include <string>
#include <map>
#include <vector>
#include <priv_aamp.h>

class Command
{
	public:
		virtual ~Command( )
		{}
		virtual bool execute(char *cmd, PlayerInstanceAAMP *playerInstanceAamp) = 0;
};

class CommandDispatcher
{
	public:
		static std::map<std::string, Command*> mCommandMap;
		void registerAampcliCommands();
		void registerCommand(const std::string& commandName, Command* command);
		bool dispatchAampcliCommands(char *cmd, PlayerInstanceAAMP *playerInstanceAamp);
};

#endif // AAMPCLIPROCESSCOMMAND_H
