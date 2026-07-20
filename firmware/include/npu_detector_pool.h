#ifndef NPU_DETECTOR_POOL_H
#define NPU_DETECTOR_POOL_H

#include "pubDataType.h"
#include "yolo_npu.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// 多实例 NPU 推理池：为 RK3588 的 3 个 NPU 核心各建一个 rknn 实例（实例 1..N 用
// rknn_dup_context 共享权重），每个 worker 线程独占一个实例并绑定到一个核心，
// 从而把三核吃满。推理与视频主链路解耦：主链路只需 Submit 一份 NV12 快照，
// 之后异步取 GetLatest 得到最近一次检测框（用于烧录），视频不再等待推理。
//
// 线程安全要点：每个 worker 线程只使用属于自己的 YoloNpuInstance（models_[k]），
// 因此各实例的内部缓冲（model_input_/outputs_ 等）天然无并发访问，
// 规避了"按任务 round-robin 选实例"可能导致的同一实例被两个线程同时使用的数据竞争。
class NpuDetectorPool
{
   public:
    using DetectBox = YoloNpuInstance::DetectBox;

    NpuDetectorPool()  = default;
    ~NpuDetectorPool() { Deinit(); }

    NpuDetectorPool(const NpuDetectorPool&)            = delete;
    NpuDetectorPool& operator=(const NpuDetectorPool&) = delete;

    // instances: NPU 实例/worker 线程数（建议 3，对应三核）。返回 0 成功。
    int  Init(const std::string& model_path, const std::string& label_path, int instances);
    void Deinit();
    bool IsReady() const { return ready_; }

    // 把一帧 NV12（RGA 输出）拷入池内独立拥有的快照 buffer 并异步入队推理。
    // 返回 true 表示已入队；false 表示当前无空闲快照（推理跟不上），调用方直接跳过本帧即可。
    bool Submit(const IO_FD_t* nv12_frame, uint64_t frame_id);

    // 取最近一次完成的检测框（线程安全拷贝）。视频线程每帧调用它来烧录框。
    void GetLatest(std::vector<DetectBox>* out) const;

    // 清空最近检测框（关闭识别或切换摄像头时调用，避免残留旧框）。
    void ClearLatest();

    std::string GetLabelName(int cls_id) const;

    // 压测：用合成帧以最大速率驱动推理池，返回吞吐（fps）。
    // 不受摄像头 30fps 限制，用于对比 1/3 实例的三核扩展性。
    double Benchmark(int iterations);

    int      instances() const { return static_cast<int>(models_.size()); }
    uint64_t infer_ok() const { return infer_ok_.load(std::memory_order_relaxed); }
    uint64_t infer_fail() const { return infer_fail_.load(std::memory_order_relaxed); }
    uint64_t submit_dropped() const { return submit_dropped_.load(std::memory_order_relaxed); }
    int      inflight() const { return inflight_.load(std::memory_order_relaxed); }
    // 最近一次统计窗口测得的检测吞吐（fps）。
    double   detect_fps() const;

   private:
    struct Snapshot
    {
        std::vector<uint8_t> data;      // 独立拥有的 NV12 帧拷贝
        IO_FD_t              io{};      // 指向 data 的描述（fd=-1，走 RGA 虚拟地址前处理）
        uint64_t             frame_id = 0;
        bool                 in_use   = false;
    };

    int  AcquireSnapshot();          // 返回空闲快照索引，无则 -1（持锁）
    void ReleaseSnapshot(int index); // 归还快照（持锁）
    void WorkerLoop(int worker_index);

    std::vector<std::unique_ptr<YoloNpuInstance>> models_;
    std::vector<std::unique_ptr<Snapshot>>        snapshots_;
    std::vector<std::thread>                      workers_;

    // 任务队列：存快照索引。
    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<int>         task_queue_;
    bool                    stop_ = false;

    // 快照池占用状态。
    std::mutex snapshot_mutex_;

    // 最近检测结果。
    mutable std::mutex      latest_mutex_;
    std::vector<DetectBox>  latest_boxes_;
    uint64_t                latest_frame_id_ = 0;

    std::atomic_uint64_t infer_ok_{0};
    std::atomic_uint64_t infer_fail_{0};
    std::atomic_uint64_t submit_dropped_{0};
    std::atomic_int      inflight_{0};

    // 检测吞吐统计（供 /api/status）。
    mutable std::mutex latest_stat_mutex_;
    mutable double     detect_fps_       = 0.0;
    mutable uint64_t   fps_last_ok_      = 0;
    mutable int64_t    fps_last_ns_      = 0;

    bool ready_ = false;
};

#endif  // NPU_DETECTOR_POOL_H
