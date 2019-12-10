#define MS_CLASS "RTC::WebRtcTransport"
// #define MS_LOG_DEV

#include "RTC/WebRtcTransport.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "Channel/Notifier.hpp"
#include "RTC/RTCP/FeedbackPsRemb.hpp"
#include "RTC/RtpDictionaries.hpp"
#include <cmath>    // std::pow()
#include <iterator> // std::ostream_iterator
#include <map>
#include <sstream> // std::ostringstream

namespace RTC
{
	/* Static. */

	static constexpr uint16_t IceCandidateDefaultLocalPriority{ 10000 };
	// We just provide "host" candidates so type preference is fixed.
	static constexpr uint16_t IceTypePreference{ 64 };
	// We do not support non rtcp-mux so component is always 1.
	static constexpr uint16_t IceComponent{ 1 };

	static inline uint32_t generateIceCandidatePriority(uint16_t localPreference)
	{
		MS_TRACE();

		return std::pow(2, 24) * IceTypePreference + std::pow(2, 8) * localPreference +
		       std::pow(2, 0) * (256 - IceComponent);
	}

	/* Instance methods. */

	WebRtcTransport::WebRtcTransport(const std::string& id, RTC::Transport::Listener* listener, json& data)
	  : RTC::Transport::Transport(id, listener)
	{
		MS_TRACE();

		bool enableUdp{ true };
		auto jsonEnableUdpIt = data.find("enableUdp");

		if (jsonEnableUdpIt != data.end())
		{
			if (!jsonEnableUdpIt->is_boolean())
				MS_THROW_TYPE_ERROR("wrong enableUdp (not a boolean)");

			enableUdp = jsonEnableUdpIt->get<bool>();
		}

		bool enableTcp{ false };
		auto jsonEnableTcpIt = data.find("enableTcp");

		if (jsonEnableTcpIt != data.end())
		{
			if (!jsonEnableTcpIt->is_boolean())
				MS_THROW_TYPE_ERROR("wrong enableTcp (not a boolean)");

			enableTcp = jsonEnableTcpIt->get<bool>();
		}

		bool preferUdp{ false };
		auto jsonPreferUdpIt = data.find("preferUdp");

		if (jsonPreferUdpIt != data.end())
		{
			if (!jsonPreferUdpIt->is_boolean())
				MS_THROW_TYPE_ERROR("wrong preferUdp (not a boolean)");

			preferUdp = jsonPreferUdpIt->get<bool>();
		}

		bool preferTcp{ false };
		auto jsonPreferTcpIt = data.find("preferTcp");

		if (jsonPreferTcpIt != data.end())
		{
			if (!jsonPreferTcpIt->is_boolean())
				MS_THROW_TYPE_ERROR("wrong preferTcp (not a boolean)");

			preferTcp = jsonPreferTcpIt->get<bool>();
		}

		auto jsonListenIpsIt = data.find("listenIps");

		if (jsonListenIpsIt == data.end())
			MS_THROW_TYPE_ERROR("missing listenIps");
		else if (!jsonListenIpsIt->is_array())
			MS_THROW_TYPE_ERROR("wrong listenIps (not an array)");
		else if (jsonListenIpsIt->empty())
			MS_THROW_TYPE_ERROR("wrong listenIps (empty array)");
		else if (jsonListenIpsIt->size() > 8)
			MS_THROW_TYPE_ERROR("wrong listenIps (too many IPs)");

		std::vector<ListenIp> listenIps(jsonListenIpsIt->size());

		for (size_t i{ 0 }; i < jsonListenIpsIt->size(); ++i)
		{
			auto& jsonListenIp = (*jsonListenIpsIt)[i];
			auto& listenIp     = listenIps[i];

			if (!jsonListenIp.is_object())
				MS_THROW_TYPE_ERROR("wrong listenIp (not an object)");

			auto jsonIpIt = jsonListenIp.find("ip");

			if (jsonIpIt == jsonListenIp.end())
				MS_THROW_TYPE_ERROR("missing listenIp.ip");
			else if (!jsonIpIt->is_string())
				MS_THROW_TYPE_ERROR("wrong listenIp.ip (not an string");

			listenIp.ip.assign(jsonIpIt->get<std::string>());

			// This may throw.
			Utils::IP::NormalizeIp(listenIp.ip);

			auto jsonAnnouncedIpIt = jsonListenIp.find("announcedIp");

			if (jsonAnnouncedIpIt != jsonListenIp.end())
			{
				if (!jsonAnnouncedIpIt->is_string())
					MS_THROW_TYPE_ERROR("wrong listenIp.announcedIp (not an string)");

				listenIp.announcedIp.assign(jsonAnnouncedIpIt->get<std::string>());
			}
		}

		auto jsonInitialAvailableOutgoingBitrateIt = data.find("initialAvailableOutgoingBitrate");

		if (jsonInitialAvailableOutgoingBitrateIt != data.end())
		{
			if (!jsonInitialAvailableOutgoingBitrateIt->is_number_unsigned())
				MS_THROW_TYPE_ERROR("wrong initialAvailableOutgoingBitrate (not a number)");

			this->initialAvailableOutgoingBitrate = jsonInitialAvailableOutgoingBitrateIt->get<uint32_t>();
		}

		auto jsonMinimumAvailableOutgoingBitrateIt = data.find("minimumAvailableOutgoingBitrate");

		if (jsonMinimumAvailableOutgoingBitrateIt != data.end())
		{
			if (!jsonMinimumAvailableOutgoingBitrateIt->is_number_unsigned())
				MS_THROW_TYPE_ERROR("wrong minimumAvailableOutgoingBitrate (not a number)");

			this->minimumAvailableOutgoingBitrate = jsonMinimumAvailableOutgoingBitrateIt->get<uint32_t>();
		}

		if (this->minimumAvailableOutgoingBitrate > this->initialAvailableOutgoingBitrate)
		{
			MS_THROW_TYPE_ERROR(
			  "minimumAvailableOutgoingBitrate bigger than initialAvailableOutgoingBitrate");
		}

		try
		{
			uint16_t iceLocalPreferenceDecrement{ 0 };

			if (enableUdp && enableTcp)
				this->iceCandidates.reserve(2 * jsonListenIpsIt->size());
			else
				this->iceCandidates.reserve(jsonListenIpsIt->size());

			for (auto& listenIp : listenIps)
			{
				if (enableUdp)
				{
					uint16_t iceLocalPreference =
					  IceCandidateDefaultLocalPriority - iceLocalPreferenceDecrement;

					if (preferUdp)
						iceLocalPreference += 1000;

					uint32_t icePriority = generateIceCandidatePriority(iceLocalPreference);

					// This may throw.
					auto* udpSocket = new RTC::UdpSocket(this, listenIp.ip);

					this->udpSockets[udpSocket] = listenIp.announcedIp;

					if (listenIp.announcedIp.empty())
						this->iceCandidates.emplace_back(udpSocket, icePriority);
					else
						this->iceCandidates.emplace_back(udpSocket, icePriority, listenIp.announcedIp);
				}

				if (enableTcp)
				{
					uint16_t iceLocalPreference =
					  IceCandidateDefaultLocalPriority - iceLocalPreferenceDecrement;

					if (preferTcp)
						iceLocalPreference += 1000;

					uint32_t icePriority = generateIceCandidatePriority(iceLocalPreference);

					// This may throw.
					auto* tcpServer = new RTC::TcpServer(this, this, listenIp.ip);

					this->tcpServers[tcpServer] = listenIp.announcedIp;

					if (listenIp.announcedIp.empty())
						this->iceCandidates.emplace_back(tcpServer, icePriority);
					else
						this->iceCandidates.emplace_back(tcpServer, icePriority, listenIp.announcedIp);
				}

				// Decrement initial ICE local preference for next IP.
				iceLocalPreferenceDecrement += 100;
			}

			// Create a ICE server.
			this->iceServer = new RTC::IceServer(
			  this, Utils::Crypto::GetRandomString(16), Utils::Crypto::GetRandomString(32));

			// Create a DTLS transport.
			this->dtlsTransport = new RTC::DtlsTransport(this);

			// May create SCTP association.
			CreateSctpAssociation(data);
		}
		catch (const MediaSoupError& error)
		{
			// Must delete everything since the destructor won't be called.

			DestroySctpAssociation();

			delete this->dtlsTransport;
			this->dtlsTransport = nullptr;

			delete this->iceServer;
			this->iceServer = nullptr;

			for (auto& kv : this->udpSockets)
			{
				auto* udpSocket = kv.first;

				delete udpSocket;
			}
			this->udpSockets.clear();

			for (auto& kv : this->tcpServers)
			{
				auto* tcpServer = kv.first;

				delete tcpServer;
			}
			this->tcpServers.clear();

			this->iceCandidates.clear();

			throw;
		}
	}

