#include "gx_thread.h"
#include "perf_stats.h"
#include "runtime_log.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <algorithm>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

// The ring: one producer (the game thread) and one consumer. Records are
// 16-byte headers followed by a payload, 16-byte aligned, always contiguous;
// a header with a null invoke is a wrap marker that skips to the ring start.
extern "C" void GxFifoConsumeBytes(const uint8_t* data, uint32_t sizeBytes);

namespace GxThread {
namespace {

constexpr uint32_t kRingBytes = 16u << 20;
constexpr uint32_t kRingMask = kRingBytes - 1u;
constexpr uint32_t kHeaderBytes = 16;
constexpr uint32_t kFifoChunkBytes = 8192;
// An empty ring is usually a gap of microseconds between two bursts of the
// same frame, so the consumer spins that long before it pays a futex sleep;
// a drain likewise spins before blocking, since its backlog is normally short.
constexpr std::chrono::microseconds kConsumerSpinBudget{50};
constexpr std::chrono::microseconds kDrainSpinBudget{300};

struct Header {
    uint64_t invoke;
    uint32_t payloadBytes;
    uint32_t stride;
};

using Clock = std::chrono::steady_clock;

bool g_requested = false;
bool g_enabled = false;
bool g_started = false;
std::thread g_thread;
uint8_t* g_ring = nullptr;
thread_local bool t_isGxThread = false;

alignas(64) std::atomic<uint64_t> g_head{0};
alignas(64) std::atomic<uint64_t> g_tail{0};
alignas(64) std::atomic<bool> g_stop{false};
std::atomic<bool> g_consumerSleeping{false};
std::atomic<bool> g_producerWaiting{false};
std::atomic<uint64_t> g_fenceCompleted{0};
std::atomic<uint32_t> g_nativeTid{0};
std::mutex g_mutex;
std::condition_variable g_cvData;
std::condition_variable g_cvSpace;
std::condition_variable g_cvFence;
void (*g_waitCallback)() = nullptr;

// Producer-only state.
uint64_t g_localTail = 0;
uint64_t g_fenceRequested = 0;
uint8_t g_pendingFifo[kFifoChunkBytes];
uint32_t g_pendingFifoBytes = 0;

// Window statistics. Producer-side counters are plain; consumer-side ones are
// relaxed atomics read by the producer when it formats the log line.
uint64_t g_statRecords = 0;
uint64_t g_statFifoRecords = 0;
uint64_t g_statFifoBytes = 0;
uint64_t g_statBytes = 0;
uint64_t g_statDrains = 0;
uint64_t g_statDrainWaitNs = 0;
uint64_t g_statSpaceWaitNs = 0;
uint64_t g_statQueuePeakBytes = 0;
std::atomic<uint64_t> g_statBusyNs{0};
std::atomic<uint64_t> g_statFaults{0};

// Per-record-kind profile for the frame-rate log: keyed by the back function
// (or 1 for FIFO chunks, 2 for fences). Consumer-written; the producer reads
// it when it formats the log line, which is diagnostics, not synchronisation.
struct RecordProfile {
    uintptr_t key = 0;
    uint64_t ns = 0;
    uint64_t count = 0;
};
constexpr size_t kProfileSlots = 128;
constexpr uintptr_t kProfileKeyFifo = 1;
constexpr uintptr_t kProfileKeyFence = 2;
RecordProfile g_profile[kProfileSlots];

void ProfileRecord(uintptr_t key, uint64_t ns) {
    size_t slot = static_cast<size_t>(key >> 4) % kProfileSlots;
    for (size_t probe = 0; probe < kProfileSlots; ++probe) {
        RecordProfile& entry = g_profile[slot];
        if (entry.key == key || entry.key == 0) {
            entry.key = key;
            entry.ns += ns;
            ++entry.count;
            return;
        }
        slot = (slot + 1) % kProfileSlots;
    }
}

// Names a back function for the log: its symbol when the loader knows it,
// otherwise its offset in the module, for llvm-symbolizer on the build's .so.
std::string DescribeProfileKey(uintptr_t key) {
    if (key == kProfileKeyFifo) {
        return "fifo";
    }
    if (key == kProfileKeyFence) {
        return "fence";
    }
    char buffer[96];
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(key), &module) && module != nullptr) {
        std::snprintf(buffer, sizeof(buffer), "+0x%llx",
                      static_cast<unsigned long long>(key - reinterpret_cast<uintptr_t>(module)));
        return buffer;
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(key), &info) != 0) {
        if (info.dli_sname != nullptr && info.dli_saddr == reinterpret_cast<const void*>(key)) {
            return info.dli_sname;
        }
        std::snprintf(buffer, sizeof(buffer), "+0x%llx",
                      static_cast<unsigned long long>(key - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        return buffer;
    }
#endif
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(key));
    return buffer;
}

