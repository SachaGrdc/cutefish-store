#pragma once

#include <cutefish/store/error.h>

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace cutefish::store {

/// Either a value or an Error. Expected failures are reported this way, never
/// by exceptions (std::expected is C++23; this is the minimal C++20 subset
/// the core needs).
///
/// Accessing value() on an error, or error() on a value, is a programming
/// error: it is checked by assert() in debug builds.
template <typename T>
class [[nodiscard]] Result
{
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    Result(const T& value) : m_data(std::in_place_index<0>, value) {}
    Result(T&& value) : m_data(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_data(std::in_place_index<1>, std::move(error)) {}

    bool hasValue() const noexcept { return m_data.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() & { assert(hasValue()); return *std::get_if<0>(&m_data); }
    const T& value() const& { assert(hasValue()); return *std::get_if<0>(&m_data); }
    T&& value() && { assert(hasValue()); return std::move(*std::get_if<0>(&m_data)); }

    const Error& error() const& { assert(!hasValue()); return *std::get_if<1>(&m_data); }
    Error&& error() && { assert(!hasValue()); return std::move(*std::get_if<1>(&m_data)); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    template <typename U>
    T valueOr(U&& fallback) const&
    {
        return hasValue() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> m_data;
};

/// Success without a value, or an Error.
template <>
class [[nodiscard]] Result<void>
{
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)) {}

    static Result success() { return {}; }

    bool hasValue() const noexcept { return !m_error.has_value(); }
    explicit operator bool() const noexcept { return hasValue(); }

    const Error& error() const& { assert(!hasValue()); return *m_error; }
    Error&& error() && { assert(!hasValue()); return std::move(*m_error); }

private:
    std::optional<Error> m_error;
};

} // namespace cutefish::store
