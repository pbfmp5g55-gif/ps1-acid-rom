// qmath.hpp — Q24 integer LUT versions of sin/cos/exp/tanh/pow2.
//
// PS1 has no FPU and the psyqo / nugget build is `-nostdlib -ffreestanding`
// so `std::sin`/`std::exp` link errors. ACB live render needs these inside
// the audio IRQ at 22050 Hz — fast lookup beats anything we could compute
// from first principles per-sample.
//
// LUTs are populated at compile time via constexpr Taylor expansion. The
// values are bit-exact between host_tests and the mips build because the
// compiler (g++) evaluates them on the host before code generation.
//
// Phase representation: a uint32_t `phase` covers a full cycle 0..2π.
// 0x00000000 → 0, 0x40000000 → π/2, 0x80000000 → π, 0xC0000000 → 3π/2.
// Adding constants to `phase` lets you increment by an arbitrary frequency
// each sample without floating-point.

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

namespace qmath_detail {

constexpr int LUT_BITS = 8;
constexpr int LUT_SIZE = 1 << LUT_BITS;
constexpr int LUT_MASK = LUT_SIZE - 1;

constexpr int TANH_BITS = 7;
constexpr int TANH_SIZE = 1 << TANH_BITS;

constexpr double PI = 3.14159265358979323846;
constexpr double LN2 = 0.69314718055994530941723212145818;

// Taylor expansions evaluated at compile time. We use `double` here only
// inside constexpr; the resulting LUT entries are i32 — no runtime float.
constexpr double taylor_sin(double x) {
    double r = 0.0;
    double term = x;
    for (int n = 1; n <= 19; n += 2) {
        r += term;
        term = -term * x * x / static_cast<double>((n + 1) * (n + 2));
    }
    return r;
}

constexpr double taylor_exp_small(double x) {
    // Assumes |x| ≤ 1. Taylor converges fast in this range — 14 terms is
    // plenty for double precision.
    double r = 0.0;
    double term = 1.0;
    for (int n = 0; n <= 16; ++n) {
        r += term;
        term = term * x / static_cast<double>(n + 1);
    }
    return r;
}

constexpr double taylor_exp(double x) {
    // Recursive squaring: exp(x) = exp(x/2)^2. Each halving brings x closer
    // to 0 where Taylor convergence is fast. exp(-8) at single Taylor expansion
    // had ~1.7% error; this brings it down to <1e-6.
    if (x < -1.0) {
        double h = taylor_exp(x / 2.0);
        return h * h;
    }
    if (x > 1.0) {
        double h = taylor_exp(x / 2.0);
        return h * h;
    }
    return taylor_exp_small(x);
}

constexpr double taylor_tanh(double x) {
    double ex  = taylor_exp(x);
    double enx = taylor_exp(-x);
    return (ex - enx) / (ex + enx);
}

struct SinTable  { i32 e[LUT_SIZE]; };
struct ExpTable  { i32 e[LUT_SIZE]; };
struct TanhTable { i32 e[TANH_SIZE]; };
struct Pow2Table { i32 e[LUT_SIZE]; };

constexpr SinTable make_sin_table() {
    SinTable t{};
    for (int i = 0; i < LUT_SIZE; ++i) {
        // We store one quarter period (0..π/2) and reconstruct the rest by
        // symmetry. Use the cell midpoint for slightly better average error
        // when linear-interpolating between neighbours.
        double phase = (static_cast<double>(i) / static_cast<double>(LUT_SIZE))
                       * (PI / 2.0);
        double v = taylor_sin(phase);
        t.e[i] = static_cast<i32>(v * (1 << Q24_SHIFT));
    }
    return t;
}

constexpr ExpTable make_exp_table() {
    ExpTable t{};
    // Range x ∈ [-8, 0] covers the env-decay area we care about: exp(-8) ≈ 3e-4.
    for (int i = 0; i < LUT_SIZE; ++i) {
        double x = -8.0 + (static_cast<double>(i) / static_cast<double>(LUT_SIZE)) * 8.0;
        double v = taylor_exp(x);
        t.e[i] = static_cast<i32>(v * (1 << Q24_SHIFT));
    }
    return t;
}

constexpr TanhTable make_tanh_table() {
    TanhTable t{};
    // Range x ∈ [0, 4]. tanh(4) ≈ 0.9993. Beyond that we clamp to ±1.
    for (int i = 0; i < TANH_SIZE; ++i) {
        double x = (static_cast<double>(i) / static_cast<double>(TANH_SIZE)) * 4.0;
        double v = taylor_tanh(x);
        t.e[i] = static_cast<i32>(v * (1 << Q24_SHIFT));
    }
    return t;
}

constexpr Pow2Table make_pow2_table() {
    Pow2Table t{};
    // Fractional octave x ∈ [0, 1]. pow2(x) = exp(x * ln2).
    for (int i = 0; i < LUT_SIZE; ++i) {
        double x = static_cast<double>(i) / static_cast<double>(LUT_SIZE);
        double v = taylor_exp(x * LN2);
        t.e[i] = static_cast<i32>(v * (1 << Q24_SHIFT));
    }
    return t;
}

inline constexpr SinTable  SIN_TABLE  = make_sin_table();
inline constexpr ExpTable  EXP_TABLE  = make_exp_table();
inline constexpr TanhTable TANH_TABLE = make_tanh_table();
inline constexpr Pow2Table POW2_TABLE = make_pow2_table();

}  // namespace qmath_detail

