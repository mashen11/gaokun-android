# gaokun3 安装器后端。被两个前端共用：
#   * scripts/install-gaokun3.sh   命令行
#   * live/installer/              图形安装器（LiveCD）
#
# ★ 一套实现两个前端，是因为本仓反复吃过"两份拷贝各自漂移"的亏。
#
# 全部输出都是【面向机器的行记录】：`键=值` 一行一条，前缀标明类型。
# 不用 JSON —— Alpine 基础系统里没有 jq，而 C 侧解析 key=value 比解析 JSON
# 便宜得多。人看的信息一律走 stderr。
#
#   . installer-lib.sh
#   gk3_probe                      # 列出磁盘 / 分区 / 空闲区
#   gk3_plan  <参数…>              # 算出分区方案（纯计算，不碰磁盘）
#   gk3_apply <方案文件> <发布目录> # 执行（唯一会写盘的函数）

# ── 布局常量 ────────────────────────────────────────────────────────────────
# 说明见 install-gaokun3.sh 顶部那段（为什么 esp 必须叫 esp、
# 为什么 misc 挂载点必须是 /misc 等等）。
GK3_ESP_MIB=300
GK3_MISC_MIB=4
GK3_METADATA_MIB=32
GK3_SUPER_MIB=12288
GK3_BOOT_MIB=64          # 每个槽位；与 BoardConfig 的 BOARD_BOOTIMAGE_PARTITION_SIZE 对齐
# ★ 救援系统从 24 GiB 降到 1 GiB —— Stage 7 之后它是 55 MiB 的 squashfs，
#   不再是一整套装在盘上的 Ubuntu。1 GiB 给日后换更大的镜像留足余量。
GK3_RESCUE_MIB=1024
GK3_USERDATA_MIN_MIB=8192

# 双系统安装时，ESP 里至少要能放下我们的两个槽位（内核+ramdisk+dtb ×2）
GK3_ESP_NEED_MIB=150

gk3_log()  { echo "$*" >&2; }
gk3_die()  { echo "!! $*" >&2; return 1; }
gk3_prog() { echo "PROGRESS $1 $2" >&2; }   # $1=百分比 $2=说明

# ── 探测 ────────────────────────────────────────────────────────────────────
# 输出：
#   DISK path=/dev/nvme0n1 size_mib=488386 model=... removable=0
#   PART path=/dev/nvme0n1p1 num=1 start=2048 end=616447 size_mib=300 \
#        type=ef00 name=esp fs=vfat fslabel=... os=windows|linux|android|
#   FREE disk=/dev/nvme0n1 start=616448 end=… size_mib=…
gk3_probe() {
    local d
    for d in /sys/block/*; do
        local name; name=$(basename "$d")
        # ⚠️ 正常情况下跳过 loop —— 没人往 loop 设备装系统。但测试需要它，
        #    所以给一个显式开关：GK3_ALLOW_LOOP=1。
        #    （否则 test-shrink.sh 没法用 gk3_probe 验"空间释放出来了没有"，
        #     只能另写一套算法，而那就等于测了两份不同的逻辑。）
        case "$name" in
            loop*)  [ "${GK3_ALLOW_LOOP:-0}" = 1 ] || continue ;;
            ram*|zram*|dm-*|sr*|md*) continue ;;
        esac
        [ -e "/dev/$name" ] || continue
        local sectors; sectors=$(cat "$d/size" 2>/dev/null || echo 0)
        [ "$sectors" -gt 0 ] || continue
        # 512 字节扇区 → MiB
        local size_mib=$(( sectors / 2048 ))
        [ "$size_mib" -ge 1024 ] || continue      # 小于 1 GiB 的不当安装目标
        local model removable
        model=$(cat "$d/device/model" 2>/dev/null | tr -d ' \n' || echo "?")
        removable=$(cat "$d/removable" 2>/dev/null || echo 0)
        echo "DISK path=/dev/$name size_mib=$size_mib model=${model:-?} removable=$removable"
        gk3__probe_parts "/dev/$name" "$sectors"
    done
}

# ⚠️ 用 sgdisk 而不是 lsblk：我们要的是【分区表层面】的起止扇区和类型 GUID，
#    而且要能算出空闲区间 —— lsblk 不报空闲区间。
gk3__probe_parts() {
    local disk=$1 total_sectors=$2
    command -v sgdisk >/dev/null || { gk3_log "缺 sgdisk，跳过 $disk 的分区探测"; return 0; }

    local first_usable last_usable
    first_usable=$(sgdisk -p "$disk" 2>/dev/null | sed -n 's/^First usable sector is \([0-9]*\).*/\1/p')
    last_usable=$(sgdisk -p "$disk" 2>/dev/null | sed -n 's/.*last usable sector is \([0-9]*\).*/\1/p')
    [ -n "$first_usable" ] || first_usable=2048
    [ -n "$last_usable" ] || last_usable=$(( total_sectors - 2048 ))

    # 收集分区，按起始扇区排序
    local tmp; tmp=$(mktemp)
    sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1" "$2" "$3}' | sort -k2 -n > "$tmp"

    local cursor=$first_usable num start end
    while read -r num start end; do
        [ -n "$num" ] || continue
        if [ "$start" -gt "$cursor" ]; then
            gk3__emit_free "$disk" "$cursor" $(( start - 1 ))
        fi
        local part; part=$(gk3_partpath "$disk" "$num")
        local ptype pname fstype fslabel
        ptype=$(sgdisk -i "$num" "$disk" 2>/dev/null | sed -n 's/^Partition GUID code: \([0-9A-Fa-f-]*\).*/\1/p')
        pname=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)
        fstype=$(blkid -o value -s TYPE "$part" 2>/dev/null || echo "")
        fslabel=$(blkid -o value -s LABEL "$part" 2>/dev/null || echo "")
        # 也报 KiB：本机 misc 只有 1007 KiB（GPT 头之后那段闲置空间），
        # 只报 MiB 会显示成 "0 MiB"，界面上看着像个空分区。
        # 实测发现的 —— 合成数据里没有这种小分区。
        echo "PART path=$part num=$num start=$start end=$end" \
             "size_mib=$(( (end - start + 1) / 2048 )) size_kib=$(( (end - start + 1) / 2 ))" \
             "type=${ptype:-?} name=${pname:-} fs=${fstype:-} fslabel=${fslabel:-}" \
             "os=$(gk3__guess_os "$part" "$ptype" "$pname" "$fstype")"
        cursor=$(( end + 1 ))
    done < "$tmp"
    rm -f "$tmp"

    if [ "$cursor" -lt "$last_usable" ]; then
        gk3__emit_free "$disk" "$cursor" "$last_usable"
    fi
}