uint64_t ElapsedNs(Clock::time_point since) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

uint32_t Align16(uint32_t bytes) { return (bytes + 15u) & ~15u; }

void SetThreadName() {
#if defined(_WIN32)
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll")) {
        if (auto fn = reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel, "SetThreadDescription"))) {
            fn(::GetCurrentThread(), L"MKW GX");
        }
    }
    g_nativeTid.store(static_cast<uint32_t>(::GetCurrentThreadId()), std::memory_order_release);
#else
#if defined(__APPLE__)
    pthread_setname_np("MKW GX");
#else
    pthread_setname_np(pthread_self(), "MKW GX");
#endif
#if defined(__linux__)
    g_nativeTid.store(static_cast<uint32_t>(syscall(SYS_gettid)), std::memory_order_release);
#else
    g_nativeTid.store(1u, std::memory_order_release);
#endif
#endif
}

// Waits on `cv` until pred() holds, running the wait callback between
// bounded waits so the game thread's VI/alarm servicing never starves.
template <typename Pred>
void WaitWithCallback(std::condition_variable& cv, std::atomic<bool>& waitingFlag, Pred pred) {
    waitingFlag.store(true, std::memory_order_release);
    while (true) {
        {
            // MKW_PERF_STATS counts this as game-thread wait; the callback below is not.
            PerfStats::WaitScope wait;
            std::unique_lock<std::mutex> lock(g_mutex);
            if (cv.wait_for(lock, std::chrono::milliseconds(2), pred)) {
                break;
            }
        }
        if (g_waitCallback != nullptr) {
            g_waitCallback();
        }
    }
    waitingFlag.store(false, std::memory_order_release);
}

void NotifyConsumer() {
    if (g_consumerSleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cvData.notify_one();
    }
}

void PostRecordRaw(detail::Invoke invoke, const void* payload, uint32_t payloadBytes) {
    const uint32_t stride = Align16(kHeaderBytes + payloadBytes);
    uint32_t offset = static_cast<uint32_t>(g_localTail & kRingMask);
    const uint32_t wrapBytes = (offset + stride > kRingBytes) ? (kRingBytes - offset) : 0u;
    const uint64_t required = static_cast<uint64_t>(stride) + wrapBytes;
    const auto freeBytes = [&] { return kRingBytes - (g_localTail - g_head.load(std::memory_order_acquire)); };
    if (freeBytes() < required) {
        const auto started = Clock::now();
        WaitWithCallback(g_cvSpace, g_producerWaiting, [&] { return freeBytes() >= required; });
        g_statSpaceWaitNs += ElapsedNs(started);
    }
    if (wrapBytes != 0) {
        const Header wrap{0, 0, wrapBytes};
        std::memcpy(g_ring + offset, &wrap, sizeof(wrap));
        g_localTail += wrapBytes;
        offset = 0;
    }
    const Header header{reinterpret_cast<uint64_t>(invoke), payloadBytes, stride};
    std::memcpy(g_ring + offset, &header, sizeof(header));
    if (payloadBytes != 0) {
        std::memcpy(g_ring + offset + kHeaderBytes, payload, payloadBytes);
    }
    g_localTail += stride;
    // seq_cst pairs with the consumer's sleeping flag: one of the two sides
    // always sees the other's store, so a post never leaves the consumer asleep.
    g_tail.store(g_localTail, std::memory_order_seq_cst);
    ++g_statRecords;
    g_statBytes += stride;
    const uint64_t queued = g_localTail - g_head.load(std::memory_order_relaxed);
    if (queued > g_statQueuePeakBytes) {
        g_statQueuePeakBytes = queued;
    }
    NotifyConsumer();
}

void FifoInvoke(const uint8_t* payload, uint32_t payloadBytes) { GxFifoConsumeBytes(payload, payloadBytes); }

void FenceInvoke(const uint8_t* payload, uint32_t) {
    uint64_t sequence = 0;
    std::memcpy(&sequence, payload, sizeof(sequence));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_fenceCompleted.store(sequence, std::memory_order_release);
    }
    g_cvFence.notify_all();
}

