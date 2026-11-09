#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    // 释放 AFE 占用。默认空实现；对话 AFE 必须在唤醒词 AFE 创建前拆掉，反之亦然。
    virtual void Deinitialize() {}
    virtual bool IsReady() const { return true; }
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
};

#endif