gk3__emit_free() {
    local disk=$1 start=$2 end=$3
    local mib=$(( (end - start + 1) / 2048 ))
    # 小于 16 MiB 的碎片没有意义，不报（GPT 对齐留下的缝隙）
    [ "$mib" -ge 16 ] || return 0
    echo "FREE disk=$disk start=$start end=$end size_mib=$mib"
}

# 认出分区上大概是什么系统 —— 只用于界面提示，不参与任何判断逻辑
gk3__guess_os() {
    local part=$1 ptype=$2 pname=$3 fstype=$4
    case "$pname" in
        esp) echo "esp"; return ;;
        super|userdata|metadata|misc|boot_a|boot_b) echo "android"; return ;;
    esac
    case "$ptype" in
        C12A7328-F81F-11D2-BA4B-00A0C93EC93B) echo "esp"; return ;;
        DE94BBA4-06D1-4D40-A16A-BFD50179D6AC) echo "winre"; return ;;
        E3C9E316-0B5C-4DB8-817D-F92DF00215AE) echo "msr"; return ;;
    esac
    case "$fstype" in
        ntfs) echo "windows" ;;
        ext4|ext3|btrfs|xfs) echo "linux" ;;
        f2fs) echo "android" ;;
        vfat) echo "fat" ;;
        crypto_LUKS) echo "luks" ;;
        *) echo "" ;;
    esac
}

# nvme 是 p1，sd 是 1
gk3_partpath() {
    case "$1" in
        *[0-9]) echo "$1p$2" ;;
        *)      echo "$1$2" ;;
    esac
}

# ── 方案计算 ────────────────────────────────────────────────────────────────
# 纯计算，不碰磁盘 —— 所以可以在任何机器上跑、可以单元测。
#
#   gk3_plan --disk /dev/nvme0n1 --mode wipe|alongside --rescue yes|no \
#            [--region-start S --region-end E] [--esp PATH] [--userdata-mib N]
#
# 输出（顺序即执行顺序）：
#   PLAN op=wipe    disk=...
#   PLAN op=mkpart  num=0 name=super start=... end=... type=... size_mib=...
#   PLAN op=useesp  path=/dev/nvme0n1p1
#   PLANSUM total_mib=... userdata_mib=... rescue=yes|no mode=...
# 失败：
#   PLANERR msg=...
#
# ⚠️ 所有分区起始都对齐到 1 MiB（2048 扇区）。不对齐会让 NVMe 的写放大变差，
#    而且 sgdisk 会自己挪，挪完之后我们算出来的 end 就对不上了。
GK3_CUR=0
GK3_TYPE_ESP=ef00
GK3_TYPE_DATA=8300

gk3_plan() {
    local disk="" mode="wipe" rescue="no" rstart="" rend="" esp="" ud_mib=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --disk) disk=$2; shift 2 ;;
            --mode) mode=$2; shift 2 ;;
            --rescue) rescue=$2; shift 2 ;;
            --region-start) rstart=$2; shift 2 ;;
            --region-end) rend=$2; shift 2 ;;
            --esp) esp=$2; shift 2 ;;
            --userdata-mib) ud_mib=$2; shift 2 ;;
            --disk-size-mib) GK3_FAKE_DISK_MIB=$2; shift 2 ;;   # 只给自测用
            *) echo "PLANERR msg=unknown-arg:$1"; return 1 ;;
        esac
    done
    [ -n "$disk" ] || { echo "PLANERR msg=no-disk"; return 1; }

    # ⚠️★ ① 分区名查重。Android 的 first-stage mount 走 by-name/super，那是
    #   ueventd 按 PARTLABEL 建的符号链接 —— 重名时哪个赢【不确定】。在一块
    #   已经装过我们系统的盘上跑 alongside，就会建出第二个 super、第二个
    #   userdata，然后开机随机挂错分区。整盘模式不用查（分区表会被清空）。
    #   ⚠️ 取名字用 cut 不用 sed 反捕获 —— 第一版用了 sed 的反向引用，那个
    #      反斜杠在写文件的路上被吃成 0x01 控制字符，于是守卫看着在那儿
    #      却从不触发。实机验出来的。
    if [ "$mode" != wipe ] && command -v sgdisk >/dev/null; then
        local dup="" nm n
        for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
            nm=$(sgdisk -i "$n" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)
            [ "$nm" = esp ] && continue
            case " misc metadata boot_a boot_b super userdata gk3rescue " in
                *" $nm "*) dup="$dup $nm" ;;
            esac
        done
        if [ -n "$dup" ]; then
            echo "PLANERR msg=partlabel-conflict names=$(echo $dup | tr " " ",")"
            return 1
        fi
    fi

    # ⚠️★ ⑤ MBR 盘。sgdisk 会把 MBR 盘【静默转成 GPT】，原布局当场没了。
    #   老 Windows 装机大多是 MBR，所以这不是理论风险。
    if command -v sgdisk >/dev/null; then
        if sgdisk -p "$disk" 2>&1 | grep -qi "MBR only"; then
            echo "PLANERR msg=mbr-disk"
            return 1
        fi
    fi

    local cur last
    if [ "$mode" = wipe ]; then
        local total_mib=${GK3_FAKE_DISK_MIB:-}
        if [ -z "$total_mib" ]; then
            local sectors; sectors=$(cat "/sys/block/$(basename "$disk")/size" 2>/dev/null || echo 0)
            total_mib=$(( sectors / 2048 ))
        fi
        [ "$total_mib" -gt 0 ] || { echo "PLANERR msg=cannot-size-disk"; return 1; }
        cur=2048
        last=$(( total_mib * 2048 - 2048 ))       # 尾部给备份 GPT 留 1 MiB
        echo "PLAN op=wipe disk=$disk"
    else
        [ -n "$rstart" ] && [ -n "$rend" ] || { echo "PLANERR msg=alongside-needs-region"; return 1; }
        # 起点向上对齐到 1 MiB
        cur=$(( (rstart + 2047) / 2048 * 2048 ))
        last=$rend
        if [ -z "$esp" ]; then
            echo "PLANERR msg=alongside-needs-existing-esp"; return 1
        fi
        echo "PLAN op=useesp path=$esp need_mib=$GK3_ESP_NEED_MIB"
    fi

    local avail_mib=$(( (last - cur + 1) / 2048 ))

    # 固定开销
    local fixed=$(( GK3_MISC_MIB + GK3_METADATA_MIB + GK3_BOOT_MIB * 2 + GK3_SUPER_MIB ))
    [ "$mode" = wipe ] && fixed=$(( fixed + GK3_ESP_MIB ))
    [ "$rescue" = yes ] && fixed=$(( fixed + GK3_RESCUE_MIB ))

    local need=$(( fixed + GK3_USERDATA_MIN_MIB ))
    if [ "$avail_mib" -lt "$need" ]; then
        echo "PLANERR msg=not-enough-space avail_mib=$avail_mib need_mib=$need"
        return 1
    fi

    # userdata 吃掉剩下的（除非调用方指定）
    local userdata_mib=$(( avail_mib - fixed ))
    if [ -n "$ud_mib" ]; then
        [ "$ud_mib" -ge "$GK3_USERDATA_MIN_MIB" ] || { echo "PLANERR msg=userdata-too-small min_mib=$GK3_USERDATA_MIN_MIB"; return 1; }
        [ "$ud_mib" -le "$userdata_mib" ] || { echo "PLANERR msg=userdata-too-big max_mib=$userdata_mib"; return 1; }
        userdata_mib=$ud_mib
    fi

    # ⚠️ 顺序即磁盘顺序，userdata 必须最后 —— 这样以后扩容不用挪任何东西。
    #    （本仓 M6 扩 /data 时正是靠这一点。）
    GK3_CUR=$cur
    if [ "$mode" = wipe ]; then
        gk3__emit_part "$disk" esp      "$GK3_ESP_MIB"      "$GK3_TYPE_ESP"
    fi
    gk3__emit_part "$disk" misc     "$GK3_MISC_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" metadata "$GK3_METADATA_MIB" "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" boot_a   "$GK3_BOOT_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" boot_b   "$GK3_BOOT_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" super    "$GK3_SUPER_MIB"    "$GK3_TYPE_DATA"
    if [ "$rescue" = yes ]; then
        gk3__emit_part "$disk" gk3rescue "$GK3_RESCUE_MIB" "$GK3_TYPE_DATA"
    fi
    gk3__emit_part "$disk" userdata "$userdata_mib"     "$GK3_TYPE_DATA"

    # ★ 最后一条不能越过可用区尾部。fixed+userdata 的算术上面已经保证了，
    #   但这里【再独立验一次】—— 算错的代价是写到别人的分区上。
    if [ "$(( GK3_CUR - 1 ))" -gt "$last" ]; then
        echo "PLANERR msg=plan-overruns-region end=$(( GK3_CUR - 1 )) limit=$last"
        return 1
    fi

    echo "PLANSUM mode=$mode rescue=$rescue avail_mib=$avail_mib fixed_mib=$fixed userdata_mib=$userdata_mib"
}

