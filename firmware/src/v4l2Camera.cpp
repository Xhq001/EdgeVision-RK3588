#include "v4l2Camera.h"
#include "common_utils.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <cmath>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>

namespace
{
constexpr const char* kCameraDmaHeapPath = "/dev/dma_heap/system";

int alloc_dma_buf_fd(size_t size, int* out_fd, void** out_base)
{
    if (!out_fd || !out_base || size == 0)
    {
        return -1;
    }

    int heap_fd = open(kCameraDmaHeapPath, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        std::cerr << "Failed to open dma heap " << kCameraDmaHeapPath << ": "
                  << std::strerror(errno) << std::endl;
        return -1;
    }

    dma_heap_allocation_data req{};
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
    {
        std::cerr << "Failed to allocate dma-buf from heap: " << std::strerror(errno) << std::endl;
        close(heap_fd);
        return -1;
    }
    close(heap_fd);

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped == MAP_FAILED)
    {
        std::cerr << "Failed to mmap dma-buf fd=" << req.fd << ": " << std::strerror(errno)
                  << std::endl;
        close(req.fd);
        return -1;
    }

    *out_fd   = req.fd;
    *out_base = mapped;
    return 0;
}

bool dma_buf_sync_read(int fd, bool is_start)
{
    if (fd < 0)
    {
        return false;
    }

    dma_buf_sync sync{};
    sync.flags = (is_start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    {
        return false;
    }
    return true;
}

int64_t get_monotonic_time_us()
{
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return -1;
    }
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec / 1000);
}

int64_t timeval_to_us(const timeval& tv)
{
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + static_cast<int64_t>(tv.tv_usec);
}

}  // namespace

V4L2_Camera::V4L2_Camera()
{
    // std::cout << "V4L2_Camera constructor called" << std::endl;
}

V4L2_Camera::~V4L2_Camera()
{
    if (isStreamOn)
    {
        capture_stream_switch(false);  // 停止流式捕获
    }
    if (NumBuffers > 0)
    {
        deinit_camera_buffer();  // 解除缓冲区映射
    }
    if (camera_fd >= 0)
    {
        close_camera();  // 关闭摄像头设备
    }
    // std::cout << "V4L2_Camera destructor called" << std::endl;
}

int V4L2_Camera::camera_GlobalInit(const std::string& device, const int exposureMs)
{
    if (camera_fd >= 0 || NumBuffers > 0 || isStreamOn)
    {
        std::cerr
            << "Camera is already initialized, please recreate object before re-initialization"
            << std::endl;
        return -1;
    }

    bool opened        = false;
    bool buffersInited = false;
    bool streamStarted = false;

    if (open_camera(device) != 0)
    {
        std::cerr << "Failed to open camera during global initialization" << std::endl;
        goto fail;
    }
    opened = true;

    if (check_cameraCapabilities() != 0)
    {
        std::cerr << "Camera does not meet the required capabilities during global initialization"
                  << std::endl;
        goto fail;
    }

    if (init_camera_buffer() != 0)
    {
        std::cerr << "Failed to initialize camera buffers during global initialization"
                  << std::endl;
        goto fail;
    }
    buffersInited = true;

    if (set_exposure_time(exposureMs) != 0)
    {
        std::cerr << "Warning: failed to set exposure time during global initialization, continue"
                  << std::endl;
    }

    if (capture_stream_switch(true) != 0)
    {
        std::cerr << "Failed to start streaming capture during global initialization" << std::endl;
        goto fail;
    }
    streamStarted = true;

    return 0;

fail:
    if (streamStarted)
    {
        capture_stream_switch(false);
    }
    if (buffersInited)
    {
        deinit_camera_buffer();
    }
    if (opened && camera_fd >= 0)
    {
        if (close(camera_fd) != 0)
        {
            std::cerr << "Rollback close failed: " << std::strerror(errno) << std::endl;
        }
        camera_fd          = -1;
        isStreamOn         = false;
        isStreamingSupport = false;
        isMultiPlanarCapture = false;
        captureBufferType    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        captureMemoryType    = V4L2_MEMORY_DMABUF;
        capturePlaneCount    = 1;
    }
    return -1;
}

int V4L2_Camera::open_camera(const char* device)
{
    if (device == nullptr)
    {
        std::cerr << "Camera device path is null" << std::endl;
        return -1;
    }

    if (camera_fd >= 0)
    {
        std::cout << "Camera already opened: fd=" << camera_fd << std::endl;
        return 0;
    }

    // std::cout << "Opening camera device: " << device << std::endl;
    camera_fd = open(device, O_RDWR);
    if (camera_fd < 0)
    {
        std::cerr << "Failed to open " << device << ": " << std::strerror(errno) << std::endl;
        return -1;
    }
    return 0;
}

int V4L2_Camera::open_camera(const std::string& device) { return open_camera(device.c_str()); }

/**
 * @brief Checks the capabilities of the opened camera device.
 * 检查摄像头是否支持视频输出、按照指定的输出格式、分辨率、帧率，设置曝光时间等功能
 */
