#include "JitMemory.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <mutex>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include "JitPool.h"

// Public kubridge v0.3.1_hotfix ABI, weak VitaSDK imports.
extern "C" SceUID kuKernelMemReserve(void**, SceSize, SceKernelMemBlockType);
extern "C" int kuKernelMemCommit(void*, SceSize, SceUInt32, void*);
extern "C" void kuKernelFlushCaches(const void*, SceSize);

// Code is written through the VM domain (sceKernelOpenVMDomain makes the
// RX block writable for the calling process), then the written range is made
// coherent between the data and instruction caches with sceKernelSyncVMDomain.

namespace
{
	CJitPool g_pool;
	SceUID g_block = -1;
	bool g_expanded = false;
	uintptr_t g_poolBase = 0;
	std::recursive_mutex g_writeMutex;
	int g_writeDepth = 0;
	std::string g_diagnostics;
	VITAJIT_STATS g_stats;
	SceUInt64 g_writeStart = 0;
	// Free memory kept for what the VU1 thread and the current EE slice compile
	// before the next safe point; the EE's code is dropped when less is left.
	constexpr size_t FLUSH_RESERVE = 1536 * 1024;
	constexpr size_t EMERGENCY_RESERVE = 512 * 1024;
	constexpr SceUInt64 MIN_FLUSH_INTERVAL_US = 5000000;
	size_t g_softLimit = 0; //0 = the whole pool
	SceUInt64 g_lastFlush = 0;
	unsigned int g_flushes = 0;

	// A weak import is not callable just because a function address is non-null.
	// Accept only the resolved ARM syscall or absolute trampoline encoding.
	bool IsResolved(uintptr_t address)
	{
		if(!address || (address & 3)) return false;
		const auto* words = reinterpret_cast<const volatile uint32_t*>(address);
		const uint32_t first = words[0], second = words[1];
		if((first & 0xFFF0F000U) != 0xE300C000U) return false;
		const uint32_t low = ((first >> 4) & 0xF000U) | (first & 0xFFFU);
		if(second == 0xEF000000U) return low != 0;
		if((second & 0xFFF0F000U) != 0xE340C000U) return false;
		const uint32_t high = ((second >> 4) & 0xF000U) | (second & 0xFFFU);
		return (low | high) != 0;
	}

	bool CanExpand()
	{
		return IsResolved(reinterpret_cast<uintptr_t>(&kuKernelMemReserve)) &&
		       IsResolved(reinterpret_cast<uintptr_t>(&kuKernelMemCommit)) &&
		       IsResolved(reinterpret_cast<uintptr_t>(&kuKernelFlushCaches));
	}
}

void VitaJit_SetSoftLimit(size_t bytes)
{
	g_softLimit = bytes;
}

unsigned int VitaJit_GetFlushCount()
{
	return g_flushes;
}

extern "C" int vitaps2_jit_should_flush()
{
	size_t limit = g_pool.GetCapacity();
	if((g_softLimit != 0) && (g_softLimit < limit)) limit = g_softLimit;
	const size_t used = g_pool.GetUsedBytes();
	if(used < limit && limit - used > FLUSH_RESERVE) return 0;
	// What is left after a flush may be VU and IOP code that cannot be dropped:
	// never spend all the time flushing.
	SceUInt64 now = sceKernelGetProcessTimeWide();
	// A nearly full pool must not abort merely because a recent flush
	// happened. The VM calls this only at its safe EE discard point.
	const bool emergency = used >= limit || limit - used <= EMERGENCY_RESERVE;
	if(!emergency && (g_flushes != 0) && (now - g_lastFlush < MIN_FLUSH_INTERVAL_US)) return 0;
	g_lastFlush = now;
	g_flushes++;
	std::printf("JIT memory: %u/%u KB used, dropping EE code (flush %u)\n", static_cast<unsigned int>(g_pool.GetUsedBytes() / 1024),
	            static_cast<unsigned int>(limit / 1024), g_flushes);
	return 1;
}

VITAJIT_STATS VitaJit_GetStats()
{
	VITAJIT_STATS stats = g_stats;
	stats.freeRanges = static_cast<unsigned int>(g_pool.GetFreeRangeCount());
	return stats;
}

std::string VitaJit_GetDiagnostics()
{
	return g_diagnostics;
}