# 打印一条 mkpart 记录，并把游标 GK3_CUR 推到下一个 1 MiB 边界。
#
# ⚠️★ 第一版是"回显新游标"，调用方写 `cur=$(gk3__emit_part …)` ——
#   于是 **PLAN 行也被 $() 吞进变量里**，一条都没到 stdout，
#   而 cur 变成了带换行的字符串，下一次算术当场 unbound variable。
#   自测（test-plan.sh）一跑就抓到了。函数既要输出数据又要回传值时，
#   **走全局变量，别混用 stdout**。
gk3__emit_part() {
    local disk=$1 name=$2 mib=$3 type=$4
    local start=$GK3_CUR
    local end=$(( start + mib * 2048 - 1 ))
    echo "PLAN op=mkpart disk=$disk num=0 name=$name start=$start end=$end size_mib=$mib type=$type"
    GK3_CUR=$(( end + 1 ))
}

# ── 执行 ────────────────────────────────────────────────────────────────────
# 这是【唯一】会写盘的函数。
#
#   gk3_apply --disk X --mode wipe|alongside --rescue yes|no --release DIR \
#             [--region-start S --region-end E --esp PATH]
#
# 进度打在 stderr：`PROGRESS <百分比> <说明>`，其余是日志。
#
# ⚠️★ GK3_DRYRUN=1 时【只打印不执行】。写这个开关不是为了方便 ——
#   是因为这段代码一旦错了就是别人的一整块盘，而它没法在 CI 里跑。
#   任何改动都应当先用 dry-run 看一遍要执行的命令序列。
gk3_apply() {
    local disk="" mode=wipe rescue=no rel="" rstart="" rend="" esp=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --disk) disk=$2; shift 2 ;;
            --mode) mode=$2; shift 2 ;;
            --rescue) rescue=$2; shift 2 ;;
            --release) rel=$2; shift 2 ;;
            --region-start) rstart=$2; shift 2 ;;
            --region-end) rend=$2; shift 2 ;;
            --esp) esp=$2; shift 2 ;;
            *) gk3_die "apply: 不认识的参数 $1"; return 1 ;;
        esac
    done
    [ -n "$disk" ] || { gk3_die "apply 要 --disk"; return 1; }
    [ -n "$rel" ] && [ -d "$rel" ] || { gk3_die "apply 要 --release <目录>"; return 1; }

    local DRY=${GK3_DRYRUN:-0}
    gk3__run() {
        if [ "$DRY" = 1 ]; then echo "DRY: $*"; else
            echo "+ $*"
            "$@" || { gk3_die "失败：$*"; return 1; }
        fi
    }

    # ── 安全闸 1：不能写自己正跑在上面的那块盘 ──────────────────────────
    # ⚠️ 安装器要么从 U 盘跑、要么从内置盘的救援分区跑。后者做整盘清空
    #    等于把自己脚下的地板锯掉 —— 而且是【跑到一半】才死，盘已经毁了。
    local medium_dev medium_disk
    medium_dev=$(findmnt -no SOURCE /media/gk3 2>/dev/null || echo "")
    if [ -n "$medium_dev" ]; then
        medium_disk=$(lsblk -no PKNAME "$medium_dev" 2>/dev/null | head -1)
        [ -n "$medium_disk" ] && medium_disk=/dev/$medium_disk
    fi
    if [ "$mode" = wipe ] && [ -n "${medium_disk:-}" ] && [ "$medium_disk" = "$disk" ]; then
        gk3_die "拒绝：安装介质（${medium_dev}）就在目标盘 $disk 上，整盘清空会锯掉自己脚下的地板"
        return 1
    fi

    # ── 安全闸 2：目标盘上不能有已挂载的分区 ────────────────────────────
    local mounted
    mounted=$(lsblk -nro MOUNTPOINT "$disk" 2>/dev/null | grep -v '^$' | tr '\n' ' ')
    if [ -n "$mounted" ] && [ "$mode" = wipe ]; then
        gk3_die "拒绝：$disk 上还有挂载着的分区（${mounted}）"
        return 1
    fi

    # ── 方案 ────────────────────────────────────────────────────────────
    gk3_prog 2 "计算分区方案"
    local plan
    plan=$(gk3_plan --disk "$disk" --mode "$mode" --rescue "$rescue" \
                    ${rstart:+--region-start "$rstart"} ${rend:+--region-end "$rend"} \
                    ${esp:+--esp "$esp"}) || { echo "$plan"; return 1; }
    printf '%s\n' "$plan" | grep -q '^PLANERR' && { printf '%s\n' "$plan" | grep '^PLANERR'; return 1; }

    # ── 建分区 ──────────────────────────────────────────────────────────
    # ⚠️★ ④ 动手之前先把分区表备份到介质上。出事能一条命令还原：
    #     sgdisk --load-backup=<文件> <盘>
    #   代价是几十 KB 和一秒钟；没有它的话，改错分区表就只能靠猜。
    if [ "${GK3_DRYRUN:-0}" != 1 ]; then
        local bkdir bk
        bkdir=/media/gk3/gaokun3
        mount -o remount,rw /media/gk3 2>/dev/null || true
        [ -d "$bkdir" ] || bkdir=/tmp
        bk="$bkdir/gpt-backup-$(basename "$disk")-$(date +%Y%m%d-%H%M%S).bin"
        if sgdisk --backup="$bk" "$disk" >/dev/null 2>&1; then
            echo "分区表已备份到 ${bk}（还原：sgdisk --load-backup=$bk ${disk}）"
        else
            echo "警告：分区表备份失败（继续，但出事就没有还原点了）"
        fi
        sync
    fi

    gk3_prog 5 "写分区表"
    if printf '%s\n' "$plan" | grep -q '^PLAN op=wipe'; then
        gk3__run sgdisk --zap-all "$disk" || return 1
    fi
    local line name start end ptype
    while read -r line; do
        case "$line" in "PLAN op=mkpart"*) ;; *) continue ;; esac
        name=$(gk3__f "$line" name); start=$(gk3__f "$line" start)
        end=$(gk3__f "$line" end);   ptype=$(gk3__f "$line" type)
        gk3__run sgdisk -n "0:${start}:${end}" -t "0:${ptype}" -c "0:${name}" "$disk" || return 1
    done <<EOF
