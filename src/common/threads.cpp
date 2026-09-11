#include "common/threads.h"

#include "common/assert.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono> // IWYU pragma: keep
#include <cinttypes>
#include <condition_variable> // IWYU pragma: keep
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_COMPILER == KYTY_COMPILER_CLANG
#define KYTY_WIN_CS
#endif

// macOS has no clock_nanosleep.
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS && !defined(__APPLE__)
#define KYTY_POSIX_HIGH_RES_SLEEP
#include <ctime>
#endif

#include <sstream>
#include <string>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <processthreadsapi.h>
#endif

#ifdef KYTY_WIN_CS
constexpr DWORD    KYTY_CS_SPIN_COUNT          = 4000;
constexpr uint64_t KYTY_SLEEP_SPIN_LIMIT_100NS = 500; // 50 us

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static void SleepHighResolution100ns(uint64_t units_100ns) {
	if (units_100ns == 0) {
		return;
	}

	// Keep spinning only where a kernel transition is
	// likely to cost more than the requested delay; ordinary millisecond sleeps use the
	// per-thread high-resolution waitable timer below.
	if (units_100ns <= KYTY_SLEEP_SPIN_LIMIT_100NS) {
		LARGE_INTEGER frequency {};
		LARGE_INTEGER start {};
		if (QueryPerformanceFrequency(&frequency) != 0 && QueryPerformanceCounter(&start) != 0 &&
		    frequency.QuadPart > 0) {
			const auto wait_ticks =
			    static_cast<LONGLONG>((static_cast<long double>(units_100ns) *
			                           static_cast<long double>(frequency.QuadPart)) /
			                          10000000.0L);
			const auto    deadline = start.QuadPart + std::max<LONGLONG>(wait_ticks, 1);
			LARGE_INTEGER now {};
			do {
				if (QueryPerformanceCounter(&now) == 0) {
					break;
				}
				YieldProcessor();
			} while (now.QuadPart < deadline);
			return;
		}
	}

	thread_local HANDLE timer = CreateWaitableTimerExW(
	    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer == nullptr) {
		thread_local HANDLE fallback_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
		timer                              = fallback_timer;
	}

	if (timer != nullptr) {
		LARGE_INTEGER due_time {};
		due_time.QuadPart = -static_cast<LONGLONG>(units_100ns);
		if (SetWaitableTimerEx(timer, &due_time, 0, nullptr, nullptr, nullptr, 0) != 0) {
			WaitForSingleObject(timer, INFINITE);
			return;
		}
	}

	std::this_thread::sleep_for(std::chrono::nanoseconds(units_100ns * 100));
}

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <synchapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <__mutex_base>
// IWYU pragma: no_include <__threading_support>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <winerror.h>

using InitializeConditionVariable_func_t = /*WINBASEAPI*/ VOID WINAPI (*)(PCONDITION_VARIABLE);
using WakeConditionVariable_func_t    = /*WINBASEAPI*/ VOID       WINAPI (*)(PCONDITION_VARIABLE);
using WakeAllConditionVariable_func_t = /*WINBASEAPI*/ VOID    WINAPI (*)(PCONDITION_VARIABLE);
using SleepConditionVariableCS_func_t = /*WINBASEAPI*/ BOOL    WINAPI (*)(PCONDITION_VARIABLE,
                                                                          PCRITICAL_SECTION, DWORD);

