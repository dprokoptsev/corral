// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024-2025 Hudson River Trading LLC
// <opensource@hudson-trading.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// SPDX-License-Identifier: MIT

#pragma once
#include <concepts>
#include <type_traits>
#include <utility>

#include "config.h"
#include "detail/concept_helpers.h"

namespace corral {

/// ERROR POLICIES
/// --------------
///
/// Some functions communicate errors through their return value (using types
/// like `std::expected` or `absl::StatusOr`) instead of by raising exceptions.
/// You can teach corral about these types in order to obtain the same
/// error-handling semantics that you would have using exceptions. For example,
/// if one task in an `allOf()` returns an error, the others will be cancelled
/// and the result of the entire `allOf()` will be that error.
///
/// Some terminology:
/// - Wrapped type: the function return type that can carry either a value or
///   an error, such as `std::expected<V, E>`
/// - Value type: the "logical" return type if an error does not occur (`V`)
/// - Error type: the type representing an error if one does occur (`E`)
///
/// An error policy `Policy` that is used with wrapped type `Wrapped` must
/// satisfy the below concept `ApplicableErrorPolicy<Policy, Wrapped>`. See the
/// comments on the concept definition for explanations of methods of `Policy`.
/// You can request use of an error policy for a particular call by passing it
/// as a template argument to anyOf/allOf combiners, or can specialize
/// `corral::UseErrorPolicy<Wrapped>::Type` to select it automatically for
/// all functions that return this type (different tasks in a single
/// anyOf/allOf cannot have different automatic error policies).
///
/// You can define a default error policy for the entire program using
/// `#define CORRAL_DEFAULT_ERROR_POLICY <qualified type name>` before
/// including any corral header.
///
/// Note that the return type of anyOf/allOf changes if an error policy is used:
/// `allOf(Task<absl::StatusOr<int>>, Task<absl::StatusOr<bool>>)` produces
/// `std::tuple<absl::StatusOr<int>, absl::StatusOr<bool>>` without a policy,
/// `absl::StatusOr<std::tuple<int, bool>>` with one.

// clang-format off
template <class P, class Wrapped>
concept ApplicableErrorPolicy = requires {
    typename P::ErrorType;
} && requires(const typename P::ErrorType& e) {
    // A default-initialized ErrorType should indicate no error
    { typename P::ErrorType{} };

    /// Converts the current exception to the ErrorType.
    /// May call std::unreachable() if the policy does not allow
    /// exceptions.
    { P::fromCurrentException() } -> std::same_as<typename P::ErrorType>;

    /// Returns true if `e` holds a non-degenerate error.
    { P::hasError(e) } -> std::convertible_to<bool>;

    /// Called in contexts where the error cannot be propagated any further,
    /// and program termination is justified.
    { P::terminateBy(e) };

    /// Wraps the error into the wrapped object.
    /// May return void if exceptions are used in this policy
    /// (in this case the function is assumed to never return).
    { P::wrapError(e) } -> detail::convertible_to_any<void, Wrapped>;

} && (std::is_same_v<Wrapped, void> || requires(Wrapped w, const Wrapped& cw) {
    /// Extracts the error from the wrapped object.
    /// Should work on objects holding either an error or a value,
    /// and produce a default-constructed error type in the latter case.
    { P::unwrapError(cw) } -> std::convertible_to<typename P::ErrorType>;

    /// Extracts the value from the wrapped object.
    /// Will only be called on wrapped objects holding a value.
    /// May return void if the wrapped object does not carry any value
    /// besides its status.
    ///
    /// NB: the function must take an explicit template parameter,
    /// matching the type of its argument (this is necessary for telling
    /// between values and rvalue references for policies not wrapping
    /// them into any new objecs). An attempt to declare
    ///    template<class T, class E> T unwrapValue(std::expected<T, E> ex)
    /// will result in a compilation error.
    ///
    /// In practice, `template<class W> auto unwrapValue(W w) { return *w; }`
    /// should work for almost any reasonable wrapped type.
    { P::template unwrapValue<Wrapped>(std::forward<Wrapped>(w)) };

}) && ((!std::is_same_v<Wrapped, void> && (
    (!std::is_same_v<decltype(P::unwrapValue(std::declval<Wrapped>())), void> &&
    requires(Wrapped w, const Wrapped& cw) {
        /// For semantic values other than void,
        /// `wrapValue(v)` wraps the value `v` into the wrapped object.
        ///
        /// NB: the function must take an explicit template parameter,
        /// which would match type of its argument -- this allows telling between
        /// values and rvalue references (so `wrapValue<int>(std::move(i))`
        /// should wrap an int, whereas `wrapValue<int&&>(std::move(i))`
        /// should wrap an rvalue reference, and later yield the same reference
        /// when unwrapped).
        {
            P::template wrapValue<
                decltype(P::template unwrapValue<Wrapped>(std::forward<Wrapped>(w)))
            >(P::template unwrapValue<Wrapped>(std::forward<Wrapped>(w)))
        } -> std::convertible_to<Wrapped>;

    }) || (std::is_same_v<decltype(P::unwrapValue(std::declval<Wrapped>())), void> && requires {
        /// For void values, `wrapValue()` should return a wrapped object
        /// indicating success.
        { P::wrapValue() } -> detail::convertible_to<Wrapped>;
    })
)) || (std::is_same_v<Wrapped, void> && requires {
    { P::wrapValue() } -> std::same_as<void>;
}));
// clang-format on

/// This trait type can be specialized for a type (or a family of types)
/// to define a default error policy for those types.
template <class T> struct UseErrorPolicy {
    using Type = detail::DefaultErrorPolicy;
};


/// An error policy which uses C++ exceptions for error propagation.
class UseExceptions {
  public:
    using ErrorType = std::exception_ptr;

