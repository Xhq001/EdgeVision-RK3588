#ifndef YOLO_NPU_H
#define YOLO_NPU_H

#include "pubDataType.h"

#include <rknn_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class YoloNpuInstance
{
   public:
    struct DetectBox
    {
        int   cls_id = -1;
        float score  = 0.0f;
        int   left   = 0;
        int   top    = 0;
        int   right  = 0;
        int   bottom = 0;
    };

    YoloNpuInstance()  = default;
    ~YoloNpuInstance() { Deinit(); }

    // core_id: 0/1/2 表示绑定到对应 NPU 核心；<0 表示 RKNN_NPU_CORE_AUTO。
    // dup_from: 非空且指向已初始化的 rknn_context 时，用 rknn_dup_context 共享权重（省内存），
    //           否则各自 rknn_init 加载模型文件。
    int  Init(const std::string& model_path, const std::string& label_path = "", int core_id = -1,
              rknn_context* dup_from = nullptr);
    void Deinit();
    bool IsReady() const { return initialized_; }

    // 供多实例池做 rknn_dup_context 的源上下文指针（实例 0 的 context）。
    rknn_context* MutableContext() { return &rknn_ctx_; }

    // 输入必须是 NV12（MPP_FMT_YUV420SP）格式的帧描述，输出检测框坐标基于原始输入分辨率。
    int Infer(const IO_FD_t* input_nv12, std::vector<DetectBox>* out_boxes);

    const char* GetLabelName(int cls_id) const;

   private:
    struct LetterBox
    {
        int   x_pad     = 0;
        int   y_pad     = 0;
        float scale     = 1.0f;
        int   resized_w = 0;
        int   resized_h = 0;
    };

    enum class ModelKind
    {
        kUnsupported = 0,
        kYoloV8Detection,
        kYoloV10Detection,
    };

    int  LoadModel(const std::string& model_path, int core_id, rknn_context* dup_from);
    int  QueryModelMeta();
    int  LoadLabels(const std::string& label_path);
    int  Preprocess(const IO_FD_t* input_nv12, LetterBox* letter_box);
    int  PostProcess(const LetterBox& letter_box, int src_width, int src_height,
                     std::vector<DetectBox>* out_boxes) const;
    int  ProcessI8Branch(const rknn_output* outputs, int box_idx, int score_idx, int score_sum_idx,
                         int grid_h, int grid_w, int stride, int dfl_len, std::vector<float>* boxes,
                         std::vector<float>* scores, std::vector<int>* class_ids) const;
    int  ProcessFp32Branch(const rknn_output* outputs, int box_idx, int score_idx, int score_sum_idx,
                           int grid_h, int grid_w, int stride, int dfl_len,
                           std::vector<float>* boxes, std::vector<float>* scores,
                           std::vector<int>* class_ids) const;
    void ComputeDfl(const float* tensor, int dfl_len, float* box) const;

    bool                    initialized_    = false;
    bool                    is_quant_       = false;
    ModelKind               model_kind_     = ModelKind::kUnsupported;
    int                     model_width_    = 0;
    int                     model_height_   = 0;
    int                     model_channel_  = 0;
    int                     class_count_    = 0;
    float                   conf_threshold_ = 0.5f;
    float                   nms_threshold_  = 0.8f;
    rknn_context            rknn_ctx_       = 0;
    rknn_input_output_num   io_num_{};
    std::vector<rknn_input> inputs_;
    mutable std::vector<rknn_output> outputs_;
    std::vector<rknn_tensor_attr>    input_attrs_;
    std::vector<rknn_tensor_attr>    output_attrs_;
    std::vector<std::string>         labels_;
    std::vector<uint8_t>             model_input_;
    std::vector<uint8_t>             resized_nv12_;
    std::vector<uint8_t>             resized_rgb_;
};

#endif  // YOLO_NPU_H