static InitializeConditionVariable_func_t ResolveInitializeConditionVariable() {
	if (HMODULE h = GetModuleHandle("KernelBase"); h != nullptr) // @suppress("Invalid arguments")
	{
		return reinterpret_cast<InitializeConditionVariable_func_t>(
		    GetProcAddress(h, "InitializeConditionVariable"));
	}
	return nullptr;
}
static WakeConditionVariable_func_t ResolveWakeConditionVariable() {
	if (HMODULE h = GetModuleHandle("KernelBase"); h != nullptr) // @suppress("Invalid arguments")
	{
		return reinterpret_cast<WakeConditionVariable_func_t>(
		    GetProcAddress(h, "WakeConditionVariable"));
	}
	return nullptr;
}
static WakeAllConditionVariable_func_t ResolveWakeAllConditionVariable() {
	if (HMODULE h = GetModuleHandle("KernelBase"); h != nullptr) // @suppress("Invalid arguments")
	{
		return reinterpret_cast<WakeAllConditionVariable_func_t>(
		    GetProcAddress(h, "WakeAllConditionVariable"));
	}
	return nullptr;
}
static SleepConditionVariableCS_func_t ResolveSleepConditionVariableCS() {
	if (HMODULE h = GetModuleHandle("KernelBase"); h != nullptr) // @suppress("Invalid arguments")
	{
		return reinterpret_cast<SleepConditionVariableCS_func_t>(
		    GetProcAddress(h, "SleepConditionVariableCS"));
	}
	return nullptr;
}

#endif

#ifdef KYTY_POSIX_HIGH_RES_SLEEP
// Spin for very short waits; use an absolute deadline for longer waits.
static void SleepHighResolutionNanos(uint64_t nanos) {
	if (nanos == 0) {
		return;
	}

	constexpr uint64_t NANOS_PER_SEC = 1000000000;
	constexpr uint64_t SPIN_LIMIT_NS = 50000; // below this a context switch dominates

	timespec deadline {};
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
		return;
	}

	auto target_nsec = static_cast<uint64_t>(deadline.tv_nsec) + nanos;
	deadline.tv_sec += static_cast<time_t>(target_nsec / NANOS_PER_SEC);
	deadline.tv_nsec = static_cast<long>(target_nsec % NANOS_PER_SEC);

	if (nanos <= SPIN_LIMIT_NS) {
		timespec now {};
		do {
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				return;
			}
		} while (now.tv_sec < deadline.tv_sec ||
		         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
		return;
	}

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
	}
}
#endif

namespace Common {

using thread_id_t = std::thread::id;

struct MutexPrivate {
#ifdef KYTY_WIN_CS
	MutexPrivate() { InitializeCriticalSectionAndSpinCount(&m_cs, KYTY_CS_SPIN_COUNT); }
	~MutexPrivate() { DeleteCriticalSection(&m_cs); }
	KYTY_CLASS_NO_COPY(MutexPrivate);
	CRITICAL_SECTION m_cs {};
#else
	std::recursive_mutex m_mutex;
#endif
};

struct CondVarPrivate {
#ifdef KYTY_WIN_CS
	CondVarPrivate() {
		static auto func = ResolveInitializeConditionVariable();
		EXIT_NOT_IMPLEMENTED(func == nullptr);
		func(&m_cv);
	}
	~CondVarPrivate() = default;
	KYTY_CLASS_NO_COPY(CondVarPrivate);
	CONDITION_VARIABLE m_cv {};
#else
	std::condition_variable_any m_cv;
#endif
};

static std::recursive_mutex                         g_cond_waiters_mutex;
static std::vector<std::pair<int, CondVarPrivate*>> g_cond_waiters;
static wait_poll_func_t                             g_cond_wait_poll_callback = nullptr;

static void WakeCondVar(CondVarPrivate* cond_var) {
#ifdef KYTY_WIN_CS
	static auto func = ResolveWakeAllConditionVariable();
	EXIT_NOT_IMPLEMENTED(func == nullptr);
	func(&cond_var->m_cv);
#else
	cond_var->m_cv.notify_all();
#endif
}

static void RegisterCondWaiter(CondVarPrivate* cond_var) {
	std::lock_guard lock(g_cond_waiters_mutex);
	g_cond_waiters.emplace_back(Thread::GetThreadIdUnique(), cond_var);
}

static void UnregisterCondWaiter(CondVarPrivate* cond_var) {
	const auto      thread_id = Thread::GetThreadIdUnique();
	std::lock_guard lock(g_cond_waiters_mutex);

	const auto it = std::find_if(g_cond_waiters.begin(), g_cond_waiters.end(),
	                             [thread_id, cond_var](const auto& waiter) {
		                             return waiter.first == thread_id && waiter.second == cond_var;
	                             });
	if (it != g_cond_waiters.end()) {
		g_cond_waiters.erase(it);
	}
}

struct ThreadPrivate {
	ThreadPrivate(thread_func_t f, void* a): func(f), arg(a), m_thread(&Run, this) {}

