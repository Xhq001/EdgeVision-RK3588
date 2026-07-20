#include "yolo_npu.h"

#include <rockchip/mpp_frame.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>

#include <im2d.h>
#include <im2d_buffer.h>

namespace
{
constexpr size_t kMaxDetectCount = 128;

// 串行化 RGA 前处理操作：librga 的 virtual-address 路径在多线程并发调用时会死锁，
// 多实例池的 worker 会并发进入 Preprocess，这里用一把锁串行化 RGA(resize/cvtcolor)。
// rknn_run 仍在各自核心并行执行，前处理很轻(~2-3ms)，串行化几乎不影响整体吞吐。
std::mutex g_rga_op_mutex;

// 热点路径分支预测提示（仅错误分支用到）。
#if defined(__GNUC__) || defined(__clang__)
#define YOLO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define YOLO_UNLIKELY(x) (x)
#endif

int MapMppToRgaFormat(uint32_t mpp_fmt)
{
    const uint32_t base_fmt = (mpp_fmt & MPP_FRAME_FMT_MASK);
    switch (base_fmt)
    {
        case MPP_FMT_YUV420SP:
            return RK_FORMAT_YCbCr_420_SP;  // NV12
        case MPP_FMT_YUV420SP_VU:
            return RK_FORMAT_YCrCb_420_SP;  // NV21
        case MPP_FMT_YUV422SP:
            return RK_FORMAT_YCbCr_422_SP;  // NV16
        case MPP_FMT_YUV422SP_VU:
            return RK_FORMAT_YCrCb_422_SP;  // NV61
        case MPP_FMT_YUV422_YUYV:
            return RK_FORMAT_YUYV_422;
        case MPP_FMT_YUV422_UYVY:
            return RK_FORMAT_UYVY_422;
        default:
            return -1;
    }
}

int ClampInt(int val, int min_val, int max_val)
{
    if (val < min_val)
    {
        return min_val;
    }
    if (val > max_val)
    {
        return max_val;
    }
    return val;
}

float DequantI8ToF32(int8_t qnt, int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

int8_t QuantF32ToI8(float f32, int32_t zp, float scale)
{
    const float inv_scale = (scale == 0.0f) ? 0.0f : (1.0f / scale);
    const float dst       = f32 * inv_scale + static_cast<float>(zp);
    const int   clipped =
        ClampInt(static_cast<int>(std::nearbyint(dst)), std::numeric_limits<int8_t>::min(),
                 std::numeric_limits<int8_t>::max());
    return static_cast<int8_t>(clipped);
}

float IoU(const std::vector<float>& boxes, int i, int j)
{
    const float x1_i = boxes[static_cast<size_t>(i) * 4u + 0u];
    const float y1_i = boxes[static_cast<size_t>(i) * 4u + 1u];
    const float x2_i = x1_i + boxes[static_cast<size_t>(i) * 4u + 2u];
    const float y2_i = y1_i + boxes[static_cast<size_t>(i) * 4u + 3u];

    const float x1_j = boxes[static_cast<size_t>(j) * 4u + 0u];
    const float y1_j = boxes[static_cast<size_t>(j) * 4u + 1u];
    const float x2_j = x1_j + boxes[static_cast<size_t>(j) * 4u + 2u];
    const float y2_j = y1_j + boxes[static_cast<size_t>(j) * 4u + 3u];

    const float inter_w = std::max(0.0f, std::min(x2_i, x2_j) - std::max(x1_i, x1_j) + 1.0f);
    const float inter_h = std::max(0.0f, std::min(y2_i, y2_j) - std::max(y1_i, y1_j) + 1.0f);
    const float inter   = inter_w * inter_h;

    const float area_i = (x2_i - x1_i + 1.0f) * (y2_i - y1_i + 1.0f);
    const float area_j = (x2_j - x1_j + 1.0f) * (y2_j - y1_j + 1.0f);
    const float uni    = area_i + area_j - inter;
    return (uni <= 0.0f) ? 0.0f : (inter / uni);
}

void ReleaseRgaHandle(rga_buffer_handle_t* handle)
{
    if (!handle)
    {
        return;
    }
    if (*handle)
    {
        (void)releasebuffer_handle(*handle);
        *handle = 0;
    }
}

int BuildRgaBuffer(const IO_FD_t* buffer, uint32_t width, uint32_t height, uint32_t h_stride,
                   uint32_t v_stride, int rga_format, bool prefer_virtual, rga_buffer_t* out,
                   rga_buffer_handle_t* handle)
{
    if (!buffer || !out || !handle || width == 0 || height == 0 || h_stride == 0 || v_stride == 0)
    {
        return -1;
    }

    *handle = 0;
    if (!prefer_virtual && buffer->fd >= 0 && buffer->size > 0)
    {
        *handle = importbuffer_fd(buffer->fd, static_cast<int>(buffer->size));
        if (*handle)
        {
            *out = wrapbuffer_handle(*handle, static_cast<int>(width), static_cast<int>(height),
                                     rga_format, static_cast<int>(h_stride),
                                     static_cast<int>(v_stride));
            return 0;
        }
    }

    if (buffer->base && buffer->size > 0)
    {
        *out = wrapbuffer_virtualaddr_t(buffer->base, static_cast<int>(width),
                                        static_cast<int>(height), static_cast<int>(h_stride),
                                        static_cast<int>(v_stride), rga_format);
        return 0;
    }

    if (buffer->fd >= 0 && buffer->size > 0)
    {
        *handle = importbuffer_fd(buffer->fd, static_cast<int>(buffer->size));
        if (*handle)
        {
            *out = wrapbuffer_handle(*handle, static_cast<int>(width), static_cast<int>(height),
                                     rga_format, static_cast<int>(h_stride),
                                     static_cast<int>(v_stride));
            return 0;
        }
    }

    return -1;
}
}  // namespace

int YoloNpuInstance::LoadModel(const std::string& model_path, int core_id, rknn_context* dup_from)
{
    int ret = 0;
    if (dup_from != nullptr && *dup_from != 0)
    {
        // 多实例池：从实例 0 的上下文复制，共享模型权重，显著省内存。
        ret = rknn_dup_context(dup_from, &rknn_ctx_);
        if (ret != RKNN_SUCC)
        {
            std::cerr << "[YOLO] rknn_dup_context failed, ret=" << ret
                      << ", fallback to independent rknn_init" << std::endl;
            rknn_ctx_ = 0;
        }
    }

    if (rknn_ctx_ == 0)
    {
        std::ifstream ifs(model_path, std::ios::binary);
        if (!ifs.is_open())
        {
            std::cerr << "[YOLO] Failed to open model: " << model_path << std::endl;
            return -1;
        }
        std::vector<char> model_data((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
        if (model_data.empty())
        {
            std::cerr << "[YOLO] Model file is empty: " << model_path << std::endl;
            return -1;
        }

        ret = rknn_init(&rknn_ctx_, model_data.data(), model_data.size(), 0, nullptr);
        if (ret != RKNN_SUCC)
        {
            std::cerr << "[YOLO] rknn_init failed, ret=" << ret << std::endl;
            rknn_ctx_ = 0;
            return -1;
        }
    }

    // 绑定 NPU 核心：0/1/2 分别绑定单核（多实例池用），<0 走 AUTO（单实例默认）。
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    const char*    core_desc = "AUTO";
    switch (core_id)
    {
        case 0:
            core_mask = RKNN_NPU_CORE_0;
            core_desc = "CORE_0";
            break;
        case 1:
            core_mask = RKNN_NPU_CORE_1;
            core_desc = "CORE_1";
            break;
        case 2:
            core_mask = RKNN_NPU_CORE_2;
            core_desc = "CORE_2";
            break;
        default:
            core_mask = RKNN_NPU_CORE_AUTO;
            core_desc = "AUTO";
            break;
    }
    ret = rknn_set_core_mask(rknn_ctx_, core_mask);
    if (ret != RKNN_SUCC)
    {
        std::cerr << "[YOLO] rknn_set_core_mask(" << core_desc << ") failed, ret=" << ret
                  << ", keep default" << std::endl;
    }
    else
    {
        std::cout << "[YOLO] NPU core mask set to " << core_desc << std::endl;
    }

    return 0;
}

int YoloNpuInstance::QueryModelMeta()
{
    int ret = rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret != RKNN_SUCC)
    {
        std::cerr << "[YOLO] rknn_query RKNN_QUERY_IN_OUT_NUM failed, ret=" << ret << std::endl;
        return -1;
    }
    if (io_num_.n_input < 1 || io_num_.n_output < 1)
    {
        std::cerr << "[YOLO] Invalid model io num, n_input=" << io_num_.n_input
                  << ", n_output=" << io_num_.n_output << std::endl;
        return -1;
    }

    input_attrs_.assign(io_num_.n_input, rknn_tensor_attr{});
    output_attrs_.assign(io_num_.n_output, rknn_tensor_attr{});
    for (uint32_t i = 0; i < io_num_.n_input; ++i)
    {
        input_attrs_[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            std::cerr << "[YOLO] query input attr failed, index=" << i << ", ret=" << ret
                      << std::endl;
            return -1;
        }
    }
    for (uint32_t i = 0; i < io_num_.n_output; ++i)
    {
        output_attrs_[i].index = i;
        ret =
            rknn_query(rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            std::cerr << "[YOLO] query output attr failed, index=" << i << ", ret=" << ret
                      << std::endl;
            return -1;
        }
    }

    const rknn_tensor_attr& input0 = input_attrs_[0];
    if (input0.n_dims < 4)
    {
        std::cerr << "[YOLO] Unsupported input dims: " << input0.n_dims << std::endl;
        return -1;
    }
    if (input0.fmt == RKNN_TENSOR_NCHW)
    {
        model_channel_ = static_cast<int>(input0.dims[1]);
        model_height_  = static_cast<int>(input0.dims[2]);
        model_width_   = static_cast<int>(input0.dims[3]);
    }
    else
    {
        model_height_  = static_cast<int>(input0.dims[1]);
        model_width_   = static_cast<int>(input0.dims[2]);
        model_channel_ = static_cast<int>(input0.dims[3]);
    }
    if (model_width_ <= 0 || model_height_ <= 0 || model_channel_ != 3)
    {
        std::cerr << "[YOLO] Unsupported model input shape, w=" << model_width_
                  << ", h=" << model_height_ << ", c=" << model_channel_ << std::endl;
        return -1;
    }

    if (output_attrs_[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
        output_attrs_[0].type == RKNN_TENSOR_INT8)
    {
        is_quant_ = true;
    }
    else
    {
        is_quant_ = false;
    }

    if (io_num_.n_output % 3 != 0)
    {
        std::cerr << "[YOLO] Unsupported output count: " << io_num_.n_output
                  << " (expect 3*N for detection model)" << std::endl;
        return -1;
    }
    const int output_per_branch = static_cast<int>(io_num_.n_output / 3);
    if (output_per_branch == 2)
    {
        model_kind_ = ModelKind::kYoloV10Detection;
    }
    else if (output_per_branch == 3)
    {
        model_kind_ = ModelKind::kYoloV8Detection;
    }
    else
    {
        std::cerr << "[YOLO] Unsupported output_per_branch=" << output_per_branch
                  << ", only detection models are supported now" << std::endl;
        return -1;
    }

    const int score_idx =
        (output_per_branch >= 2) ? 1 : -1;  // box0=0, score0=1 对于 detection 模型成立
    if (score_idx < 0 || output_attrs_.size() <= static_cast<size_t>(score_idx))
    {
        return -1;
    }
    class_count_ = static_cast<int>(output_attrs_[static_cast<size_t>(score_idx)].dims[1]);
    if (class_count_ <= 0)
    {
        std::cerr << "[YOLO] Invalid class count from output tensor: " << class_count_ << std::endl;
        return -1;
    }

    inputs_.assign(io_num_.n_input, rknn_input{});
    outputs_.assign(io_num_.n_output, rknn_output{});
    inputs_[0].index = 0;
    inputs_[0].type  = RKNN_TENSOR_UINT8;
    inputs_[0].fmt   = RKNN_TENSOR_NHWC;
    inputs_[0].size  = static_cast<uint32_t>(model_width_ * model_height_ * model_channel_);
    inputs_[0].buf   = nullptr;

    model_input_.assign(static_cast<size_t>(inputs_[0].size), 114u);
    return 0;
}

int YoloNpuInstance::LoadLabels(const std::string& label_path)
{
    labels_.clear();
    if (label_path.empty())
    {
        return 0;
    }

    std::ifstream ifs(label_path);
    if (!ifs.is_open())
    {
        std::cerr << "[YOLO] Failed to open label file: " << label_path << std::endl;
        return -1;
    }
    std::string line;
    while (std::getline(ifs, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            labels_.push_back(line);
        }
    }
    std::cout << "[YOLO] Loaded labels: " << labels_.size() << std::endl;
    return 0;
}

int YoloNpuInstance::Init(const std::string& model_path, const std::string& label_path, int core_id,
                          rknn_context* dup_from)
{
    Deinit();

    if (LoadModel(model_path, core_id, dup_from) != 0)
    {
        Deinit();
        return -1;
    }
    if (QueryModelMeta() != 0)
    {
        Deinit();
        return -1;
    }
    (void)LoadLabels(label_path);

    initialized_ = true;
    std::cout << "[YOLO] init done, model input=" << model_width_ << "x" << model_height_
              << ", classes=" << class_count_ << ", quant=" << (is_quant_ ? "true" : "false")
              << std::endl;
    return 0;
}

void YoloNpuInstance::Deinit()
{
    if (rknn_ctx_ != 0)
    {
        (void)rknn_destroy(rknn_ctx_);
        rknn_ctx_ = 0;
    }
    initialized_ = false;
    is_quant_    = false;
    model_kind_  = ModelKind::kUnsupported;
    model_width_ = 0;
    model_height_ = 0;
    model_channel_ = 0;
    class_count_ = 0;
    io_num_      = {};
    inputs_.clear();
    outputs_.clear();
    input_attrs_.clear();
    output_attrs_.clear();
    labels_.clear();
    model_input_.clear();
    resized_nv12_.clear();
    resized_rgb_.clear();
}

const char* YoloNpuInstance::GetLabelName(int cls_id) const
{
    if (cls_id >= 0 && static_cast<size_t>(cls_id) < labels_.size())
    {
        return labels_[static_cast<size_t>(cls_id)].c_str();
    }
    return "unknown";
}

int YoloNpuInstance::Preprocess(const IO_FD_t* input_nv12, LetterBox* letter_box)
{
    if (!input_nv12 || !letter_box)
    {
        return -1;
    }
    if (input_nv12->width == 0 || input_nv12->height == 0 || input_nv12->hor_stride == 0 ||
        input_nv12->ver_stride == 0)
    {
        return -1;
    }
    if ((input_nv12->format & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP)
    {
        std::cerr << "[YOLO] only NV12 input is supported now, fmt=" << input_nv12->format
                  << std::endl;
        return -1;
    }

    const int src_w = static_cast<int>(input_nv12->width);
    const int src_h = static_cast<int>(input_nv12->height);
    const float scale_w = static_cast<float>(model_width_) / static_cast<float>(src_w);
    const float scale_h = static_cast<float>(model_height_) / static_cast<float>(src_h);
    const float scale   = std::min(scale_w, scale_h);
    int         resized_w =
        std::max(2, static_cast<int>(std::floor(static_cast<float>(src_w) * scale + 0.5f)));
    int resized_h =
        std::max(2, static_cast<int>(std::floor(static_cast<float>(src_h) * scale + 0.5f)));
    // NV12 分辨率需要偶数。
    if ((resized_w & 1) != 0)
    {
        resized_w -= 1;
    }
    if ((resized_h & 1) != 0)
    {
        resized_h -= 1;
    }
    resized_w = std::max(2, resized_w);
    resized_h = std::max(2, resized_h);

    letter_box->scale     = scale;
    letter_box->resized_w = resized_w;
    letter_box->resized_h = resized_h;
    letter_box->x_pad     = (model_width_ - resized_w) / 2;
    letter_box->y_pad     = (model_height_ - resized_h) / 2;

    std::fill(model_input_.begin(), model_input_.end(), 114u);
    resized_nv12_.resize(static_cast<size_t>(resized_w) * static_cast<size_t>(resized_h) * 3u / 2u);
    resized_rgb_.resize(static_cast<size_t>(resized_w) * static_cast<size_t>(resized_h) * 3u);

    int src_rga_fmt = MapMppToRgaFormat(input_nv12->format);
    if (src_rga_fmt < 0)
    {
        return -1;
    }

    IO_FD_t resized_nv12_desc{};
    resized_nv12_desc.fd         = -1;
    resized_nv12_desc.base       = resized_nv12_.data();
    resized_nv12_desc.size       = resized_nv12_.size();
    resized_nv12_desc.format     = MPP_FMT_YUV420SP;
    resized_nv12_desc.width      = static_cast<uint32_t>(resized_w);
    resized_nv12_desc.height     = static_cast<uint32_t>(resized_h);
    resized_nv12_desc.hor_stride = static_cast<uint32_t>(resized_w);
    resized_nv12_desc.ver_stride = static_cast<uint32_t>(resized_h);

    IO_FD_t resized_rgb_desc{};
    resized_rgb_desc.fd         = -1;
    resized_rgb_desc.base       = resized_rgb_.data();
    resized_rgb_desc.size       = resized_rgb_.size();
    resized_rgb_desc.width      = static_cast<uint32_t>(resized_w);
    resized_rgb_desc.height     = static_cast<uint32_t>(resized_h);
    resized_rgb_desc.hor_stride = static_cast<uint32_t>(resized_w);
    resized_rgb_desc.ver_stride = static_cast<uint32_t>(resized_h);

    rga_buffer_t        src_buf{};
    rga_buffer_t        resized_nv12_buf{};
    rga_buffer_t        resized_rgb_buf{};
    rga_buffer_handle_t src_handle         = 0;
    rga_buffer_handle_t resized_nv12_handle = 0;
    rga_buffer_handle_t resized_rgb_handle = 0;

    // librga 要求同一次任务中的 src/dst 资源类型一致（都 handle 或都 virtual）。
    // 当前主链路里的 RGA 输出带有可用的 mmap 虚拟地址，因此这里统一走 virtual，
    // 避免出现 [src,dst]=[handle,virtual] 导致的报错。
    std::lock_guard<std::mutex> rga_lock(g_rga_op_mutex);
    const bool prefer_virtual = (input_nv12->base != nullptr);
    if (BuildRgaBuffer(input_nv12, input_nv12->width, input_nv12->height, input_nv12->hor_stride,
                       input_nv12->ver_stride, src_rga_fmt, prefer_virtual, &src_buf,
                       &src_handle) != 0)
    {
        return -1;
    }
    if (BuildRgaBuffer(&resized_nv12_desc, resized_nv12_desc.width, resized_nv12_desc.height,
                       resized_nv12_desc.hor_stride, resized_nv12_desc.ver_stride,
                       RK_FORMAT_YCbCr_420_SP, prefer_virtual, &resized_nv12_buf,
                       &resized_nv12_handle) != 0)
    {
        ReleaseRgaHandle(&src_handle);
        return -1;
    }
    if (BuildRgaBuffer(&resized_rgb_desc, resized_rgb_desc.width, resized_rgb_desc.height,
                       resized_rgb_desc.hor_stride, resized_rgb_desc.ver_stride, RK_FORMAT_RGB_888,
                       prefer_virtual, &resized_rgb_buf, &resized_rgb_handle) != 0)
    {
        ReleaseRgaHandle(&src_handle);
        ReleaseRgaHandle(&resized_nv12_handle);
        return -1;
    }

    IM_STATUS status = imresize(src_buf, resized_nv12_buf);
    if (status == IM_STATUS_SUCCESS)
    {
        status = imcvtcolor(resized_nv12_buf, resized_rgb_buf, RK_FORMAT_YCbCr_420_SP,
                            RK_FORMAT_RGB_888);
    }

    ReleaseRgaHandle(&src_handle);
    ReleaseRgaHandle(&resized_nv12_handle);
    ReleaseRgaHandle(&resized_rgb_handle);

    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "[YOLO] preprocess RGA failed: " << imStrError_t(status) << std::endl;
        return -1;
    }

    const size_t src_stride = static_cast<size_t>(resized_w) * 3u;
    for (int y = 0; y < resized_h; ++y)
    {
        const size_t src_offset = static_cast<size_t>(y) * src_stride;
        const size_t dst_offset = (static_cast<size_t>(y + letter_box->y_pad) *
                                       static_cast<size_t>(model_width_) +
                                   static_cast<size_t>(letter_box->x_pad)) *
                                  3u;
        std::memcpy(model_input_.data() + dst_offset, resized_rgb_.data() + src_offset, src_stride);
    }
    return 0;
}

void YoloNpuInstance::ComputeDfl(const float* tensor, int dfl_len, float* box) const
{
    if (!tensor || !box || dfl_len <= 0)
    {
        return;
    }
    for (int b = 0; b < 4; ++b)
    {
        float exp_sum = 0.0f;
        float acc_sum = 0.0f;
        for (int i = 0; i < dfl_len; ++i)
        {
            exp_sum += std::exp(tensor[i + b * dfl_len]);
        }
        if (exp_sum <= 0.0f)
        {
            box[b] = 0.0f;
            continue;
        }
        for (int i = 0; i < dfl_len; ++i)
        {
            acc_sum += (std::exp(tensor[i + b * dfl_len]) / exp_sum) * static_cast<float>(i);
        }
        box[b] = acc_sum;
    }
}

int YoloNpuInstance::ProcessI8Branch(const rknn_output* outputs, int box_idx, int score_idx,
                                     int score_sum_idx, int grid_h, int grid_w, int stride,
                                     int dfl_len, std::vector<float>* boxes,
                                     std::vector<float>* scores, std::vector<int>* class_ids) const
{
    if (!outputs || !boxes || !scores || !class_ids)
    {
        return -1;
    }
    const int grid_len = grid_h * grid_w;
    if (grid_len <= 0)
    {
        return 0;
    }
    if (score_idx < 0 || static_cast<size_t>(score_idx) >= output_attrs_.size() ||
        static_cast<size_t>(box_idx) >= output_attrs_.size())
    {
        return -1;
    }

    const int8_t* box_tensor = static_cast<const int8_t*>(outputs[box_idx].buf);
    const int8_t* score_tensor = static_cast<const int8_t*>(outputs[score_idx].buf);
    if (!box_tensor || !score_tensor)
    {
        return -1;
    }
    const int32_t box_zp    = output_attrs_[static_cast<size_t>(box_idx)].zp;
    const float   box_scale = output_attrs_[static_cast<size_t>(box_idx)].scale;
    const int32_t score_zp  = output_attrs_[static_cast<size_t>(score_idx)].zp;
    const float   score_scale = output_attrs_[static_cast<size_t>(score_idx)].scale;

    const int8_t* score_sum_tensor = nullptr;
    int32_t       score_sum_zp     = 0;
    float         score_sum_scale  = 1.0f;
    if (score_sum_idx >= 0 && static_cast<size_t>(score_sum_idx) < output_attrs_.size())
    {
        score_sum_tensor = static_cast<const int8_t*>(outputs[score_sum_idx].buf);
        score_sum_zp     = output_attrs_[static_cast<size_t>(score_sum_idx)].zp;
        score_sum_scale  = output_attrs_[static_cast<size_t>(score_sum_idx)].scale;
    }

    const int8_t score_thres_i8 = QuantF32ToI8(conf_threshold_, score_zp, score_scale);
    const int8_t score_sum_thres_i8 = QuantF32ToI8(conf_threshold_, score_sum_zp, score_sum_scale);
    std::vector<float> before_dfl(static_cast<size_t>(dfl_len) * 4u, 0.0f);

    int valid = 0;
    for (int h = 0; h < grid_h; ++h)
    {
        for (int w = 0; w < grid_w; ++w)
        {
            const int cell = h * grid_w + w;
            if (score_sum_tensor && score_sum_tensor[cell] < score_sum_thres_i8)
            {
                continue;
            }

            int   best_cls   = -1;
            int8_t best_score = std::numeric_limits<int8_t>::min();
            int   offset     = cell;
            for (int c = 0; c < class_count_; ++c)
            {
                const int8_t score = score_tensor[offset];
                if (score > score_thres_i8 && score > best_score)
                {
                    best_score = score;
                    best_cls   = c;
                }
                offset += grid_len;
            }
            if (best_cls < 0)
            {
                continue;
            }

            offset = cell;
            for (int k = 0; k < dfl_len * 4; ++k)
            {
                before_dfl[static_cast<size_t>(k)] =
                    DequantI8ToF32(box_tensor[offset], box_zp, box_scale);
                offset += grid_len;
            }
            float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ComputeDfl(before_dfl.data(), dfl_len, box);

            const float x1 = (-box[0] + static_cast<float>(w) + 0.5f) * static_cast<float>(stride);
            const float y1 = (-box[1] + static_cast<float>(h) + 0.5f) * static_cast<float>(stride);
            const float x2 = (box[2] + static_cast<float>(w) + 0.5f) * static_cast<float>(stride);
            const float y2 = (box[3] + static_cast<float>(h) + 0.5f) * static_cast<float>(stride);

            boxes->push_back(x1);
            boxes->push_back(y1);
            boxes->push_back(x2 - x1);
            boxes->push_back(y2 - y1);
            scores->push_back(DequantI8ToF32(best_score, score_zp, score_scale));
            class_ids->push_back(best_cls);
            valid++;
        }
    }
    return valid;
}

int YoloNpuInstance::ProcessFp32Branch(const rknn_output* outputs, int box_idx, int score_idx,
                                       int score_sum_idx, int grid_h, int grid_w, int stride,
                                       int dfl_len, std::vector<float>* boxes,
                                       std::vector<float>* scores,
                                       std::vector<int>* class_ids) const
{
    if (!outputs || !boxes || !scores || !class_ids)
    {
        return -1;
    }
    const int grid_len = grid_h * grid_w;
    if (grid_len <= 0)
    {
        return 0;
    }

    const float* box_tensor   = static_cast<const float*>(outputs[box_idx].buf);
    const float* score_tensor = static_cast<const float*>(outputs[score_idx].buf);
    const float* score_sum_tensor =
        (score_sum_idx >= 0) ? static_cast<const float*>(outputs[score_sum_idx].buf) : nullptr;
    if (!box_tensor || !score_tensor)
    {
        return -1;
    }

    std::vector<float> before_dfl(static_cast<size_t>(dfl_len) * 4u, 0.0f);
    int                valid = 0;
    for (int h = 0; h < grid_h; ++h)
    {
        for (int w = 0; w < grid_w; ++w)
        {
            const int cell = h * grid_w + w;
            if (score_sum_tensor && score_sum_tensor[cell] < conf_threshold_)
            {
                continue;
            }

            int   best_cls   = -1;
            float best_score = -1.0f;
            int   offset     = cell;
            for (int c = 0; c < class_count_; ++c)
            {
                const float score = score_tensor[offset];
                if (score > conf_threshold_ && score > best_score)
                {
                    best_score = score;
                    best_cls   = c;
                }
                offset += grid_len;
            }
            if (best_cls < 0)
            {
                continue;
            }

            offset = cell;
            for (int k = 0; k < dfl_len * 4; ++k)
            {
                before_dfl[static_cast<size_t>(k)] = box_tensor[offset];
                offset += grid_len;
            }
            float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ComputeDfl(before_dfl.data(), dfl_len, box);

            const float x1 = (-box[0] + static_cast<float>(w) + 0.5f) * static_cast<float>(stride);
            const float y1 = (-box[1] + static_cast<float>(h) + 0.5f) * static_cast<float>(stride);
            const float x2 = (box[2] + static_cast<float>(w) + 0.5f) * static_cast<float>(stride);
            const float y2 = (box[3] + static_cast<float>(h) + 0.5f) * static_cast<float>(stride);

            boxes->push_back(x1);
            boxes->push_back(y1);
            boxes->push_back(x2 - x1);
            boxes->push_back(y2 - y1);
            scores->push_back(best_score);
            class_ids->push_back(best_cls);
            valid++;
        }
    }
    return valid;
}

int YoloNpuInstance::PostProcess(const LetterBox& letter_box, int src_width, int src_height,
                                 std::vector<DetectBox>* out_boxes) const
{
    if (!out_boxes)
    {
        return -1;
    }
    out_boxes->clear();

    if (io_num_.n_output % 3 != 0)
    {
        return -1;
    }
    const int output_per_branch = static_cast<int>(io_num_.n_output / 3);
    const int dfl_len           = static_cast<int>(output_attrs_[0].dims[1] / 4);
    if (dfl_len <= 0)
    {
        return -1;
    }

    std::vector<float> boxes;
    std::vector<float> scores;
    std::vector<int>   class_ids;
    // Pre-reserve to avoid reallocations during inference
    boxes.reserve(512u);
    scores.reserve(256u);
    class_ids.reserve(256u);

    int valid_count = 0;
    for (int b = 0; b < 3; ++b)
    {
        const int box_idx      = b * output_per_branch;
        const int score_idx    = box_idx + 1;
        const int score_sum_idx = (output_per_branch >= 3) ? (box_idx + 2) : -1;

        const int grid_h = static_cast<int>(output_attrs_[static_cast<size_t>(box_idx)].dims[2]);
        const int grid_w = static_cast<int>(output_attrs_[static_cast<size_t>(box_idx)].dims[3]);
        if (grid_h <= 0 || grid_w <= 0)
        {
            continue;
        }
        const int stride = model_height_ / grid_h;

        int branch_valid = 0;
        if (is_quant_)
        {
            branch_valid = ProcessI8Branch(outputs_.data(), box_idx, score_idx, score_sum_idx, grid_h,
                                           grid_w, stride, dfl_len, &boxes, &scores, &class_ids);
        }
        else
        {
            branch_valid = ProcessFp32Branch(outputs_.data(), box_idx, score_idx, score_sum_idx,
                                             grid_h, grid_w, stride, dfl_len, &boxes, &scores,
                                             &class_ids);
        }
        if (branch_valid < 0)
        {
            return -1;
        }
        valid_count += branch_valid;
    }

    if (valid_count <= 0)
    {
        return 0;
    }

    std::vector<int> order(static_cast<size_t>(valid_count));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return scores[static_cast<size_t>(a)] > scores[static_cast<size_t>(b)]; });

    std::vector<uint8_t> suppressed(static_cast<size_t>(valid_count), 0u);
    if (model_kind_ == ModelKind::kYoloV8Detection)
    {
        for (size_t i = 0; i < order.size(); ++i)
        {
            const int idx_i = order[i];
            if (suppressed[static_cast<size_t>(idx_i)])
            {
                continue;
            }
            for (size_t j = i + 1; j < order.size(); ++j)
            {
                const int idx_j = order[j];
                if (suppressed[static_cast<size_t>(idx_j)])
                {
                    continue;
                }
                if (class_ids[static_cast<size_t>(idx_i)] != class_ids[static_cast<size_t>(idx_j)])
                {
                    continue;
                }
                if (IoU(boxes, idx_i, idx_j) > nms_threshold_)
                {
                    suppressed[static_cast<size_t>(idx_j)] = 1u;
                }
            }
        }
    }

    for (size_t i = 0; i < order.size(); ++i)
    {
        if (out_boxes->size() >= kMaxDetectCount)
        {
            break;
        }
        const int idx = order[i];
        if (idx < 0 || idx >= valid_count)
        {
            continue;
        }
        if (suppressed[static_cast<size_t>(idx)])
        {
            continue;
        }

        float x1 = boxes[static_cast<size_t>(idx) * 4u + 0u] - static_cast<float>(letter_box.x_pad);
        float y1 = boxes[static_cast<size_t>(idx) * 4u + 1u] - static_cast<float>(letter_box.y_pad);
        float x2 = x1 + boxes[static_cast<size_t>(idx) * 4u + 2u];
        float y2 = y1 + boxes[static_cast<size_t>(idx) * 4u + 3u];

        x1 = std::clamp(x1, 0.0f, static_cast<float>(model_width_));
        y1 = std::clamp(y1, 0.0f, static_cast<float>(model_height_));
        x2 = std::clamp(x2, 0.0f, static_cast<float>(model_width_));
        y2 = std::clamp(y2, 0.0f, static_cast<float>(model_height_));

        const float inv_scale = (letter_box.scale <= 0.0f) ? 1.0f : (1.0f / letter_box.scale);

        DetectBox box{};
        box.cls_id = class_ids[static_cast<size_t>(idx)];
        box.score  = scores[static_cast<size_t>(idx)];
        box.left   = ClampInt(static_cast<int>(std::floor(x1 * inv_scale)), 0, src_width - 1);
        box.top    = ClampInt(static_cast<int>(std::floor(y1 * inv_scale)), 0, src_height - 1);
        box.right  = ClampInt(static_cast<int>(std::ceil(x2 * inv_scale)), 0, src_width - 1);
        box.bottom = ClampInt(static_cast<int>(std::ceil(y2 * inv_scale)), 0, src_height - 1);
        if (box.right < box.left)
        {
            std::swap(box.right, box.left);
        }
        if (box.bottom < box.top)
        {
            std::swap(box.bottom, box.top);
        }
        out_boxes->push_back(box);
    }

    return 0;
}

int YoloNpuInstance::Infer(const IO_FD_t* input_nv12, std::vector<DetectBox>* out_boxes)
{
    if (YOLO_UNLIKELY(!initialized_ || !input_nv12 || !out_boxes))
    {
        return -1;
    }

    LetterBox letter_box{};
    if (YOLO_UNLIKELY(Preprocess(input_nv12, &letter_box) != 0))
    {
        return -1;
    }

    inputs_[0].buf = model_input_.data();
    int ret        = rknn_inputs_set(rknn_ctx_, io_num_.n_input, inputs_.data());
    if (YOLO_UNLIKELY(ret != RKNN_SUCC))
    {
        std::cerr << "[YOLO] rknn_inputs_set failed, ret=" << ret << std::endl;
        return -1;
    }
    ret = rknn_run(rknn_ctx_, nullptr);
    if (YOLO_UNLIKELY(ret != RKNN_SUCC))
    {
        std::cerr << "[YOLO] rknn_run failed, ret=" << ret << std::endl;
        return -1;
    }

    for (uint32_t i = 0; i < io_num_.n_output; ++i)
    {
        outputs_[i]            = {};
        outputs_[i].index      = i;
        outputs_[i].want_float = !is_quant_;
    }

    ret = rknn_outputs_get(rknn_ctx_, io_num_.n_output, outputs_.data(), nullptr);
    if (YOLO_UNLIKELY(ret != RKNN_SUCC))
    {
        std::cerr << "[YOLO] rknn_outputs_get failed, ret=" << ret << std::endl;
        return -1;
    }

    const int post_ret = PostProcess(letter_box, static_cast<int>(input_nv12->width),
                                     static_cast<int>(input_nv12->height), out_boxes);
    const int release_ret = rknn_outputs_release(rknn_ctx_, io_num_.n_output, outputs_.data());
    if (YOLO_UNLIKELY(release_ret != RKNN_SUCC))
    {
        std::cerr << "[YOLO] rknn_outputs_release failed, ret=" << release_ret << std::endl;
    }
    return (post_ret == 0) ? 0 : -1;
}
