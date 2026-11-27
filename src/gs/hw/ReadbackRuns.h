#pragma once
#include <cstddef>
#include <cstdint>
namespace ReadbackRuns
{
	// Preserve input order, including duplicates. Never cross a row, include
	// a gap, or exceed maxRun. No allocation or sorting on the GS thread.
	inline uint32_t Count(const uint32_t* indices, size_t size, size_t pos, uint32_t rowWidth, uint32_t maxRun)
	{
		if(pos >= size || rowWidth == 0 || maxRun == 0) return 0;
		const uint32_t first = indices[pos];
		const uint32_t remaining = rowWidth - first % rowWidth;
		uint32_t n = 1;
		while(n < maxRun && n < remaining && n < size - pos &&
		      indices[pos + n] > first && indices[pos + n] - first == n) ++n;
		return n;
	}
}