	static void Run(ThreadPrivate* t) {
		t->unique_id = Thread::GetThreadIdUnique();
		t->started   = true;
		t->func(t->arg);
	}

	thread_func_t    func;
	void*            arg;
	std::atomic_bool finished    = false;
	std::atomic_bool auto_delete = false;
	std::atomic_bool started     = false;
	int              unique_id   = 0;
	std::thread      m_thread;
};

static thread_id_t      g_main_thread;
static int              g_main_thread_int;
static std::atomic<int> g_thread_counter = 0;

void InitializeThreads() {
	g_main_thread     = std::this_thread::get_id();
	g_main_thread_int = Thread::GetThreadIdUnique();
}

Thread::Thread(thread_func_t func, void* arg)
    : m_thread(std::make_unique<ThreadPrivate>(func, arg)) {
	while (!m_thread->started) {
		Common::Thread::SleepMicro(1000);
	}
}

Thread::~Thread() {
	EXIT_IF(!m_thread->finished && !m_thread->auto_delete);

	m_thread.reset();
}

void Thread::Join() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->m_thread.join();

	m_thread->finished = true;
}

void Thread::Detach() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->auto_delete = true;
	m_thread->m_thread.detach();
}

void Thread::Sleep(uint32_t millis) {
	std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

void Thread::SleepMicro(uint32_t micros) {
#ifdef KYTY_WIN_CS
	SleepHighResolution100ns(static_cast<uint64_t>(micros) * 10);
#elif defined(KYTY_POSIX_HIGH_RES_SLEEP)
	SleepHighResolutionNanos(static_cast<uint64_t>(micros) * 1000);
#else
	std::this_thread::sleep_for(std::chrono::microseconds(micros));
#endif
}

void Thread::SleepNano(uint64_t nanos) {
#ifdef KYTY_WIN_CS
	SleepHighResolution100ns((nanos + 99) / 100);
#elif defined(KYTY_POSIX_HIGH_RES_SLEEP)
	SleepHighResolutionNanos(nanos);
#else
	std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
#endif
}

bool Thread::IsMainThread() {
	return g_main_thread == std::this_thread::get_id();
}

std::string Thread::GetId() const {
	std::stringstream ss;
	ss << m_thread->m_thread.get_id();
	return ss.str();
}

int Thread::GetUniqueId() const {
	return m_thread->unique_id;
}

std::string Thread::GetThreadId() {
	std::stringstream ss;
	ss << std::this_thread::get_id();
	return ss.str();
}

Mutex::Mutex(): m_mutex(std::make_unique<MutexPrivate>()) {}

Mutex::~Mutex() {
	m_mutex.reset();
}

void Mutex::Lock() {
#ifdef KYTY_WIN_CS
	EnterCriticalSection(&m_mutex->m_cs);
#else
	m_mutex->m_mutex.lock();
#endif
}

void Mutex::Unlock() {
#ifdef KYTY_WIN_CS
	LeaveCriticalSection(&m_mutex->m_cs);
#else
	m_mutex->m_mutex.unlock();
#endif
}

bool Mutex::TryLock() {
#ifdef KYTY_WIN_CS
	return (TryEnterCriticalSection(&m_mutex->m_cs) != 0);
#else
	return m_mutex->m_mutex.try_lock();
#endif
}

CondVar::CondVar(): m_cond_var(std::make_unique<CondVarPrivate>()) {}

CondVar::~CondVar() {
	m_cond_var.reset();
}

void CondVar::Wait(Mutex* mutex) {
	RegisterCondWaiter(m_cond_var.get());
#ifndef KYTY_WIN_CS
	std::unique_lock<std::recursive_mutex> cpp_lock(mutex->m_mutex->m_mutex, std::adopt_lock_t());
#endif
	auto poll_callback = [&] {
		auto* callback = g_cond_wait_poll_callback;
		if (callback == nullptr) {
			return;
		}
#if defined(KYTY_WIN_CS)
		LeaveCriticalSection(&mutex->m_mutex->m_cs);
		callback();
		EnterCriticalSection(&mutex->m_mutex->m_cs);
#else
		cpp_lock.unlock();
		callback();
		cpp_lock.lock();
#endif
	};
#ifdef KYTY_WIN_CS
	static auto func = ResolveSleepConditionVariableCS();
	EXIT_NOT_IMPLEMENTED(func == nullptr);
	if (g_cond_wait_poll_callback == nullptr) {
		func(&m_cond_var->m_cv, &mutex->m_mutex->m_cs, INFINITE);
	} else {
		if (func(&m_cond_var->m_cv, &mutex->m_mutex->m_cs, 10) == 0 &&
		    GetLastError() == ERROR_TIMEOUT) {
			poll_callback();
		}
	}
#else
	if (g_cond_wait_poll_callback == nullptr) {
		m_cond_var->m_cv.wait(cpp_lock);
	} else {
		if (m_cond_var->m_cv.wait_for(cpp_lock, std::chrono::microseconds(10000)) ==
		    std::cv_status::timeout) {
			poll_callback();
		}
	}
	cpp_lock.release();
#endif
	UnregisterCondWaiter(m_cond_var.get());
}

void CondVar::SetWaitPollCallback(wait_poll_func_t callback) {
	g_cond_wait_poll_callback = callback;
}

bool CondVar::WaitFor(Mutex* mutex, uint32_t micros) {
	bool ok = false;
	RegisterCondWaiter(m_cond_var.get());
#ifndef KYTY_WIN_CS
	std::unique_lock<std::recursive_mutex> cpp_lock(mutex->m_mutex->m_mutex, std::adopt_lock_t());
#endif
#ifdef KYTY_WIN_CS
	static auto func = ResolveSleepConditionVariableCS();
	EXIT_NOT_IMPLEMENTED(func == nullptr);
	ok = !(func(&m_cond_var->m_cv, &mutex->m_mutex->m_cs, (micros < 1000 ? 1 : micros / 1000)) ==
	           0 &&
	       GetLastError() == ERROR_TIMEOUT);
#else
	ok = (m_cond_var->m_cv.wait_for(cpp_lock, std::chrono::microseconds(micros)) ==
	      std::cv_status::no_timeout);
	cpp_lock.release();
#endif
	UnregisterCondWaiter(m_cond_var.get());
	return ok;
}

void CondVar::Signal() {
#ifdef KYTY_WIN_CS
	static auto func = ResolveWakeConditionVariable();
	EXIT_NOT_IMPLEMENTED(func == nullptr);
	func(&m_cond_var->m_cv);
#else
	m_cond_var->m_cv.notify_one();
#endif
}

void CondVar::SignalAll() {
	WakeCondVar(m_cond_var.get());
}

void CondVar::SignalThread(int thread_id) {
	std::lock_guard lock(g_cond_waiters_mutex);
	for (const auto& waiter: g_cond_waiters) {
		if (waiter.first == thread_id) {
			WakeCondVar(waiter.second);
		}
	}
}

int Thread::GetThreadIdUnique() {
	static thread_local int tid = ++g_thread_counter;
	return tid;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// A hexadecimal CPU mask, with or without the 0x prefix. Zero means "leave the thread alone", so an
// unset, empty or unparseable value and an explicit mask of 0 all read the same way.
static uint64_t ParseAffinityMask(const char* text) {
	if (text == nullptr) {
		return 0;
	}
	while (*text == ' ' || *text == '\t') {
		text++;
	}
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text += 2;
	}
	if (*text == '\0') {
		return 0;
	}
	char*      end   = nullptr;
	const auto value = std::strtoull(text, &end, 16);
	if (end == text) {
		return 0;
	}
	return static_cast<uint64_t>(value);
}

// The variables are a startup choice, so each one is read from the environment exactly once no
// matter how many threads ask for it. At most three entries ever land here.
static uint64_t AffinityMaskFor(const char* variable) {
	static std::recursive_mutex                          mutex;
	static std::vector<std::pair<std::string, uint64_t>> cache;

	std::lock_guard lock(mutex);
	for (const auto& entry: cache) {
		if (entry.first == variable) {
			return entry.second;
		}
	}
	// The UCRT marks getenv deprecated; the emulator target silences that globally, this one
	// does not.
#if KYTY_COMPILER == KYTY_COMPILER_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
	const auto mask = ParseAffinityMask(std::getenv(variable));
#if KYTY_COMPILER == KYTY_COMPILER_CLANG
#pragma clang diagnostic pop
#endif
	cache.emplace_back(variable, mask);
	return mask;
}

// One line per thread, at startup, and the guest printf direction is silent by default. Write it
// the way the pipeline cache writes its own startup lines: straight to stdout when the log is not
// already going there, and through the log either way.
static void WriteAffinityLine(const std::string& message) {
	if (Log::GetDirection() != Log::Direction::Console) {
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fflush(stdout);
	}
	Log::Write(message);
	Log::Flush();
}

static const char* AffinityVariable(ThreadAffinityGroup group) {
	switch (group) {
		case ThreadAffinityGroup::Render: return "KYTY_GPU_THREAD_AFFINITY";
		case ThreadAffinityGroup::Present: return "KYTY_PRESENT_THREAD_AFFINITY";
		case ThreadAffinityGroup::Guest: return "KYTY_GUEST_THREAD_AFFINITY";
	}
	return "";
}

// One L3 cache: the logical processors behind it and its size in bytes.
struct L3Cache {
	uint64_t mask = 0;
	uint64_t size = 0;
};

// Written once by InitializeThreadAffinity() before any of the threads below exists, read-only
// afterwards.
static uint64_t g_derived_large_cache_mask = 0;
static uint64_t g_derived_other_mask       = 0;

static uint64_t DerivedAffinityMaskFor(ThreadAffinityGroup group) {
	return (group == ThreadAffinityGroup::Guest ? g_derived_other_mask
	                                            : g_derived_large_cache_mask);
}

// Every distinct L3 in processor group 0, in the order the API reports them. Entries sharing a
// processor mask are merged, because a host may report one cache once per cache type.
static std::vector<L3Cache> EnumerateL3Caches() {
	std::vector<L3Cache> caches;

	DWORD length = 0;
	if (GetLogicalProcessorInformationEx(RelationCache, nullptr, &length) != 0 ||
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
		return caches;
	}

	std::vector<uint8_t> buffer(length);
	if (GetLogicalProcessorInformationEx(
	        RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
	        &length) == 0) {
		return caches;
	}

	for (DWORD offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
		const auto* info =
		    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
		if (info->Size == 0 || offset + info->Size > length) {
			break;
		}
		offset += info->Size;

		if (info->Relationship != RelationCache || info->Cache.Level != 3 ||
		    info->Cache.GroupMask.Group != 0) {
			continue;
		}

		const auto mask = static_cast<uint64_t>(info->Cache.GroupMask.Mask);
		const auto size = static_cast<uint64_t>(info->Cache.CacheSize);
		if (mask == 0 || size == 0) {
			continue;
		}

		const auto it = std::find_if(caches.begin(), caches.end(),
		                             [mask](const L3Cache& entry) { return entry.mask == mask; });
		if (it != caches.end()) {
			// <windows.h> defines a max() macro here, so compare by hand.
			if (size > it->size) {
				it->size = size;
			}
		} else {
			caches.push_back(L3Cache {mask, size});
		}
	}

	return caches;
}

// The render thread's working set is scattered guest memory, so on a host whose L3 caches differ in
// size it wants the largest one to itself. That is only derivable when every logical processor sits
// in one processor group, because SetThreadAffinityMask cannot name another group.
static void DeriveAffinityMasks() {
	if (GetActiveProcessorGroupCount() > 1) {
		return;
	}

	const auto caches = EnumerateL3Caches();
	if (caches.size() < 2) {
		return;
	}

	const auto largest = std::max_element(
	    caches.begin(), caches.end(),
	    [](const L3Cache& a, const L3Cache& b) { return a.size < b.size; });
	const auto smallest = std::min_element(
	    caches.begin(), caches.end(),
	    [](const L3Cache& a, const L3Cache& b) { return a.size < b.size; });
	if (largest->size == smallest->size) {
		// Uniform host: no die is worth preferring, and pinning would only take cores away.
		return;
	}

	DWORD_PTR process_mask = 0;
	DWORD_PTR system_mask  = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == 0) {
		return;
	}

	const auto large_mask = largest->mask & static_cast<uint64_t>(process_mask);
	const auto other_mask = static_cast<uint64_t>(process_mask) & ~largest->mask;
	if (large_mask == 0 || other_mask == 0) {
		return;
	}

	g_derived_large_cache_mask = large_mask;
	g_derived_other_mask       = other_mask;

	constexpr uint64_t BYTES_PER_MIB = 1024 * 1024;
	std::string        topology;
	for (const auto& cache: caches) {
		if (!topology.empty()) {
			topology += ", ";
		}
		topology += fmt::sprintf("%" PRIu64 " MiB on 0x%016" PRIx64, cache.size / BYTES_PER_MIB,
		                         cache.mask);
	}
	WriteAffinityLine(fmt::sprintf("affinity: L3 topology: %s; derived render+present mask "
	                               "0x%016" PRIx64 ", guest mask 0x%016" PRIx64 "\n",
	                               topology.c_str(), large_mask, other_mask));
}

