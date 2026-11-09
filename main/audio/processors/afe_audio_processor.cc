#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    // 设备侧断句：连续静音达到该值才报 VAD_SILENCE。100ms 会在词间把句子切断。
    afe_config->vad_min_noise_ms = 800;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    } else {
        ESP_LOGW(TAG, "vadnet model not in srmodels, falling back to WebRTC VAD");
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
        ESP_LOGW(TAG, "nsnet model not in srmodels, noise suppression stays off");
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // 内部 AFE 任务与 feed（core0 / 较高优先级）分开，避免 FEED ring 只进不出。
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 5;

    // enable_vad / enable_aec 只对 create 时打开的模块生效。vad_init=false 时
    // enable_vad 会报 "VAD is not initialized"，断句永远不会发生。
    // AEC 与 VAD 同时创建：打断只额外打开 AEC，VAD 保持可用。
    afe_config->aec_init = true;
    afe_config->vad_init = true;
    ESP_LOGI(TAG, "Chat AFE models: vad=%s ns=%s",
             vad_model_name != nullptr ? vad_model_name : "webrtc",
             ns_model_name != nullptr ? ns_model_name : "off");

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_ != nullptr ? afe_iface_->create_from_config(afe_config) : nullptr;
    afe_config_free(afe_config);
    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return;
    }

    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 3, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    // 新一轮聆听不要继承上一轮的“正在说话”，否则一进来就因静音立刻 listen.stop。
    is_speaking_ = false;
    if (afe_data_ != nullptr && afe_iface_ != nullptr && afe_iface_->reset_vad != nullptr) {
        afe_iface_->reset_vad(afe_data_);
    }
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            // fetch 失败时 FEED ring 不会被消费，feed() 会周期性报 ringbuffer full。
            // 丢掉积压，让后续音频重新对齐，而不是一直覆盖旧帧。
            if (afe_data_ != nullptr) {
                afe_iface_->reset_buffer(afe_data_);
            }
            static int fetch_failures = 0;
            ++fetch_failures;
            if (fetch_failures == 1 || fetch_failures % 50 == 0) {
                ESP_LOGW(TAG, "AFE fetch failed (%s), reset feed ring (%d)",
                         res == nullptr ? "null" : "ESP_FAIL", fetch_failures);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "EnableDeviceAec: AFE not initialized");
        return;
    }
    // VAD 始终开着，打断与否都能在静音后断句。AEC 只在打断打开时启用。
    afe_iface_->enable_vad(afe_data_);
    if (enable) {
        afe_iface_->enable_aec(afe_data_);
    } else {
        afe_iface_->disable_aec(afe_data_);
    }
}
