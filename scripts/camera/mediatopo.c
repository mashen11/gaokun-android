// gaokun3 media-controller 拓扑转储（media-ctl -p 的最小替代）
// ⚠️ 结构体一律来自内核树的 <linux/media.h>，不手抄偏移（M13 的教训）。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/media.h>

static const char *fn_name(unsigned f) {
    switch (f) {
    case MEDIA_ENT_F_IO_V4L:           return "IO_V4L";
    case MEDIA_ENT_F_CAM_SENSOR:       return "CAM_SENSOR";
    case MEDIA_ENT_F_VID_IF_BRIDGE:    return "VID_IF_BRIDGE";
    case MEDIA_ENT_F_PROC_VIDEO_SCALER:return "SCALER";
    case MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER: return "PIXFMT";
    case MEDIA_ENT_F_LENS:             return "LENS";
    case MEDIA_ENT_F_FLASH:            return "FLASH";
    default:                           return "?";
    }
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/media0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { fprintf(stderr, "打不开 %s: %s\n", dev, strerror(errno)); return 1; }

    struct media_device_info info;
    memset(&info, 0, sizeof info);
    if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
        fprintf(stderr, "DEVICE_INFO 失败: %s\n", strerror(errno)); return 1;
    }
    printf("设备 %s\n  driver=%s model=%s bus=%s\n\n", dev, info.driver, info.model, info.bus_info);

    struct media_v2_topology topo;
    memset(&topo, 0, sizeof topo);
    if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0) {
        fprintf(stderr, "G_TOPOLOGY(计数) 失败: %s\n", strerror(errno)); return 1;
    }
    struct media_v2_entity *ents = calloc(topo.num_entities, sizeof *ents);
    struct media_v2_pad    *pads = calloc(topo.num_pads,     sizeof *pads);
    struct media_v2_link   *lnks = calloc(topo.num_links,    sizeof *lnks);
    struct media_v2_interface *ifs = calloc(topo.num_interfaces, sizeof *ifs);
    topo.ptr_entities   = (__u64)(uintptr_t)ents;
    topo.ptr_pads       = (__u64)(uintptr_t)pads;
    topo.ptr_links      = (__u64)(uintptr_t)lnks;
    topo.ptr_interfaces = (__u64)(uintptr_t)ifs;
    if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0) {
        fprintf(stderr, "G_TOPOLOGY(取数) 失败: %s\n", strerror(errno)); return 1;
    }
    printf("实体 %u · pad %u · 链路 %u · 接口 %u\n\n",
           topo.num_entities, topo.num_pads, topo.num_links, topo.num_interfaces);

    // 只打印"有意思的"：传感器、以及与传感器相连的链路
    printf("=== 实体 ===\n");
    for (unsigned i = 0; i < topo.num_entities; i++)
        printf("  id=%-4u %-24s function=%s\n", ents[i].id, ents[i].name, fn_name(ents[i].function));

    printf("\n=== 链路（只列 pad→pad 的数据链路）===\n");
    for (unsigned i = 0; i < topo.num_links; i++) {
        unsigned t = lnks[i].flags & MEDIA_LNK_FL_LINK_TYPE;
        if (t != MEDIA_LNK_FL_DATA_LINK) continue;
        // 找 source/sink pad 所属实体
        const char *sn = "?", *kn = "?"; unsigned si = 0, ki = 0;
        for (unsigned p = 0; p < topo.num_pads; p++) {
            if (pads[p].id == lnks[i].source_id) {
                si = pads[p].index;
                for (unsigned e = 0; e < topo.num_entities; e++)
                    if (ents[e].id == pads[p].entity_id) sn = ents[e].name;
            }
            if (pads[p].id == lnks[i].sink_id) {
                ki = pads[p].index;
                for (unsigned e = 0; e < topo.num_entities; e++)
                    if (ents[e].id == pads[p].entity_id) kn = ents[e].name;
            }
        }
        printf("  %-24s:%u -> %-24s:%u  [%s%s]\n", sn, si, kn, ki,
               (lnks[i].flags & MEDIA_LNK_FL_ENABLED)   ? "已启用" : "未启用",
               (lnks[i].flags & MEDIA_LNK_FL_IMMUTABLE) ? " 不可变" : "");
    }
    close(fd);
    return 0;
}
