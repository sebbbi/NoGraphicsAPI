#include <NoGraphicsAPIUtility/delete_queue.hpp>

#include <cassert>

namespace gpu
{

DeleteQueue::DeleteQueue(TimelineSemaphore* timeline, uint32_t capacity) noexcept
    : timeline_(timeline), entries_(new Entry[capacity]), capacity_(capacity)
{
    assert(timeline);
    assert(capacity != 0);
}

DeleteQueue::~DeleteQueue() noexcept
{
    assert(count_ == 0 && "delete queue destroyed with pending callbacks");
    delete[] entries_;
}

void DeleteQueue::tick() noexcept
{
    if (count_ == 0)
        return;

    assert(timeline_);
    collect(timeline_completed_value(timeline_));
}

void DeleteQueue::drain() noexcept
{
    collect(UINT64_MAX);
}

void DeleteQueue::collect(uint64_t completed_value) noexcept
{
    assert(!ticking_ && "delete queue callbacks cannot recursively collect the same queue");
    ticking_ = true;
    while (count_ != 0)
    {
        Entry& entry = entries_[head_];
        if (entry.retire_value > completed_value)
            break;

        entry.callback();
        entry.callback.clear();
        head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
        --count_;
    }
    ticking_ = false;
}

} // namespace gpu