int V4L2_Camera::check_cameraCapabilities()
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }

    // Reset cached capability flags for every fresh check.
    isStreamingSupport        = false;
    isStreamOn                = false;
    isMJPEGSupport            = false;
    isTargetResolutionSupport = false;
    isTargetFpsSupport        = false;
    isMultiPlanarCapture      = false;
    captureBufferType         = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    captureMemoryType         = V4L2_MEMORY_DMABUF;
    capturePlaneCount         = 1;
    ActiveConfig              = {};

    if (ioctl(camera_fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        std::cerr << "Failed to query camera capabilities: " << std::strerror(errno) << std::endl;
        return -1;
    }
    std::cout << "Camera Capabilities:" << std::endl;
    std::cout << "  Driver: " << cap.driver << std::endl;
    std::cout << "  Card: " << cap.card << std::endl;
    // 某些驱动会把真实能力放在 device_caps；若不带 DEVICE_CAPS 标志则回退到 capabilities。
    // 统一使用 effective_caps 可以避免不同内核/驱动版本下能力位读取不一致。
    const __u32 effective_caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                             : cap.capabilities;

    const bool has_capture_single = (effective_caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool has_capture_mplane = (effective_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;

    // 既没有单平面也没有多平面采集能力，通常是 metadata 或控制节点。
    if (!has_capture_single && !has_capture_mplane)
    {
        std::cerr << "Camera node does not support VIDEO_CAPTURE (effective_caps=0x" << std::hex
                  << effective_caps << std::dec << ")" << std::endl;
        std::cerr << "Tip: this node may be metadata/control-only, please use a real capture node "
                     "(for example /dev/video20)"
                  << std::endl;
        return -1;
    }

    // 优先单平面；若仅支持 mplane，则走 mplane 路径（当前仅支持单-plane 的 mplane 布局）。
    // 这样可以兼容 rkisp_mainpath 这类只提供 VIDEO_CAPTURE_MPLANE 的节点。
    isMultiPlanarCapture = (!has_capture_single && has_capture_mplane);
    captureBufferType =
        isMultiPlanarCapture ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    captureMemoryType = isMultiPlanarCapture ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;
    capturePlaneCount = 1;
    if (isMultiPlanarCapture)
    {
        std::cout << "  Capture API: VIDEO_CAPTURE_MPLANE (memory=MMAP)" << std::endl;
    }
    else
    {
        std::cout << "  Capture API: VIDEO_CAPTURE (memory=DMABUF)" << std::endl;
    }
    if (!(effective_caps & V4L2_CAP_STREAMING))
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    this->isStreamingSupport = true;  // 标记支持流式传输

    bool  isYUYVSupport   = false;
    bool  isUYVYSupport   = false;
    bool  isNV12Support   = false;
    bool  isNV21Support   = false;
    bool  isNV16Support   = false;
    bool  isNV61Support   = false;
    __u32 selected_pixfmt = 0;

    // 检查输出格式支持，并按优先级选择：
    // MJPEG(保留原链路) > YUYV/UYVY(422 packed) > NV12/NV21/NV16/NV61。
    std::memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.index = 0;
    fmtdesc.type  = captureBufferType;
    std::cout << "  Supported capture formats:" << std::endl;
    while (ioctl(camera_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
        std::cout << "    - " << util::FourccToString(fmtdesc.pixelformat) << " ("
                  << reinterpret_cast<const char*>(fmtdesc.description) << ")" << std::endl;
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG)
        {
            this->isMJPEGSupport = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV)
        {
            isYUYVSupport = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_UYVY)
        {
            isUYVYSupport = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV12)
        {
            isNV12Support = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV21)
        {
            isNV21Support = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV16)
        {
            isNV16Support = true;
        }
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV61)
        {
            isNV61Support = true;
        }
        fmtdesc.index++;
    }

    if (this->isMJPEGSupport)
    {
        selected_pixfmt = V4L2_PIX_FMT_MJPEG;
    }
    else if (isYUYVSupport)
    {
        selected_pixfmt = V4L2_PIX_FMT_YUYV;
        std::cout << "  MJPEG is not supported by this camera, fallback to YUYV" << std::endl;
    }
    else if (isUYVYSupport)
    {
        selected_pixfmt = V4L2_PIX_FMT_UYVY;
        std::cout << "  MJPEG is not supported by this camera, fallback to UYVY" << std::endl;
    }
    else if (isNV12Support)
    {
        selected_pixfmt = V4L2_PIX_FMT_NV12;
        std::cout << "  MJPEG is not supported by this camera, fallback to NV12" << std::endl;
    }
    else if (isNV21Support)
    {
        selected_pixfmt = V4L2_PIX_FMT_NV21;
        std::cout << "  MJPEG is not supported by this camera, fallback to NV21" << std::endl;
    }
    else if (isNV16Support)
    {
        selected_pixfmt = V4L2_PIX_FMT_NV16;
        std::cout << "  MJPEG is not supported by this camera, fallback to NV16" << std::endl;
    }
    else if (isNV61Support)
    {
        selected_pixfmt = V4L2_PIX_FMT_NV61;
        std::cout << "  MJPEG is not supported by this camera, fallback to NV61" << std::endl;
    }
    else
    {
        std::cerr << "Camera does not support required formats (MJPEG/YUYV/UYVY/NV12/NV21/NV16/NV61)"
                  << std::endl;
        return -1;
    }

    std::cout << "  Selected capture format: " << util::FourccToString(selected_pixfmt) << std::endl;

    // 基于选中格式检查目标分辨率（同时支持 discrete / stepwise / continuous）。
    // 这里“检查”的目标是提前给出可读日志，而不是提前硬拒绝：
    // 最终是否可用仍以后续 VIDIOC_S_FMT/VIDIOC_G_FMT 的协商结果为准。
    auto is_stepwise_value_supported = [](__u32 value, __u32 min_value, __u32 max_value,
                                          __u32 step_value)
    {
        if (value < min_value || value > max_value)
        {
            return false;
        }
        if (step_value <= 1)
        {
            return true;
        }
        return ((value - min_value) % step_value) == 0;
    };

    bool has_framesize_info = false;
    std::memset(&frmsize, 0, sizeof(frmsize));
    frmsize.index        = 0;
    frmsize.pixel_format = selected_pixfmt;
    while (ioctl(camera_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
    {
        has_framesize_info = true;
        frmsize.index++;
        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
            if (frmsize.discrete.width == camera_params::kCaptureWidth &&
                frmsize.discrete.height == camera_params::kCaptureHeight)
            {
                std::cout << "    - Supported resolution: " << frmsize.discrete.width << "x"
                          << frmsize.discrete.height << std::endl;
                this->isTargetResolutionSupport = true;
            }
        }
        else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                 frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
        {
            const auto& step = frmsize.stepwise;
            if (is_stepwise_value_supported(camera_params::kCaptureWidth, step.min_width,
                                            step.max_width, step.step_width) &&
                is_stepwise_value_supported(camera_params::kCaptureHeight, step.min_height,
                                            step.max_height, step.step_height))
            {
                std::cout << "    - Supported resolution by stepwise range: "
                          << camera_params::kCaptureWidth << "x" << camera_params::kCaptureHeight
                          << " in [" << step.min_width << "x" << step.min_height << " .. "
                          << step.max_width << "x" << step.max_height << "], step "
                          << step.step_width << "x" << step.step_height << std::endl;
                this->isTargetResolutionSupport = true;
            }
        }
    }
    if (!has_framesize_info)
    {
        std::cout << "    - No frame size enum entries, continue with driver negotiation"
                  << std::endl;
        this->isTargetResolutionSupport = true;
    }
    if (this->isTargetResolutionSupport == false)
    {
        // 对 stepwise 摄像头，驱动可能不返回完整枚举列表，但 S_FMT 仍可成功。
        // 因此这里不直接失败，只打印 warning 并继续与驱动协商。
        std::cerr << "Warning: camera did not explicitly report support for "
                  << camera_params::kCaptureWidth << "x" << camera_params::kCaptureHeight
                  << " at format " << util::FourccToString(selected_pixfmt)
                  << ", continue with S_FMT negotiation" << std::endl;
    }

    // 基于选中格式和目标分辨率检查目标帧率（同时支持 discrete / stepwise / continuous）。
    bool has_frameinterval_info = false;
    std::memset(&frmival, 0, sizeof(frmival));
    frmival.index        = 0;
    frmival.pixel_format = selected_pixfmt;
    frmival.width        = camera_params::kCaptureWidth;
    frmival.height       = camera_params::kCaptureHeight;
    while (ioctl(camera_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0)
    {
        has_frameinterval_info = true;
        frmival.index++;
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
        {
            double fps =
                static_cast<double>(frmival.discrete.denominator) / frmival.discrete.numerator;
            if (std::abs(fps - static_cast<double>(camera_params::kCaptureFps)) < 0.1)
            {
                std::cout << "    - Supported frame rate: " << fps << " fps" << std::endl;
                this->isTargetFpsSupport = true;
            }
        }
        else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                 frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS)
        {
            const auto& step = frmival.stepwise;
            double      min_tpf =
                static_cast<double>(step.min.numerator) / static_cast<double>(step.min.denominator);
            double max_tpf =
                static_cast<double>(step.max.numerator) / static_cast<double>(step.max.denominator);
            if (min_tpf > max_tpf)
            {
                const double tmp = min_tpf;
                min_tpf          = max_tpf;
                max_tpf          = tmp;
            }
            const double requested_tpf = 1.0 / static_cast<double>(camera_params::kCaptureFps);
            if (requested_tpf + 1e-6 >= min_tpf && requested_tpf - 1e-6 <= max_tpf)
            {
                std::cout << "    - Supported frame rate by stepwise range: "
                          << camera_params::kCaptureFps << " fps" << std::endl;
                this->isTargetFpsSupport = true;
            }
        }
    }
    if (!has_frameinterval_info)
    {
        std::cout << "    - No frame interval enum entries, continue with S_PARM/G_PARM verify"
                  << std::endl;
        this->isTargetFpsSupport = true;
    }
    if (this->isTargetFpsSupport == false)
    {
        // 同上，帧率枚举不完整并不等于不可用，交给 S_PARM/G_PARM 做最终确认。
        std::cerr << "Warning: camera did not explicitly report support for "
                  << camera_params::kCaptureFps << "fps at " << camera_params::kCaptureWidth << "x"
                  << camera_params::kCaptureHeight << " for format "
                  << util::FourccToString(selected_pixfmt)
                  << ", continue with S_PARM/G_PARM verification" << std::endl;
    }

    // 开始设置
    std::memset(&v4l2fmt, 0, sizeof(v4l2fmt));
    v4l2fmt.type = captureBufferType;
    if (isMultiPlanarCapture)
    {
        // mplane 的 S_FMT 需要填写 pix_mp 分支；num_planes 先按 1 提交，再用 G_FMT 读取驱动最终值。
        v4l2fmt.fmt.pix_mp.width       = camera_params::kCaptureWidth;
        v4l2fmt.fmt.pix_mp.height      = camera_params::kCaptureHeight;
        v4l2fmt.fmt.pix_mp.pixelformat = selected_pixfmt;
        v4l2fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        v4l2fmt.fmt.pix_mp.num_planes  = 1;  // 当前只处理单 plane mplane 格式（如 UYVY）。
    }
    else
    {
        v4l2fmt.fmt.pix.width       = camera_params::kCaptureWidth;
        v4l2fmt.fmt.pix.height      = camera_params::kCaptureHeight;
        v4l2fmt.fmt.pix.pixelformat = selected_pixfmt;
        v4l2fmt.fmt.pix.field       = V4L2_FIELD_NONE;  // 设置为逐行扫描
    }
    if (ioctl(camera_fd, VIDIOC_S_FMT, &v4l2fmt) < 0)
    {
        std::cerr << "Failed to set pixel format: " << std::strerror(errno) << std::endl;
        if (errno == EBUSY)
        {
            std::cerr << "Tip: device is busy. Another process may still hold this node. "
                         "Check with: fuser -v /dev/video11"
                      << std::endl;
        }
        return -1;
    }
    // 二次确认：S_FMT 后立即 G_FMT，读取驱动最终生效配置。
    // 这是能力协商最关键的一步：真正要喂给下游模块的宽高/stride/pixfmt 只能以这里为准。
    {
        struct v4l2_format fmt_get
        {};
        fmt_get.type = captureBufferType;
        if (ioctl(camera_fd, VIDIOC_G_FMT, &fmt_get) < 0)
        {
            std::cerr << "Failed to get pixel format after VIDIOC_S_FMT: " << std::strerror(errno)
                      << std::endl;
            return -1;
        }
        if (isMultiPlanarCapture)
        {
            if (fmt_get.fmt.pix_mp.num_planes < 1)
            {
                std::cerr << "Invalid mplane format returned: num_planes="
                          << fmt_get.fmt.pix_mp.num_planes << std::endl;
                return -1;
            }
            capturePlaneCount        = fmt_get.fmt.pix_mp.num_planes;
            ActiveConfig.width       = fmt_get.fmt.pix_mp.width;
            ActiveConfig.height      = fmt_get.fmt.pix_mp.height;
            ActiveConfig.pixelformat = fmt_get.fmt.pix_mp.pixelformat;
            ActiveConfig.bytesperline = fmt_get.fmt.pix_mp.plane_fmt[0].bytesperline;
            ActiveConfig.sizeimage    = fmt_get.fmt.pix_mp.plane_fmt[0].sizeimage;
        }
        else
        {
            capturePlaneCount        = 1;
            ActiveConfig.width       = fmt_get.fmt.pix.width;
            ActiveConfig.height      = fmt_get.fmt.pix.height;
            ActiveConfig.pixelformat = fmt_get.fmt.pix.pixelformat;
            ActiveConfig.bytesperline = fmt_get.fmt.pix.bytesperline;
            ActiveConfig.sizeimage    = fmt_get.fmt.pix.sizeimage;
        }
        std::cout << "[V4L2] Active format:"
                  << " width=" << ActiveConfig.width << " height=" << ActiveConfig.height
                  << " pixfmt=" << util::FourccToString(ActiveConfig.pixelformat)
                  << " bytesperline=" << ActiveConfig.bytesperline
                  << " sizeimage=" << ActiveConfig.sizeimage << std::endl;
    }

    // 像素格式必须是请求值，或至少仍属于本项目支持集合。
    if (ActiveConfig.pixelformat != selected_pixfmt)
    {
        std::cerr << "Warning: camera switched pixel format from "
                  << util::FourccToString(selected_pixfmt) << " to "
                  << util::FourccToString(ActiveConfig.pixelformat) << std::endl;
        if (ActiveConfig.pixelformat != V4L2_PIX_FMT_MJPEG &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_YUYV &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_UYVY &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_NV12 &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_NV21 &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_NV16 &&
            ActiveConfig.pixelformat != V4L2_PIX_FMT_NV61)
        {
            std::cerr << "Active pixel format is not supported by pipeline" << std::endl;
            return -1;
        }
    }
    this->isMJPEGSupport = (ActiveConfig.pixelformat == V4L2_PIX_FMT_MJPEG);
    if (isMultiPlanarCapture && capturePlaneCount != 1)
    {
        std::cerr << "Unsupported mplane layout: num_planes=" << capturePlaneCount
                  << ". Current pipeline only supports single-plane mplane formats." << std::endl;
        return -1;
    }
    if (ActiveConfig.width != camera_params::kCaptureWidth ||
        ActiveConfig.height != camera_params::kCaptureHeight)
    {
        std::cerr << "Warning: active resolution is " << ActiveConfig.width << "x"
                  << ActiveConfig.height << ", requested " << camera_params::kCaptureWidth << "x"
                  << camera_params::kCaptureHeight << std::endl;
    }
    // 帧率设置：尽力设置，不支持时仅告警，避免因部分节点不实现 G/S_PARM 导致初始化失败。
    // 例如 rkisp 某些节点会返回 ENOTTY，此时沿用 fallback fps metadata 继续运行。
    ActiveConfig.fps_num = 1;
    ActiveConfig.fps_den = camera_params::kCaptureFps;

    bool can_set_fps = false;
    std::memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = captureBufferType;
    if (ioctl(camera_fd, VIDIOC_G_PARM, &streamparm) == 0)
    {
        can_set_fps = (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) != 0;
        if (!can_set_fps)
        {
            std::cerr << "Warning: camera does not support setting frame rate via TIMEPERFRAME"
                      << std::endl;
        }
    }
    else
    {
        std::cerr << "Warning: failed to get stream parameters: " << std::strerror(errno)
                  << ", use fallback fps metadata" << std::endl;
    }

    if (can_set_fps)
    {
        std::memset(&streamparm, 0, sizeof(streamparm));
        streamparm.type                                  = captureBufferType;
        streamparm.parm.capture.timeperframe.numerator   = 1;                           // 分子
        streamparm.parm.capture.timeperframe.denominator = camera_params::kCaptureFps;  // 分母
        if (ioctl(camera_fd, VIDIOC_S_PARM, &streamparm) < 0)
        {
            std::cerr << "Warning: failed to set frame rate: " << std::strerror(errno)
                      << ", continue with fallback fps metadata" << std::endl;
        }
    }

    // 二次确认：尽力读取驱动实际帧率；失败时保留 fallback 值。
    {
        struct v4l2_streamparm parm_get
        {};
        parm_get.type = captureBufferType;
        if (ioctl(camera_fd, VIDIOC_G_PARM, &parm_get) == 0 &&
            parm_get.parm.capture.timeperframe.numerator != 0)
        {
            double fps_verify = static_cast<double>(parm_get.parm.capture.timeperframe.denominator) /
                                parm_get.parm.capture.timeperframe.numerator;
            ActiveConfig.fps_num = parm_get.parm.capture.timeperframe.numerator;
            ActiveConfig.fps_den = parm_get.parm.capture.timeperframe.denominator;
            std::cout << "[V4L2] Active frame rate:"
                      << " tpf=" << ActiveConfig.fps_num << "/" << ActiveConfig.fps_den
                      << " fps=" << fps_verify << std::endl;
        }
        else
        {
            std::cout << "[V4L2] Active frame rate: fallback " << ActiveConfig.fps_den << " fps"
                      << std::endl;
        }
    }

    if (ActiveConfig.fps_num == 0)
    {
        ActiveConfig.fps_num = 1;
        ActiveConfig.fps_den = camera_params::kCaptureFps;
    }
    double fpsActual =
        static_cast<double>(ActiveConfig.fps_den) / static_cast<double>(ActiveConfig.fps_num);
    if (std::abs(fpsActual - static_cast<double>(camera_params::kCaptureFps)) > 0.1)
    {
        std::cerr << "Warning: active frame rate is " << fpsActual << " fps, requested "
                  << camera_params::kCaptureFps << " fps" << std::endl;
    }
    ActiveConfig.valid = true;
    std::cout << "Camera capabilities check passed.\nCamera Init Done " << std::endl;
    return 0;
}

/**
 * @brief Sets camera exposure mode/time.
 *
 * If exposure_time_ms is 0, this function enables auto exposure.
 * If exposure_time_ms is greater than 0, it switches to manual exposure,
 * checks the supported range of V4L2_CID_EXPOSURE_ABSOLUTE, and sets it.
 *
 * @param exposure_time_ms Exposure time in milliseconds.
 * @return 0 on success, -1 on failure.
 */
int V4L2_Camera::set_exposure_time(int exposure_time_ms)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }

    if (exposure_time_ms < 0)
    {
        std::cerr << "Invalid exposure time: " << exposure_time_ms << " ms (must be >= 0)"
                  << std::endl;
        return -1;
    }

    if (exposure_time_ms == 0)  // 自动曝光模式
    {
        v4l2_queryctrl query{};
        query.id = V4L2_CID_EXPOSURE_AUTO;
        if (ioctl(camera_fd, VIDIOC_QUERYCTRL, &query) < 0)
        {
            std::cerr << "Failed to query auto exposure control: " << std::strerror(errno)
                      << std::endl;
            return -1;
        }

        if (query.flags & V4L2_CTRL_FLAG_DISABLED)
        {
            std::cerr << "Auto exposure control is disabled by driver" << std::endl;
            return -1;
        }

        auto mode_supported = [&](int mode) -> bool
        {
            if (mode < query.minimum || mode > query.maximum)
            {
                return false;
            }
            if (query.type == V4L2_CTRL_TYPE_MENU || query.type == V4L2_CTRL_TYPE_INTEGER_MENU)
            {
                v4l2_querymenu menu{};
                menu.id    = V4L2_CID_EXPOSURE_AUTO;
                menu.index = static_cast<__u32>(mode);
                if (ioctl(camera_fd, VIDIOC_QUERYMENU, &menu) < 0)
                {
                    return false;
                }
            }
            return true;
        };

        auto mode_name = [](int mode) -> const char*
        {
            switch (mode)
            {
                case V4L2_EXPOSURE_AUTO:
                    return "Auto Mode";
                case V4L2_EXPOSURE_MANUAL:
                    return "Manual Mode";
                case V4L2_EXPOSURE_SHUTTER_PRIORITY:
                    return "Shutter Priority Mode";
                case V4L2_EXPOSURE_APERTURE_PRIORITY:
                    return "Aperture Priority Mode";
                default:
                    return "Unknown Mode";
            }
        };

        // 有些 UVC 摄像头不支持 V4L2_EXPOSURE_AUTO(0)，只支持 Aperture Priority(3)。
        // 这里按“真正自动优先，其次光圈优先”尝试，避免 EINVAL。
        const int auto_mode_candidates[] = {
            V4L2_EXPOSURE_AUTO,
            V4L2_EXPOSURE_APERTURE_PRIORITY,
            V4L2_EXPOSURE_SHUTTER_PRIORITY,
        };

        int last_errno = 0;
        for (int mode : auto_mode_candidates)
        {
            if (!mode_supported(mode))
            {
                continue;
            }

            v4l2_control auto_ctrl{};
            auto_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
            auto_ctrl.value = mode;
            if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) == 0)
            {
                std::cout << "Exposure mode set to " << mode_name(mode) << std::endl;
                return 0;
            }
            last_errno = errno;
        }

        if (last_errno != 0)
        {
            std::cerr << "Failed to set auto exposure mode: " << std::strerror(last_errno)
                      << std::endl;
        }
        else
        {
            std::cerr << "No supported auto exposure mode is available on this camera" << std::endl;
        }
        return -1;
    }

    // Most UVC drivers use V4L2_CID_EXPOSURE_ABSOLUTE in 100us units.
    const int exposure_100us = exposure_time_ms * 10;

    v4l2_control auto_ctrl{};
    auto_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
    auto_ctrl.value = V4L2_EXPOSURE_MANUAL;
    if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) < 0)
    {
        std::cerr << "Failed to set manual exposure mode: " << std::strerror(errno) << std::endl;
        return -1;
    }

    v4l2_queryctrl query{};
    query.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    if (ioctl(camera_fd, VIDIOC_QUERYCTRL, &query) < 0)
    {
        std::cerr << "Failed to query exposure range: " << std::strerror(errno) << std::endl;
        return -1;
    }

    if (query.flags & V4L2_CTRL_FLAG_DISABLED)
    {
        std::cerr << "Exposure control is disabled by driver" << std::endl;
        return -1;
    }

    if (exposure_100us < query.minimum || exposure_100us > query.maximum)
    {
        std::cerr << "Exposure out of range: " << exposure_time_ms << " ms"
                  << " (supported " << (query.minimum / 10.0) << "-" << (query.maximum / 10.0)
                  << " ms)" << std::endl;
        return -1;
    }

    v4l2_control exp_ctrl{};
    exp_ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
    exp_ctrl.value = exposure_100us;
    if (ioctl(camera_fd, VIDIOC_S_CTRL, &exp_ctrl) < 0)
    {
        std::cerr << "Failed to set exposure time: " << std::strerror(errno) << std::endl;
        return -1;
    }

    std::cout << "Exposure time set to " << exposure_time_ms << " ms" << std::endl;
    return 0;
}

int V4L2_Camera::init_camera_buffer()
{
    // 基础检查
    if (camera_fd == -1)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    if (NumBuffers > 0)
    {
        std::cerr << "Camera buffers already initialized" << std::endl;
        return -1;
    }

    NumBuffers       = 0;
    CurrentFrameDesc = nullptr;
    for (auto& frame_desc : FrameDescArray)
    {
        frame_desc.index             = -1;
        frame_desc.fd                = -1;
        frame_desc.base              = nullptr;
        frame_desc.Length            = 0;
        frame_desc.payloadSize       = 0;
        frame_desc.pts_us            = -1;
        frame_desc.dts_us            = -1;
        frame_desc.cpuSyncReadActive = false;
    }

    // 请求缓冲区
    struct v4l2_requestbuffers reqbuf
    {};
    const __u32 requested_count = camera_params::kRequestBufferCount;
    reqbuf.count                = requested_count;     // 请求的缓冲区数量
    reqbuf.type                 = captureBufferType;   // single/mplane
    reqbuf.memory               = captureMemoryType;   // DMABUF/MMAP
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        std::cerr << "Failed to request capture buffers: " << std::strerror(errno)
                  << std::endl;
        return -1;
    }
    if (reqbuf.count == 0)
    {
        std::cerr << "Driver returned zero capture buffers" << std::endl;
        return -1;
    }
    if (reqbuf.count > camera_params::kMaxMappedBuffers)
    {
        std::cerr << "Driver returned too many buffers: " << reqbuf.count
                  << ", max supported: " << camera_params::kMaxMappedBuffers << std::endl;
        return -1;
    }
    std::cout << "[V4L2] IO mode: "
              << ((captureMemoryType == V4L2_MEMORY_DMABUF) ? "dmabuf-pool" : "mmap")
              << ", type="
              << ((captureBufferType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ? "mplane"
                                                                            : "single-plane")
              << std::endl;
    std::cout << "[V4L2] Buffer request: requested=" << requested_count
              << ", granted=" << reqbuf.count << std::endl;

    if (captureMemoryType == V4L2_MEMORY_DMABUF)
    {
        // DMABUF 自建池：用户分配缓冲，驱动按 index 使用这些 fd。
        // 这条路径适合单平面 + 零拷贝链路（相机->RGA/MPP 直接传 fd）。
        const size_t alloc_size = (ActiveConfig.sizeimage > 0)
                                      ? static_cast<size_t>(ActiveConfig.sizeimage)
                                      : camera_params::kMaxFrameSize;
        for (__u32 i = 0; i < reqbuf.count; ++i)
        {
            int   dma_fd   = -1;
            void* dma_base = nullptr;
            if (alloc_dma_buf_fd(alloc_size, &dma_fd, &dma_base) != 0)
            {
                std::cerr << "Failed to allocate DMABUF for camera pool, index=" << i << std::endl;
                deinit_camera_buffer();
                return -1;
            }

            FrameDescArray[i].index  = static_cast<int>(i);  // 后续利用 index 来查找
            FrameDescArray[i].fd     = dma_fd;
            FrameDescArray[i].base   = dma_base;
            FrameDescArray[i].Length = alloc_size;
            this->NumBuffers++;
        }
    }
    else if (captureMemoryType == V4L2_MEMORY_MMAP)
    {
        for (__u32 i = 0; i < reqbuf.count; ++i)
        {
            struct v4l2_buffer buffer
            {};
            struct v4l2_plane planes[VIDEO_MAX_PLANES]
            {};
            buffer.type   = captureBufferType;
            buffer.memory = captureMemoryType;
            buffer.index  = i;
            if (isMultiPlanarCapture)
            {
                buffer.m.planes = planes;
                buffer.length   = VIDEO_MAX_PLANES;
            }

            if (ioctl(camera_fd, VIDIOC_QUERYBUF, &buffer) < 0)
            {
                std::cerr << "Failed to query buffer " << i << ": " << std::strerror(errno)
                          << std::endl;
                deinit_camera_buffer();
                return -1;
            }

            size_t map_len  = 0;
            off_t  map_off  = 0;
            __u32  n_planes = 1;
            if (isMultiPlanarCapture)
            {
                n_planes = buffer.length;
                if (n_planes < 1)
                {
                    std::cerr << "Invalid mplane buffer plane count: " << n_planes << std::endl;
                    deinit_camera_buffer();
                    return -1;
                }
                if (n_planes != 1)
                {
                    std::cerr << "Unsupported mplane buffer layout: planes=" << n_planes
                              << " (only single-plane mplane is supported)" << std::endl;
                    deinit_camera_buffer();
                    return -1;
                }
                // 当前工程仅支持“单-plane 的 mplane 格式”，统一映射 plane[0]。
                // 若后续接入多-plane mplane（例如某些 420 三平面），需要按 plane 分别处理。
                map_len = static_cast<size_t>(buffer.m.planes[0].length);
                map_off = static_cast<off_t>(buffer.m.planes[0].m.mem_offset);
            }
            else
            {
                map_len = static_cast<size_t>(buffer.length);
                map_off = static_cast<off_t>(buffer.m.offset);
            }

            void* mapped_base = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, camera_fd,
                                     map_off);
            if (mapped_base == MAP_FAILED)
            {
                std::cerr << "Failed to mmap capture buffer " << i << ": " << std::strerror(errno)
                          << std::endl;
                deinit_camera_buffer();
                return -1;
            }

            FrameDescArray[i].index  = static_cast<int>(i);
            FrameDescArray[i].fd     = -1;  // MMAP 模式下没有可复用的导出 fd。
            FrameDescArray[i].base   = mapped_base;
            FrameDescArray[i].Length = map_len;
            capturePlaneCount        = n_planes;
            this->NumBuffers++;
        }
    }
    else
    {
        std::cerr << "Unsupported V4L2 memory type: " << captureMemoryType << std::endl;
        return -1;
    }

    // 将所有缓冲区入队，准备开始捕获
    for (int i = 0; i < this->NumBuffers; i++)
    {
        struct v4l2_buffer buffer
        {};
        struct v4l2_plane planes[VIDEO_MAX_PLANES]
        {};
        buffer.index  = static_cast<__u32>(i);
        buffer.type   = captureBufferType;
        buffer.memory = captureMemoryType;

        if (captureMemoryType == V4L2_MEMORY_DMABUF)
        {
            buffer.m.fd   = FrameDescArray[i].fd;  // 驱动使用用户提供的 DMABUF fd
            buffer.length = static_cast<__u32>(FrameDescArray[i].Length);
        }
        else
        {
            if (isMultiPlanarCapture)
            {
                buffer.m.planes = planes;
                buffer.length   = capturePlaneCount;
                // MMAP + mplane 的 QBUF 至少要把每个 plane 的 length 带回去。
                planes[0].length = static_cast<__u32>(FrameDescArray[i].Length);
            }
            else
            {
                buffer.length = static_cast<__u32>(FrameDescArray[i].Length);
            }
        }

        if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)
        {
            std::cerr << "Failed to queue buffer " << buffer.index << ": " << std::strerror(errno)
                      << std::endl;
            deinit_camera_buffer();
            return -1;
        }
    }
    return 0;
}

