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


// Wm(o, k * Cin + c) = w(o, c, kh, kw), k = kh * KW + kw
template <int in_channels, int out_channels, int KH, int KW>
inline Eigen::MatrixXf pack_conv_weights(const Eigen::Tensor4dXf &w)
{
    Eigen::MatrixXf wm(out_channels, KH * KW * in_channels);
    for (int kh = 0; kh < KH; ++kh)
        for (int kw = 0; kw < KW; ++kw)
        {
            const int k = kh * KW + kw;
            for (int c = 0; c < in_channels; ++c)
                for (int o = 0; o < out_channels; ++o)
                    wm(o, k * in_channels + c) = w(o, c, kh, kw);
        }
    return wm;
}

// Wr(k * Cout + o, c) = w(c, o, kh, kw) for transposed conv weights
template <int in_channels, int out_channels, int KH, int KW>
inline Eigen::MatrixXf pack_convtr_weights(const Eigen::Tensor4dXf &w)
{
    Eigen::MatrixXf wr(KH * KW * out_channels, in_channels);
    for (int c = 0; c < in_channels; ++c)
        for (int kh = 0; kh < KH; ++kh)
            for (int kw = 0; kw < KW; ++kw)
            {
                const int k = kh * KW + kw;
                for (int o = 0; o < out_channels; ++o)
                    wr(k * out_channels + o, c) = w(c, o, kh, kw);
            }
    return wr;
}

template <int in_channels, int out_channels, int kernel_height,
          int kernel_width, int stride_height, int stride_width, int pad_height,
          int pad_width, int dilation_height, int dilation_width, bool fuse_gelu>
