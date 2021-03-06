/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "caffe2/perfkernels/embedding_lookup.h"

#include "caffe2/core/types.h"
#include "caffe2/perfkernels/common.h"
#include "caffe2/perfkernels/typed_axpy.h"
#include "caffe2/utils/cpuid.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

// Base implementation does runtime dispatch for each segment of reduction
template <typename IndexType, typename InType, typename OutType>
static void EmbeddingLookupGenericSlow(
    const TIndex block_size,
    const TIndex output_size,
    const TIndex index_size,
    const TIndex data_size,
    const InType* input,
    const IndexType* indices,
    const int* lengths,
    const float* weights, // optional, can be null for sum reducer
    const float* scale_bias, // optional scale & bias params for uint8 input
    bool normalize_by_lengths,
    OutType* out) {
  TIndex current = 0;
  for (int m = 0; m < output_size; ++m) {
    memset(out, 0, sizeof(OutType) * block_size);
    EigenVectorArrayMap<OutType> out_vector(out, block_size);
    for (int i = 0; i < lengths[m]; ++i) {
      CAFFE_ENFORCE_LT(current, index_size);
      TIndex idx = indices[current];
      CAFFE_ENFORCE(
          0 <= idx && idx < data_size,
          "Index ",
          current,
          " is out of bounds: ",
          idx,
          ", range 0 to ",
          data_size);
      CAFFE_ENFORCE_LT(idx, data_size);
#ifdef __GNUC__
      if (current + 1 < index_size) {
        __builtin_prefetch(input + block_size * indices[current + 1], 0, 1);
      }
#endif // __GNUC__

      float w = 1.f, b = 0.f;
      if (weights) {
        w = weights[current];
      }
      if (scale_bias) {
        b = w * scale_bias[2 * indices[current] + 1];
        w = w * scale_bias[2 * indices[current]];
      }

      TypedAxpy<InType, OutType>(
          block_size, w, input + block_size * indices[current], out);

      if (scale_bias) {
        out_vector = out_vector + b;
      }

      ++current;
    }
    if (normalize_by_lengths && lengths[m]) {
      // hack: context is not really used
      math::Scale<OutType, CPUContext>(
          block_size, 1.f / lengths[m], out, out, nullptr);
    }
    out += block_size;
  }
  CAFFE_ENFORCE_EQ(
      current,
      index_size,
      "Your input seems to be incorrect: the sum of lengths values should be "
      "the size of the indices tensor, but it appears not.");
}

// Proxy back to generic implementation
#define EMBEDDING_SPECIALIZATION(IndexType, InType, OutType)       \
  void EmbeddingLookup_##IndexType##_##InType##_##OutType##__base( \
      const TIndex block_size,                                     \
      const TIndex output_size,                                    \
      const TIndex index_size,                                     \
      const TIndex data_size,                                      \
      const InType* input,                                         \
      const IndexType* indices,                                    \
      const int* lengths,                                          \
      const float* weights,                                        \
      const float* scale_bias,                                     \
      bool normalize_by_lengths,                                   \
      OutType* out) {                                              \
    EmbeddingLookupGenericSlow<IndexType, InType, OutType>(        \
        block_size,                                                \
        output_size,                                               \
        index_size,                                                \
        data_size,                                                 \
        input,                                                     \
        indices,                                                   \
        lengths,                                                   \
        weights,                                                   \
        scale_bias,                                                \
        normalize_by_lengths,                                      \
        out);                                                      \
  }                                                                \
  template <>                                                      \
  void EmbeddingLookup(                                            \
      const TIndex block_size,                                     \
      const TIndex output_size,                                    \
      const TIndex index_size,                                     \
      const TIndex data_size,                                      \
      const InType* input,                                         \
      const IndexType* indices,                                    \
      const int* lengths,                                          \
      const float* weights,                                        \
      const float* scale_bias,                                     \
      bool normalize_by_lengths,                                   \
      OutType* out) {                                              \
    AVX2_FMA_DO(                                                   \
        EmbeddingLookup_##IndexType##_##InType##_##OutType,        \
        block_size,                                                \
        output_size,                                               \
        index_size,                                                \
        data_size,                                                 \
        input,                                                     \
        indices,                                                   \
        lengths,                                                   \
        weights,                                                   \
        scale_bias,                                                \
        normalize_by_lengths,                                      \
        out);                                                      \
    BASE_DO(                                                       \
        EmbeddingLookup_##IndexType##_##InType##_##OutType,        \
        block_size,                                                \
        output_size,                                               \
        index_size,                                                \
        data_size,                                                 \
        input,                                                     \
        indices,                                                   \
        lengths,                                                   \
        weights,                                                   \
        scale_bias,                                                \
        normalize_by_lengths,                                      \
        out);                                                      \
  }

EMBEDDING_SPECIALIZATION(int32_t, float, float);
EMBEDDING_SPECIALIZATION(int64_t, float, float);
EMBEDDING_SPECIALIZATION(int32_t, float16, float);
EMBEDDING_SPECIALIZATION(int64_t, float16, float);
EMBEDDING_SPECIALIZATION(int32_t, uint8_t, float);
EMBEDDING_SPECIALIZATION(int64_t, uint8_t, float);

#undef EMBEDDING_SPECIALIZATION

} // namespace caffe2