	WebRtcTransport::~WebRtcTransport()
	{
		MS_TRACE();

		// Must delete the SCTP association first since it will generate SCTP packets.
		DestroySctpAssociation();

		// Must delete the DTLS transport first since it will generate a DTLS alert
		// to be sent.
		delete this->dtlsTransport;

		delete this->iceServer;

		for (auto& kv : this->udpSockets)
		{
			auto* udpSocket = kv.first;

			delete udpSocket;
		}
		this->udpSockets.clear();

		for (auto& kv : this->tcpServers)
		{
			auto* tcpServer = kv.first;

			delete tcpServer;
		}

		this->iceCandidates.clear();

		delete this->srtpRecvSession;

		delete this->srtpSendSession;

		delete this->rembClient;

		delete this->rembServer;
	}

	void WebRtcTransport::FillJson(json& jsonObject) const
	{
		MS_TRACE();

		// Call the parent method.
		RTC::Transport::FillJson(jsonObject);

		// Add iceRole (we are always "controlled").
		jsonObject["iceRole"] = "controlled";

		// Add iceParameters.
		jsonObject["iceParameters"] = json::object();
		auto jsonIceParametersIt    = jsonObject.find("iceParameters");

		(*jsonIceParametersIt)["usernameFragment"] = this->iceServer->GetUsernameFragment();
		(*jsonIceParametersIt)["password"]         = this->iceServer->GetPassword();
		(*jsonIceParametersIt)["iceLite"]          = true;

		// Add iceCandidates.
		jsonObject["iceCandidates"] = json::array();
		auto jsonIceCandidatesIt    = jsonObject.find("iceCandidates");

		for (size_t i{ 0 }; i < this->iceCandidates.size(); ++i)
		{
			jsonIceCandidatesIt->emplace_back(json::value_t::object);

			auto& jsonEntry    = (*jsonIceCandidatesIt)[i];
			auto& iceCandidate = this->iceCandidates[i];

			iceCandidate.FillJson(jsonEntry);
		}

		// Add iceState.
		switch (this->iceServer->GetState())
		{
			case RTC::IceServer::IceState::NEW:
				jsonObject["iceState"] = "new";
				break;
			case RTC::IceServer::IceState::CONNECTED:
				jsonObject["iceState"] = "connected";
				break;
			case RTC::IceServer::IceState::COMPLETED:
				jsonObject["iceState"] = "completed";
				break;
			case RTC::IceServer::IceState::DISCONNECTED:
				jsonObject["iceState"] = "disconnected";
				break;
		}

		// Add iceSelectedTuple.
		if (this->iceSelectedTuple != nullptr)
			this->iceSelectedTuple->FillJson(jsonObject["iceSelectedTuple"]);

		// Add dtlsParameters.
		jsonObject["dtlsParameters"] = json::object();
		auto jsonDtlsParametersIt    = jsonObject.find("dtlsParameters");

		// Add dtlsParameters.fingerprints.
		(*jsonDtlsParametersIt)["fingerprints"] = json::array();
		auto jsonDtlsParametersFingerprintsIt   = jsonDtlsParametersIt->find("fingerprints");
		auto& fingerprints                      = this->dtlsTransport->GetLocalFingerprints();

		for (size_t i{ 0 }; i < fingerprints.size(); ++i)
		{
			jsonDtlsParametersFingerprintsIt->emplace_back(json::value_t::object);

			auto& jsonEntry   = (*jsonDtlsParametersFingerprintsIt)[i];
			auto& fingerprint = fingerprints[i];

			jsonEntry["algorithm"] =
			  RTC::DtlsTransport::GetFingerprintAlgorithmString(fingerprint.algorithm);
			jsonEntry["value"] = fingerprint.value;
		}

		// Add dtlsParameters.role.
		switch (this->dtlsRole)
		{
			case RTC::DtlsTransport::Role::NONE:
				(*jsonDtlsParametersIt)["role"] = "none";
				break;
			case RTC::DtlsTransport::Role::AUTO:
				(*jsonDtlsParametersIt)["role"] = "auto";
				break;
			case RTC::DtlsTransport::Role::CLIENT:
				(*jsonDtlsParametersIt)["role"] = "client";
				break;
			case RTC::DtlsTransport::Role::SERVER:
				(*jsonDtlsParametersIt)["role"] = "server";
				break;
		}

		// Add dtlsState.
		switch (this->dtlsTransport->GetState())
		{
			case RTC::DtlsTransport::DtlsState::NEW:
				jsonObject["dtlsState"] = "new";
				break;
			case RTC::DtlsTransport::DtlsState::CONNECTING:
				jsonObject["dtlsState"] = "connecting";
				break;
			case RTC::DtlsTransport::DtlsState::CONNECTED:
				jsonObject["dtlsState"] = "connected";
				break;
			case RTC::DtlsTransport::DtlsState::FAILED:
				jsonObject["dtlsState"] = "failed";
				break;
			case RTC::DtlsTransport::DtlsState::CLOSED:
				jsonObject["dtlsState"] = "closed";
				break;
		}

		// Add headerExtensionIds.
		jsonObject["rtpHeaderExtensions"] = json::object();
		auto jsonRtpHeaderExtensionsIt    = jsonObject.find("rtpHeaderExtensions");

		if (this->rtpHeaderExtensionIds.mid != 0u)
			(*jsonRtpHeaderExtensionsIt)["mid"] = this->rtpHeaderExtensionIds.mid;

		if (this->rtpHeaderExtensionIds.rid != 0u)
			(*jsonRtpHeaderExtensionsIt)["rid"] = this->rtpHeaderExtensionIds.rid;

		if (this->rtpHeaderExtensionIds.rrid != 0u)
			(*jsonRtpHeaderExtensionsIt)["rrid"] = this->rtpHeaderExtensionIds.rrid;

		if (this->rtpHeaderExtensionIds.absSendTime != 0u)
			(*jsonRtpHeaderExtensionsIt)["absSendTime"] = this->rtpHeaderExtensionIds.absSendTime;
	}

