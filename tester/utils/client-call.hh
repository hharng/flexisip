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

#pragma once

#include <functional>
#include <memory>

#include <linphone++/call.hh>
#include <linphone++/call_listener.hh>
#include <linphone++/call_stats.hh>
#include <linphone++/enums.hh>
#include <linphone++/payload_type.hh>
#include <ortp/rtp.h>
#include <ortp/rtpsession.h>

namespace flexisip::tester {

/**
 * @brief Extends linphone::Call for testing purposes.
 */
class ClientCall {
public:
	ClientCall(std::shared_ptr<linphone::Call>&&);

	/* CHEATS ~~ Use only for quick prototyping */
	static const std::shared_ptr<linphone::Call>& getLinphoneCall(const ClientCall&);

	linphone::Status accept() const;
	linphone::Status acceptEarlyMedia() const;
	linphone::Status
	update(const std::function<std::shared_ptr<linphone::CallParams>(std::shared_ptr<linphone::CallParams>&&)>&) const;
	linphone::Status pause() const;
	linphone::Status resume() const;
	linphone::Status transferTo(const std::shared_ptr<linphone::Address>& referToAddress) const;
	linphone::Status transferToAnother(const ClientCall& otherCall) const;
	linphone::Status decline(linphone::Reason) const;
	linphone::Status terminate() const;

	linphone::Reason getReason() const;
	linphone::Call::State getState() const;

	std::shared_ptr<const linphone::Address> getRemoteAddress() const;
	std::shared_ptr<const linphone::Address> getReferredByAddress() const;

	const ::RtpSession* getRtpSession() const;
	const ::RtpTransport& getMetaRtpTransport() const;
	std::shared_ptr<linphone::CallStats> getStats(linphone::StreamType type) const;

	linphone::MediaDirection getAudioDirection() const;
	std::shared_ptr<linphone::CallStats> getAudioStats() const;
	std::shared_ptr<const linphone::PayloadType> getAudioPayloadType() const;

	const bool& videoFrameDecoded();
	const ::rtp_stats& getVideoRtpStats() const;

	std::shared_ptr<linphone::Core> getCore() const;

	void setStaticPictureFps(float fps);

	void addListener(const std::shared_ptr<linphone::CallListener>& listener) const;
	std::shared_ptr<linphone::CallParams> createCallParams(const ClientCall& call) const;

	bool operator==(const ClientCall& other) const;
	bool operator!=(const ClientCall& other) const;

private:
	class VideoDecodedListener : public linphone::CallListener {
	public:
		bool mFrameDecoded = false;

	private:
		void onNextVideoFrameDecoded(const std::shared_ptr<linphone::Call>&) override {
			mFrameDecoded = true;
		}
	};

	std::shared_ptr<linphone::Call> mCall;
	std::shared_ptr<VideoDecodedListener> mListener{nullptr};
};

} // namespace flexisip::tester