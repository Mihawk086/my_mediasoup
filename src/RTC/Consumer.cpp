#define MS_CLASS "RTC::Consumer"
// #define MS_LOG_DEV

#include "RTC/Consumer.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Channel/Notifier.hpp"

namespace RTC
{
	/* Instance methods. */

	Consumer::Consumer(const std::string& id, Listener* listener, json& data, RTC::RtpParameters::Type type)
	  : id(id), listener(listener), type(type)
	{
		MS_TRACE();

		auto jsonKindIt = data.find("kind");

		if (jsonKindIt == data.end() || !jsonKindIt->is_string())
			MS_THROW_TYPE_ERROR("missing kind");

		// This may throw.
		this->kind = RTC::Media::GetKind(jsonKindIt->get<std::string>());

		if (this->kind == RTC::Media::Kind::ALL)
			MS_THROW_TYPE_ERROR("invalid empty kind");

		auto jsonRtpParametersIt = data.find("rtpParameters");

		if (jsonRtpParametersIt == data.end() || !jsonRtpParametersIt->is_object())
			MS_THROW_TYPE_ERROR("missing rtpParameters");

		// This may throw.
		this->rtpParameters = RTC::RtpParameters(*jsonRtpParametersIt);

		if (this->rtpParameters.encodings.empty())
			MS_THROW_TYPE_ERROR("empty rtpParameters.encodings");

		// All encodings must have SSRCs.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			if (encoding.ssrc == 0)
				MS_THROW_TYPE_ERROR("invalid encoding in rtpParameters (missing ssrc)");
			else if (encoding.hasRtx && encoding.rtx.ssrc == 0)
				MS_THROW_TYPE_ERROR("invalid encoding in rtpParameters (missing rtx.ssrc)");
		}

		auto jsonConsumableRtpEncodingsIt = data.find("consumableRtpEncodings");

		if (jsonConsumableRtpEncodingsIt == data.end() || !jsonConsumableRtpEncodingsIt->is_array())
			MS_THROW_TYPE_ERROR("missing consumableRtpEncodings");

		if (jsonConsumableRtpEncodingsIt->empty())
			MS_THROW_TYPE_ERROR("empty consumableRtpEncodings");

		this->consumableRtpEncodings.reserve(jsonConsumableRtpEncodingsIt->size());

		for (size_t i{ 0 }; i < jsonConsumableRtpEncodingsIt->size(); ++i)
		{
			auto& entry = (*jsonConsumableRtpEncodingsIt)[i];

			// This may throw due the constructor of RTC::RtpEncodingParameters.
			this->consumableRtpEncodings.emplace_back(entry);

			// Verify that it has ssrc field.
			auto& encoding = this->consumableRtpEncodings[i];

			if (encoding.ssrc == 0u)
				MS_THROW_TYPE_ERROR("wrong encoding in consumableRtpEncodings (missing ssrc)");
		}

		// Fill RTP header extension ids and their mapped values.
		// This may throw.
		for (auto& exten : this->rtpParameters.headerExtensions)
		{
			if (exten.id == 0u)
				MS_THROW_TYPE_ERROR("RTP extension id cannot be 0");

			if (this->rtpHeaderExtensionIds.ssrcAudioLevel == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::SSRC_AUDIO_LEVEL)
			{
				this->rtpHeaderExtensionIds.ssrcAudioLevel = exten.id;
			}

			if (this->rtpHeaderExtensionIds.videoOrientation == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::VIDEO_ORIENTATION)
			{
				this->rtpHeaderExtensionIds.videoOrientation = exten.id;
			}

			if (this->rtpHeaderExtensionIds.absSendTime == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME)
			{
				this->rtpHeaderExtensionIds.absSendTime = exten.id;
			}

			if (this->rtpHeaderExtensionIds.mid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::MID)
			{
				this->rtpHeaderExtensionIds.mid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rrid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::REPAIRED_RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rrid = exten.id;
			}
		}

		auto jsonPausedIt = data.find("paused");

		if (jsonPausedIt != data.end() && jsonPausedIt->is_boolean())
			this->paused = jsonPausedIt->get<bool>();

		// Fill supported codec payload types.
		for (auto& codec : this->rtpParameters.codecs)
		{
			if (codec.mimeType.IsMediaCodec())
				this->supportedCodecPayloadTypes.insert(codec.payloadType);
		}