$(printf '%s\n' "$plan")
EOF
    gk3__run partprobe "$disk" || true
    [ "$DRY" = 1 ] || sleep 2

    # ── 格式化 ──────────────────────────────────────────────────────────
    gk3_prog 15 "格式化"
    # ⚠️★ 每一个分区节点都必须【解析成功且确实是块设备】才往下走。
    #   loop 设备实测暴露过：partprobe 还没沉降时 gk3__bylabel 会返回空串，
    #   于是命令变成 `dd of=` / `mkfs.ext4 -F ""` —— 那种情况下会发生什么
    #   完全不可预料，而此时分区表已经写下去了，盘已经不是原来的盘。
    #   **宁可在这里停住，也不能带着空路径继续。**
    local p_esp p_meta p_data p_super p_boota p_bootb p_resc p_misc
    p_esp=$(gk3__need_part "$disk" esp "$mode")     || return 1
    p_misc=$(gk3__need_part "$disk" misc "$mode")   || return 1
    p_meta=$(gk3__need_part "$disk" metadata "$mode")   || return 1
    p_data=$(gk3__need_part "$disk" userdata "$mode")   || return 1
    p_super=$(gk3__need_part "$disk" super "$mode")     || return 1
    p_boota=$(gk3__need_part "$disk" boot_a "$mode")    || return 1
    p_bootb=$(gk3__need_part "$disk" boot_b "$mode")    || return 1
    if [ "$rescue" = yes ]; then
        p_resc=$(gk3__need_part "$disk" gk3rescue "$mode") || return 1
    fi

    if [ "$mode" = wipe ]; then
        gk3__run mkfs.vfat -F 32 -n ESP "$p_esp" || return 1
    else
        p_esp=$esp    # 复用现有 ESP，绝不格式化它
        # ⚠️★ ② 真的去量它有多少空闲。原先只在 plan 里打一行 need_mib=150
        #   却从不验证 —— 不够的话分区表已经改完、super 已经写完，然后死在
        #   装引导链那一步，留下一块半装的盘。
        if [ "${GK3_DRYRUN:-0}" != 1 ]; then
            local em fm; em=$(mktemp -d)
            if mount -t vfat "$p_esp" "$em" 2>/dev/null; then
                fm=$(df -m "$em" | awk "NR==2{print \$4}")
                umount "$em"; rmdir "$em" 2>/dev/null
                echo "现有 ESP $p_esp 空闲 ${fm} MiB（需要 ${GK3_ESP_NEED_MIB}）"
                if [ "${fm:-0}" -lt "$GK3_ESP_NEED_MIB" ]; then
                    gk3_die "ESP 空间不够：只有 ${fm} MiB，需要 ${GK3_ESP_NEED_MIB} MiB。请先在原系统里清理 EFI 分区。"
                    return 1
                fi
            else
                rmdir "$em" 2>/dev/null
                gk3_die "挂不上现有 ESP $p_esp —— 不敢往一个读不了的 ESP 上装引导链"
                return 1
            fi
        fi
        echo "复用现有 ESP：${p_esp}（不格式化）"
    fi
    gk3__run mkfs.ext4 -q -F -L metadata "$p_meta" || return 1
    gk3__run mkfs.ext4 -q -F -L userdata "$p_data" || return 1
    # misc 必须是全零：bootloader_control 的初始状态就是空
    gk3__run dd if=/dev/zero of="$p_misc" bs=1M count=4 status=none || return 1
    [ "$rescue" = yes ] && { gk3__run mkfs.ext4 -q -F -L gk3rescue "$p_resc" || return 1; }

    # ── 写镜像 ──────────────────────────────────────────────────────────
    gk3_prog 30 "展开并写入 super.img（约 12 GiB）"
    [ -f "$rel/super.img" ] || { gk3_die "发布目录里没有 super.img"; return 1; }
    # ⚠️ super.img 是 Android sparse 格式，直接 dd 会得到一个"校验和对得上
    #    但没有 LP 元数据"的分区（stage2-findings 第 1 节踩过）。
    if [ "$DRY" = 1 ]; then echo "DRY: simg2img $rel/super.img $p_super"
    else
        if head -c4 "$rel/super.img" | od -An -tx1 | tr -d ' \n' | grep -qi '3aff26ed'; then
            simg2img "$rel/super.img" "$p_super" || { gk3_die "simg2img 失败"; return 1; }
        else
            echo "super.img 不是 sparse 格式，直接写"
            dd if="$rel/super.img" of="$p_super" bs=4M status=none || return 1
        fi
    fi

    gk3_prog 70 "写入 boot_a / boot_b"
    [ -f "$rel/boot.img" ] || { gk3_die "发布目录里没有 boot.img"; return 1; }
    gk3__run dd if="$rel/boot.img" of="$p_boota" bs=4M status=none || return 1
    gk3__run dd if="$rel/boot.img" of="$p_bootb" bs=4M status=none || return 1

    # ── 引导链 ──────────────────────────────────────────────────────────
    # ⚠️ 少了这一步，前面所有东西都写对了，机器照样起不来 —— 这台机器是 UEFI，
    #    内核/dtb/ramdisk 是 ESP 上的【普通文件】，不在 boot 分区里被引导。
    #    （boot_a/boot_b 有内容是为了让 update_engine 的 A/B 流程完整。）
    gk3_prog 80 "安装引导链"
    local mid; mid=${GK3_MACHINE_ID:-$(cat /etc/machine-id 2>/dev/null || echo 8a29534fa802480d9fbb71aa18c01d7b)}
    local mnt; mnt=$(mktemp -d)
    gk3__run mount -t vfat "$p_esp" "$mnt" || return 1

    local sdboot=${GK3_SDBOOT:-/usr/share/gaokun3/systemd-bootaa64.efi}
    [ -f "$sdboot" ] || sdboot="$rel/systemd-bootaa64.efi"
    [ -f "$sdboot" ] || { umount "$mnt"; gk3_die "找不到 systemd-bootaa64.efi"; return 1; }

    # 内核/dtb/ramdisk 用【松散文件】，不从 boot.img 里拆 ——
    # 救援系统里没有 Android 的 unpack_bootimg，而我们的发布本来就带这三个文件。
    local f
    for f in Image gaokun3.dtb ramdisk.img; do
        [ -f "$rel/$f" ] || { umount "$mnt"; gk3_die "发布目录里缺 $f"; return 1; }
    done

    if [ "${GK3_DRYRUN:-0}" != 1 ]; then
        mkdir -p "$mnt/EFI/BOOT" "$mnt/EFI/systemd" "$mnt/loader/entries"                  "$mnt/$mid/android/slot_a" "$mnt/$mid/android/slot_b" "$mnt/$mid/rescue"
        cp "$sdboot" "$mnt/EFI/BOOT/BOOTAA64.EFI"
        cp "$sdboot" "$mnt/EFI/systemd/systemd-bootaa64.efi"
        local slot
        for slot in a b; do
            cp "$rel/Image" "$rel/gaokun3.dtb" "$rel/ramdisk.img" "$mnt/$mid/android/slot_$slot/"
            cat > "$mnt/loader/entries/$mid-android-$slot.conf" <<ENTRY