int V4L2_Camera::deinit_camera_buffer()
{
    if (this->NumBuffers <= 0)
    {
        return 0;
    }

    int ret = 0;
    for (int i = 0; i < this->NumBuffers; i++)
    {
        if (FrameDescArray[i].cpuSyncReadActive && FrameDescArray[i].fd >= 0)
        {
            if (!dma_buf_sync_read(FrameDescArray[i].fd, false))
            {
                std::cerr << "Failed to end dma-buf cpu read sync for buffer " << i << ": "
                          << std::strerror(errno) << std::endl;
                ret = -1;
            }
            FrameDescArray[i].cpuSyncReadActive = false;
        }

        // 先解除用户态映射
        if (FrameDescArray[i].base != nullptr && FrameDescArray[i].Length > 0)
        {
            if (munmap(FrameDescArray[i].base, FrameDescArray[i].Length) != 0)
            {
                std::cerr << "Failed to munmap buffer " << i << ": " << std::strerror(errno)
                          << std::endl;
                ret = -1;
            }
            FrameDescArray[i].base        = nullptr;
            FrameDescArray[i].Length      = 0;
            FrameDescArray[i].payloadSize = 0;
            FrameDescArray[i].pts_us      = -1;
            FrameDescArray[i].dts_us      = -1;
        }

        // 再关闭导出的 dma-buf 文件描述符
        if (FrameDescArray[i].fd >= 0)
        {
            if (close(FrameDescArray[i].fd) != 0)
            {
                std::cerr << "Failed to close exported dma-buf fd for buffer " << i << ": "
                          << std::strerror(errno) << std::endl;
                ret = -1;
            }
            FrameDescArray[i].fd = -1;
        }
        FrameDescArray[i].index  = -1;
        FrameDescArray[i].pts_us = -1;
        FrameDescArray[i].dts_us = -1;
    }

    struct v4l2_requestbuffers reqbuf
    {};
    reqbuf.count  = 0;  // 释放驱动侧分配的全部缓冲区
    reqbuf.type   = captureBufferType;
    reqbuf.memory = captureMemoryType;
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        std::cerr << "Failed to release V4L2 buffers: " << std::strerror(errno) << std::endl;
        ret = -1;
    }

    NumBuffers       = 0;
    CurrentFrameDesc = nullptr;
    capturePlaneCount = 1;
    return ret;
}