// ---- Public API ----------------------------------------------------------

// sin_q24(phase): returns sin(phase) in Q24. `phase` is uint32_t where the
// full uint32 range represents one full cycle (0..2π). Wraps naturally.
inline i32 sin_q24(uint32_t phase) {
    using namespace qmath_detail;
    // Strip sign (full range = 0..2π, halve to 0..π by negate, halve again
    // to 0..π/2 by mirror).
    bool neg = (phase & 0x80000000u) != 0;
    uint32_t p = phase & 0x7FFFFFFFu;  // 0..π
    bool mirror = (p & 0x40000000u) != 0;
    if (mirror) p = 0x80000000u - p;  // map [π/2, π] to [0, π/2]
    // Now p ∈ [0, 0x40000000]. LUT covers 256 cells over [0, π/2). When p
    // lands exactly on π/2 (idx becomes LUT_SIZE), clamp to the last cell
    // with maxed-out frac so the interpolation drives the result towards
    // the natural sin(π/2)=1 endpoint stored as (1<<Q24_SHIFT).
    constexpr int FRAC_BITS = 30 - LUT_BITS;
    uint32_t idx  = p >> FRAC_BITS;
    uint32_t frac = p & ((1u << FRAC_BITS) - 1u);
    if (idx >= LUT_SIZE) {
        idx  = LUT_SIZE - 1;
        frac = (1u << FRAC_BITS) - 1u;
    }
    i32 a = SIN_TABLE.e[idx];
    i32 b = (idx + 1 < LUT_SIZE) ? SIN_TABLE.e[idx + 1]
                                  : (1 << Q24_SHIFT);  // sin(π/2)=1
    i32 r = a + static_cast<i32>(
        (static_cast<i64>(b - a) * static_cast<i64>(frac)) >> FRAC_BITS);
    return neg ? -r : r;
}

// cos_q24 = sin(phase + π/2). Adding 0x40000000 advances by π/2.
inline i32 cos_q24(uint32_t phase) {
    return sin_q24(phase + 0x40000000u);
}