title      Android (slot_$slot)
version    android-$slot
sort-key   android$slot
linux      /$mid/android/slot_$slot/Image
devicetree /$mid/android/slot_$slot/gaokun3.dtb
initrd     /$mid/android/slot_$slot/ramdisk.img
options    androidboot.slot_suffix=_$slot androidboot.hardware=gaokun3 androidboot.selinux=permissive androidboot.veritymode=disabled androidboot.verifiedbootstate=orange androidboot.flash.locked=0 clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 efi=noruntime deferred_probe_timeout=10 firmware_class.path=/vendor/firmware/ fbcon=rotate:1 loglevel=4
ENTRY
        done
        # ★ 默认落点是 slot_a；救援系统装了的话它排在前面（sort-key linux1），
        #   但【不设成 default】—— 默认必须是能用的系统。
        cat > "$mnt/loader/loader.conf" <<LOADER
timeout 15
console-mode keep
editor no
default *-android-a.conf
LOADER
        if [ "$rescue" = yes ]; then
            [ -f "$rel/initramfs.img" ] && cp "$rel/initramfs.img" "$mnt/$mid/rescue/initramfs.img"
            cat > "$mnt/loader/entries/$mid-rescue-alpine.conf" <<RESC
title      救援系统（Alpine，全内存）
version    alpine-rescue
sort-key   linux1
linux      /$mid/android/slot_a/Image
devicetree /$mid/android/slot_a/gaokun3.dtb
initrd     /$mid/rescue/initramfs.img
options    clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 efi=noruntime fbcon=rotate:1 loglevel=4 panic=10 gk3.squash=/gaokun3/rescue.squashfs
RESC
        fi
        sync
    else
        echo "DRY: 往 $p_esp 写 systemd-boot、两个 Android 启动项、内核/dtb/ramdisk"
    fi
    gk3__run umount "$mnt" || true
    rmdir "$mnt" 2>/dev/null || true

    # ── 救援系统 ────────────────────────────────────────────────────────
    if [ "$rescue" = yes ] && [ -f "$rel/rescue.squashfs" ]; then
        gk3_prog 92 "写入救援系统"
        local rmnt; rmnt=$(mktemp -d)
        gk3__run mount "$p_resc" "$rmnt" || return 1
        if [ "${GK3_DRYRUN:-0}" != 1 ]; then
            mkdir -p "$rmnt/gaokun3"
            cp "$rel/rescue.squashfs" "$rmnt/gaokun3/rescue.squashfs"
            # ⚠️ WiFi 凭据【不打包进镜像】：安装器把用户当前用的那份复制过去，
            #    这样救援系统一开机就能连上同一个网。见 gk3-wifi 的注释。
            [ -f "$rel/wpa_supplicant.conf" ] && {
                install -Dm600 "$rel/wpa_supplicant.conf" "$rmnt/gaokun3/wpa_supplicant.conf"; }
            sync
        fi
        gk3__run umount "$rmnt" || true
        rmdir "$rmnt" 2>/dev/null || true
    fi

    gk3_prog 100 "完成"
    return 0
}