	void WebRtcTransport::FillJsonStats(json& jsonArray)
	{
		MS_TRACE();

		jsonArray.emplace_back(json::value_t::object);
		auto& jsonObject = jsonArray[0];

		// Add type.
		jsonObject["type"] = "webrtc-transport";

		// Add transportId.
		jsonObject["transportId"] = this->id;

		// Add timestamp.
		jsonObject["timestamp"] = DepLibUV::GetTime();

		// Add iceRole (we are always "controlled").
		jsonObject["iceRole"] = "controlled";

		// Add iceState.
		switch (this->iceServer->GetState())
		{
			case RTC::IceServer::IceState::NEW:
				jsonObject["iceState"] = "new";
				break;
			case RTC::IceServer::IceState::CONNECTED:
				jsonObject["iceState"] = "connected";
				break;
			case RTC::IceServer::IceState::COMPLETED:
				jsonObject["iceState"] = "completed";
				break;
			case RTC::IceServer::IceState::DISCONNECTED:
				jsonObject["iceState"] = "disconnected";
				break;
		}

		if (this->iceSelectedTuple != nullptr)
		{
			// Add iceSelectedTuple.
			this->iceSelectedTuple->FillJson(jsonObject["iceSelectedTuple"]);
		}

		// Add dtlsState.
		switch (this->dtlsTransport->GetState())
		{
			case RTC::DtlsTransport::DtlsState::NEW:
				jsonObject["dtlsState"] = "new";
				break;
			case RTC::DtlsTransport::DtlsState::CONNECTING:
				jsonObject["dtlsState"] = "connecting";
				break;
			case RTC::DtlsTransport::DtlsState::CONNECTED:
				jsonObject["dtlsState"] = "connected";
				break;
			case RTC::DtlsTransport::DtlsState::FAILED:
				jsonObject["dtlsState"] = "failed";
				break;
			case RTC::DtlsTransport::DtlsState::CLOSED:
				jsonObject["dtlsState"] = "closed";
				break;
		}

		if (this->sctpAssociation)
		{
			// Add sctpState.
			switch (this->sctpAssociation->GetState())
			{
				case RTC::SctpAssociation::SctpState::NEW:
					jsonObject["sctpState"] = "new";
					break;
				case RTC::SctpAssociation::SctpState::CONNECTING:
					jsonObject["sctpState"] = "connecting";
					break;
				case RTC::SctpAssociation::SctpState::CONNECTED:
					jsonObject["sctpState"] = "connected";
					break;
				case RTC::SctpAssociation::SctpState::FAILED:
					jsonObject["sctpState"] = "failed";
					break;
				case RTC::SctpAssociation::SctpState::CLOSED:
					jsonObject["sctpState"] = "closed";
					break;
			}
		}

		// Add bytesReceived.
		jsonObject["bytesReceived"] = RTC::Transport::GetReceivedBytes();

		// Add bytesSent.
		jsonObject["bytesSent"] = RTC::Transport::GetSentBytes();

		// Add recvBitrate.
		jsonObject["recvBitrate"] = RTC::Transport::GetRecvBitrate();

		// Add sendBitrate.
		jsonObject["sendBitrate"] = RTC::Transport::GetSendBitrate();

		// Add availableOutgoingBitrate.
		if (this->rembClient)
			jsonObject["availableOutgoingBitrate"] = this->rembClient->GetAvailableBitrate();

		// Add availableIncomingBitrate.
		if (this->rembServer)
			jsonObject["availableIncomingBitrate"] = this->rembServer->GetAvailableBitrate();

		// Add maxIncomingBitrate.
		if (this->maxIncomingBitrate != 0u)
			jsonObject["maxIncomingBitrate"] = this->maxIncomingBitrate;
	}

