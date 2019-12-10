#ifndef MS_RTC_SVC_CONSUMER_HPP
#define MS_RTC_SVC_CONSUMER_HPP

#include "RTC/Codecs/PayloadDescriptorHandler.hpp"
#include "RTC/Consumer.hpp"
#include "RTC/RtpStreamSend.hpp"
#include "RTC/SeqManager.hpp"
#include <map>

namespace RTC
{
	class SvcConsumer : public RTC::Consumer, public RTC::RtpStreamSend::Listener
	{
	public:
		SvcConsumer(const std::string& id, RTC::Consumer::Listener* listener, json& data);
		~SvcConsumer() override;

	public:
		void FillJson(json& jsonObject) const override;
		void FillJsonStats(json& jsonArray) const override;
		void FillJsonScore(json& jsonObject) const override;
		void HandleRequest(Channel::Request* request) override;
		bool IsActive() const override;
		void ProducerRtpStream(RTC::RtpStream* rtpStream, uint32_t mappedSsrc) override;
		void ProducerNewRtpStream(RTC::RtpStream* rtpStream, uint32_t mappedSsrc) override;
		void ProducerRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;
		void ProducerRtcpSenderReport(RTC::RtpStream* rtpStream, bool first) override;
		void SetExternallyManagedBitrate() override;
		uint16_t GetBitratePriority() const override;
		uint32_t UseAvailableBitrate(uint32_t bitrate) override;
		uint32_t IncreaseLayer(uint32_t bitrate) override;
		void ApplyLayers() override;
		void SendRtpPacket(RTC::RtpPacket* packet) override;
		void SendProbationRtpPacket(uint16_t seq) override;
		void GetRtcp(RTC::RTCP::CompoundPacket* packet, RTC::RtpStreamSend* rtpStream, uint64_t now) override;
		std::vector<RTC::RtpStreamSend*> GetRtpStreams() override;
		void NeedWorstRemoteFractionLost(uint32_t mappedSsrc, uint8_t& worstRemoteFractionLost) override;
		void ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket) override;
		void ReceiveKeyFrameRequest(RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc) override;
		void ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report) override;
		uint32_t GetTransmissionRate(uint64_t now) override;

	private:
		void UserOnTransportConnected() override;
		void UserOnTransportDisconnected() override;
		void UserOnPaused() override;
		void UserOnResumed() override;
		void CreateRtpStream();
		void RequestKeyFrame();
		void MayChangeLayers(bool force = false);
		bool RecalculateTargetLayers(int16_t& newTargetSpatialLayer, int16_t& newTargetTemporalLayer) const;
		void UpdateTargetLayers(int16_t newTargetSpatialLayer, int16_t newTargetTemporalLayer);
		void EmitScore() const;
		void EmitLayersChange() const;

		/* Pure virtual methods inherited from RtpStreamSend::Listener. */
	public:
		void OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;
		void OnRtpStreamRetransmitRtpPacket(
		  RTC::RtpStreamSend* rtpStream, RTC::RtpPacket* packet, bool probation = false) override;

	private:
		// Allocated by this.
		RTC::RtpStreamSend* rtpStream{ nullptr };
		// Others.
		std::vector<RTC::RtpStreamSend*> rtpStreams;
		RTC::RtpStream* producerRtpStream{ nullptr };
		bool syncRequired{ false };
		RTC::SeqManager<uint16_t> rtpSeqManager;
		int16_t preferredSpatialLayer{ -1 };
		int16_t preferredTemporalLayer{ -1 };
		int16_t provisionalTargetSpatialLayer{ -1 };
		int16_t provisionalTargetTemporalLayer{ -1 };
		std::unique_ptr<RTC::Codecs::EncodingContext> encodingContext;
		bool externallyManagedBitrate{ false };
	};

	/* Inline methods. */

	inline bool SvcConsumer::IsActive() const
	{
		return (RTC::Consumer::IsActive() && this->producerRtpStream);
	}

	inline std::vector<RTC::RtpStreamSend*> SvcConsumer::GetRtpStreams()
	{
		return this->rtpStreams;
	}
} // namespace RTC

#endif
