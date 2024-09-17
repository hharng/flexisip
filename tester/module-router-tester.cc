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

#include <chrono>
#include <memory>
#include <string>
#include <unistd.h>

#include "belle-sip/types.h"

#include "flexisip/logmanager.hh"
#include "flexisip/module-router.hh"
#include "sofia-wrapper/nta-agent.hh"

#include "utils/asserts.hh"
#include "utils/bellesip-utils.hh"
#include "utils/core-assert.hh"
#include "utils/string-utils.hh"
#include "utils/test-patterns/registrardb-test.hh"
#include "utils/test-patterns/test.hh"
#include "utils/test-suite.hh"

using namespace std;
using namespace std::chrono;
using namespace sofiasip;

namespace flexisip::tester {
namespace {

void fallbackRouteFilter() {
	const auto fallbackPort = 8282;
	Server server{{
	    {"module::DoSProtection/enabled", "false"},
	    {"module::Registrar/reg-domains", "127.0.0.1"},
	    {"module::Router/enabled", "true"},
	    {"module::Router/fallback-route", "sip:127.0.0.1:" + to_string(fallbackPort) + ";transport=udp"},
	    {"module::Router/fallback-route-filter", "request.method != 'INVITE'"},
	}};
	server.start();

	bool requestReceived = false;
	BellesipUtils belleSipUtilsFallback{
	    "0.0.0.0",
	    fallbackPort,
	    "UDP",
	    static_cast<BellesipUtils::ProcessResponseStatusCb>(nullptr),
	    [&requestReceived](const belle_sip_request_event_t*) { requestReceived = true; },
	};
	bool responseReceived = false;
	BellesipUtils belleSipUtils{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "UDP",
	    [&responseReceived](int status) {
		    if (status != 100) {
			    BC_ASSERT_CPP_EQUAL(status, 200);
			    responseReceived = true;
		    }
	    },
	};

	// Send a request matching the filter.
	stringstream requestMatchingFilter{};
	requestMatchingFilter << "OPTIONS sip:participant1@127.0.0.1:" << server.getFirstPort() << " SIP/2.0\r\n"
	                      << "Via: SIP/2.0/UDP 10.10.10.10:5060;branch=z9hG4bK1439638806\r\n"
	                      << "From: <sip:anthony@127.0.0.1>;tag=465687829\r\n"
	                      << "To: <sip:participant1@127.0.0.1>\r\n"
	                      << "CSeq: 1 OPTIONS\r\n"
	                      << "Call-ID: 1053183492\r\n"
	                      << "Contact: <sip:jehan-mac@192.168.1.8:5062>\r\n"
	                      << "Max-Forwards: 70\r\n"
	                      << "User-Agent: BelleSipUtils\r\n"
	                      << "Content-Length: 0\r\n\r\n";
	belleSipUtils.sendRawRequest(requestMatchingFilter.str());

	CoreAssert asserter{server, belleSipUtilsFallback, belleSipUtils};
	asserter
	    .wait([&responseReceived, &requestReceived]() {
		    // ... so the fallback route MUST have received the request...
		    FAIL_IF(!requestReceived);
		    // ... and the sender MUST have received the "200 Ok" from the fallback route.
		    FAIL_IF(!responseReceived);
		    return ASSERTION_PASSED();
	    })
	    .assert_passed();

	responseReceived = false;
	requestReceived = false;
	BellesipUtils belleSipUtilsBis{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "UDP",
	    [&responseReceived](int status) {
		    if (status != 100) {
			    BC_ASSERT_CPP_EQUAL(status, 404);
			    responseReceived = true;
		    }
	    },
	    nullptr,
	};

	// This time we send a request not matching the filter...
	stringstream requestNotMatchingFilter{};
	requestNotMatchingFilter << "INVITE sip:participant1@127.0.0.1:" << server.getFirstPort() << " SIP/2.0\r\n"
	                         << "Via: SIP/2.0/UDP 10.10.10.10:5060;branch=z9hG4bK1439638806\r\n"
	                         << "From: <sip:anthony@127.0.0.1>;tag=465687829\r\n"
	                         << "To: <sip:participant1@127.0.0.1>\r\n"
	                         << "CSeq: 1 INVITE\r\n"
	                         << "Call-ID: 1053183493\r\n"
	                         << "Contact: <sip:jehan-mac@192.168.1.8:5062>\r\n"
	                         << "Max-Forwards: 70\r\n"
	                         << "User-Agent: BelleSipUtils\r\n"
	                         << "Content-Length: 0\r\n\r\n";
	belleSipUtilsBis.sendRawRequest(requestNotMatchingFilter.str());

	asserter.registerSteppable(belleSipUtilsBis);
	asserter
	    .wait([&responseReceived, &requestReceived]() {
		    // ... so the fallback route MUST NOT have received the request...
		    FAIL_IF(requestReceived);
		    // ... and the sender MUST have received the "404 Not Found" from flexisip (no user in the registrar db).
		    FAIL_IF(!responseReceived);
		    return ASSERTION_PASSED();
	    })
	    .assert_passed();
}

/**
 * Verify that RouterModule removes route to itself.
 *
 * In this test we want to verify that every request that enter the module::Router
 * with a "Route:" header pointing to itself are actually resolved by using the
 * registrar DB and goes out the module::Router with the "Route:" header removed.
 */
void selfRouteHeaderRemoving() {
	SLOGD << "Step 1: Setup";
	Server server{{
	    {"global/aliases", "test.flexisip.org"},
	    {"module::DoSProtection/enabled", "false"},
	    {"module::Registrar/reg-domains", "test.flexisip.org"},
	}};
	server.start();

	bool isRequestReceived = false;
	BellesipUtils belleSipUtilsReceiver{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "TCP",
	    static_cast<BellesipUtils::ProcessResponseStatusCb>(nullptr),
	    [&isRequestReceived](const belle_sip_request_event_t* event) {
		    isRequestReceived = true;
		    if (!BC_ASSERT_PTR_NOT_NULL(belle_sip_request_event_get_request(event))) {
			    return;
		    }
		    auto request = belle_sip_request_event_get_request(event);
		    auto message = BELLE_SIP_MESSAGE(request);
		    auto routes = belle_sip_message_get_headers(message, "Route");
		    if (routes != nullptr) {
			    BC_FAIL("Route was not removed");
		    }
	    },
	};
	bool isRequestAccepted = false;
	BellesipUtils belleSipUtilsSender{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "TCP",
	    [&isRequestAccepted](int status) {
		    if (status != 100) {
			    BC_ASSERT_CPP_EQUAL(status, 200);
			    isRequestAccepted = true;
		    }
	    },
	    nullptr,
	};

	ContactInserter inserter{*server.getRegistrarDb()};
	inserter.setAor("sip:provencal_le_gaulois@test.flexisip.org")
	    .setExpire(30s)
	    .insert({"sip:provencal_le_gaulois@127.0.0.1:" + to_string(belleSipUtilsReceiver.getListeningPort()) +
	             ";transport=tcp"});

	CoreAssert asserter{server, belleSipUtilsReceiver, belleSipUtilsSender};
	asserter.wait([&inserter] { return LOOP_ASSERTION(inserter.finished()); }).assert_passed();

	SLOGD << "Step 2: Send message";
	const string body{"C'est pas faux \r\n\r\n"};
	stringstream request{};
	request << "MESSAGE sip:provencal_le_gaulois@test.flexisip.org SIP/2.0\r\n"
	        << "Via: SIP/2.0/TCP 127.0.0.1:" << belleSipUtilsSender.getListeningPort() << ";branch=z9hG4bK.PAWTmC\r\n"
	        << "From: <sip:kijou@sip.linphone.org;gr=8aabdb1c>;tag=l3qXxwsO~\r\n"
	        << "To: <sip:provencal_le_gaulois@test.flexisip.org>\r\n"
	        << "CSeq: 20 MESSAGE\r\n"
	        << "Call-ID: Tvw6USHXYv\r\n"
	        << "Max-Forwards: 70\r\n"
	        << "Route: <sip:127.0.0.1:" << server.getFirstPort() << ";transport=tcp;lr>\r\n"
	        << "Supported: replaces, outbound, gruu\r\n"
	        << "Date: Fri, 01 Apr 2022 11:18:26 GMT\r\n"
	        << "Content-Type: text/plain\r\n"
	        << "Content-Length: " << body.size() << "\r\n\r\n";
	belleSipUtilsSender.sendRawRequest(request.str(), body);

	SLOGD << "Step 3: Assert that request received an answer (200) and is received";
	asserter
	    .wait([&isRequestAccepted, &isRequestReceived]() {
		    return LOOP_ASSERTION(isRequestAccepted && isRequestReceived);
	    })
	    .assert_passed();
}

/**
 * Check that module router don't remove route to others.
 *
 * In this test the message contains two "Route:" headers :
 *  - One pointing to itself
 *  - One pointing to another proxy
 *  We want to assert that the header pointing to itself is removed.
 *  We want to assure that the module::Router is skipped (no contact is resolved) and
 *  the request directly forwarded to the other proxy, with the second route header preserved.
 */
void otherRouteHeaderNotRemoved() {
	SLOGD << "Step 1: Setup";
	Server server{{
	    {"global/aliases", "test.flexisip.org"},
	    {"module::DoSProtection/enabled", "false"},
	    {"module::Registrar/reg-domains", "test.flexisip.org"},
	}};
	server.start();

	bool isRequestReceived = false;
	auto belleSipUtilsReceiverPort = "0"s;
	BellesipUtils belleSipUtilsReceiver{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "TCP",
	    static_cast<BellesipUtils::ProcessResponseStatusCb>(nullptr),
	    [&isRequestReceived, &belleSipUtilsReceiverPort](const belle_sip_request_event_t* event) {
		    isRequestReceived = true;
		    const auto* request = belle_sip_request_event_get_request(event);
		    BC_HARD_ASSERT_NOT_NULL(request);
		    const auto* message = BELLE_SIP_MESSAGE(request);
		    BC_HARD_ASSERT_NOT_NULL(message);
		    const auto* routes = belle_sip_message_get_headers(message, BELLE_SIP_ROUTE);
		    BC_HARD_ASSERT_NOT_NULL(routes);
		    if (bctbx_list_last_elem(routes) != bctbx_list_first_elem(routes)) {
			    BC_FAIL("Both routes were preserved");
		    } else {
			    const auto* routeActual =
			        reinterpret_cast<belle_sip_header_route_t*>(bctbx_list_first_elem(routes)->data);
			    const auto* routeExpected = belle_sip_header_route_parse(
			        string{"Route: <sip:127.0.0.1:" + belleSipUtilsReceiverPort + ";transport=tcp;lr>"}.c_str());
			    BC_ASSERT_TRUE(belle_sip_header_route_equals(routeActual, routeExpected) == 0);
		    }
	    },
	};
	belleSipUtilsReceiverPort = to_string(belleSipUtilsReceiver.getListeningPort());

	bool isRequestAccepted = false;
	BellesipUtils belleSipUtilsSender{
	    "0.0.0.0",
	    BELLE_SIP_LISTENING_POINT_RANDOM_PORT,
	    "TCP",
	    [&isRequestAccepted](int status) {
		    if (status != 100) {
			    BC_ASSERT_CPP_EQUAL(status, 200);
			    isRequestAccepted = true;
		    }
	    },
	    nullptr,
	};

	// Because we want to assert that module::Router is skipped and that no user is resolved we insert
	// a contact pointing to nowhere.
	ContactInserter inserter{*server.getRegistrarDb()};
	inserter.setAor("sip:provencal_le_gaulois@test.flexisip.org")
	    .setExpire(30s)
	    .insert({"sip:provencal_le_gaulois@127.0.0.1:0;transport=tcp"});

	CoreAssert asserter{server, belleSipUtilsReceiver, belleSipUtilsSender};
	asserter.wait([&inserter] { return LOOP_ASSERTION(inserter.finished()); }).assert_passed();

	SLOGD << "Step 2: Send message";
	const string body{"C'est pas faux \r\n\r\n"};
	stringstream request{};
	request << "MESSAGE sip:provencal_le_gaulois@test.flexisip.org SIP/2.0\r\n"
	        << "Via: SIP/2.0/TCP 127.0.0.1:" << belleSipUtilsReceiver.getListeningPort() << ";branch=z9hG4bK.PAWTmC\r\n"
	        << "From: <sip:kijou@sip.linphone.org;gr=8aabdb1c>;tag=l3qXxwsO~\r\n"
	        << "To: <sip:provencal_le_gaulois@test.flexisip.org>\r\n"
	        << "CSeq: 20 MESSAGE\r\n"
	        << "Call-ID: Tvw6USHXYv\r\n"
	        << "Max-Forwards: 70\r\n"
	        << "Route: <sip:127.0.0.1:" << server.getFirstPort() << ";transport=tcp;lr>\r\n"
	        << "Route: <sip:127.0.0.1:" << belleSipUtilsReceiverPort << ";transport=tcp;lr>\r\n"
	        << "Supported: replaces, outbound, gruu\r\n"
	        << "Date: Fri, 01 Apr 2022 11:18:26 GMT\r\n"
	        << "Content-Type: text/plain\r\n"
	        << "Content-Length: " << body.size() << "\r\n\r\n";
	belleSipUtilsSender.sendRawRequest(request.str(), body);

	SLOGD << "Step 3: Assert that request received an answer (200) and is received";
	asserter
	    .wait([&isRequestAccepted, &isRequestReceived]() {
		    return LOOP_ASSERTION(isRequestAccepted && isRequestReceived);
	    })
	    .assert_passed();
}

template <typename Database>
void messageExpires() {
	Database db{};
	Server server{[&db]() {
		auto config = db.configAsMap();
		config.emplace("global/transports", "sip:127.0.0.1:0;transport=udp");
		config.emplace("module::Registrar/reg-domains", "127.0.0.1");
		return config;
	}()};
	server.start();

	auto responseCount = 0;
	BellesipUtils belleSipUtils{
	    "0.0.0.0",
	    0,
	    "UDP",
	    [&responseCount](int status) {
		    if (status != 100) {
			    ++responseCount;
		    }
	    },
	    nullptr,
	};

	ContactInserter inserter{server.getAgent()->getRegistrarDb()};
	inserter.setAor("sip:message_expires@127.0.0.1")
	    .setExpire(0s)
	    .setContactParams({"message-expires=1609"})
	    .insert({"sip:message_expires@127.0.0.1:" + to_string(belleSipUtils.getListeningPort())});

	CoreAssert asserter{server, belleSipUtils};
	asserter.wait([&inserter] { return LOOP_ASSERTION(inserter.finished()); }).hard_assert_passed();

	const auto& routerModule = static_pointer_cast<ModuleRouter>(server.getAgent()->findModule("Router"));
	BC_HARD_ASSERT(routerModule != nullptr);
	auto* forks = routerModule->mStats.mCountForks->start;
	BC_ASSERT_CPP_EQUAL(forks->read(), 0);

	stringstream rawRequest{};
	rawRequest << "OPTIONS sip:message_expires@127.0.0.1:" << server.getFirstPort() << " SIP/2.0\r\n"
	           << "Via: SIP/2.0/TCP 127.0.0.1\r\n"
	           << "From: <sip:from@127.0.0.1>;tag=stub-from-tag-1\r\n"
	           << "To: <sip:message_expires@127.0.0.1>\r\n"
	           << "CSeq: 20 OPTIONS\r\n"
	           << "Call-ID: stub-call-id-1\r\n"
	           << "Content-Length: 0\r\n\r\n";
	belleSipUtils.sendRawRequest(rawRequest.str());

	rawRequest = {};
	rawRequest << "MESSAGE sip:message_expires@127.0.0.1:" << server.getFirstPort() << " SIP/2.0\r\n"
	           << "Via: SIP/2.0/TCP 127.0.0.1\r\n"
	           << "From: <sip:from@127.0.0.1>;tag=stub-from-tag-2\r\n"
	           << "To: <sip:message_expires@127.0.0.1>\r\n"
	           << "CSeq: 20 MESSAGE\r\n"
	           << "Call-ID: stub-call-id-2\r\n"
	           << "Content-Type: text/plain\r\n"
	           << "Content-Length: 0\r\n\r\n";
	belleSipUtils.sendRawRequest(rawRequest.str());

	asserter.wait([&responseCount] { return LOOP_ASSERTION(responseCount == 2); }).hard_assert_passed();
	BC_ASSERT_CPP_EQUAL(forks->read(), 1);
}

struct Contact {
	string aor;
	string uri;
};

/*
 * Test helper for unit tests about routing requests with "module::Router/static-targets" parameter.
 */
struct RoutingWithStaticTargets {
	RoutingWithStaticTargets(const vector<Contact>& contacts, const vector<string>& staticTargets)
	    : mInjectedModule({
	          .injectAfterModule = {"Router"},
	          .onRequest =
	              [this](const shared_ptr<RequestSipEvent>& ev) {
		              if (ev->getMsgSip()->getSipMethod() != sip_method_invite) return;
		              mActualTargets.emplace_back(url_as_string(ev->getHome(), ev->getSip()->sip_request->rq_url));
	              },
	      }),
	      mProxy(
	          {
	              {"global/aliases", "localhost"},
	              {"module::NatHelper/enabled", "false"},
	              {"module::DoSProtection/enabled", "false"},
	              {"module::Registrar/reg-domains", "localhost"},
	              {"module::Router/static-targets", StringUtils::join(staticTargets)},
	          },
	          &mInjectedModule),
	      mClient(mProxy.getRoot(), "sip:127.0.0.1:0") {

		mProxy.start();

		ContactInserter inserter(mProxy.getAgent()->getRegistrarDb());
		for (const auto& contact : contacts) {
			inserter.setAor(contact.aor).setExpire(1min).insert({contact.uri});
		}
		mAsserter.wait([&inserter] { return inserter.finished(); }).hard_assert_passed();
	}

