#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

namespace gpu
{

// Stores one trivial noexcept void callback in StorageSize inline bytes. Clear before setting a new callback.
template<std::size_t StorageSize>
class FixedFunction
{
public:
    static_assert(StorageSize != 0);

    FixedFunction() noexcept = default;

    FixedFunction(const FixedFunction&) = delete;
    FixedFunction& operator=(const FixedFunction&) = delete;
    FixedFunction(FixedFunction&&) = delete;
    FixedFunction& operator=(FixedFunction&&) = delete;

    template<typename Callback>
    void set(Callback callback) noexcept
    {
        static_assert(std::is_trivially_copyable_v<Callback>);
        static_assert(std::is_trivially_destructible_v<Callback>);
        static_assert(std::is_nothrow_move_constructible_v<Callback>);
        static_assert(std::is_nothrow_invocable_v<Callback&>);
        static_assert(sizeof(Callback) <= StorageSize);
        static_assert(alignof(Callback) <= alignof(std::max_align_t));

        assert(!invoke_);
        ::new (static_cast<void*>(storage_)) Callback(static_cast<Callback&&>(callback));
        invoke_ = invoke<Callback>;
    }

    void operator()() noexcept
    {
        assert(invoke_);
        invoke_(storage_);
    }

    void clear() noexcept
    {
        invoke_ = nullptr;
    }

private:
    using Invoke = void (*)(void*) noexcept;

    template<typename Callback>
    static void invoke(void* storage) noexcept
    {
        (*std::launder(reinterpret_cast<Callback*>(storage)))();
    }

    alignas(std::max_align_t) std::byte storage_[StorageSize];
    Invoke invoke_ = nullptr;
};

} // namespace gpu
