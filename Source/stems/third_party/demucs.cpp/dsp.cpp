#include "dsp.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unsupported/Eigen/FFT>
#include <vector>
#if defined(__APPLE__) && defined(EIGEN_USE_BLAS)
// Flayr Chop: on Apple platforms (Accelerate already linked for BLAS) the
// STFT / iSTFT use vDSP's real FFT, several times faster than kissfft. Same
// transforms and scaling; results differ only by FFT rounding (~1e-7).
#include <vecLib/vDSP.h>
#define FLAYR_DSP_VDSP 1
#endif

// Flayr Chop: file-local helpers are static (the engine is compiled once per
// instruction set; global names would collide at link time).

// forward declaration of inner stft
static void stft_inner(struct demucscpp::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg);

static void istft_inner(struct demucscpp::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg);

// reflect padding
static void pad_signal(struct demucscpp::stft_buffers &stft_buf)
{
    // copy from stft_buf.padded_waveform_mono_in+pad into stft_buf.pad_start,
    // stft_buf.pad_end
    std::copy_n(stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad,
                stft_buf.pad, stft_buf.pad_start.begin());
    std::copy_n(stft_buf.padded_waveform_mono_in.end() - 2 * stft_buf.pad,
                stft_buf.pad, stft_buf.pad_end.begin());

    std::reverse(stft_buf.pad_start.begin(), stft_buf.pad_start.end());
    std::reverse(stft_buf.pad_end.begin(), stft_buf.pad_end.end());

    // copy stft_buf.pad_start into stft_buf.padded_waveform_mono_in
    std::copy_n(stft_buf.pad_start.begin(), stft_buf.pad,
                stft_buf.padded_waveform_mono_in.begin());

    // copy stft_buf.pad_end into stft_buf.padded_waveform_mono_in
    std::copy_n(stft_buf.pad_end.begin(), stft_buf.pad,
                stft_buf.padded_waveform_mono_in.end() - stft_buf.pad);
}

static Eigen::FFT<float> get_fft_cfg()
{
    Eigen::FFT<float> cfg;

    cfg.SetFlag(Eigen::FFT<float>::Speedy);
    // cfg.SetFlag(Eigen::FFT<float>::HalfSpectrum);
    // cfg.SetFlag(Eigen::FFT<float>::Unscaled);

    return cfg;
}

#if defined(FLAYR_DSP_VDSP)
static FFTSetup vdsp_setup()
{
    // read-only after creation, so one setup serves every worker thread;
    // intentionally never destroyed (lives as long as the process)
    static const FFTSetup setup =
        vDSP_create_fftsetup(12, kFFTRadix2); // log2(FFT_WINDOW_SIZE)
    return setup;
}

static void stft_vdsp(struct demucscpp::stft_buffers &stft_buf)
{
    constexpr int N = demucscpp::FFT_WINDOW_SIZE, H = N / 2;
    static_assert(N == 4096, "vDSP setup is for 4096-point FFTs");
    const FFTSetup setup = vdsp_setup();
    std::vector<float> re(H), im(H);
    DSPSplitComplex z{re.data(), im.data()};
    // vDSP's forward real FFT returns 2 * DFT; upstream scales by 1/sqrt(N)
    const float s = 0.5f / std::sqrt(float(N));
    float *win = stft_buf.windowed_waveform_mono.data();
    for (int channel = 0; channel < 2; ++channel)
    {
        Eigen::VectorXf row_vec = stft_buf.waveform.row(channel);
        std::copy_n(row_vec.data(), row_vec.size(),
                    stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad);
        pad_signal(stft_buf);
        const float *in = stft_buf.padded_waveform_mono_in.data();
        for (int j = 0; j < stft_buf.nb_frames; ++j)
        {
            const float *frame = in + (size_t)j * demucscpp::FFT_HOP_SIZE;
            for (int i = 0; i < N; ++i)
                win[i] = frame[i] * stft_buf.window[i];
            vDSP_ctoz(reinterpret_cast<const DSPComplex *>(win), 2, &z, 1, H);
            vDSP_fft_zrip(setup, &z, 1, 12, FFT_FORWARD);
            stft_buf.spec(channel, 0, j) = std::complex<float>(re[0] * s, 0.0f);
            for (int k = 1; k < H; ++k)
                stft_buf.spec(channel, k, j) =
                    std::complex<float>(re[k] * s, im[k] * s);
            stft_buf.spec(channel, H, j) = std::complex<float>(im[0] * s, 0.0f);
        }
    }
}

