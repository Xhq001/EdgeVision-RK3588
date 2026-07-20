#include "npu_detector_pool.h"

#include <rockchip/mpp_frame.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int NpuDetectorPool::Init(const std::string& model_path, const std::string& label_path,
                          int instances)
{
    Deinit();
    if (instances < 1)
    {
        instances = 1;
    }

    // 实例 0：正常 rknn_init 加载模型 + 标签；单实例时用 AUTO，多实例时绑定 CORE_0。
    const int core0 = (instances <= 1) ? -1 : 0;
    auto      first = std::make_unique<YoloNpuInstance>();
    if (first->Init(model_path, label_path, core0, nullptr) != 0)
    {
        std::cerr << "[NPU_POOL] instance 0 init failed" << std::endl;
        return -1;
    }
    models_.push_back(std::move(first));

    // 实例 1..N-1：rknn_dup_context 共享权重，绑定 CORE_(i%3)。
    for (int i = 1; i < instances; ++i)
    {
        auto inst = std::make_unique<YoloNpuInstance>();
        if (inst->Init(model_path, std::string(), i % 3, models_[0]->MutableContext()) != 0)
        {
            std::cerr << "[NPU_POOL] instance " << i << " init failed" << std::endl;
            Deinit();
            return -1;
        }
        models_.push_back(std::move(inst));
    }

    // 快照环：深度 = 实例数 * 2（至少 4），保证提交与消费之间有缓冲。
    const int ring_depth = std::max(4, instances * 2);
    snapshots_.reserve(static_cast<size_t>(ring_depth));
    for (int i = 0; i < ring_depth; ++i)
    {
        snapshots_.push_back(std::make_unique<Snapshot>());
    }

    stop_ = false;
    workers_.reserve(models_.size());
    for (size_t k = 0; k < models_.size(); ++k)
    {
        workers_.emplace_back(&NpuDetectorPool::WorkerLoop, this, static_cast<int>(k));
    }

    ready_ = true;
    std::cout << "[NPU_POOL] ready: instances=" << models_.size() << ", ring=" << ring_depth
              << std::endl;
    return 0;
}

void NpuDetectorPool::Deinit()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    queue_cv_.notify_all();
    for (auto& w : workers_)
    {
        if (w.joinable())
        {
            w.join();
        }
    }
    workers_.clear();

    // 先销毁 dup 出来的实例（1..N-1），最后销毁持有权重的实例 0，避免悬挂共享上下文。
    for (size_t i = models_.size(); i-- > 0;)
    {
        models_[i].reset();
    }
    models_.clear();
    snapshots_.clear();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<int> empty;
        std::swap(task_queue_, empty);
        stop_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_boxes_.clear();
        latest_frame_id_ = 0;
    }
    infer_ok_.store(0);
    infer_fail_.store(0);
    submit_dropped_.store(0);
    inflight_.store(0);
    ready_ = false;
}

int NpuDetectorPool::AcquireSnapshot()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    for (size_t i = 0; i < snapshots_.size(); ++i)
    {
        if (!snapshots_[i]->in_use)
        {
            snapshots_[i]->in_use = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void NpuDetectorPool::ReleaseSnapshot(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= snapshots_.size())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshots_[static_cast<size_t>(index)]->in_use = false;
}

bool NpuDetectorPool::Submit(const IO_FD_t* nv12_frame, uint64_t frame_id)
{
    if (!ready_ || !nv12_frame)
    {
        return false;
    }
    // 需要可 CPU 访问的源（RGA 输出带 mmap base），才能拷入独立快照。
    if (nv12_frame->base == nullptr || nv12_frame->size == 0)
    {
        return false;
    }

    const int idx = AcquireSnapshot();
    if (idx < 0)
    {
        submit_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;  // 推理跟不上，跳过本帧
    }

    Snapshot& snap = *snapshots_[static_cast<size_t>(idx)];
    if (snap.data.size() < nv12_frame->size)
    {
        snap.data.resize(nv12_frame->size);
    }
    std::memcpy(snap.data.data(), nv12_frame->base, nv12_frame->size);

    snap.io            = *nv12_frame;
    snap.io.base       = snap.data.data();
    snap.io.fd         = -1;  // 走 RGA 虚拟地址前处理，避免占用原始 dma-buf fd
    snap.io.size       = nv12_frame->size;
    snap.frame_id      = frame_id;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(idx);
    }
    queue_cv_.notify_one();
    return true;
}

