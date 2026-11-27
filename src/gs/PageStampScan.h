#pragma once
#include <stdint.h>
#include <algorithm>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
namespace PageStampScan
{
	inline bool ContiguousValid(const uint32_t* p, uint32_t count, uint32_t stamp)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const auto limit = vdupq_n_u32(stamp);
		while(count >= 4)
		{
			const auto greater = vcgtq_u32(vld1q_u32(p), limit);
			const auto pairs = vorr_u32(vget_low_u32(greater), vget_high_u32(greater));
			if(vget_lane_u32(pairs,0) | vget_lane_u32(pairs,1)) return false;
			p += 4;
			count -= 4;
		}
#endif
		while(count--) if(*p++ > stamp) return false;
		return true;
	}
	// The same wrapped page sequence as the scalar loop, with contiguous loads.
	inline bool Valid(const uint32_t* p, uint32_t capacity, uint32_t first, uint32_t count, uint32_t stamp)
	{
		if(count == 0) return true;
		first %= capacity;
		count = std::min(count, capacity);
		const uint32_t head = std::min(count, capacity - first);
		return ContiguousValid(p + first, head, stamp) && ContiguousValid(p, count - head, stamp);
	}
}
