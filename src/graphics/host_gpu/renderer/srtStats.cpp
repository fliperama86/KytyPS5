#include "graphics/host_gpu/renderer/srtStats.h"

#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fmt/format.h>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::SrtStats {

namespace {

bool ReadEnabled() {
	const char* value = std::getenv("KYTY_DEBUG_SRT_STATS");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

// The file is rewritten this often so a run that never exits cleanly still leaves data.
constexpr uint64_t WriteIntervalFrames = 600;

constexpr uint64_t Mix(uint64_t seed, uint64_t value) {
	return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
}

uint64_t ShaderKey(const char* stage_name, uint64_t hash) {
	uint64_t seed = 0xcbf29ce484222325ull;
	for (const char* c = stage_name; *c != '\0'; c++) {
		seed = Mix(seed, static_cast<uint64_t>(static_cast<unsigned char>(*c)));
	}
	return Mix(seed, hash);
}

struct ShaderRecord {
	std::string stage;
	uint64_t    hash                   = 0;
	uint64_t    events                 = 0;
	uint64_t    descriptor_sources     = 0;
	uint64_t    buffer_sources         = 0;
	uint64_t    scalar_buffer_sources  = 0;
	uint64_t    image_sources          = 0;
	uint64_t    sampler_sources        = 0;
	uint64_t    indirect_image_sources = 0;
	uint64_t    push_overflow          = 0;
	uint32_t    flat_insts             = 0;
	uint32_t    flat_reads             = 0;
	bool        uses_dma               = false;

	std::unordered_set<uint64_t> buffer_layouts;
	std::unordered_set<uint64_t> vertex_layouts;
};

struct Accumulator {
	bool     active        = false;
	bool     dispatch      = false;
	uint32_t stages        = 0;
	uint32_t images        = 0;
	uint32_t samplers      = 0;
	bool     uses_dma      = false;
	bool     push_overflow = false;
};

// Per thread so a compute queue running beside the render thread cannot mix its stages into a
// graphics draw. Internal linkage keeps the TLS out of a COMDAT the linker mishandles.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local Accumulator g_accumulator;

struct Collector {
	// Recursive because a fatal EXIT() raised inside the writer re-enters through the emergency
	// shutdown hook on the same thread.
	std::recursive_mutex mutex;

	uint64_t frames     = 0;
	uint64_t draws      = 0;
	uint64_t dispatches = 0;

	uint64_t draw_stage_events     = 0;
	uint64_t dispatch_stage_events = 0;

	uint64_t descriptor_sources     = 0;
	uint64_t buffer_sources         = 0;
	uint64_t scalar_buffer_sources  = 0;
	uint64_t image_sources          = 0;
	uint64_t sampler_sources        = 0;
	uint64_t indirect_image_sources = 0;

	uint64_t buffers_only_draws       = 0;
	uint64_t buffers_only_dispatches  = 0;
	uint64_t dma_draws                = 0;
	uint64_t dma_dispatches           = 0;
	uint64_t push_overflow_draws      = 0;
	uint64_t push_overflow_dispatches = 0;
	uint64_t push_overflow_events     = 0;

	uint64_t sources_sum    = 0;
	uint32_t sources_max    = 0;
	uint64_t flat_insts_sum = 0;
	uint32_t flat_insts_max = 0;
	uint64_t flat_reads_sum = 0;
	uint32_t flat_reads_max = 0;

	std::map<uint32_t, uint64_t>               sources_histogram;
	std::unordered_map<uint64_t, ShaderRecord> shaders;

	std::unordered_set<const void*> frame_pipelines;
	std::unordered_set<uint64_t>    frame_shaders;
	// Per-frame counts are small integers, so binning them keeps the run-length cost flat
	// instead of retaining one sample per presented frame for hours.
	std::map<uint32_t, uint64_t> pipelines_per_frame;
	std::map<uint32_t, uint64_t> shaders_per_frame;

	std::string path;
	bool        write_failed = false;
	bool        writing      = false;
};

Collector& Instance() {
	static Collector collector;
	return collector;
}

std::string Timestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm           parts {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (localtime_s(&parts, &now) != 0) {
		return "00000000-000000";
	}
#else
	if (localtime_r(&now, &parts) == nullptr) {
		return "00000000-000000";
	}
#endif
	return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", parts.tm_year + 1900, parts.tm_mon + 1,
	                   parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

struct Distribution {
	uint64_t min    = 0;
	uint64_t median = 0;
	uint64_t max    = 0;
	uint64_t count  = 0;
};

Distribution Summarize(const std::map<uint32_t, uint64_t>& bins) {
	Distribution result;
	for (const auto& [value, count]: bins) {
		(void)value;
		result.count += count;
	}
	if (result.count == 0) {
		return result;
	}
	result.min = bins.begin()->first;
	result.max = bins.rbegin()->first;
	// The element a sorted sample list would hold at index count / 2.
	const uint64_t target = result.count / 2;
	uint64_t       seen   = 0;
	for (const auto& [value, count]: bins) {
		seen += count;
		if (seen > target) {
			result.median = value;
			break;
		}
	}
	return result;
}

void AppendDistribution(std::string& out, const char* name, const Distribution& value,
                        const char* suffix) {
	out +=
	    fmt::format("\t\t\"{}\": {{\"min\": {}, \"median\": {}, \"max\": {}, \"samples\": {}}}{}\n",
	                name, value.min, value.median, value.max, value.count, suffix);
}

void AppendHistogram(std::string& out, const char* name, const std::map<uint32_t, uint64_t>& bins,
                     const char* suffix) {
	out += fmt::format("\t\t\"{}\": {{", name);
	bool first = true;
	for (const auto& [bin, count]: bins) {
		out += fmt::format("{}\"{}\": {}", first ? "" : ", ", bin, count);
		first = false;
	}
	out += fmt::format("}}{}\n", suffix);
}

// Caller holds the collector mutex.
void WriteLocked(Collector& collector) {
	if (collector.write_failed || collector.writing) {
		return;
	}
	collector.writing = true;
	if (collector.path.empty()) {
		collector.path = fmt::format("_Diagnostics/srt-stats-{}.json", Timestamp());
	}

	std::vector<const ShaderRecord*> ordered;
	ordered.reserve(collector.shaders.size());
	std::map<uint32_t, uint64_t> buffer_layout_histogram;
	std::map<uint32_t, uint64_t> vertex_layout_histogram;
	for (const auto& [key, record]: collector.shaders) {
		(void)key;
		ordered.push_back(&record);
		buffer_layout_histogram[static_cast<uint32_t>(record.buffer_layouts.size())]++;
		if (!record.vertex_layouts.empty()) {
			vertex_layout_histogram[static_cast<uint32_t>(record.vertex_layouts.size())]++;
		}
	}
	std::sort(ordered.begin(), ordered.end(),
	          [](const ShaderRecord* left, const ShaderRecord* right) {
		          if (left->events != right->events) {
			          return left->events > right->events;
		          }
		          return left->hash < right->hash;
	          });

	const uint64_t stage_events = collector.draw_stage_events + collector.dispatch_stage_events;

	std::string out;
	out.reserve(4096 + collector.shaders.size() * 256);
	out += "{\n";
	out += "\t\"schema\": \"kyty-srt-stats-1\",\n";
	out += fmt::format("\t\"generated\": \"{}\",\n", Timestamp());
	out += "\t\"summary\": {\n";
	out += fmt::format("\t\t\"frames\": {},\n", collector.frames);
	out += fmt::format("\t\t\"draws\": {},\n", collector.draws);
	out += fmt::format("\t\t\"dispatches\": {},\n", collector.dispatches);
	out += fmt::format("\t\t\"stage_events\": {},\n", stage_events);
	out += fmt::format("\t\t\"draw_stage_events\": {},\n", collector.draw_stage_events);
	out += fmt::format("\t\t\"dispatch_stage_events\": {},\n", collector.dispatch_stage_events);
	out += fmt::format("\t\t\"shaders_seen\": {},\n", collector.shaders.size());
	out += fmt::format("\t\t\"descriptor_sources\": {},\n", collector.descriptor_sources);
	out += fmt::format("\t\t\"buffer_sources\": {},\n", collector.buffer_sources);
	out += fmt::format("\t\t\"scalar_buffer_sources\": {},\n", collector.scalar_buffer_sources);
	out += fmt::format("\t\t\"image_sources\": {},\n", collector.image_sources);
	out += fmt::format("\t\t\"sampler_sources\": {},\n", collector.sampler_sources);
	out += fmt::format("\t\t\"indirect_image_sources\": {},\n", collector.indirect_image_sources);
	out += fmt::format("\t\t\"buffers_only_draws\": {},\n", collector.buffers_only_draws);
	out += fmt::format("\t\t\"buffers_only_dispatches\": {},\n", collector.buffers_only_dispatches);
	out += fmt::format("\t\t\"dma_draws\": {},\n", collector.dma_draws);
	out += fmt::format("\t\t\"dma_dispatches\": {},\n", collector.dma_dispatches);
	out += fmt::format("\t\t\"push_overflow_draws\": {},\n", collector.push_overflow_draws);
	out +=
	    fmt::format("\t\t\"push_overflow_dispatches\": {},\n", collector.push_overflow_dispatches);
	out += fmt::format("\t\t\"push_overflow_events\": {},\n", collector.push_overflow_events);
	out += fmt::format("\t\t\"sources_per_event_sum\": {},\n", collector.sources_sum);
	out += fmt::format("\t\t\"sources_per_event_max\": {},\n", collector.sources_max);
	out += fmt::format("\t\t\"flat_insts_sum\": {},\n", collector.flat_insts_sum);
	out += fmt::format("\t\t\"flat_insts_max\": {},\n", collector.flat_insts_max);
	out += fmt::format("\t\t\"flat_reads_sum\": {},\n", collector.flat_reads_sum);
	out += fmt::format("\t\t\"flat_reads_max\": {},\n", collector.flat_reads_max);
	AppendHistogram(out, "sources_per_event_histogram", collector.sources_histogram, ",");
	AppendHistogram(out, "distinct_buffer_layouts_histogram", buffer_layout_histogram, ",");
	AppendHistogram(out, "distinct_vertex_layouts_histogram", vertex_layout_histogram, ",");
	AppendDistribution(out, "graphics_pipelines_per_frame",
	                   Summarize(collector.pipelines_per_frame), ",");
	AppendDistribution(out, "shader_hashes_per_frame", Summarize(collector.shaders_per_frame), "");
	out += "\t},\n";
	out += "\t\"shaders\": [\n";
	for (size_t i = 0; i < ordered.size(); i++) {
		const auto& record = *ordered[i];
		out += fmt::format(
		    "\t\t{{\"hash\": \"0x{:016x}\", \"stage\": \"{}\", \"events\": {}, "
		    "\"descriptor_sources\": {}, \"buffer_sources\": {}, \"scalar_buffer_sources\": {}, "
		    "\"image_sources\": {}, \"sampler_sources\": {}, \"indirect_image_sources\": {}, "
		    "\"uses_dma\": {}, \"push_overflow\": {}, \"distinct_buffer_layouts\": {}, "
		    "\"distinct_vertex_layouts\": {}, \"flat_insts\": {}, \"flat_reads\": {}}}{}\n",
		    record.hash, record.stage, record.events, record.descriptor_sources,
		    record.buffer_sources, record.scalar_buffer_sources, record.image_sources,
		    record.sampler_sources, record.indirect_image_sources,
		    record.uses_dma ? "true" : "false", record.push_overflow, record.buffer_layouts.size(),
		    record.vertex_layouts.size(), record.flat_insts, record.flat_reads,
		    i + 1 == ordered.size() ? "" : ",");
	}
	out += "\t]\n";
	out += "}\n";

	const std::filesystem::path path(collector.path);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file;
	if (!file.Create(path) || file.IsInvalid()) {
		collector.write_failed = true;
		LOGF_COLOR(Log::Color::BrightRed, "SrtStats: can't create %s\n", collector.path.c_str());
		return;
	}
	file.Write(out.data(), static_cast<uint32_t>(out.size()));
	file.Close();
}

} // namespace

namespace Detail {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool enabled = ReadEnabled();
} // namespace Detail

void BeginEvent(bool dispatch) {
	if (!Detail::enabled) {
		return;
	}
	g_accumulator          = {};
	g_accumulator.active   = true;
	g_accumulator.dispatch = dispatch;
}

void RecordStage(const StageEvent& event) {
	if (!Detail::enabled) {
		return;
	}
	auto& local = g_accumulator;
	if (local.active) {
		local.stages++;
		local.images += event.image_sources;
		local.samplers += event.sampler_sources;
		local.uses_dma      = local.uses_dma || event.uses_dma;
		local.push_overflow = local.push_overflow || event.push_overflow;
	}

	auto&                                 collector = Instance();
	std::lock_guard<std::recursive_mutex> lock(collector.mutex);

	if (local.dispatch) {
		collector.dispatch_stage_events++;
	} else {
		collector.draw_stage_events++;
	}
	collector.descriptor_sources += event.descriptor_sources;
	collector.buffer_sources += event.buffer_sources;
	collector.scalar_buffer_sources += event.scalar_buffer_sources;
	collector.image_sources += event.image_sources;
	collector.sampler_sources += event.sampler_sources;
	collector.indirect_image_sources += event.indirect_image_sources;
	collector.push_overflow_events += event.push_overflow ? 1u : 0u;

	collector.sources_sum += event.descriptor_sources;
	collector.sources_max = std::max(collector.sources_max, event.descriptor_sources);
	collector.sources_histogram[event.descriptor_sources]++;
	collector.flat_insts_sum += event.flat_insts;
	collector.flat_insts_max = std::max(collector.flat_insts_max, event.flat_insts);
	collector.flat_reads_sum += event.flat_reads;
	collector.flat_reads_max = std::max(collector.flat_reads_max, event.flat_reads);

	const uint64_t key    = ShaderKey(event.stage_name, event.shader_hash);
	auto&          record = collector.shaders[key];
	if (record.events == 0) {
		record.stage = event.stage_name;
		record.hash  = event.shader_hash;
	}
	record.events++;
	record.descriptor_sources += event.descriptor_sources;
	record.buffer_sources += event.buffer_sources;
	record.scalar_buffer_sources += event.scalar_buffer_sources;
	record.image_sources += event.image_sources;
	record.sampler_sources += event.sampler_sources;
	record.indirect_image_sources += event.indirect_image_sources;
	record.push_overflow += event.push_overflow ? 1u : 0u;
	record.flat_insts = std::max(record.flat_insts, event.flat_insts);
	record.flat_reads = std::max(record.flat_reads, event.flat_reads);
	record.uses_dma   = record.uses_dma || event.uses_dma;
	record.buffer_layouts.insert(event.buffer_layout_key);
	if (event.has_vertex_layout) {
		record.vertex_layouts.insert(event.vertex_layout_key);
	}

	collector.frame_shaders.insert(key);
}

void EndEvent() {
	if (!Detail::enabled) {
		return;
	}
	const auto local = g_accumulator;
	g_accumulator    = {};
	if (!local.active) {
		return;
	}

	auto&                                 collector = Instance();
	std::lock_guard<std::recursive_mutex> lock(collector.mutex);
	const bool buffers_only = local.stages != 0 && local.images == 0 && local.samplers == 0;
	if (local.dispatch) {
		collector.dispatches++;
		collector.buffers_only_dispatches += buffers_only ? 1u : 0u;
		collector.dma_dispatches += local.uses_dma ? 1u : 0u;
		collector.push_overflow_dispatches += local.push_overflow ? 1u : 0u;
	} else {
		collector.draws++;
		collector.buffers_only_draws += buffers_only ? 1u : 0u;
		collector.dma_draws += local.uses_dma ? 1u : 0u;
		collector.push_overflow_draws += local.push_overflow ? 1u : 0u;
	}
}

void RecordGraphicsPipeline(const void* pipeline) {
	if (!Detail::enabled || pipeline == nullptr) {
		return;
	}
	auto&                                 collector = Instance();
	std::lock_guard<std::recursive_mutex> lock(collector.mutex);
	collector.frame_pipelines.insert(pipeline);
}

void EndFrame() {
	if (!Detail::enabled) {
		return;
	}
	auto&                                 collector = Instance();
	std::lock_guard<std::recursive_mutex> lock(collector.mutex);
	collector.frames++;
	collector.pipelines_per_frame[static_cast<uint32_t>(collector.frame_pipelines.size())]++;
	collector.shaders_per_frame[static_cast<uint32_t>(collector.frame_shaders.size())]++;
	collector.frame_pipelines.clear();
	collector.frame_shaders.clear();
	if (collector.frames % WriteIntervalFrames == 0) {
		WriteLocked(collector);
	}
}

void WriteAtExit() {
	if (!Detail::enabled) {
		return;
	}
	auto&                                 collector = Instance();
	std::lock_guard<std::recursive_mutex> lock(collector.mutex);
	WriteLocked(collector);
}

} // namespace Libs::Graphics::SrtStats