int V4L2_Camera::capture_stream_switch(bool enabled)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    enum v4l2_buf_type type = captureBufferType;  // 视频捕获类型（single/mplane）
    if (enabled)
    {
        if (isStreamOn)
        {
            return 0;
        }
        if (ioctl(camera_fd, VIDIOC_STREAMON, &type) < 0)  // 开始流式捕获
        {
            std::cerr << "Failed to start streaming: " << std::strerror(errno) << std::endl;
            return -1;
        }
        isStreamOn = true;
        std::cout << "Streaming started" << std::endl;
    }
    else
    {
        if (!isStreamOn)
        {
            return 0;
        }
        if (ioctl(camera_fd, VIDIOC_STREAMOFF, &type) < 0)  // 停止流式捕获
        {
            std::cerr << "Failed to stop streaming: " << std::strerror(errno) << std::endl;
            return -1;
        }
        isStreamOn = false;
        std::cout << "Streaming stopped" << std::endl;
    }
    return 0;
}

int V4L2_Camera::camera_read_frame(size_t* out_length, size_t max_buffer_size)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return camera_read_result::kFatal;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return camera_read_result::kFatal;
    }
    if (!isStreamOn)
    {
        std::cerr << "Camera stream is not started" << std::endl;
        return camera_read_result::kFatal;
    }
    if (out_length == nullptr)
    {
        std::cerr << "Invalid output buffer arguments" << std::endl;
        return camera_read_result::kFatal;
    }

    CurrentFrameDesc = nullptr;

    unsigned int bufIdx    = 0;
    unsigned int frameSize = 0;
    const int    read_rc =
        dequeue_buffer(bufIdx, frameSize,
                       max_buffer_size);  // 从驱动队列中取出一帧数据，得到对应的缓冲区索引和帧大小
    if (read_rc != camera_read_result::kOk)
    {
        *out_length = frameSize;
        return read_rc;
    }
    if (FrameDescArray[bufIdx].base == nullptr)
    {
        std::cerr << "Mapped buffer base is null for index: " << bufIdx << std::endl;
        if (requeue_buffer(&FrameDescArray[bufIdx]) != 0)
        {
            std::cerr << "Failed to requeue buffer after null mapped buffer" << std::endl;
        }
        return camera_read_result::kFatal;
    }

    FrameDescArray[bufIdx].payloadSize = frameSize;
    *out_length                        = frameSize;
    CurrentFrameDesc = &FrameDescArray[bufIdx];  // 记录当前帧描述，供外部解码直接使用
    return camera_read_result::kOk;
}

