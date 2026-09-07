#pragma once

#include <NoGraphicsAPI/types.h>
#include <NoGraphicsAPI/traits.hpp>

#include <initializer_list>

namespace gpu
{

template<typename T>
struct Span
{
    T* data = nullptr;
    size_t size = 0;

    constexpr Span() noexcept = default;

    constexpr Span(T* values, size_t count) noexcept : data(values), size(count) {}

    template<size_t Count>
    constexpr Span(T (&values)[Count]) noexcept : data(values), size(Count) {}

    template<typename U>
    constexpr Span(Span<U> values) noexcept requires(detail::is_same_v<detail::remove_cv_t<U>, detail::remove_cv_t<T>> && detail::is_convertible_v<U*, T*>)
        : Span(values.data, values.size) {}

    template<typename Container>
    constexpr Span(Container&& values) noexcept
        requires(detail::is_same_v<detail::remove_cv_t<detail::remove_pointer_t<decltype(values.data())>>, detail::remove_cv_t<T>> &&
                 detail::is_convertible_v<decltype(values.data()), T*> && detail::is_convertible_v<decltype(values.size()), size_t>)
        : Span(values.data(), values.size()) {}

    // Temporary container and initializer-list storage is valid only through
    // the containing full expression. Functions must not retain the span.
    constexpr Span(std::initializer_list<detail::remove_const_t<T>> values) noexcept requires detail::is_const_v<T>
        : Span(values.begin(), values.size()) {}
};

struct ByteSpan
{
    const byte* data = nullptr;
    size_t size = 0;

    constexpr ByteSpan() noexcept = default;

    ByteSpan(const void* bytes, size_t byte_size) noexcept
        : data(static_cast<const byte*>(bytes)), size(byte_size) {}

    // A span converted from a value remains valid only while that value is
    // alive. Functions must not retain the span.
    template<typename T>
        requires(__is_class(T) && !detail::is_volatile_v<T> && __is_trivially_copyable(T) && (sizeof(T) & 3u) == 0)
    ByteSpan(const T& value) noexcept
        : data(reinterpret_cast<const byte*>(__builtin_addressof(value))), size(sizeof(T)) {}
};

} // namespace gpu
