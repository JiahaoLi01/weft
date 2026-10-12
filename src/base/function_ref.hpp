#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace weft {
    template <class Signature>
    class FunctionRef;

    // A non-owning reference to a callable object, for callbacks on hot paths: one indirect call and no allocation,
    // unlike std::function. The referenced object must outlive every call; bind lambdas or function objects, not
    // plain functions.
    template <class R, class... Args>
    class FunctionRef<R(Args...)> {
    public:
        template <class F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_object_v<std::remove_cvref_t<F>> &&
                     std::is_invocable_r_v<R, std::remove_reference_t<F>&, Args...>)
        FunctionRef(F&& f) noexcept // NOLINT(google-explicit-constructor): binds like a function parameter.
            : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
              call_([](void* object, Args... args) -> R {
                  auto& callable = *static_cast<std::remove_reference_t<F>*>(object);
                  // A void signature discards the result, as INVOKE<void> (std::is_invocable_r above) does: the
                  // callable may return a value, even a [[nodiscard]] one.
                  if constexpr (std::is_void_v<R>) {
                      static_cast<void>(std::invoke(callable, std::forward<Args>(args)...));
                  } else {
                      return std::invoke(callable, std::forward<Args>(args)...);
                  }
              }) {
        }

        R operator()(Args... args) const {
            return call_(object_, std::forward<Args>(args)...);
        }

    private:
        void* object_;
        R (*call_)(void*, Args...);
    };
} // namespace weft