int V4L2_Camera::dequeue_buffer(unsigned int& BufIdx, unsigned int& bufferSize,
                                const size_t maxBufferSize)
{
    bufferSize = 0;

    // 使用 poll 来实现非阻塞等待，避免直接调用 DQBUF 可能导致的长时间阻塞
    struct pollfd pfd
    {};
    pfd.fd             = camera_fd;
    pfd.events         = POLLIN | POLLPRI;  // 等待可读事件
    // 用较短的切片轮询，使采集线程能在切换摄像头/退出时及时回到主循环检查停止标志；
    // 只有当连续无帧的累计时长超过 kDequeueTimeoutMs 才判定为相机超时（致命，触发自愈回退）。
    const int poll_ret = poll(&pfd, 1, camera_params::kPollSliceMs);
    if (poll_ret == 0)  // 本切片无事件
    {
        poll_accum_ms_ += camera_params::kPollSliceMs;
        if (poll_accum_ms_ >= camera_params::kDequeueTimeoutMs)
        {
            std::cerr << "Dequeue timeout after " << poll_accum_ms_ << " ms" << std::endl;
            poll_accum_ms_ = 0;
            return camera_read_result::kTimeout;
        }
        return camera_read_result::kRetryable;  // 让采集线程回到循环顶部（可响应停止）
    }
    if (poll_ret < 0)  // poll 出错，可能是中断或其他错误，根据 errno 判断是否重试
    {
        if (errno == EINTR || errno == EAGAIN)
        {
            return camera_read_result::kRetryable;
        }
        std::cerr << "Failed to poll camera fd before dequeue: " << std::strerror(errno)
                  << std::endl;
        return camera_read_result::kFatal;
    }
    if (pfd.revents & (POLLERR | POLLHUP |
                       POLLNVAL))  // 监测异常事件，避免在异常状态下调用 DQBUF 导致更严重的问题
    {
        std::cerr << "Poll reported camera fd abnormal revents=0x" << std::hex << pfd.revents
                  << std::dec << std::endl;
        return camera_read_result::kFatal;
    }
    if ((pfd.revents & (POLLIN | POLLPRI)) ==
        0)  // 没有可读事件，可能是误报或其他事件，安全起见不调用 DQBUF
    {
        return camera_read_result::kRetryable;
    }
    /**
     * 在确认有可读事件后，安全地调用 DQBUF 来获取帧数据。即使在极少数情况下 poll 可能误报，但 DQBUF
     * 的错误处理也足够健壮，可以通过 errno 判断是否需要重试或处理错误。
     */
    struct v4l2_buffer buffer
    {};
    struct v4l2_plane planes[VIDEO_MAX_PLANES]
    {};
    buffer.type   = captureBufferType;
    buffer.memory = captureMemoryType;
    if (isMultiPlanarCapture)
    {
        buffer.m.planes = planes;
        buffer.length   = VIDEO_MAX_PLANES;
    }
    if (ioctl(camera_fd, VIDIOC_DQBUF, &buffer) < 0)  // 从内核队列中取出一帧数据
    {
        if (errno == EINTR || errno == EAGAIN)
        {
            return camera_read_result::kRetryable;
        }
        std::cerr << "Failed to dequeue buffer: " << std::strerror(errno) << std::endl;
        return camera_read_result::kFatal;
    }
    if (static_cast<int>(buffer.index) >= this->NumBuffers)
    {
        std::cerr << "Invalid buffer index dequeued: " << buffer.index << std::endl;
        return camera_read_result::kFatal;
    }
    if (isMultiPlanarCapture && buffer.length < 1)
    {
        std::cerr << "Invalid mplane dequeued buffer, plane count=" << buffer.length << std::endl;
        return camera_read_result::kFatal;
    }

    poll_accum_ms_ = 0;  // 成功取到一帧，清零无帧累计

    FrameDesc& frame_desc  = FrameDescArray[buffer.index];
    int64_t    frame_ts_us = timeval_to_us(buffer.timestamp);
    if (frame_ts_us <= 0)
    {
        frame_ts_us = get_monotonic_time_us();
    }
    if (frame_ts_us <= 0)
    {
        frame_ts_us = 0;
    }

    // mplane 情况下，有效载荷在 plane[0].bytesused；single-plane 仍用 buffer.bytesused。
    const size_t bytes_used = [&]() -> size_t
    {
        if (!isMultiPlanarCapture)
        {
            return static_cast<size_t>(buffer.bytesused);
        }
        if (buffer.length < 1)
        {
            return 0;
        }
        return static_cast<size_t>(buffer.m.planes[0].bytesused);
    }();

    if (frame_desc.fd >= 0 && !dma_buf_sync_read(frame_desc.fd, true))
    {
        std::cerr << "Failed to start dma-buf cpu read sync for buffer " << buffer.index << ": "
                  << std::strerror(errno) << std::endl;
        return camera_read_result::kFatal;
    }
    frame_desc.cpuSyncReadActive = (frame_desc.fd >= 0);

    bufferSize        = static_cast<unsigned int>(bytes_used);
    frame_desc.pts_us = frame_ts_us;
    frame_desc.dts_us = frame_ts_us;
    if (bytes_used > maxBufferSize)
    {
        std::cerr << "Buffer size exceeds max allowed size: " << bytes_used << " > "
                  << maxBufferSize << std::endl;

        if (frame_desc.cpuSyncReadActive)
        {
            if (!dma_buf_sync_read(frame_desc.fd, false))
            {
                std::cerr << "Failed to end dma-buf cpu read sync for oversized buffer "
                          << buffer.index << ": " << std::strerror(errno) << std::endl;
            }
            frame_desc.cpuSyncReadActive = false;
        }

        if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)  // 将帧重新入队
        {
            std::cerr << "Failed to requeue buffer: " << std::strerror(errno) << std::endl;
        }
        return camera_read_result::kFatal;
    }
    BufIdx     = buffer.index;
    bufferSize = static_cast<unsigned int>(bytes_used);
    return camera_read_result::kOk;
}

