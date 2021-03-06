/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_DEPTHWISECONV_MULTITHREAD_H_
#define TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_DEPTHWISECONV_MULTITHREAD_H_

#include "tensorflow/lite/kernels/cpu_backend_context.h"
#include "tensorflow/lite/kernels/cpu_backend_threadpool.h"
#include "tensorflow/lite/kernels/internal/optimized/depthwiseconv_float.h"
#include "tensorflow/lite/kernels/internal/optimized/depthwiseconv_uint8.h"

namespace tflite {
namespace optimized_ops {

// TODO(luwa): add multithread to per-channel depthwise_conv
// DepthwiseConv can run with multi threads on the dim specified by thread_dim.
// Each thread processes output elements on dim, thread_dim, in the range of
// [thread_start, thread_end).
// For example, assume thread_start = 2, thread_end = 6, and thread_dim = 1, it
// means that it will calculate DepthwiseConv for output_data[:, 2:5, :, :].
template <typename T, typename TS>
struct DepthwiseConvWorkerTask : cpu_backend_threadpool::Task {
  DepthwiseConvWorkerTask(const DepthwiseParams& params,
                          const RuntimeShape& input_shape, const T* input_data,
                          const RuntimeShape& filter_shape,
                          const T* filter_data, const RuntimeShape& bias_shape,
                          const TS* bias_data, const RuntimeShape& output_shape,
                          T* output_data, int thread_start, int thread_end,
                          int thread_dim)
      : params_(params),
        input_shape_(input_shape),
        input_data_(input_data),
        filter_shape_(filter_shape),
        filter_data_(filter_data),
        bias_shape_(bias_shape),
        bias_data_(bias_data),
        output_shape_(output_shape),
        output_data_(output_data),
        thread_start_(thread_start),
        thread_end_(thread_end),
        thread_dim_(thread_dim) {}

  void Run() override {
    DepthwiseConvImpl(params_, input_shape_, input_data_, filter_shape_,
                      filter_data_, bias_shape_, bias_data_, output_shape_,
                      output_data_, thread_start_, thread_end_, thread_dim_);
  }

 private:
  const DepthwiseParams& params_;
  const RuntimeShape& input_shape_;
  const T* input_data_;
  const RuntimeShape& filter_shape_;
  const T* filter_data_;
  const RuntimeShape& bias_shape_;
  const TS* bias_data_;
  const RuntimeShape& output_shape_;
  T* output_data_;
  int thread_start_;
  int thread_end_;
  int thread_dim_;
};

inline int HowManyConvThreads(const RuntimeShape& output_shape,
                              const RuntimeShape& filter_shape,
                              int thread_dim) {
  constexpr int kMinMulPerThread = 8;
  const int output_units = output_shape.Dims(thread_dim);
  const int filter_height = filter_shape.Dims(1);
  const int filter_width = filter_shape.Dims(2);
  const int num_mul_per_unit =
      FlatSizeSkipDim(output_shape, thread_dim) * filter_height * filter_width;
  const int min_units_per_thread = kMinMulPerThread / num_mul_per_unit + 1;
  int thread_count = output_units / min_units_per_thread;
  return thread_count;
}

template <typename T, typename TS>
inline void DepthwiseConv(const DepthwiseParams& params,
                          const RuntimeShape& input_shape, const T* input_data,
                          const RuntimeShape& filter_shape,
                          const T* filter_data, const RuntimeShape& bias_shape,
                          const TS* bias_data, const RuntimeShape& output_shape,
                          T* output_data,
                          CpuBackendContext* cpu_backend_context) {
  gemmlowp::ScopedProfilingLabel label("DepthwiseConv");

  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

  const int output_batches = output_shape.Dims(0);
  const int output_height = output_shape.Dims(1);
  int thread_count_batch = HowManyConvThreads(output_shape, filter_shape, 0);
  int thread_count_row = HowManyConvThreads(output_shape, filter_shape, 1);
  int thread_dim, thread_count, thread_dim_size;
  if (thread_count_batch > thread_count_row) {
    thread_dim = 0;
    thread_dim_size = output_batches;
    thread_count = thread_count_batch;
  } else {
    thread_dim = 1;
    thread_dim_size = output_height;
    thread_count = thread_count_row;
  }

  const int max_threads = cpu_backend_context->max_num_threads();
  thread_count = std::max(1, std::min(thread_count, max_threads));

  // Cap the number of threads to 2 for float path to avoid regression in
  // performance (b/132294857).
  if (std::is_floating_point<T>::value) {
    thread_count = std::min(thread_count, 2);
  }

  if (thread_count == 1) {
    DepthwiseConvImpl(params, input_shape, input_data, filter_shape,
                      filter_data, bias_shape, bias_data, output_shape,
                      output_data, /*thread_start=*/0,
                      /*thread_end=*/output_height, /*thread_dim=*/1);
  } else {
    std::vector<DepthwiseConvWorkerTask<T, TS>> tasks;
    // TODO(b/131746020) don't create new heap allocations every time.
    // At least we make it a single heap allocation by using reserve().
    tasks.reserve(thread_count);
    int thread_start = 0;
    for (int i = 0; i < thread_count; ++i) {
      int thread_end =
          thread_start + (thread_dim_size - thread_start) / (thread_count - i);
      tasks.emplace_back(params, input_shape, input_data, filter_shape,
                         filter_data, bias_shape, bias_data, output_shape,
                         output_data, thread_start, thread_end, thread_dim);
      thread_start = thread_end;
    }
    cpu_backend_threadpool::Execute(tasks.size(), tasks.data(),
                                    cpu_backend_context);
  }
}

}  // namespace optimized_ops
}  // namespace tflite

#endif  // TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_DEPTHWISECONV_MULTITHREAD_H_
