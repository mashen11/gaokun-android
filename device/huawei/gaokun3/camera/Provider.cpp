/* SPDX-License-Identifier: Apache-2.0 */
#include "Provider.h"
#include "Device.h"

#include <aidl/android/hardware/camera/common/CameraDeviceStatus.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>
#include <aidl/android/hardware/camera/common/VendorTagSection.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <log/log.h>

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::common::TorchModeStatus;
using ::aidl::android::hardware::camera::common::VendorTagSection;
using ::aidl::android::hardware::camera::device::ICameraDevice;
using ::aidl::android::hardware::camera::provider::CameraIdAndStreamCombination;
using ::aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination;
using ::aidl::android::hardware::camera::provider::ICameraProviderCallback;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}
} /* namespace */

Provider::Provider() = default;

Provider::~Provider()
{
	if (cm_)
		cm_->stop();
}

bool Provider::init()
{
	cm_ = std::make_unique<libcamera::CameraManager>();
	int ret = cm_->start();
	if (ret) {
		ALOGE("libcamera CameraManager::start 失败: %d", ret);
		return false;
	}

	auto cams = cm_->cameras();
	ALOGI("libcamera 发现 %zu 个相机", cams.size());
	int idx = 0;
	for (const auto &c : cams) {
		std::string name = std::string("device@") + kDeviceVersion +
				   "/internal/" + std::to_string(idx);
		cameras_[name] = c;
		ALOGI("  %s  ←  %s", name.c_str(), c->id().c_str());
		idx++;
	}
	/* ⚠️ 一个相机都没有时【不要】报成功：provider 注册上去但列表为空，
	 *    框架只会显示"没有相机"，而真正的失败原因（camss 没 probe /
	 *    缺 DTB）就被埋掉了。宁可让服务起不来，日志里看得见。 */
	return !cameras_.empty();
}

ndk::ScopedAStatus Provider::setCallback(
	const std::shared_ptr<ICameraProviderCallback> &callback)
{
	std::lock_guard<std::mutex> lk(mutex_);
	cb_ = callback;
	if (cb_) {
		/* 内置相机是常在的，注册回调时立刻把状态推过去。 */
		for (const auto &kv : cameras_)
			cb_->cameraDeviceStatusChange(
				kv.first,
				aidl::android::hardware::camera::common::CameraDeviceStatus::PRESENT);
	}
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getVendorTags(std::vector<VendorTagSection> *out)
{
	out->clear();      /* 没有厂商私有 tag */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getCameraIdList(std::vector<std::string> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	out->clear();
	for (const auto &kv : cameras_)
		out->push_back(kv.first);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getCameraDeviceInterface(
	const std::string &name, std::shared_ptr<ICameraDevice> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);

	auto it = devices_.find(name);
	if (it != devices_.end()) {
		*out = it->second;
		return ndk::ScopedAStatus::ok();
	}

	auto cit = cameras_.find(name);
	if (cit == cameras_.end()) {
		ALOGE("getCameraDeviceInterface: 未知设备名 %s", name.c_str());
		return err(Status::ILLEGAL_ARGUMENT);
	}

	auto dev = ndk::SharedRefBase::make<Device>(cit->second, name, this);
	if (!dev->init()) {
		ALOGE("Device::init 失败: %s", name.c_str());
		return err(Status::INTERNAL_ERROR);
	}
	devices_[name] = dev;
	*out = dev;
	return ndk::ScopedAStatus::ok();
}

void Provider::notifyTorch(const std::string &deviceName, TorchModeStatus status)
{
	std::shared_ptr<ICameraProviderCallback> cb;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		cb = cb_;
	}
	if (cb)
		cb->torchModeStatusChange(deviceName, status);
}

ndk::ScopedAStatus Provider::notifyDeviceStateChange(int64_t state)
{
	/* 折叠/展开之类的设备状态，本机无关。 */
	ALOGD("notifyDeviceStateChange(%lld) —— 忽略", (long long)state);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getConcurrentCameraIds(
	std::vector<ConcurrentCameraIdCombination> *out)
{
	out->clear();      /* 只有一个相机，谈不上并发组合 */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::isConcurrentStreamCombinationSupported(
	const std::vector<CameraIdAndStreamCombination> &, bool *out)
{
	*out = false;
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */
