/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— CameraCharacteristics 构造。
 *
 * ⚠️ 这里每一个"必填"的 tag 都不是凭记忆列的：清单来自 AOSP 自己的
 *    CameraCharacteristics 文档与 libcamera 的 src/android/camera_capabilities.cpp
 *    （它为 camera3 做的是同一件事）。缺 tag 的后果不是报错，而是
 *    **相机被框架静默丢弃** —— 所以宁可多填。
 */
#include "Metadata.h"

#include <algorithm>
#include <cstring>

#include <log/log.h>
#include <system/graphics.h>

namespace gaokun3 {

std::vector<uint8_t> pack(const camera_metadata_t *m)
{
	size_t n = get_camera_metadata_compact_size(m);
	std::vector<uint8_t> out(n);
	camera_metadata_t *dst = copy_camera_metadata(out.data(), n, m);
	if (!dst) {
		ALOGE("pack: copy_camera_metadata 失败 (size=%zu)", n);
		out.clear();
	}
	return out;
}

namespace {

/* 我们对外声明支持的像素格式。
 *
 * ★ 软件 ISP 只出 RGB 族（libcamera debayer_cpu.cpp:436-441），
 *   Android 要的是 YUV_420_888 / JPEG / IMPLEMENTATION_DEFINED，
 *   所以这一层的色彩转换由本 HAL 用 libyuv 做，JPEG 用 libjpeg 做。
 *   ⇒ 这里声明的是【我们能交付的】格式，不是 libcamera 直接产出的格式。
 */
constexpr int32_t kFormats[] = {
	HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
	HAL_PIXEL_FORMAT_YCBCR_420_888,
	HAL_PIXEL_FORMAT_BLOB,          /* = JPEG */
};

void addStreamConfigs(camera_metadata_t **m, const SensorFacts &f)
{
	std::vector<int32_t> cfgs;
	for (int32_t fmt : kFormats) {
		for (const auto &s : f.outputSizes) {
			cfgs.push_back(fmt);
			cfgs.push_back(s.first);
			cfgs.push_back(s.second);
			cfgs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
		}
	}
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
				  cfgs.data(), cfgs.size());

	std::vector<int64_t> dur, st;
	for (int32_t fmt : kFormats) {
		for (const auto &s : f.outputSizes) {
			dur.push_back(fmt);
			dur.push_back(s.first);
			dur.push_back(s.second);
			dur.push_back(f.minFrameDurationNs);

			st.push_back(fmt);
			st.push_back(s.first);
			st.push_back(s.second);
			/* ★ 只有 BLOB(JPEG) 有 stall —— 软件 JPEG 编码是阻塞的。
			 *   其余格式必须是 0，否则框架会以为每帧都要停顿。 */
			st.push_back(fmt == HAL_PIXEL_FORMAT_BLOB ? 200000000LL : 0LL);
		}
	}
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
				  dur.data(), dur.size());
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
				  st.data(), st.size());
}

} /* namespace */

std::vector<uint8_t> buildCharacteristics(const SensorFacts &f)
{
	/* 条目数与数据量给足，add_ 失败是静默的，宁可浪费几 KB。 */
	camera_metadata_t *m = allocate_camera_metadata(128, 16384);
	if (!m) {
		ALOGE("allocate_camera_metadata 失败");
		return {};
	}

	/* ── 传感器几何 ── */
	const int32_t activeArray[] = { 0, 0, f.activeW, f.activeH };
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
				  activeArray, 4);
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
				  activeArray, 4);
	const int32_t pixelArray[] = { f.pixelArrayW, f.pixelArrayH };
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
				  pixelArray, 2);
	const int32_t orientation = f.orientation;
	add_camera_metadata_entry(m, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

	const uint8_t facing = f.frontFacing ? ANDROID_LENS_FACING_FRONT
					     : ANDROID_LENS_FACING_BACK;
	add_camera_metadata_entry(m, ANDROID_LENS_FACING, &facing, 1);

	/* [raw, processed(非stall), processedStall] —— 我们不提供 RAW。
	 * ⚠️ 不用复合字面量：那是 GNU 扩展，-Werror 下会被拦。 */
	const int32_t maxOutStreams[] = { 0, 2, 1 };
	add_camera_metadata_entry(m, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
				  maxOutStreams, 3);

	/* ── 流配置 ── */
	addStreamConfigs(&m, f);

	const int64_t maxFrameDuration = 1000000000LL;   /* 1 s，与最长曝光一致 */
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
				  &maxFrameDuration, 1);

	/* ── 能力声明：只做 BACKWARD_COMPATIBLE，硬件等级 LIMITED ──
	 * ⚠️ 不要声明 LEGACY：LEGACY 走的是框架里那条兼容路径，对 HAL 的
	 *    行为假设更多更旧；LIMITED 才是"实现了 camera2 基本面"的正确说法。 */
	const uint8_t caps[] = { ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE };
	add_camera_metadata_entry(m, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, caps, 1);
	const uint8_t level = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
	add_camera_metadata_entry(m, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &level, 1);

	const int32_t partialCount = 1;
	add_camera_metadata_entry(m, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialCount, 1);
	const uint8_t pipelineDepth = 4;
	add_camera_metadata_entry(m, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineDepth, 1);

	/* ── 3A：软件 ISP 只有 AE/AWB，没有 AF（定焦镜头） ── */
	const uint8_t aeModes[] = { ANDROID_CONTROL_AE_MODE_ON };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModes, 1);
	const uint8_t awbModes[] = { ANDROID_CONTROL_AWB_MODE_AUTO };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes, 1);
	const uint8_t afModes[] = { ANDROID_CONTROL_AF_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModes, 1);
	const uint8_t controlModes[] = { ANDROID_CONTROL_MODE_AUTO };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_MODES, controlModes, 1);
	const uint8_t sceneModes[] = { ANDROID_CONTROL_SCENE_MODE_DISABLED };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, sceneModes, 1);
	const uint8_t effects[] = { ANDROID_CONTROL_EFFECT_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_EFFECTS, effects, 1);
	const uint8_t antibanding[] = { ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
				  antibanding, 1);
	const uint8_t vstabModes[] = { ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
				  vstabModes, 1);
	const int32_t aeCompRange[] = { 0, 0 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_COMPENSATION_RANGE, aeCompRange, 2);
	const camera_metadata_rational aeCompStep = { 1, 1 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_COMPENSATION_STEP, &aeCompStep, 1);
	const uint8_t aeLockAvail = ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_LOCK_AVAILABLE, &aeLockAvail, 1);
	const uint8_t awbLockAvail = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awbLockAvail, 1);

	/* 帧率区间：软件去拜耳 8 MP 跑不快，如实声明。 */
	const int32_t fpsRanges[] = { 5, 15, 15, 15 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
				  fpsRanges, 4);

	const int32_t maxRegions[] = { 0, 0, 0 };   /* AE/AWB/AF 都不支持区域 */
	add_camera_metadata_entry(m, ANDROID_CONTROL_MAX_REGIONS, maxRegions, 3);

	/* ── 闪光灯 / 镜头 ── */
	const uint8_t flashAvailable = ANDROID_FLASH_INFO_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_FLASH_INFO_AVAILABLE, &flashAvailable, 1);
	const float focalLengths[] = { 3.0f };       /* ⚠️ 占位值，未测 */
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
				  focalLengths, 1);
	const float apertures[] = { 2.2f };          /* ⚠️ 占位值，未测 */
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_APERTURES, apertures, 1);
	const float filterDensities[] = { 0.0f };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
				  filterDensities, 1);
	const uint8_t opticalStab[] = { ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
				  opticalStab, 1);
	const float hyperfocal = 0.0f;
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperfocal, 1);
	const float minFocus = 0.0f;                 /* 0 = 定焦 */
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &minFocus, 1);
	const uint8_t focusCalib = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
				  &focusCalib, 1);

	/* ── 缩放 / 裁剪 ── */
	const float maxZoom = 1.0f;
	add_camera_metadata_entry(m, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);
	const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
	add_camera_metadata_entry(m, ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

	/* ── JPEG ── */
	const int32_t thumbSizes[] = { 0, 0, 160, 120 };
	add_camera_metadata_entry(m, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumbSizes, 4);
	const int32_t maxJpegSize = f.activeW * f.activeH * 3 / 2 + 65536;
	add_camera_metadata_entry(m, ANDROID_JPEG_MAX_SIZE, &maxJpegSize, 1);

	/* ── 统计 ── */
	const uint8_t faceModes[] = { ANDROID_STATISTICS_FACE_DETECT_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
				  faceModes, 1);
	const int32_t maxFaces = 0;
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaces, 1);
	const uint8_t hotPixelMapModes[] = { ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
				  hotPixelMapModes, 1);
	const uint8_t lensShadingMapModes[] = { ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
				  lensShadingMapModes, 1);

	/* ── 时间戳 / 同步 ──
	 * ⚠️ UNKNOWN 而不是 REALTIME：libcamera 给的是 CLOCK_MONOTONIC，
	 *    而 REALTIME 在 Android 指的是 CLOCK_BOOTTIME（含休眠时间）。
	 *    声明错了会让框架把时间戳与其它传感器错误对齐。 */
	const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);
	const int32_t syncLatency = ANDROID_SYNC_MAX_LATENCY_UNKNOWN;
	add_camera_metadata_entry(m, ANDROID_SYNC_MAX_LATENCY, &syncLatency, 1);

	/* ── 其余必填 ── */
	const uint8_t noiseModes[] = { ANDROID_NOISE_REDUCTION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
				  noiseModes, 1);
	const uint8_t edgeModes[] = { ANDROID_EDGE_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edgeModes, 1);
	const uint8_t abModes[] = { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
				  abModes, 1);
	const uint8_t tonemapModes[] = { ANDROID_TONEMAP_MODE_FAST };
	add_camera_metadata_entry(m, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, tonemapModes, 1);
	const int32_t maxCurvePoints = 2;
	add_camera_metadata_entry(m, ANDROID_TONEMAP_MAX_CURVE_POINTS, &maxCurvePoints, 1);
	const uint8_t testPatternModes[] = { ANDROID_SENSOR_TEST_PATTERN_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
				  testPatternModes, 1);
	const uint8_t shadingModes[] = { ANDROID_SHADING_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_SHADING_AVAILABLE_MODES, shadingModes, 1);

	std::vector<uint8_t> out = pack(m);
	free_camera_metadata(m);
	ALOGI("characteristics 构造完成：%zu 字节", out.size());
	return out;
}

std::vector<uint8_t> buildDefaultRequest(int templateId, const SensorFacts &f)
{
	(void)templateId;
	camera_metadata_t *m = allocate_camera_metadata(32, 2048);
	if (!m)
		return {};

	const uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
	add_camera_metadata_entry(m, ANDROID_CONTROL_MODE, &controlMode, 1);
	const uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
	const uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
	const uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_MODE, &afMode, 1);
	const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
	add_camera_metadata_entry(m, ANDROID_FLASH_MODE, &flashMode, 1);
	const int32_t fps[] = { 5, 15 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fps, 2);
	const int32_t cropRegion[] = { 0, 0, f.activeW, f.activeH };
	add_camera_metadata_entry(m, ANDROID_SCALER_CROP_REGION, cropRegion, 4);
	const uint8_t jpegQuality = 90;
	add_camera_metadata_entry(m, ANDROID_JPEG_QUALITY, &jpegQuality, 1);

	std::vector<uint8_t> out = pack(m);
	free_camera_metadata(m);
	return out;
}

} /* namespace gaokun3 */
