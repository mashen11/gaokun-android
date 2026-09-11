// gaokun3 相机通路实测：接链 -> 传格式 -> 抓帧
// ⚠️ 所有结构体/ioctl 来自内核树 uapi 头，不手抄。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>

/* 总线码 -> RDI 上对应的 V4L2 像素格式。
 * ★ 出处不是记忆：drivers/media/platform/qcom/camss/camss-vfe.c:59/96/145
 *   那张表原文就是 { MEDIA_BUS_FMT_SGBRG10_1X10, 10, V4L2_PIX_FMT_SGBRG10P, ... }。
 * ⚠️ RDI 是【裸转储】，不做去拜耳也不做色彩转换 —— video 节点的像素格式
 *   必须与传感器的总线码同族，否则 STREAMON 直接 EPIPE（媒体流水线校验不过），
 *   而错误信息完全不提"格式不匹配"。 */
static const struct { uint32_t code; uint32_t pixfmt; } kBusToPix[] = {
    { MEDIA_BUS_FMT_SBGGR8_1X8,   V4L2_PIX_FMT_SBGGR8   },
    { MEDIA_BUS_FMT_SGBRG8_1X8,   V4L2_PIX_FMT_SGBRG8   },
    { MEDIA_BUS_FMT_SGRBG8_1X8,   V4L2_PIX_FMT_SGRBG8   },
    { MEDIA_BUS_FMT_SRGGB8_1X8,   V4L2_PIX_FMT_SRGGB8   },
    { MEDIA_BUS_FMT_SBGGR10_1X10, V4L2_PIX_FMT_SBGGR10P },
    { MEDIA_BUS_FMT_SGBRG10_1X10, V4L2_PIX_FMT_SGBRG10P },
    { MEDIA_BUS_FMT_SGRBG10_1X10, V4L2_PIX_FMT_SGRBG10P },
    { MEDIA_BUS_FMT_SRGGB10_1X10, V4L2_PIX_FMT_SRGGB10P },
    { MEDIA_BUS_FMT_UYVY8_1X16,   V4L2_PIX_FMT_UYVY     },
    { MEDIA_BUS_FMT_YUYV8_1X16,   V4L2_PIX_FMT_YUYV     },
};
static uint32_t pix_for_bus(uint32_t code) {
    for (unsigned i = 0; i < sizeof kBusToPix / sizeof *kBusToPix; i++)
        if (kBusToPix[i].code == code) return kBusToPix[i].pixfmt;
    return 0;
}

#define DIE(...) do { fprintf(stderr, "✗ " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
static int xioctl(int fd, unsigned long req, void *arg, const char *what) {
    int r = ioctl(fd, req, arg);
    if (r < 0) fprintf(stderr, "  ! %s 失败: %s\n", what, strerror(errno));
    return r;
}

/* 实体名 -> /dev/<node>：/sys/class/video4linux/<n>/name 就等于实体名 */
static int open_by_entity(const char *ent, char *out, size_t outsz) {
    DIR *d = opendir("/sys/class/video4linux");
    if (!d) return -1;
    struct dirent *e; char p[512], nm[128];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(nm, sizeof nm, f)) { nm[strcspn(nm, "\n")] = 0;
            if (!strcmp(nm, ent)) { snprintf(out, outsz, "/dev/%s", e->d_name); fclose(f); closedir(d); return 0; } }
        fclose(f);
    }
    closedir(d); return -1;
}

/* 在 media 拓扑里按 实体名+pad序号 找 pad id */
struct topo { struct media_v2_entity *e; struct media_v2_pad *p; struct media_v2_link *l;
              struct media_v2_topology t; };