	void WebRtcTransport::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::TRANSPORT_CONNECT:
			{
				// Ensure this method is not called twice.
				if (this->connected)
					MS_THROW_ERROR("connect() already called");

				RTC::DtlsTransport::Fingerprint dtlsRemoteFingerprint;
				RTC::DtlsTransport::Role dtlsRemoteRole;

				auto jsonDtlsParametersIt = request->data.find("dtlsParameters");

				if (jsonDtlsParametersIt == request->data.end() || !jsonDtlsParametersIt->is_object())
					MS_THROW_TYPE_ERROR("missing dtlsParameters");

				auto jsonFingerprintsIt = jsonDtlsParametersIt->find("fingerprints");

				if (jsonFingerprintsIt == jsonDtlsParametersIt->end() || !jsonFingerprintsIt->is_array())
					MS_THROW_TYPE_ERROR("missing dtlsParameters.fingerprints");

				// NOTE: Just take the first fingerprint.
				for (auto& jsonFingerprint : *jsonFingerprintsIt)
				{
					if (!jsonFingerprint.is_object())
						MS_THROW_TYPE_ERROR("wrong entry in dtlsParameters.fingerprints (not an object)");

					auto jsonAlgorithmIt = jsonFingerprint.find("algorithm");

					if (jsonAlgorithmIt == jsonFingerprint.end())
						MS_THROW_TYPE_ERROR("missing fingerprint.algorithm");
					else if (!jsonAlgorithmIt->is_string())
						MS_THROW_TYPE_ERROR("wrong fingerprint.algorithm (not a string)");

					dtlsRemoteFingerprint.algorithm =
					  RTC::DtlsTransport::GetFingerprintAlgorithm(jsonAlgorithmIt->get<std::string>());

					if (dtlsRemoteFingerprint.algorithm == RTC::DtlsTransport::FingerprintAlgorithm::NONE)
						MS_THROW_TYPE_ERROR("invalid fingerprint.algorithm value");

					auto jsonValueIt = jsonFingerprint.find("value");

					if (jsonValueIt == jsonFingerprint.end())
						MS_THROW_TYPE_ERROR("missing fingerprint.value");
					else if (!jsonValueIt->is_string())
						MS_THROW_TYPE_ERROR("wrong fingerprint.value (not a string)");

					dtlsRemoteFingerprint.value = jsonValueIt->get<std::string>();

					// Just use the first fingerprint.
					break;
				}

				auto jsonRoleIt = jsonDtlsParametersIt->find("role");

				if (jsonRoleIt != jsonDtlsParametersIt->end())
				{
					if (!jsonRoleIt->is_string())
						MS_THROW_TYPE_ERROR("wrong dtlsParameters.role (not a string)");

					dtlsRemoteRole = RTC::DtlsTransport::StringToRole(jsonRoleIt->get<std::string>());

					if (dtlsRemoteRole == RTC::DtlsTransport::Role::NONE)
						MS_THROW_TYPE_ERROR("invalid dtlsParameters.role value");
				}
				else
				{
					dtlsRemoteRole = RTC::DtlsTransport::Role::AUTO;
				}

				// Set local DTLS role.
				switch (dtlsRemoteRole)
				{
					case RTC::DtlsTransport::Role::CLIENT:
					{
						this->dtlsRole = RTC::DtlsTransport::Role::SERVER;

						break;
					}
					case RTC::DtlsTransport::Role::SERVER:
					{
						this->dtlsRole = RTC::DtlsTransport::Role::CLIENT;

						break;
					}
					// If the peer has role "auto" we become "client" since we are ICE controlled.
					case RTC::DtlsTransport::Role::AUTO:
					{
						this->dtlsRole = RTC::DtlsTransport::Role::CLIENT;

						break;
					}
					case RTC::DtlsTransport::Role::NONE:
					{
						MS_THROW_TYPE_ERROR("invalid remote DTLS role");
					}
				}

				this->connected = true;

				// Pass the remote fingerprint to the DTLS transport.
				if (this->dtlsTransport->SetRemoteFingerprint(dtlsRemoteFingerprint))
				{
					// If everything is fine, we may run the DTLS transport if ready.
					MayRunDtlsTransport();
				}

				// Tell the caller about the selected local DTLS role.
				json data = json::object();

				switch (this->dtlsRole)
				{
					case RTC::DtlsTransport::Role::CLIENT:
						data["dtlsLocalRole"] = "client";
						break;

					case RTC::DtlsTransport::Role::SERVER:
						data["dtlsLocalRole"] = "server";
						break;

					default:
						MS_ABORT("invalid local DTLS role");
				}

				request->Accept(data);

				break;
			}