int V4L2_Camera::requeue_buffer(FrameDesc* frame_desc)
{
    if (frame_desc == nullptr)
    {
        std::cerr << "Invalid frame descriptor for requeue: null pointer" << std::endl;
        return -1;
    }

    const int idx = frame_desc->index;
    if (idx < 0 || idx >= NumBuffers)
    {
        std::cerr << "Invalid buffer index for requeue: " << idx << std::endl;
        return -1;
    }
    const auto buf_idx = static_cast<unsigned int>(idx);

    if (frame_desc->cpuSyncReadActive && frame_desc->fd >= 0)
    {
        if (!dma_buf_sync_read(frame_desc->fd, false))
        {
            std::cerr << "Failed to end dma-buf cpu read sync for buffer " << buf_idx << ": "
                      << std::strerror(errno) << std::endl;
            return -1;
        }
        frame_desc->cpuSyncReadActive = false;
    }

    struct v4l2_buffer buffer
    {};
    struct v4l2_plane planes[VIDEO_MAX_PLANES]
    {};
    buffer.index  = buf_idx;
    buffer.type   = captureBufferType;  // single/mplane
    buffer.memory = captureMemoryType;  // DMABUF/MMAP
    if (captureMemoryType == V4L2_MEMORY_DMABUF)
    {
        buffer.m.fd   = FrameDescArray[buf_idx].fd;
        buffer.length = static_cast<__u32>(FrameDescArray[buf_idx].Length);
    }
    else
    {
        if (isMultiPlanarCapture)
        {
            buffer.m.planes = planes;
            buffer.length   = capturePlaneCount;
            // 与 init_camera_buffer 中 QBUF 要求一致，补齐 plane 长度信息。
            planes[0].length = static_cast<__u32>(FrameDescArray[buf_idx].Length);
        }
        else
        {
            buffer.length = static_cast<__u32>(FrameDescArray[buf_idx].Length);
        }
    }
    if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)  // 将帧重新入队
    {
        std::cerr << "Failed to requeue buffer: " << std::strerror(errno) << std::endl;
        return -1;
    }

    frame_desc->payloadSize             = 0;
    frame_desc->pts_us                  = -1;
    frame_desc->dts_us                  = -1;
    FrameDescArray[buf_idx].payloadSize = 0;
    FrameDescArray[buf_idx].pts_us      = -1;
    FrameDescArray[buf_idx].dts_us      = -1;
    if (CurrentFrameDesc == frame_desc || CurrentFrameDesc == &FrameDescArray[buf_idx])
    {
        CurrentFrameDesc = nullptr;
    }

    return 0;
}

int V4L2_Camera::close_camera()
{
    if (camera_fd < 0)
    {
        std::cout << "Camera already closed" << std::endl;
        return 0;
    }

    std::cout << "Closing camera device: fd=" << camera_fd << std::endl;
    if (close(camera_fd) != 0)
    {
        std::cerr << "Failed to close camera fd=" << camera_fd << ": " << std::strerror(errno)
                  << std::endl;
        return -1;
    }

    camera_fd          = -1;
    isStreamOn         = false;
    isStreamingSupport = false;
    isMultiPlanarCapture = false;
    captureBufferType    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    captureMemoryType    = V4L2_MEMORY_DMABUF;
    capturePlaneCount    = 1;
    return 0;
}