static void topo_load(int mfd, struct topo *T) {
    memset(&T->t, 0, sizeof T->t);
    if (ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &T->t) < 0) DIE("G_TOPOLOGY 计数失败: %s", strerror(errno));
    T->e = calloc(T->t.num_entities, sizeof *T->e);
    T->p = calloc(T->t.num_pads, sizeof *T->p);
    T->l = calloc(T->t.num_links, sizeof *T->l);
    T->t.ptr_entities = (uint64_t)(uintptr_t)T->e;
    T->t.ptr_pads     = (uint64_t)(uintptr_t)T->p;
    T->t.ptr_links    = (uint64_t)(uintptr_t)T->l;
    T->t.ptr_interfaces = 0; T->t.num_interfaces = 0;
    if (ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &T->t) < 0) DIE("G_TOPOLOGY 取数失败: %s", strerror(errno));
}
static uint32_t ent_id(struct topo *T, const char *name) {
    for (unsigned i = 0; i < T->t.num_entities; i++) if (!strcmp(T->e[i].name, name)) return T->e[i].id;
    DIE("拓扑里找不到实体 %s", name); return 0;
}
static uint32_t pad_id(struct topo *T, uint32_t eid, uint32_t idx) {
    for (unsigned i = 0; i < T->t.num_pads; i++)
        if (T->p[i].entity_id == eid && T->p[i].index == idx) return T->p[i].id;
    DIE("找不到 pad %u:%u", eid, idx); return 0;
}

static int link_enable(int mfd, struct topo *T, const char *src, uint32_t sp, const char *snk, uint32_t kp) {
    uint32_t se = ent_id(T, src), ke = ent_id(T, snk);
    uint32_t sid = pad_id(T, se, sp), kid = pad_id(T, ke, kp);
    struct media_link_desc ld; memset(&ld, 0, sizeof ld);
    ld.source.entity = se; ld.source.index = sp; ld.source.flags = MEDIA_PAD_FL_SOURCE;
    ld.sink.entity   = ke; ld.sink.index   = kp; ld.sink.flags   = MEDIA_PAD_FL_SINK;
    ld.flags = MEDIA_LNK_FL_ENABLED;
    (void)sid; (void)kid;
    int r = xioctl(mfd, MEDIA_IOC_SETUP_LINK, &ld, "SETUP_LINK");
    printf("  接链 %-16s:%u -> %-16s:%u  %s\n", src, sp, snk, kp, r == 0 ? "✅" : "❌");
    return r;
}

static int sd_set_fmt(const char *ent, uint32_t pad, uint32_t code, uint32_t w, uint32_t h,
                      uint32_t *ow, uint32_t *oh) {
    char dev[64];
    if (open_by_entity(ent, dev, sizeof dev) < 0) { printf("  ! 找不到 %s 的设备节点\n", ent); return -1; }
    int fd = open(dev, O_RDWR); if (fd < 0) { printf("  ! 打不开 %s\n", dev); return -1; }
    struct v4l2_subdev_format f; memset(&f, 0, sizeof f);
    f.pad = pad; f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    f.format.code = code; f.format.width = w; f.format.height = h;
    f.format.field = V4L2_FIELD_NONE;
    int r = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f);
    printf("  格式 %-16s pad%u  %ux%u code=0x%04x  %s%s\n", ent, pad, w, h, code,
           r == 0 ? "✅" : "❌ ", r == 0 ? "" : strerror(errno));
    if (r == 0 && (f.format.width != w || f.format.height != h || f.format.code != code))
        printf("      ↳ 驱动改成了 %ux%u code=0x%04x\n", f.format.width, f.format.height, f.format.code);
    if (r == 0) { if (ow) *ow = f.format.width; if (oh) *oh = f.format.height; }
    close(fd); return r;
}

