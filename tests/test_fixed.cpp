// 16.16 fixed point (src/engine/fixed.h) against double: arithmetic, rounding, floor/ceil/round, sin/cos from the LUT,
// sqrt, angle wrapping and IEEE float bits. Prints the worst error of each in raw steps (1/65536).
//   test_fixed.exe
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/fixed.h"

using namespace cr;

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what);
    }
}

static uint32_t s = 12345;
static double rnd01()
{
    s = s * 1664525u + 1013904223u;
    return double(s >> 8) / 16777216.0;
}

static Fixed fx(double d) { return Fixed::fromRaw(int32_t(std::lround(d * 65536.0))); }
static double err(Fixed f, double d) { return std::fabs(double(f.v) - d * 65536.0); }

int main()
{
    // construction
    check(Fixed(3).v == 3 * 65536 && Fixed(-2).v == -2 * 65536, "int construction exact");
    check(Fixed(0.5).v == 32768 && Fixed(-0.25).v == -16384 && Fixed(0.1).v == 6554, "literal construction rounds");
    check(int(Fixed(2.9)) == 2 && int(Fixed(-2.9)) == -2, "int() truncates toward zero");

    double worstMul = 0, worstDiv = 0, worstAdd = 0;
    for (int i = 0; i < 200000; i++) {
        const double a = (rnd01() - 0.5) * 200, b = (rnd01() - 0.5) * 200;
        const Fixed fa = fx(a), fb = fx(b);
        const double da = fa.toDouble(), db = fb.toDouble(); // compare against the same quantised inputs
        worstAdd = std::max(worstAdd, err(fa + fb, da + db));
        if (std::fabs(da * db) < 30000) worstMul = std::max(worstMul, err(fa * fb, da * db));
        if (std::fabs(db) > 0.01 && std::fabs(da / db) < 30000) worstDiv = std::max(worstDiv, err(fa / fb, da / db));
    }
    std::printf("worst raw error: add %.2f mul %.2f div %.2f\n", worstAdd, worstMul, worstDiv);
    check(worstAdd == 0, "add exact");
    check(worstMul <= 0.5, "mul rounds to nearest");
    check(worstDiv <= 0.5, "div rounds to nearest");
    check((Fixed(5) / Fixed(0)).v == 0, "division by zero gives 0");

    // floor / ceil / JS round
    bool floorsOk = true;
    for (int i = -300000; i <= 300000; i += 7) {
        const Fixed f = Fixed::fromRaw(i);
        const double d = f.toDouble();
        if (fixedFloor(f).toDouble() != std::floor(d)) floorsOk = false;
        if (fixedCeil(f).toDouble() != std::ceil(d)) floorsOk = false;
        if (fixedRound(f).toDouble() != std::floor(d + 0.5)) floorsOk = false;
        if (fixedAbs(f).toDouble() != std::fabs(d)) floorsOk = false;
    }
    check(floorsOk, "floor, ceil, JS round and abs exact on the grid");

    // sin / cos
    double worstSin = 0, worstCos = 0;
    for (int i = -400000; i <= 400000; i += 13) {
        const Fixed a = Fixed::fromRaw(i * 5); // about +-30 radians
        const double d = a.toDouble();
        worstSin = std::max(worstSin, err(fixedSin(a), std::sin(d)));
        worstCos = std::max(worstCos, err(fixedCos(a), std::cos(d)));
    }
    std::printf("worst raw error: sin %.2f cos %.2f\n", worstSin, worstCos);
    check(worstSin <= 2.5 && worstCos <= 2.5, "sin/cos from the LUT within 2.5/65536");
    check(fixedSin(Fixed(0)).v == 0 && std::abs(fixedCos(Fixed(0)).v - 65536) <= 1, "sin 0, cos 0");

    // sqrt
    double worstSqrt = 0;
    for (int i = 0; i < 100000; i++) {
        const Fixed a = fx(rnd01() * 30000);
        worstSqrt = std::max(worstSqrt, err(fixedSqrt(a), std::sqrt(a.toDouble())));
    }
    std::printf("worst raw error: sqrt %.2f\n", worstSqrt);
    check(worstSqrt <= 1, "sqrt within 1/65536");
    check(fixedSqrt(Fixed(0)).v == 0 && fixedSqrt(Fixed(-1)).v == 0, "sqrt of 0 and negatives");

    // Math.atan2(sin a, cos a)
    double worstAngle = 0;
    for (int i = -200000; i <= 200000; i += 11) {
        const Fixed a = Fixed::fromRaw(i * 7);
        const double d = a.toDouble();
        const double expect = std::atan2(std::sin(d), std::cos(d));
        const double twoPi = 2 * 3.141592653589793;
        double e = err(fixedNormalizeAngle(a), expect);
        const double up = err(fixedNormalizeAngle(a), expect + twoPi); // pi and -pi are the same direction
        const double down = err(fixedNormalizeAngle(a), expect - twoPi);
        if (up < e) e = up;
        if (down < e) e = down;
        if (e > worstAngle) worstAngle = e;
    }
    std::printf("worst raw error: normalizeAngle %.2f\n", worstAngle);
    check(worstAngle <= 4, "angle wrap within 4/65536 (2 pi is 411775.4 raw)");

    // IEEE float bits
    double worstBits = 0;
    const float samples[] = {0.0f, 1.0f, -1.0f, 0.4f, -0.35f, 1e-6f, 123.456f, -2345.5f, 32767.0f, 0.5f, 3.0f};
    for (float f : samples) worstBits = std::max(worstBits, err(fixedFromFloat(f), double(f)));
    for (int i = 0; i < 100000; i++) {
        const float f = float((rnd01() - 0.5) * 60000);
        worstBits = std::max(worstBits, err(fixedFromFloat(f), double(f)));
    }
    std::printf("worst raw error: float bits %.2f\n", worstBits);
    check(worstBits <= 0.5, "float bits to 16.16 rounds to nearest");
    check(fixedFromFloat(1e9f).v == 0x7fffffff && fixedFromFloat(-1e9f).v == -0x7fffffff, "float bits saturate");

    std::printf("test_fixed: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
