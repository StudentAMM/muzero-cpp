// Copyright 2019 DeepMind Technologies Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modifications made for the muzero network (tuero@ualberta.ca)

#include "muzero-cpp/vprnet.h"

#include <filesystem>
#include <iostream>

#include "absl/strings/str_cat.h"
#include "muzero-cpp/vprnet.h"

namespace muzero_cpp {
using namespace model;
using namespace types;
using namespace torch::indexing;

VPRNetModel::VPRNetModel(const muzero_config::MuZeroConfig& config, const std::string& device)
    : device_(device),
      path_(absl::StrCat(config.path, "/checkpoints/")),
      config_(config),
      model_(config),
      model_optimizer_(model_->parameters(), torch::optim::AdamOptions(config.network_config.learning_rate)
                                                 .weight_decay(config.network_config.l2_weight_decay)),
      torch_device_(device),
      value_encoder_(config.min_value, config.max_value, config.use_contractive_mapping),
      reward_encoder_(config.min_reward, config.max_reward, config.use_contractive_mapping) {
    // Calculate the flat observation sizes for initial and recurrent inference
    initial_flat_size_ =
        model_->get_initial_inference_channels() * config.observation_shape.h * config.observation_shape.w;
    encoded_obs_shape_ = model_->get_encoded_observation_shape();
    recurrent_flat_size_ = encoded_obs_shape_.c * encoded_obs_shape_.h * encoded_obs_shape_.w;
    action_flat_size_ = config_.action_channels * encoded_obs_shape_.h * encoded_obs_shape_.w;
    value_loss_weight_ = config_.value_loss_weight;
    // Put model into given device
    model_->to(torch_device_);
    std::filesystem::create_directories(absl::StrCat(path_));
}

// Pretty print torch model
void VPRNetModel::print() const {
    std::cout << model_ << std::endl;
}

// Save the model to a checkpoint model
std::string VPRNetModel::SaveCheckpoint(int step) {
    std::string full_path = absl::StrCat(path_, "checkpoint-", step);
    torch::save(model_, absl::StrCat(full_path, ".pt"));
    torch::save(model_optimizer_, absl::StrCat(full_path, "-optimizer.pt"));
    return full_path;
}

// Load model from checkpoint
void VPRNetModel::LoadCheckpoint(int step) {
    LoadCheckpoint(absl::StrCat(path_, "checkpoint-", step));
}

// Load model from checkpoint
void VPRNetModel::LoadCheckpoint(const std::string& path) {
    torch::load(model_, absl::StrCat(path, ".pt"), torch_device_);
    torch::load(model_optimizer_, absl::StrCat(path, "-optimizer.pt"), torch_device_);
}

// Perform initial inference
std::vector<VPRNetModel::InferenceOutputs> VPRNetModel::InitialInference(
    std::vector<VPRNetModel::InitialInferenceInputs>& inputs) {
    int batch_size = (int)inputs.size();

    // Create tensor from raw flat array
    // torch::from_blob requires a pointer to non-const and doesn't take ownership
    auto options = torch::TensorOptions().dtype(torch::kFloat);
    torch::Tensor stacked_observations = torch::empty({batch_size, initial_flat_size_}, options);
    for (int batch = 0; batch < batch_size; ++batch) {
        stacked_observations[batch] =
            torch::from_blob(inputs[batch].stacked_observation.data(), {initial_flat_size_}, options).clone();
    }

    // Reshape to expected size for network (batch_size, flat) -> (batch_size, c, h, w)
    stacked_observations =
        stacked_observations.reshape({batch_size, -1, encoded_obs_shape_.h, encoded_obs_shape_.w});

    // Move to device
    stacked_observations = stacked_observations.to(torch_device_);

    // Put model in eval mode for inference + scoped no_grad
    model_->eval();
    torch::NoGradGuard no_grad;
    model::InferenceOutput batched_output = model_->initial_inference(stacked_observations);

    // Create batched decoded value/reward
    torch::Tensor value_decoded =
        value_encoder_.decode(batched_output.value).to(torch::kCPU).to(torch::kDouble);
    torch::Tensor reward_decoded =
        reward_encoder_.decode(batched_output.reward).to(torch::kCPU).to(torch::kDouble);

    std::vector<VPRNetModel::InferenceOutputs> outputs;
    outputs.reserve(batch_size);
    for (int batch = 0; batch < batch_size; ++batch) {
        // Inference output is a struct of batched tensors, need to extract/convert the tensors into the
        // proper vector data structures
        assert((int)value_decoded[batch].size(0) == 1);
        assert((int)reward_decoded[batch].size(0) == 1);
        double value = value_decoded[batch][0].item<double>();
        double reward = reward_decoded[batch][0].item<double>();
        // Inference uses a policy, so we softmax the logits here
        torch::Tensor batch_policy =
            torch::softmax(batched_output.policy_logits[batch], -1).to(torch::kCPU).to(torch::kDouble);
        std::vector<double> policy(batch_policy.data_ptr<double>(),
                                   batch_policy.data_ptr<double>() + batch_policy.numel());
        torch::Tensor batch_encoded_state =
            batched_output.encoded_state[batch].to(torch::kCPU).to(torch::kDouble);
        Observation encoded_state(batch_encoded_state.data_ptr<double>(),
                                  batch_encoded_state.data_ptr<double>() + batch_encoded_state.numel());
        outputs.push_back({value, reward, policy, encoded_state});
    }
    return outputs;
}

std::vector<VPRNetModel::InferenceOutputs> VPRNetModel::RecurrentInference(
    std::vector<VPRNetModel::RecurrentInferenceInputs>& inputs) {
    int batch_size = (int)inputs.size();

    // Create tensor from raw flat array
    // torch::from_blob requires a pointer to non-const and doesn't take ownership
    auto options = torch::TensorOptions().dtype(torch::kFloat);
    torch::Tensor encoded_observations = torch::empty({batch_size, recurrent_flat_size_}, options);
    torch::Tensor actions = torch::empty({batch_size, action_flat_size_}, options);
    for (int batch = 0; batch < batch_size; ++batch) {
        encoded_observations[batch] =
            torch::from_blob(inputs[batch].encoded_state.data(), {recurrent_flat_size_}, options).clone();
        Observation encoded_action = config_.action_representation(inputs[batch].action);
        actions[batch] =
            torch::from_blob(encoded_action.data(), {(int)encoded_action.size()}, options).clone();
    }

    // Reshape to expected size for network (batch_size, flat) -> (batch_size, c, h, w)
    encoded_observations =
        encoded_observations.reshape({batch_size, -1, encoded_obs_shape_.h, encoded_obs_shape_.w});
    actions = actions.reshape({batch_size, -1, encoded_obs_shape_.h, encoded_obs_shape_.w});

    // Move to device
    encoded_observations = encoded_observations.to(torch_device_);
    actions = actions.to(torch_device_);

    // Put model in eval mode for inference + scoped no_grad
    model_->eval();
    torch::NoGradGuard no_grad;
    model::InferenceOutput batched_output = model_->recurrent_inference(encoded_observations, actions);

    // Create batched decoded value/reward
    torch::Tensor value_decoded =
        value_encoder_.decode(batched_output.value).to(torch::kCPU).to(torch::kDouble);
    torch::Tensor reward_decoded =
        reward_encoder_.decode(batched_output.reward).to(torch::kCPU).to(torch::kDouble);

    std::vector<VPRNetModel::InferenceOutputs> outputs;
    outputs.reserve(batch_size);
    for (int batch = 0; batch < batch_size; ++batch) {
        // Inference output is a struct of batched tensors, need to extract/convert the tensors into the
        // proper vector data structures
        assert((int)value_decoded[batch].size(0) == 1);
        assert((int)reward_decoded[batch].size(0) == 1);
        double value = value_decoded[batch][0].item<double>();
        double reward = reward_decoded[batch][0].item<double>();
        // Inference uses a policy, so we softmax the logits here
        torch::Tensor batch_policy =
            torch::softmax(batched_output.policy_logits[batch], -1).to(torch::kCPU).to(torch::kDouble);
        std::vector<double> policy(batch_policy.data_ptr<double>(),
                                   batch_policy.data_ptr<double>() + batch_policy.numel());
        torch::Tensor batch_encoded_state =
            batched_output.encoded_state[batch].to(torch::kDouble).to(torch::kCPU);
        Observation encoded_state(batch_encoded_state.data_ptr<double>(),
                                  batch_encoded_state.data_ptr<double>() + batch_encoded_state.numel());
        outputs.push_back({value, reward, policy, encoded_state});
    }
    return outputs;
}

// Perform a learning step for the given batch
VPRNetModel::LossInfo VPRNetModel::Learn(std::vector<types::BatchItem>& inputs) {
    int batch_size = (int)inputs.size();
    int num_steps = config_.num_unroll_steps + 1;
    int num_actions = (int)config_.action_space.size();
    auto options = torch::TensorOptions().dtype(torch::kFloat);
    // Init tensors for batch items
    torch::Tensor stacked_observations = torch::empty({batch_size, initial_flat_size_}, options);
    torch::Tensor actions = torch::empty({batch_size, num_steps, action_flat_size_}, options);
    torch::Tensor target_values = torch::empty({batch_size, num_steps}, options);
    torch::Tensor target_rewards = torch::empty({batch_size, num_steps}, options);
    torch::Tensor target_policies = torch::empty({batch_size, num_steps, num_actions}, options);
    torch::Tensor gradient_scale = torch::empty({batch_size, num_steps}, options);
    torch::Tensor is_priorities = torch::empty({batch_size, 1}, options);

    // Place batch items into the above
    for (int batch = 0; batch < batch_size; ++batch) {
        stacked_observations[batch] =
            torch::from_blob(inputs[batch].stacked_observation.data(), {initial_flat_size_}, options).clone();
        for (int step = 0; step < num_steps; ++step) {
            Observation encoded_action = config_.action_representation(inputs[batch].actions[step]);
            actions[batch][step] =
                torch::from_blob(encoded_action.data(), {action_flat_size_}, options).clone();
            target_values[batch][step] = (float)inputs[batch].target_values[step];
            target_rewards[batch][step] = (float)inputs[batch].target_rewards[step];
            gradient_scale[batch][step] = (float)inputs[batch].gradient_scale[step];
            target_policies[batch][step] =
                torch::from_blob(inputs[batch].target_policies[step].data(), {num_actions}, {torch::kDouble})
                    .to(options)
                    .clone();
        }
        is_priorities[batch] = inputs[batch].priority;
    }

    // Move tensors to device
    stacked_observations = stacked_observations.to(torch_device_);
    actions = actions.to(torch_device_);
    target_values = target_values.to(torch_device_);
    target_rewards = target_rewards.to(torch_device_);
    target_policies = target_policies.to(torch_device_);
    gradient_scale = gradient_scale.to(torch_device_);
    is_priorities = is_priorities.to(torch_device_);

    // Reshape to expected size for network
    stacked_observations =
        stacked_observations.reshape({batch_size, -1, encoded_obs_shape_.h, encoded_obs_shape_.w});
    actions = actions.reshape({batch_size, num_steps, -1, encoded_obs_shape_.h, encoded_obs_shape_.w});

    // Convert the raw value/reward into the encoded support
    target_values = value_encoder_.encode(target_values);       // (batch, num_steps, value.support_size)
    target_rewards = reward_encoder_.encode(target_rewards);    // (batch, num_steps, reward.support_size)

    // Put model in train mode for learning
    model_->train();

    // Generate initial inference predictions
    model::InferenceOutput model_output = model_->initial_inference(stacked_observations);
    LossOutput loss_output =
        model_->loss(model_output.value, model_output.reward, model_output.policy_logits,
                     target_values.index({Slice(), 0}), target_rewards.index({Slice(), 0}),
                     target_policies.index({Slice(), 0}));
    torch::Tensor value_loss = loss_output.value_loss;
    torch::Tensor reward_loss = torch::zeros_like(loss_output.reward_loss);
    torch::Tensor policy_loss = loss_output.policy_loss;

    // Get updated errors for updating the prioritized replay buffer
    // |search value - observed n_step value|
    std::vector<double> errors;
    std::vector<int> indices;
    torch::Tensor initial_values =
        value_encoder_.decode(model_output.value).to(torch::kCPU).to(torch::kDouble);
    for (int batch = 0; batch < batch_size; ++batch) {
        errors.push_back(initial_values[batch].item<double>() - inputs[batch].target_values[0]);
        indices.push_back(inputs[batch].index);
    }

    // Generate recurrent predictions
    torch::Tensor prev_state = model_output.encoded_state;
    for (int step = 1; step < num_steps; ++step) {
        model::InferenceOutput model_output =
            model_->recurrent_inference(prev_state, actions.index({Slice(), step}));
        // Scale the gradient at the start of the dynamics function (See paper Appendix G Training)
        model_output.encoded_state.register_hook([](torch::Tensor grad) { return grad * 0.5; });
        LossOutput loss_output =
            model_->loss(model_output.value, model_output.reward, model_output.policy_logits,
                         target_values.index({Slice(), step}), target_rewards.index({Slice(), step}),
                         target_policies.index({Slice(), step}));
        // Scale gradients by the number of unroll steps (See paper Appendix G Training)
        loss_output.value_loss.register_hook([&](torch::Tensor grad) {
            return grad * gradient_scale.index({Slice(), 1});
        });
        loss_output.reward_loss.register_hook([&](torch::Tensor grad) {
            return grad * gradient_scale.index({Slice(), 1});
        });
        loss_output.policy_loss.register_hook([&](torch::Tensor grad) {
            return grad * gradient_scale.index({Slice(), 1});
        });
        value_loss = value_loss + loss_output.value_loss;
        reward_loss = reward_loss + loss_output.reward_loss;
        policy_loss = policy_loss + loss_output.policy_loss;
        prev_state = model_output.encoded_state;
    }

    // Save non-scaled losses
    double value_loss_scalar = value_loss.mean().item<double>();
    double policy_loss_scalar = policy_loss.mean().item<double>();
    double reward_loss_scalar = reward_loss.mean().item<double>();

    // Scale loss by correcting for bias introduced by PER using the importance sampling weights
    value_loss = (is_priorities * value_loss).mean();
    reward_loss = (is_priorities * reward_loss).mean();
    policy_loss = (is_priorities * policy_loss).mean();

    // Add losses
    torch::Tensor total_loss = value_loss * value_loss_weight_ + reward_loss + policy_loss;

    // Optimize model
    model_->zero_grad();
    total_loss.backward();
    model_optimizer_.step();

    return {total_loss.item<double>(), value_loss_scalar, policy_loss_scalar,
            reward_loss_scalar,        indices,           errors};
}

}    // namespace muzero_cpp