# 解析分区节点，解析不出来就直接失败。
# dry-run 时分区还不存在，回一个明显是占位的名字，好让打印出来的命令可读。
gk3__need_part() {
    local disk=$1 want=$2 mode=$3 path
    if [ "${GK3_DRYRUN:-0}" = 1 ]; then echo "<${want}分区>"; return 0; fi
    path=$(gk3__bylabel "$disk" "$want")
    if [ -z "$path" ]; then
        gk3_die "分区 $want 没解析出来（$disk 上找不到这个 PARTLABEL）"; return 1
    fi
    # ⚠️ 分区节点的出现是异步的（udev / devtmpfs），刚写完分区表时它可能还没到。
    #    第一版在这里直接判死，结果 loop 设备实测必然失败 —— 而 lsblk 明明
    #    看得见分区。**"还没出现"和"不存在"是两回事**：等它，别判它死。
    local i=0
    while [ ! -b "$path" ] && [ $i -lt 50 ]; do
        [ $i -eq 0 ] && command -v udevadm >/dev/null && udevadm settle --timeout=5 2>/dev/null
        sleep 0.2; i=$((i+1))
    done
    if [ ! -b "$path" ]; then
        gk3_die "$path 等了 10 秒还不是块设备 —— 分区表写下去了但内核没认"; return 1
    fi
    echo "$path"
}

# 从一行 key=value 里取值
gk3__f() {
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

# 按 PARTLABEL 找分区节点。⚠️ 不用 /dev/disk/by-partlabel —— 救援系统里
# 没有 udev 规则时那个目录可能不存在；直接问 sgdisk 才是确定的。
gk3__bylabel() {
    local disk=$1 want=$2 n
    for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        if [ "$(sgdisk -i "$n" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)" = "$want" ]; then
            gk3_partpath "$disk" "$n"; return 0
        fi
    done
    echo ""
}

# ── 缩分区 ──────────────────────────────────────────────────────────────────
#
# ★ 这是安装器里【唯一会去动用户现有数据】的操作，纪律高于别处：
#
#   1. 最小能缩到多少【问文件系统】，不自己猜（ntfsresize --info / resize2fs -P）
#   2. NTFS 必须是干净卸载的。ntfsresize 自己会拒绝带 dirty 位的卷 ——
#      **不要绕过它**。脏 NTFS 缩 = 数据损坏，而用户往往是因为 Windows
#      快速启动/休眠才脏的，他自己都不知道盘是脏的。
#   3. 先演练通过，再真做
#   4. **先缩文件系统，再缩分区**。反了就是把文件系统截断 —— 直接丢数据
#   5. 重建分区时保住 PARTUUID：Windows 的 BCD 按 PARTUUID 找系统盘，
#      换了它 Windows 就起不来（分区还在、数据还在，但引导指向不存在的 UUID）

# 问文件系统"最小能缩到多少 MiB"。问不出来就报 can=no —— 不猜。
gk3_shrink_info() {
    local part=$1 fs cur_mib min_mib can why out b bs blocks
    if [ ! -b "$part" ]; then
        echo "SHRINK part=$part can=no why=not-a-block-device"; return 1
    fi
    fs=$(blkid -o value -s TYPE "$part" 2>/dev/null)
    cur_mib=$(( $(blockdev --getsize64 "$part" 2>/dev/null || echo 0) / 1048576 ))
    can=no; why=""; min_mib=""
    case "$fs" in
        ntfs)
            if ! command -v ntfsresize >/dev/null; then
                why=no-ntfsresize
            else
                out=$(ntfsresize --info --force "$part" 2>&1)
                if [ $? -ne 0 ]; then
                    # ⚠️ 最常见的原因是卷脏（Windows 快速启动/休眠）。
                    #    这不是该绕过的错误，是该转达给用户的错误。
                    case "$out" in
                        *dirty*|*Dirty*|*unclean*)  why=ntfs-dirty ;;
                        *"resize support"*)         why=ntfs-unsupported ;;
                        *)                          why=ntfsresize-failed ;;
                    esac
                else
                    b=$(printf '%s' "$out" | grep -o 'You might resize at [0-9]* bytes' | grep -o '[0-9]*')
                    if [ -n "$b" ]; then
                        min_mib=$(( b / 1048576 + 1 )); can=yes
                    else
                        why=cannot-parse-min
                    fi
                fi
            fi ;;
        ext2|ext3|ext4)
            if ! command -v resize2fs >/dev/null; then
                why=no-resize2fs
            else
                bs=$(dumpe2fs -h "$part" 2>/dev/null | awk -F: '/Block size/{gsub(/ /,"",$2); print $2}')
                blocks=$(resize2fs -P "$part" 2>/dev/null | grep -o '[0-9]*$')
                if [ -n "$bs" ] && [ -n "$blocks" ]; then
                    min_mib=$(( blocks * bs / 1048576 + 1 )); can=yes
                else
                    why=cannot-parse-min
                fi
            fi ;;
        "") why=no-filesystem ;;
        *)  why=fs-not-shrinkable ;;
    esac
    echo "SHRINK part=$part fs=${fs:-none} cur_mib=$cur_mib min_mib=${min_mib:-0} can=$can why=$why"
    [ "$can" = yes ]
}

