#pragma once

#include <NoGraphicsAPI/types.h>
#include <NoGraphicsAPI/traits.hpp>

#include <assert.h>

namespace gpu::detail
{
enum class CallbackPlacement {};
}

inline void* operator new(size_t, void* storage, gpu::detail::CallbackPlacement) noexcept
{
    return storage;
}

namespace gpu
{

// Stores one trivial noexcept void callback in StorageSize inline bytes. Clear before setting a new callback.
template<size_t StorageSize>
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
        static_assert(__is_trivially_copyable(Callback));
        static_assert(detail::is_trivially_destructible_v<Callback>);
        static_assert(noexcept(Callback(static_cast<Callback&&>(callback))));
        static_assert(noexcept(callback()));
        static_assert(sizeof(Callback) <= StorageSize);
        static_assert(alignof(Callback) <= maximum_alignment);

        assert(!invoke_);
        ::new (static_cast<void*>(storage_), detail::CallbackPlacement{}) Callback(static_cast<Callback&&>(callback));
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
        (*__builtin_launder(reinterpret_cast<Callback*>(storage)))();
    }

    alignas(maximum_alignment) byte storage_[StorageSize];
    Invoke invoke_ = nullptr;
};

} // namespace gpu
