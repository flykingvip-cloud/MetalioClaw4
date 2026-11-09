#include "afe_audio_processor.h"
#include <esp_heap_caps.h>
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

    if (afe_data_ != nullptr) {
        return;
    }

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
    // 大块权重在 PSRAM（2.0.53 实测约 848KB）。剩下的内部堆不能落到 LP/RTC：
    // P4 上 CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP 会把 0x5010xxxx 并进 heap，
    // LSTM 的向量 load 访问这块会 Load access fault。
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

    ESP_LOGI(TAG, "Creating chat AFE, internal=%uKB psram=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_ != nullptr ? afe_iface_->create_from_config(afe_config) : nullptr;
    afe_config_free(afe_config);
    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "create_from_config failed, internal=%uKB psram=%uKB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Chat AFE created, internal=%uKB psram=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    if (fetch_task_ == nullptr) {
        // 栈只跑 fetch 循环和输出回调；LSTM 在 AFE 内部任务。4KB 偏紧，放到 8KB。
        xTaskCreate([](void* arg) {
            auto this_ = (AfeAudioProcessor*)arg;
            this_->AudioProcessorTask();
            vTaskDelete(NULL);
        }, "audio_communication", 8192, this, 3, &fetch_task_);
    }
}

void AfeAudioProcessor::Deinitialize() {
    Stop();
    for (int i = 0; i < 40 && fetching_.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (fetching_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Deinitialize: fetch still in progress, destroying anyway");
    }

    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Destroying chat AFE");
    afe_iface_->destroy(afe_data_);
    afe_data_ = nullptr;
    afe_iface_ = nullptr;
}

bool AfeAudioProcessor::IsReady() const {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    return afe_data_ != nullptr && afe_iface_ != nullptr;
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    // 新一轮聆听不要继承上一轮的“正在说话”，否则一进来就因静音立刻 listen.stop。
    is_speaking_ = false;
    fetch_ok_ = false;

    size_t chunk = 0;
    int channels = 1;
    {
        std::lock_guard<std::mutex> lock(afe_mutex_);
        if (afe_data_ != nullptr && afe_iface_ != nullptr) {
            if (afe_iface_->reset_vad != nullptr) {
                afe_iface_->reset_vad(afe_data_);
            }
            chunk = afe_iface_->get_feed_chunksize(afe_data_);
            if (codec_ != nullptr && codec_->input_channels() > 0) {
                channels = codec_->input_channels();
            }
        }
    }

    // 输入任务有约 120ms 预热，fetch 超时只有 100ms。先灌静音，避免第一帧
    // 打在空 ring 上（2.0.53：empty → ESP_FAIL → reset_buffer → LSTM panic）。
    if (chunk > 0) {
        constexpr int kPrimeChunks = 4;
        std::vector<int16_t> silence(chunk * static_cast<size_t>(channels), 0);
        for (int i = 0; i < kPrimeChunks; ++i) {
            Feed(std::vector<int16_t>(silence));
        }
        ESP_LOGI(TAG, "Primed %d silent chunks (%u samples/ch) before fetch",
                 kPrimeChunks, static_cast<unsigned>(chunk));
    }

    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ != nullptr && afe_iface_ != nullptr) {
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
    int fetch_size = 0;
    int feed_size = 0;
    {
        std::lock_guard<std::mutex> lock(afe_mutex_);
        if (afe_iface_ != nullptr && afe_data_ != nullptr) {
            fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
            feed_size = afe_iface_->get_feed_chunksize(afe_data_);
        }
    }
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        const esp_afe_sr_iface_t* iface = nullptr;
        esp_afe_sr_data_t* data = nullptr;
        {
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_iface_ == nullptr || afe_data_ == nullptr) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            fetching_.store(true, std::memory_order_release);
            iface = afe_iface_;
            data = afe_data_;
        }

        // 有限等待，Deinitialize 才能在停喂之后返回，而不是卡在 portMAX_DELAY。
        auto res = iface->fetch_with_delay(data, pdMS_TO_TICKS(100));
        fetching_.store(false, std::memory_order_release);

        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr) {
            continue;
        }
        if (res->ret_value == ESP_FAIL) {
            // 空 ring（还没有任何成功帧）是启动空窗，不是溢出。此时 reset_buffer
            // 会重入尚未喂数的 LSTM，2.0.53 在这里 Load access fault。
            if (!fetch_ok_) {
                static int empty_fails = 0;
                ++empty_fails;
                if (empty_fails == 1 || empty_fails % 50 == 0) {
                    ESP_LOGW(TAG, "AFE fetch empty (ESP_FAIL), skip reset (%d)", empty_fails);
                }
                continue;
            }
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_data_ != nullptr && afe_iface_ != nullptr) {
                afe_iface_->reset_buffer(afe_data_);
            }
            static int fetch_failures = 0;
            ++fetch_failures;
            if (fetch_failures == 1 || fetch_failures % 50 == 0) {
                ESP_LOGW(TAG, "AFE fetch failed (ESP_FAIL), reset feed ring (%d)", fetch_failures);
            }
            fetch_ok_ = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        fetch_ok_ = true;

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
    std::lock_guard<std::mutex> lock(afe_mutex_);
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
