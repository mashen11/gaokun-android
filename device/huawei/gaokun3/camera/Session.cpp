/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 会话：流配置、请求、以及把 libcamera 的 RGB 帧
 * 交付到 Android 的 gralloc 缓冲。
 *
 * ⚠️★ 设计上的一个硬约束：**软件 ISP 只输出 RGB 族**
 *    （libcamera debayer_cpu.cpp:436-441 —— 没有 YUV/NV12/YUYV），
 *    而 Android 要 YUV_420_888 / JPEG。色彩转换因此必须由本层用 libyuv 做。
 *    这正是 AOSP 自带的 ExternalCameraProvider 接不上的原因（[#91]）。
 */
#include "Session.h"

#include <cstring>

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/ConfigureStreamsRet.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidl/android/hardware/camera/device/ErrorMsg.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraOfflineSession.h>
#include <aidl/android/hardware/camera/device/NotifyMsg.h>
#include <aidl/android/hardware/camera/device/ShutterMsg.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <hardware/gralloc.h>
#include <libyuv.h>
#include <log/log.h>
#include <sys/mman.h>
#include <system/graphics.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <utils/Errors.h>

#include <libcamera/formats.h>

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::BufferCache;
using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::CaptureRequest;
using ::aidl::android::hardware::camera::device::CaptureResult;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::camera::device::HalStream;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::NotifyMsg;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::ShutterMsg;
using ::aidl::android::hardware::camera::device::StreamBuffer;
using ::aidl::android::hardware::camera::device::StreamConfiguration;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}
} /* namespace */

Session::Session(std::shared_ptr<libcamera::Camera> cam, SensorFacts facts,
		 std::shared_ptr<ICameraDeviceCallback> cb)
	: cam_(std::move(cam)), facts_(std::move(facts)), cb_(std::move(cb))
{
}

Session::~Session()
{
	close();
}

bool Session::init()
{
	if (cam_->acquire()) {
		ALOGE("libcamera Camera::acquire 失败（是不是已经被别人占了）");
		return false;
	}
	cam_->requestCompleted.connect(this, &Session::onRequestCompleted);
	/* 框架建会话时会取这两个描述符；我们不往里写（fmqResultSize 恒 0），
	 * 但必须是有效的队列。大小照抄 AOSP 的 ExternalCameraDeviceSession。 */
	requestQueue_ = std::make_shared<MetadataQueue>(1 << 20, false);
	resultQueue_ = std::make_shared<MetadataQueue>(1 << 20, false);
	if (!requestQueue_->isValid() || !resultQueue_->isValid()) {
		ALOGE("建 FMQ 元数据队列失败");
		return false;
	}
	return true;
}

