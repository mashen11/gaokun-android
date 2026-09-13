/* SPDX-License-Identifier: Apache-2.0 */
#include "Device.h"
#include "Session.h"

#include <log/log.h>

#include <libcamera/property_ids.h>
#include <libcamera/stream.h>

using ::aidl::android::hardware::camera::common::CameraResourceCost;
using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraDeviceSession;
using ::aidl::android::hardware::camera::device::ICameraInjectionSession;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::StreamConfiguration;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}
} /* namespace */

Device::Device(std::shared_ptr<libcamera::Camera> cam, std::string name)
	: cam_(std::move(cam)), name_(std::move(name))
{
}

Device::~Device() = default;

bool Device::init()
{
	const libcamera::ControlList &props = cam_->properties();

	/* ── 传感器阵列尺寸 ── */
	auto pixelArray = props.get(libcamera::properties::PixelArraySize);
	if (pixelArray) {
		facts_.pixelArrayW = pixelArray->width;
		facts_.pixelArrayH = pixelArray->height;
	} else {
		ALOGW("相机没有报 PixelArraySize");
	}

	/* 有效区优先用 PixelArrayActiveAreas，没有就退回全阵列。 */
	auto active = props.get(libcamera::properties::PixelArrayActiveAreas);
	if (active && !active->empty()) {
		facts_.activeW = (*active)[0].width;
		facts_.activeH = (*active)[0].height;
	} else {
		facts_.activeW = facts_.pixelArrayW;
		facts_.activeH = facts_.pixelArrayH;
	}
	if (facts_.activeW <= 0 || facts_.activeH <= 0) {
		ALOGE("拿不到有效的传感器尺寸（%dx%d）", facts_.activeW, facts_.activeH);
		return false;
	}

	/* ── 朝向 ── */
	auto loc = props.get(libcamera::properties::Location);
	facts_.frontFacing = !loc || *loc == libcamera::properties::CameraLocationFront;
	auto rot = props.get(libcamera::properties::Rotation);
	facts_.orientation = rot ? *rot : 0;

	/*
	 * ── 对外声明的输出尺寸 ──
	 * ⚠️★ 不要把传感器满分辨率一股脑报上去：软件去拜耳 8 MP 单帧就要几十毫秒
	 *    （[#95] 实测 AGC 收敛后帧间隔约 130 ms），预览用满分辨率会卡死。
	 *    这里给一组【降采样】的常用尺寸 + 满分辨率（留给静态拍照），
	 *    宽高比与传感器一致，避免框架做非等比裁剪。
	 */
	auto ratio = [&](int w) {
		return std::make_pair(w, static_cast<int>(
			static_cast<int64_t>(w) * facts_.activeH / facts_.activeW) & ~1);
	};
	facts_.outputSizes = { ratio(640), ratio(1280),
			       { facts_.activeW, facts_.activeH } };

	/* 15 fps 是软件 ISP 的现实上限，别声明做不到的数。 */
	facts_.minFrameDurationNs = 66666666;   /* 15 fps */

	characteristics_ = buildCharacteristics(facts_);
	if (characteristics_.empty()) {
		ALOGE("characteristics 构造失败");
		return false;
	}

	ALOGI("%s 就绪：阵列 %dx%d 有效 %dx%d 朝向 %d %s",
	      name_.c_str(), facts_.pixelArrayW, facts_.pixelArrayH,
	      facts_.activeW, facts_.activeH, facts_.orientation,
	      facts_.frontFacing ? "前摄" : "后摄");
	return true;
}

ndk::ScopedAStatus Device::getCameraCharacteristics(CameraMetadata *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	out->metadata = characteristics_;
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::getPhysicalCameraCharacteristics(const std::string &,
							    CameraMetadata *)
{
	/* 不是逻辑多摄，没有物理子相机。 */
	return err(Status::ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Device::getResourceCost(CameraResourceCost *out)
{
	out->resourceCost = 100;     /* 软件 ISP 吃 CPU，如实报满 */
	out->conflictingDevices.clear();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::isStreamCombinationSupported(const StreamConfiguration &cfg,
							bool *out)
{
	/* 软件 ISP 一次只喂得起一路。 */
	*out = cfg.streams.size() <= 1;
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::open(const std::shared_ptr<ICameraDeviceCallback> &cb,
				std::shared_ptr<ICameraDeviceSession> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	if (!cb)
		return err(Status::ILLEGAL_ARGUMENT);

	auto session = ndk::SharedRefBase::make<Session>(cam_, facts_, cb);
	if (!session->init()) {
		ALOGE("Session::init 失败");
		return err(Status::INTERNAL_ERROR);
	}
	*out = session;
	ALOGI("%s 已打开", name_.c_str());
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::openInjectionSession(
	const std::shared_ptr<ICameraDeviceCallback> &,
	std::shared_ptr<ICameraInjectionSession> *out)
{
	*out = nullptr;
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::setTorchMode(bool)
{
	return err(Status::OPERATION_NOT_SUPPORTED);   /* 前摄没有闪光灯 */
}

ndk::ScopedAStatus Device::turnOnTorchWithStrengthLevel(int32_t)
{
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::getTorchStrengthLevel(int32_t *)
{
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::constructDefaultRequestSettings(RequestTemplate type,
							   CameraMetadata *out)
{
	out->metadata = buildDefaultRequest(static_cast<int>(type), facts_);
	if (out->metadata.empty())
		return err(Status::INTERNAL_ERROR);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::isStreamCombinationWithSettingsSupported(
	const StreamConfiguration &cfg, bool *out)
{
	return isStreamCombinationSupported(cfg, out);
}

ndk::ScopedAStatus Device::getSessionCharacteristics(const StreamConfiguration &,
						     CameraMetadata *out)
{
	/* 会话特性与静态特性一致（我们没有随配置变化的能力）。 */
	std::lock_guard<std::mutex> lk(mutex_);
	out->metadata = characteristics_;
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */
