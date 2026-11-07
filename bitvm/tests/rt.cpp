#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#include "bitvm/engine.hpp"
#include "bitvm/proto/text.hpp"

namespace
{
thread_local bool     g_audio       = false;
thread_local unsigned g_allocations = 0;
thread_local unsigned g_deletions   = 0;
}  // namespace

extern "C"
{
void* __real_malloc (std::size_t) noexcept;
void* __real_calloc (std::size_t, std::size_t) noexcept;
void* __real_realloc (void*, std::size_t) noexcept;
void  __real_free (void*) noexcept;

void*
__wrap_malloc (std::size_t size) noexcept {
    if (g_audio) {
        ++g_allocations;
    }
    return __real_malloc (size);
}
void*
__wrap_calloc (std::size_t count, std::size_t size) noexcept {
    if (g_audio) {
        ++g_allocations;
    }
    return __real_calloc (count, size);
}
void*
__wrap_realloc (void* memory, std::size_t size) noexcept {
    if (g_audio) {
        ++g_allocations;
    }
    if (g_audio && memory) {
        ++g_deletions;
    }
    return __real_realloc (memory, size);
}
void
__wrap_free (void* memory) noexcept {
    if (g_audio && memory) {
        ++g_deletions;
    }
    __real_free (memory);
}
}

[[gnu::noinline]] void*
operator new (std::size_t size) {
    if (g_audio) {
        ++g_allocations;
    }
    if (auto* memory = std::malloc (size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}
void*
operator new[] (std::size_t size) {
    return ::operator new (size);
}
void*
operator new (std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new (size);
    } catch (...) {
        return nullptr;
    }
}
void*
operator new[] (std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new (size, std::nothrow);
}
void
operator delete (void* memory) noexcept {
    if (g_audio && memory) {
        ++g_deletions;
    }
    std::free (memory);
}
void
operator delete[] (void* memory) noexcept {
    ::operator delete (memory);
}
void
operator delete (void* memory, std::size_t) noexcept {
    ::operator delete (memory);
}
void
operator delete[] (void* memory, std::size_t) noexcept {
    ::operator delete (memory);
}
void
operator delete (void* memory, const std::nothrow_t&) noexcept {
    ::operator delete (memory);
}
void
operator delete[] (void* memory, const std::nothrow_t&) noexcept {
    ::operator delete (memory);
}
[[gnu::noinline]] void*
operator new (std::size_t size, std::align_val_t alignment) {
    if (g_audio) {
        ++g_allocations;
    }
    void* memory = nullptr;
    if (::posix_memalign (&memory, static_cast<std::size_t> (alignment), size == 0 ? 1 : size) ==
        0) {
        return memory;
    }
    throw std::bad_alloc();
}
void*
operator new[] (std::size_t size, std::align_val_t alignment) {
    return ::operator new (size, alignment);
}
void*
operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new (size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void*
operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new (size, alignment, std::nothrow);
}
void
operator delete (void* memory, std::align_val_t) noexcept {
    ::operator delete (memory);
}
void
operator delete[] (void* memory, std::align_val_t) noexcept {
    ::operator delete (memory);
}
void
operator delete (void* memory, std::size_t, std::align_val_t) noexcept {
    ::operator delete (memory);
}
void
operator delete[] (void* memory, std::size_t, std::align_val_t) noexcept {
    ::operator delete (memory);
}
void
operator delete (void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    ::operator delete (memory);
}
void
operator delete[] (void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    ::operator delete (memory);
}

TEST (Realtime, CallbackDoesNotAllocateOrDeleteDuringCreationDoneAndRollback) {
    bitvm::Engine engine;
    const auto    graph = bitvm::proto::assemble_graph (
        "graph ping\n%osc = sin_osc.ar 440\n"
        "%env = env_perc.ar 0 0.001 1 -4 1\n%s = mul.ar %osc %env\nout.ar 0 %s\n"
    );
    ASSERT_TRUE (graph);
    const auto bytes = bitvm::proto::encode_graph (*graph);
    ASSERT_TRUE (bytes);
    const auto loaded = engine.call (1, bitvm::proto::DefLoad{ *bytes });
    ASSERT_TRUE (loaded);
    const auto def = std::get<bitvm::proto::DefLoaded> (*loaded).def;
    for (std::uint32_t i = 1; i <= 20; ++i) {
        ASSERT_TRUE (engine.submit (
            1,
            i,
            { static_cast<std::int64_t> (i * 1000000),
              { bitvm::proto::
                    NodeNew{ 0x01000000 + i, def, bitvm::proto::AddAction::head, 0, false, {} } } }
        ));
    }
    ASSERT_TRUE (engine.submit (
        1,
        21,
        { 21000000, { bitvm::proto::GroupNew{ 0x01000030 }, bitvm::proto::GroupNew{ 0x01000030 } } }
    ));
    std::array<float, 256> output;
    g_allocations = g_deletions = 0;
    for (unsigned i = 0; i < 20; ++i) {
        g_audio = true;
        engine.process (output, 128);
        g_audio = false;
        engine.collect();
    }
    EXPECT_EQ (g_allocations, 0U);
    EXPECT_EQ (g_deletions, 0U);
    EXPECT_EQ (engine.status().nodes, 0U);
    EXPECT_EQ (engine.status().groups, 1U);
}

TEST (Realtime, ProducerAndRendererExchangeAndReclaimConcurrently) {
    bitvm::EngineConfig config;
    config.max_nodes      = 32;
    config.queue_capacity = 16;
    bitvm::Engine          engine (config);
    std::atomic<bool>      done       = false;
    std::atomic<bool>      unexpected = false;
    std::thread            control ([&] {
        for (std::uint32_t i = 0; i < 1000; ++i) {
            bitvm::proto::Bundle bundle{ 0,
                                         { bitvm::proto::GroupNew{ 0x01000001 },
                                           bitvm::proto::NodeFree{ { 0x01000001 } } } };
            for (;;) {
                auto result = engine.submit (1, i, bundle);
                if (result) {
                    break;
                }
                if (result.error().code != bitvm::proto::ErrorCode::limit) {
                    unexpected = true;
                    break;
                }
                std::this_thread::yield();
            }
        }
        done = true;
    });
    std::array<float, 128> output;
    while (!done.load()) {
        engine.process (output, 64);
    }
    control.join();
    engine.process (output, 64);
    engine.collect();
    EXPECT_FALSE (unexpected.load());
    EXPECT_EQ (engine.status().groups, 1U);
    EXPECT_EQ (engine.status().rt_queue_used, 0U);
}