ndk::ScopedAStatus Session::close()
{
	std::lock_guard<std::mutex> lk(mutex_);
	if (closed_)
		return ndk::ScopedAStatus::ok();
	closed_ = true;

	if (streaming_) {
		cam_->stop();
		streaming_ = false;
	}
	cam_->requestCompleted.disconnect(this, &Session::onRequestCompleted);
	freeRequests_.clear();
	pending_.clear();
	allocator_.reset();
	config_.reset();
	cam_->release();
	ALOGI("会话已关闭");
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::configureStreams(const StreamConfiguration &cfg,
					     std::vector<HalStream> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	out->clear();

	if (cfg.streams.empty())
		return err(Status::ILLEGAL_ARGUMENT);
	if (cfg.streams.size() > 1) {
		/* ⚠️ 如实拒绝：软件去拜耳 8 MP 一路就吃满 CPU，多路只会都卡。
		 *    characteristics 里也声明了只支持 1 路输出。 */
		ALOGE("请求了 %zu 路流，本 HAL 只支持 1 路", cfg.streams.size());
		return err(Status::ILLEGAL_ARGUMENT);
	}

	const auto &s = cfg.streams[0];
	ALOGI("configureStreams: id=%d %dx%d fmt=0x%x usage=0x%llx",
	      s.id, s.width, s.height, static_cast<int>(s.format),
	      static_cast<unsigned long long>(s.usage));

	if (streaming_) {
		cam_->stop();
		streaming_ = false;
		freeRequests_.clear();
		pending_.clear();
		allocator_.reset();
	}

	/* Viewfinder role 会启用软件 ISP（Raw role 不会）。 */
	config_ = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
	if (!config_ || config_->empty()) {
		ALOGE("generateConfiguration 失败");
		return err(Status::INTERNAL_ERROR);
	}
	libcamera::StreamConfiguration &sc = config_->at(0);
	sc.size = libcamera::Size(s.width, s.height);
	/* 固定要 RGB888 —— 下面用 libyuv 转成 Android 要的格式。
	 * ⚠️ 用 3 字节的 RGB888 而不是 4 字节的 ABGR8888：8 MP 下每帧少搬
	 *    8 MB，而软件去拜耳本来就是 CPU 瓶颈。 */
	sc.pixelFormat = libcamera::formats::RGB888;
	sc.bufferCount = 4;

	libcamera::CameraConfiguration::Status st = config_->validate();
	if (st == libcamera::CameraConfiguration::Invalid) {
		ALOGE("配置无效");
		return err(Status::ILLEGAL_ARGUMENT);
	}
	if (st == libcamera::CameraConfiguration::Adjusted)
		ALOGW("配置被调整为 %s", sc.toString().c_str());

	if (cam_->configure(config_.get())) {
		ALOGE("Camera::configure 失败");
		return err(Status::INTERNAL_ERROR);
	}

	stream_ = sc.stream();
	allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(cam_);
	if (allocator_->allocate(stream_) < 0) {
		ALOGE("分配 libcamera 缓冲失败");
		return err(Status::INTERNAL_ERROR);
	}
	for (const auto &b : allocator_->buffers(stream_)) {
		auto r = cam_->createRequest();
		if (!r || r->addBuffer(stream_, b.get())) {
			ALOGE("建 libcamera 请求失败");
			return err(Status::INTERNAL_ERROR);
		}
		freeRequests_.push_back(std::move(r));
	}

	if (cam_->start()) {
		ALOGE("Camera::start 失败");
		return err(Status::INTERNAL_ERROR);
	}
	streaming_ = true;

	halStreamId_ = s.id;
	halWidth_ = sc.size.width;
	halHeight_ = sc.size.height;
	halFormat_ = static_cast<int32_t>(s.format);

	HalStream hs;
	hs.id = s.id;
	/* IMPLEMENTATION_DEFINED 要由 HAL 定成具体格式；我们交付 YCbCr_420_888。 */
	hs.overrideFormat = (s.format ==
			     aidl::android::hardware::graphics::common::PixelFormat::IMPLEMENTATION_DEFINED)
				    ? aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888
				    : s.format;
	hs.producerUsage = aidl::android::hardware::graphics::common::BufferUsage::CPU_WRITE_OFTEN;
	hs.consumerUsage = static_cast<aidl::android::hardware::graphics::common::BufferUsage>(0);
	hs.maxBuffers = 4;
	hs.overrideDataSpace = s.dataSpace;
	hs.physicalCameraId = "";
	hs.supportOffline = false;
	hs.enableHalBufferManager = false;
	out->push_back(hs);

	ALOGI("流已配置：%dx%d，libcamera 输出 %s",
	      halWidth_, halHeight_, sc.pixelFormat.toString().c_str());
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::configureStreamsV2(
	const StreamConfiguration &cfg,
	aidl::android::hardware::camera::device::ConfigureStreamsRet *out)
{
	return configureStreams(cfg, &out->halStreams);
}

ndk::ScopedAStatus Session::constructDefaultRequestSettings(RequestTemplate type,
							    CameraMetadata *out)
{
	out->metadata = buildDefaultRequest(static_cast<int>(type), facts_);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::flush()
{
	/* 我们不排队积压请求，libcamera 自己会把在途的完成掉。 */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::isReconfigurationRequired(const CameraMetadata &,
						      const CameraMetadata &, bool *out)
{
	*out = false;
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::processCaptureRequest(const std::vector<CaptureRequest> &reqs,
						  const std::vector<BufferCache> &,
						  int32_t *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	*out = 0;
	if (!streaming_)
		return err(Status::INTERNAL_ERROR);

	for (const auto &r : reqs) {
		if (r.outputBuffers.empty())
			continue;
		if (freeRequests_.empty()) {
			/* ⚠️ 没有空闲请求就如实返回已接受的数量，让框架回压 ——
			 *    悄悄丢帧会让上层等一个永远不来的结果。 */
			ALOGW("无空闲请求，已接受 %d/%zu", *out, reqs.size());
			break;
		}
		auto lreq = std::move(freeRequests_.front());
		freeRequests_.pop_front();

		const auto &ob = r.outputBuffers[0];
		/* ⚠️ 现在就导入：AIDL 的 StreamBuffer 不可拷贝（见 Session.h 的说明），
		 *    而且完成回调里再导入等于多绕一圈。 */
		buffer_handle_t imported = importBuffer(ob);
		if (!imported) {
			ALOGE("导入 gralloc 缓冲失败 frame=%d", r.frameNumber);
			freeRequests_.push_back(std::move(lreq));
			break;
		}

		lreq->reuse(libcamera::Request::ReuseBuffers);
		libcamera::Request *key = lreq.get();
		Pending p;
		p.frameNumber = r.frameNumber;
		p.streamId = ob.streamId;
		p.bufferId = ob.bufferId;
		p.imported = imported;
		p.settings = r.settings.metadata;
		pending_[key] = std::move(p);

		if (cam_->queueRequest(key)) {
			ALOGE("queueRequest 失败 frame=%d", r.frameNumber);
			releaseBuffer(imported);
			pending_.erase(key);
			freeRequests_.push_back(std::move(lreq));
			break;
		}
		/* libcamera 持有裸指针直到完成，所以这里放掉所有权，
		 * 完成回调里再放回 freeRequests_。 */
		lreq.release();
		(*out)++;
	}
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::switchToOffline(
	const std::vector<int32_t> &,
	aidl::android::hardware::camera::device::CameraOfflineSessionInfo *,
	std::shared_ptr<aidl::android::hardware::camera::device::ICameraOfflineSession> *out)
{
	*out = nullptr;
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Session::repeatingRequestEnd(int32_t, const std::vector<int32_t> &)
{
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getCaptureRequestMetadataQueue(
	::aidl::android::hardware::common::fmq::MQDescriptor<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite> *out)
{
	*out = requestQueue_->dupeDesc();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getCaptureResultMetadataQueue(
	::aidl::android::hardware::common::fmq::MQDescriptor<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite> *out)
{
	*out = resultQueue_->dupeDesc();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::signalStreamFlush(const std::vector<int32_t> &, int32_t)
{
	/* HAL buffer manager 关着（enableHalBufferManager=false），无事可做。 */
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */

/* ────────────────────────── 帧交付 ────────────────────────── */

namespace gaokun3 {

buffer_handle_t Session::importBuffer(const StreamBuffer &sb)
{
	const native_handle_t *raw = ::android::makeFromAidl(sb.buffer);
	if (!raw) {
		ALOGE("makeFromAidl 失败");
		return nullptr;
	}
	auto &mapper = android::GraphicBufferMapper::get();
	buffer_handle_t imported = nullptr;
	android::status_t st = mapper.importBuffer(raw, halWidth_, halHeight_,
						   /*layerCount=*/1,
						   HAL_PIXEL_FORMAT_YCBCR_420_888,
						   GRALLOC_USAGE_SW_WRITE_OFTEN,
						   /*stride=*/halWidth_, &imported);
	/* makeFromAidl 造的是一份新句柄，importBuffer 之后就不需要它了。 */
	native_handle_close(const_cast<native_handle_t *>(raw));
	native_handle_delete(const_cast<native_handle_t *>(raw));
	if (st != android::OK || !imported) {
		ALOGE("importBuffer 失败: %d", st);
		return nullptr;
	}
	return imported;
}

void Session::releaseBuffer(buffer_handle_t h)
{
	if (h)
		android::GraphicBufferMapper::get().freeBuffer(h);
}

/*
 * 把 libcamera 的一帧 RGB 转成 YUV 写进 gralloc 缓冲。
 *
 * ★★ 字节序是【核过源码】的，不是按名字猜的 —— 猜错是红蓝互换，而且不报错：
 *      · libcamera 的 formats::RGB888 映射到 V4L2_PIX_FMT_BGR24
 *        （src/libcamera/formats.cpp:185）⇒ 内存里 B,G,R
 *      · libyuv 的 "RGB24" 对应 FOURCC_24BG（video_common.h:81）也是 B,G,R；
 *        R,G,B 那个在 libyuv 里叫 "RAW"
 *    ⇒ 用 RGB24ToI420()，不是 RAWToI420()。
 */
bool Session::deliver(const libcamera::FrameBuffer *fb, buffer_handle_t dst,
		      int32_t width, int32_t height)
{
	const auto &planes = fb->planes();
	if (planes.empty()) {
		ALOGE("deliver: libcamera 帧没有 plane");
		return false;
	}
	const libcamera::FrameBuffer::Plane &p = planes[0];

	void *src = mmap(nullptr, p.offset + p.length, PROT_READ, MAP_SHARED,
			 p.fd.get(), 0);
	if (src == MAP_FAILED) {
		ALOGE("deliver: mmap libcamera 帧失败: %s", strerror(errno));
		return false;
	}
	const uint8_t *rgb = static_cast<const uint8_t *>(src) + p.offset;
	const int rgbStride = width * 3;

	auto &mapper = android::GraphicBufferMapper::get();
	android_ycbcr ycbcr = {};
	android::status_t st = mapper.lockYCbCr(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
						android::Rect(width, height), &ycbcr);
	bool ok = false;
	if (st == android::OK && ycbcr.y) {
		/*
		 * ⚠️ 先转 I420 再按目标布局搬一次。多一次拷贝，但 gralloc 给的
		 *    可能是平面(I420)也可能是半平面(NV12/NV21)，由 chroma_step
		 *    决定 —— 直接写会在某些布局上悄悄写错。
		 *    ⬜ 之后可按 chroma_step 分支省掉这次拷贝。
		 */
		const int cw = (width + 1) / 2, chh = (height + 1) / 2;
		std::vector<uint8_t> i420(static_cast<size_t>(width) * height +
					  static_cast<size_t>(cw) * chh * 2);
		uint8_t *dy = i420.data();
		uint8_t *du = dy + static_cast<size_t>(width) * height;
		uint8_t *dv = du + static_cast<size_t>(cw) * chh;

		if (libyuv::RGB24ToI420(rgb, rgbStride, dy, width, du, cw, dv, cw,
					width, height) == 0) {
			for (int y = 0; y < height; y++)
				memcpy(static_cast<uint8_t *>(ycbcr.y) + y * ycbcr.ystride,
				       dy + static_cast<size_t>(y) * width, width);
			for (int y = 0; y < chh; y++) {
				uint8_t *cb = static_cast<uint8_t *>(ycbcr.cb) + y * ycbcr.cstride;
				uint8_t *cr = static_cast<uint8_t *>(ycbcr.cr) + y * ycbcr.cstride;
				const uint8_t *su = du + static_cast<size_t>(y) * cw;
				const uint8_t *sv = dv + static_cast<size_t>(y) * cw;
				for (int x = 0; x < cw; x++) {
					cb[x * ycbcr.chroma_step] = su[x];
					cr[x * ycbcr.chroma_step] = sv[x];
				}
			}
			ok = true;
		} else {
			ALOGE("deliver: RGB24ToI420 失败");
		}
		mapper.unlock(dst);
	} else {
		ALOGE("deliver: lockYCbCr 失败: %d", st);
	}

	munmap(src, p.offset + p.length);
	return ok;
}

void Session::onRequestCompleted(libcamera::Request *req)
{
	if (req->status() == libcamera::Request::RequestCancelled)
		return;

	Pending pend;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		auto it = pending_.find(req);
		if (it == pending_.end()) {
			ALOGW("完成了一个不认识的请求 %p", (void *)req);
			return;
		}
		pend = std::move(it->second);
		pending_.erase(it);
		/* 请求还回空闲池（queue 时 release 过所有权）。 */
		freeRequests_.push_back(std::unique_ptr<libcamera::Request>(req));
	}

	const libcamera::FrameBuffer *fb = req->buffers().begin()->second;
	const int64_t timestamp = fb->metadata().timestamp;

	/* ── 顺序要求：先 shutter，后结果 ── */
	NotifyMsg msg;
	ShutterMsg shutter;
	shutter.frameNumber = pend.frameNumber;
	shutter.timestamp = timestamp;
	shutter.readoutTimestamp = timestamp;
	msg.set<NotifyMsg::Tag::shutter>(shutter);
	std::vector<NotifyMsg> msgs;
	msgs.push_back(std::move(msg));
	cb_->notify(msgs);

	bool ok = deliver(fb, pend.imported, halWidth_, halHeight_);
	if (!ok) {
		NotifyMsg emsg;
		ErrorMsg e;
		e.frameNumber = pend.frameNumber;
		e.errorStreamId = pend.streamId;
		e.errorCode = ErrorCode::ERROR_BUFFER;
		emsg.set<NotifyMsg::Tag::error>(e);
		std::vector<NotifyMsg> emsgs;
		emsgs.push_back(std::move(emsg));
		cb_->notify(emsgs);
	}
	releaseBuffer(pend.imported);

	CaptureResult result;
	result.frameNumber = pend.frameNumber;
	result.fmqResultSize = 0;
	result.partialResult = 1;
	/* ⬜ 结果元数据暂时回传请求里的设置；之后应填 libcamera 的实际曝光/增益。 */
	result.result.metadata = pend.settings;

	StreamBuffer sb;
	sb.streamId = pend.streamId;
	sb.bufferId = pend.bufferId;
	sb.status = ok ? BufferStatus::OK : BufferStatus::ERROR;
	/* ⚠️ buffer/fence 都留空：框架按 bufferId 认，回传句柄会被当成一次新导入。 */
	result.outputBuffers.push_back(std::move(sb));
	result.inputBuffer.streamId = -1;
	result.inputBuffer.bufferId = 0;

	std::vector<CaptureResult> results;
	results.push_back(std::move(result));
	cb_->processCaptureResult(results);
}

} /* namespace gaokun3 */
