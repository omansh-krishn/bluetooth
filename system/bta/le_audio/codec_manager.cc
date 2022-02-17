/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "codec_manager.h"

#include "client_audio.h"
#include "device/include/controller.h"
#include "osi/include/log.h"
#include "osi/include/properties.h"
#include "stack/acl/acl.h"
#include "stack/include/acl_api.h"
#include "le_audio_set_configuration_provider.h"

namespace {

using bluetooth::hci::iso_manager::kIsoDataPathHci;
using bluetooth::hci::iso_manager::kIsoDataPathPlatformDefault;
using le_audio::CodecManager;
using le_audio::types::CodecLocation;

using bluetooth::le_audio::btle_audio_codec_config_t;
using bluetooth::le_audio::btle_audio_codec_index_t;
using le_audio::AudioSetConfigurationProvider;
using le_audio::set_configurations::AudioSetConfiguration;
using le_audio::set_configurations::AudioSetConfigurations;
using le_audio::set_configurations::SetConfiguration;

}  // namespace

namespace le_audio {

struct codec_manager_impl {
 public:
  codec_manager_impl(
      const std::vector<btle_audio_codec_config_t>& offloading_preference,
      const std::vector<AudioSetConfiguration>& adsp_capabilities) {
    offload_enable_ = osi_property_get_bool(
                          "ro.bluetooth.leaudio_offload.supported", false) &&
                      osi_property_get_bool(
                          "persist.bluetooth.leaudio_offload.enabled", true);
    if (offload_enable_ == false) {
      LOG_INFO("offload disabled");
      return;
    }

    if (!LeAudioHalVerifier::SupportsLeAudioHardwareOffload()) {
      LOG_WARN("HAL not support hardware offload");
      return;
    }

    if (!controller_get_interface()->supports_configure_data_path()) {
      LOG_WARN("Controller does not support config data path command");
      return;
    }

    LOG_INFO("LeAudioCodecManagerImpl: configure_data_path for encode");
    btm_configure_data_path(btm_data_direction::HOST_TO_CONTROLLER,
                            kIsoDataPathPlatformDefault, {});
    btm_configure_data_path(btm_data_direction::CONTROLLER_TO_HOST,
                            kIsoDataPathPlatformDefault, {});
    UpdateOffloadCapability(offloading_preference, adsp_capabilities);
    SetCodecLocation(CodecLocation::ADSP);
  }
  ~codec_manager_impl() {
    if (GetCodecLocation() != CodecLocation::HOST) {
      btm_configure_data_path(btm_data_direction::HOST_TO_CONTROLLER,
                              kIsoDataPathHci, {});
      btm_configure_data_path(btm_data_direction::CONTROLLER_TO_HOST,
                              kIsoDataPathHci, {});
    }
  }
  CodecLocation GetCodecLocation(void) const { return codec_location_; }

  void UpdateActiveSourceAudioConfig(
      const le_audio::stream_configuration& stream_conf, uint16_t delay_ms) {
    if (stream_conf.sink_streams.empty()) return;

    sink_config.stream_map = std::move(stream_conf.sink_streams);
    // TODO: set the default value 16 for now, would change it if we support
    // mode bits_per_sample
    sink_config.bits_per_sample = 16;
    sink_config.sampling_rate = stream_conf.sink_sample_frequency_hz;
    sink_config.frame_duration = stream_conf.sink_frame_duration_us;
    sink_config.octets_per_frame = stream_conf.sink_octets_per_codec_frame;
    // TODO: set the default value 1 for now, would change it if we need more
    // configuration
    sink_config.blocks_per_sdu = 1;
    sink_config.peer_delay_ms = delay_ms;
    LeAudioClientAudioSource::UpdateAudioConfigToHal(sink_config);
  }

  void UpdateActiveSinkAudioConfig(
      const le_audio::stream_configuration& stream_conf, uint16_t delay_ms) {
    if (stream_conf.source_streams.empty()) return;

    source_config.stream_map = std::move(stream_conf.source_streams);
    // TODO: set the default value 16 for now, would change it if we support
    // mode bits_per_sample
    source_config.bits_per_sample = 16;
    source_config.sampling_rate = stream_conf.source_sample_frequency_hz;
    source_config.frame_duration = stream_conf.source_frame_duration_us;
    source_config.octets_per_frame = stream_conf.source_octets_per_codec_frame;
    // TODO: set the default value 1 for now, would change it if we need more
    // configuration
    source_config.blocks_per_sdu = 1;
    source_config.peer_delay_ms = delay_ms;
    LeAudioClientAudioSink::UpdateAudioConfigToHal(source_config);
  }

  const AudioSetConfigurations* GetOffloadCodecConfig(
      types::LeAudioContextType ctx_type) {
    return &context_type_offload_config_map_[ctx_type];
  }

 private:
  void SetCodecLocation(CodecLocation location) {
    if (offload_enable_ == false) return;
    codec_location_ = location;
  }

  bool IsLc3ConfigMatched(
      const set_configurations::CodecCapabilitySetting& adsp_config,
      const set_configurations::CodecCapabilitySetting& target_config) {
    if (adsp_config.id.coding_format != types::kLeAudioCodingFormatLC3 ||
        target_config.id.coding_format != types::kLeAudioCodingFormatLC3) {
      return false;
    }

    const types::LeAudioLc3Config adsp_lc3_config =
        std::get<types::LeAudioLc3Config>(adsp_config.config);
    const types::LeAudioLc3Config target_lc3_config =
        std::get<types::LeAudioLc3Config>(target_config.config);

    if (adsp_lc3_config.sampling_frequency !=
            target_lc3_config.sampling_frequency ||
        adsp_lc3_config.frame_duration != target_lc3_config.frame_duration ||
        adsp_lc3_config.channel_count != target_lc3_config.channel_count ||
        adsp_lc3_config.octets_per_codec_frame !=
            target_lc3_config.octets_per_codec_frame) {
      return false;
    }

    return true;
  }

  bool IsSetConfigurationMatched(const SetConfiguration& software_set_config,
                                 const SetConfiguration& adsp_set_config) {
    // Skip the check of stategry and ase_cnt due to ADSP doesn't have the info
    return (
        software_set_config.direction == adsp_set_config.direction &&
        software_set_config.device_cnt == adsp_set_config.device_cnt &&
        IsLc3ConfigMatched(software_set_config.codec, adsp_set_config.codec));
  }

  bool IsAudioSetConfigurationMatched(
      const AudioSetConfiguration* software_audio_set_conf,
      std::unordered_set<uint8_t>& offload_preference_set,
      const std::vector<AudioSetConfiguration>& adsp_capabilities) {
    if (software_audio_set_conf->confs.empty()) {
      return false;
    }

    std::unordered_map<uint8_t, const SetConfiguration&>
        software_set_conf_direction_map;

    for (auto& software_set_conf : software_audio_set_conf->confs) {
      // Checks offload preference supports the codec
      if (offload_preference_set.find(
              software_set_conf.codec.id.coding_format) ==
          offload_preference_set.end()) {
        return false;
      }
      software_set_conf_direction_map.emplace(software_set_conf.direction,
                                              software_set_conf);
    }

    // Checks any of offload config matches the input audio set config
    for (const auto& adsp_audio_set_conf : adsp_capabilities) {
      if (adsp_audio_set_conf.confs.size() !=
          software_audio_set_conf->confs.size()) {
        continue;
      }

      size_t match_cnt = 0;

      for (auto& adsp_set_conf : adsp_audio_set_conf.confs) {
        auto it = software_set_conf_direction_map.find(adsp_set_conf.direction);

        if (it == software_set_conf_direction_map.end()) {
          continue;
        }

        if (IsSetConfigurationMatched(it->second, adsp_set_conf)) {
          match_cnt++;
        }
      }

      if (match_cnt == software_set_conf_direction_map.size()) {
        return true;
      }
    }

    return false;
  }

