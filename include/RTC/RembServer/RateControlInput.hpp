#ifndef MS_RTC_REMB_SERVER_RATE_CONTROL_INPUT_HPP
#define MS_RTC_REMB_SERVER_RATE_CONTROL_INPUT_HPP

#include "common.hpp"
#include "RTC/RembServer/BandwidthUsage.hpp"

// webrtc/modules/remote_bitrate_estimator/include/bweDefines.h

namespace RTC
{
	namespace RembServer
	{
		struct RateControlInput
		{
			RateControlInput(BandwidthUsage bwState, uint32_t incomingBitrate, double noiseVar);

			BandwidthUsage bwState;
			uint32_t incomingBitrate{ 0 };
			double noiseVar{ 0 };
		};

		/* Inline methods. */

		inline RateControlInput::RateControlInput(
		  BandwidthUsage bwState, const uint32_t incomingBitrate, double noiseVar)
		  : bwState(bwState), incomingBitrate(incomingBitrate), noiseVar(noiseVar)
		{
		}
	} // namespace RembServer
} // namespace RTC

#endif
