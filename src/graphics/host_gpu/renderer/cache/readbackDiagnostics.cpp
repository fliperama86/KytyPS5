#include "graphics/host_gpu/renderer/cache/readbackDiagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ReadbackDiag {

namespace {

// One producer entry per 64 KiB block: the submission tick that last claimed the block for the
// GPU and the draw-and-dispatch clock at that moment.
constexpr uint64_t BlockBits = 16;

struct Producer {
	uint64_t tick     = 0;
	uint32_t progress = 0;
};

struct Record {
	uint64_t vaddr             = 0;
	uint64_t hash              = 0;
	uint64_t window_bytes      = 0;
	uint64_t copies            = 0;
	uint64_t producer_tick     = 0;
	uint64_t producer_min_tick = 0;
	uint64_t current_tick      = 0;
	uint64_t wait_ns           = 0;
	uint64_t total_ns          = 0;
	uint32_t producer_progress = 0;
	uint32_t current_progress  = 0;
	uint32_t known_blocks      = 0;
	uint32_t unknown_blocks    = 0;
	uint32_t loop              = 0;
	uint32_t stage             = 0;
	uint8_t  kind              = 0;
	uint8_t  form              = 0;
	uint8_t  producer_complete = 0;
	uint8_t  downloaded        = 0;
	uint8_t  eligible          = 0;
	uint8_t  own_submission    = 0;
	uint32_t reason            = 0;
};

std::mutex                             g_mutex;
std::unordered_map<uint64_t, Producer> g_producers;
std::vector<Record>                    g_records;
uint32_t                               g_loop    = 0;
bool                                   g_pending = false;
Record                                 g_record;
std::chrono::steady_clock::time_point  g_fault_start;

constexpr size_t MaxRecords = 1u << 21;

const char* KindName(uint8_t kind) {
	switch (static_cast<RootKind>(kind)) {
		case RootKind::Outside: return "outside";
		case RootKind::Source: return "source";
		case RootKind::FlatRead: return "flat-read";
		case RootKind::CleanRead: return "clean-read";
		case RootKind::Condition: return "condition";
		default: return "unknown";
	}
}

const char* FormName(uint8_t form) {
	switch (static_cast<EvalForm>(form)) {
		case EvalForm::Packed: return "packed";
		case EvalForm::Flat: return "flat";
		case EvalForm::Walker: return "walker";
		default: return "none";
	}
}

// ShaderType (graphics/shader/shader.h), kept as a number here so this file does not depend on
// the recompiler headers.
const char* StageName(uint32_t stage) {
	switch (stage) {
		case 1: return "vs";
		case 2: return "ps";
		case 3: return "fetch";
		case 4: return "cs";
		case 5: return "ms";
		default: return "unknown";
	}
}

struct Bucket {
	uint64_t faults       = 0;
	uint64_t downloads    = 0;
	uint64_t bytes        = 0;
	uint64_t wait_ns      = 0;
	uint64_t total_ns     = 0;
	uint64_t complete     = 0;
	uint64_t pending      = 0;
	uint64_t unknown      = 0;
	uint64_t tick_gap     = 0;
	uint64_t progress_gap = 0;
	uint64_t same_tick    = 0;
	uint64_t eligible     = 0;
	uint64_t own          = 0;
	uint64_t eligible_ns  = 0;
};

void Accumulate(Bucket& bucket, const Record& record) {
	bucket.faults++;
	bucket.downloads += record.downloaded;
	bucket.bytes += record.window_bytes;
	bucket.wait_ns += record.wait_ns;
	bucket.total_ns += record.total_ns;
	if (record.producer_tick == 0) {
		bucket.unknown++;
	} else if (record.producer_complete != 0u) {
		bucket.complete++;
	} else {
		bucket.pending++;
	}
	if (record.producer_tick != 0 && record.current_tick >= record.producer_tick) {
		bucket.tick_gap += record.current_tick - record.producer_tick;
		if (record.current_tick == record.producer_tick) {
			bucket.same_tick++;
		}
	}
	if (record.current_progress >= record.producer_progress) {
		bucket.progress_gap += record.current_progress - record.producer_progress;
	}
	bucket.eligible += record.eligible;
	bucket.own += record.own_submission;
	if (record.eligible != 0u) {
		bucket.eligible_ns += record.total_ns;
	}
}

uint64_t Percentile(const std::vector<uint64_t>& values, double fraction) {
	if (values.empty()) {
		return 0;
	}
	const auto index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
	return values[std::min(index, values.size() - 1)];
}

} // namespace

