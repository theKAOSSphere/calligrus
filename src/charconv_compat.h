// std::from_chars for float and double. The libc++ of the Android NDK does not have it (the call is deleted),
// so there it is built on strtof / strtod; everywhere else this is std::from_chars itself.
#pragma once

#include <charconv>

#ifdef __ANDROID__
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <type_traits>
#endif

namespace compat {

#ifdef __ANDROID__
template <class T>
std::from_chars_result from_chars_fp(const char* first, const char* last, T& value) {
    // Like from_chars: no leading space, no '+', and no "0x" prefix (only "0" is read of "0x10").
    if (first == last || std::isspace(static_cast<unsigned char>(*first)) || *first == '+') return {first, std::errc::invalid_argument};
    const char* digits = first + (*first == '-' ? 1 : 0);
    std::string text = (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) ? std::string(first, digits + 1) : std::string(first, last);
    char* end = nullptr;
    errno = 0;
    const double d = std::is_same_v<T, float> ? static_cast<double>(std::strtof(text.c_str(), &end)) : std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    const char* stop = first + (end - text.c_str());
    if (errno == ERANGE && (std::isinf(d) || d == 0.0)) return {stop, std::errc::result_out_of_range};
    value = static_cast<T>(d);
    return {stop, std::errc()};
}
#else
template <class T>
inline std::from_chars_result from_chars_fp(const char* first, const char* last, T& value) {
    return std::from_chars(first, last, value);
}
#endif

}  // namespace compat