  void UpdateOffloadCapability(
      const std::vector<btle_audio_codec_config_t>& offloading_preference,
      const std::vector<AudioSetConfiguration>& adsp_capabilities) {
    LOG(INFO) << __func__;
    std::unordered_set<uint8_t> offload_preference_set;

    if (AudioSetConfigurationProvider::Get() == nullptr) {
      LOG(ERROR) << __func__ << " Audio set configuration provider is not available.";
      return;
    }

    for (auto codec : offloading_preference) {
      auto it = btle_audio_codec_type_map_.find(codec.codec_type);

      if (it != btle_audio_codec_type_map_.end()) {
        offload_preference_set.insert(it->second);
      }
    }

    for (types::LeAudioContextType ctx_type :
         types::kLeAudioContextAllTypesArray) {
      // Gets the software supported context type and the corresponding config
      // priority
      const AudioSetConfigurations* software_audio_set_confs =
          AudioSetConfigurationProvider::Get()->GetConfigurations(ctx_type);

      for (const auto& software_audio_set_conf : *software_audio_set_confs) {
        if (IsAudioSetConfigurationMatched(software_audio_set_conf,
                                           offload_preference_set,
                                           adsp_capabilities)) {
          LOG(INFO) << "Offload supported conf, context type: " << (int)ctx_type
                    << ", settings -> " << software_audio_set_conf->name;
          context_type_offload_config_map_[ctx_type].push_back(
              software_audio_set_conf);
        }
      }
    }
  }

  CodecLocation codec_location_ = CodecLocation::HOST;
  bool offload_enable_ = false;
  le_audio::offload_config sink_config;
  le_audio::offload_config source_config;
  std::unordered_map<types::LeAudioContextType, AudioSetConfigurations>
      context_type_offload_config_map_;
  std::unordered_map<btle_audio_codec_index_t, uint8_t>
      btle_audio_codec_type_map_ = {
          {::bluetooth::le_audio::LE_AUDIO_CODEC_INDEX_SOURCE_LC3,
           types::kLeAudioCodingFormatLC3}};
};

struct CodecManager::impl {
  impl(const CodecManager& codec_manager) : codec_manager_(codec_manager) {}

  void Start(
      const std::vector<btle_audio_codec_config_t>& offloading_preference,
      const std::vector<set_configurations::AudioSetConfiguration>&
          adsp_capabilities) {
    LOG_ASSERT(!codec_manager_impl_);
    codec_manager_impl_ = std::make_unique<codec_manager_impl>(
        offloading_preference, adsp_capabilities);
  }

  void Stop() {
    LOG_ASSERT(codec_manager_impl_);
    codec_manager_impl_.reset();
  }

  bool IsRunning() { return codec_manager_impl_ ? true : false; }

  const CodecManager& codec_manager_;
  std::unique_ptr<codec_manager_impl> codec_manager_impl_;
};

CodecManager::CodecManager() : pimpl_(std::make_unique<impl>(*this)) {}

void CodecManager::Start(
    const std::vector<btle_audio_codec_config_t>& offloading_preference,
    const std::vector<set_configurations::AudioSetConfiguration>&
        adsp_capabilities) {
  if (!pimpl_->IsRunning())
    pimpl_->Start(offloading_preference, adsp_capabilities);
}

void CodecManager::Stop() {
  if (pimpl_->IsRunning()) pimpl_->Stop();
}

types::CodecLocation CodecManager::GetCodecLocation(void) const {
  if (!pimpl_->IsRunning()) {
    return CodecLocation::HOST;
  }

  return pimpl_->codec_manager_impl_->GetCodecLocation();
}

void CodecManager::UpdateActiveSourceAudioConfig(
    const stream_configuration& stream_conf, uint16_t delay_ms) {
  if (pimpl_->IsRunning())
    pimpl_->codec_manager_impl_->UpdateActiveSourceAudioConfig(stream_conf,
                                                               delay_ms);
}

void CodecManager::UpdateActiveSinkAudioConfig(
    const stream_configuration& stream_conf, uint16_t delay_ms) {
  if (pimpl_->IsRunning())
    pimpl_->codec_manager_impl_->UpdateActiveSinkAudioConfig(stream_conf,
                                                             delay_ms);
}

const AudioSetConfigurations* CodecManager::GetOffloadCodecConfig(
    types::LeAudioContextType ctx_type) {
  if (pimpl_->IsRunning()) {
    return pimpl_->codec_manager_impl_->GetOffloadCodecConfig(ctx_type);
  }

  return nullptr;
}

}  // namespace le_audio
