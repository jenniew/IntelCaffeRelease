#include <vector>

#include "caffe/layers/lrn_layer.hpp"
#include "caffe/util/math_functions.hpp"
#ifdef _OPENMP
#include <omp.h>
#endif

namespace caffe {

template <typename Dtype>
void LRNLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  size_ = this->layer_param_.lrn_param().local_size();
  CHECK_EQ(size_ % 2, 1) << "LRN only supports odd values for local_size";
  pre_pad_ = (size_ - 1) / 2;
  alpha_ = this->layer_param_.lrn_param().alpha();
  beta_ = this->layer_param_.lrn_param().beta();
  k_ = this->layer_param_.lrn_param().k();
  if (this->layer_param_.lrn_param().norm_region() ==
      LRNParameter_NormRegion_WITHIN_CHANNEL) {
    // Set up split_layer_ to use inputs in the numerator and denominator.
    split_top_vec_.clear();
    split_top_vec_.push_back(&product_input_);
    split_top_vec_.push_back(&square_input_);
    LayerParameter split_param;
    split_layer_.reset(new SplitLayer<Dtype>(split_param));
    split_layer_->SetUp(bottom, split_top_vec_);
    // Set up square_layer_ to square the inputs.
    square_bottom_vec_.clear();
    square_top_vec_.clear();
    square_bottom_vec_.push_back(&square_input_);
    square_top_vec_.push_back(&square_output_);
    LayerParameter square_param;
    square_param.mutable_power_param()->set_power(Dtype(2));
    square_layer_.reset(new PowerLayer<Dtype>(square_param));
    square_layer_->SetUp(square_bottom_vec_, square_top_vec_);
    // Set up pool_layer_ to sum over square neighborhoods of the input.
    pool_top_vec_.clear();
    pool_top_vec_.push_back(&pool_output_);
    LayerParameter pool_param;
    pool_param.mutable_pooling_param()->set_pool(
        PoolingParameter_PoolMethod_AVE);
    pool_param.mutable_pooling_param()->set_pad(pre_pad_);
    pool_param.mutable_pooling_param()->set_kernel_size(size_);
    pool_layer_.reset(new PoolingLayer<Dtype>(pool_param));
    pool_layer_->SetUp(square_top_vec_, pool_top_vec_);
    // Set up power_layer_ to compute (1 + alpha_/N^2 s)^-beta_, where s is
    // the sum of a squared neighborhood (the output of pool_layer_).
    power_top_vec_.clear();
    power_top_vec_.push_back(&power_output_);
    LayerParameter power_param;
    power_param.mutable_power_param()->set_power(-beta_);
    power_param.mutable_power_param()->set_scale(alpha_);
    power_param.mutable_power_param()->set_shift(Dtype(1));
    power_layer_.reset(new PowerLayer<Dtype>(power_param));
    power_layer_->SetUp(pool_top_vec_, power_top_vec_);
    // Set up a product_layer_ to compute outputs by multiplying inputs by the
    // inverse demoninator computed by the power layer.
    product_bottom_vec_.clear();
    product_bottom_vec_.push_back(&product_input_);
    product_bottom_vec_.push_back(&power_output_);
    LayerParameter product_param;
    EltwiseParameter* eltwise_param = product_param.mutable_eltwise_param();
    eltwise_param->set_operation(EltwiseParameter_EltwiseOp_PROD);
    product_layer_.reset(new EltwiseLayer<Dtype>(product_param));
    product_layer_->SetUp(product_bottom_vec_, top);
  }
}

template <typename Dtype>
void LRNLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  CHECK_EQ(4, bottom[0]->num_axes()) << "Input must have 4 axes, "
      << "corresponding to (num, channels, height, width)";
  num_ = bottom[0]->num();
  channels_ = bottom[0]->channels();
  height_ = bottom[0]->height();
  width_ = bottom[0]->width();
  //  ---- openmp ----
  num_of_threads_ = 1;
#ifdef _OPENMP
  num_of_threads_ = omp_get_max_threads() < num_ ? omp_get_max_threads() : num_;
  if (num_of_threads_ < 1) {
     LOG(WARNING) << "LRN layer: omp_get_max_threads() =" << num_of_threads_;
     num_of_threads_ = 1;
  }
