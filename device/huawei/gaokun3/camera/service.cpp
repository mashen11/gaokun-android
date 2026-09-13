/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL 服务入口。
 *
 * ★ 注册方式照抄 AOSP 自己的参考实现
 *   （hardware/interfaces/camera/provider/default/external-service.cpp 的 main）：
 *   服务名 = <ICameraProvider::descriptor> + "/" + 实例名。
 */
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include "Provider.h"

int main()
{
	ALOGI("gaokun3 相机 HAL 启动中");

	/* 相机 HAL 的 binder 调用并发不高，但取帧回调会占线程。 */
	ABinderProcess_setThreadPoolMaxThreadCount(6);

	auto provider = ndk::SharedRefBase::make<gaokun3::Provider>();
	if (!provider->init()) {
		/* ⚠️ 故意用非零退出而不是注册一个空 provider：
		 *    空列表在框架里表现为"没有相机"，真正的原因（camss 没 probe /
		 *    用错了 dtb）就被埋掉了。让 init 重启我们并在日志里留痕。 */
		ALOGE("Provider::init 失败 —— 不注册服务。"
		      "检查 camss 是否 probe（/dev/media*）以及是否用了带相机的 dtb");
		return 1;
	}

	const std::string name = std::string(gaokun3::Provider::descriptor) + "/" +
				 gaokun3::kInstance;
	binder_exception_t ret = AServiceManager_addService(provider->asBinder().get(),
							    name.c_str());
	if (ret != EX_NONE) {
		ALOGE("注册 %s 失败: %d", name.c_str(), ret);
		return 1;
	}
	ALOGI("已注册 %s", name.c_str());

	ABinderProcess_joinThreadPool();
	return 1;   /* joinThreadPool 不该返回 */
}
