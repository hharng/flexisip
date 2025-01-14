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

#include "b2bua-server.hh"

#include <memory>

#include <mediastreamer2/ms_srtp.h>

#include "flexisip/logmanager.hh"

#include "b2bua/async-stop-core.hh"
#include "exceptions/bad-configuration.hh"
#include "sip-bridge/sip-bridge.hh"
#include "trenscrypter.hh"
#include "utils/string-utils.hh"
#include "utils/variant-utils.hh"

#define FUNC_LOG_PREFIX B2buaServer::kLogPrefix << "::" << __func__ << "()"

using namespace std;

namespace flexisip {

/**
 * @brief Retrieve peer call that is linked to the given call.
 *
 * @param call call leg
 * @return peer call leg or nullptr if not found
 */
shared_ptr<linphone::Call> B2buaServer::getPeerCall(const shared_ptr<linphone::Call>& call) const {
	const auto peerCallEntry = mPeerCalls.find(call);
	if (peerCallEntry == mPeerCalls.cend()) {
		SLOGW << kLogPrefix << ": failed to find peer call of current call {ptr = " << call
		      << ", call-id = " << call->getCallLog()->getCallId() << "}";
		return nullptr;
	}

	return peerCallEntry->second.lock();
}

B2buaServer::B2buaServer(const shared_ptr<sofiasip::SuRoot>& root, const std::shared_ptr<ConfigManager>& cfg)
    : ServiceServer(root), mConfigManager(cfg), mCli("b2bua", cfg, root) {
}

void B2buaServer::onCallStateChanged(const shared_ptr<linphone::Core>&,
                                     const shared_ptr<linphone::Call>& call,
                                     linphone::Call::State state,
                                     const string&) {
	const auto legName = call->getDir() == linphone::Call::Dir::Outgoing ? "legB"sv : "legA"sv;
	SLOGD << FUNC_LOG_PREFIX << ": call " << call << " (" << legName << ") state changed to " << (int)state;

	switch (state) {
		case linphone::Call::State::IncomingReceived: {
			SLOGD << FUNC_LOG_PREFIX << ": incoming call received from " << call->getRemoteAddress()->asString()
			      << " to " << call->getToAddress()->asString();
			// Create outgoing call using parameters from the incoming call in order to avoid duplicating the callId.
			auto outgoingCallParams = mCore->createCallParams(call);
			// Add this custom header so this call will not be intercepted by the B2BUA.
			outgoingCallParams->addCustomHeader(kCustomHeader, "ignore");
			outgoingCallParams->enableEarlyMediaSending(true);

			const auto callee = Match(mApplication->onCallCreate(*call, *outgoingCallParams))
			                        .against([](shared_ptr<const linphone::Address> callee) { return callee; },
			                                 [&call](linphone::Reason&& reason) {
				                                 call->decline(reason);
				                                 return shared_ptr<const linphone::Address>{};
			                                 });
			if (callee == nullptr) return;

			// Create a conference and attach it.
			auto conferenceParams = mCore->createConferenceParams(nullptr);
			conferenceParams->setHidden(true); // Hide conference to prevent the contact address from being updated.
			conferenceParams->enableVideo(true);
			conferenceParams->enableLocalParticipant(false); // B2BUA core is not part of it.
			conferenceParams->enableOneParticipantConference(true);
			conferenceParams->setConferenceFactoryAddress(nullptr);

			auto conference = mCore->createConferenceWithParams(conferenceParams);

			// Replicate "Referred-By" header if present (for call transfers).
			const auto referredByAddress = call->getReferredByAddress();
			if (referredByAddress) {
				outgoingCallParams->addCustomHeader("Referred-By", referredByAddress->asString());
			}

			// Create legB and add it to the conference.
			auto legB = mCore->inviteAddressWithParams(callee, outgoingCallParams);
			if (!legB) {
				// E.g. TLS is not supported
				SLOGE << FUNC_LOG_PREFIX << ": could not establish bridge call, please verify your configuration";
				call->decline(linphone::Reason::NotImplemented);
				return;
			}
			conference->addParticipant(legB);

			// Add legA to the conference, but do not answer now.
			conference->addParticipant(call);

			// Store each call.
			mPeerCalls[call] = legB;
			mPeerCalls[legB] = call;
		} break;
		case linphone::Call::State::PushIncomingReceived:
			break;
		case linphone::Call::State::OutgoingInit:
			break;
		case linphone::Call::State::OutgoingProgress:
			break;
		case linphone::Call::State::OutgoingRinging: {
			// This is legB getting its ring from callee, relay it to the legA call.
			const auto& legA = getPeerCall(call);
			if (legA) legA->notifyRinging();
		} break;
		case linphone::Call::State::OutgoingEarlyMedia: {
			// LegB call sends early media: relay a 183.
			const auto& legA = getPeerCall(call);
			if (legA) {
				const auto callParams = mCore->createCallParams(legA);
				callParams->enableEarlyMediaSending(true);
				legA->acceptEarlyMediaWithParams(callParams);
			}
		} break;
		case linphone::Call::State::Connected: {
		} break;
		case linphone::Call::State::StreamsRunning: {
			auto peerCall = getPeerCall(call);
			if (!peerCall) return;

			// If this is legB and that legA is in incoming state, answer it.
			// This cannot be done in connected state as currentCallParams are not updated yet.
			if (call->getDir() == linphone::Call::Dir::Outgoing &&
			    (peerCall->getState() == linphone::Call::State::IncomingReceived ||
			     peerCall->getState() == linphone::Call::State::IncomingEarlyMedia)) {
				SLOGD << FUNC_LOG_PREFIX << ": legB is now running -> answer legA";
				auto incomingCallParams = mCore->createCallParams(peerCall);
				// Add this custom header so this call will not be intercepted by the B2BUA.
				incomingCallParams->addCustomHeader(kCustomHeader, "ignore");
				// Enforce same video/audio enable to legA than on legB - manage video rejected by legB.
				incomingCallParams->enableAudio(call->getCurrentParams()->audioEnabled());
				incomingCallParams->enableVideo(call->getCurrentParams()->videoEnabled());
				peerCall->acceptWithParams(incomingCallParams);
			}

			// If peer is in state UpdatedByRemote, we deferred an update, so accept it now.
			if (peerCall->getState() == linphone::Call::State::UpdatedByRemote) {
				SLOGD << FUNC_LOG_PREFIX << ": peer call deferred update, accept it now";
				// Update is deferred only on video/audio add remove.
				// Create call params for peer call and copy video/audio enabling settings from this call.
				auto peerCallParams = mCore->createCallParams(peerCall);
				peerCallParams->enableVideo(call->getCurrentParams()->videoEnabled());
				peerCallParams->enableAudio(call->getCurrentParams()->audioEnabled());
				peerCall->acceptUpdate(peerCallParams);
			} else if (peerCall->getState() != linphone::Call::State::PausedByRemote) {
				// Resuming from PausedByRemote, update peer back to "sendrecv".
				auto peerCallAudioDirection = peerCall->getCurrentParams()->getAudioDirection();
				if (peerCallAudioDirection == linphone::MediaDirection::SendOnly ||
				    peerCallAudioDirection == linphone::MediaDirection::Inactive) {
					SLOGD << FUNC_LOG_PREFIX << ": peer call is paused, update it to resume";
					auto peerCallParams = mCore->createCallParams(peerCall);
					peerCallParams->setAudioDirection(linphone::MediaDirection::SendRecv);
					peerCall->update(peerCallParams);
				}
			}
		} break;
		case linphone::Call::State::Pausing:
			break;
		case linphone::Call::State::Paused:
			break;
		case linphone::Call::State::Resuming:
			break;
		case linphone::Call::State::Referred: {
			const auto& peerCall = getPeerCall(call);
			if (!peerCall) return;

			const auto& referToAddress = mApplication->onTransfer(*call);
			if (!referToAddress) {
				SLOGE << FUNC_LOG_PREFIX << ": unable to process call transfer request, \"Refer-To\" header is empty";
				return;
			}

			const auto& replacesHeader = referToAddress->getHeader("Replaces");
			if (replacesHeader.empty()) {
				// Case: blind call transfer.
				SLOGD << FUNC_LOG_PREFIX << ": blind call transfer requested from "
				      << call->getRemoteAddress()->asString() << ", refer to " << referToAddress->asString();
				peerCall->addListener(make_shared<b2bua::CallTransferListener>(call));
				peerCall->transferTo(referToAddress->clone());
			} else {
				// Case: attended call transfer.
				SLOGE << FUNC_LOG_PREFIX << ": attended call transfer is not implemented yet";
			}
		} break;
		case linphone::Call::State::Error:
			// When call is in error state we shall kill the conference: just do as in End state.
		case linphone::Call::State::End: {
			mApplication->onCallEnd(*call);
			// Terminate peer Call, copy error information from this call.
			const auto& peerCall = getPeerCall(call);
			if (peerCall) peerCall->terminateWithErrorInfo(call->getErrorInfo());
		} break;
		case linphone::Call::State::PausedByRemote: {
			// Paused by remote: do not pause peer call as it will kick it out of the conference.
			// Just switch the media direction to sendOnly (only if it is not already set this way).
			const auto& peerCall = getPeerCall(call);
			if (!peerCall) return;
			if (peerCall->getState() == linphone::Call::State::PausedByRemote) {
				const auto peerLegName = legName == "legA" ? "legB"sv : "legA"sv;
				SLOGE << FUNC_LOG_PREFIX
				      << ": both calls are in state LinphoneCallPausedByRemote, lost track of who initiated the pause"
				      << " [" << legName << ": " << call << ", " << peerLegName << ": " << peerCall << "]";
				call->terminate();
				peerCall->terminate();
				return;
			}

			const auto peerCallAudioDirection = mCore->createCallParams(peerCall)->getAudioDirection();
			// Nothing to do if peer call is already not sending audio.
			if (peerCallAudioDirection != linphone::MediaDirection::Inactive &&
			    peerCallAudioDirection != linphone::MediaDirection::SendOnly) {
				const auto peerCallParams = mCore->createCallParams(peerCall);
				peerCallParams->setAudioDirection(linphone::MediaDirection::SendOnly);
				peerCall->update(peerCallParams);
			}
		} break;
		case linphone::Call::State::UpdatedByRemote: {
			// Manage add/remove video - ignore for other changes.
			const auto& peerCall = getPeerCall(call);
			if (!peerCall) return;
			auto peerCallParams = mCore->createCallParams(peerCall);
			const auto selfCallParams = call->getCurrentParams();
			const auto selfRemoteCallParams = call->getRemoteParams();
			bool updatePeerCall = false;
			if (selfRemoteCallParams->videoEnabled() != selfCallParams->videoEnabled()) {
				updatePeerCall = true;
				peerCallParams->enableVideo(selfRemoteCallParams->videoEnabled());
			}
			if (selfRemoteCallParams->audioEnabled() != selfCallParams->audioEnabled()) {
				updatePeerCall = true;
				peerCallParams->enableAudio(selfRemoteCallParams->audioEnabled());
			}
			if (updatePeerCall) {
				SLOGD << FUNC_LOG_PREFIX << ": update peer call";
				// Add this custom header so this call will not be intercepted by the B2BUA.
				peerCallParams->addCustomHeader(kCustomHeader, "ignore");
				peerCall->update(peerCallParams);
				call->deferUpdate();
			} else { // No update on video/audio status, just accept it with requested params.
				SLOGD << FUNC_LOG_PREFIX << ": accept update without forwarding it to peer call";
				// Accept all minor changes.
				call->acceptUpdate(nullptr);
			}
		} break;
		case linphone::Call::State::IncomingEarlyMedia:
			break;
		case linphone::Call::State::Updating:
			break;
		case linphone::Call::State::Released: {
			// If there are some data in that call, it is the first one to end.
			const auto callId = call->getCallLog()->getCallId();
			const auto peerCallEntry = mPeerCalls.find(call);
			if (peerCallEntry != mPeerCalls.cend()) {
				SLOGD << FUNC_LOG_PREFIX << ": release peer call {ptr = " << peerCallEntry->second.lock()
				      << ", call-id = " << callId << "}";
				mPeerCalls.erase(peerCallEntry);
			} else {
				SLOGD << FUNC_LOG_PREFIX << ": call {ptr = " << call << ", call-id = " << callId
				      << "} is in end state but it is already terminated";
			}
		} break;
		case linphone::Call::State::EarlyUpdating:
			break;
		case linphone::Call::State::EarlyUpdatedByRemote:
			break;
		default:
			break;
	}
}

void B2buaServer::onDtmfReceived([[maybe_unused]] const shared_ptr<linphone::Core>& _core,
                                 const shared_ptr<linphone::Call>& call,
                                 int dtmf) {
	const auto& otherLeg = getPeerCall(call);
	if (!otherLeg) return;

	SLOGD << FUNC_LOG_PREFIX << ": forwarding DTMF " << dtmf << " from " << call->getCallLog()->getCallId() << " to "
	      << otherLeg->getCallLog()->getCallId();
	otherLeg->sendDtmf(dtmf);
}

void B2buaServer::onSubscribeReceived(const std::shared_ptr<linphone::Core>& core,
                                      const std::shared_ptr<linphone::Event>& legAEvent,
                                      const std::string& subscribeEvent,
                                      const std::shared_ptr<const linphone::Content>& body) {
	SLOGD << FUNC_LOG_PREFIX << ": received subscribe event " << legAEvent;
	int expires = 0;
	try {
		expires = stoi(legAEvent->getCustomHeader("Expires"));
	} catch (std::exception const& ex) {
		SLOGE << FUNC_LOG_PREFIX << ": invalid expires in received SUBSCRIBE, deny subscription";
		legAEvent->denySubscription(linphone::Reason::NotAcceptable);
		return;
	}

	const auto subscriber = Match(mApplication->onSubscribe(*legAEvent, subscribeEvent))
	                            .against([](shared_ptr<const linphone::Address> subscriber) { return subscriber; },
	                                     [&legAEvent](linphone::Reason&& reason) {
		                                     legAEvent->denySubscription(reason);
		                                     return shared_ptr<const linphone::Address>{};
	                                     });
	if (subscriber == nullptr) return;

	// Create the outgoing SUBSCRIBE and copy the request address and Accept header from the incoming one.
	auto resource = subscriber->clone();
	auto legBEvent = core->createSubscribe(resource, subscribeEvent, expires);
	legBEvent->setRequestAddress(legAEvent->getRequestAddress()->clone());
	auto acceptHeader = legAEvent->getCustomHeader("Accept");
	if (!acceptHeader.empty()) legBEvent->addCustomHeader("Accept", acceptHeader);
	legBEvent->addListener(shared_from_this());

	if (legBEvent->sendSubscribe(body) < 0) {
		legAEvent->denySubscription(linphone::Reason::NotAcceptable);
		return;
	}

	// Store a shared pointer to each event.
	mPeerEvents[legAEvent] = {.peerEvent = legBEvent, .isLegA = true};
	mPeerEvents[legBEvent] = {.peerEvent = legAEvent, .isLegA = false};
	legAEvent->addListener(shared_from_this());
}

void B2buaServer::onSubscribeStateChanged(const std::shared_ptr<linphone::Event>& event,
                                          linphone::SubscriptionState state) {
	SLOGD << FUNC_LOG_PREFIX << ": event " << event << " state change to " << static_cast<int>(state);
	const auto eventEntry = mPeerEvents.find(event);
	if (eventEntry == mPeerEvents.cend()) return;

	const auto& eventInfo = eventEntry->second;
	if (eventInfo.isLegA) {
		if (state == linphone::SubscriptionState::Terminated) {
			// Un-SUBSCRIBE from the subscriber.
			const auto peerEvent = eventInfo.peerEvent.lock();
			if (peerEvent == nullptr) {
				SLOGE << FUNC_LOG_PREFIX << ": peer event pointer is null for event " << event;
				return;
			}
			peerEvent->terminate();
			const auto peerEventEntry = mPeerEvents.find(peerEvent);
			if (peerEventEntry != mPeerEvents.cend()) mPeerEvents.erase(peerEventEntry);
			mPeerEvents.erase(eventEntry);
		}
	} else {
		if (state == linphone::SubscriptionState::Active) {
			// Forward the subscription acceptation.
			const auto peerEvent = eventInfo.peerEvent.lock();
			if (peerEvent == nullptr) {
				SLOGE << FUNC_LOG_PREFIX << ": peer event pointer is null for event " << event;
				return;
			}
			peerEvent->acceptSubscription();
		} else if (state == linphone::SubscriptionState::Error) {
			// Forward the subscription error.
			const auto peerEvent = eventInfo.peerEvent.lock();
			if (peerEvent == nullptr) {
				SLOGE << FUNC_LOG_PREFIX << ": peer event pointer is null for event " << event;
				return;
			}
			peerEvent->denySubscription(event->getReason());
		}
	}
}

/**
 *  @brief NOTIFY requests listener on a subscribe event.
 *  @note This is called when a SUBSCRIBE request is forwarded by the B2BUA and then a NOTIFY request is received for
 *  this subscription.
 */
void B2buaServer::onNotifyReceived(const std::shared_ptr<linphone::Event>& event,
                                   const std::shared_ptr<const linphone::Content>& content) {
	SLOGD << FUNC_LOG_PREFIX << ": received notify event " << event;
	const auto eventEntry = mPeerEvents.find(event);
	if (eventEntry == mPeerEvents.cend()) {
		SLOGE << FUNC_LOG_PREFIX << ": no data associated with the event " << event << ", cannot forward the NOTIFY";
		return;
	}

	// Forward NOTIFY request.
	const auto peerEvent = eventEntry->second.peerEvent.lock();
	if (peerEvent == nullptr) {
		SLOGE << FUNC_LOG_PREFIX << ": peer event pointer is null for event " << event;
		return;
	}

	peerEvent->notify(content);
}

/**
 * @brief MWI listener on the core.
 * @note This is called when a MWI NOTIFY request is received out-of-dialog.
 */
void B2buaServer::onMessageWaitingIndicationChanged(
    const std::shared_ptr<linphone::Core>& core,
    const std::shared_ptr<linphone::Event>& legBEvent,
    const std::shared_ptr<const linphone::MessageWaitingIndication>& mwi) {

	// Try to create a temporary account configured with the correct outbound proxy to be able to bridge the received
	// NOTIFY request.
	const auto destination = mApplication->onNotifyToBeSent(*legBEvent);
	if (!destination) return;
	const auto& [subscriber, accountUsedToSendNotify] = *destination;

	// Modify the MWI content so that its Message-Account is mapped according to the account mapping of the sip
	// provider.
	auto newMwi = mwi->clone();
	newMwi->setAccountAddress(core->createAddress(subscriber.str()));
	auto content = newMwi->toContent();
	auto resource = core->createAddress(subscriber.str());
	auto legAEvent = core->createNotify(resource, "message-summary");
	legAEvent->notify(content);
}

void B2buaServer::_init() {
	// Parse configuration for Data directory. Handle the case where the directory is not created.
	const auto* config = mConfigManager->getRoot()->get<GenericStruct>(b2bua::configSection);
	auto dataDirPath = config->get<ConfigString>("data-directory")->read();
	if (!bctbx_directory_exists(dataDirPath.c_str())) {
		SLOGI << kLogPrefix << ": creating data directory " << dataDirPath;
		// Verify parent directory exists as default path requires creation of 2 levels.
		auto parentDir = dataDirPath.substr(0, dataDirPath.find_last_of('/'));
		if (!bctbx_directory_exists(parentDir.c_str())) {
			if (bctbx_mkdir(parentDir.c_str()) != 0) {
				SLOGE << kLogPrefix << ": could not create data parent directory " << parentDir;
			}
		}
		if (bctbx_mkdir(dataDirPath.c_str()) != 0) {
			SLOGE << kLogPrefix << ": could not create data directory " << dataDirPath;
		}
	}
	SLOGI << kLogPrefix << ": data directory set to " << dataDirPath;
	const auto& factory = linphone::Factory::get();
	factory->setDataDir(dataDirPath + "/");

	mCore = b2bua::B2buaCore::create(*factory, *config);

	mCore->addListener(shared_from_this());

	auto applicationType = config->get<ConfigString>("application")->read();
	SLOGI << kLogPrefix << ": starting with '" << applicationType << "' application";
	if (applicationType == "trenscrypter") {
		mApplication = make_unique<b2bua::trenscrypter::Trenscrypter>();
	} else if (applicationType == "sip-bridge") {
		auto bridge = make_unique<b2bua::bridge::SipBridge>(mRoot, mCore);
		mCli.registerHandler(*bridge);
		mApplication = std::move(bridge);
	} else {
		throw BadConfiguration{"unknown B2BUA server application type: "s + applicationType};
	}
	mApplication->init(mCore, *mConfigManager);

	mCore->start();
	mCli.start();
	SLOGI << kLogPrefix << ": started successfully";
}

void B2buaServer::_run() {
	mCore->iterate();
}

std::unique_ptr<AsyncCleanup> B2buaServer::_stop() {
	if (mCore == nullptr) return nullptr;

	mCore->removeListener(shared_from_this());
	mCli.stop();
	return std::make_unique<b2bua::AsyncStopCore>(mCore);
}

void b2bua::CallTransferListener::onTransferStateChanged(const std::shared_ptr<linphone::Call>& call,
                                                         linphone::Call::State state) {
	SLOGD << B2buaServer::kLogPrefix << ": call " << call << " transfer state changed to " << static_cast<int>(state);

	string body{};
	switch (state) {
		case linphone::Call::State::OutgoingProgress:
			body = "SIP/2.0 100 Trying\r\n";
			break;
		case linphone::Call::State::Connected:
			body = "SIP/2.0 200 Ok\r\n";
			break;
		case linphone::Call::State::Error:
			body = "SIP/2.0 500 Internal Server Error\r\n";
			SLOGD << B2buaServer::kLogPrefix << ": forward NOTIFY request with body \""
			      << body.substr(0, body.size() - 2)
			      << "\" because we cannot yet distinguish all cases (603 Decline, 503 Service Unavailable, etc.)";
			break;
		default:
			SLOGW << B2buaServer::kLogPrefix << ": unable to forward NOTIFY request, case " << static_cast<int>(state)
			      << " is not implemented";
			return;
	}
	sendNotify(body);
}

void b2bua::CallTransferListener::sendNotify(const std::string& body) {
	const auto peerCall = mPeerCall.lock();
	if (!peerCall) {
		SLOGW << B2buaServer::kLogPrefix << ": unable to forward NOTIFY request (" << body
		      << "), peer call has been freed";
		return;
	}

	const auto content = linphone::Factory::get()->createContent();
	if (!content) {
		SLOGE << B2buaServer::kLogPrefix << ": error while forwarding NOTIFY request, could not create content object";
		return;
	}
	content->setType("message");
	content->setSubtype("sipfrag");
	content->setUtf8Text(body);
	const auto event = peerCall->createNotify("refer");
	if (!event) {
		SLOGE << B2buaServer::kLogPrefix << ": error while forwarding NOTIFY request, could not create request";
		return;
	}
	event->notify(content);
}

namespace {

// Statically define default configuration items.
auto& defineConfig = ConfigManager::defaultInit().emplace_back([](GenericStruct& root) {
	ConfigItemDescriptor items[] = {
	    {
	        String,
	        "application",
	        "The type of application that will handle calls bridged through the server. Possible values:\n"
	        "- `trenscrypter`: bridge different encryption types on both ends transparently.\n"
	        "- `sip-bridge`: bridge calls through an external SIP provider (e.g. for PSTN gateways).",
	        "trenscrypter",
	    },
	    {
	        String,
	        "transport",
	        "Unique SIP URI on which the server is listening.",
	        "sip:127.0.0.1:6067;transport=tcp",
	    },
	    {
	        IntegerRange,
	        "audio-port",
	        "Audio port to use for RTP and RTCP traffic. You can set a specific port, a range of ports or let the "
	        "server ask the kernel for an available port (special value: 0).\n"
	        "Examples: 'audio-port=0' or 'audio-port=12345' or 'audio-port=1024-65535'",
	        "0",
	    },
	    {
	        IntegerRange,
	        "video-port",
	        "Video port to use for RTP and RTCP traffic. You can set a specific port, a range of ports or let the "
	        "server ask the kernel for an available port (special value: 0).\n"
	        "Examples: 'video-port=0' or 'video-port=12345' or 'video-port=1024-65535'",
	        "0",
	    },
	    {
	        String,
	        "user-agent",
	        "Value of User-Agent header. Use the following syntax: <name>[/<version>] where <version> can bet set to "
	        "'{version}' that is a placeholder for the Flexisip version.",
	        "Flexisip-B2BUA/{version}",
	    },
	    {
	        String,
	        "data-directory",
	        "Directory where to store server local files",
	        DEFAULT_B2BUA_DATA_DIR,
	    },
	    {
	        String,
	        "outbound-proxy",
	        "The SIP proxy URI to which the server will send all outgoing requests.",
	        "sip:127.0.0.1:5060;transport=tcp",
	    },
	    {
	        DurationS,
	        "no-rtp-timeout",
	        "Duration after which the server will terminate a call if no RTP packets are received from the other call "
	        "participant. For performance reasons, this parameter cannot be disabled.",
	        "30",
	    },
	    {
	        DurationS,
	        "max-call-duration",
	        "The server will terminate any bridged call that has been running for longer than this amount of time.\n"
	        "Set to 0 to disable and let calls run unbounded.",
	        "0",
	    },
	    {
	        String,
	        "audio-codec",
	        "Turn off all audio codecs except this one. This will effectively force this codec on both ends of all "
	        "bridged calls. If either end does not support the codec set here, the call will fail to establish. "
	        "Setting this option will also turn on the media payload forwarding optimization which improves the "
	        "performances of the B2BUA.\n"
	        "Format: <codec>/<sample-rate>.\n"
	        "Example: speex/8000",
	        "",
	    },
	    {
	        String,
	        "video-codec",
	        "Same as 'audio-codec' but for video.\n"
	        "Format: <codec>.\n"
	        "Example: H264",
	        "",
	    },
	    {
	        Boolean,
	        "one-connection-per-account",
	        "The server shall use a separate connection (port) for each (external) account it manages.\n"
	        "This can be used to work around DoS protection and rate-limiting systems on external proxies.",
	        "false",
	    },
	    config_item_end};

	root.addChild(make_unique<GenericStruct>(b2bua::configSection,
	                                         "Flexisip back-to-back user agent (B2BUA) server parameters.", 0))
	    ->addChildrenValues(items);
});

} // namespace
} // namespace flexisip