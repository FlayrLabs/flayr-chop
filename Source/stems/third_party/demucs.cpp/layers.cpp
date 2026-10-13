#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "layers.hpp"
#include "conv.hpp"
#include "lstm.hpp"
#include "model.hpp"
#include "tensor.hpp"
#include <Eigen/Dense>
#include <unsupported/Eigen/CXX11/Tensor>

// Flayr Chop: the unnormalized half of a numerically stable softmax over a
// contiguous vector: p = exp(p - max(p)); returns sum(p). The caller divides
// the (much smaller) attention output by the sum instead of the scores.
// Eigen's fused (x - max).exp() pass measured faster than vDSP + vForce's
// separate subtract / exp / sum / scale passes.
static inline float softmax_exp_inplace(float *p, int n)
{
    Eigen::Map<Eigen::ArrayXf> col(p, n);
    const float mx = col.maxCoeff();
    col = (col - mx).exp();
    return col.sum();
}

// Flayr Chop: norms rewritten as cache-friendly passes over the column-major
// tensor (axis 0 is contiguous). Same statistics as upstream: two-pass
// mean/variance with the (N - 1) divisor, per (axis-0 index, group).
Eigen::Tensor3dXf demucscpp::group_norm(const Eigen::Tensor3dXf &x,
                                        const Eigen::Tensor1dXf &weight,
                                        const Eigen::Tensor1dXf &b,
                                        int num_groups, float eps)
{
    const int freq = x.dimension(0);
    const int channels = x.dimension(1);
    const int width = x.dimension(2);
    const int group_size = channels / num_groups;
    const double n = (double)group_size * width;

    Eigen::Tensor3dXf y_out(freq, channels, width);
    const float *xd = x.data();
    float *yd = y_out.data();
    std::vector<double> acc(freq);
    std::vector<float> mean(freq), inv(freq);

    if (freq == 1)
    {
        // element (0, c, w) lives at c + channels * w
        for (int g = 0; g < num_groups; ++g)
        {
            const int start = g * group_size;
            double sum = 0.0, ss = 0.0;
            for (int w = 0; w < width; ++w)
                sum += Eigen::Map<const Eigen::ArrayXf>(
                           xd + start + (size_t)channels * w, group_size)
                           .cast<double>()
                           .sum();
            const float mean = (float)(sum / n);
            for (int w = 0; w < width; ++w)
                ss += (Eigen::Map<const Eigen::ArrayXf>(
                           xd + start + (size_t)channels * w, group_size) -
                       mean)
                          .square()
                          .cast<double>()
                          .sum();
            const float inv = 1.0f / std::sqrt((float)(ss / (n - 1.0)) + eps);
            Eigen::Map<const Eigen::ArrayXf> wv(weight.data() + start, group_size);
            Eigen::Map<const Eigen::ArrayXf> bv(b.data() + start, group_size);
            for (int w = 0; w < width; ++w)
            {
                const size_t off = start + (size_t)channels * w;
                Eigen::Map<Eigen::ArrayXf>(yd + off, group_size) =
                    (Eigen::Map<const Eigen::ArrayXf>(xd + off, group_size) - mean) *
                        inv * wv +
                    bv;
            }
        }
        return y_out;
    }

    for (int g = 0; g < num_groups; ++g)
    {
        const int start = g * group_size, end = start + group_size;
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
            {
                const float *p = xd + (size_t)freq * (c + (size_t)channels * w);
                for (int i = 0; i < freq; ++i)
                    acc[i] += p[i];
            }
        for (int i = 0; i < freq; ++i)
            mean[i] = (float)(acc[i] / n);
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
            {
                const float *p = xd + (size_t)freq * (c + (size_t)channels * w);
                for (int i = 0; i < freq; ++i)
                {
                    const float d = p[i] - mean[i];
                    acc[i] += (double)d * d;
                }
            }
        for (int i = 0; i < freq; ++i)
            inv[i] = 1.0f / std::sqrt((float)(acc[i] / (n - 1.0)) + eps);
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
            {
                const size_t off = (size_t)freq * (c + (size_t)channels * w);
                const float wc = weight(c), bc = b(c);
                for (int i = 0; i < freq; ++i)
                    yd[off + i] = (xd[off + i] - mean[i]) * inv[i] * wc + bc;
            }
    }
    return y_out;
}

Eigen::Tensor3dXf
demucscpp::group_norm_fused_gelu(const Eigen::Tensor3dXf &x,
                                 const Eigen::Tensor1dXf &weight,
                                 const Eigen::Tensor1dXf &bias, float eps)
{
    // num_groups is always 1 here
    Eigen::Tensor3dXf y_out = demucscpp::group_norm(x, weight, bias, 1, eps);
    demucscpp::gelu_inplace(y_out.data(), y_out.size());
    return y_out;
}

Eigen::Tensor3dXf demucscpp::glu(const Eigen::Tensor3dXf &x, const int dim)
{
    if (x.dimension(dim) % 2 != 0)
        throw std::runtime_error("glu: dimension size must be even");

    // view x as (inner, D, outer) around `dim`; both halves are contiguous
    // runs of inner*half floats for every outer index
    Eigen::Index inner = 1, outer = 1;
    for (int d = 0; d < dim; ++d)
        inner *= x.dimension(d);
    for (int d = dim + 1; d < 3; ++d)
        outer *= x.dimension(d);
    const Eigen::Index D = x.dimension(dim), half = D / 2;

    Eigen::array<Eigen::Index, 3> dims = {x.dimension(0), x.dimension(1),
                                          x.dimension(2)};
    dims[dim] = half;
    Eigen::Tensor3dXf y(dims);
    const Eigen::Index run = inner * half;
    for (Eigen::Index o = 0; o < outer; ++o)
    {
        const float *a = x.data() + o * inner * D;
        const float *g = a + run;
        Eigen::Map<Eigen::ArrayXf>(y.data() + o * run, run) =
            Eigen::Map<const Eigen::ArrayXf>(a, run) /
            (1.0f + (-Eigen::Map<const Eigen::ArrayXf>(g, run)).exp());
    }
    return y;
}

