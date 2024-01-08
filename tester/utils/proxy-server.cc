/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2024 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "proxy-server.hh"

#include <algorithm>
#include <optional>

#include "bctoolbox/tester.h"
#include "sofia-sip/nta_tport.h"

#include "registrar/registrar-db.hh"
#include "tester.hh"
#include "utils/client-builder.hh"

using namespace std;
using namespace std::chrono;

namespace flexisip {
namespace tester {

const char* getFirstPort(const Agent& agent) {
	const auto firstTransport = ::tport_primaries(::nta_agent_tports(agent.getSofiaAgent()));
	return ::tport_name(firstTransport)->tpn_port;
}

/**
 * A class to manage the flexisip proxy server
 */
Server::Server(const std::string& configFile, InjectedHooks* injectedHooks)
    : mInjectedModule(injectedHooks ? decltype(mInjectedModule){*injectedHooks} : std::nullopt) {

	if (!configFile.empty()) {
		auto configFilePath = bcTesterRes(configFile);
		int ret = -1;
		if (bctbx_file_exist(configFilePath.c_str()) == 0) {
			ret = mConfigManager->load(configFilePath);
		} else {
			ret = mConfigManager->load(bcTesterRes(configFile));
		}
		if (ret != 0) {
			BC_FAIL("Unable to load configuration file");
		}

		// For testing purposes, override the auth file path to be relative to the config file.
		const auto& configFolderPath = configFilePath.substr(0, configFilePath.find_last_of('/') + 1);
		auto authFilePath = mConfigManager->getRoot()
		                        ->get<flexisip::GenericStruct>("module::Authentication")
		                        ->get<flexisip::ConfigString>("file-path");
		authFilePath->set(configFolderPath + authFilePath->read());
	}

	mAuthDbOwner = std::make_shared<AuthDbBackendOwner>(mConfigManager);
	mAgent = std::make_shared<Agent>(std::make_shared<sofiasip::SuRoot>(), mConfigManager, mAuthDbOwner);
}

Server::Server(const std::map<std::string, std::string>& customConfig, InjectedHooks* injectedHooks)
    : mInjectedModule(injectedHooks ? decltype(mInjectedModule){*injectedHooks} : std::nullopt) {
	mConfigManager->load("");

	// add minimal config if not present
	auto config = customConfig;
	config.merge(map<string, string>{// Requesting bind on port 0 to let the kernel find any available port
	                                 {"global/transports", "sip:127.0.0.1:0"},
	                                 {"module::Registrar/reg-domains", "sip.example.org"}});
	for (const auto& kv : config) {
		const auto& key = kv.first;
		const auto& value = kv.second;
		auto slashPos = key.find('/');
		if (slashPos == decay_t<decltype(key)>::npos) {
			throw invalid_argument{"missing '/' in parameter name [" + key + "]"};
		}
		if (slashPos == key.size() - 1) {
			throw invalid_argument{"invalid parameter name [" + key + "]: forbidden ending '/'"};
		}
		auto sectionName = key.substr(0, slashPos);
		auto parameterName = key.substr(slashPos + 1);
		mConfigManager->getRoot()->get<GenericStruct>(sectionName)->get<ConfigValue>(parameterName)->set(value);
	}

	mAuthDbOwner = std::make_shared<AuthDbBackendOwner>(mConfigManager);
	mAgent = std::make_shared<Agent>(std::make_shared<sofiasip::SuRoot>(), mConfigManager, mAuthDbOwner);
}

Server::~Server() {
	mAgent->unloadConfig();
	RegistrarDb::resetDB();
}

void Server::runFor(std::chrono::milliseconds duration) {
	auto beforePlusDuration = steady_clock::now() + duration;
	while (beforePlusDuration >= steady_clock::now()) {
		mAgent->getRoot()->step(100ms);
	}
}
const char* Server::getFirstPort() const {
	return tester::getFirstPort(*mAgent);
}

} // namespace tester
} // namespace flexisip
