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
#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/CameraBlobId.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <jpeglib.h>
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
using ::aidl::android::hardware::camera::device::CameraBlob;
using ::aidl::android::hardware::camera::device::CameraBlobId;
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
	dropAllCaches();
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

	/*
	 * ★ 相机应用一定会配多路（预览 + 拍照）。libcamera 只开【一路】，
	 *   尺寸取各路里最大的那个，再用 libyuv 缩放分发 ——
	 *   软件 ISP 是 CPU 瓶颈，开多路等于把同一份拜耳去马赛克多次。
	 */
	int32_t maxW = 0, maxH = 0;
	for (const auto &s2 : cfg.streams) {
		ALOGI("  请求流 id=%d %dx%d fmt=0x%x", s2.id, s2.width, s2.height,
		      static_cast<int>(s2.format));
		if (static_cast<int64_t>(s2.width) * s2.height >
		    static_cast<int64_t>(maxW) * maxH) {
			maxW = s2.width;
			maxH = s2.height;
		}
	}

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
	sc.size = libcamera::Size(maxW, maxH);
	/* 固定要 RGB888。⚠️ 用 3 字节的 RGB888 而不是 4 字节的 ABGR8888：
	 * 8 MP 下每帧少搬 8 MB，而软件去拜耳本来就是 CPU 瓶颈。 */
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
	srcWidth_ = sc.size.width;
	srcHeight_ = sc.size.height;

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

	halStreams_.clear();
	for (const auto &s2 : cfg.streams) {
		const bool isBlob =
			s2.format == aidl::android::hardware::graphics::common::PixelFormat::BLOB;
		/* ⚠️ BLOB 流的 bufferSize 由框架给（Stream::bufferSize）；
		 *    它是【字节数】，不是像素宽高。 */
		halStreams_.push_back({ s2.id, s2.width, s2.height, isBlob,
					isBlob ? s2.bufferSize : 0 });

		HalStream hs;
		hs.id = s2.id;
		/* IMPLEMENTATION_DEFINED 要由 HAL 定成具体格式；我们交付 YCbCr_420_888。 */
		hs.overrideFormat =
			(s2.format == aidl::android::hardware::graphics::common::PixelFormat::
					      IMPLEMENTATION_DEFINED)
				? aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888
				: s2.format;
		hs.producerUsage =
			aidl::android::hardware::graphics::common::BufferUsage::CPU_WRITE_OFTEN;
		hs.consumerUsage =
			static_cast<aidl::android::hardware::graphics::common::BufferUsage>(0);
		hs.maxBuffers = 4;
		hs.overrideDataSpace = s2.dataSpace;
		hs.physicalCameraId = "";
		hs.supportOffline = false;
		hs.enableHalBufferManager = false;
		out->push_back(hs);
	}

	ALOGI("流已配置：%zu 路，libcamera 源 %dx%d 输出 %s",
	      halStreams_.size(), srcWidth_, srcHeight_,
	      sc.pixelFormat.toString().c_str());
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
						  const std::vector<BufferCache> &cachesToRemove,
						  int32_t *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	dropCaches(cachesToRemove);
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

		lreq->reuse(libcamera::Request::ReuseBuffers);
		libcamera::Request *key = lreq.get();
		Pending p;
		p.frameNumber = r.frameNumber;
		p.settings = r.settings.metadata;

		/* ⚠️ 现在就导入每一路的缓冲：AIDL 的 StreamBuffer 不可拷贝
		 *    （见 Session.h 的说明），而入参是 const&，move 不出来。 */
		bool importOk = true;
		for (const auto &ob : r.outputBuffers) {
			int32_t w = 0, h = 0;
			bool isBlob = false;
			int32_t blobSize = 0;
			for (const auto &hs : halStreams_)
				if (hs.id == ob.streamId) {
					w = hs.width; h = hs.height;
					isBlob = hs.isBlob; blobSize = hs.blobSize;
				}
			if (!w || !h) {
				ALOGE("请求里出现未配置的流 id=%d", ob.streamId);
				importOk = false;
				break;
			}
			buffer_handle_t hnd = getBuffer(ob);
			if (!hnd) { importOk = false; break; }
			p.buffers.push_back({ ob.streamId, ob.bufferId, hnd, w, h,
					      isBlob, blobSize });
		}
		if (!importOk) {
			/* ⚠️ 不要在这里 free：句柄归缓存所有，下一帧还要用。 */
			freeRequests_.push_back(std::move(lreq));
			break;
		}

		pending_[key] = std::move(p);

		if (cam_->queueRequest(key)) {
			ALOGE("queueRequest 失败 frame=%d", r.frameNumber);
			pending_.erase(key);   /* 句柄归缓存，不在这里释放 */
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

buffer_handle_t Session::getBuffer(const StreamBuffer &sb)
{
	const auto key = std::make_pair(sb.streamId, sb.bufferId);
	auto it = bufferCache_.find(key);
	if (it != bufferCache_.end())
		return it->second;

	/*
	 * 第一次见到这个 bufferId —— 此时（且仅此时）框架给的句柄是有效的。
	 * ★ StreamBuffer.aidl 原文："If the bufferId has been sent to the HAL
	 *   before, this buffer handle must be empty and HAL must look up the
	 *   actual buffer handle to use from its own bufferId to buffer handle map."
	 */
	const native_handle_t *raw = ::android::makeFromAidl(sb.buffer);
	if (!raw) {
		ALOGE("makeFromAidl 失败 stream=%d buffer=%lld", sb.streamId,
		      (long long)sb.bufferId);
		return nullptr;
	}

	/*
	 * ⚠️★ 用 importBufferNoValidate()，不要用带宽高/格式/usage 的那个重载：
	 *   gralloc4 会拿传进去的描述符跟缓冲【实际分配时】的参数比对，对不上
	 *   返回 BAD_BUFFER(2)，而我们并不知道框架分配时用的确切 usage
	 *   （producerUsage | consumerUsage 再加框架自己的位，我们只声明了一半）。
	 *   ★ 判据：错误码 2 是 gralloc 的 BAD_BUFFER，不是 errno 的 ENOENT。
	 */
	auto &mapper = android::GraphicBufferMapper::get();
	buffer_handle_t imported = nullptr;
	android::status_t st = mapper.importBufferNoValidate(raw, &imported);
	/*
	 * ⚠️★ 只 delete、不 close：importBufferNoValidate() 成功后【接管了 fd】，
	 *   再 native_handle_close() 会去关已经被接管/关闭的描述符，
	 *   日志刷 "Could not close FD nn: Bad file descriptor"。
	 *   失败时才要自己关掉，否则漏 fd。
	 */
	if (st != android::OK || !imported)
		native_handle_close(const_cast<native_handle_t *>(raw));
	native_handle_delete(const_cast<native_handle_t *>(raw));
	if (st != android::OK || !imported) {
		ALOGE("importBuffer 失败: %d (stream=%d buffer=%lld)", st,
		      sb.streamId, (long long)sb.bufferId);
		return nullptr;
	}

	bufferCache_[key] = imported;
	return imported;
}

void Session::dropCaches(const std::vector<BufferCache> &caches)
{
	for (const auto &c : caches) {
		auto it = bufferCache_.find(std::make_pair(c.streamId, c.bufferId));
		if (it == bufferCache_.end())
			continue;
		android::GraphicBufferMapper::get().freeBuffer(it->second);
		bufferCache_.erase(it);
	}
}

void Session::dropAllCaches()
{
	for (auto &kv : bufferCache_)
		android::GraphicBufferMapper::get().freeBuffer(kv.second);
	bufferCache_.clear();
}

bool Session::deliver(const uint8_t *rgb, buffer_handle_t dst,
		      int32_t dstW, int32_t dstH)
{
	const int srcStride = srcWidth_ * 3;

	auto &mapper = android::GraphicBufferMapper::get();
	android_ycbcr ycbcr = {};
	android::status_t st = mapper.lockYCbCr(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
						android::Rect(dstW, dstH), &ycbcr);
	if (st != android::OK || !ycbcr.y) {
		ALOGE("deliver: lockYCbCr 失败: %d", st);
		return false;
	}

	bool ok = false;
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;
	std::vector<uint8_t> i420(static_cast<size_t>(dstW) * dstH +
				  static_cast<size_t>(cw) * chh * 2);
	uint8_t *dy = i420.data();
	uint8_t *du = dy + static_cast<size_t>(dstW) * dstH;
	uint8_t *dv = du + static_cast<size_t>(cw) * chh;

	/*
	 * ★★ 用 J420（全范围）而不是 I420（限制范围 16–235）。
	 *   Android 相机输出的 YUV_420_888 按约定是全范围 BT.601；
	 *   给限制范围会让预览发灰发闷（对比度被压掉约 13%）。
	 *   libyuv 的命名：I420 = 限制范围，J420 = 全范围（JPEG 范围）。
	 *   ⚠️ 这一条是【假说】，靠前后截图 A/B 验证，不是从文档抄来的。
	 *   字节序照旧：convert.h:959 注释明写 "RGB little endian (bgr in
	 *   memory) to J420"，与 libcamera 的 RGB888(=BGR24) 对得上。
	 */
	int rc;
	if (dstW == srcWidth_ && dstH == srcHeight_) {
		rc = libyuv::RGB24ToJ420(rgb, srcStride, dy, dstW, du, cw, dv, cw,
					 dstW, dstH);
	} else {
		/*
		 * 需要缩放：先在源尺寸上转成 I420，再缩放到目标尺寸。
		 * ⚠️ 顺序不能反 —— libyuv 没有"RGB24 直接缩放到 I420"的接口，
		 *    而先缩放 RGB 再转会多搬一次 3 字节/像素的数据。
		 * ⬜ 这条路每帧多一次全图拷贝；多路预览时值得再优化。
		 */
		const int scw = (srcWidth_ + 1) / 2, schh = (srcHeight_ + 1) / 2;
		std::vector<uint8_t> src(static_cast<size_t>(srcWidth_) * srcHeight_ +
					 static_cast<size_t>(scw) * schh * 2);
		uint8_t *sy = src.data();
		uint8_t *su = sy + static_cast<size_t>(srcWidth_) * srcHeight_;
		uint8_t *sv = su + static_cast<size_t>(scw) * schh;
		rc = libyuv::RGB24ToJ420(rgb, srcStride, sy, srcWidth_, su, scw, sv, scw,
					 srcWidth_, srcHeight_);
		if (rc == 0)
			rc = libyuv::I420Scale(sy, srcWidth_, su, scw, sv, scw,
					       srcWidth_, srcHeight_,
					       dy, dstW, du, cw, dv, cw,
					       dstW, dstH, libyuv::kFilterBilinear);
	}

	if (rc == 0) {
		for (int y = 0; y < dstH; y++)
			memcpy(static_cast<uint8_t *>(ycbcr.y) + y * ycbcr.ystride,
			       dy + static_cast<size_t>(y) * dstW, dstW);
		/* 按 chroma_step 同时支持平面(I420)与半平面(NV12/NV21)。 */
		for (int y = 0; y < chh; y++) {
			uint8_t *cb = static_cast<uint8_t *>(ycbcr.cb) + y * ycbcr.cstride;
			uint8_t *cr = static_cast<uint8_t *>(ycbcr.cr) + y * ycbcr.cstride;
			const uint8_t *su2 = du + static_cast<size_t>(y) * cw;
			const uint8_t *sv2 = dv + static_cast<size_t>(y) * cw;
			for (int x = 0; x < cw; x++) {
				cb[x * ycbcr.chroma_step] = su2[x];
				cr[x * ycbcr.chroma_step] = sv2[x];
			}
		}
		ok = true;
	} else {
		ALOGE("deliver: 色彩转换/缩放失败 rc=%d", rc);
	}

	mapper.unlock(dst);
	return ok;
}

/*
 * 把一帧 RGB 编成 JPEG 写进 Android 的 BLOB 缓冲。
 *
 * ★ Android 的约定（`CameraBlob.aidl` / `CameraBlobId.aidl`）：
 *   JPEG 数据从缓冲开头写，**缓冲末尾**放一个 8 字节的
 *   `CameraBlob{ blobId = JPEG(0x00FF), blobSizeBytes }`。
 *   框架按这个结构去找真实长度 —— 少了它，图片会被当成整个缓冲那么大。
 *
 * ⚠️ libcamera 给的是 B,G,R 顺序（formats::RGB888 → V4L2_PIX_FMT_BGR24，
 *    见 formats.cpp:185），而 libjpeg 的 JCS_EXT_BGR 正好对应它 ——
 *    用 JCS_RGB 会红蓝互换，而且不报任何错。
 */
bool Session::deliverJpeg(const uint8_t *rgb, buffer_handle_t dst,
			  int32_t dstW, int32_t dstH, int32_t blobSize, int quality)
{
	auto &mapper = android::GraphicBufferMapper::get();
	void *raw = nullptr;
	android::status_t st = mapper.lock(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
					   android::Rect(blobSize, 1), &raw);
	if (st != android::OK || !raw) {
		ALOGE("deliverJpeg: lock 失败: %d", st);
		return false;
	}

	/*
	 * 源尺寸与目标不同的话先缩放。
	 * ⚠️ libyuv **没有** RGBScale（我一开始想当然写了，编译器拦下）。
	 *    走 RGB24 → ARGB → ARGBScale 这条：libyuv 的 "ARGB" 在内存里是
	 *    B,G,R,A，正好对上 libjpeg 的 JCS_EXT_BGRA，不用再转回 3 通道。
	 */
	std::vector<uint8_t> scaled;
	const uint8_t *src = rgb;
	int srcStride = srcWidth_ * 3;
	int components = 3;
	J_COLOR_SPACE colorSpace = JCS_EXT_BGR;   /* ★ 字节序见上面的说明 */

	if (dstW != srcWidth_ || dstH != srcHeight_) {
		std::vector<uint8_t> argbSrc(static_cast<size_t>(srcWidth_) * srcHeight_ * 4);
		if (libyuv::RGB24ToARGB(rgb, srcStride, argbSrc.data(), srcWidth_ * 4,
					srcWidth_, srcHeight_) != 0) {
			ALOGE("deliverJpeg: RGB24ToARGB 失败");
			mapper.unlock(dst);
			return false;
		}
		scaled.resize(static_cast<size_t>(dstW) * dstH * 4);
		if (libyuv::ARGBScale(argbSrc.data(), srcWidth_ * 4, srcWidth_, srcHeight_,
				      scaled.data(), dstW * 4, dstW, dstH,
				      libyuv::kFilterBilinear) != 0) {
			ALOGE("deliverJpeg: ARGBScale 失败");
			mapper.unlock(dst);
			return false;
		}
		src = scaled.data();
		srcStride = dstW * 4;
		components = 4;
		colorSpace = JCS_EXT_BGRA;
	}

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	/* 直接写进 gralloc 缓冲；留出末尾 8 字节给 CameraBlob。 */
	const size_t maxJpeg = static_cast<size_t>(blobSize) - sizeof(CameraBlob);
	unsigned char *out = static_cast<unsigned char *>(raw);
	unsigned long outSize = maxJpeg;
	jpeg_mem_dest(&cinfo, &out, &outSize);

	cinfo.image_width = dstW;
	cinfo.image_height = dstH;
	cinfo.input_components = components;
	cinfo.in_color_space = colorSpace;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = const_cast<JSAMPROW>(
			src + static_cast<size_t>(cinfo.next_scanline) * srcStride);
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	const size_t jpegLen = outSize;
	jpeg_destroy_compress(&cinfo);

	bool ok = false;
	if (jpegLen > 0 && jpegLen <= maxJpeg) {
		/* jpeg_mem_dest 可能自己 malloc 了新缓冲（超出我们给的大小时），
		 * 那种情况下 out 不再指向 gralloc，要拷回去。 */
		if (out != raw)
			memcpy(raw, out, jpegLen);
		CameraBlob blob;
		blob.blobId = CameraBlobId::JPEG;
		blob.blobSizeBytes = static_cast<int32_t>(jpegLen);
		memcpy(static_cast<uint8_t *>(raw) + blobSize - sizeof(CameraBlob),
		       &blob, sizeof(CameraBlob));
		ok = true;
		ALOGD("JPEG %dx%d 质量%d → %zu 字节", dstW, dstH, quality, jpegLen);
	} else {
		ALOGE("deliverJpeg: JPEG 长度异常 %zu（上限 %zu）", jpegLen, maxJpeg);
	}
	if (out != raw)
		free(out);

	mapper.unlock(dst);
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
	}

	/*
	 * ⚠️★★ 请求【不能】在这里就还回空闲池 —— 下面还要读 req->buffers()。
	 *   还回去之后 processCaptureRequest 可能立刻取走并 reuse()，
	 *   于是我们读到的是一个已经被复用的对象。实测就是这样崩的
	 *   （tid name=CameraManager，栈顶 onRequestCompleted+1428）。
	 *   ★ 归还所有权和"用完它"是两件事，顺序不能图省事。
	 *   现在改成在函数最后才归还。
	 */
	if (req->buffers().empty()) {
		ALOGE("完成的请求里没有 buffer");
		std::lock_guard<std::mutex> lk(mutex_);
		freeRequests_.push_back(std::unique_ptr<libcamera::Request>(req));
		return;
	}
	const libcamera::FrameBuffer *fb = req->buffers().begin()->second;
	const int64_t timestamp = fb->metadata().timestamp;

	/* mmap 一次，分发给所有流。 */
	const auto &planes = fb->planes();
	const uint8_t *rgb = nullptr;
	void *srcMap = MAP_FAILED;
	size_t srcLen = 0;
	if (!planes.empty()) {
		srcLen = planes[0].offset + planes[0].length;
		srcMap = mmap(nullptr, srcLen, PROT_READ, MAP_SHARED,
			      planes[0].fd.get(), 0);
		if (srcMap != MAP_FAILED)
			rgb = static_cast<const uint8_t *>(srcMap) + planes[0].offset;
		else
			ALOGE("mmap libcamera 帧失败: %s", strerror(errno));
	}

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

	std::vector<bool> okv;
	for (auto &pb : pend.buffers) {
		bool ok = rgb && (pb.isBlob
				  ? deliverJpeg(rgb, pb.handle, pb.width, pb.height,
						pb.blobSize, /*quality=*/90)
				  : deliver(rgb, pb.handle, pb.width, pb.height));
		okv.push_back(ok);
		if (!ok) {
			NotifyMsg emsg;
			ErrorMsg e;
			e.frameNumber = pend.frameNumber;
			e.errorStreamId = pb.streamId;
			e.errorCode = ErrorCode::ERROR_BUFFER;
			emsg.set<NotifyMsg::Tag::error>(e);
			std::vector<NotifyMsg> emsgs;
			emsgs.push_back(std::move(emsg));
			cb_->notify(emsgs);
		}
	}
	if (srcMap != MAP_FAILED)
		munmap(srcMap, srcLen);

	CaptureResult result;
	result.frameNumber = pend.frameNumber;
	result.fmqResultSize = 0;
	result.partialResult = 1;
	/* ⬜ 之后应把 libcamera 的实际曝光/增益也填回去。 */
	result.result.metadata = buildResult(pend.settings, timestamp, /*pipelineDepth=*/4);

	for (size_t i = 0; i < pend.buffers.size(); i++) {
		StreamBuffer sb;
		sb.streamId = pend.buffers[i].streamId;
		sb.bufferId = pend.buffers[i].bufferId;
		sb.status = okv[i] ? BufferStatus::OK : BufferStatus::ERROR;
		/* ⚠️ buffer/fence 都留空：框架按 bufferId 认，
		 *    回传句柄会被当成一次新导入。 */
		result.outputBuffers.push_back(std::move(sb));
	}
	result.inputBuffer.streamId = -1;
	result.inputBuffer.bufferId = 0;

	std::vector<CaptureResult> results;
	results.push_back(std::move(result));
	cb_->processCaptureResult(results);

	/* 到这里才用完 req，可以还回空闲池了（见上面的说明）。 */
	{
		std::lock_guard<std::mutex> lk(mutex_);
		freeRequests_.push_back(std::unique_ptr<libcamera::Request>(req));
	}
}

} /* namespace gaokun3 */