// y_out(i, c, w) = residual(i, c, w) + scale(c) * a(i, c, w) * sigmoid(g(i, c, w))
// where a / g are the first / second halves of x along axis 1.
Eigen::Tensor3dXf demucscpp::glu_scale_residual(const Eigen::Tensor3dXf &x,
                                                const Eigen::Tensor1dXf &scale,
                                                const Eigen::Tensor3dXf &residual)
{
    const Eigen::Index d0 = x.dimension(0), D = x.dimension(1),
                       d2 = x.dimension(2), half = D / 2;
    if (D % 2 != 0 || residual.dimension(0) != d0 ||
        residual.dimension(1) != half || residual.dimension(2) != d2)
        throw std::runtime_error("glu_scale_residual: shape mismatch");
    Eigen::Tensor3dXf y(d0, half, d2);
    const Eigen::Index n = d0 * half;
    Eigen::Map<const Eigen::ArrayXf> sc(scale.data(), half);
    for (Eigen::Index w = 0; w < d2; ++w)
    {
        // per w: a, g and residual are contiguous (d0 x half) blocks
        Eigen::Map<const Eigen::ArrayXf> a(x.data() + d0 * D * w, n);
        Eigen::Map<const Eigen::ArrayXf> g(x.data() + d0 * (half + D * w), n);
        const float *r = residual.data() + n * w;
        float *o = y.data() + n * w;
        Eigen::Map<Eigen::ArrayXf> out(o, n);
        out = a / (1.0f + (-g).exp());
        if (d0 == 1)
            out = Eigen::Map<const Eigen::ArrayXf>(r, n) + out * sc;
        else
            for (Eigen::Index c = 0; c < half; ++c)
                Eigen::Map<Eigen::ArrayXf>(o + d0 * c, d0) =
                    Eigen::Map<const Eigen::ArrayXf>(r + d0 * c, d0) +
                    scale(c) * Eigen::Map<Eigen::ArrayXf>(o + d0 * c, d0);
    }
    return y;
}

Eigen::Tensor3dXf demucscpp::layer_norm(const Eigen::Tensor3dXf &x,
                                        const Eigen::Tensor1dXf &weight,
                                        const Eigen::Tensor1dXf &bias,
                                        float eps)
{
    // normalize over axis 2 for every (i, c); axis 2 has stride freq*channels
    const int freq = x.dimension(0);
    const int channels = x.dimension(1);
    const int width = x.dimension(2);
    const size_t rows = (size_t)freq * channels;

    Eigen::Tensor3dXf y_out(freq, channels, width);
    const float *xd = x.data();
    float *yd = y_out.data();
    std::vector<double> acc(rows, 0.0);
    std::vector<float> mean(rows), inv(rows);
    for (int w = 0; w < width; ++w)
    {
        const float *p = xd + rows * w;
        for (size_t r = 0; r < rows; ++r)
            acc[r] += p[r];
    }
    for (size_t r = 0; r < rows; ++r)
        mean[r] = (float)(acc[r] / width);
    std::fill(acc.begin(), acc.end(), 0.0);
    for (int w = 0; w < width; ++w)
    {
        const float *p = xd + rows * w;
        for (size_t r = 0; r < rows; ++r)
        {
            const float d = p[r] - mean[r];
            acc[r] += (double)d * d;
        }
    }
    for (size_t r = 0; r < rows; ++r)
        inv[r] = 1.0f / std::sqrt((float)(acc[r] / (width - 1.0)) + eps);
    for (int w = 0; w < width; ++w)
    {
        const float *p = xd + rows * w;
        float *q = yd + rows * w;
        const float ww = weight(w), bw = bias(w);
        for (size_t r = 0; r < rows; ++r)
            q[r] = (p[r] - mean[r]) * inv[r] * ww + bw;
    }
    return y_out;
}

// Flayr Chop: DConv rewritten for a channel-first (C, L, B) tensor (B = the
// batch axis: frequency rows on the frequency branch, 1 on the time branch).
// Upstream works on (B, C, L) and transposes around every convolution. Here
// every batch element is a contiguous C x L block, the dilated convolutions
// are one im2col GEMM, and both residual sub-blocks run on a few batch
// elements at a time so all intermediates stay in cache. Same operations,
// statistics and quirks as upstream (per-element GroupNorm(1) with the
// (N - 1) variance, the dilated conv's two trailing zero columns, the crop).

