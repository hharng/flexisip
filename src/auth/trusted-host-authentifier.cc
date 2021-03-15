/*
 * Flexisip, a flexible SIP proxy server with media capabilities.
 * Copyright (C) 2021  Belledonne Communications SARL, All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "trusted-host-authentifier.hh"

namespace flexisip {

void TrustedHostAuthentifier::verify(const std::shared_ptr<AuthStatus> &as) {
	const auto &ev = as->mEvent;
	auto sip = ev->getSip();

	// Check for trusted host
	auto via = sip->sip_via;
	auto printableReceivedHost = !empty(via->v_received) ? via->v_received : via->v_host;

	BinaryIp receivedHost{printableReceivedHost};

	if (mTrustedHosts.find(receivedHost) != mTrustedHosts.end()){
		LOGD("Allowing message from trusted host %s", printableReceivedHost);
		if (as->as_callback) as->as_callback(as, Status::Pass);
	} else {
		auto nextAuth = mNextAuth.lock();
		if (nextAuth) nextAuth->verify(as);
		else if (as->as_callback) as->as_callback(as, Status::End);
	}
}

} // namespace flexisip
