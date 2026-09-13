/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <libcamera/libcamera.h>

#include "Metadata.h"

namespace gaokun3 {

class Session : public aidl::android::hardware::camera::device::BnCameraDeviceSession {
public:
	Session(std::shared_ptr<libcamera::Camera> cam, SensorFacts facts,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceCallback> cb);
	~Session() override;

	bool init();

	::ndk::ScopedAStatus close() override;
	::ndk::ScopedAStatus configureStreams(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_cfg,
		std::vector<aidl::android::hardware::camera::device::HalStream> *out) override;
	::ndk::ScopedAStatus constructDefaultRequestSettings(
		aidl::android::hardware::camera::device::RequestTemplate in_type,
		aidl::android::hardware::camera::device::CameraMetadata *out) override;
	::ndk::ScopedAStatus flush() override;
	::ndk::ScopedAStatus isReconfigurationRequired(
		const aidl::android::hardware::camera::device::CameraMetadata &in_oldSessionParams,
		const aidl::android::hardware::camera::device::CameraMetadata &in_newSessionParams,
		bool *out) override;
	::ndk::ScopedAStatus processCaptureRequest(
		const std::vector<aidl::android::hardware::camera::device::CaptureRequest> &in_requests,
		const std::vector<aidl::android::hardware::camera::device::BufferCache> &in_cachesToRemove,
		int32_t *out) override;
	::ndk::ScopedAStatus switchToOffline(
		const std::vector<int32_t> &in_streamsToKeep,
		aidl::android::hardware::camera::device::CameraOfflineSessionInfo *out_info,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraOfflineSession>
			*out) override;
	::ndk::ScopedAStatus repeatingRequestEnd(int32_t in_frameNumber,
						 const std::vector<int32_t> &in_streamIds) override;

private:
	/* libcamera 的请求完成回调（在 CameraManager 线程上）。 */
	void onRequestCompleted(libcamera::Request *req);

	/* 把一帧 RGB 交付到 Android 的 gralloc 缓冲里。 */
	bool deliver(const libcamera::FrameBuffer *fb, const aidl::android::hardware::camera::
			     device::StreamBuffer &dst, int32_t width, int32_t height,
		     int32_t format);

	std::shared_ptr<libcamera::Camera> cam_;
	SensorFacts facts_;
	std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceCallback> cb_;

	std::mutex mutex_;
	bool streaming_ = false;
	bool closed_ = false;

	std::unique_ptr<libcamera::CameraConfiguration> config_;
	std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
	libcamera::Stream *stream_ = nullptr;

	/* 配置好的 Android 流（我们只支持一路）。 */
	int32_t halStreamId_ = -1;
	int32_t halWidth_ = 0, halHeight_ = 0, halFormat_ = 0;

	/* 空闲的 libcamera 请求。 */
	std::deque<std::unique_ptr<libcamera::Request>> freeRequests_;
	/*
	 * libcamera Request* → Android frameNumber + 目标缓冲。
	 * ⚠️ 不能用 cookie：Request::cookie_ 是 const、只能在构造时给
	 *    （request.h:47,73 —— 没有 setCookie()），而我们的请求对象是
	 *    在 configureStreams 时一次建好反复复用的。所以用指针当键。
	 */
	struct Pending {
		int32_t frameNumber;
		aidl::android::hardware::camera::device::StreamBuffer buffer;
		std::vector<uint8_t> settings;
	};
	std::map<libcamera::Request *, Pending> pending_;
};

} /* namespace gaokun3 */
