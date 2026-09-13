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
#include <cutils/native_handle.h>
#include <fmq/AidlMessageQueue.h>
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
	/* V2 起新增；只是把 HalStream[] 包进一个 parcelable，委托给上面那个。 */
	::ndk::ScopedAStatus configureStreamsV2(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_cfg,
		aidl::android::hardware::camera::device::ConfigureStreamsRet *out) override;
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
	/* ── FMQ 元数据队列：我们不用它们（fmqResultSize 恒为 0），但接口是
	 *    纯虚的，必须实现。框架会在建会话时取一次描述符。 ── */
	::ndk::ScopedAStatus getCaptureRequestMetadataQueue(
		::aidl::android::hardware::common::fmq::MQDescriptor<
			int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
			*out) override;
	::ndk::ScopedAStatus getCaptureResultMetadataQueue(
		::aidl::android::hardware::common::fmq::MQDescriptor<
			int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
			*out) override;
	::ndk::ScopedAStatus signalStreamFlush(const std::vector<int32_t> &in_streamIds,
					       int32_t in_streamConfigCounter) override;

private:
	/* libcamera 的请求完成回调（在 CameraManager 线程上）。 */
	void onRequestCompleted(libcamera::Request *req);

	/*
	 * 取得一路流某个 bufferId 对应的 gralloc 句柄（带缓存）。
	 * ★★ AIDL 的契约（StreamBuffer.aidl 原文）：同一个 bufferId **只在第一次**
	 *   带有效句柄，之后框架发过来的 `buffer` 字段是【空的】，
	 *   "HAL must look up the actual buffer handle to use from its own
	 *    bufferId to buffer handle map"。
	 *   漏了这个缓存的症状是第二帧起报 "Failed to importBuffer. Bad handle."
	 */
	buffer_handle_t getBuffer(
		const aidl::android::hardware::camera::device::StreamBuffer &sb);
	/* 按 cachesToRemove 释放；会话结束时全部释放。 */
	void dropCaches(const std::vector<
			aidl::android::hardware::camera::device::BufferCache> &caches);
	void dropAllCaches();

	/* 把一帧 RGB（源尺寸 srcWidth_×srcHeight_）交付到一个 gralloc 缓冲，
	 * 必要时缩放到该路流自己的尺寸。 */
	bool deliver(const uint8_t *rgb, buffer_handle_t dst,
		     int32_t dstW, int32_t dstH);
	/* JPEG 流：把 RGB 编码成 JPEG 写进 BLOB 缓冲。 */
	bool deliverJpeg(const uint8_t *rgb, buffer_handle_t dst,
			 int32_t dstW, int32_t dstH, int32_t blobSize,
			 int quality);

	std::shared_ptr<libcamera::Camera> cam_;
	SensorFacts facts_;
	std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceCallback> cb_;

	std::mutex mutex_;
	bool streaming_ = false;
	bool closed_ = false;

	std::unique_ptr<libcamera::CameraConfiguration> config_;
	std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
	libcamera::Stream *stream_ = nullptr;

	/*
	 * 配置好的 Android 流。
	 * ★ 相机应用【一定】会配多路（预览 + 拍照，实测 Aperture 要 2 路）。
	 *   libcamera 这边只开一路（按最大请求尺寸），再用 libyuv 缩放分发到每一路
	 *   —— 软件 ISP 是 CPU 瓶颈，开多路等于把同一份拜耳去马赛克多次。
	 */
	struct HalStreamInfo {
		int32_t id = -1;
		int32_t width = 0, height = 0;
		bool isBlob = false;        /* JPEG 流 */
		int32_t blobSize = 0;       /* Stream::bufferSize */
	};
	std::vector<HalStreamInfo> halStreams_;
	/* libcamera 实际输出的尺寸（= 各路里最大的那个）。 */
	int32_t srcWidth_ = 0, srcHeight_ = 0;

	/* 空闲的 libcamera 请求。 */
	std::deque<std::unique_ptr<libcamera::Request>> freeRequests_;
	/*
	 * libcamera Request* → Android frameNumber + 目标缓冲。
	 * ⚠️ 不能用 cookie：Request::cookie_ 是 const、只能在构造时给
	 *    （request.h:47,73 —— 没有 setCookie()），而我们的请求对象是
	 *    在 configureStreams 时一次建好反复复用的。所以用指针当键。
	 */
	/*
	 * ⚠️★ 这里【不能】存 AIDL 的 StreamBuffer：它含 NativeHandle，
	 *    里面是 vector<ndk::ScopedFileDescriptor> —— **move-only，不可拷贝**
	 *    （编译器报的是 vector::operator= 没有匹配的 assign）。
	 *    而 processCaptureRequest 的入参是 const&，move 不出来。
	 *    ⇒ 收到请求时就把 gralloc 缓冲导入，这里只存导入后的句柄和 id，
	 *      完成时直接往里写，然后 freeBuffer。这样也顺带省掉一次导入。
	 */
	struct PendingBuffer {
		int32_t streamId = -1;
		int64_t bufferId = 0;
		buffer_handle_t handle = nullptr;   /* 缓存里的，不拥有 */
		int32_t width = 0, height = 0;
		bool isBlob = false;
		int32_t blobSize = 0;
	};
	struct Pending {
		int32_t frameNumber = 0;
		std::vector<PendingBuffer> buffers;
		std::vector<uint8_t> settings;
	};
	std::map<libcamera::Request *, Pending> pending_;

	/* (streamId, bufferId) → 已导入的句柄。见 getBuffer() 的说明。 */
	std::map<std::pair<int32_t, int64_t>, buffer_handle_t> bufferCache_;

	using MetadataQueue = ::android::AidlMessageQueue<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>;
	std::shared_ptr<MetadataQueue> requestQueue_;
	std::shared_ptr<MetadataQueue> resultQueue_;
};

} /* namespace gaokun3 */
