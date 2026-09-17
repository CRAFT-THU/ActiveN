// Independent exact-rounding check for the production positive-finite FP32
// power helper. Build with -std=c++20 -O3 -ffp-contract=off -fopenmp.
// The default samples exponent boundaries and mantissas; --exhaustive checks
// every positive finite FP32 bit pattern (including all subnormals).
#include "../payloads/sys/brunel_numeric.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

using Wide = unsigned __int128;

static int bit_length(Wide x) {
  const uint64_t high = x >> 64;
  return high ? 128 - __builtin_clzll(high)
              : 64 - __builtin_clzll(uint64_t(x));
}

// Compare x^2 against midpoint(y, nextafter(y,+inf))^5 exactly. Fifth powers
// of 25-bit midpoint significands fit in 128 bits. Results are always normal,
// even for the smallest input subnormal.
static int compare_midpoint(uint32_t x, uint32_t y) {
  const uint32_t exponent = x >> 23;
  const uint32_t mantissa = (x & 0x7fffff) | (exponent ? 0x800000 : 0);
  const int input_exp = exponent ? int(exponent) - 150 : -149;
  const Wide middle = 2 * ((y & 0x7fffff) | 0x800000) + 1;
  const Wide square = middle * middle;
  Wide a = Wide(mantissa) * mantissa;
  Wide c = square * square * middle;
  const int a_length = bit_length(a), c_length = bit_length(c);
  const int a_top = a_length + 2 * input_exp;
  const int c_top = c_length + 5 * (int(y >> 23) - 151);
  if (a_top != c_top)
    return a_top < c_top ? -1 : 1;
  if (a_length < c_length)
    a <<= c_length - a_length;
  else
    c <<= a_length - c_length;
  return a < c ? -1 : a > c ? 1 : 0;
}

static uint32_t reference(uint32_t x) {
  if (!x)
    return 0;
  // libm supplies only a starting estimate; exact integer comparisons decide
  // rounding, including ties to even. No dependence on its accuracy remains.
  uint32_t y = bits(float(std::pow(double(value(x)), 0.4)));
  int cmp = compare_midpoint(x, y);
  while (cmp > 0 || (cmp == 0 && (y & 1)))
    cmp = compare_midpoint(x, ++y);
  cmp = compare_midpoint(x, y - 1);
  while (cmp < 0 || (cmp == 0 && (y & 1))) {
    --y;
    cmp = compare_midpoint(x, y - 1);
  }
  return y;
}

int main(int argc, char **argv) {
  const bool exhaustive = argc == 2 && !std::strcmp(argv[1], "--exhaustive");
  if (argc > 1 && !exhaustive)
    return 2;
  uint64_t count = 0, over_four = 0, boundary_failures = 0;
  unsigned max_error = 0;
  const uint64_t stride = exhaustive ? 1 : 8191;
#pragma omp parallel for num_threads(8) reduction(+:count,over_four,boundary_failures) reduction(max:max_error)
  for (uint64_t word = 1; word < 0x7f800000; word += stride) {
    const uint32_t expected = reference(word), actual = pow04(word);
    const int64_t error = int64_t(actual) - expected;
    const unsigned distance = error < 0 ? -error : error;
    max_error = std::max(max_error, distance);
    over_four += distance > 4;
    ++count;
    // A two-representable-step error implies <4 mathematical ULP except
    // possibly just below a binade boundary. Exclude that case exactly.
    if (error == 2 && (expected & 0x7fffff) == 0) {
      const uint32_t exponent = word >> 23;
      const uint32_t mantissa = (word & 0x7fffff) | (exponent ? 0x800000 : 0);
      const int input_exp = exponent ? int(exponent) - 150 : -149;
      boundary_failures += bit_length(Wide(mantissa) * mantissa) - 1 +
                               2 * input_exp < 5 * (int(expected >> 23) - 127);
    }
  }
  for (uint32_t exponent = 1; exponent < 255; ++exponent) {
    for (int delta = -1; delta <= 1; ++delta) {
      const uint32_t word = (exponent << 23) + delta;
      const uint32_t expected = reference(word), actual = pow04(word);
      const int64_t error = int64_t(actual) - expected;
      const unsigned distance = error < 0 ? -error : error;
      max_error = std::max(max_error, distance);
      over_four += distance > 4;
    }
  }
  std::printf("{\"sweep_cases\":%llu,\"max_representable_steps\":%u,"
              "\"over_four\":%llu,\"binade_boundary_failures\":%llu,"
              "\"zero_exact\":%s}\n",
              (unsigned long long)count, max_error,
              (unsigned long long)over_four,
              (unsigned long long)boundary_failures,
              pow04(0) == 0 ? "true" : "false");
  return over_four || boundary_failures || max_error > 2 || pow04(0) != 0;
}
