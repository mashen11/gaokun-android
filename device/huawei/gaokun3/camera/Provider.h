/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <libcamera/libcamera.h>

#include "Metadata.h"

namespace gaokun3 {

/*
 * ★ 命名与注册约定都是从 AOSP 源码核出来的，不是猜的：
 *   设备名   "device@<major>.<minor>/<type>/<id>"
 *            —— CameraProviderManager.cpp:3400 的 parseDeviceName()
 *   版本     "1.1"
 *            —— hardware/interfaces/camera/device/default/ExternalCameraDevice.cpp:51
 *   服务名   <ICameraProvider::descriptor> + "/" + <实例名>
 *            —— camera/provider/default/external-service.cpp 的 main()
 *   类型段   我们用 "internal"（内置相机），外接相机才用 "external"。
 */
inline constexpr char kDeviceVersion[] = "1.1";
inline constexpr char kInstance[] = "internal/0";

class Provider : public aidl::android::hardware::camera::provider::BnCameraProvider {
public:
	Provider();
	~Provider() override;

	/* 初始化 libcamera 并枚举。失败返回 false（此时服务不该注册）。 */
	bool init();

	::ndk::ScopedAStatus setCallback(
		const std::shared_ptr<aidl::android::hardware::camera::provider::
					      ICameraProviderCallback> &callback) override;
	::ndk::ScopedAStatus getVendorTags(
		std::vector<aidl::android::hardware::camera::common::VendorTagSection>
			*_aidl_return) override;
	::ndk::ScopedAStatus getCameraIdList(std::vector<std::string> *_aidl_return) override;
	::ndk::ScopedAStatus getCameraDeviceInterface(
		const std::string &in_cameraDeviceName,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraDevice>
			*_aidl_return) override;
	::ndk::ScopedAStatus notifyDeviceStateChange(int64_t in_deviceState) override;
	::ndk::ScopedAStatus getConcurrentCameraIds(
		std::vector<aidl::android::hardware::camera::provider::
				    ConcurrentCameraIdCombination> *_aidl_return) override;
	::ndk::ScopedAStatus isConcurrentStreamCombinationSupported(
		const std::vector<aidl::android::hardware::camera::provider::
					  CameraIdAndStreamCombination> &in_configs,
		bool *_aidl_return) override;

	/* 手电筒状态变化 → 框架（Device 在 setTorchMode / open / close 时调）。 */
	void notifyTorch(const std::string &deviceName,
			 aidl::android::hardware::camera::common::TorchModeStatus status);

private:
	std::mutex mutex_;
	std::unique_ptr<libcamera::CameraManager> cm_;
	std::shared_ptr<aidl::android::hardware::camera::provider::ICameraProviderCallback> cb_;
	/* AIDL 设备名 → (libcamera 相机, 已建好的 ICameraDevice) */
	std::map<std::string, std::shared_ptr<libcamera::Camera>> cameras_;
	std::map<std::string,
		 std::shared_ptr<aidl::android::hardware::camera::device::ICameraDevice>> devices_;
};

} /* namespace gaokun3 */
