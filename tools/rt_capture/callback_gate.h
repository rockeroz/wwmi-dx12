#pragma once
#include <type_traits>
#include <utility>

namespace rt_capture {
// Disabled hooks must not call into ReShade/native objects or take a capture lock.
// A disabled boolean event returns false, preserving the game's original call.
template <auto Fn, class... Args>
auto invoke_when_enabled(bool enabled, Args&&... args) -> std::invoke_result_t<decltype(Fn), Args...>
{
    using Result = std::invoke_result_t<decltype(Fn), Args...>;
    if (enabled) return Fn(std::forward<Args>(args)...);
    if constexpr (!std::is_void_v<Result>) return Result{};
}
} // namespace rt_capture