namespace demucscpp
{
namespace
{

struct DConvParams
{
    const float *w1, *b1;     // dilated conv (H, C, 3), bias H
    const float *gn1w, *gn1b; // GroupNorm(1, H)
    const float *w2, *b2;     // 1x1 conv (2C, H, 1), bias 2C
    const float *gn2w, *gn2b; // GroupNorm(1, 2C)
    const float *scale;       // LayerScale (C)
};

// mean and 1/std of each of nb contiguous blocks of `rows` x `cols` floats,
// like demucscpp::group_norm: double sums, float mean, (n - 1) divisor
static void block_stats(const float *x, Eigen::Index rows, Eigen::Index cols,
                        int nb, float eps, float *mean, float *inv)
{
    const double n = (double)rows * cols;
    for (int b = 0; b < nb; ++b)
    {
        const float *p = x + (size_t)b * rows * cols;
        double sum = 0.0;
        for (Eigen::Index c = 0; c < cols; ++c)
            sum += Eigen::Map<const Eigen::ArrayXf>(p + rows * c, rows)
                       .cast<double>()
                       .sum();
        const float m = (float)(sum / n);
        double ss = 0.0;
        for (Eigen::Index c = 0; c < cols; ++c)
            ss += (Eigen::Map<const Eigen::ArrayXf>(p + rows * c, rows) - m)
                      .square()
                      .cast<double>()
                      .sum();
        mean[b] = m;
        inv[b] = 1.0f / std::sqrt((float)(ss / (n - 1.0)) + eps);
    }
}

// One residual sub-block on nb batch elements.
//   x: C x L x nb (input and residual), y: C x lout x nb (output)
// lconv = upstream's allocated conv output length (L + 2 * dil - 2); columns
// at or past L of the conv output are zero (no bias), then everything is
// cropped to lout <= lconv; lout >= L.
static void dconv_block(const DConvParams &p, const float *x, float *y, int C,
                        int H, int L, int nb, int dil, int lout,
                        std::vector<float> &col, std::vector<float> &hbuf,
                        std::vector<float> &zbuf)
{
    const Eigen::Index rows = (Eigen::Index)3 * C;
    const Eigen::Index ncol = (Eigen::Index)L * nb;
    col.resize((size_t)rows * ncol);
    hbuf.resize((size_t)H * lout * nb);
    zbuf.resize((size_t)2 * C * lout * nb);

    // im2col: column (l, b) = [x(:, l - dil, b); x(:, l, b); x(:, l + dil, b)]
    for (int b = 0; b < nb; ++b)
        for (int l = 0; l < L; ++l)
        {
            float *dst = col.data() + rows * (l + (Eigen::Index)L * b);
            for (int k = 0; k < 3; ++k)
            {
                const int il = l + (k - 1) * dil;
                if (il >= 0 && il < L)
                    std::memcpy(dst + k * C,
                                x + (size_t)C * (il + (size_t)L * b),
                                sizeof(float) * C);
                else
                    std::memset(dst + k * C, 0, sizeof(float) * C);
            }
        }

    // h = W1 * col + b1 for l < L, 0 for L <= l < lout
    Eigen::Map<const Eigen::MatrixXf> w1(p.w1, H, rows);
    Eigen::Map<const Eigen::VectorXf> b1(p.b1, H);
    Eigen::Map<Eigen::MatrixXf> h(hbuf.data(), H, (Eigen::Index)lout * nb);
    if (lout == L)
    {
        h.noalias() = w1 * Eigen::Map<const Eigen::MatrixXf>(col.data(), rows, ncol);
        h.colwise() += b1;
    }
    else
    {
        for (int b = 0; b < nb; ++b)
        {
            auto hb = h.middleCols((Eigen::Index)lout * b, lout);
            hb.leftCols(L).noalias() =
                w1 * Eigen::Map<const Eigen::MatrixXf>(
                         col.data() + rows * (Eigen::Index)L * b, rows, L);
            hb.leftCols(L).colwise() += b1;
            hb.rightCols(lout - L).setZero();
        }
    }

    // GroupNorm(1, H) + GELU
    float mean[64], inv[64];
    for (int b0 = 0; b0 < nb; b0 += 64)
    {
        const int bn = std::min(64, nb - b0);
        block_stats(hbuf.data() + (size_t)H * lout * b0, H, lout, bn, 1e-05f,
                    mean, inv);
        for (int bi = 0; bi < bn; ++bi)
        {
            Eigen::Map<Eigen::ArrayXXf> hb(
                hbuf.data() + (size_t)H * lout * (b0 + bi), H, lout);
            Eigen::Map<const Eigen::ArrayXf> g(p.gn1w, H), be(p.gn1b, H);
            const float m = mean[bi], iv = inv[bi];
            for (int l = 0; l < lout; ++l)
                hb.col(l) = (hb.col(l) - m) * iv * g + be;
        }
    }
    demucscpp::gelu_inplace(hbuf.data(), (Eigen::Index)H * lout * nb);

    // z = W2 * h + b2
    Eigen::Map<const Eigen::MatrixXf> w2(p.w2, 2 * C, H);
    Eigen::Map<const Eigen::VectorXf> b2(p.b2, 2 * C);
    Eigen::Map<Eigen::MatrixXf> z(zbuf.data(), 2 * C, (Eigen::Index)lout * nb);
    z.noalias() = w2 * h;
    z.colwise() += b2;

    // GroupNorm(1, 2C), GLU over channels, LayerScale, residual
    Eigen::Map<const Eigen::ArrayXf> g2(p.gn2w, 2 * C), be2(p.gn2b, 2 * C);
    Eigen::Map<const Eigen::ArrayXf> sc(p.scale, C);
    for (int b0 = 0; b0 < nb; b0 += 64)
    {
        const int bn = std::min(64, nb - b0);
        block_stats(zbuf.data() + (size_t)2 * C * lout * b0, 2 * C, lout, bn,
                    1e-05f, mean, inv);
        for (int bi = 0; bi < bn; ++bi)
        {
            const int b = b0 + bi;
            const float m = mean[bi], iv = inv[bi];
            for (int l = 0; l < lout; ++l)
            {
                Eigen::Map<Eigen::ArrayXf> zc(
                    zbuf.data() + (size_t)2 * C * (l + (size_t)lout * b), 2 * C);
                zc = (zc - m) * iv * g2 + be2;
                Eigen::Map<Eigen::ArrayXf> out(
                    y + (size_t)C * (l + (size_t)lout * b), C);
                out = zc.head(C) / (1.0f + (-zc.tail(C)).exp());
                if (l < L)
                    out = Eigen::Map<const Eigen::ArrayXf>(
                              x + (size_t)C * (l + (size_t)L * b), C) +
                          sc * out;
                else
                    out = sc * out;
            }
        }
    }
}

// The same sub-block for one long batch element (the time branch, up to
// 86k columns), in column chunks so nothing large is materialized: the 2C x L
// activation is recomputed (cheap 1x1 GEMM) for each of GroupNorm's two
// statistics passes and the output pass, which visit the chunks in the same
// order as block_stats visits columns, so the statistics are identical.
static void dconv_block_long(const DConvParams &p, const float *x, float *y,
                             int C, int H, int L, int dil, int lout,
                             std::vector<float> &col, std::vector<float> &hbuf,
                             std::vector<float> &zbuf)
{
    const Eigen::Index rows = (Eigen::Index)3 * C;
    const int chunk = std::max(256, (int)((128 * 1024) / rows));
    col.resize((size_t)rows * chunk);
    hbuf.resize((size_t)H * lout);
    zbuf.resize((size_t)2 * C * chunk);

    Eigen::Map<const Eigen::MatrixXf> w1(p.w1, H, rows);
    Eigen::Map<const Eigen::VectorXf> b1(p.b1, H);
    Eigen::Map<Eigen::MatrixXf> h(hbuf.data(), H, lout);
    for (int l0 = 0; l0 < L; l0 += chunk)
    {
        const int n = std::min(chunk, L - l0);
        for (int l = l0; l < l0 + n; ++l)
        {
            float *dst = col.data() + rows * (l - l0);
            for (int k = 0; k < 3; ++k)
            {
                const int il = l + (k - 1) * dil;
                if (il >= 0 && il < L)
                    std::memcpy(dst + k * C, x + (size_t)C * il, sizeof(float) * C);
                else
                    std::memset(dst + k * C, 0, sizeof(float) * C);
            }
        }
        auto hc = h.middleCols(l0, n);
        hc.noalias() =
            w1 * Eigen::Map<const Eigen::MatrixXf>(col.data(), rows, n);
        hc.colwise() += b1;
    }
    if (lout > L)
        h.rightCols(lout - L).setZero();

    float mean, inv;
    block_stats(hbuf.data(), H, lout, 1, 1e-05f, &mean, &inv);
    {
        Eigen::Map<const Eigen::ArrayXf> g(p.gn1w, H), be(p.gn1b, H);
        for (int l = 0; l < lout; ++l)
            h.col(l).array() = (h.col(l).array() - mean) * inv * g + be;
    }
    demucscpp::gelu_inplace(hbuf.data(), (Eigen::Index)H * lout);

    Eigen::Map<const Eigen::MatrixXf> w2(p.w2, 2 * C, H);
    Eigen::Map<const Eigen::VectorXf> b2(p.b2, 2 * C);
    auto zchunk = [&](int l0, int n) {
        Eigen::Map<Eigen::MatrixXf> z(zbuf.data(), 2 * C, n);
        z.noalias() = w2 * h.middleCols(l0, n);
        z.colwise() += b2;
        return z;
    };
    const double cnt = (double)2 * C * lout;
    double sum = 0.0;
    for (int l0 = 0; l0 < lout; l0 += chunk)
    {
        const int n = std::min(chunk, lout - l0);
        auto z = zchunk(l0, n);
        for (int l = 0; l < n; ++l)
            sum += z.col(l).cast<double>().sum();
    }
    const float m2 = (float)(sum / cnt);
    double ss = 0.0;
    for (int l0 = 0; l0 < lout; l0 += chunk)
    {
        const int n = std::min(chunk, lout - l0);
        auto z = zchunk(l0, n);
        for (int l = 0; l < n; ++l)
            ss += (z.col(l).array() - m2).square().cast<double>().sum();
    }
    const float iv2 = 1.0f / std::sqrt((float)(ss / (cnt - 1.0)) + 1e-05f);

    Eigen::Map<const Eigen::ArrayXf> g2(p.gn2w, 2 * C), be2(p.gn2b, 2 * C);
    Eigen::Map<const Eigen::ArrayXf> sc(p.scale, C);
    for (int l0 = 0; l0 < lout; l0 += chunk)
    {
        const int n = std::min(chunk, lout - l0);
        auto z = zchunk(l0, n);
        for (int j = 0; j < n; ++j)
        {
            const int l = l0 + j;
            auto zc = z.col(j).array();
            zc = (zc - m2) * iv2 * g2 + be2;
            Eigen::Map<Eigen::ArrayXf> out(y + (size_t)C * l, C);
            out = zc.head(C) / (1.0f + (-zc.tail(C)).exp());
            if (l < L)
                out = Eigen::Map<const Eigen::ArrayXf>(x + (size_t)C * l, C) +
                      sc * out;
            else
                out = sc * out;
        }
    }
}

} // namespace
} // namespace demucscpp

