#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

namespace gpu
{

class DeleteQueue
{
public:
    explicit DeleteQueue(TimelineSemaphore* timeline, uint32_t capacity) noexcept;
    ~DeleteQueue() noexcept;

    DeleteQueue(const DeleteQueue&) = delete;
    DeleteQueue& operator=(const DeleteQueue&) = delete;
    DeleteQueue(DeleteQueue&&) = delete;
    DeleteQueue& operator=(DeleteQueue&&) = delete;

    // Retire values must be enqueued in nondecreasing order.
    template<typename Callback>
    void defer(uint64_t retire_value, Callback callback) noexcept
    {
        static_assert(std::is_trivially_copyable_v<Callback>);
        static_assert(std::is_trivially_destructible_v<Callback>);
        static_assert(std::is_nothrow_move_constructible_v<Callback>);
        static_assert(std::is_nothrow_invocable_v<Callback&>);
        static_assert(sizeof(Callback) <= callback_storage_size);
        static_assert(alignof(Callback) <= alignof(std::max_align_t));

        assert(count_ < capacity_ && "delete queue capacity exhausted");

        assert((count_ == 0 || entries_[tail_ == 0 ? capacity_ - 1 : tail_ - 1].retire_value <= retire_value) &&
               "delete queue retire values must be nondecreasing");

        Entry& entry = entries_[tail_];
        entry.retire_value = retire_value;
        entry.invoke = invoke_callback<Callback>;
        ::new (static_cast<void*>(entry.callback)) Callback(static_cast<Callback&&>(callback));
        tail_ = tail_ + 1 == capacity_ ? 0 : tail_ + 1;
        ++count_;
    }

    void tick() noexcept;

private:
    static constexpr uint32_t callback_storage_size = 128;
    using InvokeCallback = void (*)(void*) noexcept;

    struct Entry
    {
        uint64_t retire_value;
        InvokeCallback invoke;
        alignas(std::max_align_t) byte callback[callback_storage_size];
    };

    template<typename Callback>
    static void invoke_callback(void* storage) noexcept
    {
        (*std::launder(reinterpret_cast<Callback*>(storage)))();
    }

    TimelineSemaphore* timeline_ = nullptr;
    Entry* entries_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint32_t count_ = 0;
    bool ticking_ = false;
};

} // namespace gpu