	vector<string> mActualTargets{};
	Contact mCaller{"sip:caller@localhost", "sip:caller@voluntarily-unreachable:0"};
	InjectedHooks mInjectedModule;
	Server mProxy;
	sofiasip::NtaAgent mClient;
	CoreAssert<kDefaultSleepInterval> mAsserter{mProxy};
};

/*
 * Test that INVITE request is both routed to the callee and to the provided static targets.
 */
void requestIsAlsoRoutedToStaticTargets() {
	// Set up expected targets without transport and port 0 so the server does not try to send forked INVITE requests.
	const auto callee = Contact{"sip:callee@localhost", "sip:callee@127.0.0.1:0"};
	const auto sTarget = Contact{"sip:sTarget@localhost", "sip:sTarget@127.0.0.1:0"};
	const auto sTargetBis = Contact{"sip:sTargetBis@localhost", "sip:sTargetBis@127.0.0.1:0"};

	RoutingWithStaticTargets helper{{callee}, {sTarget.uri, sTargetBis.uri}};
	vector<string> expectedTargets = {sTarget.uri, sTargetBis.uri, callee.uri};
	const auto routeUri = "sip:127.0.0.1:"s + helper.mProxy.getFirstPort();

	ostringstream request;
	request << "INVITE " << callee.aor << " SIP/2.0\r\n"
	        << "Via: SIP/2.0/TCP 127.0.0.1\r\n"
	        << "From: \"Caller\" <" << helper.mCaller.aor << ">;tag=stub-tag\r\n"
	        << "To: \"Callee\" <" << callee.aor << ">\r\n"
	        << "CSeq: 20 INVITE\r\n"
	        << "Call-ID: stub-id\r\n"
	        << "Contact: <" << helper.mCaller.aor << ";transport=tcp>\r\n"
	        << "User-Agent: NtaAgent\r\n"
	        << "Allow: INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, SUBSCRIBE, INFO, PRACK, UPDATE\r\n"
	        << "Supported: replaces, outbound, gruu\r\n"
	        << "Content-Type: application/sdp\r\n"
	        << "Content-Length: 0\r\n\r\n";

	const auto transaction = helper.mClient.createOutgoingTransaction(request.str(), routeUri);
	helper.mAsserter.wait([&transaction]() { return transaction->isCompleted(); }).assert_passed();

	BC_HARD_ASSERT_CPP_EQUAL(helper.mActualTargets.size(), expectedTargets.size());
	for (auto targetId = 0U; targetId < expectedTargets.size(); ++targetId) {
		BC_ASSERT_CPP_EQUAL(helper.mActualTargets[targetId], expectedTargets[targetId]);
	}
}

/*
 * Test that INVITE request is both routed to the list of targets defined in the "X-Target-Uris" header and to provided
 * static targets. In this case, it should not be routed to the callee.
 */
void requestIsRoutedToXTargetUrisAndStaticTargets() {
	// Set up expected targets without transport and port 0 so the server does not try to send forked INVITE requests.
	const auto callee = Contact{"sip:callee@localhost", "sip:callee@127.0.0.1:0"};
	const auto sTarget = Contact{"sip:sTarget@localhost", "sip:sTarget@127.0.0.1:0"};
	const auto sTargetBis = Contact{"sip:sTargetBis@localhost", "sip:sTargetBis@127.0.0.1:0"};
	const auto xTarget = Contact{"sip:xTarget@localhost", "sip:xTarget@127.0.0.1:0"};
	const auto xTargetBis = Contact{"sip:xTargetBis@localhost", "sip:xTargetBis@127.0.0.1:0"};

	RoutingWithStaticTargets helper{{xTarget, xTargetBis}, {sTarget.uri, sTargetBis.uri}};
	vector<string> expectedTargets = {sTarget.uri, sTargetBis.uri, xTarget.uri, xTargetBis.uri};
	const auto routeUri = "sip:127.0.0.1:"s + helper.mProxy.getFirstPort();

	ostringstream request;
	request << "INVITE " << callee.aor << " SIP/2.0\r\n"
	        << "Via: SIP/2.0/TCP 127.0.0.1\r\n"
	        << "From: \"Caller\" <" << helper.mCaller.aor << ">;tag=stub-tag\r\n"
	        << "To: \"Callee\" <" << callee.aor << ">\r\n"
	        << "CSeq: 20 INVITE\r\n"
	        << "Call-ID: stub-id\r\n"
	        << "Contact: <" << helper.mCaller.aor << ";transport=tcp>\r\n"
	        << "X-Target-Uris: <" << xTarget.aor << ">,<" << xTargetBis.aor << ">\r\n"
	        << "User-Agent: NtaAgent\r\n"
	        << "Allow: INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, SUBSCRIBE, INFO, PRACK, UPDATE\r\n"
	        << "Supported: replaces, outbound, gruu\r\n"
	        << "Content-Type: application/sdp\r\n"
	        << "Content-Length: 0\r\n\r\n";

	const auto transaction = helper.mClient.createOutgoingTransaction(request.str(), routeUri);
	helper.mAsserter.wait([&transaction]() { return transaction->isCompleted(); }).assert_passed();

	BC_HARD_ASSERT_CPP_EQUAL(helper.mActualTargets.size(), expectedTargets.size());
	for (auto targetId = 0U; targetId < expectedTargets.size(); ++targetId) {
		BC_ASSERT_CPP_EQUAL(helper.mActualTargets[targetId], expectedTargets[targetId]);
	}
}

TestSuite _("RouterModule",
            {
                CLASSY_TEST(fallbackRouteFilter),
                CLASSY_TEST(selfRouteHeaderRemoving),
                CLASSY_TEST(otherRouteHeaderNotRemoved),
                CLASSY_TEST(messageExpires<DbImplementation::Internal>),
                CLASSY_TEST(messageExpires<DbImplementation::Redis>),
                CLASSY_TEST(requestIsAlsoRoutedToStaticTargets),
                CLASSY_TEST(requestIsRoutedToXTargetUrisAndStaticTargets),
            });

} // namespace
} // namespace flexisip::tester