# 真的缩。$2 是目标大小（MiB）。
# ⚠️ 这个函数会改用户的数据，所以它自己把所有前提再验一遍，不依赖调用方。
gk3_shrink() {
    local part=$1 target_mib=$2
    local info fs cur min floor disk num pu pl pt start end bk newpu newmib rc
    info=$(gk3_shrink_info "$part") || { echo "$info"; return 1; }
    fs=$(gk3__f "$info" fs); cur=$(gk3__f "$info" cur_mib); min=$(gk3__f "$info" min_mib)

    # 余量：至少给文件系统留 512 MiB。缩到贴着最小值，用户开机就没地方
    # 写页面文件了 —— 那是"能装上但不能用"。
    floor=$(( min + 512 ))
    if [ "$target_mib" -lt "$floor" ]; then
        gk3_die "目标 ${target_mib} MiB 太小：最小 ${min} + 512 余量 = ${floor} MiB"; return 1
    fi
    if [ "$target_mib" -ge "$cur" ]; then
        gk3_die "目标 ${target_mib} MiB 不小于当前 ${cur} MiB，没必要缩"; return 1
    fi

    disk=/dev/$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
    num=$(cat "/sys/class/block/$(basename "$part")/partition" 2>/dev/null)
    if [ ! -b "$disk" ] || [ -z "$num" ]; then
        gk3_die "认不出 $part 属于哪块盘的第几个分区"; return 1
    fi

    # ★ 保住身份：PARTUUID（Windows BCD 靠它）、PARTLABEL、类型 GUID
    pu=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep '^Partition unique GUID:' | awk '{print $4}')
    pl=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep '^Partition name:' | cut -d"'" -f2)
    pt=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep '^Partition GUID code:' | awk '{print $4}')
    if [ -z "$pu" ] || [ -z "$pt" ]; then
        gk3_die "读不到分区 $num 的 GUID —— 不敢重建它"; return 1
    fi
    echo "分区 $num 身份：PARTUUID=$pu 类型=$pt 名字=${pl:-(无)}"

    gk3_prog 5 "备份分区表"
    mount -o remount,rw /media/gk3 2>/dev/null || true
    bk=/media/gk3/gaokun3/gpt-before-shrink-$(date +%Y%m%d-%H%M%S).bin
    [ -d /media/gk3/gaokun3 ] || bk=/tmp/gpt-before-shrink.bin
    if sgdisk --backup="$bk" "$disk" >/dev/null 2>&1; then
        echo "分区表备份：${bk}（还原：sgdisk --load-backup=$bk ${disk}）"
    else
        echo "警告：分区表备份失败"
    fi

    # ── 第 1 步：缩文件系统（演练 → 真做）───────────────────────────────
    gk3_prog 15 "演练缩小文件系统"
    case "$fs" in
        ntfs)
            if ! ntfsresize --no-action --force --size "${target_mib}M" "$part" >/dev/null 2>&1; then
                gk3_die "ntfsresize 演练没通过 —— 不往下做"; return 1
            fi
            gk3_prog 30 "缩小 NTFS"
            # 两个 --force 是 ntfsresize 自己的要求（第二次是确认），不是硬来
            if ! printf 'y\n' | ntfsresize --force --force --size "${target_mib}M" "$part" >/dev/null 2>&1; then
                gk3_die "缩小 NTFS 失败 —— 分区表还没动过，数据应当完好"; return 1
            fi ;;
        ext2|ext3|ext4)
            gk3_prog 20 "检查文件系统（resize2fs 要求）"
            e2fsck -fp "$part" >/dev/null 2>&1; rc=$?
            # e2fsck 返回 1/2 表示"修好了"；>=4 才是真出事
            if [ "$rc" -ge 4 ]; then
                gk3_die "e2fsck 报错（${rc}），不敢缩"; return 1
            fi
            gk3_prog 30 "缩小 ext 文件系统"
            if ! resize2fs "$part" "${target_mib}M" >/dev/null 2>&1; then
                gk3_die "resize2fs 失败 —— 分区表还没动过"; return 1
            fi ;;
        *) gk3_die "不支持缩 $fs"; return 1 ;;
    esac

    # ── 第 2 步：缩分区（文件系统已经小了，这一步才安全）────────────────
    gk3_prog 70 "改分区表"
    start=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep '^First sector:' | awk '{print $3}')
    if [ -z "$start" ]; then
        gk3_die "读不到分区 $num 的起始扇区"; return 1
    fi
    end=$(( start + target_mib * 2048 - 1 ))
    if ! sgdisk -d "$num" "$disk" >/dev/null 2>&1; then
        gk3_die "删旧分区项失败"; return 1
    fi
    if ! sgdisk -n "${num}:${start}:${end}" -t "${num}:${pt}" -u "${num}:${pu}" "$disk" >/dev/null 2>&1; then
        gk3_die "重建分区项失败 —— 分区表备份在 $bk"; return 1
    fi
    [ -n "$pl" ] && sgdisk -c "${num}:${pl}" "$disk" >/dev/null 2>&1
    partprobe "$disk" 2>/dev/null || true
    sleep 1

    # ── 第 3 步：验 ─────────────────────────────────────────────────────
    gk3_prog 90 "复核"
    newpu=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep '^Partition unique GUID:' | awk '{print $4}')
    if [ "$newpu" != "$pu" ]; then
        gk3_die "PARTUUID 变了（$pu -> ${newpu}）—— Windows 会起不来"; return 1
    fi
    newmib=$(( $(blockdev --getsize64 "$part" 2>/dev/null || echo 0) / 1048576 ))
    echo "分区 ${num}：${cur} MiB -> ${newmib} MiB（PARTUUID 未变）"
    gk3_prog 100 "缩小完成"
    return 0
}

# ── 网络 ────────────────────────────────────────────────────────────────────
#
# ⚠️ 这台机器【只有 WiFi】。所以"配网"不是可选步骤 —— 网络安装、下载变体、
#    甚至装完之后的远程救援，全都压在这一条链上。
#
# 走 wpa_supplicant 的控制接口（wpa_cli），不自己解析 iw scan：
# 连接状态、密码错、DHCP 有没有拿到地址，wpa_supplicant 都已经知道了，
# 自己再实现一遍只会实现出一个不一致的版本。

GK3_WIFI_IF=${GK3_WIFI_IF:-wlan0}
GK3_WPA_CTRL=/run/wpa_supplicant

# 确保 wlan0 起来、wpa_supplicant 在跑且带控制接口。
gk3_wifi_up() {
    local ifc=$GK3_WIFI_IF
    if [ ! -e "/sys/class/net/$ifc" ]; then
        gk3_die "没有 $ifc —— ath11k 没起来（看 dmesg | grep ath11k）"; return 1
    fi
    ip link set "$ifc" up 2>/dev/null
    if wpa_cli -i "$ifc" -p "$GK3_WPA_CTRL" status >/dev/null 2>&1; then
        return 0    # 已经在跑
    fi
    # 起一个只带控制接口的实例，网络等下用 wpa_cli 加
    local cfg=/tmp/gk3-wpa.conf
    printf 'ctrl_interface=%s\nupdate_config=1\n' "$GK3_WPA_CTRL" > "$cfg"
    wpa_supplicant -B -i "$ifc" -c "$cfg" >/dev/null 2>&1
    local i=0
    while ! wpa_cli -i "$ifc" -p "$GK3_WPA_CTRL" status >/dev/null 2>&1; do
        i=$((i+1)); [ "$i" -gt 20 ] && { gk3_die "wpa_supplicant 起不来"; return 1; }
        sleep 0.5
    done
}