void demucscpp::apply_dconv(const struct demucscpp::demucs_model &model,
                            Eigen::Tensor3dXf &y, int freq_idx, int encdec_idx,
                            int layer_idx, int mid_crop)
{
    const int C = (int)y.dimension(0);
    const int L = (int)y.dimension(1);
    const int B = (int)y.dimension(2);
    const int H = C / 8; // DConv compress = 8
    if (C != (48 << layer_idx))
        throw std::runtime_error("apply_dconv: unexpected channel count");

    auto params = [&](int sub) {
        DConvParams p;
        p.w1 = model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx][sub].data();
        p.b1 = model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx][sub].data();
        p.gn1w = model.dconv_layers_1_groupnorm_weight[freq_idx][encdec_idx][layer_idx][sub].data();
        p.gn1b = model.dconv_layers_1_groupnorm_bias[freq_idx][encdec_idx][layer_idx][sub].data();
        p.w2 = model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx][sub].data();
        p.b2 = model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx][sub].data();
        p.gn2w = model.dconv_layers_4_groupnorm_weight[freq_idx][encdec_idx][layer_idx][sub].data();
        p.gn2b = model.dconv_layers_4_groupnorm_bias[freq_idx][encdec_idx][layer_idx][sub].data();
        p.scale = model.dconv_layers_6_scale[freq_idx][encdec_idx][layer_idx][sub].data();
        return p;
    };
    const DConvParams p0 = params(0), p1 = params(1);

    // sub-block 1: kernel 3, dilation 1, padding 1 -> length L
    // sub-block 2: kernel 3, dilation 2, padding 2 -> upstream allocates
    // L + 2 columns (the last two zero), then crops to mid_crop
    const int lout = std::min(mid_crop, L + 2);
    if (lout < L)
        throw std::runtime_error("apply_dconv: crop shorter than the input");

    Eigen::Tensor3dXf out(C, lout, B);

    // batch elements per pass: keep the 2C x L intermediate around 1 MB
    const long long per = (long long)2 * C * std::max(L, lout);
    const int nb = (int)std::max<long long>(1, std::min<long long>(B, (256 * 1024) / per));

    std::vector<float> col, hbuf, zbuf, mid;
    if (B == 1 && per > 256 * 1024)
    {
        mid.resize((size_t)C * L);
        dconv_block_long(p0, y.data(), mid.data(), C, H, L, 1, L, col, hbuf, zbuf);
        dconv_block_long(p1, mid.data(), out.data(), C, H, L, 2, lout, col, hbuf,
                         zbuf);
        y = std::move(out);
        return;
    }
    for (int b0 = 0; b0 < B; b0 += nb)
    {
        const int n = std::min(nb, B - b0);
        mid.resize((size_t)C * L * n);
        dconv_block(p0, y.data() + (size_t)C * L * b0, mid.data(), C, H, L, n, 1, L,
                    col, hbuf, zbuf);
        dconv_block(p1, mid.data(), out.data() + (size_t)C * lout * b0, C, H, L, n, 2,
                    lout, col, hbuf, zbuf);
    }
    y = std::move(out);
}

