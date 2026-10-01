#include "../src/BufferAddressMap.hpp"
#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{
bool resolve(const gpu::detail::BufferAddressMap& map, uint64 base, uint64 size, uint64 expected_buffer, uint64 expected_offset)
{
    uint64 offset = 0;
    return map.resolve({.gpu = reinterpret_cast<void*>(base + expected_offset), .size = size}, &offset) == expected_buffer &&
           offset == expected_offset;
}

struct Worker
{
    gpu::detail::BufferAddressMap* map = nullptr;
    uint32 index = 0;
    bool valid = true;
#if defined(_WIN32)
    HANDLE thread = nullptr;
#else
    pthread_t thread = {};
#endif
};

#if defined(_WIN32)
DWORD WINAPI churn(void* argument)
#else
void* churn(void* argument)
#endif
{
    Worker& worker = *static_cast<Worker*>(argument);
    for (uint32 iteration = 0; iteration < 50000; ++iteration)
    {
        const uint64 base = 0x1400 + ((iteration * 13 + worker.index * 17) % 60) * 0x1000 + worker.index * 0x100;
        const uint64 buffer = (uint64(worker.index) << 32) | (iteration + 100);
        gpu::detail::BufferRecord* record = worker.map->insert(base, 256, buffer);
        worker.valid = resolve(*worker.map, base, 192, buffer, 64) && worker.valid;
        for (uint32 i = 0; i < 4; ++i)
            worker.valid = resolve(*worker.map, 0x1000 + i * 19 * 0x1000, 32, i * 19 + 1, 224) && worker.valid;
        worker.map->remove(record);
    }
    return 0;
}
}

int main()
{
    gpu::detail::BufferAddressMap map;
    gpu::detail::BufferRecord* records[64] = {};
    bool valid = true;
    for (uint32 i = 0; i < 64; ++i)
    {
        const uint32 index = (i * 37) & 63u;
        records[index] = map.insert(0x1000 + index * 0x1000, 256, index + 1);
    }
    for (uint32 i = 0; i < 64; ++i)
    {
        valid = resolve(map, 0x1000 + i * 0x1000, 256, i + 1, 0) && valid;
        valid = resolve(map, 0x1000 + i * 0x1000, 192, i + 1, 64) && valid;
        valid = resolve(map, 0x1000 + i * 0x1000, 0, i + 1, 256) && valid;
    }
    for (uint32 i = 0; i < 64; ++i) map.remove(records[(i * 19) & 63u]);
    for (uint32 i = 0; i < 60; ++i) records[i] = map.insert(0x1000 + i * 0x1000, 256, i + 1);
    Worker workers[4];
    uint32 started = 0;
    for (; started < 4; ++started)
    {
        workers[started].map = &map;
        workers[started].index = started;
#if defined(_WIN32)
        workers[started].thread = CreateThread(nullptr, 0, churn, workers + started, 0, nullptr);
        if (!workers[started].thread) break;
#else
        if (pthread_create(&workers[started].thread, nullptr, churn, workers + started) != 0) break;
#endif
    }
    valid = started == 4 && valid;
    for (uint32 i = 0; i < started; ++i)
    {
#if defined(_WIN32)
        valid = WaitForSingleObject(workers[i].thread, INFINITE) == WAIT_OBJECT_0 && valid;
        CloseHandle(workers[i].thread);
#else
        valid = pthread_join(workers[i].thread, nullptr) == 0 && valid;
#endif
        valid = workers[i].valid && valid;
    }
    for (uint32 i = 0; i < 60; ++i) map.remove(records[i]);
    records[0] = map.insert(0x1000, 512, 999);
    valid = resolve(map, 0x1000, 1, 999, 511) && valid;
    map.remove(records[0]);
    printf("Buffer address offsets, capacity, reuse and concurrent lookup: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