#endif
  switch (this->layer_param_.lrn_param().norm_region()) {
  case LRNParameter_NormRegion_ACROSS_CHANNELS:
    top[0]->Reshape(num_, channels_, height_, width_);
    scale_.Reshape(num_, channels_, height_, width_);
    padded_ratio_.Reshape(num_of_threads_, channels_ + size_ - 1,
                          height_, width_);
    accum_ratio_.Reshape(num_of_threads_, 1, height_, width_);
    break;
  case LRNParameter_NormRegion_WITHIN_CHANNEL:
    split_layer_->Reshape(bottom, split_top_vec_);
    square_layer_->Reshape(square_bottom_vec_, square_top_vec_);
    pool_layer_->Reshape(square_top_vec_, pool_top_vec_);
    power_layer_->Reshape(pool_top_vec_, power_top_vec_);
    product_layer_->Reshape(product_bottom_vec_, top);
    break;
  }
}

template <typename Dtype>
void LRNLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  switch (this->layer_param_.lrn_param().norm_region()) {
  case LRNParameter_NormRegion_ACROSS_CHANNELS:
    CrossChannelForward_cpu(bottom, top);
    break;
  case LRNParameter_NormRegion_WITHIN_CHANNEL:
    WithinChannelForward(bottom, top);
    break;
  default:
    LOG(FATAL) << "Unknown normalization region.";
  }
}

template <typename Dtype>
void LRNLayer<Dtype>::CrossChannelForward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* top_data = top[0]->mutable_cpu_data();
  Dtype* scale_data = scale_.mutable_cpu_data();
  Dtype alpha_over_size = alpha_ / size_;
  int limit = pre_pad_ < (channels_-1) ? pre_pad_ : (channels_-1);

  caffe_sqr(num_ * channels_ * height_ * width_, bottom_data, top_data);

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int n = 0; n < num_; ++n) {
    caffe_set(limit * height_ * width_, Dtype(k_),
      scale_data + scale_.offset(n, 0));
    for (int c = 0; c <= limit; ++c) {
      caffe_axpy<Dtype>(height_ * width_, alpha_over_size,
        top_data + scale_.offset(n, c),
        scale_data + scale_.offset(n, 0));
    }
    for (int c = 1; c < channels_; ++c) {
      caffe_cpu_copy<Dtype>(height_ * width_,
        scale_data + scale_.offset(n, c - 1),
        scale_data + scale_.offset(n, c));
      // copy previous scale
      if (c < (channels_ - pre_pad_)) {
        caffe_axpy<Dtype>(height_ * width_, alpha_over_size,
          top_data + scale_.offset(n, c + pre_pad_),
          scale_data + scale_.offset(n, c));
      }
      // subtract tail
      if (c > pre_pad_) {
        caffe_axpy<Dtype>(height_ * width_, -alpha_over_size,
          top_data + scale_.offset(n, c - pre_pad_ - 1),
          scale_data + scale_.offset(n, c));
      }
    }
  }

  caffe_powx<Dtype>(scale_.count(), scale_data, -beta_, top_data);
  caffe_mul<Dtype>(scale_.count(), top_data, bottom_data, top_data);
}

template <typename Dtype>
void LRNLayer<Dtype>::WithinChannelForward(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  split_layer_->Forward(bottom, split_top_vec_);
  square_layer_->Forward(square_bottom_vec_, square_top_vec_);
  pool_layer_->Forward(square_top_vec_, pool_top_vec_);
  power_layer_->Forward(pool_top_vec_, power_top_vec_);
  product_layer_->Forward(product_bottom_vec_, top);
}

template <typename Dtype>
void LRNLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  switch (this->layer_param_.lrn_param().norm_region()) {
  case LRNParameter_NormRegion_ACROSS_CHANNELS:
    CrossChannelBackward_cpu(top, propagate_down, bottom);
    break;
  case LRNParameter_NormRegion_WITHIN_CHANNEL:
    WithinChannelBackward(top, propagate_down, bottom);
    break;
  default:
    LOG(FATAL) << "Unknown normalization region.";
  }
}