// Flayr Chop: transformer layer rewritten to stream through cache. Same math
// as upstream, evaluated in blocks:
//  - one GEMM for Q|K|V (self-attention) or Q and K|V (cross-attention);
//    heads are read in place (a head is a contiguous T x 64 column block);
//  - Q is pre-scaled by 1/sqrt(64) = 0.125 (a power of two, so the scores
//    are bit-identical to scaling them afterwards);
//  - attention runs per head over blocks of query tokens, so each S x Tb
//    block of scores is softmaxed while it is still in cache instead of
//    materializing all S x T scores (29 MB per head for the frequency
//    branch); the softmax normalization is applied to the Tb x 64 output
//    rows instead of the S x Tb probabilities (same value up to rounding);
//  - the feed-forward block runs over blocks of tokens (the T x 2048 hidden
//    activation never exists in full);
//  - norm_out (GroupNorm(1) over the whole T x C, as upstream's
//    shuffle + group_norm + shuffle computes it) runs on the T x C layout.
void demucscpp::common_encoder_layer(
    Eigen::Tensor3dXf &q,       // q = x = frequency
    const Eigen::Tensor3dXf &k, // k = xt = time
    const Eigen::Tensor1dXf &norm1_weight, const Eigen::Tensor1dXf &norm1_bias,
    const Eigen::Tensor1dXf &norm2_weight, const Eigen::Tensor1dXf &norm2_bias,
    const Eigen::MatrixXf &in_proj_weight, const Eigen::VectorXf &in_proj_bias,
    const Eigen::MatrixXf &out_proj_weight,
    const Eigen::VectorXf &out_proj_bias, const Eigen::VectorXf &gamma1_scale,
    const Eigen::Tensor1dXf &norm3_weight, const Eigen::Tensor1dXf &norm3_bias,
    const Eigen::MatrixXf &linear1_weight, const Eigen::VectorXf &linear1_bias,
    const Eigen::MatrixXf &linear2_weight, const Eigen::VectorXf &linear2_bias,
    const Eigen::VectorXf &gamma2_scale,
    const Eigen::Tensor1dXf &norm_out_weight,
    const Eigen::Tensor1dXf &norm_out_bias, const int num_heads,
    // optional params
    float eps, const bool self_attention)
{
    const int T = (int)q.dimension(1);
    const int C = (int)q.dimension(2);
    const int S = (int)k.dimension(1);
    if (q.dimension(0) != 1 || k.dimension(0) != 1 || k.dimension(2) != C ||
        C % num_heads != 0)
        throw std::runtime_error("common_encoder_layer: unexpected shapes");
    const int hd = C / num_heads;

    // ---- projections (rows = tokens, columns = channels), computed over
    // blocks of 256 tokens so the GEMM operands stay in cache
    constexpr int kRowBlock = 256;
    Eigen::MatrixXf qkv; // self: T x 3C (Q|K|V); cross: T x C (Q)
    Eigen::MatrixXf kv;  // cross: S x 2C (K|V)
    {
        Eigen::Tensor3dXf q_norm =
            demucscpp::layer_norm(q, norm1_weight, norm1_bias, eps);
        Eigen::Map<const Eigen::MatrixXf> qn(q_norm.data(), T, C);
        const auto wq = self_attention ? in_proj_weight.topRows(3 * C)
                                       : in_proj_weight.topRows(C);
        qkv.resize(T, wq.rows());
        for (int t0 = 0; t0 < T; t0 += kRowBlock)
        {
            const int n = std::min(kRowBlock, T - t0);
            qkv.middleRows(t0, n).noalias() = qn.middleRows(t0, n) * wq.transpose();
        }
        if (!self_attention)
        {
            Eigen::Tensor3dXf k_norm =
                demucscpp::layer_norm(k, norm2_weight, norm2_bias, eps);
            Eigen::Map<const Eigen::MatrixXf> kn(k_norm.data(), S, C);
            const auto wkv = in_proj_weight.bottomRows(2 * C);
            kv.resize(S, 2 * C);
            for (int s0 = 0; s0 < S; s0 += kRowBlock)
            {
                const int n = std::min(kRowBlock, S - s0);
                kv.middleRows(s0, n).noalias() = kn.middleRows(s0, n) * wkv.transpose();
            }
        }
    }
    const float qscale = 1.0f / std::sqrt((float)hd);
    for (int c = 0; c < C; ++c)
        qkv.col(c).array() = (qkv.col(c).array() + in_proj_bias(c)) * qscale;
    float *Kd, *Vd;
    if (self_attention)
    {
        for (int c = C; c < 3 * C; ++c)
            qkv.col(c).array() += in_proj_bias(c);
        Kd = qkv.data() + (size_t)T * C;
        Vd = qkv.data() + (size_t)2 * T * C;
    }
    else
    {
        for (int c = 0; c < 2 * C; ++c)
            kv.col(c).array() += in_proj_bias(C + c);
        Kd = kv.data();
        Vd = kv.data() + (size_t)S * C;
    }

    // ---- attention, per head and block of query tokens
    Eigen::MatrixXf attn(T, C);
    {
        const int tb = std::max(64, std::min(T, (int)((512 * 1024) / S) / 32 * 32));
        Eigen::MatrixXf scores(S, tb);
        Eigen::VectorXf inv_sum(tb);
        for (int h = 0; h < num_heads; ++h)
        {
            Eigen::Map<const Eigen::MatrixXf> Qh(qkv.data() + (size_t)T * hd * h, T, hd);
            Eigen::Map<const Eigen::MatrixXf> Kh(Kd + (size_t)S * hd * h, S, hd);
            Eigen::Map<const Eigen::MatrixXf> Vh(Vd + (size_t)S * hd * h, S, hd);
            for (int t0 = 0; t0 < T; t0 += tb)
            {
                const int n = std::min(tb, T - t0);
                auto sc = scores.leftCols(n);
                sc.noalias() = Kh * Qh.middleRows(t0, n).transpose();
                for (int t = 0; t < n; ++t)
                    inv_sum(t) = 1.0f / softmax_exp_inplace(sc.col(t).data(), S);
                auto out = attn.block(t0, (Eigen::Index)h * hd, n, hd);
                out.noalias() = sc.transpose() * Vh;
                out.array().colwise() *= inv_sum.head(n).array();
            }
        }
    }
    qkv.resize(0, 0);
    kv.resize(0, 0);

    // ---- output projection, LayerScale, residual
    Eigen::Map<Eigen::MatrixXf> q_2d(q.data(), T, C);
    {
        Eigen::MatrixXf out_proj(T, C);
        for (int t0 = 0; t0 < T; t0 += kRowBlock)
        {
            const int n = std::min(kRowBlock, T - t0);
            out_proj.middleRows(t0, n).noalias() =
                attn.middleRows(t0, n) * out_proj_weight.transpose();
        }
        attn.resize(0, 0);
        for (int c = 0; c < C; ++c)
            q_2d.col(c).array() +=
                (out_proj.col(c).array() + out_proj_bias(c)) * gamma1_scale(c);
    }

    // ---- feed-forward over blocks of tokens
    {
        Eigen::Tensor3dXf q_norm =
            demucscpp::layer_norm(q, norm3_weight, norm3_bias, eps);
        Eigen::Map<const Eigen::MatrixXf> qn(q_norm.data(), T, C);
        const int F = (int)linear1_weight.rows();
        const int fb = std::min(T, kRowBlock);
        Eigen::MatrixXf ff1(fb, F), ff2(fb, C);
        for (int t0 = 0; t0 < T; t0 += fb)
        {
            const int n = std::min(fb, T - t0);
            auto f1 = ff1.topRows(n);
            f1.noalias() = qn.middleRows(t0, n) * linear1_weight.transpose();
            f1.rowwise() += linear1_bias.transpose();
            for (int c = 0; c < F; ++c)
                demucscpp::gelu_inplace(f1.col(c).data(), n);
            auto f2 = ff2.topRows(n);
            f2.noalias() = f1 * linear2_weight.transpose();
            for (int c = 0; c < C; ++c)
                q_2d.col(c).segment(t0, n).array() +=
                    (f2.col(c).array() + linear2_bias(c)) * gamma2_scale(c);
        }
    }

    // ---- norm_out: GroupNorm(1, C) over all T x C values (per-channel affine)
    {
        const double cnt = (double)T * C;
        double sum = 0.0;
        for (int t = 0; t < T; ++t)
            sum += q_2d.row(t).cast<double>().sum();
        const float mean = (float)(sum / cnt);
        double ss = 0.0;
        for (int c = 0; c < C; ++c)
            ss += (q_2d.col(c).array() - mean).square().cast<double>().sum();
        const float inv = 1.0f / std::sqrt((float)(ss / (cnt - 1.0)) + eps);
        for (int c = 0; c < C; ++c)
            q_2d.col(c).array() = (q_2d.col(c).array() - mean) * inv *
                                      norm_out_weight(c) +
                                  norm_out_bias(c);
    }
}

