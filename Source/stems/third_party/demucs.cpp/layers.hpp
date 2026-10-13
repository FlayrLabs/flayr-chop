#ifndef LAYERS_HPP
#define LAYERS_HPP

#include "conv.hpp"
#include "model.hpp"
#include "tensor.hpp"
#include <Eigen/Dense>
#include <iostream>
#include <unsupported/Eigen/CXX11/Tensor>

namespace demucscpp
{

void apply_dconv(const struct demucscpp::demucs_model &model,
                 Eigen::Tensor3dXf &y, int freq_idx, int encdec_idx,
                 int layer_idx, int mid_crop);

// used for implementing both self-attention and cross-attention
// let's not modify the second argument
void common_encoder_layer(
    Eigen::Tensor3dXf &q,       // q = x = frequency|time
    const Eigen::Tensor3dXf &k, // k = xt = time|frequency, _or_ k == q
    const Eigen::Tensor1dXf &norm1_weight, const Eigen::Tensor1dXf &norm1_bias,
    const Eigen::Tensor1dXf &norm2_weight, const Eigen::Tensor1dXf &norm2_bias,
    const Eigen::MatrixXf &in_proj_weight, const Eigen::VectorXf &in_proj_bias,
    const Eigen::MatrixXf &out_proj_weight,
    const Eigen::VectorXf &out_proj_bias, const Eigen::VectorXf &gamma_1_scale,
    const Eigen::Tensor1dXf &norm3_weight, const Eigen::Tensor1dXf &norm3_bias,
    const Eigen::MatrixXf &linear1_weight, const Eigen::VectorXf &linear1_bias,
    const Eigen::MatrixXf &linear2_weight, const Eigen::VectorXf &linear2_bias,
    const Eigen::VectorXf &gamma_2_scale,
    const Eigen::Tensor1dXf &norm_out_weight,
    const Eigen::Tensor1dXf &norm_out_bias, const int num_heads,
    float eps = 1e-5, const bool self_attention = false);

Eigen::Tensor3dXf group_norm(const Eigen::Tensor3dXf &x,
                             const Eigen::Tensor1dXf &w,
                             const Eigen::Tensor1dXf &b, int num_groups,
                             float eps);

Eigen::Tensor3dXf group_norm_fused_gelu(const Eigen::Tensor3dXf &x,
                                        const Eigen::Tensor1dXf &w,
                                        const Eigen::Tensor1dXf &b, float eps);

Eigen::Tensor3dXf layer_norm(const Eigen::Tensor3dXf &x,
                             const Eigen::Tensor1dXf &weight,
                             const Eigen::Tensor1dXf &b, float eps);

Eigen::Tensor3dXf glu(const Eigen::Tensor3dXf &x, const int dim);

// Flayr Chop: residual + layer_scale(glu(x, 1), scale) in one pass
Eigen::Tensor3dXf glu_scale_residual(const Eigen::Tensor3dXf &x,
                                     const Eigen::Tensor1dXf &scale,
                                     const Eigen::Tensor3dXf &residual);

inline Eigen::Tensor3dXf gelu(const Eigen::Tensor3dXf &x)
{
    Eigen::Tensor3dXf y = x;
    gelu_inplace(y.data(), y.size());
    return y;
}

inline Eigen::MatrixXf gelu(const Eigen::MatrixXf &x)
{
    Eigen::MatrixXf y = x;
    gelu_inplace(y.data(), y.size());
    return y;
}

inline Eigen::Tensor3dXf layer_scale(const Eigen::Tensor3dXf &x,
                                     const Eigen::Tensor1dXf &scale_weights)
{
    // y(i, c, w) = x(i, c, w) * s(c); one (d0 x d1) block per w
    const Eigen::Index d0 = x.dimension(0), d1 = x.dimension(1),
                       d2 = x.dimension(2);
    Eigen::Tensor3dXf y_out(x.dimensions());
    Eigen::Map<const Eigen::Array<float, 1, Eigen::Dynamic>> sc(
        scale_weights.data(), d1);
    for (Eigen::Index w = 0; w < d2; ++w)
        Eigen::Map<Eigen::ArrayXXf>(y_out.data() + d0 * d1 * w, d0, d1) =
            Eigen::Map<const Eigen::ArrayXXf>(x.data() + d0 * d1 * w, d0, d1)
                .rowwise() *
            sc;
    return y_out;
}

inline float calculate_variance(const Eigen::Tensor3dXf &tensor, float mean)
{
    Eigen::Tensor<float, 0> sum_squares = (tensor - mean).square().sum();
    float variance = sum_squares(0) / (tensor.size() - 1);
    return variance;
}

inline float calculate_variance(const Eigen::Tensor2dXf &tensor, float mean)
{
    Eigen::Tensor<float, 0> sum_squares = (tensor - mean).square().sum();
    float variance = sum_squares(0) / (tensor.size() - 1);
    return variance;
}

inline float calculate_variance(const Eigen::Tensor1dXf &tensor, float mean)
{
    Eigen::Tensor<float, 0> sum_squares = (tensor - mean).square().sum();
    float variance = sum_squares(0) / (tensor.size() - 1);
    return variance;
}
} // namespace demucscpp