void ConsumerLoop() {
    t_isGxThread = true;
    SetThreadName();
    uint64_t head = g_head.load(std::memory_order_relaxed);
    uint32_t spins = 0;
    Clock::time_point spinStarted{};
    while (true) {
        const uint64_t tail = g_tail.load(std::memory_order_acquire);
        if (head == tail) {
            if (g_stop.load(std::memory_order_acquire)) {
                break;
            }
            if (spins == 0) {
                spinStarted = Clock::now();
            }
            ++spins;
            if ((spins & 31u) != 0 || Clock::now() - spinStarted < kConsumerSpinBudget) {
                std::this_thread::yield();
                continue;
            }
            g_consumerSleeping.store(true, std::memory_order_seq_cst);
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cvData.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    return g_tail.load(std::memory_order_seq_cst) != head || g_stop.load(std::memory_order_acquire);
                });
            }
            g_consumerSleeping.store(false, std::memory_order_seq_cst);
            spins = 0;
            continue;
        }
        spins = 0;
        Header header;
        std::memcpy(&header, g_ring + (head & kRingMask), sizeof(header));
        if (header.invoke != 0) {
            const auto started = Clock::now();
            const uint8_t* payload = g_ring + ((head + kHeaderBytes) & kRingMask);
            uintptr_t profileKey = kProfileKeyFifo;
            if (header.invoke == reinterpret_cast<uint64_t>(&FenceInvoke)) {
                profileKey = kProfileKeyFence;
            } else if (header.invoke != reinterpret_cast<uint64_t>(&FifoInvoke)) {
                // Every call record starts with the back function's pointer.
                std::memcpy(&profileKey, payload, sizeof(profileKey));
            }
            try {
                reinterpret_cast<detail::Invoke>(header.invoke)(payload, header.payloadBytes);
            } catch (const std::exception& ex) {
                const uint64_t faults = g_statFaults.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (faults <= 64) {
                    RT_LOGF(RT_TAG_GX, "GX thread: command raised '%s' (n=%llu)\n", ex.what(),
                            static_cast<unsigned long long>(faults));
                }
            } catch (...) {
                const uint64_t faults = g_statFaults.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (faults <= 64) {
                    RT_LOGF(RT_TAG_GX, "GX thread: command raised an unknown exception (n=%llu)\n",
                            static_cast<unsigned long long>(faults));
                }
            }
            const uint64_t elapsed = ElapsedNs(started);
            g_statBusyNs.fetch_add(elapsed, std::memory_order_relaxed);
            ProfileRecord(profileKey, elapsed);
        }
        head += header.stride;
        g_head.store(head, std::memory_order_release);
        if (g_producerWaiting.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cvSpace.notify_one();
        }
    }
    t_isGxThread = false;
}

} // namespace

void Configure(bool enabled) { g_requested = enabled; }
bool Enabled() noexcept { return g_enabled; }
bool IsGxThread() noexcept { return t_isGxThread; }
void SetWaitCallback(void (*callback)()) { g_waitCallback = callback; }
uint32_t NativeThreadId() noexcept { return g_nativeTid.load(std::memory_order_acquire); }

void Start() {
    if (!g_requested || g_started) {
        return;
    }
    g_ring = static_cast<uint8_t*>(std::malloc(kRingBytes));
    if (g_ring == nullptr) {
        RT_LOGF(RT_TAG_GX, "GX thread: ring allocation failed; running the GX pipeline on the game thread\n");
        return;
    }
    g_started = true;
    g_enabled = true;
    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(ConsumerLoop);
    RT_LOGF(RT_TAG_GX, "GX thread started (%u MiB command ring)\n", kRingBytes >> 20);
}

void Stop() {
    if (!g_started) {
        return;
    }
    Drain();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop.store(true, std::memory_order_release);
    }
    g_cvData.notify_all();
    g_thread.join();
    g_started = false;
    g_enabled = false;
}

void FlushFifo() {
    if (g_pendingFifoBytes == 0) {
        return;
    }
    const uint32_t bytes = g_pendingFifoBytes;
    g_pendingFifoBytes = 0;
    ++g_statFifoRecords;
    g_statFifoBytes += bytes;
    PostRecordRaw(&FifoInvoke, g_pendingFifo, bytes);
}