void demucscpp_v3::local_attention(
    Eigen::Tensor3dXf &x, // x = frequency, time, or combined
                          // input tensor [B, C, T]
    const Eigen::Tensor3dXf &content_weight,
    const Eigen::Tensor1dXf &content_bias,
    const Eigen::Tensor3dXf &query_weight, const Eigen::Tensor1dXf &query_bias,
    const Eigen::Tensor3dXf &key_weight, const Eigen::Tensor1dXf &key_bias,
    const Eigen::Tensor3dXf &query_decay_weight,
    const Eigen::Tensor1dXf &query_decay_bias,
    const Eigen::Tensor2dXf &query_decay_kernel,
    const Eigen::Tensor3dXf &proj_weight, const Eigen::Tensor1dXf &proj_bias,
    const int hidden_size)
{
    // local-attention block

    int B = x.dimension(0);
    int C = x.dimension(1);
    int T = x.dimension(2);

    const int num_heads = demucscpp_v3::LOCAL_ATTN_N_HEADS;

    // apply query conv1d on x
    Eigen::Tensor3dXf queries;
    Eigen::Tensor3dXf query_decays;
    Eigen::Tensor3dXf keys;
    Eigen::Tensor3dXf content;

    if (hidden_size == 192)
    {
        queries = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, query_weight,
                                                          query_bias);
        keys = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, key_weight, key_bias);
        query_decays = demucscpp::conv1d<192, 16, 1, 1, 0, 1>(
            x, query_decay_weight, query_decay_bias);
        content = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, content_weight,
                                                          content_bias);
    }
    else
    {
        queries = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, query_weight,
                                                          query_bias);
        keys = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, key_weight, key_bias);
        query_decays = demucscpp::conv1d<384, 16, 1, 1, 0, 1>(
            x, query_decay_weight, query_decay_bias);
        content = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, content_weight,
                                                          content_bias);
    }

    // so far, this is correct and matches pytorch

    int features_per_head = C / num_heads;

    // now implement dots calculation

    // Initialize the dots tensor
    Eigen::Tensor4dXf dots(B, num_heads, T, T);
    dots.setZero();

    // Precompute the square root of features_per_head
    float sqrt_features_per_head = std::sqrt(features_per_head);

    // apply a sigmoid activation with a 1/2 incorporated
    query_decays = query_decays.unaryExpr(
        [](float v) { return 0.5f / (1.0f + std::exp(-v)); });

    // Initialize the weights tensor for softmax
    Eigen::Tensor4dXf weights(B, num_heads, T, T);

    // Loop structure to compute both dot products and apply decay
    // simultaneously
    for (int b = 0; b < B; ++b)
    {
        for (int h = 0; h < num_heads; ++h)
        {
            for (int t = 0; t < T; ++t)
            {
                for (int s = 0; s < T; ++s)
                {
                    float dot_product = 0.0f;
                    float decay_effect = 0.0f;

                    // Compute the standard dot product
                    for (int c = 0; c < features_per_head; ++c)
                    {
                        int channel_index = h * features_per_head + c;
                        dot_product += queries(b, channel_index, s) *
                                       keys(b, channel_index, t);
                    }
                    dots(b, h, t, s) = dot_product / sqrt_features_per_head;

                    // Calculate decay effect for this dot product
                    for (int n = 0; n < LOCAL_ATTN_N_DECAY; ++n)
                    {
                        int decay_index = std::abs(
                            t - s); // Assuming decay_kernel is indexed by delta
                        float decay_kernel_value =
                            query_decay_kernel(n, decay_index);

                        // Transform query_decay by applying sigmoid directly
                        // here
                        float decay_query_value =
                            query_decays(b, h * LOCAL_ATTN_N_DECAY + n, s);

                        decay_effect += decay_kernel_value * decay_query_value;
                    }

                    // Apply decay effect directly to the dot product
                    if (t != s)
                    {
                        dots(b, h, t, s) += decay_effect;
                    }
                    else
                    {
                        dots(b, h, t, s) = -100.0f;
                    }
                }
            }

            for (int t = 0; t < T; ++t)
            {
                float max_val = -std::numeric_limits<float>::infinity();
                for (int s = 0; s < T; ++s)
                {
                    if (dots(b, h, s, t) > max_val)
                    {
                        max_val = dots(b, h, s, t);
                    }
                }

                float sum_exp = 0.0f;
                // Calculate the exponentials and sum them
                for (int s = 0; s < T; ++s)
                {
                    weights(b, h, s, t) = std::exp(dots(b, h, s, t) - max_val);
                    sum_exp += weights(b, h, s, t);
                }

                // Normalize the weights to form a proper probability
                // distribution
                for (int s = 0; s < T; ++s)
                {
                    weights(b, h, s, t) /= sum_exp;
                }
            }
        }
    }

    // Initialize the reshaped result tensor directly
    Eigen::Tensor3dXf reshaped_result(B, C, T);
    reshaped_result.setZero();

    // Merge computation of result tensor and reshaping
    for (int b = 0; b < B; ++b)
    {
        for (int h = 0; h < num_heads; ++h)
        {
            for (int c = 0; c < C / num_heads; ++c)
            {
                for (int s = 0; s < T; ++s)
                {
                    for (int t = 0; t < T; ++t)
                    {
                        // Directly update the reshaped_result tensor
                        int full_channel_index = h * (C / num_heads) + c;
                        reshaped_result(b, full_channel_index, s) +=
                            weights(b, h, t, s) *
                            content(b, h * (C / num_heads) + c, t);
                    }
                }
            }
        }
    }

    // Apply projection layer
    Eigen::Tensor3dXf projected_result;
    if (hidden_size == 192)
    {
        projected_result = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(
            reshaped_result, proj_weight, proj_bias);
    }
    else
    {
        projected_result = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(
            reshaped_result, proj_weight, proj_bias);
    }

    // Add x to projected_result
    x += projected_result;
}

