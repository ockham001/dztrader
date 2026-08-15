#ifndef DZTRADER_PLATFORM_SHM_CONFIG_H_
#define DZTRADER_PLATFORM_SHM_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <dztrader/data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform::detail {

// ===== 内部工具函数 =====

inline int64_t get_int_value(const nlohmann::json& j) {
    return j.is_number_integer() ? j.get<int64_t>() : static_cast<int64_t>(j.get<double>());
}

// bytes/check_bytes 上界:1024×1024×1024×1024 = 2^40
inline constexpr int64_t SHM_BYTES_MAX = 1LL << 40;

/// SHM 通道配置基类,封装 SET_xxx_SHM_CONFIG / RTN_xxx_SHM_CONFIG 的接收处理与发送。
/// 持有当前配置镜像,与配置文件持久化同步。
///
/// 职责边界(与 LogConfig 一致):类只负责配置的加载/修改/持久化/发送 RTN。
/// rtn 的时机和次数由外部控制——类内部不自己调 rtn_shm_config()。
/// SET 失败的日志+NOTIFY_UI 也由外部 catch 块处理。
///
/// 严格遵循 docs/frame_contracts/01-shm.md 契约:
///   - SET 纯 RFC 7386 JSON Merge Patch(page_size_mb 不可变,完全跳过)
///   - null 仅允许 preload_points 内部某 key 的 value(表示删除该 key),其余位置 null 均校验失败
///   - preload_points 值为 {} 时按纯 RFC 7386 语义为无操作
///   - 新增 preload_points 的 key 时缺失 pages/bytes 补默认值 0 后再校验范围
class ShmConfigBase {
public:
    virtual ~ShmConfigBase() = default;

    // 持有引用成员 writer_,不可拷贝/移动(Rule of Five 显式声明)
    ShmConfigBase(const ShmConfigBase&) = delete;
    ShmConfigBase& operator=(const ShmConfigBase&) = delete;
    ShmConfigBase(ShmConfigBase&&) = delete;
    ShmConfigBase& operator=(ShmConfigBase&&) = delete;

    /// 启动时加载配置。失败用默认值并修复文件(自愈)。不抛,不发 RTN。
    /// 调用者需自行决定是否调 rtn_shm_config() 上报。
    void load();

    /// 接收处理 SET_xxx_SHM_CONFIG。
    /// 成功:检查 patch → 合并 → validate → save → 更新 cfg_。
    /// 失败:抛 std::runtime_error,内部 cfg_ 不变(强保证)。
    /// 空对象 {} 视为无操作,仍走完整流程(不改变 cfg_)。
    /// 本方法不发 RTN,也不做 NOTIFY_UI——由调用者 catch 后处理。
    void set_shm_config(const nlohmann::json& patch);

    /// 发送 RTN_xxx_SHM_CONFIG 帧。始终全量当前 cfg_,无 error 字段(契约要求)。
    /// 非 const:内部写共享内存有副作用。
    /// rtn 的时机和次数完全由外部控制,类内部不自动调用本方法。
    void rtn_shm_config() { send_rtn(cfg_); }

    /// 预加载点只读视图(封装 JSON 解析,供 maintenance 遍历)
    struct PreloadPointView {
        std::string time;       ///< "HH:MM"
        uint32_t pages = 0;     ///< 预加载页数
        uint64_t bytes = 0;     ///< 预加载字节数
    };