void Drain() {
    if (!g_enabled || !g_started || t_isGxThread) {
        return;
    }
    FlushFifo();
    const uint64_t sequence = ++g_fenceRequested;
    PostRecordRaw(&FenceInvoke, &sequence, sizeof(sequence));
    ++g_statDrains;
    const auto started = Clock::now();
    while (g_fenceCompleted.load(std::memory_order_acquire) < sequence) {
        if (Clock::now() - started >= kDrainSpinBudget) {
            WaitWithCallback(g_cvFence, g_producerWaiting,
                             [&] { return g_fenceCompleted.load(std::memory_order_acquire) >= sequence; });
            break;
        }
        PerfStats::WaitScope wait;
        std::this_thread::yield();
    }
    g_statDrainWaitNs += ElapsedNs(started);
}

void PostFifoWord(uint32_t value, uint32_t sizeBytes) {
    if (sizeBytes == 0 || sizeBytes > 4) {
        sizeBytes = 4;
    }
    if (g_pendingFifoBytes + sizeBytes > kFifoChunkBytes) {
        FlushFifo();
    }
    uint8_t* out = g_pendingFifo + g_pendingFifoBytes;
    for (uint32_t i = 0; i < sizeBytes; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8u * (sizeBytes - 1u - i)));
    }
    g_pendingFifoBytes += sizeBytes;
}

void PostFifoBytes(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }
    if (g_pendingFifoBytes + sizeBytes > kFifoChunkBytes) {
        FlushFifo();
    }
    if (sizeBytes > kFifoChunkBytes) {
        ++g_statFifoRecords;
        g_statFifoBytes += sizeBytes;
        PostRecordRaw(&FifoInvoke, data, sizeBytes);
        return;
    }
    std::memcpy(g_pendingFifo + g_pendingFifoBytes, data, sizeBytes);
    g_pendingFifoBytes += sizeBytes;
}

std::string FormatStatsAndReset(double windowSeconds, uint32_t frames) {
    const double perFrame = frames != 0 ? 1.0 / static_cast<double>(frames) : 0.0;
    const double busyNs = static_cast<double>(g_statBusyNs.exchange(0, std::memory_order_relaxed));
    const double busyPercent = windowSeconds > 0.0 ? busyNs / (windowSeconds * 1e9) * 100.0 : 0.0;
    char buffer[1024];
    const int written = std::snprintf(buffer, sizeof(buffer),
                  "GX thread: %.0f records/frame (%.1f KiB, %.0f FIFO chunks with %.1f KiB), queue peak %.1f KiB; "
                  "game thread waited %.2f ms/frame for ring space and %.2f ms/frame in %.1f drains/frame; "
                  "GX thread busy %.1f%%; faults %llu",
                  static_cast<double>(g_statRecords) * perFrame,
                  static_cast<double>(g_statBytes) * perFrame / 1024.0,
                  static_cast<double>(g_statFifoRecords) * perFrame,
                  static_cast<double>(g_statFifoBytes) * perFrame / 1024.0,
                  static_cast<double>(g_statQueuePeakBytes) / 1024.0,
                  static_cast<double>(g_statSpaceWaitNs) * perFrame / 1e6,
                  static_cast<double>(g_statDrainWaitNs) * perFrame / 1e6,
                  static_cast<double>(g_statDrains) * perFrame, busyPercent,
                  static_cast<unsigned long long>(g_statFaults.load(std::memory_order_relaxed)));
    g_statRecords = g_statFifoRecords = g_statFifoBytes = g_statBytes = 0;
    g_statDrains = g_statDrainWaitNs = g_statSpaceWaitNs = g_statQueuePeakBytes = 0;
    // The costliest record kinds of the window, as ms per frame and calls per frame.
    RecordProfile top[kProfileSlots];
    std::memcpy(top, g_profile, sizeof(top));
    std::memset(g_profile, 0, sizeof(g_profile));
    std::sort(std::begin(top), std::end(top), [](const RecordProfile& a, const RecordProfile& b) { return a.ns > b.ns; });
    std::string line = written > 0 ? std::string(buffer, static_cast<size_t>(std::min<int>(written, sizeof(buffer) - 1))) : std::string();
    line += "; costliest:";
    for (size_t i = 0; i < 6 && top[i].key != 0; ++i) {
        std::snprintf(buffer, sizeof(buffer), " %s %.2f ms x%.0f", DescribeProfileKey(top[i].key).c_str(),
                      static_cast<double>(top[i].ns) * perFrame / 1e6, static_cast<double>(top[i].count) * perFrame);
        line += buffer;
    }
    return line;
}

namespace detail {
void PostRecord(Invoke invoke, const void* payload, uint32_t payloadBytes) {
    FlushFifo();
    PostRecordRaw(invoke, payload, payloadBytes);
}
} // namespace detail

} // namespace GxThread
