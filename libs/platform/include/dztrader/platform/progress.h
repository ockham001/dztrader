#ifndef DZTRADER_PLATFORM_PROGRESS_H_
#define DZTRADER_PLATFORM_PROGRESS_H_

#include <string>

#include <nlohmann/json.hpp>

#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

/// 进度推送器。绑定 source + writer，封装 DZ_FRAME_RTN_PROGRESS 帧。
///
/// 与 LogConfig/AutoLoginConfig 一致：内部持有 json 镜像，rtn 推送全量镜像。
/// 与 LogConfig/AutoLoginConfig 的差异：无 SET、无持久化、无文件 IO（纯推送）。
///
/// 严格遵循 docs/frame_contracts/06-progress.md 契约：
///   - max > min：确定进度条，前端按 (current-min)/(max-min) 渲染
///   - max <= min（含 max==0）：不确定进度，前端显示 spinner 或隐藏进度条
///   - current 应在 [min, max] 范围内；超出时前端钳制（防御性，不报错）
///   - desc 空串 = 无文本说明（契约允许空串或省略；实现统一带 desc 字段，
///     前端按空串处理为无文本）
class ProgressReporter {
public:
    /// @param source 来源进程名，作 RTN_PROGRESS 的帧头 instance_id
    /// @param writer  共享内存写入器
    ProgressReporter(std::string source, shm::MultiWriter& writer)
        : source_(std::move(source)),
          writer_(writer),
          cfg_(default_cfg()) {}

    // 禁拷贝/移动：持有引用成员，绑定后终身不变，不应放入容器或按值传递
    ProgressReporter(const ProgressReporter&) = delete;
    ProgressReporter& operator=(const ProgressReporter&) = delete;
    ProgressReporter(ProgressReporter&&) = delete;
    ProgressReporter& operator=(ProgressReporter&&) = delete;

    ~ProgressReporter() = default;

    /// 设置进度（修改内部镜像，不推送）。一次性设置全部字段，适合初始化。
    /// @param min     进度最小值（通常 0）
    /// @param max     进度最大值；max <= min 表示不确定进度
    /// @param current 当前进度
    /// @param desc    简短文本说明（空串 = 无文本）
    void set(int min, int max, int current, std::string desc = "") {
        cfg_["min"] = min;
        cfg_["max"] = max;
        cfg_["current"] = current;
        cfg_["desc"] = std::move(desc);
    }

    /// 设置当前进度（desc 保留不变）。
    void set_current(int current) {
        cfg_["current"] = current;
    }

    /// 设置当前进度 + 说明。
    void set_current(int current, std::string desc) {
        cfg_["current"] = current;
        cfg_["desc"] = std::move(desc);
    }

    /// 推送 RTN_PROGRESS 帧（始终全量当前镜像）。
    /// rtn 的时机和次数完全由外部控制，类内部不自动调用本方法。
    void rtn() {
        write_ext_inst_json_obj(writer_, DZ_FRAME_RTN_PROGRESS, source_, cfg_);
    }

    /// 只读访问当前配置镜像（快照上报时判断是否有活跃进度用）。
    const nlohmann::json& config() const noexcept { return cfg_; }

private:
    std::string source_;
    shm::MultiWriter& writer_;
    nlohmann::json cfg_;  // { "min": int, "max": int, "current": int, "desc": string }

    static nlohmann::json default_cfg() {
        return {{"min", 0}, {"max", 0}, {"current", 0}, {"desc", ""}};
    }
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_PROGRESS_H_