bool VitaJit_Init(size_t minSize, size_t maxSize, bool allowExpansion)
{
	if(g_block >= 0) return true;
	const size_t MB = 1024 * 1024;
	minSize = std::max<size_t>((minSize + MB - 1) & ~(MB - 1), MB);
	maxSize = std::max<size_t>((maxSize + MB - 1) & ~(MB - 1), minSize);

	SceKernelFreeMemorySizeInfo freeInfo = {};
	freeInfo.size = sizeof(freeInfo);
	sceKernelGetFreeMemorySize(&freeInfo);
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer), "Free memory: user %u KB, cdram %u KB, phycont %u KB\n",
	              freeInfo.size_user / 1024, freeInfo.size_cdram / 1024, freeInfo.size_phycont / 1024);
	g_diagnostics = buffer;
	std::printf("%s", buffer);

	// Keep a single contiguous pool so linked ARM blocks stay within branch
	// reach. Try the optional already-installed kernel bridge, never install
	// or patch kernel modules ourselves. Limit expansion to 32 MiB.
	if(allowExpansion && maxSize > 16 * MB && CanExpand())
	{
		const size_t expandedSize = std::min(maxSize, 32 * MB);
		void* addr = nullptr;
		const SceUID id = kuKernelMemReserve(&addr, static_cast<SceSize>(expandedSize),
		                                    static_cast<SceKernelMemBlockType>(0x0C20D050));
		int result = id;
		if(id >= 0 && addr)
		{
			// Public ABI: READ=0x40, WRITE=0x20, EXEC=0x10.
			result = kuKernelMemCommit(addr, static_cast<SceSize>(expandedSize), 0x70, nullptr);
			if(result >= 0)
			{
				g_block = id;
				g_expanded = true;
				g_poolBase = reinterpret_cast<uintptr_t>(addr);
				g_pool.Init(addr, expandedSize);
				std::snprintf(buffer, sizeof(buffer), "JIT pool: %u MB (kubridge RWX)\n", static_cast<unsigned>(expandedSize / MB));
				g_diagnostics += buffer;
				return true;
			}
		}
		if(id >= 0) sceKernelFreeMemBlock(id);
		std::snprintf(buffer, sizeof(buffer), "kubridge allocation failed: 0x%08X; fallback to VM domain\n", static_cast<unsigned>(result));
		g_diagnostics += buffer;
	}
	else if(maxSize > 16 * MB)
	{
		g_diagnostics += allowExpansion ? "kubridge API unavailable; fallback to 16 MiB VM domain\n" : "kubridge disabled by user; fallback to 16 MiB VM domain\n";
	}
	// Stock VM executable memory has a per-process quota. Do not retry
	// requests above the proven 16 MiB limit.
	minSize = std::min(minSize, 16 * MB);
	size_t poolSize = std::min(maxSize, 16 * MB);
	int lastError = 0;
	while(true)
	{
		g_block = sceKernelAllocMemBlockForVM("VitaPS2_JIT", poolSize);
		if(g_block >= 0) break;
		lastError = g_block;
		std::snprintf(buffer, sizeof(buffer), "VM allocation %u MB failed: 0x%08X\n", static_cast<unsigned>(poolSize / MB), static_cast<unsigned>(lastError));
		g_diagnostics += buffer;
		std::printf("%s", buffer);
		if(poolSize <= minSize)
		{
			std::snprintf(buffer, sizeof(buffer), "sceKernelAllocMemBlockForVM failed for %u..%u MB, last error 0x%08X\n",
			              static_cast<unsigned int>(minSize / MB), static_cast<unsigned int>(maxSize / MB), static_cast<unsigned int>(lastError));
			g_diagnostics += buffer;
			return false;
		}
		poolSize = std::max(minSize, poolSize - 4 * MB);
	}
	std::snprintf(buffer, sizeof(buffer), "JIT pool: %u MB (requested %u MB)\n", static_cast<unsigned>(poolSize / MB), static_cast<unsigned>(maxSize / MB));
	g_diagnostics += buffer;
	std::printf("%s", buffer);
	void* base = nullptr;
	if(sceKernelGetMemBlockBase(g_block, &base) < 0)
	{
		sceKernelFreeMemBlock(g_block);
		g_block = -1;
		return false;
	}
	g_poolBase = reinterpret_cast<uintptr_t>(base);
	g_pool.Init(base, poolSize);
	return true;
}

size_t VitaJit_GetUsedBytes()
{
	return g_pool.GetUsedBytes();
}

size_t VitaJit_GetCapacity()
{
	return g_pool.GetCapacity();
}

size_t VitaJit_GetLargestFree()
{
	return g_pool.GetLargestFree();
}

extern "C" void* vitaps2_jit_alloc(size_t size)
{
	SceUInt64 start = sceKernelGetProcessTimeWide();
	void* result = g_pool.Alloc(size);
	g_stats.allocUs += sceKernelGetProcessTimeWide() - start;
	g_stats.allocs++;
	if(!result)
	{
		std::printf("JIT pool exhausted (%u bytes requested, %u/%u used)\n",
		            static_cast<unsigned int>(size), static_cast<unsigned int>(g_pool.GetUsedBytes()),
		            static_cast<unsigned int>(g_pool.GetCapacity()));
		// stdout goes nowhere on the Vita.
		if(FILE* file = std::fopen(VITAPS2_DATA_DIR "/crash.txt", "w"))
		{
			std::fprintf(file, "JIT pool exhausted: %u bytes requested, %u/%u used, %u flushes\n", static_cast<unsigned int>(size),
			             static_cast<unsigned int>(g_pool.GetUsedBytes()), static_cast<unsigned int>(g_pool.GetCapacity()), g_flushes);
			std::fclose(file);
		}
		std::abort();
	}
	return result;
}

extern "C" void vitaps2_jit_free(void* ptr)
{
	g_pool.Free(ptr);
}

extern "C" void vitaps2_jit_begin_write()
{
	g_writeMutex.lock();
	if(g_writeDepth++ == 0)
	{
		g_writeStart = sceKernelGetProcessTimeWide();
		if(!g_expanded) sceKernelOpenVMDomain();
	}
}

extern "C" void vitaps2_jit_end_write(void* ptr, size_t size)
{
	if(--g_writeDepth == 0)
	{
		if(!g_expanded) sceKernelCloseVMDomain();
	}
	if(ptr && size)
	{
		const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
		const size_t capacity = g_pool.GetCapacity();
		if(address < g_poolBase || address - g_poolBase > capacity || size > capacity - (address - g_poolBase))
			std::abort();
		if(g_expanded)
		{
			// The bridge handles executable pages outside the VM domain.
			const uintptr_t low = address & ~uintptr_t(0xFFF);
			const uintptr_t high = (address + size + 0xFFF) & ~uintptr_t(0xFFF);
			kuKernelFlushCaches(reinterpret_cast<const void*>(low), static_cast<SceSize>(high - low));
		}
		else
		{
			sceKernelSyncVMDomain(g_block, ptr, size);
		}
	}
	if(g_writeDepth == 0)
	{
		g_stats.writes++;
		g_stats.writeUs += sceKernelGetProcessTimeWide() - g_writeStart;
	}
	g_writeMutex.unlock();
}
