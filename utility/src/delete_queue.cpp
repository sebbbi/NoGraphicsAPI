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

    assert(!ticking_ && "delete queue callbacks cannot call tick on the same queue");
    assert(timeline_);
    ticking_ = true;
    const uint64_t completed_value = timeline_completed_value(timeline_);
    while (count_ != 0)
    {
        Entry& entry = entries_[head_];
        if (entry.retire_value > completed_value)
            break;

        entry.invoke(entry.callback);
        head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
        --count_;
    }
    ticking_ = false;
}

} // namespace gpu
