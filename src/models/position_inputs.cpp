#include "../generators.h"
#include "model.h"
#include "position_inputs.h"
#include "kernels.h"

namespace Generators {

PositionInputs::PositionInputs(const Model& model, State& state, RoamingArray<int32_t>& sequence_lengths_unk)
    : model_{model},
      state_{state} {
  has_mask_input_ = model_.session_info_->HasInput(model_.config_->model.decoder.inputs.attention_mask);
  has_posid_input_ = model_.session_info_->HasInput(model_.config_->model.decoder.inputs.position_ids);
  has_seqlens_k_input_ = model_.session_info_->HasInput(model_.config_->model.decoder.inputs.seqlens_k);
  has_total_sequence_length_input_ = model_.session_info_->HasInput(model_.config_->model.decoder.inputs.total_sequence_length);

  type_ = Ort::TypeToTensorType<int32_t>::type;
  if (has_mask_input_) {
    type_ = model_.session_info_->GetInputDataType(model_.config_->model.decoder.inputs.attention_mask);
  }
  if (has_posid_input_) {
    if (has_mask_input_) {
      if (model_.session_info_->GetInputDataType(model_.config_->model.decoder.inputs.position_ids) != type_) {
        throw std::runtime_error("position_ids & attention_mask must have the same data type");
      }
    }
    type_ = model_.session_info_->GetInputDataType(model_.config_->model.decoder.inputs.position_ids);
  }

  if (type_ != Ort::TypeToTensorType<int32_t>::type && type_ != Ort::TypeToTensorType<int64_t>::type)
    throw std::runtime_error("position_ids & attention_mask only support int32 or int64 types");

  std::array<int64_t, 2> shape{state_.params_->batch_size, state_.params_->sequence_length};  // Only batch_size initially, as we haven't expanded over the beams yet
  position_ids_ = OrtValue::CreateTensor(model.allocator_cpu_, shape, type_);
  position_ids_next_ = OrtValue::CreateTensor(model.allocator_cpu_, std::array<int64_t, 2>{shape[0], 1}, type_);
  attention_mask_ = OrtValue::CreateTensor(model.allocator_cpu_, shape, type_);

  initial_sequence_lengths_.resize(state_.params_->BatchBeamSize());

  if (type_ == Ort::TypeToTensorType<int32_t>::type)
    InitializeTensors<int32_t>(shape, sequence_lengths_unk);
  else
    InitializeTensors<int64_t>(shape, sequence_lengths_unk);

  position_ids_ = model_.ExpandInputs(position_ids_, state_.params_->search.num_beams);
  position_ids_next_ = model_.ExpandInputs(position_ids_next_, state_.params_->search.num_beams);
  attention_mask_ = model_.ExpandInputs(attention_mask_, state_.params_->search.num_beams);
  shape[0] *= state_.params_->search.num_beams;
  position_ids_shape_ = shape;
  attention_mask_shape_ = shape;

  if (model_.use_cuda_graph_ && (model_.device_type_ == DeviceType::CUDA || model_.device_type_ == DeviceType::DML)) {
    size_t max_beam_batch_size = static_cast<size_t>(model_.config_->search.num_beams) * model_.max_batch_size_;
    sb_position_ids_ = std::make_unique<StaticBuffer>(model_.allocator_device_, max_beam_batch_size);
    sb_seqlens_k_ = std::make_unique<StaticBuffer>(model_.allocator_device_, max_beam_batch_size);
  }
}

void PositionInputs::Add() {
  if (has_posid_input_) {
    AddPositionIDs();
  }
  if (has_seqlens_k_input_) {
    AddSeqlensK();
  }
  if (has_total_sequence_length_input_) {
    AddTotalSequenceLength();
  }
  if (has_mask_input_) {
    AddAttentionMask();
  }
}

void PositionInputs::Update(int current_length) {
  if (has_posid_input_) {
    UpdatePositionIDs(current_length);
  }
  if (has_seqlens_k_input_) {
    UpdateSeqlensK(current_length);
  }
  if (has_total_sequence_length_input_) {
    UpdateTotalSequenceLength(current_length);
  }
  if (has_mask_input_) {
    UpdateAttentionMask(current_length);
  }
}

void PositionInputs::AddAttentionMask() {
  mask_input_index_ = state_.inputs_.size();

  state_.inputs_.push_back(attention_mask_.get());
  state_.input_names_.push_back(model_.config_->model.decoder.inputs.attention_mask.c_str());
}

void PositionInputs::AddPositionIDs() {
  posid_input_index_ = state_.inputs_.size();

  state_.inputs_.push_back(position_ids_.get());
  state_.input_names_.push_back(model_.config_->model.decoder.inputs.position_ids.c_str());
}

void PositionInputs::AddSeqlensK() {
  seqlens_k_input_index_ = state_.inputs_.size();

  senlens_k_shape_ = {static_cast<int64_t>(state_.params_->batch_size) * state_.params_->search.num_beams};
  seqlens_k_ = OrtValue::CreateTensor(model_.allocator_cpu_, senlens_k_shape_, Ort::TypeToTensorType<int32_t>::type);

  std::copy(initial_sequence_lengths_.begin(),
            initial_sequence_lengths_.end(),
            seqlens_k_->GetTensorMutableData<int32_t>());

  state_.inputs_.push_back(seqlens_k_.get());
  state_.input_names_.push_back(model_.config_->model.decoder.inputs.seqlens_k.c_str());
}

void PositionInputs::AddTotalSequenceLength() {
  total_sequence_length_input_index_ = state_.inputs_.size();
  total_sequence_length_ = OrtValue::CreateTensor(model_.allocator_cpu_,
                                                  {},
                                                  Ort::TypeToTensorType<int32_t>::type);

  total_sequence_length_->GetTensorMutableData<int32_t>()[0] = state_.params_->sequence_length;
  state_.inputs_.push_back(total_sequence_length_.get());
  state_.input_names_.push_back(model_.config_->model.decoder.inputs.total_sequence_length.c_str());
}

void PositionInputs::UpdatePositionIDs(int current_length) {
  // Reallocate position_ids for the 2nd and onward shape
  if (is_first_posid_update_) {
    position_ids_shape_[1] = 1;
    if (!sb_position_ids_) {
      position_ids_ = std::move(position_ids_next_);
    } else {
#if USE_CUDA
      position_ids_ = sb_position_ids_->CreateTensorOnStaticBuffer(position_ids_shape_, type_);
      assert(model_.device_type_ == DeviceType::CUDA);
      if (type_ == Ort::TypeToTensorType<int32_t>::type) {
        cudaMemcpyAsync(position_ids_->GetTensorMutableRawData(),
                        position_ids_next_->GetTensorData<int32_t>(),
                        sizeof(int32_t) * position_ids_shape_[0],
                        cudaMemcpyDeviceToDevice,
                        model_.cuda_stream_);
      } else {
        cudaMemcpyAsync(position_ids_->GetTensorMutableRawData(),
                        position_ids_next_->GetTensorData<int64_t>(),
                        sizeof(int64_t) * position_ids_shape_[0],
                        cudaMemcpyDeviceToDevice,
                        model_.cuda_stream_);
      }
#elif USE_DML
      position_ids_ = sb_position_ids_->CreateTensorOnStaticBuffer(position_ids_shape_, type_);
      assert(model_.device_type_ == DeviceType::DML);

      ComPtr<ID3D12Resource> target_resource;
      Ort::ThrowOnError(model_.GetOrtDmlApi()->GetD3D12ResourceFromAllocation(model_.allocator_device_, position_ids_->GetTensorMutableRawData(), &target_resource));

      if (type_ == Ort::TypeToTensorType<int32_t>::type) {
        auto source = std::span(position_ids_next_->GetTensorData<const uint8_t>(), sizeof(int32_t) * position_ids_shape_[0]);

        model_.GetDmlUploadHeap()->BeginUploadToGpu(
            target_resource.Get(),
            0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            source);
      } else {
        auto source = std::span(position_ids_next_->GetTensorData<const uint8_t>(), sizeof(int64_t) * position_ids_shape_[0]);

        model_.GetDmlUploadHeap()->BeginUploadToGpu(
            target_resource.Get(),
            0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            source);
      }
#endif
    }
    is_first_posid_update_ = false;
    state_.inputs_[posid_input_index_] = position_ids_.get();
  } else {  // Just incrementing existing position IDs
    switch (model_.device_type_) {
#if USE_DML
      case DeviceType::DML: {
        ComPtr<ID3D12Resource> target_resource;
        Ort::ThrowOnError(model_.GetOrtDmlApi()->GetD3D12ResourceFromAllocation(model_.allocator_device_, position_ids_->GetTensorMutableRawData(), &target_resource));

        // DML doesn't support on-device position ids update yet, so we do it on the CPU
        if (type_ == Ort::TypeToTensorType<int32_t>::type) {
          auto* source = position_ids_next_->GetTensorMutableData<int32_t>();
          for (int i = 0; i < position_ids_shape_[0]; i++) {
            source[i]++;
          }

          auto source_bytes = std::span(reinterpret_cast<const uint8_t*>(source), sizeof(int32_t) * position_ids_shape_[0]);
          model_.GetDmlUploadHeap()->BeginUploadToGpu(
              target_resource.Get(),
              0,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              source_bytes);
        } else {
          auto* source = position_ids_next_->GetTensorMutableData<int64_t>();
          for (int i = 0; i < position_ids_shape_[0]; i++) {
            source[i]++;
          }

          auto source_bytes = std::span(reinterpret_cast<const uint8_t*>(source), sizeof(int64_t) * position_ids_shape_[0]);
          model_.GetDmlUploadHeap()->BeginUploadToGpu(
              target_resource.Get(),
              0,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              source_bytes);
        }
      } break;
#endif
      case DeviceType::CPU: {
        if (type_ == Ort::TypeToTensorType<int32_t>::type)
          UpdatePositionIDsImpl<int32_t>();
        else
          UpdatePositionIDsImpl<int64_t>();
        break;
      }
#if USE_CUDA
      case DeviceType::CUDA:
        if (type_ == Ort::TypeToTensorType<int32_t>::type)
          cuda::Launch_UpdatePositionIds(position_ids_->GetTensorMutableData<int32_t>(), static_cast<int>(position_ids_shape_[0]), model_.cuda_stream_);
        else
          cuda::Launch_UpdatePositionIds(position_ids_->GetTensorMutableData<int64_t>(), static_cast<int>(position_ids_shape_[0]), model_.cuda_stream_);
        break;
#endif
      default:
        throw std::runtime_error("PositionIDs::Update - Unsupported device type");
    }
  }
}

void PositionInputs::UpdateSeqlensK(int current_length) {
#if USE_CUDA
  assert(type_ == Ort::TypeToTensorType<int32_t>::type);
  assert(model_.device_type_ == DeviceType::CUDA);

  if (is_first_seqlen_update_) {
    if (!sb_seqlens_k_) {
      seqlens_k_ = OrtValue::CreateTensor(*model_.allocator_device_, senlens_k_shape_, Ort::TypeToTensorType<int32_t>::type);
    } else {
      seqlens_k_ = sb_seqlens_k_->CreateTensorOnStaticBuffer(senlens_k_shape_, Ort::TypeToTensorType<int32_t>::type);
    }
    state_.inputs_[seqlens_k_input_index_] = seqlens_k_.get();
    cudaMemcpyAsync(seqlens_k_->GetTensorMutableRawData(), initial_sequence_lengths_.data(), sizeof(int32_t) * initial_sequence_lengths_.size(), cudaMemcpyHostToDevice, model_.cuda_stream_);
    is_first_seqlen_update_ = false;
  } else {
    cuda::Launch_UpdatePositionIds(seqlens_k_->GetTensorMutableData<int32_t>(), static_cast<int>(senlens_k_shape_[0]), model_.cuda_stream_);
  }
#elif USE_DML
  assert(model_.device_type_ == DeviceType::DML);

  if (is_first_seqlen_update_) {
    if (!sb_seqlens_k_) {
      seqlens_k_ = OrtValue::CreateTensor(*model_.allocator_device_, senlens_k_shape_, Ort::TypeToTensorType<int32_t>::type);
    } else {
      seqlens_k_ = sb_seqlens_k_->CreateTensorOnStaticBuffer(senlens_k_shape_, Ort::TypeToTensorType<int32_t>::type);
    }
    state_.inputs_[seqlens_k_input_index_] = seqlens_k_.get();

    is_first_seqlen_update_ = false;
  } else {
    for (int i = 0; i < initial_sequence_lengths_.size(); i++) {
      initial_sequence_lengths_[i]++;
    }
  }

  auto source = std::span(reinterpret_cast<const uint8_t*>(initial_sequence_lengths_.data()), sizeof(int32_t) * initial_sequence_lengths_.size());

  ComPtr<ID3D12Resource> target_resource;
  Ort::ThrowOnError(model_.GetOrtDmlApi()->GetD3D12ResourceFromAllocation(model_.allocator_device_, seqlens_k_->GetTensorMutableRawData(), &target_resource));

  model_.GetDmlUploadHeap()->BeginUploadToGpu(
      target_resource.Get(),
      0,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      source);
#endif
}

void PositionInputs::UpdateTotalSequenceLength(int current_length) {
  total_sequence_length_->GetTensorMutableData<int32_t>()[0] = current_length;
}

void PositionInputs::UpdateAttentionMask(int current_length) {
  // Update attention mask
  assert(attention_mask_shape_[1] == current_length - 1);  // We should always be growing by 1
  attention_mask_shape_[1] = current_length;

  auto& allocator = model_.device_type_ == DeviceType::DML ? model_.allocator_cpu_ : *model_.allocator_device_;
  std::unique_ptr<OrtValue> next_attention_mask = OrtValue::CreateTensor(allocator, attention_mask_shape_, type_);

  switch (model_.device_type_) {
    case DeviceType::DML:
      // DML doesn't support on-device mask updating yet, so we fallback to the CPU
    case DeviceType::CPU: {
      if (type_ == Ort::TypeToTensorType<int32_t>::type)
        UpdateAttentionMaskImpl(next_attention_mask->GetTensorMutableData<int32_t>(), attention_mask_->GetTensorData<int32_t>(), current_length);
      else
        UpdateAttentionMaskImpl(next_attention_mask->GetTensorMutableData<int64_t>(), attention_mask_->GetTensorData<int64_t>(), current_length);
      break;
    }
#if USE_CUDA
    case DeviceType::CUDA:
      if (type_ == Ort::TypeToTensorType<int32_t>::type)
        cuda::Launch_UpdateAttentionMask(next_attention_mask->GetTensorMutableData<int32_t>(), attention_mask_->GetTensorData<int32_t>(), static_cast<int>(attention_mask_shape_[0]), current_length, model_.cuda_stream_);
      else
        cuda::Launch_UpdateAttentionMask(next_attention_mask->GetTensorMutableData<int64_t>(), attention_mask_->GetTensorData<int64_t>(), static_cast<int>(attention_mask_shape_[0]), current_length, model_.cuda_stream_);
      break;
#endif
    default:
      throw std::runtime_error("PositionIDs::Update - Unsupported device type");
  }
  attention_mask_ = std::move(next_attention_mask);
  state_.inputs_[mask_input_index_] = attention_mask_.get();
}

template <typename T>
void PositionInputs::InitializeTensors(std::array<int64_t, 2> shape, cpu_span<int32_t> sequence_lengths) {
  // Set attention mask to be 0 for pad tokens, and 1 for all other tokens.
  // Set position id to be 0 for pad tokens, and accumulated sum of mask in a batch for other tokens
  auto* mask_data = attention_mask_->GetTensorMutableData<T>();
  auto* position_data = position_ids_->GetTensorMutableData<T>();
  auto* position_data_next = position_ids_next_->GetTensorMutableData<T>();
  const auto* word_id = state_.params_->input_ids.data();
  auto* mask = mask_data;
  auto* position = position_data;
  for (int i = 0; i < shape[0]; i++) {
    T abs_position = 0;
    for (int j = 0; j < shape[1]; j++, word_id++, mask++, position++) {
      if (*word_id == state_.params_->pad_token_id) {
        *mask = 0;
        *position = 0;
      } else {
        *mask = 1;
        *position = abs_position++;
      }
    }

    position_data_next[i] = abs_position;
    for (int k = 0; k < state_.params_->search.num_beams; k++) {
      sequence_lengths[i * state_.params_->search.num_beams + k] = static_cast<int32_t>(abs_position);
      initial_sequence_lengths_[i * state_.params_->search.num_beams + k] = static_cast<int32_t>(abs_position);
    }
  }
}

template <typename T>
void PositionInputs::UpdatePositionIDsImpl() {
  // Increment position IDs
  auto* data = position_ids_->GetTensorMutableData<T>();
  for (int i = 0; i < position_ids_shape_[0]; i++) {
    data[i]++;
  }
};

template <typename T>
void PositionInputs::UpdateAttentionMaskImpl(T* data, const T* old_data, int current_length) {
  for (int i = 0; i < attention_mask_shape_[0]; i++) {
    for (int j = 0; j < current_length - 1; j++) {
      data[i * current_length + j] = old_data[i * (current_length - 1) + j];
    }
    data[i * current_length + current_length - 1] = 1;
  }
};

}  // namespace Generators
