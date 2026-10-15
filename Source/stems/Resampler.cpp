#include "stems/Resampler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chop::stems::detail {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kZeroCrossings = 32;
constexpr int kPhasesPerCrossing = 4096;
constexpr double kBeta = 10.0;
constexpr double kRolloff = 0.95;
constexpr double kMaxRatio = 256.0;            // 768 kHz <-> 3 kHz; real rate pairs are far inside
constexpr long long kMaxPolyTaps = 1LL << 22;  // 16 MB of polyphase table at most

double besselI0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}
} // namespace

SincResampler::SincResampler(double inRate, double outRate)
{
    // The kernel widens with the downsampling ratio and the polyphase table with the reduced rate
    // ratio, so absurd rates (from a damaged file header, say) would ask for gigabytes.
    if (!std::isfinite(inRate) || !std::isfinite(outRate) || inRate <= 0.0 || outRate <= 0.0 ||
        inRate / outRate > kMaxRatio || outRate / inRate > kMaxRatio)
        throw std::invalid_argument("SincResampler: unsupported sample rates");
    step_ = inRate / outRate;
    cutoff_ = 0.5 * std::min(1.0, outRate / inRate) * kRolloff;
    halfWidth_ = kZeroCrossings / (2.0 * cutoff_);

    const int n = kZeroCrossings * kPhasesPerCrossing;
    table_.resize((size_t)n + 2);
    const double i0b = besselI0(kBeta);
    for (int i = 0; i <= n + 1; ++i) {
        const double x = halfWidth_ * double(i) / n; // input-sample units
        const double r = std::min(1.0, x / halfWidth_);
        const double win = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        const double a = 2.0 * cutoff_ * x;
        const double sinc = a == 0.0 ? 1.0 : std::sin(kPi * a) / (kPi * a);
        table_[(size_t)i] = float(2.0 * cutoff_ * sinc * (i > n ? 0.0 : win));
    }
    tableScale_ = n / halfWidth_;

    // exact polyphase filters when both rates are integers with a small ratio
    const double ir = std::round(inRate), orr = std::round(outRate);
    if (std::abs(ir - inRate) < 1e-9 && std::abs(orr - outRate) < 1e-9 && ir > 0 && orr > 0) {
        long a = (long)ir, b = (long)orr;
        while (b) { const long t = a % b; a = b; b = t; }
        const long g = a;
        const long phases = (long)orr / g;
        half_ = (int)std::ceil(halfWidth_);
        taps_ = 2 * half_;
        if (phases <= 4096 && (long long)phases * taps_ <= kMaxPolyTaps) {
            inRateInt_ = (long)ir;
            outRateInt_ = (long)orr;
            gcd_ = g;
            phases_ = (int)phases;
            poly_.assign((size_t)phases_ * taps_, 0.0f);
            for (int p = 0; p < phases_; ++p) {
                const double frac = double(p) / phases_;
                for (int j = 0; j < taps_; ++j) {
                    const int m = j - half_ + 1; // input offset from floor(t)
                    poly_[(size_t)p * taps_ + j] = float(exactKernel(frac - m));
                }
            }
        }
    }
}

double SincResampler::exactKernel(double x) const
{
    x = std::abs(x);
    if (x >= halfWidth_)
        return 0.0;
    const double r = x / halfWidth_;
    const double win = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(kBeta);
    const double a = 2.0 * cutoff_ * x;
    const double sinc = a == 0.0 ? 1.0 : std::sin(kPi * a) / (kPi * a);
    return 2.0 * cutoff_ * sinc * win;
}

float SincResampler::kernel(double x) const
{
    const double p = std::abs(x) * tableScale_;
    const size_t i = (size_t)p;
    if (i + 1 >= table_.size())
        return 0.0f;
    const float f = float(p - double(i));
    return table_[i] + f * (table_[i + 1] - table_[i]);
}

double SincResampler::inputReach() const
{
    if (std::abs(step_ - 1.0) < 1e-12)
        return 0.0;
    if (phases_ > 0)
        return (double)half_ + 1.0;
    return halfWidth_ + 1.0;
}

void SincResampler::process(const float* in, int inLength, float* out, int outLength) const
{
    processRange(in, inLength, out, 0, outLength);
}

void SincResampler::processRange(const float* in, int inLength, float* out, int outStart, int outEnd) const
{
    if (std::abs(step_ - 1.0) < 1e-12) {
        for (int n = outStart; n < outEnd; ++n)
            out[n - outStart] = n < inLength ? in[n] : 0.0f;
        return;
    }
    if (phases_ > 0) {
        for (int n = outStart; n < outEnd; ++n) {
            const long long num = (long long)n * inRateInt_;
            const long long base = num / outRateInt_;
            const int phase = (int)((num % outRateInt_) / gcd_);
            if (base - half_ + 1 > inLength) {
                out[n - outStart] = 0.0f;
                continue;
            }
            const long long first = base - half_ + 1; // input index of tap 0
            const float* h = poly_.data() + (size_t)phase * taps_;
            float acc = 0.0f;
            if (first >= 0 && first + taps_ <= inLength) {
                const float* x = in + first;
                for (int j = 0; j < taps_; ++j)
                    acc += x[j] * h[j];
            } else {
                // taps outside the input read zeros (as a zero-padded copy would)
                for (int j = 0; j < taps_; ++j) {
                    const long long k = first + j;
                    const float x = (k >= 0 && k < inLength) ? in[k] : 0.0f;
                    acc += x * h[j];
                }
            }
            out[n - outStart] = acc;
        }
        return;
    }
    for (int n = outStart; n < outEnd; ++n) {
        const double t = n * step_;
        const long first = std::max(0L, (long)std::ceil(t - halfWidth_));
        const long last = std::min((long)inLength - 1, (long)std::floor(t + halfWidth_));
        double acc = 0.0;
        for (long k = first; k <= last; ++k)
            acc += double(in[k]) * kernel(t - double(k));
        out[n - outStart] = float(acc);
    }
}

} // namespace chop::stems::detail
