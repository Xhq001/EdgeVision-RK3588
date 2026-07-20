#!/usr/bin/env bash
# RK3588 性能锁频脚本：把 NPU / CPU / DDR 频率锁到最高档，
# 便于压满 3 个 NPU 核并稳定帧率。需要 root 运行（sudo）。
#
#   sudo scripts/perf_setup.sh          # 锁到 performance
#   sudo scripts/perf_setup.sh restore  # 恢复到 ondemand/默认
set -u

MODE="${1:-performance}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "[perf] 需要 root 权限，请用: sudo $0 ${MODE}" >&2
  exit 1
fi

write() { # write <value> <file>
  local val="$1" file="$2"
  [[ -w "$file" ]] || return 0
  echo "$val" >"$file" 2>/dev/null && echo "[perf] $file <- $val"
}

set_governor_all() { # set_governor_all <governor>
  local gov="$1"
  # CPU 各 policy
  for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    [[ -e "$g" ]] && write "$gov" "$g"
  done
  # NPU
  if [[ -e /sys/class/devfreq/fdab0000.npu/governor ]]; then
    write "$gov" /sys/class/devfreq/fdab0000.npu/governor
  fi
  # 其它 devfreq（DDR/GPU 等），尽量锁高
  for d in /sys/class/devfreq/*/governor; do
    [[ -e "$d" ]] || continue
    # DDR 使用 performance，其余保持一致
    write "$gov" "$d"
  done
}

if [[ "$MODE" == "restore" ]]; then
  echo "[perf] 恢复默认调频策略 (CPU=ondemand / NPU=rknpu_ondemand)"
  for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    [[ -e "$g" ]] && write "ondemand" "$g"
  done
  [[ -e /sys/class/devfreq/fdab0000.npu/governor ]] && write "rknpu_ondemand" /sys/class/devfreq/fdab0000.npu/governor
  echo "[perf] done."
  exit 0
fi

echo "[perf] 锁定 performance 模式..."
set_governor_all "performance"

echo ""
echo "[perf] 当前状态:"
for p in /sys/devices/system/cpu/cpufreq/policy*/; do
  [[ -e "$p/scaling_governor" ]] || continue
  echo "  CPU $(basename "$p"): gov=$(cat "$p/scaling_governor" 2>/dev/null) cur=$(cat "$p/scaling_cur_freq" 2>/dev/null)"
done
if [[ -e /sys/class/devfreq/fdab0000.npu ]]; then
  echo "  NPU: gov=$(cat /sys/class/devfreq/fdab0000.npu/governor 2>/dev/null) cur=$(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null)"
fi
echo ""
echo "[perf] NPU 实时负载(三核): cat /sys/kernel/debug/rknpu/load  (需 root)"
echo "[perf] done."