void demucscpp_v3::apply_dconv_v3(
    const struct demucscpp_v3::demucs_v3_model &model, Eigen::Tensor3dXf &y,
    int freq_idx, int layer_idx, int mid_crop)
{
    // store another copy of y to sum back later
    Eigen::Tensor3dXf y_copy = y;

    // now dconv time

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 12, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 24, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 48, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 96, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    };

    // y = demucscpp_v3::groupnorm::group_norm_fused_gelu(
    //     y,
    //     model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx]
    //                                          [0],
    //     model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][0],
    //     1,
    //     1e-05);

    y = demucscpp::group_norm_fused_gelu(
        y, model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx][0],
        model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][0], 1e-05);

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<12, 96, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<24, 192, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 2:
        y = demucscpp::conv1d<48, 384, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 3:
        y = demucscpp::conv1d<96, 768, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.dconv_layers_4_groupnorm_weight[freq_idx][layer_idx][0],
        model.dconv_layers_4_groupnorm_bias[freq_idx][layer_idx][0], 1, 1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][layer_idx][0]);

    // now we add y to itself
    y = y + y_copy;

    // store another copy of y to sum back later
    y_copy = y;

    // NEXT ENTIRE SUBSEQUENCE OF DCONV WITH SLIGHTLY DIFFERENT PARAMS

    // Conv1d(48, 6, kernel_size=(3,), stride=(1,), padding=(2,), dilation=(2,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 12, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 24, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 48, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 96, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    };

    Eigen::Tensor3dXf y_cropped =
        y.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                Eigen::array<Eigen::Index, 3>(
                    {y.dimension(0), y.dimension(1), mid_crop}));

    y = y_cropped;

    y = demucscpp::group_norm_fused_gelu(
        y, model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx][1],
        model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][1], 1e-05);

    // Conv1d(6, 96, kernel_size=(1,), stride=(1,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<12, 96, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<24, 192, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 2:
        y = demucscpp::conv1d<48, 384, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 3:
        y = demucscpp::conv1d<96, 768, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.dconv_layers_4_groupnorm_weight[freq_idx][layer_idx][1],
        model.dconv_layers_4_groupnorm_bias[freq_idx][layer_idx][1], 1, 1e-05);

    y = demucscpp::glu(y, 1);
    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][layer_idx][1]);

    // if y_copy is shorter than y in the last dim
    // pad the last dim with zeros to match

    if (y_copy.dimension(2) < y.dimension(2))
    {
        // pad the last dim with zeros to match
        Eigen::Tensor3dXf padded_tensor_copy(
            y_copy.dimension(0), y_copy.dimension(1), y.dimension(2));
        padded_tensor_copy.setZero();
        padded_tensor_copy.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                                 y_copy.dimensions()) = y_copy;
        y_copy = padded_tensor_copy;
    }

    // now sum with itself
    y = y + y_copy;
}

