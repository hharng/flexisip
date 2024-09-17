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

#include "client-builder.hh"

#include <memory>
#include <stdexcept>

#include <linphone/core.h>

#include "flexisip/flexisip-version.h"
#include "pushnotification/rfc8599-push-params.hh"
#include "tester.hh"
#include "utils/client-core.hh"
#include "utils/core-assert.hh"
#include "utils/string-utils.hh"
#include "utils/variant-utils.hh"

namespace flexisip::tester {

ClientBuilder::ClientBuilder(const Agent& agent)
    : mFactory(linphone::Factory::get()), mCoreTemplate(tester::minimalCore(*mFactory)),
      mAccountParams(mCoreTemplate->createAccountParams()), mAgent(agent), mLimeX3DH(OnOff::On), mSendVideo(OnOff::Off),
      mReceiveVideo(OnOff::Off), mSendRtcp(OnOff::On), mIce(OnOff::Off), mRegister(OnOff::On) {
}

CoreClient ClientBuilder::build(const std::string& baseAddress) const {
	const std::string& me = StringUtils::startsWith(baseAddress, "sip:") ? baseAddress : "sip:" + baseAddress;
	auto myAddress = mFactory->createAddress(me);
	if (!myAddress) {
		std::ostringstream msg{};
		msg << "Invalid contact adress '" << me << "'";
		bc_assert(__FILE__, __LINE__, false, msg.str().c_str());
		throw std::invalid_argument{msg.str()};
	}

	auto core = minimalCore(*mFactory);
	core->setLabel(me);
	core->setPrimaryContact(me);
	core->setUserAgent("LinphoneSDK for Flexisip regression tests", FLEXISIP_GIT_VERSION);

	auto accountParams = mAccountParams->clone();
	accountParams->setIdentityAddress(myAddress);
	accountParams->enableRegister(bool(mRegister));
	{
		// Clients register to the first of the list of transports read in the proxy configuration
		auto route = mFactory->createAddress(mAgent.getConfigManager()
		                                         .getRoot()
		                                         ->get<flexisip::GenericStruct>("global")
		                                         ->get<flexisip::ConfigStringList>("transports")
		                                         ->read()
		                                         .front());
		// Fix port if auto-bound
		if (route->getPort() == 0) {
			route->setPort(std::atoi(getFirstPort(mAgent)));
		}

		accountParams->setServerAddress(route);
		accountParams->setRoutesAddresses({route});
	}
	BC_ASSERT(accountParams->outboundProxyEnabled());
	auto account = core->createAccount(accountParams);
	core->addAccount(account);
	core->setDefaultAccount(account);
	core->enablePushNotification(accountParams->getPushNotificationAllowed());

	if (!mPassword.empty()) {
		core->addAuthInfo(
		    mFactory->createAuthInfo(myAddress->getUsername(), "", mPassword, "", "", myAddress->getDomain()));
	}

	core->enableLimeX3Dh(bool(mLimeX3DH));

	{
		auto config = core->getConfig();
		config->setString("storage", "backend", "sqlite3");
		config->setString("storage", "uri", ":memory:");
		config->setBool("rtp", "rtcp_enabled", bool(mSendRtcp));
		config->setBool("sip", "inactive_audio_on_pause", static_cast<bool>(mSetAudioInactiveOnPause));
	}

	Match(mAudioPort)
	    .against([&core](port::Range range) { core->setAudioPortRange(range.min, range.max); },
	             [&core](port::Port port) { core->setAudioPort(port.port); },
	             [&core](port::Auto) { core->setAudioPort(LC_SIP_TRANSPORT_RANDOM); });
	Match(mVideoPort)
	    .against([&core](port::Range range) { core->setVideoPortRange(range.min, range.max); },
	             [&core](port::Port port) { core->setVideoPort(port.port); },
	             [&core](port::Auto) { core->setVideoPort(LC_SIP_TRANSPORT_RANDOM); });
	core->setUseFiles(true);
	// final check on call successfully established is based on bandwidth used,
	// so use file as input to make sure there is some traffic
	{
		auto helloPath = bcTesterRes("sounds/hello8000.wav");
		if (bctbx_file_exist(helloPath.c_str()) != 0) {
			BC_FAIL("Unable to find resource sound, did you forget to use --resource-dir option?");
		} else {
			core->setPlayFile(helloPath);
		}
	}

	{
		auto policy = mFactory->createVideoActivationPolicy();
		policy->setAutomaticallyAccept(true);
		policy->setAutomaticallyInitiate(false); // requires explicit setting in the parameters to initiate a video call
		core->setVideoActivationPolicy(policy);
	}

	if (bool(mSendVideo)) {
		auto* msFactory = linphone_core_get_ms_factory(core->cPtr());
		auto* webCamMan = ms_factory_get_web_cam_manager(msFactory);
		auto* mire_desc = ms_mire_webcam_desc_get();
		auto* mire = ms_web_cam_new(mire_desc);
		ms_web_cam_manager_add_cam(webCamMan, mire);
		core->setVideoDevice("Mire: Mire (synthetic moving picture)");
		core->enableVideoCapture(true);
	} else {
		core->enableVideoCapture(false);
	}

	if (bool(mReceiveVideo)) {
		// Enabling display enables video decoding, letting tests setup hooks to get notified of frames decoded.
		core->enableVideoDisplay(true);
		// The MSExtDisplay filter is designed to forward buffers to another layer, but when it is not setup it just
		// does nothing and acts as a void sink.
		core->setVideoDisplayFilter("MSExtDisplay");
	} else {
		core->enableVideoDisplay(false);
	}

	{
		const auto& nat = core->getNatPolicy();
		nat->enableIce(bool(mIce));
		core->setNatPolicy(nat);
	}

	core->start();
	if (bool(mRegister)) {
		CoreAssert(core, mAgent)
		    .iterateUpTo(0x10,
		                 [&account] {
			                 FAIL_IF(account->getState() != linphone::RegistrationState::Ok);
			                 return ASSERTION_PASSED();
		                 })
		    .assert_passed();
	}

	return CoreClient{std::move(core), std::move(account), std::move(myAddress), mAgent};
}

std::shared_ptr<CoreClient> ClientBuilder::make(const std::string& baseAddress) const {
	return std::make_shared<CoreClient>(build(baseAddress));
}

ClientBuilder& ClientBuilder::setConferenceFactoryUri(const std::string& uri) {
	mAccountParams->setConferenceFactoryUri(uri);
	return *this;
}

void ClientBuilder::setLimeX3DH(OnOff state) {
	mLimeX3DH = state;
}

ClientBuilder& ClientBuilder::setVideoReceive(OnOff value) {
	mReceiveVideo = value;
	return *this;
}
ClientBuilder& ClientBuilder::setVideoSend(OnOff value) {
	mSendVideo = value;
	return *this;
}

ClientBuilder& ClientBuilder::setRtcpSend(OnOff value) {
	mSendRtcp = value;
	return *this;
}

ClientBuilder& ClientBuilder::setIce(OnOff value) {
	mIce = value;
	return *this;
}
ClientBuilder& ClientBuilder::setRegistration(OnOff value) {
	mRegister = value;
	return *this;
}
ClientBuilder& ClientBuilder::setCpimInBasicChatroom(OnOff value) {
	mAccountParams->enableCpimInBasicChatRoom(bool(value));
	return *this;
}

ClientBuilder& ClientBuilder::setCustomContact(const std::string& contact) {
	mAccountParams->setCustomContact(mFactory->createAddress(contact));
	return *this;
}

ClientBuilder& ClientBuilder::setPushParams(const pushnotification::RFC8599PushParams& params) {
	mAccountParams->setContactUriParameters(params.toUriParams());
	return *this;
}

ClientBuilder& ClientBuilder::setInactiveAudioOnPause(OnOff value) {
	mSetAudioInactiveOnPause = value;
	return *this;
}

ClientBuilder& ClientBuilder::setAudioPort(port::PortSetting setting) {
	mAudioPort = setting;
	return *this;
}

ClientBuilder& ClientBuilder::setVideoPort(port::PortSetting setting) {
	mVideoPort = setting;
	return *this;
}

ClientBuilder& ClientBuilder::setApplePushConfig() {
	const auto pushConfig = mAccountParams->getPushNotificationConfig();
	pushConfig->setProvider("apns");
	pushConfig->setPrid("AAAAAAAAAAAAAAAAAAAA7DF897B431746F49E271E66BBF655C13C2BBD70FFC18:remote&"
	                    "8A499FF20722E0C47A4F52657554B22E2AE6BF45AC91AAAAAAAAAAAAAAAAAAAA:voip");
	pushConfig->setParam("ABCD1234.org.linphone.phone.remote&voip");
	mAccountParams->setPushNotificationAllowed(true);

	return *this;
}

ClientBuilder& ClientBuilder::setApplePushConfigRemoteOnly() {
	const auto pushConfig = mAccountParams->getPushNotificationConfig();
	pushConfig->setProvider("apns");
	pushConfig->setPrid("AAAAAAAAAAAAAAAAAAAA7DF897B431746F49E271E66BBF655C13C2BBD70FFC19:remote");
	pushConfig->setParam("ABCD1234.org.linphone.phone.remote");
	mAccountParams->setPushNotificationAllowed(true);

	return *this;
}

ClientBuilder& ClientBuilder::setPassword(const std::string_view& password) {
	mPassword = password;
	return *this;
}

} // namespace flexisip::tester
