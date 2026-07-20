#include "app_controller.h"
#include "common_utils.h"

#include "npu_detector_pool.h"

#include <linux/videodev2.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace
{
// 本流水线可处理的采集像素格式（其余如 Bayer RG10 无法直接进链路，过滤掉）。
bool IsSupportedFourcc(uint32_t f)
{
    switch (f)
    {
        case V4L2_PIX_FMT_MJPEG:
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
        case V4L2_PIX_FMT_NV16:
        case V4L2_PIX_FMT_NV61:
            return true;
        default:
            return false;
    }
}



// 极简 JSON 取值：从受控前端发来的 body 里提取 "key": <value>。
bool JsonGetBool(const std::string& body, const std::string& key, bool* out)
{
    const std::string pat = "\"" + key + "\"";
    size_t            p   = body.find(pat);
    if (p == std::string::npos)
    {
        return false;
    }
    p = body.find(':', p + pat.size());
    if (p == std::string::npos)
    {
        return false;
    }
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t'))
    {
        ++p;
    }
    if (body.compare(p, 4, "true") == 0 || body.compare(p, 1, "1") == 0)
    {
        *out = true;
        return true;
    }
    if (body.compare(p, 5, "false") == 0 || body.compare(p, 1, "0") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

bool JsonGetString(const std::string& body, const std::string& key, std::string* out)
{
    const std::string pat = "\"" + key + "\"";
    size_t            p   = body.find(pat);
    if (p == std::string::npos)
    {
        return false;
    }
    p = body.find(':', p + pat.size());
    if (p == std::string::npos)
    {
        return false;
    }
    p = body.find('"', p);
    if (p == std::string::npos)
    {
        return false;
    }
    ++p;
    const size_t end = body.find('"', p);
    if (end == std::string::npos)
    {
        return false;
    }
    *out = body.substr(p, end - p);
    return true;
}
}  // namespace

AppController& AppController::Instance()
{
    static AppController inst;
    return inst;
}

std::string AppController::current_device()
{
    std::lock_guard<std::mutex> lock(dev_mutex_);
    return current_device_;
}

void AppController::set_current_device(const std::string& dev)
{
    std::lock_guard<std::mutex> lock(dev_mutex_);
    current_device_ = dev;
}

std::string AppController::pending_device()
{
    std::lock_guard<std::mutex> lock(dev_mutex_);
    return pending_device_;
}

void AppController::set_stream_stats(double video_fps, int width, int height, uint64_t dropped)
{
    std::lock_guard<std::mutex> lock(stat_mutex_);
    video_fps_ = video_fps;
    width_     = width;
    height_    = height;
    dropped_   = dropped;
}

void AppController::set_model_name(const std::string& m)
{
    std::lock_guard<std::mutex> lock(stat_mutex_);
    model_name_ = m;
}

std::vector<CameraInfo> AppController::EnumerateCameras()
{
    std::vector<CameraInfo> result;

    // 排除清单：默认排除 ISP 的 iqtool 路径（非采集/推流用），
    // 另可通过环境变量 CAMERA_EXCLUDE 追加设备节点（逗号分隔），
    // 例如本板 selfpath 不出帧：CAMERA_EXCLUDE=/dev/video12,/dev/video14
    std::vector<std::string> exclude_devs;
    if (const char* ex = std::getenv("CAMERA_EXCLUDE"))
    {
        std::string       s(ex);
        std::stringstream ss(s);
        std::string       item;
        while (std::getline(ss, item, ','))
        {
            // 去首尾空白
            size_t a = item.find_first_not_of(" \t");
            size_t b = item.find_last_not_of(" \t");
            if (a != std::string::npos)
            {
                exclude_devs.push_back(item.substr(a, b - a + 1));
            }
        }
    }

    for (int i = 0; i < 64; ++i)
    {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/video%d", i);
        const int fd = ::open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0)
        {
            continue;
        }

        v4l2_capability cap{};
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
        {
            ::close(fd);
            continue;
        }
        const uint32_t caps =
            (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        const bool is_capture =
            (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
        if (!is_capture)
        {
            ::close(fd);
            continue;
        }

        const std::string card = reinterpret_cast<const char*>(cap.card);
        // 排除 ISP iqtool 路径（非采集口）与 CAMERA_EXCLUDE 指定的节点。
        if (card.find("iqtool") != std::string::npos)
        {
            ::close(fd);
            continue;
        }
        if (std::find(exclude_devs.begin(), exclude_devs.end(), std::string(path)) !=
            exclude_devs.end())
        {
            ::close(fd);
            continue;
        }

        // 枚举支持的格式，只保留能进本链路的设备。
        std::string    formats;
        bool           any_supported = false;
        const bool     mplane        = (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        const uint32_t buf_type      = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                              : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (uint32_t idx = 0;; ++idx)
        {
            v4l2_fmtdesc fmt{};
            fmt.index = idx;
            fmt.type  = buf_type;
            if (::ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0)
            {
                break;
            }
            if (IsSupportedFourcc(fmt.pixelformat))
            {
                any_supported = true;
                if (!formats.empty())
                {
                    formats += ",";
                }
                formats += util::FourccToString(fmt.pixelformat);
            }
        }
        ::close(fd);

        if (!any_supported)
        {
            continue;  // 例如纯 Bayer 的 rkcif 节点，跳过
        }

        CameraInfo info;
        info.device  = path;
        info.card    = card;
        info.formats = formats;
        result.push_back(std::move(info));
    }
    return result;
}

bool AppController::IsValidCaptureDevice(const std::string& dev)
{
    const auto cams = EnumerateCameras();
    return std::any_of(cams.begin(), cams.end(),
                       [&](const CameraInfo& c) { return c.device == dev; });
}

std::string AppController::BuildCamerasJson()
{
    const auto        cams    = EnumerateCameras();
    const std::string cur     = current_device();
    std::ostringstream os;
    os << "{\"code\":0,\"current\":\"" << util::JsonEscape(cur) << "\",\"cameras\":[";
    for (size_t i = 0; i < cams.size(); ++i)
    {
        if (i)
        {
            os << ",";
        }
        os << "{\"device\":\"" << util::JsonEscape(cams[i].device) << "\",\"card\":\""
           << util::JsonEscape(cams[i].card) << "\",\"formats\":\"" << util::JsonEscape(cams[i].formats)
           << "\"}";
    }
    os << "]}";
    return os.str();
}

std::string AppController::BuildStatusJson()
{
    double      video_fps = 0.0;
    int         w = 0, h = 0;
    uint64_t    dropped = 0;
    std::string model;
    {
        std::lock_guard<std::mutex> lock(stat_mutex_);
        video_fps = video_fps_;
        w         = width_;
        h         = height_;
        dropped   = dropped_;
        model     = model_name_;
    }

    std::ostringstream os;
    os << "{\"code\":0";
    os << ",\"device\":\"" << util::JsonEscape(current_device()) << "\"";
    os << ",\"yolo_enabled\":" << (yolo_enabled() ? "true" : "false");
    os << ",\"yolo_available\":" << (yolo_available() ? "true" : "false");
    os << ",\"model\":\"" << util::JsonEscape(model) << "\"";
    os << ",\"width\":" << w << ",\"height\":" << h;
    os << ",\"video_fps\":" << video_fps;
    os << ",\"dropped_frames\":" << dropped;

    if (pool_ != nullptr)
    {
        os << ",\"npu_instances\":" << pool_->instances();
        os << ",\"detect_fps\":" << pool_->detect_fps();
        os << ",\"infer_ok\":" << pool_->infer_ok();
        os << ",\"infer_fail\":" << pool_->infer_fail();
        os << ",\"submit_dropped\":" << pool_->submit_dropped();
        os << ",\"inflight\":" << pool_->inflight();
    }
    os << "}";
    return os.str();
}

bool AppController::HandleApi(const std::string& method, const std::string& path,
                             const std::string& body, std::string& out_json)
{
    if (path == "/api/status")
    {
        out_json = BuildStatusJson();
        return true;
    }
    if (path == "/api/cameras")
    {
        out_json = BuildCamerasJson();
        return true;
    }
    if (path == "/api/yolo")
    {
        if (method != "POST")
        {
            out_json = "{\"code\":-1,\"msg\":\"use POST\"}";
            return true;
        }
        if (!yolo_available())
        {
            out_json = "{\"code\":-1,\"msg\":\"yolo model not loaded\"}";
            return true;
        }
        bool en = false;
        if (!JsonGetBool(body, "enable", &en))
        {
            out_json = "{\"code\":-1,\"msg\":\"missing enable field\"}";
            return true;
        }
        set_yolo_enabled(en);
        out_json = std::string("{\"code\":0,\"yolo_enabled\":") + (en ? "true" : "false") + "}";
        return true;
    }
    if (path == "/api/camera")
    {
        if (method != "POST")
        {
            out_json = "{\"code\":-1,\"msg\":\"use POST\"}";
            return true;
        }
        std::string dev;
        if (!JsonGetString(body, "device", &dev) || dev.empty())
        {
            out_json = "{\"code\":-1,\"msg\":\"missing device field\"}";
            return true;
        }
        if (dev == current_device())
        {
            out_json = "{\"code\":0,\"msg\":\"already active\"}";
            return true;
        }
        if (!IsValidCaptureDevice(dev))
        {
            out_json = "{\"code\":-1,\"msg\":\"device not available or unsupported\"}";
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(dev_mutex_);
            pending_device_ = dev;
        }
        restart_requested_.store(true);
        out_json = std::string("{\"code\":0,\"switching_to\":\"") + util::JsonEscape(dev) + "\"}";
        return true;
    }
    return false;  // 非 /api/* 已知路由
}
