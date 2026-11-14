#include "strict_stox.hh"
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
template<class T> bool same(T left, T right) {
	if constexpr (std::is_floating_point_v<T>) {
		if (std::isnan(left) && std::isnan(right)) { return true; }
		if (left == 0 && right == 0) { return std::signbit(left) == std::signbit(right); }
	}
	return left == right;
}
template<class Throwing, class Nothrow>
void check(std::size_t size, Throwing&& throwing, Nothrow&& nothrow) {
	for (bool prefix : {false, true}) {
		std::size_t expected_index = 0, actual_index = 0;
		int error = 0;
		errno = EDOM;
		const auto expected = nothrow(&error, prefix ? &expected_index : nullptr);
		if (errno != EDOM) { std::abort(); }
		try {
			const auto actual = throwing(prefix ? &actual_index : nullptr);
			if (error || errno != EDOM || !same(actual, expected) ||
				(prefix && (actual_index != expected_index || actual_index > size))) {
				std::abort();
			}
		} catch (const std::invalid_argument&) {
			if (error != EINVAL || errno != EDOM) { std::abort(); }
		} catch (const std::out_of_range&) {
			if (error != ERANGE || errno != EDOM) { std::abort(); }
		}
	}
}
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	const std::string input(reinterpret_cast<const char*>(data), size);
	const std::string_view text(input);
	constexpr int bases[] = {0, 2, 8, 10, 16, 36};
	const int base = bases[size ? data[0] % 6 : 3];
#define CHECK_INTEGER(name) check(size, [&](std::size_t* index) { return strict_##name(text, index, base); }, \
	[&](int* error, std::size_t* index) { return strict_##name(error, text, index, base); })
	CHECK_INTEGER(stoi); CHECK_INTEGER(stou); CHECK_INTEGER(stol); CHECK_INTEGER(stoul);
	CHECK_INTEGER(stoll); CHECK_INTEGER(stoull); CHECK_INTEGER(stoz);
#undef CHECK_INTEGER
#define CHECK_FLOAT(name) check(size, [&](std::size_t* index) { return strict_##name(text, index); }, \
	[&](int* error, std::size_t* index) { return strict_##name(error, text, index); })
	CHECK_FLOAT(stof); CHECK_FLOAT(stod); CHECK_FLOAT(stold);
#undef CHECK_FLOAT
	return 0;
}