		// Fill media SSRCs vector.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			this->mediaSsrcs.push_back(encoding.ssrc);
		}

		// Set the RTCP report generation interval.
		if (this->kind == RTC::Media::Kind::AUDIO)
			this->maxRtcpInterval = RTC::RTCP::MaxAudioIntervalMs;
		else
			this->maxRtcpInterval = RTC::RTCP::MaxVideoIntervalMs;
	}

	Consumer::~Consumer()
	{
		MS_TRACE();
	}

	void Consumer::FillJson(json& jsonObject) const
	{
		MS_TRACE();

		// Add id.
		jsonObject["id"] = this->id;

		// Add kind.
		jsonObject["kind"] = RTC::Media::GetString(this->kind);

		// Add rtpParameters.
		this->rtpParameters.FillJson(jsonObject["rtpParameters"]);

		// Add type.
		jsonObject["type"] = RTC::RtpParameters::GetTypeString(this->type);

		// Add consumableRtpEncodings.
		jsonObject["consumableRtpEncodings"] = json::array();
		auto jsonConsumableRtpEncodingsIt    = jsonObject.find("consumableRtpEncodings");

		for (size_t i{ 0 }; i < this->consumableRtpEncodings.size(); ++i)
		{
			jsonConsumableRtpEncodingsIt->emplace_back(json::value_t::object);

			auto& jsonEntry = (*jsonConsumableRtpEncodingsIt)[i];
			auto& encoding  = this->consumableRtpEncodings[i];

			encoding.FillJson(jsonEntry);
		}

		// Add supportedCodecPayloadTypes.
		jsonObject["supportedCodecPayloadTypes"] = this->supportedCodecPayloadTypes;

		// Add paused.
		jsonObject["paused"] = this->paused;

		// Add producerPaused.
		jsonObject["producerPaused"] = this->producerPaused;
	}

	void Consumer::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::CONSUMER_DUMP:
			{
				json data = json::object();

				FillJson(data);

				request->Accept(data);

				break;
			}

			case Channel::Request::MethodId::CONSUMER_GET_STATS:
			{
				json data = json::array();

				FillJsonStats(data);

				request->Accept(data);

				break;
			}

			case Channel::Request::MethodId::CONSUMER_PAUSE:
			{
				if (this->paused)
				{
					request->Accept();

					return;
				}

				bool wasActive = IsActive();

				this->paused = true;

				MS_DEBUG_DEV("Consumer paused [consumerId:%s]", this->id.c_str());

				if (wasActive)
					UserOnPaused();

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::CONSUMER_RESUME:
			{
				if (!this->paused)
				{
					request->Accept();

					return;
				}

				this->paused = false;

				MS_DEBUG_DEV("Consumer resumed [consumerId:%s]", this->id.c_str());

				if (IsActive())
					UserOnResumed();

				request->Accept();

				break;
			}

			default:
			{
				MS_THROW_ERROR("unknown method '%s'", request->method.c_str());
			}
		}
	}

	void Consumer::TransportConnected()
	{
		MS_TRACE();

		this->transportConnected = true;

		MS_DEBUG_DEV("Transport connected [consumerId:%s]", this->id.c_str());

		UserOnTransportConnected();
	}

	void Consumer::TransportDisconnected()
	{
		MS_TRACE();

		this->transportConnected = false;

		MS_DEBUG_DEV("Transport disconnected [consumerId:%s]", this->id.c_str());

		UserOnTransportDisconnected();
	}

	void Consumer::ProducerPaused()
	{
		MS_TRACE();

		if (this->producerPaused)
			return;

		bool wasActive = IsActive();

		this->producerPaused = true;

		MS_DEBUG_DEV("Producer paused [consumerId:%s]", this->id.c_str());

		if (wasActive)
			UserOnPaused();

		Channel::Notifier::Emit(this->id, "producerpause");
	}

	void Consumer::ProducerResumed()
	{
		MS_TRACE();

		if (!this->producerPaused)
			return;

		this->producerPaused = false;

		MS_DEBUG_DEV("Producer resumed [consumerId:%s]", this->id.c_str());

		if (IsActive())
			UserOnResumed();

		Channel::Notifier::Emit(this->id, "producerresume");
	}

	// The caller (Router) is supposed to proceed with the deletion of this Consumer
	// right after calling this method. Otherwise ugly things may happen.
	void Consumer::ProducerClosed()
	{
		MS_TRACE();

		this->producerClosed = true;

		MS_DEBUG_DEV("Producer closed [consumerId:%s]", this->id.c_str());

		Channel::Notifier::Emit(this->id, "producerclose");

		this->listener->OnConsumerProducerClosed(this);
	}

	void Consumer::SetExternallyManagedBitrate()
	{
		MS_TRACE();

		// Do nothing.
	}

	uint16_t Consumer::GetBitratePriority() const
	{
		MS_TRACE();

		// This method must be override by subclasses with layers. By default
		// it just returns 0.
		return 0u;
	}

	uint32_t Consumer::UseAvailableBitrate(uint32_t /*bitrate*/)
	{
		MS_TRACE();

		// This method must be override by subclasses with layers. By default
		// it just returns 0.
		return 0;
	}

	uint32_t Consumer::IncreaseLayer(uint32_t /*bitrate*/)
	{
		MS_TRACE();

		// This method must be override by subclasses with layers. By default
		// it just returns 0.
		return 0;
	}

	void Consumer::ApplyLayers()
	{
		MS_TRACE();

		// This method must be override by subclasses with layers. By default
		// it does nothing.
	}
} // namespace RTC
