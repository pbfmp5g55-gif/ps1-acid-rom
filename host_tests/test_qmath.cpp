// test_qmath.cpp — Verify Q24 LUT sin / cos / exp / tanh / pow2 against
// std::cmath on the host. The same qmath.hpp will run on the PS1 mips
// build, where it has to produce bit-exact identical i32 outputs.
//
// Acceptance per the ACB_LIVE_RENDER design doc (Phase 1):
//   sin / cos: |err| ≤ 0.001 (-60 dB)
//   exp:       |err| ≤ 0.005
//   tanh:      |err| ≤ 0.003
//   pow2:      |err| ≤ 0.0005

#include "dsp/qmath.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using acid::dsp::i32;
constexpr int Q = acid::dsp::Q24_SHIFT;
constexpr double SCALE = static_cast<double>(1 << Q);

inline double q24_to_d(i32 x) {
    return static_cast<double>(x) / SCALE;
}

bool check_sin() {
    constexpr double TOL = 0.001;
    double maxErr = 0.0;
    // Sample one full cycle.
    for (int i = 0; i < 2048; ++i) {
        double phase = (static_cast<double>(i) / 2048.0) * 2.0 * M_PI;
        uint32_t phaseU = static_cast<uint32_t>(
            (phase / (2.0 * M_PI)) * 4294967296.0);
        double got = q24_to_d(acid::dsp::sin_q24(phaseU));
        double ref = std::sin(phase);
        double err = std::abs(got - ref);
        if (err > maxErr) maxErr = err;
    }
    std::printf("sin  max err = %.6f (tol %.6f)\n", maxErr, TOL);
    return maxErr <= TOL;
}

bool check_cos() {
    constexpr double TOL = 0.001;
    double maxErr = 0.0;
    for (int i = 0; i < 2048; ++i) {
        double phase = (static_cast<double>(i) / 2048.0) * 2.0 * M_PI;
        uint32_t phaseU = static_cast<uint32_t>(
            (phase / (2.0 * M_PI)) * 4294967296.0);
        double got = q24_to_d(acid::dsp::cos_q24(phaseU));
        double ref = std::cos(phase);
        double err = std::abs(got - ref);
        if (err > maxErr) maxErr = err;
    }
    std::printf("cos  max err = %.6f (tol %.6f)\n", maxErr, TOL);
    return maxErr <= TOL;
}

bool check_exp() {
    constexpr double TOL = 0.005;
    double maxErr = 0.0;
    // Test range x ∈ [-8, -0.01] — we explicitly skip the upper boundary
    // because the LUT covers [-8, 0) and the i32 representation of 0 maps
    // to LUT index LUT_SIZE-1 with a special extrapolation rule. Inside
    // the real audio path env decay never reaches 0 anyway.
    for (int i = 0; i < 800; ++i) {
        double x = -8.0 + (static_cast<double>(i) / 800.0) * 7.99;
        i32 xq = static_cast<i32>(x * SCALE);
        double got = q24_to_d(acid::dsp::exp_q24(xq));
        double ref = std::exp(x);
        double err = std::abs(got - ref);
        if (err > maxErr) maxErr = err;
    }
    std::printf("exp  max err = %.6f (tol %.6f)\n", maxErr, TOL);
    return maxErr <= TOL;
}

bool check_tanh() {
    constexpr double TOL = 0.003;
    double maxErr = 0.0;
    for (int i = 0; i < 800; ++i) {
        double x = -4.0 + (static_cast<double>(i) / 800.0) * 8.0;
        i32 xq = static_cast<i32>(x * SCALE);
        double got = q24_to_d(acid::dsp::tanh_q24(xq));
        double ref = std::tanh(x);
        double err = std::abs(got - ref);
        if (err > maxErr) maxErr = err;
    }
    std::printf("tanh max err = %.6f (tol %.6f)\n", maxErr, TOL);
    return maxErr <= TOL;
}

bool check_pow2() {
    constexpr double TOL = 0.0005;
    double maxErr = 0.0;
    for (int i = 0; i < 800; ++i) {
        double x = (static_cast<double>(i) / 800.0) * 0.999;
        i32 xq = static_cast<i32>(x * SCALE);
        double got = q24_to_d(acid::dsp::pow2_unit_q24(xq));
        double ref = std::pow(2.0, x);
        double err = std::abs(got - ref);
        if (err > maxErr) maxErr = err;
    }
    std::printf("pow2 max err = %.6f (tol %.6f)\n", maxErr, TOL);
    return maxErr <= TOL;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= check_sin();
    ok &= check_cos();
    ok &= check_exp();
    ok &= check_tanh();
    ok &= check_pow2();
    if (!ok) {
        std::fprintf(stderr, "FAIL: qmath LUT exceeds tolerance\n");
        return EXIT_FAILURE;
    }
    std::puts("OK: all qmath LUTs within tolerance");
    return EXIT_SUCCESS;
}