int main(int argc, char **argv) {
    const char *SENSOR = "hi846 2-0020";
    const char *PHY = "msm_csiphy3", *CSID = "msm_csid0";
    const char *RDI = "msm_vfe0_rdi0", *VNODE = "msm_vfe0_video0";
    unsigned W = argc > 1 ? atoi(argv[1]) : 1280, H = argc > 2 ? atoi(argv[2]) : 720;
    unsigned NFR = argc > 3 ? atoi(argv[3]) : 3;

    int mfd = open("/dev/media0", O_RDWR); if (mfd < 0) DIE("打不开 /dev/media0: %s", strerror(errno));
    struct topo T; topo_load(mfd, &T);
    printf("拓扑: 实体 %u · pad %u · 链路 %u\n\n", T.t.num_entities, T.t.num_pads, T.t.num_links);

    /* 1) 传感器支持什么 */
    char sdev[64];
    if (open_by_entity(SENSOR, sdev, sizeof sdev) < 0) DIE("找不到传感器 %s 的 subdev 节点", SENSOR);
    int sfd = open(sdev, O_RDWR); if (sfd < 0) DIE("打不开 %s: %s", sdev, strerror(errno));
    printf("=== 传感器 %s (%s) 支持的格式 ===\n", SENSOR, sdev);
    uint32_t code0 = 0;
    for (unsigned i = 0; ; i++) {
        struct v4l2_subdev_mbus_code_enum mc; memset(&mc, 0, sizeof mc);
        mc.index = i; mc.pad = 0; mc.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        if (ioctl(sfd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mc) < 0) break;
        printf("  code[%u] = 0x%04x\n", i, mc.code);
        if (!code0) code0 = mc.code;
        for (unsigned j = 0; ; j++) {
            struct v4l2_subdev_frame_size_enum fs; memset(&fs, 0, sizeof fs);
            fs.index = j; fs.pad = 0; fs.code = mc.code; fs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (ioctl(sfd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fs) < 0) break;
            printf("      尺寸[%u] %ux%u .. %ux%u\n", j, fs.min_width, fs.min_height, fs.max_width, fs.max_height);
        }
    }
    close(sfd);
    if (!code0) DIE("传感器没报任何 mbus code");
    printf("\n选用 code=0x%04x  %ux%u\n\n", code0, W, H);

    /* 2) 接链 */
    printf("=== 接链 ===\n");
    link_enable(mfd, &T, PHY, 1, CSID, 0);
    link_enable(mfd, &T, CSID, 1, RDI, 0);

    /* 3) 沿链传格式 */
    printf("\n=== 传格式 ===\n");
    /* ★ 先让传感器自己协商，再把【它实际给的尺寸】往下游传 —— 上下游不一致
       会让 camss 在 streamon 时拒绝（媒体流水线校验）。 */
    uint32_t aw = W, ah = H;
    sd_set_fmt(SENSOR, 0, code0, W, H, &aw, &ah);
    if (aw != W || ah != H) printf("  → 下游一律改用传感器协商出的 %ux%u\n", aw, ah);
    W = aw; H = ah;
    sd_set_fmt(PHY,  0, code0, W, H, NULL, NULL);  sd_set_fmt(PHY,  1, code0, W, H, NULL, NULL);
    sd_set_fmt(CSID, 0, code0, W, H, NULL, NULL);  sd_set_fmt(CSID, 1, code0, W, H, NULL, NULL);
    sd_set_fmt(RDI,  0, code0, W, H, NULL, NULL);  sd_set_fmt(RDI,  1, code0, W, H, NULL, NULL);

    /* 4) video 节点 */
    char vdev[64];
    if (open_by_entity(VNODE, vdev, sizeof vdev) < 0) DIE("找不到 %s", VNODE);
    int vfd = open(vdev, O_RDWR); if (vfd < 0) DIE("打不开 %s: %s", vdev, strerror(errno));
    printf("\n=== video 节点 %s (%s) ===\n", VNODE, vdev);
    /* ⚠️ camss 走的是 multiplanar API —— 用单平面 type 时 ENUM_FMT 返回空、
       S_FMT 直接 EINVAL，看起来像"节点坏了"，其实只是问错了接口。 */
    struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
    xioctl(vfd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    int mplane = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    int btype = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    printf("  driver=%s caps=0x%08x  %s\n", cap.driver, caps, mplane ? "多平面(MPLANE)" : "单平面");
    printf("  支持的像素格式: ");
    uint32_t want = pix_for_bus(code0), pixfmt = 0, first = 0;
    for (unsigned i = 0; ; i++) {
        struct v4l2_fmtdesc fd_; memset(&fd_, 0, sizeof fd_);
        fd_.index = i; fd_.type = btype;
        if (ioctl(vfd, VIDIOC_ENUM_FMT, &fd_) < 0) break;
        printf("%.4s ", (char *)&fd_.pixelformat);
        if (!first) first = fd_.pixelformat;
        if (want && fd_.pixelformat == want) pixfmt = want;
    }
    printf("\n");
    if (!first) DIE("video 节点一个像素格式都不报");
    if (!want)
        printf("  ⚠️ 表里没有总线码 0x%04x 的对应像素格式，退回列表第一个\n", code0);
    else if (!pixfmt)
        printf("  ⚠️ 节点不支持 %.4s（总线码 0x%04x 应配的格式），退回列表第一个\n",
               (char *)&want, code0);
    else
        printf("  ★ 按总线码 0x%04x 选中 %.4s（camss-vfe.c 的映射表）\n",
               code0, (char *)&pixfmt);
    if (!pixfmt) pixfmt = first;
    struct v4l2_format vf; memset(&vf, 0, sizeof vf);
    vf.type = btype;
    if (mplane) {
        vf.fmt.pix_mp.width = W; vf.fmt.pix_mp.height = H;
        vf.fmt.pix_mp.pixelformat = pixfmt; vf.fmt.pix_mp.field = V4L2_FIELD_NONE;
        vf.fmt.pix_mp.num_planes = 1;
    } else {
        vf.fmt.pix.width = W; vf.fmt.pix.height = H;
        vf.fmt.pix.pixelformat = pixfmt; vf.fmt.pix.field = V4L2_FIELD_NONE;
    }
    if (xioctl(vfd, VIDIOC_S_FMT, &vf, "S_FMT") < 0) DIE("video 节点 S_FMT 失败");
    if (mplane)
        printf("  实际 %ux%u fmt=%.4s planes=%u bytesperline=%u sizeimage=%u\n",
               vf.fmt.pix_mp.width, vf.fmt.pix_mp.height, (char *)&vf.fmt.pix_mp.pixelformat,
               vf.fmt.pix_mp.num_planes, vf.fmt.pix_mp.plane_fmt[0].bytesperline,
               vf.fmt.pix_mp.plane_fmt[0].sizeimage);
    else
        printf("  实际 %ux%u fmt=%.4s bytesperline=%u sizeimage=%u\n",
               vf.fmt.pix.width, vf.fmt.pix.height, (char *)&vf.fmt.pix.pixelformat,
               vf.fmt.pix.bytesperline, vf.fmt.pix.sizeimage);

    /* 5) 申请缓冲并抓帧 */
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = 4; rb.type = btype; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) DIE("REQBUFS 失败");
    printf("  分到 %u 个缓冲\n", rb.count);
    void *bufs[8]; size_t lens[8];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        struct v4l2_plane pl[VIDEO_MAX_PLANES]; memset(pl, 0, sizeof pl);
        b.type = rb.type; b.memory = rb.memory; b.index = i;
        if (mplane) { b.m.planes = pl; b.length = VIDEO_MAX_PLANES; }
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF") < 0) DIE("QUERYBUF 失败");
        size_t blen = mplane ? pl[0].length : b.length;
        off_t boff = mplane ? pl[0].m.mem_offset : b.m.offset;
        bufs[i] = mmap(NULL, blen, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, boff);
        lens[i] = blen;
        if (bufs[i] == MAP_FAILED) DIE("mmap 失败: %s", strerror(errno));
        if (xioctl(vfd, VIDIOC_QBUF, &b, "QBUF") < 0) DIE("QBUF 失败");
    }
    int type = btype;
    printf("\n=== STREAMON ===\n");
    if (xioctl(vfd, VIDIOC_STREAMON, &type, "STREAMON") < 0) DIE("STREAMON 失败 —— 通路没打通");
    printf("  ✅ 流已开\n");

    /* ★ 因为"开机后只有第一次 STREAMON 能成功"（camss 下电有缺陷，见案卷），
       两个条件必须放在【同一次流】里对照，否则第二组永远拿不到数据。 */
    for (unsigned round = 0; round < 4; round++) {
    if (round == 2 || round == 3) {
        int pat = (round == 2) ? 2 : 9;   /* 2 = 100% 彩条, 9 = 分辨率图案 */
        /* ★★ 终极判据：让传感器自己生成测试图案。
           它绕开镜头与环境光 —— 如果 VFE 写出来的是【有结构的已知图案】，
           那么 sensor→CSIPHY→CSID→VFE→DMA 整条通路就被证明是对的，
           与"屋里黑不黑"彻底无关。 */
        printf("\n=== 打开传感器测试图案，再抓一轮 ===\n");
        int sfd3 = open(sdev, O_RDWR);
        if (sfd3 >= 0) {
            struct v4l2_querymenu mu; 
            if (round == 2) for (int v = 0; v <= 9; v++) {
                memset(&mu, 0, sizeof mu); mu.id = V4L2_CID_TEST_PATTERN; mu.index = v;
                if (ioctl(sfd3, VIDIOC_QUERYMENU, &mu) == 0)
                    printf("    图案[%d] = %s\n", v, mu.name);
            }
            struct v4l2_control c = { .id = V4L2_CID_TEST_PATTERN, .value = pat };
            if (ioctl(sfd3, VIDIOC_S_CTRL, &c) < 0)
                printf("  ! 设测试图案失败: %s\n", strerror(errno));
            else printf("  已设 Test Pattern = %d\n", pat);
            close(sfd3);
        }
    } else if (round == 1) {
        printf("\n=== 把曝光/增益拉到最大，再抓一轮 ===\n");
        int sfd2 = open(sdev, O_RDWR);
        if (sfd2 < 0) { printf("  ! 打不开传感器 subdev\n"); }
        else {
            struct v4l2_query_ext_ctrl q; memset(&q, 0, sizeof q);
            q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
            while (ioctl(sfd2, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
                int want = (q.id == V4L2_CID_EXPOSURE || q.id == V4L2_CID_ANALOGUE_GAIN
                            || q.id == V4L2_CID_GAIN || q.id == V4L2_CID_DIGITAL_GAIN);
                printf("  控件 %-22s id=0x%08x [%lld..%lld] 默认=%lld%s\n",
                       q.name, q.id, (long long)q.minimum, (long long)q.maximum,
                       (long long)q.default_value, want ? "  ← 拉满" : "");
                if (want && !(q.flags & V4L2_CTRL_FLAG_READ_ONLY)) {
                    struct v4l2_control c = { .id = q.id, .value = (int)q.maximum };
                    if (ioctl(sfd2, VIDIOC_S_CTRL, &c) < 0)
                        printf("      ! 设置失败: %s\n", strerror(errno));
                }
                q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
            }
            close(sfd2);
        }
    } else {
        printf("\n=== 第一轮：传感器默认曝光/增益 ===\n");
    }
    for (unsigned n = 0; n < NFR; n++) {
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        struct v4l2_plane pl[VIDEO_MAX_PLANES]; memset(pl, 0, sizeof pl);
        b.type = type; b.memory = V4L2_MEMORY_MMAP;
        if (mplane) { b.m.planes = pl; b.length = VIDEO_MAX_PLANES; }
        fd_set fds; FD_ZERO(&fds); FD_SET(vfd, &fds);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        int r = select(vfd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) { printf("  帧 %u: 超时/出错（3 秒无数据）\n", n); break; }
        if (xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF") < 0) break;
        /* 简单统计：非零字节比例 + 均值，用来判断"真有图像"而不是全黑 */
        unsigned used = mplane ? pl[0].bytesused : b.bytesused;
        unsigned char *p = bufs[b.index]; unsigned long sum = 0, nz = 0;
        for (unsigned k = 0; k < used; k++) { sum += p[k]; if (p[k]) nz++; }
        printf("  帧 %u: %u 字节 · 非零 %.1f%% · 均值 %.1f · seq=%u\n",
               n, used, used ? 100.0 * nz / used : 0.0,
               used ? (double)sum / used : 0.0, b.sequence);
        if (n == 0 && used) {
            char fn[64]; snprintf(fn, sizeof fn, "/data/local/tmp/frame-r%u.raw", round);
            FILE *o = fopen(fn, "wb");
            if (o) { fwrite(p, 1, used, o); fclose(o); printf("      ↳ 已存 %s\n", fn); }
        }
        xioctl(vfd, VIDIOC_QBUF, &b, "QBUF");
    }
    }
    xioctl(vfd, VIDIOC_STREAMOFF, &type, "STREAMOFF");
    for (unsigned i = 0; i < rb.count; i++) munmap(bufs[i], lens[i]);
    close(vfd); close(mfd);
    printf("\n完成。\n");
    return 0;
}