void InitializeThreadAffinity(bool derive) {
	if (derive) {
		DeriveAffinityMasks();
	}
}

void ApplyThreadAffinity(ThreadAffinityGroup group, const char* thread_label) {
	const auto* variable = AffinityVariable(group);
	const auto* source   = variable;
	auto        mask     = AffinityMaskFor(variable);
	if (mask == 0) {
		mask   = DerivedAffinityMaskFor(group);
		source = "derived";
	}
	if (mask == 0) {
		return;
	}
	const auto* label    = (thread_label != nullptr && *thread_label != '\0' ? thread_label : "?");
	const auto  thread   = static_cast<uint32_t>(GetCurrentThreadId());
	const auto  previous = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
	if (previous == 0) {
		WriteAffinityLine(
		    fmt::sprintf("affinity: %s = 0x%016" PRIx64 " rejected for %s (thread %u), error %u\n",
		                 source, mask, label, thread, static_cast<uint32_t>(GetLastError())));
		return;
	}
	WriteAffinityLine(fmt::sprintf("affinity: %s -> %s (thread %u): mask 0x%016" PRIx64
	                               ", was 0x%016" PRIx64 "\n",
	                               source, label, thread, mask, static_cast<uint64_t>(previous)));
}

#else

void InitializeThreadAffinity(bool /*derive*/) {}

void ApplyThreadAffinity(ThreadAffinityGroup /*group*/, const char* /*thread_label*/) {}

#endif

} // namespace Common