namespace demucscpp_v3
{
void apply_dconv_v3(const struct demucscpp_v3::demucs_v3_model &model,
                    Eigen::Tensor3dXf &y, int freq_idx, int layer_idx,
                    int mid_crop);

void apply_dconv_v3_encoder_4_5(
    const struct demucscpp_v3::demucs_v3_model &model, Eigen::Tensor3dXf &y,
    int encoder_idx, int mid_crop,
    struct demucscpp_v3::demucs_v3_segment_buffers &buffers);

// new function for LocalState, a local attention layer used
// in demucs v3
void local_attention(Eigen::Tensor3dXf &x, // x = frequency, time, or combined
                     const Eigen::Tensor3dXf &content_weight,
                     const Eigen::Tensor1dXf &content_bias,
                     const Eigen::Tensor3dXf &query_weight,
                     const Eigen::Tensor1dXf &query_bias,
                     const Eigen::Tensor3dXf &key_weight,
                     const Eigen::Tensor1dXf &key_bias,
                     const Eigen::Tensor3dXf &query_decay_weight,
                     const Eigen::Tensor1dXf &query_decay_bias,
                     const Eigen::Tensor2dXf &query_decay_kernel,
                     const Eigen::Tensor3dXf &proj_weight,
                     const Eigen::Tensor1dXf &proj_bias, const int hidden_size);

// this shit is complicated, give it its own namespace
namespace groupnorm
{
using ActivationFunc = std::function<float(float)>;

// this  applies the group norm on the second dimension
// Flayr Chop: single cache-friendly implementation. Statistics per group
// over (axis 0, channels in group, axis 2), (N - 1) divisor like upstream.
inline Eigen::Tensor3dXf generalized_group_norm_impl(const Eigen::Tensor3dXf &x,
                                                     const Eigen::Tensor1dXf &weight,
                                                     const Eigen::Tensor1dXf &bias,
                                                     int num_groups, float eps,
                                                     bool apply_gelu)
{
    const int freq = x.dimension(0);
    const int channels = x.dimension(1);
    const int width = x.dimension(2);
    const int group_size = channels / num_groups;
    const double n = (double)freq * group_size * width;

    Eigen::Tensor3dXf y_out(freq, channels, width);
    const float *xd = x.data();
    float *yd = y_out.data();

    for (int g = 0; g < num_groups; ++g)
    {
        const int start = g * group_size, end = start + group_size;
        double sum = 0.0;
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
                sum += Eigen::Map<const Eigen::ArrayXf>(
                           xd + (size_t)freq * (c + (size_t)channels * w), freq)
                           .template cast<double>()
                           .sum();
        const float mean = (float)(sum / n);
        double ss = 0.0;
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
                ss += (Eigen::Map<const Eigen::ArrayXf>(
                           xd + (size_t)freq * (c + (size_t)channels * w), freq) -
                       mean)
                          .square()
                          .template cast<double>()
                          .sum();
        const float inv = 1.0f / std::sqrt((float)(ss / (n - 1.0)) + eps);
        for (int w = 0; w < width; ++w)
            for (int c = start; c < end; ++c)
            {
                const size_t off = (size_t)freq * (c + (size_t)channels * w);
                Eigen::Map<Eigen::ArrayXf>(yd + off, freq) =
                    (Eigen::Map<const Eigen::ArrayXf>(xd + off, freq) - mean) *
                        (inv * weight(c)) +
                    bias(c);
            }
    }
    if (apply_gelu)
        demucscpp::gelu_inplace(yd, y_out.size());
    return y_out;
}

template <typename ActivationFunc>
inline Eigen::Tensor3dXf generalized_group_norm(const Eigen::Tensor3dXf &x,
                                                const Eigen::Tensor1dXf &weight,
                                                const Eigen::Tensor1dXf &bias,
                                                int num_groups, float eps,
                                                ActivationFunc activation_func)
{
    Eigen::Tensor3dXf y =
        generalized_group_norm_impl(x, weight, bias, num_groups, eps, false);
    float *d = y.data();
    for (Eigen::Index i = 0; i < y.size(); ++i)
        d[i] = activation_func(d[i]);
    return y;
}

inline float gelu(float a)
{
    return 0.5f * a * (1.0f + std::erf(a / std::sqrt(2.0f)));
}

inline Eigen::Tensor3dXf group_norm(const Eigen::Tensor3dXf &x,
                                    const Eigen::Tensor1dXf &weight,
                                    const Eigen::Tensor1dXf &bias,
                                    int num_groups, float eps)
{
    return generalized_group_norm_impl(x, weight, bias, num_groups, eps, false);
}

inline Eigen::Tensor3dXf group_norm_fused_gelu(const Eigen::Tensor3dXf &x,
                                               const Eigen::Tensor1dXf &weight,
                                               const Eigen::Tensor1dXf &bias,
                                               int num_groups, float eps)
{
    return generalized_group_norm_impl(x, weight, bias, num_groups, eps, true);
}

inline Eigen::Tensor3dXf group_norm_2(const Eigen::Tensor3dXf &x,
                                      const Eigen::Tensor1dXf &weight,
                                      const Eigen::Tensor1dXf &bias,
                                      int num_groups, float eps)
{
    Eigen::array<int, 3> shuffle_dims = {1, 0, 2}; // Shuffle dimensions

    // Shuffle, apply group norm, and unshuffle
    Eigen::Tensor3dXf x_shuffled = x.shuffle(shuffle_dims);
    Eigen::Tensor3dXf y_shuffled = generalized_group_norm_impl(
        x_shuffled, weight, bias, num_groups, eps, false);
    return y_shuffled.shuffle(shuffle_dims);
}

inline Eigen::Tensor3dXf group_norm_fused_gelu_2(
    const Eigen::Tensor3dXf &x, const Eigen::Tensor1dXf &weight,
    const Eigen::Tensor1dXf &bias, int num_groups, float eps)
{
    Eigen::array<int, 3> shuffle_dims = {1, 0, 2}; // Shuffle dimensions

    // Shuffle, apply group norm with GELU, and unshuffle
    Eigen::Tensor3dXf x_shuffled = x.shuffle(shuffle_dims);
    Eigen::Tensor3dXf y_shuffled = generalized_group_norm_impl(
        x_shuffled, weight, bias, num_groups, eps, true);
    return y_shuffled.shuffle(shuffle_dims);
}
} // namespace groupnorm
} // namespace demucscpp_v3

#endif // LAYERS_HPP
