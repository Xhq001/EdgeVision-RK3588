#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class NpuDetectorPool;

// 一个可用采集设备的描述。
struct CameraInfo
{
    std::string device;   // /dev/videoX
    std::string card;     // 驱动上报的 card 名
    std::string formats;  // 逗号分隔的受支持像素格式（fourcc）
};

// 进程内单例控制器：承接网页平台的控制指令（切摄像头 / 开关识别 / 查状态），
// 并在主循环与流水线之间传递"退出 / 重启（换摄像头）"信号。
// ZLM 的 HTTP 钩子把 /api/* 请求转发到本类 HandleApi。
class AppController
{
   public:
    static AppController& Instance();

    void BindPool(NpuDetectorPool* pool) { pool_ = pool; }

    // ---- 生命周期信号（主循环 / 流水线读取）----
    bool exit_requested() const { return exit_requested_.load(); }
    void request_exit() { exit_requested_.store(true); }
    bool restart_requested() const { return restart_requested_.load(); }
    void clear_restart() { restart_requested_.store(false); }

    std::string current_device();
    void        set_current_device(const std::string& dev);
    std::string pending_device();

    // ---- 识别开关 ----
    bool yolo_enabled() const { return yolo_enabled_.load(); }
    void set_yolo_enabled(bool en) { yolo_enabled_.store(en); }
    bool yolo_available() const { return yolo_available_.load(); }
    void set_yolo_available(bool a) { yolo_available_.store(a); }

    // ---- 流水线上报的统计（供 /api/status）----
    void set_stream_stats(double video_fps, int width, int height, uint64_t dropped);
    void set_model_name(const std::string& m);

    // ZLM 钩子入口：处理 /api/* 请求，返回 true 表示已处理并填好 out_json。
    bool HandleApi(const std::string& method, const std::string& path, const std::string& body,
                   std::string& out_json);

    // 扫描 /dev/video*，返回带受支持采集格式的设备列表。
    static std::vector<CameraInfo> EnumerateCameras();

   private:
    AppController() = default;

    std::string BuildStatusJson();
    std::string BuildCamerasJson();
    bool        IsValidCaptureDevice(const std::string& dev);

    NpuDetectorPool* pool_ = nullptr;

    std::atomic_bool exit_requested_{false};
    std::atomic_bool restart_requested_{false};
    std::atomic_bool yolo_enabled_{false};
    std::atomic_bool yolo_available_{false};

    std::mutex  dev_mutex_;
    std::string current_device_;
    std::string pending_device_;

    std::mutex  stat_mutex_;
    double      video_fps_ = 0.0;
    int         width_     = 0;
    int         height_    = 0;
    uint64_t    dropped_   = 0;
    std::string model_name_;
};

#endif  // APP_CONTROLLER_H
