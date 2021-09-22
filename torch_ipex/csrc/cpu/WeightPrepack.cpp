#include <torch/extension.h>

#include "WeightPrepack.h"
#include "mkldnn/MKLDNNCommon.h"
#include "torch_ipex/csrc/rw_lock.h"
#include "torch_ipex/csrc/utils.h"

namespace torch_ipex {
namespace cpu {

namespace {

using weakref_type = c10::weak_intrusive_ptr<c10::TensorImpl, c10::UndefinedTensorImpl>;
using val_blocked = std::tuple<weakref_type, ideep::tensor>;
std::unordered_map<c10::TensorImpl *, val_blocked> cached_weights;

using map_iter =
    std::unordered_map<c10::TensorImpl *, val_blocked>::const_iterator;

torch_ipex::ReadWriteMutex rwmutex;

ideep::tensor read_cached_weights(const at::Tensor &weight) {
  torch_ipex::UniqueReadLock<torch_ipex::ReadWriteMutex> lock(rwmutex);
  ideep::tensor cached_weight;
  auto it = cached_weights.find(weight.unsafeGetTensorImpl());
  if (it != cached_weights.end()) {
    cached_weight = std::get<1>(it->second);
  }
  return cached_weight;
}

void write_cached_weights(const at::Tensor &weight, ideep::tensor &result) {
  torch_ipex::UniqueWriteLock<torch_ipex::ReadWriteMutex> lock(rwmutex);
  cached_weights.emplace(
      weight.unsafeGetTensorImpl(),
      val_blocked{weakref_type(weight.getIntrusivePtr()), result});
}

}  // namespace

ideep::tensor get_conv_prepacked_weight(
    const at::Tensor& input,
    const at::Tensor& weight,
    at::IntArrayRef stride,
    at::IntArrayRef padding,
    at::IntArrayRef dilation,
    int64_t groups,
    const ideep::attr_t& attr,
    const at::MemoryFormat& mkldnn_memory_format) {
  ideep::tensor cached_weight = read_cached_weights(weight);
  if (cached_weight.is_empty()) {
    auto weight_ = weight.contiguous(mkldnn_memory_format);
    ideep::tensor w = itensor_view_from_dense(weight_);
    // TODO: 3d check
    bool is_channels_last = input.suggest_memory_format() == at::MemoryFormat::ChannelsLast;
    ideep::tensor::desc packed_desc;
    if (is_channels_last) {
      packed_desc = ideep::convolution_forward::expected_weights_desc<true>(
        w.get_dims(),
        w.get_data_type(),
        stride.vec(),
        padding.vec(),
        padding.vec(),
        dilation.vec(),
        groups,
        ideep::algorithm::convolution_direct,
        ideep::prop_kind::forward,
        w.get_data_type(),
        input.sizes().vec(),
        attr);
    } else {
      packed_desc = ideep::convolution_forward::expected_weights_desc<false>(
        w.get_dims(),
        w.get_data_type(),
        stride.vec(),
        padding.vec(),
        padding.vec(),
        dilation.vec(),
        groups,
        ideep::algorithm::convolution_direct,
        ideep::prop_kind::forward,
        w.get_data_type(),
        input.sizes().vec(),
        attr);
    }
    cached_weight.init(packed_desc);
    cached_weight.feed_from(w);
    write_cached_weights(weight, cached_weight);
  }
  return cached_weight;
}

ideep::tensor get_conv_prepacked_weight(
    const at::Tensor& weight,
    at::IntArrayRef stride,
    at::IntArrayRef padding,
    at::IntArrayRef dilation,
    at::IntArrayRef kernel_size,
    int64_t groups,
    int64_t output_channel,
    int64_t input_channel,
    bool is_channels_last,
    bool weight_prepacked) {
  std::vector<int64_t> origin_weight_dims;
  origin_weight_dims.push_back(output_channel);
  origin_weight_dims.push_back(input_channel / groups);
  for (auto&s:kernel_size) {
    origin_weight_dims.push_back(s);
  }
  // TODO: 3d check
  auto data_type = weight.scalar_type();
  ideep::tensor::desc packed_desc;
  if (is_channels_last) {
    packed_desc = ideep::convolution_forward::expected_weights_desc<true>(
        {origin_weight_dims.begin(), origin_weight_dims.end()},
        get_mkldnn_dtype(data_type),
        stride.vec(),
        padding.vec(),
        padding.vec(),
        dilation.vec(),
        groups,
        ideep::algorithm::convolution_direct);
  } else {
    packed_desc = ideep::convolution_forward::expected_weights_desc<false>(
        {origin_weight_dims.begin(), origin_weight_dims.end()},
        get_mkldnn_dtype(data_type),
        stride.vec(),
        padding.vec(),
        padding.vec(),
        dilation.vec(),
        groups,
        ideep::algorithm::convolution_direct);
  }
  ideep::tensor result;
  if (!weight_prepacked) {
    // weight is not preack
    ideep::tensor w = itensor_view_from_dense(weight);
    result.init(packed_desc);
    result.feed_from(w);
  } else  {
    if (data_type == at::ScalarType::Float) {
      result.init(packed_desc, weight.template data_ptr<float>());
    } else {
      result.init(packed_desc, weight.template data_ptr<c10::BFloat16>());
    }
  }
  return result;
}

at::Tensor conv2d_weight_prepack(
    const at::Tensor& weight,
    at::IntArrayRef padding,
    at::IntArrayRef stride,
    at::IntArrayRef dilation,
    int64_t groups,
    c10::optional<at::ScalarType> dtype) {
  auto weight_ = IS_CONTIGUOUS_ANY(weight) ? weight : weight.contiguous(weight.suggest_memory_format());
  bool is_channels_last = weight_.suggest_memory_format() == at::MemoryFormat::ChannelsLast;
  auto w = itensor_view_from_dense(weight_);
  // get the format give data type.
  ideep::data_type desc_dtype = dtype.has_value() ? get_mkldnn_dtype(dtype.value()) : w.get_data_type();
  ideep::tensor::desc expected_desc;
  if (is_channels_last) {
    expected_desc =
        ideep::convolution_forward::expected_weights_desc</* is_channels_last */ true>(
            w.get_dims(),
            desc_dtype,
            {stride.begin(), stride.end()},
            {padding.begin(), padding.end()},
            {padding.begin(), padding.end()},
            {dilation.begin(), dilation.end()},
            groups,
            ideep::algorithm::convolution_direct);
  } else {
    expected_desc =
        ideep::convolution_forward::expected_weights_desc</* is_channels_last */false>(
            w.get_dims(),
            desc_dtype,
            {stride.begin(), stride.end()},
            {padding.begin(), padding.end()},
            {padding.begin(), padding.end()},
            {dilation.begin(), dilation.end()},
            groups,
            ideep::algorithm::convolution_direct);
  }

  auto weight_dtype = w.get_data_type();
  expected_desc = expected_desc.to_type(weight_dtype);
  auto output = empty_aten_tensor_from_desc(expected_desc, weight.options());
  ideep::tensor y;
  if (ideep::data_type::f32 == weight_dtype) {
    y.init(expected_desc, output.template data_ptr<float>());
  } else {
    y.init(expected_desc, output.template data_ptr<c10::BFloat16>());
  }
  y.feed_from(w);
  return output;
}

at::Tensor conv2d_weight_unpack(
    const at::Tensor& weight,
    at::IntArrayRef padding,
    at::IntArrayRef stride,
    at::IntArrayRef dilation,
    at::IntArrayRef kernel_size,
    int64_t groups,
    int64_t output_channel,
    int64_t input_channel,
    bool is_channels_last,
    c10::optional<at::ScalarType> dtype) {
  std::vector<int64_t> origin_weight_dims;
  origin_weight_dims.push_back(output_channel);
  origin_weight_dims.push_back(input_channel / groups);
  for (auto&s:kernel_size) {
    origin_weight_dims.push_back(s);
  }
  auto weight_dtype = get_mkldnn_dtype(weight.scalar_type());
  // get the format give data type.
  ideep::data_type desc_dtype = dtype.has_value() ? get_mkldnn_dtype(dtype.value()) : weight_dtype;
  ideep::tensor::desc expected_desc;
  if (is_channels_last) {
    expected_desc =
        ideep::convolution_forward::expected_weights_desc</* is_channels_last */ true>(
           {origin_weight_dims.begin(), origin_weight_dims.end()},
            desc_dtype,
            {stride.begin(), stride.end()},
            {padding.begin(), padding.end()},
            {padding.begin(), padding.end()},
            {dilation.begin(), dilation.end()},
            groups,
            ideep::algorithm::convolution_direct);
  } else {
    expected_desc =
        ideep::convolution_forward::expected_weights_desc</* is_channels_last */false>(
            {origin_weight_dims.begin(), origin_weight_dims.end()},
            desc_dtype,
            {stride.begin(), stride.end()},
            {padding.begin(), padding.end()},
            {padding.begin(), padding.end()},
            {dilation.begin(), dilation.end()},
            groups,
            ideep::algorithm::convolution_direct);
  }
  expected_desc = expected_desc.to_type(weight_dtype);
  ideep::tensor blocked_weight;
  if (ideep::data_type::f32 == weight_dtype) {
    blocked_weight.init(expected_desc, weight.template data_ptr<float>());
  } else {
    blocked_weight.init(expected_desc, weight.template data_ptr<c10::BFloat16>());
  }

  // init output.
  at::Tensor result = at::empty(origin_weight_dims, weight.options());
  if (is_channels_last) {
    result = result.to(at::MemoryFormat::ChannelsLast);
  }
  auto y = itensor_view_from_dense(result);
  y.feed_from(blocked_weight);
  return result;
}

ideep::tensor get_linear_prepacked_weight(
    const at::Tensor& weight,
    const int64_t out_features,
    const int64_t in_features){
  auto weight_dtype = get_mkldnn_dtype(weight.scalar_type());
  ideep::tensor::desc expected_desc = ideep::inner_product_forward::expected_weights_desc(
    {out_features, in_features},
    /*input_size*/{}, // pack weight without input, will use default batchsize=128
    /*dtype*/weight_dtype, //assume input_dype is same with weight
    /*input_dtype*/weight_dtype);

  // get ideep blocked tensor
  ideep::tensor result;
  if (weight.ndimension() == 2) {
    // weight is public format
    ideep::tensor w = itensor_view_from_dense(weight);
    // weight is already best format
    if (expected_desc == w.get_desc()) return w;
    result.init(expected_desc);
    result.feed_from(w);
  } else  {
    if (weight.scalar_type() == at::ScalarType::Float) {
      result.init(expected_desc, weight.template data_ptr<float>());
    } else {
      result.init(expected_desc, weight.template data_ptr<c10::BFloat16>());
    }
  }
  return result;
}

ideep::tensor get_linear_prepacked_weight(
    const at::Tensor& weight,
    const int64_t batch_size,
    const at::ScalarType src_dtype) {
  ideep::tensor cached_weight = read_cached_weights(weight);
  if (cached_weight.is_empty()) {
    auto weight_ = weight.is_contiguous() ? weight : weight.contiguous();
    ideep::tensor w = itensor_view_from_dense(weight_);
    auto out_features = weight_.size(0);
    auto in_features = weight_.size(1);
    ideep::dims input_dims({batch_size, weight.size(1)});
    auto packed_desc = ideep::inner_product_forward::expected_weights_desc(
        {weight.sizes().cbegin(), weight.sizes().cend()}, input_dims,
        w.get_data_type(), get_mkldnn_dtype(src_dtype));
    cached_weight.init(packed_desc);
    cached_weight.feed_from(w);
    write_cached_weights(weight, cached_weight);
  }
  return cached_weight;
}

bool is_prepacked(const at::Tensor& weight) {
  auto cached_weight = read_cached_weights(weight);
  return cached_weight.is_empty();
}

std::tuple<ideep::tensor, ideep::tensor> get_lstm_prepacked_weight(
    const at::Tensor& weight_ih,
    const at::Tensor& weight_hh,
    int64_t input_size,
    int64_t num_gates,
    int64_t hidden_size,
    const ideep::dims& output_sizes,
    const ideep::tensor& src_layer,
    const ideep::tensor& src_iter,
    const ideep::tensor& src_iter_c,
    const ideep::tensor& bias,
    const bool reverse) {
  auto cached_weight_ih = read_cached_weights(weight_ih);
  auto cached_weight_hh = read_cached_weights(weight_hh);
  bool all_in_cache =
      !cached_weight_ih.is_empty() && !cached_weight_hh.is_empty();
  bool all_miss = cached_weight_ih.is_empty() && cached_weight_hh.is_empty();
  TORCH_CHECK(all_in_cache || all_miss, "both of the weights of LSTM should be cached or neither should be cached");

  if (cached_weight_ih.is_empty()) {
    auto w1 = itensor_view_from_dense(
        weight_ih,
        {{1, 1, input_size, num_gates, hidden_size},
         get_mkldnn_dtype(weight_ih.scalar_type()),
         ideep::format_tag::ldgoi});
    auto w2 = itensor_view_from_dense(
        weight_hh,
        {{1, 1, hidden_size, num_gates, hidden_size},
         get_mkldnn_dtype(weight_hh.scalar_type()),
         ideep::format_tag::ldgoi});

    ideep::tensor::desc packed_desc_ih, packed_desc_hh;
    std::tie(packed_desc_ih, packed_desc_hh) = ideep::lstm_forward::expected_weights_desc(
        output_sizes,
        src_layer,
        src_iter,
        src_iter_c,
        w1,
        w2,
        bias,
        reverse);
    
    // Don't prepack when the weight is of rnn_packed format
    // When the weight is of rnn_packed format, if the seq_lens of
    // the input changes, the format of weight also changes.
    // oneDNN does not support reorder from rnn_packed back to public format.
    // LSTM based on BRGEMM kernel (on AVX512 and newest ISAs) will use blocked
    // format for weight of LSTM, which won't change when the input seq_lens changes.
    if (packed_desc_ih.is_rnn_packed() || packed_desc_hh.is_rnn_packed()) {
      return std::make_tuple(w1, w2);
    }
    cached_weight_ih.init(packed_desc_ih);
    cached_weight_hh.init(packed_desc_hh);

    cached_weight_ih.feed_from(w1);
    cached_weight_hh.feed_from(w2);

    write_cached_weights(weight_ih, cached_weight_ih);
    write_cached_weights(weight_hh, cached_weight_hh);
  }
  return std::make_tuple(cached_weight_ih, cached_weight_hh);
}

at::Tensor linear_weight_prepack(
    const at::Tensor& weight,
    c10::optional<at::ScalarType> dtype) {
  TORCH_CHECK(weight.ndimension() == 2, "expected unpack weight which dim == 2");
  TORCH_CHECK(weight.is_contiguous() || is_transposed_2d(weight), "ipex linear prepack only support contiguous or transposed weight");
  auto w = itensor_view_from_dense(weight);
  // get the format with given data type.
  ideep::data_type desc_dtype = dtype.has_value() ? get_mkldnn_dtype(dtype.value()) : w.get_data_type();
  ideep::tensor::desc expected_desc = ideep::inner_product_forward::expected_weights_desc(
    w.get_dims(),
    /*input_size*/{}, // pack weight without input, will use default batchsize=128
    /*dtype*/desc_dtype, //assume input_dype is same with weight
    /*input_dtype*/desc_dtype);

  auto weight_dtype = w.get_data_type();
  expected_desc = expected_desc.to_type(weight_dtype);

  // Case1: expect desc is public format 
  if (expected_desc.is_plain()){
    // Case1.1: expected desc is same with weight's format
    if (expected_desc == w.get_desc()) return weight;
    // Case2.2: expected desc equals to weight's transpose
    return weight.t().contiguous().t();
  }

  // Case2: expected desc is block format
  auto output = empty_aten_tensor_from_desc(expected_desc, weight.options());
  ideep::tensor y;
  if (ideep::data_type::f32 == weight_dtype) {
    y.init(expected_desc, output.template data_ptr<float>());
  } else {
    y.init(expected_desc, output.template data_ptr<c10::BFloat16>());
  }
  y.feed_from(w);
  return output;
}

at::Tensor linear_weight_unpack(
    const at::Tensor& weight,
    const int64_t out_features,
    const int64_t in_features,
    const bool original_weight_transposed,
    c10::optional<at::ScalarType> dtype) {
  // packed weight and original weight can only be transposed or contiguous
  if (weight.ndimension() == 2) {
    // packed weight is public format
    if (weight.is_contiguous() != original_weight_transposed ){
      // packed weight and original_weight have same format include
      // 1.both packed weight and original_weight is contiguous
      // 2.both packed weight and original_weight is transposed
      return weight;
    } else if (weight.is_contiguous() && original_weight_transposed){
      // weight is contiguous but original weight is transposed
      return weight.t().contiguous().t();
    } else {
       // weight is transposed but original weight is contiguous
      TORCH_CHECK(!weight.is_contiguous() && !original_weight_transposed);
      return weight.contiguous();
    }
  }
  auto weight_dtype = get_mkldnn_dtype(weight.scalar_type());
  // get the format give data type.
  ideep::data_type desc_dtype = dtype.has_value() ? get_mkldnn_dtype(dtype.value()) : weight_dtype;

  // get ideep weight's desc
  ideep::tensor::desc expected_desc = ideep::inner_product_forward::expected_weights_desc(
    {out_features, in_features},
    /*input_size*/{}, // pack weight without input, will use default batchsize=128
    /*dtype*/desc_dtype, //assume input_dype is same with weight
    /*input_dtype*/desc_dtype).to_type(weight_dtype);

  // get ideep blocked tensor
  ideep::tensor blocked_weight;
  if (ideep::data_type::f32 == weight_dtype) {
    blocked_weight.init(expected_desc, weight.template data_ptr<float>());
  } else {
    blocked_weight.init(expected_desc, weight.template data_ptr<c10::BFloat16>());
  }

  //reorder to public format
  at::Tensor result = at::empty({out_features, in_features}, weight.options());
  auto pub_tensor =
      ideep::data_type::f32 == weight_dtype
      ? blocked_weight.to_public(result.template data_ptr<float>(),
                                 ideep::data_type::f32)
      : blocked_weight.to_public(result.template data_ptr<c10::BFloat16>(),
                                ideep::data_type::bf16);
  if (original_weight_transposed) result = result.t().contiguous().t();
  return result;
}

void sync_master_weight_to_bf16(const at::Tensor master_weight, at::Tensor& bf16_weight) {
  TORCH_CHECK(master_weight.sizes() == bf16_weight.sizes(), "expected master weight has same sizes with bf16 weight");
  TORCH_CHECK(master_weight.scalar_type() == at::kFloat && bf16_weight.scalar_type() == at::kBFloat16,
              "expected master weght has same dims with bf16 weight");
  auto w_master = itensor_view_from_dense(master_weight);
  auto w_bf16 = itensor_view_from_dense(bf16_weight);
  w_bf16.feed_from(w_master);
}

}  // namespace cpu
}  // namespace torch_ipex

namespace {

TORCH_LIBRARY_FRAGMENT(torch_ipex, m) {
  m.def("conv2d_weight_prepack(Tensor weight, int[] padding, int[] stride, int[] dilation, int groups, ScalarType? dtype=None) -> Tensor", torch_ipex::cpu::conv2d_weight_prepack);
  m.def("conv2d_weight_unpack(Tensor weight, int[] padding, int[] stride, int[] dilation, int[] kernel_size, int groups, int output_channel, int input_channel, bool is_channels_last, ScalarType? dtype=None) -> Tensor", torch_ipex::cpu::conv2d_weight_unpack);
  m.def("linear_weight_prepack(Tensor weight, ScalarType? dtype=None) -> Tensor", torch_ipex::cpu::linear_weight_prepack);
  m.def("linear_weight_unpack(Tensor weight, int out_features, int in_features, bool transposed, ScalarType? dtype=None) -> Tensor", torch_ipex::cpu::linear_weight_unpack);
  m.def("sync_master_weight_to_bf16(Tensor master_weight, Tensor bf16_weight) -> ()", torch_ipex::cpu::sync_master_weight_to_bf16);

}

}