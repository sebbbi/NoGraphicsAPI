#pragma once

namespace gpu::detail
{

template<typename T, typename U> constexpr bool is_same_v = false;
template<typename T> constexpr bool is_same_v<T, T> = true;
template<typename T> constexpr bool is_const_v = false;
template<typename T> constexpr bool is_const_v<const T> = true;
template<typename T> constexpr bool is_volatile_v = false;
template<typename T> constexpr bool is_volatile_v<volatile T> = true;

template<typename T> struct RemoveConst { using type = T; };
template<typename T> struct RemoveConst<const T> { using type = T; };
template<typename T> using remove_const_t = typename RemoveConst<T>::type;
template<typename T> struct RemoveVolatile { using type = T; };
template<typename T> struct RemoveVolatile<volatile T> { using type = T; };
template<typename T> using remove_cv_t = typename RemoveVolatile<remove_const_t<T>>::type;
template<typename T> struct RemovePointer { using type = T; };
template<typename T> struct RemovePointer<T*> { using type = T; };
template<typename T> using remove_pointer_t = typename RemovePointer<remove_cv_t<T>>::type;

#if defined(_MSC_VER) || defined(__clang__)
template<typename From, typename To> constexpr bool is_convertible_v = __is_convertible_to(From, To);
template<typename T> constexpr bool is_trivially_destructible_v = __is_trivially_destructible(T);
#else
template<typename T> T&& declval() noexcept;
template<typename T> void accept(T) noexcept;
template<typename From, typename To> constexpr bool is_convertible_v = requires { detail::accept<To>(detail::declval<From>()); };
template<typename T> constexpr bool is_trivially_destructible_v = requires(T& value) { value.~T(); } && __has_trivial_destructor(T);
#endif

} // namespace gpu::detail