// exp_q24(xQ24): returns exp(x) in Q24 for x in [-8, 0]. Clamps outside.
inline i32 exp_q24(i32 xQ24) {
    using namespace qmath_detail;
    constexpr i32 X_MIN_Q24 = -(8 << Q24_SHIFT);
    if (xQ24 <= X_MIN_Q24) return EXP_TABLE.e[0];
    if (xQ24 >= 0)         return EXP_TABLE.e[LUT_SIZE - 1] +
                                   (((1 << Q24_SHIFT) - EXP_TABLE.e[LUT_SIZE - 1]));
    // x ∈ [-8, 0]. Map to LUT index [0, LUT_SIZE).
    // unit = (x + 8) / 8, in Q24: unit = (xQ24 + 8*ONE) / 8 = (xQ24 + 8*ONE) >> 3
    i32 shifted = (xQ24 + (8 << Q24_SHIFT)) >> 3;  // Q24, range [0, ONE)
    // top LUT_BITS = index, rest = interp fraction
    constexpr int FRAC_BITS = Q24_SHIFT - LUT_BITS;
    uint32_t idx  = (static_cast<uint32_t>(shifted) >> FRAC_BITS) & LUT_MASK;
    uint32_t frac = static_cast<uint32_t>(shifted) & ((1u << FRAC_BITS) - 1u);
    i32 a = EXP_TABLE.e[idx];
    i32 b = (idx + 1 < LUT_SIZE) ? EXP_TABLE.e[idx + 1] : (1 << Q24_SHIFT);
    return a + static_cast<i32>(
        (static_cast<i64>(b - a) * static_cast<i64>(frac)) >> FRAC_BITS);
}

// tanh_q24(xQ24): returns tanh(x) in Q24. Antisymmetric.
inline i32 tanh_q24(i32 xQ24) {
    using namespace qmath_detail;
    bool neg = xQ24 < 0;
    i32 ax = neg ? -xQ24 : xQ24;
    constexpr i32 X_MAX_Q24 = (4 << Q24_SHIFT);
    if (ax >= X_MAX_Q24) return neg ? -(1 << Q24_SHIFT) : (1 << Q24_SHIFT);
    // ax ∈ [0, 4]. Map to LUT index [0, TANH_SIZE).
    constexpr int FRAC_BITS = (Q24_SHIFT + 2) - TANH_BITS;  // ax range is 4*ONE
    uint32_t idx  = (static_cast<uint32_t>(ax) >> FRAC_BITS) & (TANH_SIZE - 1);
    uint32_t frac = static_cast<uint32_t>(ax) & ((1u << FRAC_BITS) - 1u);
    i32 a = TANH_TABLE.e[idx];
    i32 b = (idx + 1 < TANH_SIZE) ? TANH_TABLE.e[idx + 1] : (1 << Q24_SHIFT);
    i32 r = a + static_cast<i32>(
        (static_cast<i64>(b - a) * static_cast<i64>(frac)) >> FRAC_BITS);
    return neg ? -r : r;
}

// pow2_unit_q24(fracQ24): returns 2^x in Q24 for x in [0, 1).
// Used to derive a frequency ratio inside one octave (combine with integer
// shifts for whole octaves).
inline i32 pow2_unit_q24(i32 fracQ24) {
    using namespace qmath_detail;
    // Clamp to [0, ONE).
    if (fracQ24 <= 0) return (1 << Q24_SHIFT);
    if (fracQ24 >= (1 << Q24_SHIFT)) return (2 << Q24_SHIFT);
    constexpr int FRAC_BITS = Q24_SHIFT - LUT_BITS;
    uint32_t idx  = (static_cast<uint32_t>(fracQ24) >> FRAC_BITS) & LUT_MASK;
    uint32_t frac = static_cast<uint32_t>(fracQ24) & ((1u << FRAC_BITS) - 1u);
    i32 a = POW2_TABLE.e[idx];
    i32 b = (idx + 1 < LUT_SIZE) ? POW2_TABLE.e[idx + 1] : (2 << Q24_SHIFT);
    return a + static_cast<i32>(
        (static_cast<i64>(b - a) * static_cast<i64>(frac)) >> FRAC_BITS);
}

}  // namespace acid::dsp