    /// 只读访问器(maintenance 用)
    /// @{
    [[nodiscard]] uint64_t page_size_mb() const { return cfg_["page_size_mb"].get<uint64_t>(); }
    [[nodiscard]] int check_interval_min() const { return cfg_["check_interval_min"].get<int>(); }
    [[nodiscard]] uint32_t check_pages() const { return cfg_["check_pages"].get<uint32_t>(); }
    [[nodiscard]] uint64_t check_bytes() const { return cfg_["check_bytes"].get<uint64_t>(); }
    [[nodiscard]] std::vector<PreloadPointView> preload_points() const;
    /// @}

protected:
    /// @param default_cfg 默认配置(子类提供,page_size_mb 默认不同)
    /// @param cfg_path    持久化路径
    /// @param writer      共享内存写入器
    /// @param section     JSON 路径;空路径 "" 表示 root
    ShmConfigBase(nlohmann::json default_cfg,
                  std::filesystem::path cfg_path,
                  shm::MultiWriter& writer,
                  nlohmann::json::json_pointer section);

    // 子类实现:按对应帧类型 + 头类型发送 RTN
    virtual void send_rtn(const nlohmann::json& cfg) = 0;

    // 子类(MdShmConfig)含 instance_id 帧发送需要;基类设计需要 protected
    shm::MultiWriter& writer_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    nlohmann::json default_cfg_;
    std::filesystem::path cfg_path_;
    nlohmann::json::json_pointer section_;
    nlohmann::json cfg_;  // 当前配置镜像(始终全量,契约第 90 行)

    // 按 section_ 从文件读取,文件不存在/section 缺失返回默认值,JSON 解析失败抛。
    // degraded=true 表示文件内容非法/不可读,调用方应修复文件并记日志
    [[nodiscard]] nlohmann::json load_cfg_from_file(bool& degraded) const;

    // 按 section_ 原子写入文件(load-modify-save 保留其他 section),失败抛
    void save_cfg_to_file(const nlohmann::json& cfg) const;

    // 校验配置值,返回错误消息或 nullopt
    static std::optional<std::string> validate(const nlohmann::json& cfg);

    // 把 patch 合并进 cfg_ 并返回 merged(抛异常时 cfg_ 不变,因为只动 merged 副本)
    [[nodiscard]] nlohmann::json merge_patch(const nlohmann::json& patch) const;
};

}  // namespace dztrader::platform::detail

// ===== 外部暴露的两个类 =====

namespace dztrader::platform {

/// 最小 page size 字节数 (1MB), 低于此值会被 clamp
inline constexpr uint64_t MIN_PAGE_SIZE_BYTES = 1ULL * 1024 * 1024;

/// MB 转 bytes
constexpr uint64_t mb_to_bytes(uint64_t mb) { return mb * 1024 * 1024; }

/// 事件通道 SHM 配置(master 用)。RTN 无 instance_id。
/// 默认 page_size_mb=64。
class EventShmConfig : public detail::ShmConfigBase {
public:
    EventShmConfig(
        std::filesystem::path cfg_path,
        shm::MultiWriter& writer,
        nlohmann::json::json_pointer section = nlohmann::json::json_pointer("/event_shm"))
        : ShmConfigBase(make_default(), std::move(cfg_path), writer, std::move(section)) {}

protected:
    void send_rtn(const nlohmann::json& cfg) override;

private:
    static nlohmann::json make_default();
};

/// 行情通道 SHM 配置(各行情进程用)。RTN 带 instance_id(与通道名一致)。
/// 默认 page_size_mb=1024。
class MdShmConfig : public detail::ShmConfigBase {
public:
    MdShmConfig(std::string instance_id,
                std::filesystem::path cfg_path,
                shm::MultiWriter& writer,
                nlohmann::json::json_pointer section = nlohmann::json::json_pointer("/md_shm"))
        : ShmConfigBase(make_default(), std::move(cfg_path), writer, std::move(section)),
          instance_id_(std::move(instance_id)) {}

protected:
    void send_rtn(const nlohmann::json& cfg) override {
        // 行情通道含 instance_id 帧 (DzExtInstFrameHeader)
        write_ext_inst_json_obj(writer_, DZ_FRAME_RTN_MD_SHM_CONFIG, instance_id_, cfg);
    }

private:
    std::string instance_id_;
    static nlohmann::json make_default();
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_SHM_CONFIG_H_