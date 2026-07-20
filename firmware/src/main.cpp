#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_controller.h"
#include "common_utils.h"
#include "npu_detector_pool.h"
#include "nv12_overlay.h"
#include "rkmppdec.h"
#include "rkmppenc.h"
#include "rkrga.h"
#include "v4l2Camera.h"
#include "yolo_npu.h"
#include "zlm_publisher.h"

static const std::string     kDefaultDevice = "/dev/video11";  // 默认采集设备（可运行时切换）
static volatile sig_atomic_t should_exit    = 0;               // 用于控制程序退出的标志

static void signal_handler(int signum)
{
    if (signum == SIGINT)
    {
        should_exit = 1;
    }
}



// 运行一整条采集→解码→RGA→编码→推流流水线，直到收到退出或"重启（换摄像头）"信号。
// ZLM 推流器与 NPU 推理池由调用方持有并跨重启保活；本函数仅负责按设备重建可变部分。
static int run_pipeline(const std::string& device, ZlmPublisher& zlm_publisher,
                        NpuDetectorPool& yolo_pool, unsigned detect_stride)
{
    AppController& controller = AppController::Instance();

    std::cout << "[CFG] Fixed pipeline: CAMERA_IO_MODE=dmabuf, MPP_DEC_INPUT_MODE=zerocopy"
              << std::endl;
    std::cout << "[CFG] capture device = " << device << std::endl;

    V4L2_Camera camera;
    if (camera.camera_GlobalInit(device, 0) != 0)  // 全局初始化，设置曝光时间为 20ms
    {
        std::cerr << "Failed to initialize camera globally" << std::endl;
        return 1;
    }
    const VideoCaptureConfig& active_cfg = camera.get_active_config();
    if (!active_cfg.valid)
    {
        std::cerr << "Camera active config is invalid after initialization" << std::endl;
        return 1;
    }

    const bool use_mjpeg_decode = (active_cfg.pixelformat == V4L2_PIX_FMT_MJPEG);
    uint32_t   raw_input_mpp_fmt = 0;
    // 非 MJPEG 路径下，相机输出会直接送入 RGA/编码链路。
    // 这里必须把 V4L2 fourcc 映射成 MPP 帧格式，否则后续模块会把 buffer 按错误布局解释，
    // 典型后果是色彩错乱、步幅错误甚至直接崩溃。
    if (!use_mjpeg_decode)
    {
        if (active_cfg.pixelformat == V4L2_PIX_FMT_YUYV)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV422_YUYV;
        }
        else if (active_cfg.pixelformat == V4L2_PIX_FMT_UYVY)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV422_UYVY;
        }
        else if (active_cfg.pixelformat == V4L2_PIX_FMT_NV12)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV420SP;
        }
        else if (active_cfg.pixelformat == V4L2_PIX_FMT_NV21)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV420SP_VU;
        }
        else if (active_cfg.pixelformat == V4L2_PIX_FMT_NV16)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV422SP;
        }
        else if (active_cfg.pixelformat == V4L2_PIX_FMT_NV61)
        {
            raw_input_mpp_fmt = MPP_FMT_YUV422SP_VU;
        }
        else
        {
            std::cerr << "Unsupported camera pixel format for current pipeline: "
                      << util::FourccToString(active_cfg.pixelformat) << std::endl;
            return 1;
        }
    }

    std::vector<IO_FD_t> raw_camera_input_descs;
    if (!use_mjpeg_decode)
    {
        const size_t camera_buf_count = static_cast<size_t>(camera.get_buffer_count());
        if (camera_buf_count == 0)
        {
            std::cerr << "Invalid camera buffer count for raw path: 0" << std::endl;
            return 1;
        }

        raw_camera_input_descs.resize(camera_buf_count);
        const bool     is_packed_422 =
            (active_cfg.pixelformat == V4L2_PIX_FMT_YUYV || active_cfg.pixelformat == V4L2_PIX_FMT_UYVY);
        // 注意：bytesperline 的单位是“字节”。
        // 对 packed 422（YUYV/UYVY）来说，2 字节对应 1 像素，因此要 /2 转成像素步幅；
        // 对 NV12/NV21/NV16/NV61 等半平面格式，bytesperline 与像素步幅可直接等价使用。
        const uint32_t stride_pixels = (active_cfg.bytesperline > 0)
                                           ? (is_packed_422 ? (active_cfg.bytesperline / 2)
                                                            : active_cfg.bytesperline)
                                           : active_cfg.width;
        for (size_t i = 0; i < camera_buf_count; ++i)
        {
            const FrameDesc& src_desc = camera.FrameDescArray[i];
            IO_FD_t&         io_desc  = raw_camera_input_descs[i];
            io_desc.fd                = src_desc.fd;
            io_desc.base              = src_desc.base;
            io_desc.size              = src_desc.Length;
            io_desc.format            = raw_input_mpp_fmt;
            io_desc.width             = active_cfg.width;
            io_desc.height            = active_cfg.height;
            io_desc.hor_stride        = stride_pixels;
            io_desc.ver_stride        = active_cfg.height;
            io_desc.pts_us            = -1;
            io_desc.dts_us            = -1;
        }
        std::cout << "[CFG] Camera raw path enabled, pixfmt="
                  << util::FourccToString(active_cfg.pixelformat)
                  << ", stride_pixels=" << stride_pixels << std::endl;
    }
    else
    {
        std::cout << "[CFG] Camera MJPEG path enabled, using MPP decoder" << std::endl;
    }

    MppDecInstance mpp_decoder_instance;
    if (use_mjpeg_decode)
    {
        if (mpp_decoder_instance.DecInit() != 0)  // 初始化 MPP 实例
        {
            std::cerr << "Failed to initialize MPP instance" << std::endl;
            return 1;
        }
        if (mpp_decoder_instance.DecConfigWidthHeight(active_cfg.width, active_cfg.height) !=
            0)  // 配置 MPP 的宽高参数
        {
            std::cerr << "Failed to configure MPP width and height" << std::endl;
            return 1;
        }
        if (mpp_decoder_instance.DecAllocBuffer(camera.FrameDescArray,
                                                static_cast<size_t>(camera.get_buffer_count())) !=
            0)
        {
            std::cerr << "Failed to import V4L2 dma-buf into MPP input buffers" << std::endl;
            return 1;
        }
    }

    RgaInstance rga_instance;
    if (rga_instance.RgaInit() != 0)
    {
        std::cerr << "Failed to initialize RGA instance" << std::endl;
        return 1;
    }

    // NPU 推理池由 main() 创建并跨重启保活；这里只需引用它。
    // 换摄像头会重启本函数，先清掉上一路残留的检测框。
    yolo_pool.ClearLatest();

    MppEncInstance mpp_encoder_instance;
    if (mpp_encoder_instance.EncInit() != 0)
    {
        std::cerr << "Failed to initialize MPP encoder instance" << std::endl;
        return 1;
    }
    if (mpp_encoder_instance.EncConfigWidthHeight(active_cfg.width, active_cfg.height, 0, 0) != 0)
    {
        std::cerr << "Failed to configure MPP encoder width and height" << std::endl;
        return 1;
    }

    // ZLM 推流器由 main() 创建并跨重启保活；按当前摄像头帧率更新时间戳兜底 fps。
    uint32_t expected_fps = 30;
    if (active_cfg.fps_num > 0 && active_cfg.fps_den > 0)
    {
        expected_fps = active_cfg.fps_den / active_cfg.fps_num;
        if (expected_fps == 0)
        {
            expected_fps = 30;
        }
    }
    zlm_publisher.SetExpectedFps(expected_fps);
    // 上报当前分辨率给控制器（供 /api/status）。
    controller.set_stream_stats(0.0, static_cast<int>(active_cfg.width),
                                static_cast<int>(active_cfg.height), 0);
    std::cout << "[CFG] Timestamp fallback fps=" << expected_fps << std::endl;

    std::atomic_uint64_t enc_pkt_total{0};
    std::atomic_uint64_t zlm_input_ok{0};
    std::atomic_uint64_t zlm_input_fail{0};

    std::mutex ready_mutex;               // 保护 readyQueue 的互斥锁 相机->解码线程
    std::mutex recycle_mutex;             // 保护 recycleQueue 的互斥锁 解码线程->相机
    std::mutex rga_consume_mutex;         // 保护 rgaConsumeQueue 的互斥锁 解码线程->RGA 线程
    std::mutex mpp_recycle_output_mutex;  // 保护 mppRecycleOutputQueue 的互斥锁 RGA 线程->解码线程
    std::mutex mpp_encode_input_mutex;    // 保护 mppEncodeInputQueue 的互斥锁 RGA 线程->编码线程
    std::condition_variable    ready_cv;  // 用于通知解码线程有新帧可处理的条件变量
    std::condition_variable    rga_consume_cv;         // 用于通知 RGA 线程有新帧可处理的条件变量
    std::condition_variable    mpp_recycle_output_cv;  // 用于通知解码输出可回收线程有新数据
    std::condition_variable    mpp_encode_input_cv;    // 用于通知编码输入线程有新帧可处理
    std::deque<FrameDesc*>     readyQueue;             // 存储已捕获但未解码的帧描述指针的队列
    std::deque<FrameDesc*>     recycleQueue;     /// 存储已处理完毕、等待回收的帧描述指针的队列
    std::deque<const IO_FD_t*> rgaConsumeQueue;  // 存储已解码但未 RGA 处理的帧描述指针的队列
    std::deque<const IO_FD_t*>
        mppRecycleOutputQueue;                       // 存储已 RGA 处理但未回收的输出描述指针的队列
    std::deque<const IO_FD_t*> mppEncodeInputQueue;  // 存储已 RGA 处理但未送编码的帧描述指针的队列

    // 开关
    std::atomic_bool stop_requested{false};
    std::atomic_bool fatal_error{false};
    std::atomic_bool rga_thread_done{false};  // RGA线程退出标志  其退出后回收线程再退出
    std::atomic_bool encode_input_done{false};
    // 统计信息
    std::atomic<size_t> dropped_frames{0};
    std::atomic<size_t> dropped_rga_queue_frames{0};
    constexpr size_t kMaxReadyQueueDepth =
        resource_limits::kCameraRequestBufferCount * 0.75;  // 控制端到端延迟，超限时丢弃最旧帧
    constexpr size_t kMaxRgaConsumeQueueDepth =
        resource_limits::kCameraRequestBufferCount / 2;  // RGA 消费队列上限，防止 raw buffer 长时间占用
    constexpr auto kRetryBackoffSleep = std::chrono::milliseconds(2);  // 重试退避，避免短时间忙轮询
    constexpr auto kEncodeNoPacketSleep =
        std::chrono::milliseconds(30);  // 无包时退避，避免编码输出线程忙等抢占 CPU
    constexpr auto kMainLoopSleep = std::chrono::milliseconds(20);  // 主线程轮询退出信号间隔

    auto request_stop = [&]()
    {
        stop_requested.store(true);
        ready_cv.notify_all();
        rga_consume_cv.notify_all();
        mpp_recycle_output_cv.notify_all();
        mpp_encode_input_cv.notify_all();
    };

    auto drain_mpp_recycle_requests = [&]()
    {
        std::deque<const IO_FD_t*> pending_outputs;
        {
            std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);
            while (!mppRecycleOutputQueue.empty())
            {
                pending_outputs.push_back(mppRecycleOutputQueue.front());
                mppRecycleOutputQueue.pop_front();
            }
        }

        if (!use_mjpeg_decode)
        {
            return;
        }

        for (const IO_FD_t* output_desc : pending_outputs)
        {
            if (mpp_decoder_instance.DecQueueOutputForRecycle(output_desc) != 0)
            {
                std::cerr << "Failed to queue decoded output for recycle, fd="
                          << ((output_desc) ? output_desc->fd : -1) << std::endl;
            }
        }
    };

    auto run_capture_thread = [&]()
    {
        if (camera.get_buffer_count() <= 0)
        {
            std::cerr << "Invalid camera buffer count: 0" << std::endl;
            fatal_error.store(true);
            request_stop();
            ready_cv.notify_all();
            return;
        }

        while (!stop_requested.load())
        {
            std::deque<FrameDesc*> recycled_batch;
            FrameDesc*             dropped_desc = nullptr;
            {
                // 这里扩起来是为了减少持锁时间，先取出要回收的缓冲区指针，等到锁外再执行回收操作，避免在可能的
                // I/O 操作中持锁。
                std::lock_guard<std::mutex> lock(recycle_mutex);
                while (!recycleQueue.empty())
                {
                    recycled_batch.push_back(recycleQueue.front());
                    recycleQueue.pop_front();
                }
            }

            for (FrameDesc* recycled_desc : recycled_batch)
            {
                if (recycled_desc && camera.requeue_buffer(recycled_desc) != 0)
                {
                    std::cerr << "Failed to requeue recycled buffer in capture thread" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
            }
            if (stop_requested.load())
            {
                break;
            }

            size_t frame_length = camera_params::kMaxFrameSize;
            int    frame_capture_ret =
                camera.camera_read_frame(&frame_length, camera_params::kMaxFrameSize);
            if (frame_capture_ret == camera_read_result::kRetryable)
            {
                std::this_thread::sleep_for(kRetryBackoffSleep);
                continue;
            }
            if (frame_capture_ret == camera_read_result::kTimeout)
            {
                std::cerr << "Camera dequeue timeout reached (" << camera_params::kDequeueTimeoutMs
                          << " ms), stop capture thread" << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }
            if (frame_capture_ret != camera_read_result::kOk)
            {
                std::cerr << "Failed to read a frame from the camera" << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }

            FrameDesc* frame_desc = camera.CurrentFrameDesc;
            if (frame_desc == nullptr || frame_desc->index < 0)
            {
                std::cerr << "Invalid current frame descriptor after capture" << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }

            {
                // 将新捕获的帧描述加入 readyQueue，准备被解码线程处理。
                // 扩起来是为了减小持锁作用域，先在锁内检查并更新 readyQueue
                // 的状态，决定是否需要丢弃最旧的帧，然后在锁外执行具体的入队和丢弃操作。
                std::lock_guard<std::mutex> lock(ready_mutex);
                if (readyQueue.size() >= kMaxReadyQueueDepth)
                {
                    dropped_desc = readyQueue.front();
                    readyQueue.pop_front();
                }
                readyQueue.push_back(frame_desc);
            }

            if (dropped_desc)
            {
                {
                    // 满队列时丢弃最旧帧，但不在这里立即回队；
                    // 统一交给采集线程下一轮的 recycleQueue 批量回收流程处理。
                    std::lock_guard<std::mutex> lock(recycle_mutex);
                    recycleQueue.push_back(dropped_desc);
                }
                ++dropped_frames;
            }
            ready_cv.notify_one();
        }

        ready_cv.notify_all();
    };
    std::thread capture_thread(run_capture_thread);

    auto run_decode_thread = [&]()
    {
        while (true)
        {
            FrameDesc* frame_desc = nullptr;
            {
                std::unique_lock<std::mutex> lock(ready_mutex);
                ready_cv.wait(lock, [&]() { return !readyQueue.empty() || stop_requested.load(); });
                if (readyQueue.empty())
                {
                    if (stop_requested.load())
                    {
                        break;
                    }
                    continue;
                }
                frame_desc = readyQueue.front();
                readyQueue.pop_front();
            }

            if (frame_desc == nullptr)
            {
                std::cerr << "Ready queue returned null frame descriptor" << std::endl;
                fatal_error.store(true);
                request_stop();
                continue;
            }

            if (use_mjpeg_decode)
            {
                if (mpp_decoder_instance.MppDecode(frame_desc) != 0)
                {
                    // 解码失败直接跳过该帧，随后由 recycleQueue 回收到相机，保证采集链路继续前进。
                    std::cerr << "Failed to decode frame, index=" << frame_desc->index
                              << ", payload=" << frame_desc->payloadSize << std::endl;
                }
                else
                {
                    const IO_FD_t* decoded_output = mpp_decoder_instance.CurrentOutputDesc;
                    if (!decoded_output || decoded_output->width == 0 ||
                        decoded_output->height == 0 || decoded_output->hor_stride == 0 ||
                        decoded_output->ver_stride == 0)
                    {
                        std::cerr << "Decoded output metadata is invalid" << std::endl;
                        if (decoded_output &&
                            mpp_decoder_instance.DecQueueOutputForRecycle(decoded_output) != 0)
                        {
                            std::cerr << "Failed to recycle invalid decoded output, fd="
                                      << decoded_output->fd << std::endl;
                        }
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(rga_consume_mutex);
                            rgaConsumeQueue.push_back(decoded_output);
                        }
                        rga_consume_cv.notify_one();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(recycle_mutex);
                    recycleQueue.push_back(frame_desc);
                }
            }
            else
            {
                if (frame_desc->index < 0 ||
                    static_cast<size_t>(frame_desc->index) >= raw_camera_input_descs.size())
                {
                    std::cerr << "Invalid raw camera frame index for RGA path: "
                              << frame_desc->index << std::endl;
                    {
                        std::lock_guard<std::mutex> lock(recycle_mutex);
                        recycleQueue.push_back(frame_desc);
                    }
                    if (!stop_requested.load())
                    {
                        fatal_error.store(true);
                        request_stop();
                    }
                }
                else
                {
                    IO_FD_t* input_desc = &raw_camera_input_descs[static_cast<size_t>(frame_desc->index)];
                    input_desc->pts_us  = frame_desc->pts_us;
                    input_desc->dts_us  = frame_desc->dts_us;

                    bool enqueued_to_rga = false;
                    {
                        std::lock_guard<std::mutex> lock(rga_consume_mutex);
                        if (rgaConsumeQueue.size() < kMaxRgaConsumeQueueDepth)
                        {
                            rgaConsumeQueue.push_back(input_desc);
                            enqueued_to_rga = true;
                        }
                    }
                    if (enqueued_to_rga)
                    {
                        rga_consume_cv.notify_one();
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(recycle_mutex);
                            recycleQueue.push_back(frame_desc);
                        }
                        dropped_rga_queue_frames++;
                        if ((dropped_rga_queue_frames % 120u) == 1u)
                        {
                            std::cerr << "RGA consume queue backlog, drop frame to keep camera alive: "
                                      << dropped_rga_queue_frames << std::endl;
                        }
                    }
                }
            }
        }
    };
    std::thread decode_thread(run_decode_thread);

    auto run_mpp_output_recycle_thread = [&]()
    {
        while (true)
        {
            std::deque<const IO_FD_t*> pending_outputs;
            {
                std::unique_lock<std::mutex> lock(mpp_recycle_output_mutex);
                mpp_recycle_output_cv.wait(
                    lock,
                    [&]()
                    {
                        return !mppRecycleOutputQueue.empty() ||
                               (stop_requested.load() && rga_thread_done.load());
                    });
                while (!mppRecycleOutputQueue.empty())
                {
                    pending_outputs.push_back(mppRecycleOutputQueue.front());
                    mppRecycleOutputQueue.pop_front();
                }
                if (pending_outputs.empty() && stop_requested.load() && rga_thread_done.load())
                {
                    break;
                }
            }

            for (const IO_FD_t* output_desc : pending_outputs)
            {
                if (!use_mjpeg_decode)
                {
                    continue;
                }
                if (mpp_decoder_instance.DecQueueOutputForRecycle(output_desc) != 0)
                {
                    std::cerr << "Failed to queue decoded output for recycle, fd="
                              << ((output_desc) ? output_desc->fd : -1) << std::endl;
                }
            }
        }
    };
    std::thread mpp_output_recycle_thread(run_mpp_output_recycle_thread);

    auto run_rga_thread = [&]()
    {
        constexpr size_t kMaxRgaTransformRetry = 2000;  // 约 4 秒重试窗口
        constexpr size_t kYoloStatInterval     = 60;
        size_t           rga_frame_count       = 0;
        bool             encoder_io_ready      = false;
        std::vector<YoloNpuInstance::DetectBox> yolo_boxes;
        while (true)
        {
            const IO_FD_t* input_desc = nullptr;
            bool           raw_input_recycled = false;
            {
                std::unique_lock<std::mutex> lock(rga_consume_mutex);  // 获取锁
                rga_consume_cv.wait(
                    lock, [&]() { return !rgaConsumeQueue.empty() || stop_requested.load(); });
                if (rgaConsumeQueue.empty())
                {
                    if (stop_requested.load())
                    {
                        break;
                    }
                    continue;
                }
                input_desc = rgaConsumeQueue.front();  // 有数据了，取出队首元素
                rgaConsumeQueue.pop_front();           // 从队列中移除已取出的元素
            }

            if (!input_desc)
            {
                continue;
            }

            const IO_FD_t* rga_output      = nullptr;
            size_t         rga_retry_count = 0;
            while (!stop_requested.load())
            {
                rga_output = rga_instance.Copy(input_desc);
                if (rga_output != nullptr)
                {
                    break;
                }

                rga_retry_count++;
                if (rga_retry_count >= kMaxRgaTransformRetry)
                {
                    std::cerr << "RGA processing failed repeatedly, fd=" << input_desc->fd
                              << ", retry_count=" << rga_retry_count << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
                std::this_thread::sleep_for(kRetryBackoffSleep);
            }

            if (rga_output != nullptr)
            {
                if (!use_mjpeg_decode)
                {
                    const IO_FD_t* raw_begin = raw_camera_input_descs.data();
                    const IO_FD_t* raw_end   = raw_begin + raw_camera_input_descs.size();
                    if (raw_begin && input_desc >= raw_begin && input_desc < raw_end)
                    {
                        const size_t     index      = static_cast<size_t>(input_desc - raw_begin);
                        FrameDesc* const frame_desc = &camera.FrameDescArray[index];
                        {
                            std::lock_guard<std::mutex> lock(recycle_mutex);
                            recycleQueue.push_back(frame_desc);
                        }
                        raw_input_recycled = true;
                    }
                    else
                    {
                        std::cerr << "Raw path got unknown input descriptor from RGA, fd="
                                  << input_desc->fd << std::endl;
                        fatal_error.store(true);
                        request_stop();
                    }
                }

                if (!encoder_io_ready)
                {
                    if (mpp_encoder_instance.AllocBufferForIO(
                            rga_instance.output_pool_, resource_limits::kRgaOutputBufferCount) != 0)
                    {
                        std::cerr << "Failed to import RGA output buffers for encoder" << std::endl;
                        (void)rga_instance.QueueOutputToRecycle(rga_output);
                        fatal_error.store(true);
                        request_stop();
                    }
                    else
                    {
                        encoder_io_ready = true;
                    }
                }
                if (encoder_io_ready && !stop_requested.load() && rga_output != nullptr)
                {
                    // 解耦：视频线程不再等待推理。
                    // 1) 每帧烧录"最近一次"NPU 池完成的检测框（最多滞后 1~2 帧，监控足够）。
                    // 2) 按采样节奏把当前帧异步提交给 NPU 池（三核并行推理），提交本身不阻塞。
                    if (controller.yolo_enabled())
                    {
                        yolo_boxes.clear();
                        yolo_pool.GetLatest(&yolo_boxes);
                        if (!yolo_boxes.empty())
                        {
                            draw_yolo_boxes_on_nv12(rga_output, yolo_boxes,
                                                    [&](int c) { return yolo_pool.GetLabelName(c); });
                        }
                        if ((rga_frame_count % detect_stride) == 0u)
                        {
                            (void)yolo_pool.Submit(rga_output, rga_frame_count);
                        }
                        if ((rga_frame_count % kYoloStatInterval) == 0u)
                        {
                            std::cout << "[YOLO] infer_ok=" << yolo_pool.infer_ok()
                                      << ", infer_fail=" << yolo_pool.infer_fail()
                                      << ", detect_fps=" << yolo_pool.detect_fps()
                                      << ", inflight=" << yolo_pool.inflight()
                                      << ", submit_drop=" << yolo_pool.submit_dropped()
                                      << ", det_count=" << yolo_boxes.size() << std::endl;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
                        mppEncodeInputQueue.push_back(rga_output);  // 压入MPP编码输入队列
                    }
                    mpp_encode_input_cv.notify_one();
                }
            }

            if (use_mjpeg_decode)
            {
                {
                    std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);  // 获取锁
                    mppRecycleOutputQueue.push_back(
                        input_desc);  // 将处理完成的输出描述加入MPP 解码器回收队列
                }
                mpp_recycle_output_cv.notify_one();  // 通知解码器回收线程有新输出可回收
            }
            else
            {
                if (!raw_input_recycled)
                {
                    const IO_FD_t* raw_begin = raw_camera_input_descs.data();
                    const IO_FD_t* raw_end   = raw_begin + raw_camera_input_descs.size();
                    if (raw_begin && input_desc >= raw_begin && input_desc < raw_end)
                    {
                        const size_t     index      = static_cast<size_t>(input_desc - raw_begin);
                        FrameDesc* const frame_desc = &camera.FrameDescArray[index];
                        {
                            std::lock_guard<std::mutex> lock(recycle_mutex);
                            recycleQueue.push_back(frame_desc);
                        }
                    }
                    else
                    {
                        std::cerr << "Raw path got unknown input descriptor from RGA, fd="
                                  << input_desc->fd << std::endl;
                        fatal_error.store(true);
                        request_stop();
                    }
                }
            }
            rga_frame_count++;
            if ((rga_frame_count % 300u) == 0u)
            {
                std::cout << "RGA processed frames: " << rga_frame_count
                          << ", last_input_fd=" << input_desc->fd << std::endl;
            }
        }
        rga_thread_done.store(true);
        mpp_recycle_output_cv.notify_all();
    };
    std::thread rga_thread(run_rga_thread);

    auto run_mpp_encode_input_thread = [&]()
    {
        constexpr size_t kMaxEncodePushRetry  = 200;
        size_t           encode_push_failures = 0;
        while (true)
        {
            const IO_FD_t* encode_input_desc = nullptr;
            {
                std::unique_lock<std::mutex> lock(mpp_encode_input_mutex);
                mpp_encode_input_cv.wait(
                    lock, [&]() { return !mppEncodeInputQueue.empty() || stop_requested.load(); });
                if (mppEncodeInputQueue.empty())
                {
                    if (stop_requested.load())
                    {
                        break;
                    }
                    continue;
                }
                encode_input_desc = mppEncodeInputQueue.front();
                mppEncodeInputQueue.pop_front();
            }

            if (!encode_input_desc)
            {
                continue;
            }
            if (stop_requested.load())
            {
                (void)rga_instance.QueueOutputToRecycle(encode_input_desc);
                break;
            }
            // 将 RGA 处理后的输出送入编码器，若失败则重试，重试超过阈值则记录错误并停止程序。
            if (mpp_encoder_instance.EncodePushFrame(encode_input_desc) != 0)
            {
                if (stop_requested.load())
                {
                    (void)rga_instance.QueueOutputToRecycle(encode_input_desc);
                    break;
                }
                encode_push_failures++;
                if (encode_push_failures <= kMaxEncodePushRetry)
                {
                    if ((encode_push_failures % 20u) == 1u)
                    {
                        std::cerr << "Encode push temporary failure, fd=" << encode_input_desc->fd
                                  << ", retry=" << encode_push_failures << std::endl;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
                        mppEncodeInputQueue.push_front(
                            encode_input_desc);  // 回队重试，避免瞬时失败直接中断流程
                    }
                    mpp_encode_input_cv.notify_one();
                    std::this_thread::sleep_for(kRetryBackoffSleep);  // 退避
                    continue;
                }

                std::cerr << "Failed to push frame to encoder after retries, fd="
                          << encode_input_desc->fd << ", retry=" << encode_push_failures
                          << std::endl;
                (void)rga_instance.QueueOutputToRecycle(
                    encode_input_desc);  // 重试后仍失败，回收该输出避免资源泄漏
                fatal_error.store(true);
                request_stop();
                break;
            }

            encode_push_failures = 0;
            // encode_put_frame 为阻塞式，返回时输入帧已被编码器消费，可立即回收。
            if (rga_instance.QueueOutputToRecycle(encode_input_desc) != 0)
            {
                std::cerr << "Failed to recycle RGA output after encode_put_frame, fd="
                          << encode_input_desc->fd << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }
        }
        encode_input_done.store(true);
    };
    std::thread mpp_encode_input_thread(run_mpp_encode_input_thread);

    auto run_mpp_encode_output_thread = [&]()
    {
        auto                 last_stat_tp = std::chrono::steady_clock::now();
        std::vector<uint8_t> pending_au;
        int64_t              pending_dts_us   = -1;
        int64_t              pending_pts_us   = -1;
        bool                 pending_eos      = false;
        bool                 first_ts_logged  = false;
        uint64_t             bad_pkt_ts_count = 0;
        int64_t              last_pkt_dts_us  = -1;
        int64_t              last_pkt_pts_us  = -1;
        auto                 flush_pending_au = [&]()
        {
            if (pending_au.empty())
            {
                return;
            }
            EncPacketView au_view;
            au_view.data         = pending_au.data();
            au_view.len          = pending_au.size();
            au_view.dts_us       = pending_dts_us;
            au_view.pts_us       = pending_pts_us;
            au_view.eos          = pending_eos;
            au_view.is_partition = false;
            au_view.is_eoi       = true;
            au_view.is_extra     = false;
            au_view.handle       = nullptr;

            if (zlm_publisher.InputPacketChunk(au_view) == 0)
            {
                zlm_input_ok.fetch_add(
                    1, std::memory_order_relaxed);  // 以原子方式将指定值加到当前变量上
            }
            else
            {
                zlm_input_fail.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "Failed to input encoded AU to ZLM, len=" << au_view.len << std::endl;
            }
            pending_au.clear();
            pending_dts_us = -1;
            pending_pts_us = -1;
            pending_eos    = false;
        };
        while (true)
        {
            const bool    stop_now   = stop_requested.load();
            const bool    input_done = encode_input_done.load();
            EncPacketView pkt_view;
            const int     get_packet_ret =
                mpp_encoder_instance.EncoderGetPacket(&pkt_view);  // 拿到一个 packet
            if (get_packet_ret == mpp_enc_packet_result::kError)   // 出错分支
            {
                if (stop_now && input_done)
                {
                    break;
                }
                std::cerr << "Failed to get encoded packet from encoder" << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }
            if (get_packet_ret == mpp_enc_packet_result::kNoPacket)  // 当前无输出包
            {
                // 如果是因为到达 EOS 导致无包，则先 flush 掉可能积压的
                /// AU，再退出循环；否则继续等待包的产生。注意这里即使输入已经结束了，也可能存在编码器内部积压的
                /// AU 没有及时输出，所以不能直接以输入结束作为退出条件。
                if (mpp_encoder_instance.IsPacketEOS())
                {
                    flush_pending_au();
                    request_stop();
                    break;
                }
                if (stop_now && input_done)
                {
                    flush_pending_au();
                    break;
                }
                // 无包但未到
                // EOS，说明编码器内部可能还在处理输入帧，继续等待输出包的产生；
                // 如果一直等不到包了，则说明可能出现了问题，后续循环中会有超时统计输出。
                std::this_thread::sleep_for(kEncodeNoPacketSleep);
                continue;
            }
            if (get_packet_ret != mpp_enc_packet_result::kHasPacket)
            {
                // 既不是出错也不是无包，理论上应该就是成功拿到包了，但这里加个兜底的日志输出以防万一。
                std::cerr << "Unexpected encoder packet state: " << get_packet_ret << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }

            enc_pkt_total.fetch_add(1, std::memory_order_relaxed);  // 成功获取到包，计数器加一
            if (!first_ts_logged)
            {
                first_ts_logged = true;
                std::cout << "[ENC->ZLM] first packet ts: dts_us=" << pkt_view.dts_us
                          << ", pts_us=" << pkt_view.pts_us << std::endl;
            }
            if (pkt_view.dts_us < 0 || pkt_view.pts_us < 0)
            {
                bad_pkt_ts_count++;
            }
            last_pkt_dts_us = pkt_view.dts_us;
            last_pkt_pts_us = pkt_view.pts_us;
            if (pkt_view.data && pkt_view.len > 0)  // 确保数据有效
            {
                if (pending_au.empty())  // 说明当前这个 packet 是一个新的 AU 的开始
                {
                    pending_dts_us = pkt_view.dts_us;
                    pending_pts_us = pkt_view.pts_us;
                    pending_eos    = pkt_view.eos;
                }
                else
                {
                    pending_eos = pending_eos || pkt_view.eos;
                    if (pending_dts_us < 0 && pkt_view.dts_us >= 0)
                    {
                        pending_dts_us = pkt_view.dts_us;
                    }
                    if (pending_pts_us < 0 && pkt_view.pts_us >= 0)
                    {
                        pending_pts_us = pkt_view.pts_us;
                    }
                }
                const uint8_t* packet_bytes_addr = static_cast<const uint8_t*>(pkt_view.data);
                pending_au.insert(
                    pending_au.end(), packet_bytes_addr,
                    packet_bytes_addr + pkt_view.len);  // 将当前 packet 的数据追加到 pending_au 中
            }
            const bool packet_is_partition = pkt_view.is_partition;
            const bool packet_is_eoi       = pkt_view.is_eoi;
            const bool packet_eos          = pkt_view.eos;
            if (mpp_encoder_instance.EncoderReleasePacket(&pkt_view) != 0)
            {
                std::cerr << "Failed to release encoded packet" << std::endl;
                fatal_error.store(true);
                request_stop();
                break;
            }
            const bool au_done = (!packet_is_partition) || packet_is_eoi || packet_eos;
            if (au_done)  // 完整的AU 拿到了，送给 ZLM 推流
            {
                flush_pending_au();
            }

            const auto now_tp = std::chrono::steady_clock::now();
            if (now_tp - last_stat_tp >=
                std::chrono::seconds(5))  // 每 5 秒输出一次统计信息，帮助观察程序运行状态和性能指标
            {
                last_stat_tp = now_tp;  // 更新上次输出统计的时间点
                std::cout << "[ENC->ZLM] enc_pkt_total=" << enc_pkt_total.load()
                          << ", zlm_input_ok=" << zlm_input_ok.load()
                          << ", zlm_input_fail=" << zlm_input_fail.load()
                          << ", zlm_frame_out=" << zlm_publisher.GetOutputFrameCount()
                          << ", ts_fallback=" << zlm_publisher.GetTimestampFallbackCount()
                          << ", bad_pkt_ts=" << bad_pkt_ts_count
                          << ", last_pkt_dts_us=" << last_pkt_dts_us
                          << ", last_pkt_pts_us=" << last_pkt_pts_us << std::endl;
            }

            if (mpp_encoder_instance.IsPacketEOS())
            {
                request_stop();
                break;
            }
        }
    };
    std::thread mpp_encode_output_thread(run_mpp_encode_output_thread);

    auto     stat_tp        = std::chrono::steady_clock::now();
    uint64_t stat_last_pkts = 0;
    while (!stop_requested.load())  // 主线程等待退出信号的触发
    {
        // 退出（Ctrl+C 或平台请求）
        if (should_exit || controller.exit_requested())
        {
            controller.request_exit();
            request_stop();
            break;
        }
        // 换摄像头：跳出本次流水线，外层循环用新设备重建
        if (controller.restart_requested())
        {
            std::cout << "[CTL] camera switch requested -> stopping current pipeline" << std::endl;
            request_stop();
            break;
        }
        // 周期性把视频输出 fps / 掉帧上报给控制器（供 /api/status）
        const auto   now_tp = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now_tp - stat_tp).count();
        if (dt >= 1.0)
        {
            const uint64_t pkts     = enc_pkt_total.load(std::memory_order_relaxed);
            const double   vfps     = static_cast<double>(pkts - stat_last_pkts) / dt;
            stat_last_pkts          = pkts;
            stat_tp                 = now_tp;
            controller.set_stream_stats(vfps, static_cast<int>(active_cfg.width),
                                        static_cast<int>(active_cfg.height),
                                        dropped_frames.load() + dropped_rga_queue_frames.load());
        }
        std::this_thread::sleep_for(kMainLoopSleep);
    }
    // 请求所有线程停止，并等待它们退出
    request_stop();
    if (capture_thread.joinable())
    {
        capture_thread.join();
    }
    if (decode_thread.joinable())
    {
        decode_thread.join();
    }
    if (rga_thread.joinable())
    {
        rga_thread.join();
    }
    if (mpp_encode_input_thread.joinable())
    {
        mpp_encode_input_thread.join();
    }
    if (mpp_encode_output_thread.joinable())
    {
        mpp_encode_output_thread.join();
    }
    if (mpp_output_recycle_thread.joinable())
    {
        mpp_output_recycle_thread.join();
    }
    drain_mpp_recycle_requests();  // 确保所有待回收的输出都被处理掉，避免资源泄漏

    {
        std::deque<const IO_FD_t*> pending_encode_inputs;
        {
            std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
            while (!mppEncodeInputQueue.empty())
            {
                pending_encode_inputs.push_back(mppEncodeInputQueue.front());
                mppEncodeInputQueue.pop_front();
            }
        }
        for (const IO_FD_t* output_desc : pending_encode_inputs)
        {
            if (rga_instance.QueueOutputToRecycle(output_desc) != 0)
            {
                std::cerr << "Failed to recycle pending encoder input descriptor, fd="
                          << ((output_desc) ? output_desc->fd : -1) << std::endl;
            }
        }
    }

    // 注意：ZLM 推流器跨重启保活，切摄像头时不在此关闭（由 main() 退出时统一 Close）。

    // 退出阶段不再执行回队，交由 V4L2 反初始化流程（STREAMOFF + REQBUFS(0)）统一释放。
    {
        std::lock_guard<std::mutex> lock(ready_mutex);
        readyQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(recycle_mutex);
        recycleQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(rga_consume_mutex);
        rgaConsumeQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);
        mppRecycleOutputQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
        mppEncodeInputQueue.clear();
    }

    // 依赖析构函数完成资源的回收
    if (dropped_frames > 0)
    {
        std::cout << "Dropped old frames due to readyQueue backlog: " << dropped_frames
                  << std::endl;
    }
    if (dropped_rga_queue_frames > 0)
    {
        std::cout << "Dropped frames due to rgaConsumeQueue backlog: " << dropped_rga_queue_frames
                  << std::endl;
    }
    std::cout << "[CTL] pipeline for " << device << " stopped" << std::endl;
    return fatal_error.load() ? 1 : 0;
}