Evaluation& Current() noexcept {
	static thread_local Evaluation evaluation;
	return evaluation;
}

void Enable(bool on) {
	std::lock_guard lock(g_mutex);
	Detail::g_enabled.store(on, std::memory_order_relaxed);
	if (on && g_records.capacity() == 0) {
		g_records.reserve(1u << 16);
	}
}

void NoteGpuWrite(uint64_t address, uint64_t size, uint64_t tick, uint32_t progress) {
	if (size == 0) {
		return;
	}
	std::lock_guard lock(g_mutex);
	const auto      last = (address + size - 1) >> BlockBits;
	for (auto block = address >> BlockBits; block <= last; block++) {
		g_producers[block] = Producer {tick, progress};
	}
}

void BeginFault(uint64_t vaddr) {
	const auto&     evaluation = Current();
	std::lock_guard lock(g_mutex);
	if (g_pending) {
		// A nested fault cannot happen on the render thread; if it ever does, keep the outer one.
		return;
	}
	g_pending      = true;
	g_record       = Record {};
	g_record.vaddr = vaddr;
	g_record.loop  = g_loop;
	g_record.hash  = evaluation.hash;
	g_record.stage = evaluation.stage;
	g_record.form  = static_cast<uint8_t>(evaluation.form);
	g_record.kind =
	    static_cast<uint8_t>(evaluation.active ? evaluation.kind : RootKind::Outside);
	g_fault_start = std::chrono::steady_clock::now();
}

void NoteDownloadRange(uint64_t address, uint64_t size) {
	if (size == 0) {
		return;
	}
	std::lock_guard lock(g_mutex);
	if (!g_pending) {
		return;
	}
	const auto last = (address + size - 1) >> BlockBits;
	for (auto block = address >> BlockBits; block <= last; block++) {
		const auto found = g_producers.find(block);
		if (found == g_producers.end()) {
			g_record.unknown_blocks++;
			continue;
		}
		g_record.known_blocks++;
		if (found->second.tick > g_record.producer_tick) {
			g_record.producer_tick     = found->second.tick;
			g_record.producer_progress = found->second.progress;
		}
		if (g_record.producer_min_tick == 0 || found->second.tick < g_record.producer_min_tick) {
			g_record.producer_min_tick = found->second.tick;
		}
	}
}

uint64_t PendingProducerTick() {
	std::lock_guard lock(g_mutex);
	return g_pending ? g_record.producer_tick : 0;
}

void NoteDownloadTotals(uint64_t window_bytes, uint64_t copies, uint64_t current_tick,
                        uint32_t current_progress, bool producer_complete) {
	std::lock_guard lock(g_mutex);
	if (!g_pending) {
		return;
	}
	g_record.window_bytes      = window_bytes;
	g_record.copies            = copies;
	g_record.current_tick      = current_tick;
	g_record.current_progress  = current_progress;
	g_record.producer_complete = producer_complete ? 1u : 0u;
	g_record.downloaded        = 1u;
}

void NoteWait(uint64_t wait_ns) {
	std::lock_guard lock(g_mutex);
	if (g_pending) {
		g_record.wait_ns += wait_ns;
	}
}

