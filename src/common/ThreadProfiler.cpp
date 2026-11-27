#include "ThreadProfiler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace
{
	struct THREAD
	{
#if defined(__vita__)
		int threadId = -1;
#else
		clockid_t clock = 0;
#endif
		uint64_t lastCpuMicros = 0;
		uint64_t lastWallMicros = 0;
	};

	std::mutex g_mutex;
	std::map<std::string, THREAD> g_threads;

	uint64_t WallMicros()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	bool CpuMicros(const THREAD& thread, uint64_t& micros, ThreadProfiler::SAMPLE* sample = nullptr)
	{
#if defined(__vita__)
		SceKernelThreadInfo info = {};
		info.size = sizeof(info);
		if(sceKernelGetThreadInfo(thread.threadId, &info) < 0) return false;
		micros = info.runClocks; //SceKernelSysClock: microseconds
		if(sample)
		{
			sample->threadId = thread.threadId;
			sample->affinityMask = sceKernelGetThreadCpuAffinityMask(thread.threadId);
			sample->status = info.status;
		}
		return true;
#else
		timespec ts;
		if(clock_gettime(thread.clock, &ts) != 0) return false;
		micros = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
		return true;
#endif
	}
}

void ThreadProfiler::RegisterCurrentThread(const char* name)
{
	THREAD thread;
#if defined(__vita__)
	thread.threadId = sceKernelGetThreadId();
#else
	if(pthread_getcpuclockid(pthread_self(), &thread.clock) != 0) return;
#endif
	if(!CpuMicros(thread, thread.lastCpuMicros)) return;
	thread.lastWallMicros = WallMicros();
	std::lock_guard<std::mutex> lock(g_mutex);
	g_threads[name] = thread;
}

void ThreadProfiler::Unregister(const char* name)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_threads.erase(name);
}

std::vector<ThreadProfiler::SAMPLE> ThreadProfiler::Sample()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	uint64_t now = WallMicros();

	std::vector<SAMPLE> samples;
	for(auto it = g_threads.begin(); it != g_threads.end();)
	{
		uint64_t cpu = 0;
		SAMPLE sample;
		if(!CpuMicros(it->second, cpu, &sample))
		{
			it = g_threads.erase(it); //thread is gone
			continue;
		}
		sample.name = it->first;
		const uint64_t elapsed = now > it->second.lastWallMicros ? now - it->second.lastWallMicros : 0;
		if(elapsed && cpu >= it->second.lastCpuMicros)
			sample.cpuPercent = std::min(100.0f, 100.0f * static_cast<float>(cpu - it->second.lastCpuMicros) / static_cast<float>(elapsed));
		it->second.lastCpuMicros = cpu;
		it->second.lastWallMicros = now;
		samples.push_back(sample);
		++it;
	}
	return samples;
}
