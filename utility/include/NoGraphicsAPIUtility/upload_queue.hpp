#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

namespace gpu
{

struct UploadQueueStats
{
    uint64 capacity = 0;
    uint64 bytes_in_use = 0;
    uint64 peak_bytes = 0;
    uint64 submissions = 0;
    uint64 waits = 0;
    uint32 pending_batches = 0;
    uint32 pending_operations = 0;
};

// Externally synchronize this uploader and its selected queue. Destination and callback resource lifetimes remain caller-owned.
// Order texture initialization and prior resource use on that queue before enqueuing uploads, including any waits on other queues.
class UploadQueue
{
public:
    static constexpr uint32 retirement_capacity = 16;
    static constexpr uint32 operation_limit = 4096;
    static constexpr uint64 alignment = 16;

    UploadQueue() noexcept = default;
    ~UploadQueue() noexcept;
    UploadQueue(const UploadQueue&) = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;
    UploadQueue(UploadQueue&& other) noexcept;
    UploadQueue& operator=(UploadQueue&& other) noexcept;

    // Capacity must be a positive multiple of 16. queue_index must be less than DeviceCaps::queue_count.
    [[nodiscard]] bool init(Device* device, uint64 capacity = 64ull * 1024 * 1024, uint32 queue_index = 0) noexcept;
    // Flushes and waits for uploads. Finish external submissions using returned timeline points before destruction or move assignment.
    void destroy() noexcept;

    // Sources are copied immediately. Storage never grows: capacity or operation pressure may submit work and wait for reusable space.
    // Flush between overlapping transfer writes. Buffer copies follow copy_memory's requirements; large sources are split to fit.
    void upload_buffer(GpuRange destination, ByteSpan source) noexcept;
    // Source covers one complete region, including pitch padding, and must fit capacity. The region follows copy_memory_to_texture's requirements.
    void upload_texture(Texture* destination, ByteSpan source, const TextureCopyDesc& copy = {}) noexcept;

    // Reserves byte_size (1..capacity), then invokes callback(commands, staging) synchronously with 16-byte-aligned CPU/GPU addresses.
    // Fill staging and record compute work using ordinary command APIs. The reservation remains occupied until its submission completes.
    // The callback must not mutate the uploader or end/submit/retain its command buffer. Staging cannot be used by later submissions.
    // Bind required pipeline/heaps and record dependencies between dispatches inside the callback.
    template<typename Callback>
    void upload_with_compute(uint64 byte_size, Callback&& callback) noexcept
    {
        const GpuCpuRange<byte> staging = begin_compute(byte_size);
        static_assert(noexcept(callback(state_.commands, staging)));
        callback(state_.commands, staging);
        end_compute();
    }

    // The device's per-command-buffer timestamp limit applies to each batch.
    void write_timestamp(uint64* gpu_destination) noexcept;
    // Returns the latest upload completion point, borrowing this uploader's semaphore. Other queues can consume it through SubmitDesc::waits.
    TimelinePoint flush() noexcept;
    void wait() noexcept;
    // Occupied bytes and pending batches reflect retirements observed by the most recent operation.
    [[nodiscard]] UploadQueueStats stats() const noexcept;

private:
    struct Batch
    {
        CommandPool* pool = nullptr;
        uint64 end = 0;
        uint64 value = 0;
    };

    struct State
    {
        Device* device = nullptr;
        GpuHeap heap{};
        TimelinePoint completion{};
        CommandBuffer* commands = nullptr;
        uint64 head = 0;
        uint64 tail = 0;
        Batch batches[retirement_capacity]{};
        uint32 queue_index = 0;
        uint32 retirement_first = 0;
        uint32 retirement_count = 0;
        uint32 operation_count = 0;
        bool in_callback = false;
        uint64 submissions = 0;
        uint64 waits = 0;
        uint64 peak_bytes = 0;
    };

    CommandBuffer* begin() noexcept;
    void reclaim() noexcept;
    void wait_oldest() noexcept;
    GpuCpuRange<byte> reserve(uint64 byte_size) noexcept;
    GpuCpuRange<byte> begin_compute(uint64 byte_size) noexcept;
    void end_compute() noexcept;

    State state_{};
};

} // namespace gpu