			case Channel::Request::MethodId::TRANSPORT_SET_MAX_INCOMING_BITRATE:
			{
				static constexpr uint32_t MinBitrate{ 10000 };

				auto jsonBitrateIt = request->data.find("bitrate");

				if (jsonBitrateIt == request->data.end() || !jsonBitrateIt->is_number_unsigned())
					MS_THROW_TYPE_ERROR("missing bitrate");

				auto bitrate = jsonBitrateIt->get<uint32_t>();

				if (bitrate < MinBitrate)
					bitrate = MinBitrate;

				this->maxIncomingBitrate = bitrate;

				MS_DEBUG_TAG(bwe, "WebRtcTransport maximum incoming bitrate set to %" PRIu32 "bps", bitrate);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::TRANSPORT_RESTART_ICE:
			{
				std::string usernameFragment = Utils::Crypto::GetRandomString(16);
				std::string password         = Utils::Crypto::GetRandomString(32);

				this->iceServer->SetUsernameFragment(usernameFragment);
				this->iceServer->SetPassword(password);

				MS_DEBUG_DEV(
				  "WebRtcTransport ICE usernameFragment and password changed [id:%s]", this->id.c_str());

				// Reply with the updated ICE local parameters.
				json data = json::object();

				data["iceParameters"]    = json::object();
				auto jsonIceParametersIt = data.find("iceParameters");

				(*jsonIceParametersIt)["usernameFragment"] = this->iceServer->GetUsernameFragment();
				(*jsonIceParametersIt)["password"]         = this->iceServer->GetPassword();
				(*jsonIceParametersIt)["iceLite"]          = true;

				request->Accept(data);

				break;
			}

			default:
			{
				// Pass it to the parent class.
				RTC::Transport::HandleRequest(request);
			}
		}
	}

	inline bool WebRtcTransport::IsConnected() const
	{
		return (
		  this->iceSelectedTuple != nullptr &&
		  this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED);
	}

	void WebRtcTransport::MayRunDtlsTransport()
	{
		MS_TRACE();

		// Do nothing if we have the same local DTLS role as the DTLS transport.
		// NOTE: local role in DTLS transport can be NONE, but not ours.
		if (this->dtlsTransport->GetLocalRole() == this->dtlsRole)
			return;

		// Check our local DTLS role.
		switch (this->dtlsRole)
		{
			// If still 'auto' then transition to 'server' if ICE is 'connected' or
			// 'completed'.
			case RTC::DtlsTransport::Role::AUTO:
			{
				if (
				  this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
				  this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED)
				{
					MS_DEBUG_TAG(
					  dtls, "transition from DTLS local role 'auto' to 'server' and running DTLS transport");

					this->dtlsRole = RTC::DtlsTransport::Role::SERVER;
					this->dtlsTransport->Run(RTC::DtlsTransport::Role::SERVER);
				}

				break;
			}

			// 'client' is only set if a 'connect' request was previously called with
			// remote DTLS role 'server'.
			//
			// If 'client' then wait for ICE to be 'completed' (got USE-CANDIDATE).
			//
			// NOTE: This is the theory, however let's be more flexible as told here:
			//   https://bugs.chromium.org/p/webrtc/issues/detail?id=3661
			case RTC::DtlsTransport::Role::CLIENT:
			{
				if (
				  this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
				  this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED)
				{
					MS_DEBUG_TAG(dtls, "running DTLS transport in local role 'client'");

					this->dtlsTransport->Run(RTC::DtlsTransport::Role::CLIENT);
				}

				break;
			}

			// If 'server' then run the DTLS transport if ICE is 'connected' (not yet
			// USE-CANDIDATE) or 'completed'.
			case RTC::DtlsTransport::Role::SERVER:
			{
				if (
				  this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
				  this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED)
				{
					MS_DEBUG_TAG(dtls, "running DTLS transport in local role 'server'");

					this->dtlsTransport->Run(RTC::DtlsTransport::Role::SERVER);
				}

				break;
			}

			case RTC::DtlsTransport::Role::NONE:
			{
				MS_ABORT("local DTLS role not set");
			}
		}
	}

	void WebRtcTransport::SendRtpPacket(
	  RTC::RtpPacket* packet, RTC::Consumer* consumer, bool retransmitted, bool probation)
	{
		MS_TRACE();

		if (!IsConnected())
			return;

		// Ensure there is sending SRTP session.
		if (this->srtpSendSession == nullptr)
		{
			MS_WARN_DEV("ignoring RTP packet due to non sending SRTP session");

			return;
		}

		auto seq            = packet->GetSequenceNumber();
		const uint8_t* data = packet->GetData();
		size_t len          = packet->GetSize();

		if (!this->srtpSendSession->EncryptRtp(&data, &len))
			return;

		this->iceSelectedTuple->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);

		// Feed the REMB client if this is a simulcast or SVC Consumer.
		// clang-format off
		if (
			this->rembClient &&
			(
				consumer->GetType() == RTC::RtpParameters::Type::SIMULCAST ||
				consumer->GetType() == RTC::RtpParameters::Type::SVC
			)
		)
		// clang-format on
		{
			if (!probation)
			{
				this->rembClient->SentRtpPacket(packet, retransmitted);

				// May need to generate probation packets.
				for (int count{ 1 }; count <= 3; ++count)
				{
					if (!this->rembClient->IsProbationNeeded())
						break;

					consumer->SendProbationRtpPacket(seq);
				}
			}
			else
			{
				this->rembClient->SentProbationRtpPacket(packet);
			}
		}
	}

	void WebRtcTransport::SendRtcpPacket(RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		if (!IsConnected())
			return;

		const uint8_t* data = packet->GetData();
		size_t len          = packet->GetSize();

		// Ensure there is sending SRTP session.
		if (this->srtpSendSession == nullptr)
		{
			MS_WARN_DEV("ignoring RTCP packet due to non sending SRTP session");

			return;
		}

		if (!this->srtpSendSession->EncryptRtcp(&data, &len))
			return;

		this->iceSelectedTuple->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	void WebRtcTransport::DistributeAvailableOutgoingBitrate()
	{
		MS_TRACE();

		// TODO: Uncomment when Transport-CC is ready.
		// MS_ASSERT(this->rembClient || this->transportCcClient, "no REMB client nor Transport-CC client");
		MS_ASSERT(this->rembClient, "no REMB client");

		std::multimap<uint16_t, RTC::Consumer*> multimapPriorityConsumer;
		uint16_t totalPriorities{ 0 };

		// Fill the map with Consumers and their priority (if > 0).
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;
			auto priority  = consumer->GetBitratePriority();

			if (priority > 0)
			{
				multimapPriorityConsumer.emplace(priority, consumer);
				totalPriorities += priority;
			}
		}

		// Nobody wants bitrate. Exit.
		if (totalPriorities == 0)
			return;

		uint32_t availableBitrate{ 0 };

		if (this->rembClient)
		{
			availableBitrate = this->rembClient->GetAvailableBitrate();

			// Resechedule next REMB event.
			this->rembClient->ResecheduleNextEvent();
		}

		MS_DEBUG_TAG(bwe, "before iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

		uint32_t remainingBitrate = availableBitrate;

		// First of all, redistribute the available bitrate by taking into account
		// Consumers' priorities.
		for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
		{
			auto priority    = it->first;
			auto consumer    = it->second;
			uint32_t bitrate = (availableBitrate * priority) / totalPriorities;

			MS_DEBUG_TAG(
			  bwe,
			  "main bitrate for Consumer [priority:%" PRIu16 ", bitrate:%" PRIu32 ", consumerId:%s]",
			  priority,
			  bitrate,
			  consumer->id.c_str());

			auto usedBitrate = consumer->UseAvailableBitrate(bitrate);

			if (usedBitrate <= remainingBitrate)
				remainingBitrate -= usedBitrate;
			else
				remainingBitrate = 0;
		}

		MS_DEBUG_TAG(bwe, "after first main iteration [remainingBitrate:%" PRIu32 "]", remainingBitrate);

		// Then redistribute the remaining bitrate by allowing Consumers to increase
		// layer by layer.
		while (remainingBitrate >= 2000)
		{
			auto previousRemainingBitrate = remainingBitrate;

			for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
			{
				auto consumer = it->second;

				MS_DEBUG_TAG(
				  bwe,
				  "layer bitrate for Consumer [bitrate:%" PRIu32 ", consumerId:%s]",
				  remainingBitrate,
				  consumer->id.c_str());

				auto usedBitrate = consumer->IncreaseLayer(remainingBitrate);

				MS_ASSERT(usedBitrate <= remainingBitrate, "Consumer used more layer bitrate than given");

				remainingBitrate -= usedBitrate;

				// No more.
				if (remainingBitrate < 2000)
					break;
			}

			// If no Consumer used bitrate, exit the loop.
			if (remainingBitrate == previousRemainingBitrate)
				break;
		}

		MS_DEBUG_TAG(
		  bwe, "after layer-by-layer iteration [remainingBitrate:%" PRIu32 "]", remainingBitrate);

		// Finally instruct Consumers to apply their computed layers.
		for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
		{
			auto consumer = it->second;

			consumer->ApplyLayers();
		}
	}

	void WebRtcTransport::SendRtcpCompoundPacket(RTC::RTCP::CompoundPacket* packet)
	{
		MS_TRACE();

		if (!IsConnected())
			return;

		const uint8_t* data = packet->GetData();
		size_t len          = packet->GetSize();

		// Ensure there is sending SRTP session.
		if (this->srtpSendSession == nullptr)
		{
			MS_WARN_TAG(rtcp, "ignoring RTCP compound packet due to non sending SRTP session");

			return;
		}

		if (!this->srtpSendSession->EncryptRtcp(&data, &len))
			return;

		this->iceSelectedTuple->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	inline void WebRtcTransport::OnPacketReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		// Increase receive transmission.
		RTC::Transport::DataReceived(len);

		// Check if it's STUN.
		if (RTC::StunPacket::IsStun(data, len))
		{
			OnStunDataReceived(tuple, data, len);
		}
		// Check if it's RTCP.
		else if (RTC::RTCP::Packet::IsRtcp(data, len))
		{
			OnRtcpDataReceived(tuple, data, len);
		}
		// Check if it's RTP.
		else if (RTC::RtpPacket::IsRtp(data, len))
		{
			OnRtpDataReceived(tuple, data, len);
		}
		// Check if it's DTLS.
		else if (RTC::DtlsTransport::IsDtls(data, len))
		{
			OnDtlsDataReceived(tuple, data, len);
		}
		else
		{
			MS_WARN_DEV("ignoring received packet of unknown type");
		}
	}

	inline void WebRtcTransport::OnStunDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		RTC::StunPacket* packet = RTC::StunPacket::Parse(data, len);

		if (packet == nullptr)
		{
			MS_WARN_DEV("ignoring wrong STUN packet received");

			return;
		}

		// Pass it to the IceServer.
		this->iceServer->ProcessStunPacket(packet, tuple);

		delete packet;
	}

	inline void WebRtcTransport::OnDtlsDataReceived(
	  const RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(dtls, "ignoring DTLS data coming from an invalid tuple");

			return;
		}

		// Trick for clients performing aggressive ICE regardless we are ICE-Lite.
		this->iceServer->ForceSelectedTuple(tuple);

		// Check that DTLS status is 'connecting' or 'connected'.
		if (
		  this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTING ||
		  this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_DEV("DTLS data received, passing it to the DTLS transport");

			this->dtlsTransport->ProcessDtlsData(data, len);
		}
		else
		{
			MS_WARN_TAG(dtls, "Transport is not 'connecting' or 'connected', ignoring received DTLS data");

			return;
		}
	}

	inline void WebRtcTransport::OnRtpDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		// Ensure DTLS is connected.
		if (this->dtlsTransport->GetState() != RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_2TAGS(dtls, rtp, "ignoring RTP packet while DTLS not connected");

			return;
		}

		// Ensure there is receiving SRTP session.
		if (this->srtpRecvSession == nullptr)
		{
			MS_DEBUG_TAG(srtp, "ignoring RTP packet due to non receiving SRTP session");

			return;
		}

		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(rtp, "ignoring RTP packet coming from an invalid tuple");

			return;
		}

		// Decrypt the SRTP packet.
		if (!this->srtpRecvSession->DecryptSrtp(data, &len))
		{
			RTC::RtpPacket* packet = RTC::RtpPacket::Parse(data, len);

			if (packet == nullptr)
			{
				MS_WARN_TAG(srtp, "DecryptSrtp() failed due to an invalid RTP packet");
			}
			else
			{
				MS_WARN_TAG(
				  srtp,
				  "DecryptSrtp() failed [ssrc:%" PRIu32 ", payloadType:%" PRIu8 ", seq:%" PRIu16 "]",
				  packet->GetSsrc(),
				  packet->GetPayloadType(),
				  packet->GetSequenceNumber());

				delete packet;
			}

			return;
		}

		RTC::RtpPacket* packet = RTC::RtpPacket::Parse(data, len);

		if (packet == nullptr)
		{
			MS_WARN_TAG(rtp, "received data is not a valid RTP packet");

			return;
		}

		// Apply the Transport RTP header extension ids so the RTP listener can use them.
		packet->SetMidExtensionId(this->rtpHeaderExtensionIds.mid);
		packet->SetRidExtensionId(this->rtpHeaderExtensionIds.rid);
		packet->SetRepairedRidExtensionId(this->rtpHeaderExtensionIds.rrid);
		packet->SetAbsSendTimeExtensionId(this->rtpHeaderExtensionIds.absSendTime);

		// Feed the REMB server.
		if (this->rembServer)
		{
			uint32_t absSendTime;

			if (packet->ReadAbsSendTime(absSendTime))
			{
				this->rembServer->IncomingPacket(
				  DepLibUV::GetTime(), packet->GetPayloadLength(), *packet, absSendTime);
			}
		}

		// Get the associated Producer.
		RTC::Producer* producer = this->rtpListener.GetProducer(packet);

		if (producer == nullptr)
		{
			MS_WARN_TAG(
			  rtp,
			  "no suitable Producer for received RTP packet [ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]",
			  packet->GetSsrc(),
			  packet->GetPayloadType());

			delete packet;

			return;
		}

		// MS_DEBUG_DEV(
		//   "RTP packet received [ssrc:%" PRIu32 ", payloadType:%" PRIu8 ", producerId:%s]",
		//   packet->GetSsrc(),
		//   packet->GetPayloadType(),
		//   producer->id.c_str());

		// Trick for clients performing aggressive ICE regardless we are ICE-Lite.
		this->iceServer->ForceSelectedTuple(tuple);

		// Pass the RTP packet to the corresponding Producer.
		producer->ReceiveRtpPacket(packet);

		delete packet;
	}

	inline void WebRtcTransport::OnRtcpDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		// Ensure DTLS is connected.
		if (this->dtlsTransport->GetState() != RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_2TAGS(dtls, rtcp, "ignoring RTCP packet while DTLS not connected");

			return;
		}

		// Ensure there is receiving SRTP session.
		if (this->srtpRecvSession == nullptr)
		{
			MS_DEBUG_TAG(srtp, "ignoring RTCP packet due to non receiving SRTP session");

			return;
		}

		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(rtcp, "ignoring RTCP packet coming from an invalid tuple");

			return;
		}

		// Decrypt the SRTCP packet.
		if (!this->srtpRecvSession->DecryptSrtcp(data, &len))
			return;

		RTC::RTCP::Packet* packet = RTC::RTCP::Packet::Parse(data, len);

		if (packet == nullptr)
		{
			MS_WARN_TAG(rtcp, "received data is not a valid RTCP compound or single packet");

			return;
		}

		// Handle each RTCP packet.
		while (packet != nullptr)
		{
			ReceiveRtcpPacket(packet);

			RTC::RTCP::Packet* previousPacket = packet;

			packet = packet->GetNext();
			delete previousPacket;
		}
	}

	void WebRtcTransport::UserOnNewProducer(RTC::Producer* producer)
	{
		MS_TRACE();

		auto& rtpHeaderExtensionIds = producer->GetRtpHeaderExtensionIds();
		auto& codecs                = producer->GetRtpParameters().codecs;

		// Set REMB server bitrate estimator:
		// - if not already set, and
		// - there is abs-send-time RTP header extension, and
		// - there is "remb" in codecs RTCP feedback.
		//
		// clang-format off
		if (
			!this->rembServer &&
			rtpHeaderExtensionIds.absSendTime != 0u &&
			std::any_of(
				codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
				{
					return std::any_of(
						codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RtcpFeedback& fb)
						{
							return fb.type == "goog-remb";
						});
				})
		)
		// clang-format on
		{
			MS_DEBUG_TAG(bwe, "enabling REMB server");

			this->rembServer = new RTC::RembServer::RemoteBitrateEstimatorAbsSendTime(this);
		}
	}

	void WebRtcTransport::UserOnNewConsumer(RTC::Consumer* consumer)
	{
		MS_TRACE();

		auto& rtpHeaderExtensionIds = consumer->GetRtpHeaderExtensionIds();
		auto& codecs                = consumer->GetRtpParameters().codecs;

		// Set REMB client bitrate estimator:
		// - if not already set, and
		// - Consumer is simulcast or SVC, and
		// - there is abs-send-time RTP header extension, and
		// - there is "remb" in codecs RTCP feedback.
		//
		// clang-format off
		if (
			!this->rembClient &&
			(
				consumer->GetType() == RTC::RtpParameters::Type::SIMULCAST ||
				consumer->GetType() == RTC::RtpParameters::Type::SVC
			) &&
			rtpHeaderExtensionIds.absSendTime != 0u &&
			std::any_of(
				codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
				{
					return std::any_of(
						codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RtcpFeedback& fb)
						{
							return fb.type == "goog-remb";
						});
				})
		)
		// clang-format on
		{
			MS_DEBUG_TAG(bwe, "enabling REMB client");

			this->rembClient = new RTC::RembClient(
			  this, this->initialAvailableOutgoingBitrate, this->minimumAvailableOutgoingBitrate);

			// Tell all the Consumers that we are gonna manage their bitrate.
			for (auto& kv : this->mapConsumers)
			{
				auto* consumer = kv.second;

				consumer->SetExternallyManagedBitrate();
			}
		}
		// Otherwise, if REMB client is set, tell the new Consumer that we are
		// gonna manage its bitrate.
		else if (this->rembClient)
		{
			consumer->SetExternallyManagedBitrate();
		}
	}

	void WebRtcTransport::UserOnRembFeedback(RTC::RTCP::FeedbackPsRembPacket* remb)
	{
		MS_TRACE();

		if (!this->rembClient)
			return;

		this->rembClient->ReceiveRembFeedback(remb);
	}

	void WebRtcTransport::UserOnSendSctpData(const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (this->dtlsTransport->GetState() != RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_WARN_TAG(sctp, "DTLS not connected, cannot send SCTP data");

			return;
		}

		this->dtlsTransport->SendApplicationData(data, len);
	}

	inline void WebRtcTransport::OnConsumerNeedBitrateChange(RTC::Consumer* /*consumer*/)
	{
		MS_TRACE();

		DistributeAvailableOutgoingBitrate();
	}

	inline void WebRtcTransport::OnUdpSocketPacketReceived(
	  RTC::UdpSocket* socket, const uint8_t* data, size_t len, const struct sockaddr* remoteAddr)
	{
		MS_TRACE();

		RTC::TransportTuple tuple(socket, remoteAddr);

		OnPacketReceived(&tuple, data, len);
	}

	inline void WebRtcTransport::OnRtcTcpConnectionClosed(
	  RTC::TcpServer* /*tcpServer*/, RTC::TcpConnection* connection)
	{
		MS_TRACE();

		RTC::TransportTuple tuple(connection);

		this->iceServer->RemoveTuple(&tuple);
	}

	inline void WebRtcTransport::OnTcpConnectionPacketReceived(
	  RTC::TcpConnection* connection, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		RTC::TransportTuple tuple(connection);

		OnPacketReceived(&tuple, data, len);
	}

	inline void WebRtcTransport::OnIceServerSendStunPacket(
	  const RTC::IceServer* /*iceServer*/, const RTC::StunPacket* packet, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		// Send the STUN response over the same transport tuple.
		tuple->Send(packet->GetData(), packet->GetSize());

		// Increase send transmission.
		RTC::Transport::DataSent(packet->GetSize());
	}

	inline void WebRtcTransport::OnIceServerSelectedTuple(
	  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		/*
		 * RFC 5245 section 11.2 "Receiving Media":
		 *
		 * ICE implementations MUST be prepared to receive media on each component
		 * on any candidates provided for that component.
		 */

		// Update the selected tuple.
		this->iceSelectedTuple = tuple;

		MS_DEBUG_TAG(ice, "ICE selected tuple");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		this->iceSelectedTuple->FillJson(data["iceSelectedTuple"]);

		Channel::Notifier::Emit(this->id, "iceselectedtuplechange", data);
	}

	inline void WebRtcTransport::OnIceServerConnected(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(ice, "ICE connected");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["iceState"] = "connected";

		Channel::Notifier::Emit(this->id, "icestatechange", data);

		// If ready, run the DTLS handler.
		MayRunDtlsTransport();
	}

	inline void WebRtcTransport::OnIceServerCompleted(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(ice, "ICE completed");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["iceState"] = "completed";

		Channel::Notifier::Emit(this->id, "icestatechange", data);

		// If ready, run the DTLS handler.
		MayRunDtlsTransport();
	}

	inline void WebRtcTransport::OnIceServerDisconnected(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		// Unset the selected tuple.
		this->iceSelectedTuple = nullptr;

		MS_DEBUG_TAG(ice, "ICE disconnected");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["iceState"] = "disconnected";

		Channel::Notifier::Emit(this->id, "icestatechange", data);

		// Tell the parent class.
		RTC::Transport::Disconnected();
	}

	inline void WebRtcTransport::OnDtlsTransportConnecting(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(dtls, "DTLS connecting");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["dtlsState"] = "connecting";

		Channel::Notifier::Emit(this->id, "dtlsstatechange", data);
	}

	inline void WebRtcTransport::OnDtlsTransportConnected(
	  const RTC::DtlsTransport* /*dtlsTransport*/,
	  RTC::SrtpSession::Profile srtpProfile,
	  uint8_t* srtpLocalKey,
	  size_t srtpLocalKeyLen,
	  uint8_t* srtpRemoteKey,
	  size_t srtpRemoteKeyLen,
	  std::string& remoteCert)
	{
		MS_TRACE();

		MS_DEBUG_TAG(dtls, "DTLS connected");

		// Close it if it was already set and update it.
		if (this->srtpSendSession != nullptr)
		{
			delete this->srtpSendSession;
			this->srtpSendSession = nullptr;
		}
		if (this->srtpRecvSession != nullptr)
		{
			delete this->srtpRecvSession;
			this->srtpRecvSession = nullptr;
		}

		try
		{
			this->srtpSendSession = new RTC::SrtpSession(
			  RTC::SrtpSession::Type::OUTBOUND, srtpProfile, srtpLocalKey, srtpLocalKeyLen);
		}
		catch (const MediaSoupError& error)
		{
			MS_ERROR("error creating SRTP sending session: %s", error.what());
		}

		try
		{
			this->srtpRecvSession = new RTC::SrtpSession(
			  RTC::SrtpSession::Type::INBOUND, srtpProfile, srtpRemoteKey, srtpRemoteKeyLen);
		}
		catch (const MediaSoupError& error)
		{
			MS_ERROR("error creating SRTP receiving session: %s", error.what());

			delete this->srtpSendSession;
			this->srtpSendSession = nullptr;
		}

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["dtlsState"]      = "connected";
		data["dtlsRemoteCert"] = remoteCert;

		Channel::Notifier::Emit(this->id, "dtlsstatechange", data);

		// Tell the parent class.
		RTC::Transport::Connected();
	}

	inline void WebRtcTransport::OnDtlsTransportFailed(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_WARN_TAG(dtls, "DTLS failed");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["dtlsState"] = "failed";

		Channel::Notifier::Emit(this->id, "dtlsstatechange", data);
	}

	inline void WebRtcTransport::OnDtlsTransportClosed(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_WARN_TAG(dtls, "DTLS remotely closed");

		// Notify the Node WebRtcTransport.
		json data = json::object();

		data["dtlsState"] = "closed";

		Channel::Notifier::Emit(this->id, "dtlsstatechange", data);

		// Tell the parent class.
		RTC::Transport::Disconnected();
	}

	inline void WebRtcTransport::OnDtlsTransportSendData(
	  const RTC::DtlsTransport* /*dtlsTransport*/, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (this->iceSelectedTuple == nullptr)
		{
			MS_WARN_TAG(dtls, "no selected tuple set, cannot send DTLS packet");

			return;
		}

		this->iceSelectedTuple->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	inline void WebRtcTransport::OnDtlsTransportApplicationDataReceived(
	  const RTC::DtlsTransport* /*dtlsTransport*/, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (!this->sctpAssociation)
		{
			MS_DEBUG_TAG(sctp, "ignoring DTLS application data (SCTP not enabled)");

			return;
		}

		// Pass it to the SctpAssociation.
		this->sctpAssociation->ProcessSctpData(data, len);
	}

	inline void WebRtcTransport::OnRembClientAvailableBitrate(
	  RTC::RembClient* /*rembClient*/, uint32_t availableBitrate)
	{
		MS_TRACE();

		MS_DEBUG_TAG(bwe, "outgoing available bitrate [bitrate:%" PRIu32 "bps]", availableBitrate);

		DistributeAvailableOutgoingBitrate();
	}

	inline void WebRtcTransport::OnRembServerAvailableBitrate(
	  const RTC::RembServer::RemoteBitrateEstimator* /*rembServer*/,
	  const std::vector<uint32_t>& ssrcs,
	  uint32_t availableBitrate)
	{
		MS_TRACE();

		// Limit announced bitrate if requested via API.
		if (this->maxIncomingBitrate != 0u)
			availableBitrate = std::min(availableBitrate, this->maxIncomingBitrate);

		if (MS_HAS_DEBUG_TAG(bwe))
		{
			std::ostringstream ssrcsStream;

			if (!ssrcs.empty())
			{
				std::copy(ssrcs.begin(), ssrcs.end() - 1, std::ostream_iterator<uint32_t>(ssrcsStream, ","));
				ssrcsStream << ssrcs.back();
			}

			// MS_DEBUG_TAG(
			//   bwe,
			//   "sending RTCP REMB packet [bitrate:%" PRIu32 "bps, ssrcs:%s]",
			//   availableBitrate,
			//   ssrcsStream.str().c_str());
		}

		RTC::RTCP::FeedbackPsRembPacket packet(0, 0);

		packet.SetBitrate(availableBitrate);
		packet.SetSsrcs(ssrcs);
		packet.Serialize(RTC::RTCP::Buffer);
		SendRtcpPacket(&packet);
	}
} // namespace RTC
