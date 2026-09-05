#include <NoGraphicsAPIUtility/bump_allocator.hpp>

#include <cassert>

namespace gpu
{
using std::uintptr_t;

namespace
{

byte* offset_pointer(byte* pointer, uint64_t offset) noexcept
{
    if (!pointer)
        return nullptr;
    return reinterpret_cast<byte*>(reinterpret_cast<uintptr_t>(pointer) + offset);
}

} // namespace

BumpAllocator::BumpAllocator(GpuCpuRange<byte> storage) noexcept
    : storage_(storage)
{
    assert(storage.size != 0);
    assert(storage.cpu || storage.gpu);
    assert((!storage.cpu || reinterpret_cast<uintptr_t>(storage.cpu) % alignment == 0) &&
           (!storage.gpu || reinterpret_cast<uintptr_t>(storage.gpu) % alignment == 0));
}

BumpAllocator::BumpAllocator(BumpAllocator&& other) noexcept
    : storage_(other.storage_), offset_(other.offset_)
{
    other.storage_ = {};
    other.offset_ = 0;
}

BumpAllocator& BumpAllocator::operator=(BumpAllocator&& other) noexcept
{
    if (this == &other)
        return *this;

    storage_ = other.storage_;
    offset_ = other.offset_;
    other.storage_ = {};
    other.offset_ = 0;
    return *this;
}

GpuCpuRange<byte> BumpAllocator::allocate(uint64_t byte_size) noexcept
{
    assert(byte_size != 0);
    if (byte_size == 0 || offset_ > storage_.size || byte_size > storage_.size - offset_)
        return {};

    const GpuCpuRange<byte> allocation{
        .cpu = offset_pointer(storage_.cpu, offset_),
        .gpu = offset_pointer(storage_.gpu, offset_),
        .size = byte_size,
    };
    const uint64_t remaining = storage_.size - offset_;
    const uint64_t aligned_size = byte_size <= std::numeric_limits<uint64_t>::max() - (alignment - 1)
                                           ? (byte_size + alignment - 1) & ~(alignment - 1)
                                           : remaining;
    offset_ += aligned_size < remaining ? aligned_size : remaining;
    return allocation;
}

void BumpAllocator::reset() noexcept
{
    offset_ = 0;
}

} // namespace gpu
