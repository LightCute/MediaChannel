#pragma once
#include <rtc/rtc.hpp>
#include <cstdint>

// 纯数据载体，无任何业务逻辑
struct VideoFrame {
    rtc::binary data;          // H264裸数据（AVCC格式）
    uint64_t timestamp_us;     // 时间戳(us)
    bool isIdr;                // 是否关键帧
};