# 扫描。输出：WIFI ssid=... signal=<dBm> secure=yes|no
# ⚠️ SSID 里可能有空格，所以它放在【行尾】，解析时取 ssid= 之后的全部。
gk3_wifi_scan() {
    gk3_wifi_up || return 1
    local ifc=$GK3_WIFI_IF
    wpa_cli -i "$ifc" -p "$GK3_WPA_CTRL" scan >/dev/null 2>&1
    sleep 3
    wpa_cli -i "$ifc" -p "$GK3_WPA_CTRL" scan_results 2>/dev/null \
    | awk 'NR>1 && NF>=5 {
        sig=$3; flags=$4;
        ssid=""; for(i=5;i<=NF;i++) ssid=ssid (i>5?" ":"") $i;
        if (ssid == "") next;
        sec = (flags ~ /WPA|WEP/) ? "yes" : "no";
        # 同名的只留信号最强的那个（2.4G/5G 双频会出现两条）
        if (!(ssid in best) || sig > best[ssid]) { best[ssid]=sig; secure[ssid]=sec }
      }
      END { for (s in best) printf "WIFI signal=%s secure=%s ssid=%s\n", best[s], secure[s], s }' \
    | sort -t= -k2 -rn
}

# 连接。$1=SSID $2=密码（空 = 开放网络）
gk3_wifi_connect() {
    local ssid=$1 psk=${2:-}
    gk3_wifi_up || return 1
    local ifc=$GK3_WIFI_IF W
    W="wpa_cli -i $ifc -p $GK3_WPA_CTRL"
    local id
    id=$($W add_network 2>/dev/null | tail -1)
    case "$id" in ''|*[!0-9]*) gk3_die "add_network 失败"; return 1 ;; esac
    $W set_network "$id" ssid "\"$ssid\"" >/dev/null 2>&1
    if [ -n "$psk" ]; then
        $W set_network "$id" psk "\"$psk\"" >/dev/null 2>&1
    else
        $W set_network "$id" key_mgmt NONE >/dev/null 2>&1
    fi
    $W enable_network "$id" >/dev/null 2>&1
    $W select_network "$id" >/dev/null 2>&1

    gk3_prog 20 "正在连接 $ssid"
    local i=0 st
    while [ "$i" -lt 40 ]; do
        st=$($W status 2>/dev/null | sed -n 's/^wpa_state=//p')
        case "$st" in
            COMPLETED) break ;;
            # ⚠️ 密码错的表现是反复回到 SCANNING/DISCONNECTED，不会有明确报错。
            #    所以只能靠超时判断 —— 这一点要在界面上说清楚。
            *) : ;;
        esac
        i=$((i+1)); sleep 0.5
    done
    [ "$st" = COMPLETED ] || { gk3_die "连不上 ${ssid}（密码错？信号弱？）"; return 1; }

    gk3_prog 60 "取 IP 地址"
    dhcpcd -n "$ifc" >/dev/null 2>&1 || dhcpcd "$ifc" >/dev/null 2>&1
    i=0
    while [ "$i" -lt 30 ]; do
        ip -4 addr show "$ifc" 2>/dev/null | grep -q 'inet ' && break
        i=$((i+1)); sleep 0.5
    done
    ip -4 addr show "$ifc" 2>/dev/null | grep -q 'inet ' \
        || { gk3_die "连上了但没拿到 IP（DHCP 没响应？）"; return 1; }
    gk3_prog 100 "已连接"
    gk3_net_status
}

gk3_net_status() {
    local ifc=$GK3_WIFI_IF ip4 ssid
    ip4=$(ip -4 addr show "$ifc" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
    ssid=$(wpa_cli -i "$ifc" -p "$GK3_WPA_CTRL" status 2>/dev/null | sed -n 's/^ssid=//p')
    echo "NET if=$ifc ip=${ip4:-none} ssid=${ssid:-none} online=$([ -n "$ip4" ] && echo yes || echo no)"
}

# ── 网络安装 ────────────────────────────────────────────────────────────────
#
# ⚠️ 变体是【构建期】决定的：KSU 要编进内核、GApps 要进 super。
#    安装器只是"挑一个已经构建好的镜像下载"，不是在设备上组装。
#    清单托管在 R2（和 OTA 用同一套布局）。
#
# 清单格式（一行一个变体，key=value）：
#   VARIANT id=stock name=标准版 desc=... url=... size_mib=... sha256=...

GK3_MANIFEST_URL=${GK3_MANIFEST_URL:-https://ota.072172.xyz/installer/variants.txt}

gk3_net_manifest() {
    local url=${1:-$GK3_MANIFEST_URL}
    command -v curl >/dev/null || { gk3_die "没有 curl"; return 1; }
    local out
    out=$(curl -fsSL --max-time 30 "$url" 2>/dev/null) || { gk3_die "取不到清单：$url"; return 1; }
    printf '%s\n' "$out" | grep '^VARIANT ' || { gk3_die "清单里一个 VARIANT 都没有"; return 1; }
}

# 下载并校验。$1=url $2=目标文件 $3=期望 sha256（可空）
gk3_net_fetch() {
    local url=$1 dst=$2 want=${3:-}
    gk3_prog 0 "开始下载"
    # ⚠️ 用 --continue-at 支持断点续传：这台机器的 WAN 只有 1–2 MB/s，
    #    1.2 GB 要十几分钟，中途断一次全部重来是不可接受的。
    curl -fL --retry 3 --retry-delay 2 --continue-at - -o "$dst" "$url" 2>&1 \
        | tr '\r' '\n' | awk '/^ *[0-9]/{ if ($1+0 > 0) printf "PROGRESS %d 下载中 %s\n", $1, $1"%" }' >&2
    [ -f "$dst" ] || { gk3_die "下载失败"; return 1; }
    if [ -n "$want" ]; then
        gk3_prog 95 "校验 sha256"
        local got; got=$(sha256sum "$dst" | cut -d' ' -f1)
        [ "$got" = "$want" ] || { gk3_die "sha256 不符：$got != $want"; return 1; }
        echo "sha256 校验通过"
    fi
    gk3_prog 100 "下载完成"
}

# 一次问完整块盘上所有分区能不能缩。
# ⚠️ 界面那边原来是逐个分区调 gk3_shrink_info，每次都要 fork 一个 shell 并
#    source 整个库 —— 8 个分区就是 8 次。合成一个调用。
gk3_shrink_scan() {
    local disk=$1 n part
    [ -b "$disk" ] || { gk3_die "不是块设备：$disk"; return 1; }
    for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        part=$(gk3_partpath "$disk" "$n")
        [ -b "$part" ] || continue
        gk3_shrink_info "$part" || true
    done
}
