#ifndef CONV_HPP
#define CONV_HPP

// Flayr Chop: convolution kernels rewritten for speed (same semantics and
// output shapes as upstream sevagh/demucs.cpp, including its quirks).
//
// Upstream built an im2col matrix with scattered writes, multiplied it, then
// copied the result into the output tensor element by element. Here the GEMM
// is arranged so its output memory IS the output tensor (column-major
// (Cout, H, W) == matrix Cout x (H*W)), the im2col copies are contiguous
// Cin-length runs, and 1x1 convolutions skip im2col entirely. Bias and GELU
// are applied with vectorized Eigen array ops.

#include "model.hpp"
#include "tensor.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unsupported/Eigen/CXX11/Tensor>
#include <unsupported/Eigen/SpecialFunctions>

namespace demucscpp
{

// In-place exact (erf) GELU over a contiguous float buffer.
inline void gelu_inplace(float *data, Eigen::Index n)
{
    Eigen::Map<Eigen::ArrayXf> a(data, n);
    a = 0.5f * a * (1.0f + (a * 0.70710678118654752440f).erf());
}

namespace convdetail
{

// dst (cols x rows) = src (rows x cols)^T, both column-major, cache blocked.
inline void transpose_blocked(const float *src, float *dst, Eigen::Index rows,
                              Eigen::Index cols)
{
    constexpr Eigen::Index B = 32;
    for (Eigen::Index c0 = 0; c0 < cols; c0 += B)
    {
        const Eigen::Index c1 = std::min(cols, c0 + B);
        for (Eigen::Index r0 = 0; r0 < rows; r0 += B)
        {
            const Eigen::Index r1 = std::min(rows, r0 + B);
            for (Eigen::Index c = c0; c < c1; ++c)
                for (Eigen::Index r = r0; r < r1; ++r)
                    dst[c + cols * r] = src[r + rows * c];
        }
    }
}

// (d0, d1, d2) -> (d1, d2, d0): a 2D transpose of a d0 x (d1*d2) matrix
inline Eigen::Tensor3dXf move_axis0_last(const Eigen::Tensor3dXf &x)
{
    const Eigen::Index d0 = x.dimension(0), d1 = x.dimension(1),
                       d2 = x.dimension(2);
    Eigen::Tensor3dXf y(d1, d2, d0);
    if (d0 == 1)
        std::memcpy(y.data(), x.data(), sizeof(float) * x.size());
    else
        transpose_blocked(x.data(), y.data(), d0, d1 * d2);
    return y;
}

// (d0, d1, d2) -> (d2, d0, d1): a 2D transpose of a (d0*d1) x d2 matrix
inline Eigen::Tensor3dXf move_axis2_first(const Eigen::Tensor3dXf &x)
{
    const Eigen::Index d0 = x.dimension(0), d1 = x.dimension(1),
                       d2 = x.dimension(2);
    Eigen::Tensor3dXf y(d2, d0, d1);
    if (d2 == 1)
        std::memcpy(y.data(), x.data(), sizeof(float) * x.size());
    else
        transpose_blocked(x.data(), y.data(), d0 * d1, d2);
    return y;
}


// (d0, d1, d2) -> (d0, d2, d1): moves contiguous d0-runs (channel vectors),
// cache blocked over the two outer axes.
inline Eigen::Tensor3dXf swap_axes12(const Eigen::Tensor3dXf &x)
{
    const Eigen::Index d0 = x.dimension(0), d1 = x.dimension(1),
                       d2 = x.dimension(2);
    Eigen::Tensor3dXf y(d0, d2, d1);
    const float *src = x.data();
    float *dst = y.data();
    constexpr Eigen::Index B = 16;
    for (Eigen::Index j0 = 0; j0 < d2; j0 += B)
    {
        const Eigen::Index j1 = std::min(d2, j0 + B);
        for (Eigen::Index i0 = 0; i0 < d1; i0 += B)
        {
            const Eigen::Index i1 = std::min(d1, i0 + B);
            for (Eigen::Index j = j0; j < j1; ++j)
                for (Eigen::Index i = i0; i < i1; ++i)
                    std::memcpy(dst + d0 * (j + d2 * i), src + d0 * (i + d1 * j),
                                sizeof(float) * d0);
        }
    }
    return y;
}

// Flayr Chop: no weight repacking. With the im2col row order
// k * Cin + c, k = kh + KH * kw, the GEMM weight matrix Wm(o, k * Cin + c) =
// w(o, c, kh, kw) is exactly the column-major memory of the (Cout, Cin, KH,
// KW) weight tensor, and for transposed convolutions Wr(k * Cout + o, c) =
// w(c, o, kh, kw) is the transpose of the (Cin, Cout * KH * KW) view of it.
// (For the 1-D kernels every k is just kh, as upstream.)

// What happens to each block of GEMM output (after the bias):
//   kPlain  nothing
//   kGelu   exact GELU
//   kGlu    GLU over channels: out = a / (1 + exp(-g)), a / g = first /
//           second half of the channels (halves the channel count)
enum ConvEpilogue : int
{
    kPlain = 0,
    kGelu = 1,
    kGlu = 2
};

// x: (in_channels, in_height, in_width) column-major; returns
// (out_channels, out_height, out_width), or (out_channels, out_width,
// out_height) with swap_out (the two spatial axes exchanged, written
// directly instead of transposing afterwards). Epilogues and swap_out are
// applied per column block while it is in cache.
template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, int epi,
          bool swap_out>
Eigen::Tensor3dXf conv2d_any(const float *xd, int in_height, int in_width,
                             const float *wd, const float *bd)
{
    // allocated output size: upstream formula (ignores dilation)
    const int out_height =
        static_cast<int>(std::ceil(
            (float)(in_height + 2 * pad_height - (kernel_height - 1) - 1) /
            stride_height)) +
        1;
    const int out_width =
        static_cast<int>(std::ceil(
            (float)(in_width + 2 * pad_width - (kernel_width - 1) - 1) /
            stride_width)) +
        1;
    // computed size: im2col formula (with dilation). Rows past it stay zero,
    // exactly like upstream.
    const int valid_height =
        static_cast<int>(std::ceil((in_height + 2 * pad_height -
                                    dilation_height * (kernel_height - 1) - 1) /
                                   float(stride_height)) +
                         1);
    const int valid_width =
        static_cast<int>(std::ceil((in_width + 2 * pad_width -
                                    dilation_width * (kernel_width - 1) - 1) /
                                   float(stride_width)) +
                         1);

    constexpr int KK = kernel_height * kernel_width;
    constexpr int out_rows = epi == kGlu ? out_channels / 2 : out_channels;
    static_assert(epi != kGlu || out_channels % 2 == 0, "GLU needs even channels");
    Eigen::Map<const Eigen::MatrixXf> wm(wd, out_channels,
                                         (Eigen::Index)KK * in_channels);
    Eigen::Map<const Eigen::VectorXf> bias(bd, out_channels);

    const bool direct = (valid_height == out_height && valid_width == out_width);
    if ((epi == kGlu || swap_out) && !direct)
        throw std::runtime_error("conv2d: fused GLU / swap need a dense output");
    // write the GEMM straight into the output when nothing reshapes it
    const bool in_place = direct && epi != kGlu && !swap_out;

    Eigen::Tensor3dXf y_out = swap_out
                                  ? Eigen::Tensor3dXf(out_rows, out_width, out_height)
                                  : Eigen::Tensor3dXf(out_rows, out_height, out_width);
    Eigen::MatrixXf tmp; // non-direct: the valid region, padded at the end
    if (!direct)
        tmp.resize(out_channels, (Eigen::Index)valid_height * valid_width);

    constexpr bool is_pointwise =
        (KK == 1 && stride_height == 1 && stride_width == 1 &&
         pad_height == 0 && pad_width == 0);
    const Eigen::Index rows = (Eigen::Index)KK * in_channels;
    const Eigen::Index total = (Eigen::Index)valid_height * valid_width;
    // column blocks of about 1 MB so the GEMM operands stay in cache
    // (several workers share the L2 and, on Apple silicon, the matrix unit)
    const Eigen::Index chunk = std::max<Eigen::Index>(
        128, (Eigen::Index)(256 * 1024) /
                 std::max<Eigen::Index>(rows, out_channels) / 32 * 32);
    Eigen::MatrixXf col, buf;
    if (!is_pointwise)
        col.resize(rows, std::min(chunk, total));
    if (direct && !in_place)
        buf.resize(out_channels, std::min(chunk, total));
    Eigen::Map<const Eigen::MatrixXf> xm(xd, in_channels,
                                         (Eigen::Index)in_height * in_width);

    for (Eigen::Index n0 = 0; n0 < total; n0 += chunk)
    {
        const Eigen::Index nc = std::min(chunk, total - n0);
        if (!is_pointwise)
        {
            for (Eigen::Index n = n0; n < n0 + nc; ++n)
            {
                const int oh = (int)(n % valid_height);
                const int ow = (int)(n / valid_height);
                float *dst = col.data() + (n - n0) * rows;
                for (int kw = 0; kw < kernel_width; ++kw)
                {
                    const int iw =
                        ow * stride_width + kw * dilation_width - pad_width;
                    for (int kh = 0; kh < kernel_height; ++kh)
                    {
                        const int ih =
                            oh * stride_height + kh * dilation_height - pad_height;
                        float *d = dst + (kh + kernel_height * kw) * in_channels;
                        if (ih >= 0 && ih < in_height && iw >= 0 &&
                            iw < in_width)
                        {
                            std::memcpy(d,
                                        xd + (Eigen::Index)in_channels *
                                                 (ih + (Eigen::Index)in_height * iw),
                                        sizeof(float) * in_channels);
                        }
                        else
                        {
                            std::memset(d, 0, sizeof(float) * in_channels);
                        }
                    }
                }
            }
        }
        float *dst = in_place ? y_out.data() + (Eigen::Index)out_channels * n0
                     : direct ? buf.data()
                              : tmp.data() + (Eigen::Index)out_channels * n0;
        Eigen::Map<Eigen::MatrixXf> d(dst, out_channels, nc);
        if (is_pointwise)
            d.noalias() = wm * xm.middleCols(n0, nc);
        else
            d.noalias() = wm * col.leftCols(nc);
        d.colwise() += bias;
        if (epi == kGelu)
            gelu_inplace(dst, d.size());
        if (direct && !in_place)
        {
            for (Eigen::Index j = 0; j < nc; ++j)
            {
                const Eigen::Index n = n0 + j;
                const Eigen::Index pos =
                    swap_out ? (n / valid_height) + (Eigen::Index)out_width *
                                                        (n % valid_height)
                             : n;
                float *o = y_out.data() + (Eigen::Index)out_rows * pos;
                const float *src = dst + (Eigen::Index)out_channels * j;
                if constexpr (epi == kGlu)
                {
                    Eigen::Map<const Eigen::ArrayXf> a(src, out_rows);
                    Eigen::Map<const Eigen::ArrayXf> g(src + out_rows, out_rows);
                    Eigen::Map<Eigen::ArrayXf>(o, out_rows) = a / (1.0f + (-g).exp());
                }
                else
                {
                    std::memcpy(o, src, sizeof(float) * out_channels);
                }
            }
        }
    }

    if (!direct)
    {
        y_out.setZero();
        for (int ow = 0; ow < valid_width && ow < out_width; ++ow)
            for (int oh = 0; oh < valid_height && oh < out_height; ++oh)
                std::memcpy(y_out.data() + (Eigen::Index)out_channels *
                                               (oh + (Eigen::Index)out_height * ow),
                            tmp.data() + (Eigen::Index)out_channels *
                                             (oh + (Eigen::Index)valid_height * ow),
                            sizeof(float) * out_channels);
    }
    return y_out;
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, bool fuse_gelu>
Eigen::Tensor3dXf conv2d_raw(const float *xd, int in_height, int in_width,
                             const float *wd, const float *bd)
{
    return conv2d_any<in_channels, out_channels, kernel_height, kernel_width,
                      stride_height, stride_width, pad_height, pad_width,
                      dilation_height, dilation_width,
                      fuse_gelu ? kGelu : kPlain, false>(xd, in_height,
                                                         in_width, wd, bd);
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, bool fuse_gelu,
          bool swap_in = false>
Eigen::Tensor3dXf conv2d_tr_raw(const float *xd, int in_height, int in_width,
                                const float *wd, const float *bd)
{
    // swap_in: x is stored as (in_channels, in_width, in_height) (spatial
    // axes exchanged); the result is the same as for the unswapped input,
    // including the order in which overlapping kernel taps are summed.
    // every transposed conv in htdemucs has zero padding; upstream's index
    // math is only self-consistent in that case
    static_assert(pad_height == 0 && pad_width == 0,
                  "conv2d_tr: only zero padding is supported");

    constexpr int eff_kh =
        kernel_height + (kernel_height - 1) * (dilation_height - 1);
    constexpr int eff_kw =
        kernel_width + (kernel_width - 1) * (dilation_width - 1);
    const int out_height = (in_height - 1) * stride_height + eff_kh;
    const int out_width = (in_width - 1) * stride_width + eff_kw;
    constexpr int KK = kernel_height * kernel_width;

    // (Cin, Cout * KK) view of the weights; its transpose is Wr
    Eigen::Map<const Eigen::MatrixXf> wv(wd, in_channels,
                                         (Eigen::Index)KK * out_channels);

    Eigen::Map<const Eigen::MatrixXf> xm(xd, in_channels,
                                         (Eigen::Index)in_height * in_width);
    Eigen::Tensor3dXf y_out(out_channels, out_height, out_width);
    y_out.setZero();
    float *yd = y_out.data();

    // z = Wr * x in column chunks of about 1 MB, each scatter-added into the
    // output before the next chunk is computed
    const Eigen::Index zrows = (Eigen::Index)KK * out_channels;
    const Eigen::Index total = (Eigen::Index)in_height * in_width;
    const Eigen::Index chunk = std::max<Eigen::Index>(
        128, (Eigen::Index)(256 * 1024) / zrows / 32 * 32);
    Eigen::MatrixXf z(zrows, std::min(chunk, total));
    for (Eigen::Index n0 = 0; n0 < total; n0 += chunk)
    {
        const Eigen::Index nc = std::min(chunk, total - n0);
        z.leftCols(nc).noalias() = wv.transpose() * xm.middleCols(n0, nc);
        for (Eigen::Index n = n0; n < n0 + nc; ++n)
        {
            const int ih = swap_in ? (int)(n / in_width) : (int)(n % in_height);
            const int iw = swap_in ? (int)(n % in_width) : (int)(n / in_height);
            const float *zc = z.data() + (n - n0) * zrows;
            for (int kw = 0; kw < kernel_width; ++kw)
            {
                const int ow = iw * stride_width + kw * dilation_width;
                for (int kh = 0; kh < kernel_height; ++kh)
                {
                    const int oh = ih * stride_height + kh * dilation_height;
                    float *dst =
                        yd + (Eigen::Index)out_channels *
                                 (oh + (Eigen::Index)out_height * ow);
                    const float *src =
                        zc + (kh + kernel_height * kw) * out_channels;
                    Eigen::Map<Eigen::ArrayXf>(dst, out_channels) +=
                        Eigen::Map<const Eigen::ArrayXf>(src, out_channels);
                }
            }
        }
    }

    Eigen::Map<Eigen::MatrixXf> ymat(yd, out_channels,
                                     (Eigen::Index)out_height * out_width);
    Eigen::Map<const Eigen::VectorXf> bias(bd, out_channels);
    ymat.colwise() += bias;
    if (fuse_gelu)
        gelu_inplace(yd, ymat.size());
    return y_out;
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, bool fuse_gelu>
Eigen::Tensor3dXf conv2d_impl(const Eigen::Tensor3dXf &x,
                              const Eigen::Tensor4dXf &w,
                              const Eigen::Tensor1dXf &b)
{
    return conv2d_raw<in_channels, out_channels, kernel_height, kernel_width,
                      stride_height, stride_width, pad_height, pad_width,
                      dilation_height, dilation_width, fuse_gelu>(
        x.data(), (int)x.dimension(1), (int)x.dimension(2), w.data(), b.data());
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, bool fuse_gelu>
Eigen::Tensor3dXf conv2d_tr_impl(const Eigen::Tensor3dXf &x,
                                 const Eigen::Tensor4dXf &w,
                                 const Eigen::Tensor1dXf &b)
{
    return conv2d_tr_raw<in_channels, out_channels, kernel_height,
                         kernel_width, stride_height, stride_width, pad_height,
                         pad_width, dilation_height, dilation_width, fuse_gelu>(
        x.data(), (int)x.dimension(1), (int)x.dimension(2), w.data(), b.data());
}

// 1-D convolution of x (B, Cin, L) with w (Cout, Cin, K) -> (B, Cout, L').
// With B == 1 the (1, C, L) memory already is the channel-first (C, L, 1)
// image the 2-D kernels want, so no transposes are needed.
template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation, bool tr, int epi>
Eigen::Tensor3dXf conv1d_any(const Eigen::Tensor3dXf &x,
                             const Eigen::Tensor3dXf &w,
                             const Eigen::Tensor1dXf &b)
{
    const bool batch1 = x.dimension(0) == 1;
    Eigen::Tensor3dXf x_shuff;
    if (!batch1)
        x_shuff = move_axis0_last(x);
    const float *xd = batch1 ? x.data() : x_shuff.data();
    const int len = (int)x.dimension(2);
    const int batch = (int)x.dimension(0);
    Eigen::Tensor3dXf y;
    if constexpr (tr)
    {
        static_assert(epi != kGlu, "conv1d_tr: no fused GLU");
        y = conv2d_tr_raw<in_channels, out_channels, kernel_size, 1, stride, 1,
                          pad, 0, dilation, 1, epi == kGelu>(xd, len, batch,
                                                             w.data(), b.data());
    }
    else
    {
        y = conv2d_any<in_channels, out_channels, kernel_size, 1, stride, 1,
                       pad, 0, dilation, 1, epi, false>(xd, len, batch, w.data(),
                                                        b.data());
    }
    if (!batch1)
        return move_axis2_first(y);
    // (Cout, L', 1) and (1, Cout, L') share one memory layout
    const Eigen::Index c = y.dimension(0), l = y.dimension(1);
    y.resize(1, c, l);
    return y;
}

} // namespace convdetail

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width>
Eigen::Tensor3dXf conv2d(const Eigen::Tensor3dXf &x, const Eigen::Tensor4dXf &w,
                         const Eigen::Tensor1dXf &b)
{
    return convdetail::conv2d_impl<in_channels, out_channels, kernel_height,
                                   kernel_width, stride_height, stride_width,
                                   pad_height, pad_width, dilation_height,
                                   dilation_width, false>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width>
Eigen::Tensor3dXf conv2d_fused_gelu(const Eigen::Tensor3dXf &x,
                                    const Eigen::Tensor4dXf &w,
                                    const Eigen::Tensor1dXf &b)
{
    return convdetail::conv2d_impl<in_channels, out_channels, kernel_height,
                                   kernel_width, stride_height, stride_width,
                                   pad_height, pad_width, dilation_height,
                                   dilation_width, true>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width>
Eigen::Tensor3dXf conv2d_tr(const Eigen::Tensor3dXf &x,
                            const Eigen::Tensor4dXf &w,
                            const Eigen::Tensor1dXf &b)
{
    return convdetail::conv2d_tr_impl<in_channels, out_channels, kernel_height,
                                      kernel_width, stride_height, stride_width,
                                      pad_height, pad_width, dilation_height,
                                      dilation_width, false>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width>
Eigen::Tensor3dXf conv2d_tr_fused_gelu(const Eigen::Tensor3dXf &x,
                                       const Eigen::Tensor4dXf &w,
                                       const Eigen::Tensor1dXf &b)
{
    return convdetail::conv2d_tr_impl<in_channels, out_channels, kernel_height,
                                      kernel_width, stride_height, stride_width,
                                      pad_height, pad_width, dilation_height,
                                      dilation_width, true>(x, w, b);
}

// 1D convolutions: treat the leading axis as a batch (width) axis of a
// kernel-width-1 2D convolution, as upstream does.
template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d(const Eigen::Tensor3dXf &x, const Eigen::Tensor3dXf &w,
                         const Eigen::Tensor1dXf &b)
{
    return convdetail::conv1d_any<in_channels, out_channels, kernel_size,
                                  stride, pad, dilation, false, convdetail::kPlain>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_fused_gelu(const Eigen::Tensor3dXf &x,
                                    const Eigen::Tensor3dXf &w,
                                    const Eigen::Tensor1dXf &b)
{
    return convdetail::conv1d_any<in_channels, out_channels, kernel_size,
                                  stride, pad, dilation, false, convdetail::kGelu>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_tr(const Eigen::Tensor3dXf &x,
                            const Eigen::Tensor3dXf &w,
                            const Eigen::Tensor1dXf &b)
{
    return convdetail::conv1d_any<in_channels, out_channels, kernel_size,
                                  stride, pad, dilation, true, convdetail::kPlain>(x, w, b);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_tr_fused_gelu(const Eigen::Tensor3dXf &x,
                                       const Eigen::Tensor3dXf &w,
                                       const Eigen::Tensor1dXf &b)
{
    return convdetail::conv1d_any<in_channels, out_channels, kernel_size,
                                  stride, pad, dilation, true, convdetail::kGelu>(x, w, b);
}

// Flayr Chop: conv1d followed by GLU over channels (glu(conv1d(x), 1) for
// a batch-1 (1, C, L) input), fused so the 2C-channel output never exists.
template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_glu(const Eigen::Tensor3dXf &x,
                             const Eigen::Tensor3dXf &w,
                             const Eigen::Tensor1dXf &b)
{
    if (x.dimension(0) != 1)
        throw std::runtime_error("conv1d_glu: batch must be 1");
    return convdetail::conv1d_any<in_channels, out_channels, kernel_size,
                                  stride, pad, dilation, false, convdetail::kGlu>(x, w, b);
}

// Flayr Chop: a 1-D convolution along axis 1 of a (C, F, T) tensor, batched
// over axis 2 (what upstream gets by shuffling the frequency branch to
// (T, C, F) and calling conv1d, without the shuffles). w: (Cout, Cin, K).
// swap_out writes (Cout', T, F') instead of (Cout', F', T).
template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation, int epi, bool swap_out>
Eigen::Tensor3dXf conv_freq(const Eigen::Tensor3dXf &x,
                            const Eigen::Tensor3dXf &w,
                            const Eigen::Tensor1dXf &b)
{
    return convdetail::conv2d_any<in_channels, out_channels, kernel_size, 1,
                                  stride, 1, pad, 0, dilation, 1, epi, swap_out>(
        x.data(), (int)x.dimension(1), (int)x.dimension(2), w.data(), b.data());
}

} // namespace demucscpp

#endif // CONV_HPP
