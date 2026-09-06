#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/fixed_function.hpp>

#include <cassert>

namespace gpu
{

class DeleteQueue
{
public:
    // Capacity must be nonzero, every callback must run before destruction, and the timeline must be live when tick() is called.
    explicit DeleteQueue(TimelineSemaphore* timeline, uint32_t capacity) noexcept;
    ~DeleteQueue() noexcept;

    DeleteQueue(const DeleteQueue&) = delete;
    DeleteQueue& operator=(const DeleteQueue&) = delete;
    DeleteQueue(DeleteQueue&&) = delete;
    DeleteQueue& operator=(DeleteQueue&&) = delete;

    // Retire values must be enqueued in nondecreasing order. Callbacks use 128 inline bytes and must satisfy FixedFunction's requirements.
    template<typename Callback>
    void defer(uint64_t retire_value, Callback callback) noexcept
    {
        assert(count_ < capacity_ && "delete queue capacity exhausted");

        assert((count_ == 0 || entries_[tail_ == 0 ? capacity_ - 1 : tail_ - 1].retire_value <= retire_value) &&
               "delete queue retire values must be nondecreasing");

        Entry& entry = entries_[tail_];
        entry.retire_value = retire_value;
        entry.callback.set(static_cast<Callback&&>(callback));
        tail_ = tail_ + 1 == capacity_ ? 0 : tail_ + 1;
        ++count_;
    }

    void tick() noexcept;
    void drain() noexcept; // Call after wait_idle() and before destroy_device(); runs every queued callback without checking the timeline.

private:
    struct Entry
    {
        uint64_t retire_value;
        FixedFunction<128> callback;
    };

    void collect(uint64_t completed_value) noexcept; // Callbacks cannot recursively collect the same queue.

    TimelineSemaphore* timeline_ = nullptr;
    Entry* entries_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint32_t count_ = 0;
    bool ticking_ = false;
};

} // namespace gpu
