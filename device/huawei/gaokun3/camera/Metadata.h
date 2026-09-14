/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— CameraCharacteristics 构造。
 *
 * ★ 这一层之所以便宜，是因为 AIDL 的 CameraMetadata 就是
 *   camera_metadata_t 的序列化 blob（CameraMetadata.aidl 原文：
 *   "Access by casting to a camera_metadata*"），所以我们直接用
 *   AOSP 的 libcamera_metadata 建表，再把字节丢进 AIDL。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <system/camera_metadata.h>

namespace gaokun3 {

/* 从 libcamera 那边拿到的、构造 characteristics 需要的硬件事实。 */
struct SensorFacts {
	int32_t pixelArrayW = 0;
	int32_t pixelArrayH = 0;
	int32_t activeW = 0;
	int32_t activeH = 0;
	int32_t orientation = 0;        /* 0/90/180/270 */
	bool    frontFacing = true;
	/* 闪光灯的 LED class 目录（如 /sys/class/leds/white:flash）；空 = 这个相机没有闪光灯。
	 * ★ #110：后摄的 LED 挂在 PM8350C 闪光模块 1+4 路，内核侧只暴露成一个 LED。 */
	std::string flashLed;
	int64_t minFrameDurationNs = 33333333;
	/* 我们打算对外声明的输出尺寸（软件 ISP 产出，见 Provider.cpp 的说明）。 */
	std::vector<std::pair<int32_t, int32_t>> outputSizes;
};

/*
 * 建一份 static characteristics。
 * 返回的 vector 就是可以直接塞进 aidl CameraMetadata.metadata 的字节。
 */
std::vector<uint8_t> buildCharacteristics(const SensorFacts &f);

/*
 * 构造一帧的【结果】元数据。
 * ⚠️ 不能只把请求设置原样回传：框架要求结果里带上 SENSOR_TIMESTAMP 与几个
 *    3A 状态，缺了会让 CameraCaptureSession 判定这帧无效。
 */
struct FrameResultFacts {
	uint8_t flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
	uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
	/* 请求里可能不带这两项（沿用上一帧），结果里要按会话的粘滞值回显。 */
	uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
	uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
};
std::vector<uint8_t> buildResult(const std::vector<uint8_t> &requestSettings,
				 int64_t timestampNs, uint8_t pipelineDepth,
				 const FrameResultFacts &fr);

/* RequestTemplate → 默认请求设置。 */
std::vector<uint8_t> buildDefaultRequest(int templateId, const SensorFacts &f);

/* 小工具：把 camera_metadata_t 打包成字节。 */
std::vector<uint8_t> pack(const camera_metadata_t *m);

} /* namespace gaokun3 */
