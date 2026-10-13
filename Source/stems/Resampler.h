#pragma once
// Band-limited resampler for the stems engine: Kaiser-windowed sinc
// (beta 10, ~100 dB stopband), 32 zero crossings per side at the cutoff,
// passband to 95 % of the lower Nyquist. Kernel is tabulated at 4096 phases
// per zero crossing with linear interpolation (table error below -130 dB).
// Integer rate pairs with a small reduced ratio (44.1k <-> 48k, 88.2k,
// 96k, 22.05k ...) use exact precomputed polyphase filters; other ratios
// interpolate the tabulated kernel. Output length is chosen by the caller.

#include <vector>

namespace chop::stems::detail {

class SincResampler {
public:
    SincResampler(double inRate, double outRate);

    // out[n] = x(n * inRate / outRate), samples outside [0, inLength) are 0.
    void process(const float* in, int inLength, float* out, int outLength) const;

    // The same for output samples [outStart, outEnd) only, written to
    // out[0 .. outEnd - outStart); process() is this over the whole range,
    // so the samples are the same. Only input samples within inputReach()
    // of the requested outputs are read.
    void processRange(const float* in, int inLength, float* out, int outStart, int outEnd) const;

    // Output sample n reads input samples k with
    // |k - n * inRate / outRate| <= inputReach().
    double inputReach() const;

private:
    double step_ = 1.0;      // input samples per output sample
    double cutoff_ = 0.5;    // cycles per input sample
    double halfWidth_ = 0.0; // kernel half width in input samples
    double tableScale_ = 0.0;
    std::vector<float> table_; // kernel sampled over [0, halfWidth]
    float kernel(double x) const;
    double exactKernel(double x) const;

    // polyphase path
    long inRateInt_ = 0, outRateInt_ = 0, gcd_ = 1;
    int phases_ = 0, taps_ = 0, half_ = 0;
    std::vector<float> poly_; // [phase][taps_]
};

} // namespace chop::stems::detail