int main()
{
    // Ctrl+C 优雅退出
    std::signal(SIGINT, signal_handler);

    AppController& controller = AppController::Instance();

    // ---- 解析 YOLO 相关环境变量并创建跨重启保活的 NPU 推理池 ----
    const char*       model_env  = std::getenv("YOLO_MODEL_PATH");
    const char*       label_env  = std::getenv("YOLO_LABEL_PATH");
    const std::string model_path = (model_env && model_env[0] != '\0') ? model_env
                                                                       : "./model/yolov8n.rknn";
    const std::string label_path =
        (label_env && label_env[0] != '\0') ? label_env : "./model/coco_80_labels_list.txt";

    int         instances = 3;  // 默认 3 实例，吃满 RK3588 三核
    const char* inst_env  = std::getenv("YOLO_NPU_INSTANCES");
    if (inst_env && inst_env[0] != '\0')
    {
        const int v = std::atoi(inst_env);
        if (v >= 1 && v <= 3)
        {
            instances = v;
        }
    }
    unsigned    detect_stride = 1;  // 每 N 帧提交一次检测（1=每帧）
    const char* stride_env    = std::getenv("YOLO_DETECT_STRIDE");
    if (stride_env && stride_env[0] != '\0')
    {
        const int v = std::atoi(stride_env);
        if (v >= 1 && v <= 30)
        {
            detect_stride = static_cast<unsigned>(v);
        }
    }

    NpuDetectorPool yolo_pool;
    if (yolo_pool.Init(model_path, label_path, instances) == 0)
    {
        controller.set_yolo_available(true);
        controller.set_yolo_enabled(true);
        controller.set_model_name(model_path);
        std::cout << "[YOLO] enabled, model=" << model_path << ", instances=" << instances
                  << ", detect_stride=" << detect_stride << std::endl;
        std::cout << "[YOLO] labels=" << label_path << std::endl;
        std::cout << "[YOLO] env 覆盖: YOLO_MODEL_PATH / YOLO_LABEL_PATH / YOLO_NPU_INSTANCES(1-3) / "
                     "YOLO_DETECT_STRIDE(1-30)"
                  << std::endl;
    }
    else
    {
        controller.set_yolo_available(false);
        controller.set_yolo_enabled(false);
        std::cerr << "[YOLO] disabled: failed to init model from " << model_path << std::endl;
    }
    controller.BindPool(&yolo_pool);

    // ---- 压测模式：YOLO_BENCH=N 时，用合成帧测推理吞吐（不启动摄像头/推流），跑完退出 ----
    const char* bench_env = std::getenv("YOLO_BENCH");
    if (bench_env && bench_env[0] != '\0')
    {
        if (!controller.yolo_available())
        {
            std::cerr << "[BENCH] 模型未加载，无法压测" << std::endl;
            return 1;
        }
        int iters = std::atoi(bench_env);
        if (iters < 30)
        {
            iters = 300;
        }
        std::cout << "[BENCH] 预热..." << std::endl;
        (void)yolo_pool.Benchmark(60);  // 预热
        std::cout << "[BENCH] 开始压测 " << iters << " 次推理 (instances=" << instances << ")..."
                  << std::endl;
        const double fps = yolo_pool.Benchmark(iters);
        std::cout << "[BENCH] 结果: instances=" << instances << ", 推理吞吐=" << fps << " fps"
                  << " (单实例约 " << (fps / (instances > 0 ? instances : 1)) << " fps/核)"
                  << std::endl;
        yolo_pool.Deinit();
        return 0;
    }

    // ---- 创建跨重启保活的 ZLM 推流器（端口/流名固定）----
    ZlmPublisher zlm_publisher;
    if (zlm_publisher.Init() != 0)
    {
        std::cerr << "Failed to initialize ZLM publisher" << std::endl;
        return 1;
    }
    std::cout << "[ZLM] RTSP URL: " << zlm_publisher.GetRtspUrl() << std::endl;
    std::cout << "[ZLM] WebRTC API(play): " << zlm_publisher.GetWebRtcApiUrl() << std::endl;
    std::cout << "[ZLM] 控制接口: GET /api/status | GET /api/cameras | POST /api/camera | POST /api/yolo"
              << std::endl;
    std::cout << "[ZLM] 请将地址中的 <设备IP> 替换为开发板实际 IP" << std::endl;

    // 把 /api/* 路由到控制器。
    ZlmPublisher::SetApiHandler(
        [&controller](const std::string& method, const std::string& path, const std::string& body,
                      std::string& out_json)
        { return controller.HandleApi(method, path, body, out_json); });

    // 初始设备：env CAMERA_DEVICE 覆盖，否则默认 /dev/video11。
    const char* dev_env = std::getenv("CAMERA_DEVICE");
    controller.set_current_device((dev_env && dev_env[0] != '\0') ? dev_env : kDefaultDevice);

    // ---- 主循环：运行流水线；换摄像头则用新设备重启，退出请求则结束；
    //      相机瞬时故障（如无帧超时）则退避后重启同一设备重试，连续多次快速失败才放弃 ----
    int           last_ret     = 0;
    std::string   good_device  = controller.current_device();  // 最近一次能正常运行的设备
    int           consec_fail  = 0;
    constexpr int kMaxConsecFail = 5;   // 连续快速失败上限，超过才真正退出
    while (!controller.exit_requested())
    {
        const std::string dev     = controller.current_device();
        const auto         t_start = std::chrono::steady_clock::now();
        last_ret                   = run_pipeline(dev, zlm_publisher, yolo_pool, detect_stride);
        const double ran_sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

        if (controller.exit_requested())
        {
            break;
        }
        if (controller.restart_requested())
        {
            controller.clear_restart();
            good_device            = dev;  // 当前设备此前运行正常，记为可回退目标
            consec_fail            = 0;
            const std::string next = controller.pending_device();
            if (!next.empty())
            {
                controller.set_current_device(next);
            }
            std::cout << "[CTL] restarting pipeline with device=" << controller.current_device()
                      << std::endl;
            continue;
        }

        // 非重启的异常返回（相机超时 / 致命错误）。
        // 若这一路此前健康运行过一段时间(>=20s)，视为瞬时故障，重置连续失败计数。
        if (ran_sec >= 20.0)
        {
            consec_fail = 0;
        }

        // 刚切过去的新设备一上来就失败：回退到上一个可用设备。
        if (dev != good_device && !good_device.empty())
        {
            std::cerr << "[CTL] device " << dev << " failed, revert to " << good_device << std::endl;
            controller.set_current_device(good_device);
            continue;
        }

        // 当前（可用）设备发生故障：退避后重启重试，别让一次卡顿把整机搞退出。
        if (++consec_fail <= kMaxConsecFail)
        {
            std::cerr << "[CTL] pipeline for " << dev << " stopped (连续失败 " << consec_fail << "/"
                      << kMaxConsecFail << ")，2 秒后重启同一设备重试..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::cerr << "[CTL] " << dev << " 连续 " << consec_fail << " 次快速失败，放弃并退出。"
                  << std::endl;
        break;
    }

    zlm_publisher.Close();
    yolo_pool.Deinit();
    std::cout << "Camera platform app exited" << std::endl;
    return last_ret;
}