void NpuDetectorPool::WorkerLoop(int worker_index)
{
    YoloNpuInstance*       model = models_[static_cast<size_t>(worker_index)].get();
    std::vector<DetectBox> boxes;
    boxes.reserve(64);

    while (true)
    {
        int idx = -1;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&]() { return stop_ || !task_queue_.empty(); });
            if (stop_ && task_queue_.empty())
            {
                break;
            }
            idx = task_queue_.front();
            task_queue_.pop();
        }
        if (idx < 0)
        {
            continue;
        }

        Snapshot& snap = *snapshots_[static_cast<size_t>(idx)];
        inflight_.fetch_add(1, std::memory_order_relaxed);
        boxes.clear();
        const int ret = model->Infer(&snap.io, &boxes);
        const uint64_t fid = snap.frame_id;
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        ReleaseSnapshot(idx);

        if (ret == 0)
        {
            infer_ok_.fetch_add(1, std::memory_order_relaxed);
            // 只有更新的帧才覆盖，避免乱序完成时旧结果盖掉新结果。
            std::lock_guard<std::mutex> lock(latest_mutex_);
            if (fid >= latest_frame_id_)
            {
                latest_frame_id_ = fid;
                latest_boxes_    = boxes;
            }
        }
        else
        {
            infer_fail_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NpuDetectorPool::GetLatest(std::vector<DetectBox>* out) const
{
    if (!out)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(latest_mutex_);
    *out = latest_boxes_;
}

void NpuDetectorPool::ClearLatest()
{
    std::lock_guard<std::mutex> lock(latest_mutex_);
    latest_boxes_.clear();
    latest_frame_id_ = 0;
}

std::string NpuDetectorPool::GetLabelName(int cls_id) const
{
    if (models_.empty())
    {
        return "unknown";
    }
    return models_[0]->GetLabelName(cls_id);
}

double NpuDetectorPool::Benchmark(int iterations)
{
    if (!ready_ || iterations <= 0)
    {
        return 0.0;
    }
    // 合成 1080p NV12 帧（Y 平面填渐变，让前处理/推理做真实工作）。
    const uint32_t       W = 1920, H = 1080;
    std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 3u / 2u, 128u);
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
    {
        buf[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
    }
    IO_FD_t synth{};
    synth.base       = buf.data();
    synth.fd         = -1;
    synth.size       = buf.size();
    synth.format     = MPP_FMT_YUV420SP;
    synth.width      = W;
    synth.height     = H;
    synth.hor_stride = W;
    synth.ver_stride = H;

    const uint64_t start_ok = infer_ok();
    const auto     t0       = std::chrono::steady_clock::now();
    int            submitted = 0;
    while (infer_ok() - start_ok < static_cast<uint64_t>(iterations))
    {
        if (submitted < iterations && Submit(&synth, static_cast<uint64_t>(submitted)))
        {
            ++submitted;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    const auto   t1 = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(t1 - t0).count();
    return (dt > 0.0) ? static_cast<double>(iterations) / dt : 0.0;
}

double NpuDetectorPool::detect_fps() const
{
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const uint64_t ok = infer_ok_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(latest_stat_mutex_);
    if (fps_last_ns_ != 0)
    {
        const double dt = static_cast<double>(now_ns - fps_last_ns_) / 1e9;
        if (dt > 0.05)
        {
            detect_fps_  = static_cast<double>(ok - fps_last_ok_) / dt;
            fps_last_ns_ = now_ns;
            fps_last_ok_ = ok;
        }
    }
    else
    {
        fps_last_ns_ = now_ns;
        fps_last_ok_ = ok;
    }
    return detect_fps_;
}
