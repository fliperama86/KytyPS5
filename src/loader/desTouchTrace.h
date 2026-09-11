#pragma once

// Temporary, opt-in investigation of PPSA01342 01.005.000. Only the loaded
// executable is instrumented; the on-disk game and its saves are never patched.
#include "common/assert.h"
#include "common/hostException.h"
#include "common/threads.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Loader::DesTouchTrace {
namespace {

uint64_t base = 0;
bool serialize = false;
bool full_capture = true;
std::recursive_mutex touch_mutex;
constexpr uint64_t begin_rva = 0xd8c0f7;
constexpr uint64_t visit_rva = 0xd8c1e4;
constexpr uint64_t insert_rva = 0xd8b697;
constexpr uint64_t remove_rva = 0xd8b7ce;

struct Node {
	uint64_t address = 0;
	uint64_t data[12] {};
	bool readable = false;
};

struct Trace {
	uint64_t sequence = 0;
	uint64_t sentinel = 0;
	uint64_t snapshot_end = 0;
	size_t initial_count = 0;
	size_t visit_count = 0;
	std::array<Node, 1024> initial;
	std::array<Node, 2048> visited;
};

thread_local std::unique_ptr<Trace> trace;

struct Mutation {
	uint64_t serial = 0;
	uint64_t counter = 0;
	uint64_t rva = 0;
	uint64_t list = 0;
	uint64_t node = 0;
	int thread = 0;
	size_t depth = 0;
	uint64_t callers[16] {};
};

std::mutex mutations_mutex;
std::array<Mutation, 4096> mutations;
uint64_t mutation_count = 0;

inline bool Read(uint64_t address, void* data, size_t size) {
	SIZE_T copied = 0;
	return address >= 0x10000 &&
	       ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
	                         data, size, &copied) && copied == size;
}

inline void Capture(Node& node, uint64_t address) {
	node = {};
	node.address = address;
	node.readable = Read(address, node.data, sizeof(node.data));
}

inline void CaptureMutation(const Common::HostException::ExceptionInfo& info, uint64_t rva) {
	// All failing objects so far occupy the game's extended address region. Keep
	// history bounded and avoid logging static level objects on every operation.
	if (info.r14 < 0x1000000000) {
		return;
	}
	Mutation event {};
	event.rva = rva;
	event.list = info.rbx;
	event.node = info.r14;
	event.thread = Common::Thread::GetThreadIdUnique();
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	event.counter = static_cast<uint64_t>(counter.QuadPart);
	uint64_t frame = info.rbp;
	while (event.depth < std::size(event.callers) && frame >= info.rsp &&
	       frame - info.rsp < 2 * 1024 * 1024) {
		uint64_t data[2] {};
		if (!Read(frame, data, sizeof(data))) {
			break;
		}
		event.callers[event.depth++] = data[1];
		if (data[0] <= frame) {
			break;
		}
		frame = data[0];
	}
	std::lock_guard lock(mutations_mutex);
	event.serial = ++mutation_count;
	mutations[(event.serial - 1) % mutations.size()] = event;
}