    static ErrorType fromCurrentException() noexcept {
        std::exception_ptr ex = std::current_exception();
        CORRAL_ASSERT(
                ex &&
                "foreign exceptions and forced unwinds are not supported");
        return ex;
    }

    static bool hasError(const ErrorType& ex) noexcept { return ex != nullptr; }

    static void terminateBy(const ErrorType& ex) noexcept {
        std::rethrow_exception(ex);
    }

    static ErrorType unwrapError(const auto&) noexcept { return nullptr; }

    template <class T> static T unwrapValue(T&& t) {
        return std::forward<T>(t);
    }
    static void unwrapValue() {}

    template <class T> static T wrapValue(T&& t) { return std::forward<T>(t); }
    static void wrapValue() {}

    [[noreturn]] static void wrapError(std::exception_ptr ex) {
        std::rethrow_exception(ex);
    }
};


namespace detail {
[[noreturn]] void unreachable();
} // namespace detail

/// An error policy for operations which cannot fail.
struct Infallible {
    using ErrorType = std::monostate;

    static ErrorType fromCurrentException() noexcept { detail::unreachable(); }
    static bool hasError(const auto&) noexcept { return false; }
    static void terminateBy(const ErrorType&) noexcept {
        detail::unreachable();
    }

    static auto unwrapError(const auto&) noexcept { return std::monostate{}; }
    template <class T> static T unwrapValue(T&& t) {
        return std::forward<T>(t);
    }
    static void unwrapValue() {}

    template <class T> static T wrapValue(T&& t) { return std::forward<T>(t); }
    static void wrapValue() {}
    [[noreturn]] static void wrapError(std::monostate) {
        detail::unreachable();
    }
};


namespace detail {

template <class T>
    requires ApplicableErrorPolicy<typename UseErrorPolicy<T>::Type, T>
struct ValidateErrorPolicy {
    using Type = typename UseErrorPolicy<T>::Type;
};

template <class T, class... Rest>
constexpr bool HaveCompatibleErrorPolicies =
        (std::is_same_v<typename UseErrorPolicy<T>::Type,
                        typename UseErrorPolicy<Rest>::Type> &&
         ...);

template <class... Ts> struct DetectErrorPolicyImpl {
    static_assert(sizeof...(Ts) == 0,
                  "Incompatible error policies across arguments; "
                  "supply the policy explicitly");
    using Type = detail::DefaultErrorPolicy; // make older compilers happy
};

template <> struct DetectErrorPolicyImpl<> {
    using Type = Infallible;
};

template <class T, class... Rest>
    requires HaveCompatibleErrorPolicies<T, Rest...>
struct DetectErrorPolicyImpl<T, Rest...> {
    using Type = typename ValidateErrorPolicy<T>::Type;
};

template <class... Ts>
using DetectErrorPolicy = typename DetectErrorPolicyImpl<Ts...>::Type;


//
// Utility type helpers
//

template <class Policy, class T> struct PolicyReturnTypeForImpl {
    using Type = decltype(Policy::template wrapValue<T>(std::declval<T>()));
};
template <class Policy> struct PolicyReturnTypeForImpl<Policy, void> {
    using Type = decltype(Policy::wrapValue());
};
// The wrapped type that will be returned by functions conforming
// to the policy, if they semantically produce a value of type T.
// (for example, this can be `std::expected<T, SomeErrorType>`).
template <class Policy, class T>
using PolicyReturnTypeFor = typename PolicyReturnTypeForImpl<Policy, T>::Type;

template <class Policy, class T> struct PolicyValueTypeForImpl {
    using Type = decltype(Policy::template unwrapValue<T>(std::declval<T>()));
};
template <class Policy> struct PolicyValueTypeForImpl<Policy, void> {
    using Type = void;
};
// Inverse of the above: the value type semantically produced by a
// function returning the given wrapped type
// (so `std::expected<T, SomeErrorType>` might be transformed into T).
//
// `void` always transforms into `void`, since void-returning functions
// don't have any error propagation channels (short of raising exceptions).
template <class Policy, class T>
using PolicyValueTypeFor = typename PolicyValueTypeForImpl<Policy, T>::Type;

template <class Policy>
static constexpr const bool PolicyUsesErrorCodes =
        !std::is_same_v<PolicyReturnTypeFor<Policy, void>, void>;


} // namespace detail
} // namespace corral