static void istft_vdsp(struct demucscpp::stft_buffers &stft_buf)
{
    constexpr int N = demucscpp::FFT_WINDOW_SIZE, H = N / 2;
    const FFTSetup setup = vdsp_setup();
    std::vector<float> re(H), im(H);
    DSPSplitComplex z{re.data(), im.data()};
    float *win = stft_buf.windowed_waveform_mono.data();
    // upstream: Eigen's real inverse of X * sqrt(N), which comes out as the
    // unscaled inverse DFT (measured: its 1/N scaling is cancelled by the
    // half-size complex FFT it uses). vDSP's inverse is also the unscaled
    // sum, so the factor is sqrt(N) (a power of two, exact).
    const float s = std::sqrt(float(N));
    for (int channel = 0; channel < 2; ++channel)
    {
        std::fill(stft_buf.padded_waveform_mono_out.begin(),
                  stft_buf.padded_waveform_mono_out.end(), 0.0f);
        float *out = stft_buf.padded_waveform_mono_out.data();
        for (int j = 0; j < stft_buf.nb_frames; ++j)
        {
            re[0] = stft_buf.spec(channel, 0, j).real();
            im[0] = stft_buf.spec(channel, H, j).real();
            for (int k = 1; k < H; ++k)
            {
                const std::complex<float> v = stft_buf.spec(channel, k, j);
                re[k] = v.real();
                im[k] = v.imag();
            }
            vDSP_fft_zrip(setup, &z, 1, 12, FFT_INVERSE);
            vDSP_ztoc(&z, 1, reinterpret_cast<DSPComplex *>(win), 2, H);
            const int start = j * demucscpp::FFT_HOP_SIZE;
            for (int i = 0; i < N; ++i)
            {
                const float w = win[i] * s;
                out[start + i] += w * stft_buf.window[i] * 1.0f / float(N) /
                                  (stft_buf.normalized_window[start + i] + 1e-8f);
            }
        }
        stft_buf.waveform.row(channel) = Eigen::Map<Eigen::MatrixXf>(
            out + stft_buf.pad, 1,
            stft_buf.padded_waveform_mono_out.size() - N);
    }
}
#endif

void demucscpp::stft(struct stft_buffers &stft_buf)
{
#if defined(FLAYR_DSP_VDSP)
    stft_vdsp(stft_buf);
    return;
#endif
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        Eigen::VectorXf row_vec = stft_buf.waveform.row(channel);

        std::copy_n(row_vec.data(), row_vec.size(),
                    stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad);

        // apply padding equivalent to center padding with center=True
        // in torch.stft:
        // https://pytorch.org/docs/stable/generated/torch.stft.html

        // reflect pads stft_buf.padded_waveform_mono in-place
        pad_signal(stft_buf);

        // does forward fft on stft_buf.padded_waveform_mono, stores spectrum in
        // complex_spec_mono
        stft_inner(stft_buf, cfg);

        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                stft_buf.spec(channel, i, j) = stft_buf.complex_spec_mono[j][i];
            }
        }
    }
}

void demucscpp::istft(struct stft_buffers &stft_buf)
{
#if defined(FLAYR_DSP_VDSP)
    istft_vdsp(stft_buf);
    return;
#endif
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        // Populate the nested vectors
        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                stft_buf.complex_spec_mono[j][i] = stft_buf.spec(channel, i, j);
            }
        }

        // does inverse fft on stft_buf.complex_spec_mono, stores waveform in
        // padded_waveform_mono
        istft_inner(stft_buf, cfg);

        // copies waveform_mono into stft_buf.waveform past first pad samples
        stft_buf.waveform.row(channel) = Eigen::Map<Eigen::MatrixXf>(
            stft_buf.padded_waveform_mono_out.data() + stft_buf.pad, 1,
            stft_buf.padded_waveform_mono_out.size() - FFT_WINDOW_SIZE);
    }
}

static void stft_inner(struct demucscpp::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg)
{
    int frame_idx = 0;

    // Loop over the waveform with a stride of hop_size
    for (std::size_t start = 0;
         start <=
         stft_buf.padded_waveform_mono_in.size() - demucscpp::FFT_WINDOW_SIZE;
         start += demucscpp::FFT_HOP_SIZE)
    {
        // Apply window and run FFT
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE; ++i)
        {
            stft_buf.windowed_waveform_mono[i] =
                stft_buf.padded_waveform_mono_in[start + i] *
                stft_buf.window[i];
        }
        cfg.fwd(stft_buf.complex_spec_mono[frame_idx],
                stft_buf.windowed_waveform_mono);
        // now scale stft_buf.complex_spec_mono[frame_idx] by 1.0f /
        // sqrt(float(FFT_WINDOW_SIZE)))

        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                1.0f / sqrt(float(demucscpp::FFT_WINDOW_SIZE));
        }
        frame_idx++;
    }
}

static void istft_inner(struct demucscpp::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg)
{
    // clear padded_waveform_mono
    std::fill(stft_buf.padded_waveform_mono_out.begin(),
              stft_buf.padded_waveform_mono_out.end(), 0.0f);

    // Loop over the input with a stride of (hop_size)
    for (int start = 0; start < stft_buf.nb_frames * demucscpp::FFT_HOP_SIZE;
         start += demucscpp::FFT_HOP_SIZE)
    {
        int frame_idx = start / demucscpp::FFT_HOP_SIZE;
        // undo sqrt(nfft) scaling
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                sqrt(float(demucscpp::FFT_WINDOW_SIZE));
        }
        // Run iFFT
        cfg.inv(stft_buf.windowed_waveform_mono,
                stft_buf.complex_spec_mono[frame_idx]);

        // Apply window and add to output
        for (int i = 0; i < demucscpp::FFT_WINDOW_SIZE; ++i)
        {
            // x[start+i] is the sum of squared window values
            // https://github.com/librosa/librosa/blob/main/librosa/core/spectrum.py#L613
            // 1e-8f is a small number to avoid division by zero
            stft_buf.padded_waveform_mono_out[start + i] +=
                stft_buf.windowed_waveform_mono[i] * stft_buf.window[i] * 1.0f /
                float(demucscpp::FFT_WINDOW_SIZE) /
                (stft_buf.normalized_window[start + i] + 1e-8f);
        }
    }
}