inline void Install(const Program* program) {
	const char* enabled = std::getenv("KYTY_DEBUG_DES_TOUCH_TRACE");
	if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
		return;
	}
	const char* serialization = std::getenv("KYTY_DEBUG_DES_TOUCH_SERIALIZE");
	serialize = serialization != nullptr && std::strcmp(serialization, "1") == 0;
	const char* detailed = std::getenv("KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE");
	full_capture = !serialize || (detailed != nullptr && std::strcmp(detailed, "1") == 0);
	std::string title;
	std::string version;
	if (program == nullptr || program->file_name.filename() != "eboot.bin" ||
	    !SystemContentParamSfoGetString("TITLE_ID", &title) || title != "PPSA01342" ||
	    !SystemContentParamSfoGetString("APP_VER", &version) || version != "01.005.000" ||
	    program->mapped_size < 0xd8c42f) {
		// The probes are hard-coded RVAs for Demon's Souls PPSA01342 01.005.000. Any other
		// title leaves base == 0, so Handle() stays inert and the game runs unmodified.
		::printf("DesTouchTrace: not Demon's Souls PPSA01342 01.005.000 (title=%s version=%s);"
		         " not instrumenting\n",
		         title.empty() ? "?" : title.c_str(), version.empty() ? "?" : version.c_str());
		std::fflush(stdout);
		return;
	}
	// MOV/XCHG probes preserve flags and SIMD state. XCHG is reproduced atomically.
	struct Probe { uint64_t rva; size_t size; uint8_t expected[7]; };
	std::vector<Probe> probes = {
	    {begin_rva, 7, {0x48, 0x8b, 0x9b, 0x80, 0, 0, 0}}
	};
	if (full_capture) {
		probes.push_back({visit_rva, 4, {0x48, 0x8b, 0x43, 0x08}});
		probes.push_back({insert_rva, 7, {0x48, 0x8b, 0x8b, 0x80, 0, 0, 0}});
		probes.push_back({remove_rva, 4, {0x49, 0x8b, 0x46, 0x10}});
	}
	if (serialize) {
		// Lock before the game's own mutation spinlocks. A recursive host mutex
		// permits same-thread callbacks; other threads wait until traversal ends.
		const Probe serialization_probes[] = {
		    {0xd8b365, 3, {0x48, 0x89, 0xfb}}, // update: mov rbx,rdi
		    {0xd8b5f8, 3, {0x48, 0x89, 0xfb}}, // insert: mov rbx,rdi
		    {0xd8b759, 3, {0x48, 0x89, 0xfb}}, // remove: mov rbx,rdi
		    {0xd8b464, 3, {0x48, 0x89, 0xdf}}, // update tail-calls insert: mov rdi,rbx
		    {0xd8b5c8, 7, {0x48, 0x87, 0x83, 0x90, 0, 0, 0}},
		    {0xd8b725, 7, {0x48, 0x87, 0x83, 0x90, 0, 0, 0}},
		    {0xd8b7fb, 7, {0x48, 0x87, 0x83, 0x90, 0, 0, 0}},
		    {0xd8c428, 7, {0x48, 0x8b, 0x05, 0x71, 0x3c, 0xa1, 0x01}}
		};
		probes.insert(probes.end(), std::begin(serialization_probes), std::end(serialization_probes));
	}
	for (const auto& probe: probes) {
		if (std::memcmp(reinterpret_cast<const void*>(program->base_vaddr + probe.rva),
		                probe.expected, probe.size) != 0) {
			EXIT("DesTouchTrace: instruction signature mismatch at %" PRIx64 "\n", probe.rva);
		}
	}
	DWORD protection = 0;
	auto* page = reinterpret_cast<void*>(program->base_vaddr + 0xd8b000);
	if (!VirtualProtect(page, 0x2000, PAGE_EXECUTE_READWRITE, &protection)) {
		EXIT("DesTouchTrace: could not make probe page writable\n");
	}
	for (const auto& probe: probes) {
		auto* address = reinterpret_cast<uint8_t*>(program->base_vaddr + probe.rva);
		std::memset(address, 0x90, probe.size);
		address[0] = 0x0f;
		address[1] = 0x0b; // UD2, handled only at these installed addresses.
	}
	DWORD ignored = 0;
	if (!VirtualProtect(page, 0x2000, protection, &ignored) ||
	    !FlushInstructionCache(GetCurrentProcess(), page, 0x2000)) {
		EXIT("DesTouchTrace: could not finalize probe page\n");
	}
	base = program->base_vaddr;
	::printf("DesTouchTrace: experimental serialization=%d full_capture=%d probes=%zu\n",
	         static_cast<int>(serialize), static_cast<int>(full_capture), probes.size());
	std::fflush(stdout);
}