Eigen::Tensor3dXf conv2d_impl(const Eigen::Tensor3dXf &x,
                              const Eigen::Tensor4dXf &w,
                              const Eigen::Tensor1dXf &b)
{
    const int in_height = (int)x.dimension(1);
    const int in_width = (int)x.dimension(2);

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
    const Eigen::MatrixXf wm =
        pack_conv_weights<in_channels, out_channels, kernel_height,
                          kernel_width>(w);
    Eigen::Map<const Eigen::VectorXf> bias(b.data(), out_channels);

    Eigen::Tensor3dXf y_out(out_channels, out_height, out_width);
    const bool direct = (valid_height == out_height && valid_width == out_width);
    Eigen::MatrixXf tmp;
    float *ydata = y_out.data();
    if (!direct)
    {
        tmp.resize(out_channels, (Eigen::Index)valid_height * valid_width);
        ydata = tmp.data();
    }
    Eigen::Map<Eigen::MatrixXf> ymat(ydata, out_channels,
                                     (Eigen::Index)valid_height * valid_width);

    constexpr bool is_pointwise =
        (KK == 1 && stride_height == 1 && stride_width == 1 &&
         pad_height == 0 && pad_width == 0);
    if (is_pointwise)
    {
        Eigen::Map<const Eigen::MatrixXf> xm(
            x.data(), in_channels, (Eigen::Index)in_height * in_width);
        ymat.noalias() = wm * xm;
    }
    else
    {
        // im2col in column chunks (about 8 MB each) so peak memory stays
        // small; every chunk is one GEMM straight into the output columns
        const Eigen::Index rows = (Eigen::Index)KK * in_channels;
        const Eigen::Index total = (Eigen::Index)valid_height * valid_width;
        const Eigen::Index chunk =
            std::max<Eigen::Index>(512, (Eigen::Index)(2 << 20) / rows);
        Eigen::MatrixXf col(rows, std::min(chunk, total));
        const float *xd = x.data();
        for (Eigen::Index n0 = 0; n0 < total; n0 += chunk)
        {
            const Eigen::Index nc = std::min(chunk, total - n0);
            for (Eigen::Index n = n0; n < n0 + nc; ++n)
            {
                const int oh = (int)(n % valid_height);
                const int ow = (int)(n / valid_height);
                float *dst = col.data() + (n - n0) * rows;
                for (int kh = 0; kh < kernel_height; ++kh)
                {
                    const int ih =
                        oh * stride_height + kh * dilation_height - pad_height;
                    for (int kw = 0; kw < kernel_width; ++kw)
                    {
                        const int iw =
                            ow * stride_width + kw * dilation_width - pad_width;
                        float *d = dst + (kh * kernel_width + kw) * in_channels;
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
            ymat.middleCols(n0, nc).noalias() = wm * col.leftCols(nc);
        }
    }

    ymat.colwise() += bias;
    if (fuse_gelu)
        gelu_inplace(ymat.data(), ymat.size());

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
Eigen::Tensor3dXf conv2d_tr_impl(const Eigen::Tensor3dXf &x,
                                 const Eigen::Tensor4dXf &w,
                                 const Eigen::Tensor1dXf &b)
{
    // every transposed conv in htdemucs has zero padding; upstream's index
    // math is only self-consistent in that case
    static_assert(pad_height == 0 && pad_width == 0,
                  "conv2d_tr: only zero padding is supported");

    const int in_height = (int)x.dimension(1);
    const int in_width = (int)x.dimension(2);
    constexpr int eff_kh =
        kernel_height + (kernel_height - 1) * (dilation_height - 1);
    constexpr int eff_kw =
        kernel_width + (kernel_width - 1) * (dilation_width - 1);
    const int out_height = (in_height - 1) * stride_height + eff_kh;
    const int out_width = (in_width - 1) * stride_width + eff_kw;
    constexpr int KK = kernel_height * kernel_width;

    const Eigen::MatrixXf wr =
        pack_convtr_weights<in_channels, out_channels, kernel_height,
                            kernel_width>(w);

    Eigen::Map<const Eigen::MatrixXf> xm(x.data(), in_channels,
                                         (Eigen::Index)in_height * in_width);
    Eigen::Tensor3dXf y_out(out_channels, out_height, out_width);
    y_out.setZero();
    float *yd = y_out.data();

    // z = Wr * x in column chunks (about 8 MB), each scatter-added into the
    // output before the next chunk is computed
    const Eigen::Index zrows = (Eigen::Index)KK * out_channels;
    const Eigen::Index total = (Eigen::Index)in_height * in_width;
    const Eigen::Index chunk =
        std::max<Eigen::Index>(512, (Eigen::Index)(2 << 20) / zrows);
    Eigen::MatrixXf z(zrows, std::min(chunk, total));
    for (Eigen::Index n0 = 0; n0 < total; n0 += chunk)
    {
        const Eigen::Index nc = std::min(chunk, total - n0);
        z.leftCols(nc).noalias() = wr * xm.middleCols(n0, nc);
        for (Eigen::Index n = n0; n < n0 + nc; ++n)
        {
            const int ih = (int)(n % in_height);
            const int iw = (int)(n / in_height);
            const float *zc = z.data() + (n - n0) * zrows;
            for (int kh = 0; kh < kernel_height; ++kh)
            {
                const int oh = ih * stride_height + kh * dilation_height;
                for (int kw = 0; kw < kernel_width; ++kw)
                {
                    const int ow = iw * stride_width + kw * dilation_width;
                    float *dst =
                        yd + (Eigen::Index)out_channels *
                                 (oh + (Eigen::Index)out_height * ow);
                    const float *src = zc + (kh * kernel_width + kw) * out_channels;
                    Eigen::Map<Eigen::ArrayXf>(dst, out_channels) +=
                        Eigen::Map<const Eigen::ArrayXf>(src, out_channels);
                }
            }
        }
    }

    Eigen::Map<Eigen::MatrixXf> ymat(yd, out_channels,
                                     (Eigen::Index)out_height * out_width);
    Eigen::Map<const Eigen::VectorXf> bias(b.data(), out_channels);
    ymat.colwise() += bias;
    if (fuse_gelu)
        gelu_inplace(yd, ymat.size());
    return y_out;
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
    Eigen::TensorMap<const Eigen::Tensor4dXf> w_4d(
        w.data(), w.dimension(0), w.dimension(1), w.dimension(2), 1);
    Eigen::Tensor4dXf w4 = w_4d;
    Eigen::Tensor3dXf x_shuff = convdetail::move_axis0_last(x);
    Eigen::Tensor3dXf y_out =
        conv2d<in_channels, out_channels, kernel_size, 1, stride, 1, pad, 0,
               dilation, 1>(x_shuff, w4, b);
    return convdetail::move_axis2_first(y_out);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_fused_gelu(const Eigen::Tensor3dXf &x,
                                    const Eigen::Tensor3dXf &w,
                                    const Eigen::Tensor1dXf &b)
{
    Eigen::TensorMap<const Eigen::Tensor4dXf> w_4d(
        w.data(), w.dimension(0), w.dimension(1), w.dimension(2), 1);
    Eigen::Tensor4dXf w4 = w_4d;
    Eigen::Tensor3dXf x_shuff = convdetail::move_axis0_last(x);
    Eigen::Tensor3dXf y_out =
        conv2d_fused_gelu<in_channels, out_channels, kernel_size, 1, stride, 1,
                          pad, 0, dilation, 1>(x_shuff, w4, b);
    return convdetail::move_axis2_first(y_out);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_tr(const Eigen::Tensor3dXf &x,
                            const Eigen::Tensor3dXf &w,
                            const Eigen::Tensor1dXf &b)
{
    Eigen::TensorMap<const Eigen::Tensor4dXf> w_4d(
        w.data(), w.dimension(0), w.dimension(1), w.dimension(2), 1);
    Eigen::Tensor4dXf w4 = w_4d;
    Eigen::Tensor3dXf x_shuff = convdetail::move_axis0_last(x);
    Eigen::Tensor3dXf y_out =
        conv2d_tr<in_channels, out_channels, kernel_size, 1, stride, 1, pad, 0,
                  dilation, 1>(x_shuff, w4, b);
    return convdetail::move_axis2_first(y_out);
}

template <int in_channels, int out_channels, int kernel_size, int stride,
          int pad, int dilation>
Eigen::Tensor3dXf conv1d_tr_fused_gelu(const Eigen::Tensor3dXf &x,
                                       const Eigen::Tensor3dXf &w,
                                       const Eigen::Tensor1dXf &b)
{
    Eigen::TensorMap<const Eigen::Tensor4dXf> w_4d(
        w.data(), w.dimension(0), w.dimension(1), w.dimension(2), 1);
    Eigen::Tensor4dXf w4 = w_4d;
    Eigen::Tensor3dXf x_shuff = convdetail::move_axis0_last(x);
    Eigen::Tensor3dXf y_out =
        conv2d_tr_fused_gelu<in_channels, out_channels, kernel_size, 1, stride,
                             1, pad, 0, dilation, 1>(x_shuff, w4, b);
    return convdetail::move_axis2_first(y_out);
}

} // namespace demucscpp

#endif // CONV_HPP