void demucscpp_v3::apply_dconv_v3_encoder_4_5(
    const struct demucscpp_v3::demucs_v3_model &model, Eigen::Tensor3dXf &y,
    int encoder_idx, int mid_crop,
    struct demucscpp_v3::demucs_v3_segment_buffers &buffers)
{
    int lstm_hidden_size = encoder_idx == 0 ? demucscpp_v3::LSTM_HIDDEN_SIZE_0
                                            : demucscpp_v3::LSTM_HIDDEN_SIZE_1;

    // store another copy of y to sum back later
    Eigen::Tensor3dXf y_copy = y;

    // now dconv time

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<768, 192, 3, 1, 1, 1>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<1536, 384, 3, 1, 1, 1>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][0]);
        break;
    };

    y = demucscpp::group_norm_fused_gelu(
        y, model.encoder_4_5_dconv_layers_1_groupnorm_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_1_groupnorm_bias[encoder_idx][0], 1e-05);

    // transpose it to put time seq last
    Eigen::MatrixXf y_mat =
        Eigen::Map<Eigen::MatrixXf>(y.data(), y.dimension(1), y.dimension(2))
            .transpose();

    // then, bilstm
    demucscpp_v3::lstm_forward(model, encoder_idx, 0, y_mat, buffers,
                               lstm_hidden_size);

    // access last element of the last dim which is the output of the bilstm
    Eigen::MatrixXf lstm_out_0 = buffers.lstm_output[encoder_idx][0][1];

    // set lstm state to 0
    demucscpp_v3::lstm_reset_zero(encoder_idx, 0, buffers);

    // apply the linear layer on the lstm_out_0
    lstm_out_0 = (lstm_out_0 *
                  model.encoder_4_5_dconv_layers_3_linear_weight[encoder_idx][0]
                      .transpose())
                     .rowwise() +
                 model.encoder_4_5_dconv_layers_3_linear_bias[encoder_idx][0]
                     .transpose();

    // then apply skip connection
    lstm_out_0 = lstm_out_0 + y_mat;

    // copy it to a original 3d tensor
    y = Eigen::TensorMap<Eigen::Tensor3dXf>(
        lstm_out_0.data(), lstm_out_0.rows(), 1, lstm_out_0.cols());

    // swap dims from 0,1,2 to 1,2,0
    Eigen::Tensor3dXf y_shuff = y.shuffle(Eigen::array<int, 3>({1, 2, 0}));

    // then, localattn
    demucscpp_v3::local_attention(
        y_shuff,
        model.encoder_4_5_dconv_layers_4_content_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_content_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_key_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_key_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_decay_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_decay_bias[encoder_idx][0],
        buffers.local_attn_decay_kernel,
        model.encoder_4_5_dconv_layers_4_proj_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_proj_bias[encoder_idx][0],
        lstm_hidden_size);

    y = y_shuff;

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<192, 1536, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<384, 3072, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][0]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.encoder_4_5_dconv_layers_6_groupnorm_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_6_groupnorm_bias[encoder_idx][0], 1,
        1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.encoder_4_5_dconv_layers_8_scale[encoder_idx][0]);

    // now we add y to itself
    y = y + y_copy;

    // store another copy of y to sum back later
    y_copy = y;

    // NEXT ENTIRE SUBSEQUENCE OF DCONV WITH SLIGHTLY DIFFERENT PARAMS
    // now dconv time

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<768, 192, 3, 1, 2, 2>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<1536, 384, 3, 1, 2, 2>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][1]);
        break;
    };

    Eigen::Tensor3dXf y_cropped =
        y.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                Eigen::array<Eigen::Index, 3>(
                    {y.dimension(0), y.dimension(1), mid_crop}));

    y = y_cropped;

    y = demucscpp::group_norm_fused_gelu(
        y, model.encoder_4_5_dconv_layers_1_groupnorm_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_1_groupnorm_bias[encoder_idx][1], 1e-05);

    // transpose it to put time seq last
    y_mat =
        Eigen::Map<Eigen::MatrixXf>(y.data(), y.dimension(1), y.dimension(2))
            .transpose();

    // then, bilstm
    demucscpp_v3::lstm_forward(model, encoder_idx, 1, y_mat, buffers,
                               lstm_hidden_size);

    // access last element of the last dim which is the output of the bilstm
    lstm_out_0 = buffers.lstm_output[encoder_idx][1][1];

    // reset lstm state to 0
    demucscpp_v3::lstm_reset_zero(encoder_idx, 1, buffers);

    // apply the linear layer on the lstm_out_0
    lstm_out_0 = (lstm_out_0 *
                  model.encoder_4_5_dconv_layers_3_linear_weight[encoder_idx][1]
                      .transpose())
                     .rowwise() +
                 model.encoder_4_5_dconv_layers_3_linear_bias[encoder_idx][1]
                     .transpose();

    // then apply skip connection
    lstm_out_0 = lstm_out_0 + y_mat;

    // copy it to a original 3d tensor
    y = Eigen::TensorMap<Eigen::Tensor3dXf>(
        lstm_out_0.data(), lstm_out_0.rows(), 1, lstm_out_0.cols());

    // swap dims from 0,1,2 to 1,2,0
    y_shuff = y.shuffle(Eigen::array<int, 3>({1, 2, 0}));

    // then, localattn
    demucscpp_v3::local_attention(
        y_shuff,
        model.encoder_4_5_dconv_layers_4_content_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_content_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_key_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_key_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_decay_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_decay_bias[encoder_idx][1],
        buffers.local_attn_decay_kernel,
        model.encoder_4_5_dconv_layers_4_proj_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_proj_bias[encoder_idx][1],
        lstm_hidden_size);

    y = y_shuff;

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<192, 1536, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<384, 3072, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][1]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.encoder_4_5_dconv_layers_6_groupnorm_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_6_groupnorm_bias[encoder_idx][1], 1,
        1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.encoder_4_5_dconv_layers_8_scale[encoder_idx][1]);

    // now sum with itself
    y = y + y_copy;
}