inline bool Handle(const Common::HostException::ExceptionInfo& info) {
	if (base == 0 || info.type != Common::HostException::ExceptionType::IllegalInstruction ||
	    info.native_context == nullptr) {
		return false;
	}
	const auto rva = info.exception_address - base;
	if (serialize) {
		auto* context = static_cast<CONTEXT*>(info.native_context);
		if (rva == 0xd8b365 || rva == 0xd8b5f8 || rva == 0xd8b759) {
			touch_mutex.lock();
			context->Rbx = info.rdi;
			context->Rip += 3;
			return true;
		}
		if (rva == 0xd8b464) {
			context->Rdi = info.rbx;
			context->Rip += 3;
			touch_mutex.unlock();
			return true;
		}
		if (rva == 0xd8b5c8 || rva == 0xd8b725 || rva == 0xd8b7fb) {
			context->Rax = static_cast<uint64_t>(InterlockedExchange64(
			    reinterpret_cast<volatile LONG64*>(info.rbx + 0x90), static_cast<LONG64>(info.rax)));
			context->Rip += 7;
			touch_mutex.unlock();
			return true;
		}
		if (rva == 0xd8c428) {
			uint64_t value = 0;
			if (!Read(base + 0x27a00a0, &value, sizeof(value))) {
				touch_mutex.unlock();
				return false;
			}
			context->Rax = value;
			context->Rip += 7;
			touch_mutex.unlock();
			return true;
		}
	}
	if (rva == insert_rva || rva == remove_rva) {
		uint64_t value = 0;
		if (!Read(rva == insert_rva ? info.rbx + 0x80 : info.r14 + 0x10,
		          &value, sizeof(value))) {
			return false;
		}
		CaptureMutation(info, rva);
		auto* context = static_cast<CONTEXT*>(info.native_context);
		if (rva == insert_rva) {
			context->Rcx = value;
			context->Rip += 7;
		} else {
			context->Rax = value;
			context->Rip += 4;
		}
		return true;
	}
	if (rva != begin_rva && rva != visit_rva) {
		return false;
	}
	if (serialize && rva == begin_rva) {
		touch_mutex.lock();
	}
	uint64_t value = 0;
	if (!Read(info.rbx + (rva == begin_rva ? 0x80 : 8), &value, sizeof(value))) {
		if (serialize && rva == begin_rva) {
			touch_mutex.unlock();
		}
		return false;
	}
	if (!full_capture && rva == begin_rva) {
		auto* context = static_cast<CONTEXT*>(info.native_context);
		context->Rbx = value;
		context->Rip += 7;
		return true;
	}
	if (!trace) {
		trace = std::make_unique<Trace>();
	}
	auto& state = *trace;
	auto* context = static_cast<CONTEXT*>(info.native_context);
	if (rva == begin_rva) {
		++state.sequence;
		state.sentinel = info.rbx + 0x68;
		state.initial_count = 0;
		state.visit_count = 0;
		uint64_t current = value;
		while (current != state.sentinel && state.initial_count < state.initial.size()) {
			auto& node = state.initial[state.initial_count++];
			Capture(node, current);
			if (!node.readable) {
				break;
			}
			current = node.data[3];
		}
		state.snapshot_end = current;
		context->Rbx = value;
		context->Rip += 7;
	} else {
		if (state.visit_count < state.visited.size()) {
			auto& node = state.visited[state.visit_count];
			Capture(node, info.rbx);
			// This is the value returned by the emulated MOV, even if a concurrent
			// writer changed the pair pointer before the wider diagnostic snapshot.
			node.data[1] = value;
		}
		++state.visit_count;
		context->Rax = value;
		context->Rip += 4;
	}
	return true;
}

inline void PrintNode(const char* phase, size_t index, const Node& node) {
	float key = 0;
	std::memcpy(&key, &node.data[0], sizeof(key));
	::printf("DesTouchTrace: %s %zu addr=%016" PRIx64 " readable=%d key=%.9g"
	         " pair=%016" PRIx64 " prev=%016" PRIx64 " next=%016" PRIx64
	         " flags=%016" PRIx64 " owner=%016" PRIx64 " active_next=%016" PRIx64
	         " active_prev=%016" PRIx64 "\n", phase, index, node.address,
	         static_cast<int>(node.readable), static_cast<double>(key), node.data[1],
	         node.data[2], node.data[3], node.data[8], node.data[9], node.data[10], node.data[11]);
}

inline void Dump(const Common::HostException::ExceptionInfo& info) {
	if (base == 0 || !trace || info.exception_address != base + 0xd8c1f5) {
		return;
	}
	const auto& state = *trace;
	std::vector<Mutation> related;
	{
		std::lock_guard lock(mutations_mutex);
		const uint64_t first = mutation_count > mutations.size() ? mutation_count - mutations.size() : 0;
		for (uint64_t i = first; i < mutation_count; ++i) {
			const auto& event = mutations[i % mutations.size()];
			if (event.node == info.rax ||
			    (event.list + 0x68 == info.r13 && i + 128 >= mutation_count)) {
				related.push_back(event);
			}
		}
	}
	for (const auto& event: related) {
		::printf("DesTouchMutation: serial=%" PRIu64 " qpc=%" PRIu64 " kind=%s thread=%d"
		         " list=%016" PRIx64 " node=%016" PRIx64 " target=%d callers=",
		         event.serial, event.counter, event.rva == insert_rva ? "insert" : "remove",
		         event.thread, event.list, event.node, static_cast<int>(event.node == info.rax));
		for (size_t i = 0; i < event.depth; ++i) {
			::printf("%s%016" PRIx64, i == 0 ? "" : ",", event.callers[i]);
		}
		::printf("\n");
	}
	::printf("DesTouchTrace: thread=%d sequence=%" PRIu64 " sentinel=%016" PRIx64
	         " snapshot_end=%016" PRIx64 " initial=%zu visited=%zu stored=%zu\n",
	         Common::Thread::GetThreadIdUnique(), state.sequence, state.sentinel,
	         state.snapshot_end, state.initial_count, state.visit_count,
	         std::min(state.visit_count, state.visited.size()));
	for (size_t i = 0; i < state.initial_count; ++i) {
		PrintNode("begin", i, state.initial[i]);
	}
	for (size_t i = 0; i < std::min(state.visit_count, state.visited.size()); ++i) {
		PrintNode("visit", i, state.visited[i]);
	}
	std::fflush(stdout);
}

} // namespace
} // namespace Loader::DesTouchTrace
#endif
