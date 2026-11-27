#include "JitCacheSetup.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "JitCache.h"
#include "BasicBlock.h"
#include "xxhash.h"
#if defined(__arm__) && !defined(__aarch64__)
#include "Jitter_CodeGen_AArch32.h"
#endif
#if defined(__vita__)
#include <psp2/kernel/modulemgr.h>
#elif defined(__linux__)
#include <cstring>
#include <unistd.h>
#endif

namespace
{
	std::string g_openDescription = "cache not opened";
	std::vector<CJitCache::RANGE> GetImageRanges()
	{
		std::vector<CJitCache::RANGE> ranges;
#if defined(__vita__)
		// The module holding this function: its segments (code, data).
		SceUID modules[64];
		SceSize count = 64;
		if(sceKernelGetModuleList(0xFF, modules, &count) < 0) return ranges;
		auto self = reinterpret_cast<uintptr_t>(&GetImageRanges);
		for(SceSize i = 0; i < count; i++)
		{
			SceKernelModuleInfo info = {};
			info.size = sizeof(info);
			if(sceKernelGetModuleInfo(modules[i], &info) < 0) continue;
			bool mine = false;
			for(const auto& segment : info.segments)
			{
				auto start = reinterpret_cast<uintptr_t>(segment.vaddr);
				if((self >= start) && (self < start + segment.memsz)) mine = true;
			}
			if(!mine) continue;
			for(const auto& segment : info.segments)
			{
				if(segment.vaddr && segment.memsz) ranges.push_back({reinterpret_cast<uintptr_t>(segment.vaddr), segment.memsz});
			}
			break;
		}
#elif defined(__linux__)
		// The main program's mappings, from /proc/self/maps (file-backed, then the
		// anonymous one right after them: .bss).
		char exe[512] = {};
		ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		if(length <= 0) return ranges;
		std::FILE* maps = std::fopen("/proc/self/maps", "r");
		if(!maps) return ranges;
		char line[1024];
		bool previousMine = false;
		while(std::fgets(line, sizeof(line), maps))
		{
			unsigned long start = 0, end = 0;
			char path[600] = {};
			if(std::sscanf(line, "%lx-%lx %*s %*s %*s %*s %599[^\n]", &start, &end, path) < 2) continue;
			bool mine = (std::strcmp(path, exe) == 0);
			if(mine || (previousMine && (path[0] == 0)))
			{
				if(!ranges.empty() && (ranges.back().start + ranges.back().size == start))
					ranges.back().size += end - start;
				else
					ranges.push_back({static_cast<uintptr_t>(start), static_cast<size_t>(end - start)});
			}
			previousMine = mine;
		}
		std::fclose(maps);
#endif
		return ranges;
	}

	uint64 Fingerprint(const std::string& executablePath)
	{
		XXH3_state_t* state = XXH3_createState();
		if(!state) return 0;
		XXH3_64bits_reset(state);
		if(std::FILE* file = std::fopen(executablePath.c_str(), "rb"))
		{
			std::vector<uint8> buffer(256 * 1024);
			size_t read = 0;
			while((read = std::fread(buffer.data(), 1, buffer.size(), file)) != 0) XXH3_64bits_update(state, buffer.data(), read);
			bool ok = !std::ferror(file);
			std::fclose(file);
			if(!ok) { XXH3_freeState(state); return 0; }
		}
		else { XXH3_freeState(state); return 0; }
#if defined(__arm__) && !defined(__aarch64__)
		// Modes that change the generated code without changing the executable.
		uint8 modes[7] = {Jitter::CCodeGen_AArch32::GetConservativeAllocation(), Jitter::CCodeGen_AArch32::GetChainedFrames(),
		                  Jitter::CCodeGen_AArch32::GetGuestMemoryOps(), CBasicBlock::GetCheapExits(),
		                  Jitter::CCodeGen_AArch32::GetPinnedFields(), CBasicBlock::GetFastRam(),
		                  CBasicBlock::GetMemoryExceptionPoison()};
		XXH3_64bits_update(state, modes, sizeof(modes));
#endif
		uint64 hash = XXH3_64bits_digest(state);
		XXH3_freeState(state);
		return hash;
	}
}

bool JitCacheSetup::Open(const std::string& cachePath, const std::string& executablePath, const std::string& configuration, bool isolate)
{
	g_openDescription = "cache unsupported";
	if(!CJitCache::IsSupported()) return false;
	auto ranges = GetImageRanges();
	g_openDescription = "no image ranges";
	if(ranges.empty()) return false;
	auto fingerprint = Fingerprint(executablePath);
	g_openDescription = "executable fingerprint failed";
	if(!fingerprint) return false;
	if(!configuration.empty()) fingerprint = XXH3_64bits_withSeed(configuration.data(), configuration.size(), fingerprint);
	char id[32];
	std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(fingerprint));
	const std::string actualPath = isolate ? cachePath + "-" + id + ".jit" : cachePath;
	g_openDescription = "cache_path=" + actualPath + "\nfingerprint=" + id + "\nconfig=" + configuration;
	long length = -1;
	if(FILE* f = std::fopen(actualPath.c_str(), "rb"))
	{
		char magic[8] = {};
		uint64 stored = 0;
		uint32 count = 0;
		const char expected[8] = {'V','S','2','J','I','T','C',1};
		bool valid = std::fread(magic,1,8,f)==8 && std::fread(&stored,1,8,f)==8 &&
		             std::fread(&count,1,4,f)==4 && std::memcmp(magic, expected, 8)==0 &&
		             stored==fingerprint && count==ranges.size();
		for(size_t i=0; valid && i<ranges.size(); ++i)
		{
			uint32 bytes=0;
			valid = std::fread(&bytes,1,4,f)==4 && bytes==ranges[i].size;
		}
		if(std::fseek(f,0,SEEK_END)==0) length=std::ftell(f);
		std::fclose(f);
		g_openDescription += "\nheader_valid=" + std::to_string(valid);
		// Never truncate a suspect isolated cache, even if the core would
		// normally replace it. Keep it for inspection and run without disk cache.
		if(isolate && !valid) { g_openDescription += "\nrejected_existing_cache=1"; return false; }
	}
	g_openDescription += "\nexisting_bytes=" + std::to_string(length);
	const bool ok = CJitCache::GetInstance().Open(actualPath, fingerprint, ranges);
	g_openDescription += "\nopen_ok=" + std::to_string(ok);
	return ok;
}

std::string JitCacheSetup::DescribeOpen()
{
	return g_openDescription;
}

void JitCacheSetup::Close()
{
	CJitCache::GetInstance().Close();
}

std::string JitCacheSetup::Describe()
{
	auto stats = CJitCache::GetInstance().GetStats();
	char text[160];
	std::snprintf(text, sizeof(text), "jit cache: %u loaded, %u hits, %u stored, %u uncacheable, %u failures", stats.loaded, stats.hits,
	              stats.stored, stats.uncacheable, stats.failures);
	return text;
}