template <typename Dtype>
void LRNLayer<Dtype>::CrossChannelBackward_cpu(
    const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  const Dtype* top_diff = top[0]->cpu_diff();
  const Dtype* top_data = top[0]->cpu_data();
  const Dtype* bottom_data = bottom[0]->cpu_data();
  const Dtype* scale_data = scale_.cpu_data();
  Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
  Dtype* padded_ratio_data = padded_ratio_.mutable_cpu_data();
  Dtype* accum_ratio_data = accum_ratio_.mutable_cpu_data();
  // We hack a little bit by using the diff() to store an additional result
  Dtype* accum_ratio_times_bottom = accum_ratio_.mutable_cpu_diff();
  caffe_set(padded_ratio_.count(), Dtype(0), padded_ratio_data);
  Dtype cache_ratio_value = 2. * alpha_ * beta_ / size_;

  caffe_powx<Dtype>(scale_.count(), scale_data, -beta_, bottom_diff);
  caffe_mul<Dtype>(scale_.count(), top_diff, bottom_diff, bottom_diff);

  // go through individual data
  int inverse_pre_pad = size_ - (size_ + 1) / 2;
#ifdef _OPENMP
    #pragma omp parallel for num_threads(this->num_of_threads_)
#endif
    for (int n = 0; n < num_; ++n) {
      int tid = 0;
#ifdef _OPENMP
      tid = omp_get_thread_num();
#endif
      int block_offset = scale_.offset(n);
      // first, compute diff_i * y_i / s_i
      caffe_mul<Dtype>(channels_ * height_ * width_,
          top_diff + block_offset,
          top_data + block_offset,
          padded_ratio_data + padded_ratio_.offset(tid, inverse_pre_pad));
      caffe_div<Dtype>(channels_ * height_ * width_,
          padded_ratio_data + padded_ratio_.offset(tid, inverse_pre_pad),
          scale_data + block_offset,
          padded_ratio_data + padded_ratio_.offset(tid, inverse_pre_pad));
      // Now, compute the accumulated ratios and the bottom diff
      caffe_set(height_*width_,
                Dtype(0),
                accum_ratio_data + accum_ratio_.offset(tid, 0));
      for (int c = 0; c < size_ - 1; ++c) {
        caffe_add<Dtype>(height_ * width_,
                          accum_ratio_data + accum_ratio_.offset(tid, 0),
                          padded_ratio_data + padded_ratio_.offset(tid, c),
                          accum_ratio_data + accum_ratio_.offset(tid, 0));
      }
      for (int c = 0; c < channels_; ++c) {
        caffe_add<Dtype>(height_ * width_,
            accum_ratio_data + accum_ratio_.offset(tid, 0),
            padded_ratio_data + padded_ratio_.offset(tid, c + size_ - 1),
            accum_ratio_data + accum_ratio_.offset(tid, 0));
        // compute bottom diff
        caffe_mul<Dtype>(height_ * width_,
                         bottom_data + top[0]->offset(n, c),
                         accum_ratio_data + accum_ratio_.offset(tid, 0),
                         accum_ratio_times_bottom +
                         accum_ratio_.offset(tid, 0));
        caffe_axpy<Dtype>(height_ * width_, -cache_ratio_value,
            accum_ratio_times_bottom + accum_ratio_.offset(tid, 0),
            bottom_diff + top[0]->offset(n, c));
        caffe_sub<Dtype>(height_ * width_,
            accum_ratio_data + accum_ratio_.offset(tid, 0),
            padded_ratio_data + padded_ratio_.offset(tid, c),
            accum_ratio_data + accum_ratio_.offset(tid, 0));
      }
    }
}

template <typename Dtype>
void LRNLayer<Dtype>::WithinChannelBackward(
    const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  if (propagate_down[0]) {
    vector<bool> product_propagate_down(2, true);
    product_layer_->Backward(top, product_propagate_down, product_bottom_vec_);
    power_layer_->Backward(power_top_vec_, propagate_down, pool_top_vec_);
    pool_layer_->Backward(pool_top_vec_, propagate_down, square_top_vec_);
    square_layer_->Backward(square_top_vec_, propagate_down,
                            square_bottom_vec_);
    split_layer_->Backward(split_top_vec_, propagate_down, bottom);
  }
}

#ifdef CPU_ONLY
STUB_GPU(LRNLayer);
STUB_GPU_FORWARD(LRNLayer, CrossChannelForward);
STUB_GPU_BACKWARD(LRNLayer, CrossChannelBackward);
#endif

INSTANTIATE_CLASS(LRNLayer);

}  // namespace caffe