void NoteEligible(bool eligible, uint32_t reason) {
	std::lock_guard lock(g_mutex);
	if (g_pending) {
		g_record.eligible = eligible ? 1u : 0u;
		g_record.reason   = reason;
	}
}

void NoteOwnSubmission(bool own) {
	std::lock_guard lock(g_mutex);
	if (g_pending && own) {
		g_record.own_submission = 1u;
	}
}

void EndFault() {
	std::lock_guard lock(g_mutex);
	if (!g_pending) {
		return;
	}
	g_pending         = false;
	g_record.total_ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
	                                                         g_fault_start)
	        .count());
	if (g_records.size() < MaxRecords) {
		g_records.push_back(g_record);
	}
}

void SetLoop(uint32_t loop) {
	std::lock_guard lock(g_mutex);
	g_loop = loop;
}

uint64_t FaultCount() {
	std::lock_guard lock(g_mutex);
	return g_records.size();
}

void Dump(const std::filesystem::path& path) {
	std::vector<Record> records;
	{
		std::lock_guard lock(g_mutex);
		records = g_records;
	}
	if (records.empty()) {
		return;
	}

	// Per loop, with the first loop that faulted excluded the way every other replay number is.
	uint32_t first_loop = records.front().loop;
	uint32_t last_loop  = records.front().loop;
	for (const auto& record: records) {
		first_loop = std::min(first_loop, record.loop);
		last_loop  = std::max(last_loop, record.loop);
	}
	const uint32_t skip_loop = first_loop;
	uint64_t       loops     = last_loop > skip_loop ? last_loop - skip_loop : 1;

	Bucket                                          all;
	std::map<std::string, Bucket>                   by_kind;
	std::map<std::pair<uint64_t, uint32_t>, Bucket> by_shader;
	std::map<uint64_t, uint64_t>                    pages;
	std::map<uint64_t, uint64_t>                    producer_ticks;
	std::vector<uint64_t>                           tick_gaps;
	std::vector<uint64_t>                           progress_gaps;

	for (const auto& record: records) {
		if (record.loop == skip_loop) {
			continue;
		}
		Accumulate(all, record);
		Accumulate(by_kind[KindName(record.kind)], record);
		Accumulate(by_shader[{record.hash, record.stage}], record);
		pages[record.vaddr & ~uint64_t {0xfff}]++;
		if (record.producer_tick != 0) {
			producer_ticks[record.producer_tick]++;
			if (record.current_tick >= record.producer_tick) {
				tick_gaps.push_back(record.current_tick - record.producer_tick);
			}
			if (record.current_progress >= record.producer_progress) {
				progress_gaps.push_back(record.current_progress - record.producer_progress);
			}
		}
	}
	if (all.faults == 0 || loops == 0) {
		return;
	}

	std::sort(tick_gaps.begin(), tick_gaps.end());
	std::sort(progress_gaps.begin(), progress_gaps.end());

	// Why a readback could not take the own-submission path: index 0 is "the table answered but
	// the producer had not retired", the rest are FindGpuWriteTick's reasons.
	std::array<uint64_t, 5> reasons {};
	for (const auto& record: records) {
		if (record.loop == skip_loop || record.eligible != 0u) {
			continue;
		}
		reasons[std::min<size_t>(record.reason, reasons.size() - 1)]++;
	}

	FILE* file = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (::fopen_s(&file, path.string().c_str(), "w") != 0) {
		file = nullptr;
	}
#else
	file = std::fopen(path.string().c_str(), "w");
#endif
	if (file == nullptr) {
		return;
	}
	const auto per_loop = [loops](uint64_t value) {
		return static_cast<double>(value) / static_cast<double>(loops);
	};
	std::fprintf(file, "{\n");
	std::fprintf(file, "  \"loops\": %llu,\n", static_cast<unsigned long long>(loops));
	std::fprintf(file, "  \"faults\": %llu,\n", static_cast<unsigned long long>(all.faults));
	std::fprintf(file, "  \"faults_per_loop\": %.2f,\n", per_loop(all.faults));
	std::fprintf(file, "  \"downloads_per_loop\": %.2f,\n", per_loop(all.downloads));
	std::fprintf(file, "  \"read_ms_per_loop\": %.3f,\n", per_loop(all.total_ns) / 1.0e6);
	std::fprintf(file, "  \"wait_ms_per_loop\": %.3f,\n", per_loop(all.wait_ns) / 1.0e6);
	std::fprintf(file, "  \"bytes_per_loop\": %.0f,\n", per_loop(all.bytes));
	std::fprintf(file, "  \"producer_complete\": %llu,\n",
	             static_cast<unsigned long long>(all.complete));
	std::fprintf(file, "  \"producer_pending\": %llu,\n",
	             static_cast<unsigned long long>(all.pending));
	std::fprintf(file, "  \"producer_unknown\": %llu,\n",
	             static_cast<unsigned long long>(all.unknown));
	std::fprintf(file, "  \"producer_is_open_submission\": %llu,\n",
	             static_cast<unsigned long long>(all.same_tick));
	std::fprintf(file, "  \"eligible\": %llu,\n", static_cast<unsigned long long>(all.eligible));
	std::fprintf(file, "  \"own_submission\": %llu,\n", static_cast<unsigned long long>(all.own));
	std::fprintf(file, "  \"eligible_ms_per_loop\": %.3f,\n", per_loop(all.eligible_ns) / 1.0e6);
	std::fprintf(file, "  \"distinct_pages\": %zu,\n", pages.size());
	std::fprintf(file, "  \"distinct_producer_ticks\": %zu,\n", producer_ticks.size());
	std::fprintf(file, "  \"tick_gap\": {\"p50\": %llu, \"p90\": %llu, \"max\": %llu},\n",
	             static_cast<unsigned long long>(Percentile(tick_gaps, 0.5)),
	             static_cast<unsigned long long>(Percentile(tick_gaps, 0.9)),
	             static_cast<unsigned long long>(tick_gaps.empty() ? 0 : tick_gaps.back()));
	std::fprintf(file, "  \"progress_gap\": {\"p50\": %llu, \"p90\": %llu, \"max\": %llu},\n",
	             static_cast<unsigned long long>(Percentile(progress_gaps, 0.5)),
	             static_cast<unsigned long long>(Percentile(progress_gaps, 0.9)),
	             static_cast<unsigned long long>(progress_gaps.empty() ? 0
	                                                                   : progress_gaps.back()));

	std::fprintf(file, "  \"by_kind\": [\n");
	bool first = true;
	for (const auto& entry: by_kind) {
		const auto& bucket = entry.second;
		std::fprintf(file,
		             "%s    {\"kind\": \"%s\", \"faults_per_loop\": %.2f, \"ms_per_loop\": %.3f, "
		             "\"wait_ms_per_loop\": %.3f, \"complete\": %llu, \"pending\": %llu, "
		             "\"unknown\": %llu}",
		             first ? "" : ",\n", entry.first.c_str(), per_loop(bucket.faults),
		             per_loop(bucket.total_ns) / 1.0e6, per_loop(bucket.wait_ns) / 1.0e6,
		             static_cast<unsigned long long>(bucket.complete),
		             static_cast<unsigned long long>(bucket.pending),
		             static_cast<unsigned long long>(bucket.unknown));
		first = false;
	}
	std::fprintf(file, "\n  ],\n");

	std::vector<std::pair<std::pair<uint64_t, uint32_t>, Bucket>> shaders(by_shader.begin(),
	                                                                     by_shader.end());
	std::sort(shaders.begin(), shaders.end(), [](const auto& left, const auto& right) {
		return left.second.total_ns > right.second.total_ns;
	});
	std::fprintf(file, "  \"top_shaders\": [\n");
	first = true;
	for (size_t index = 0; index < shaders.size() && index < 20; index++) {
		const auto& key    = shaders[index].first;
		const auto& bucket = shaders[index].second;
		std::fprintf(file,
		             "%s    {\"hash\": \"0x%016llx\", \"stage\": \"%s\", \"faults_per_loop\": %.2f, "
		             "\"ms_per_loop\": %.3f, \"complete\": %llu, \"pending\": %llu}",
		             first ? "" : ",\n", static_cast<unsigned long long>(key.first),
		             StageName(key.second), per_loop(bucket.faults),
		             per_loop(bucket.total_ns) / 1.0e6,
		             static_cast<unsigned long long>(bucket.complete),
		             static_cast<unsigned long long>(bucket.pending));
		first = false;
	}
	std::fprintf(file, "\n  ],\n");

	// Every record, for the histograms: loop, shader, stage, kind, form, page, bytes, ns, wait ns,
	// producer tick, current tick, producer progress, current progress, producer complete, known
	// and unknown 64 KiB blocks, eligible for the own-submission path, took it.
	std::fprintf(file, "  \"records\": [\n");
	first = true;
	for (const auto& record: records) {
		std::fprintf(file,
		             "%s    [%u,\"0x%016llx\",\"%s\",\"%s\",\"%s\",\"0x%llx\",%llu,%llu,%llu,%llu,"
		             "%llu,%u,%u,%u,%u,%u,%u,%u]",
		             first ? "" : ",\n", record.loop,
		             static_cast<unsigned long long>(record.hash), StageName(record.stage),
		             KindName(record.kind), FormName(record.form),
		             static_cast<unsigned long long>(record.vaddr),
		             static_cast<unsigned long long>(record.window_bytes),
		             static_cast<unsigned long long>(record.total_ns),
		             static_cast<unsigned long long>(record.wait_ns),
		             static_cast<unsigned long long>(record.producer_tick),
		             static_cast<unsigned long long>(record.current_tick),
		             record.producer_progress, record.current_progress, record.producer_complete,
		             record.known_blocks, record.unknown_blocks, record.eligible,
		             record.own_submission);
		first = false;
	}
	std::fprintf(file, "\n  ]\n}\n");
	std::fclose(file);

	std::printf("  readbacks      %.1f read faults a loop, %.2f ms (%.2f ms of device wait), "
	            "%.0f KiB downloaded; producer complete %llu, pending %llu, unknown %llu; "
	            "%zu pages, tick gap p50 %llu p90 %llu\n",
	            per_loop(all.faults), per_loop(all.total_ns) / 1.0e6, per_loop(all.wait_ns) / 1.0e6,
	            per_loop(all.bytes) / 1024.0, static_cast<unsigned long long>(all.complete),
	            static_cast<unsigned long long>(all.pending),
	            static_cast<unsigned long long>(all.unknown), pages.size(),
	            static_cast<unsigned long long>(Percentile(tick_gaps, 0.5)),
	            static_cast<unsigned long long>(Percentile(tick_gaps, 0.9)));
	std::printf("                 own-submission path: %llu eligible (%.2f ms a loop), %llu taken; "
	            "refused wide %llu, no entry %llu, no block %llu, wide write newer %llu, "
	            "producer pending %llu\n",
	            static_cast<unsigned long long>(all.eligible), per_loop(all.eligible_ns) / 1.0e6,
	            static_cast<unsigned long long>(all.own),
	            static_cast<unsigned long long>(reasons[1]),
	            static_cast<unsigned long long>(reasons[2]),
	            static_cast<unsigned long long>(reasons[3]),
	            static_cast<unsigned long long>(reasons[4]),
	            static_cast<unsigned long long>(reasons[0]));
	std::printf("                 report %s\n", path.string().c_str());
	std::fflush(stdout);
}

} // namespace Libs::Graphics::ReadbackDiag
