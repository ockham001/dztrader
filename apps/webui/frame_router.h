#ifndef DZTRADER_WEBUI_FRAME_ROUTER_H_
#define DZTRADER_WEBUI_FRAME_ROUTER_H_

#include <drogon/drogon.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <spdlog/spdlog.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace dztrader::webui {

class FrameRouter {
public:
    using Poster = std::function<void(std::function<void()>)>;
    using RawHandler = std::function<void(const shm::FrameView&)>;

    explicit FrameRouter(Poster poster = default_poster()) : poster_(std::move(poster)) {}

    template <typename Payload>
    void register_json(DzFrameType type, bool has_instance_id,
                       std::function<void(const std::string& source, const Payload&)> h) {
        handlers_[type] = [this, has_instance_id, h = std::move(h)](const shm::FrameView& view) {
            // 第一层（监听线程）：decode，失败 WARN 丢帧
            try {
                if (has_instance_id) {
                    auto payload = shm::decode_ext_inst_json<Payload>(view);
                    auto source = std::string(view.ext_inst_id());
                    const int type = static_cast<int>(view.type());
                    poster_([h, source = std::move(source), payload = std::move(payload), type]() {
                        // 第二层（IO 线程）：handler，异常 ERROR 不扩散
                        try {
                            h(source, payload);
                        } catch (const std::exception& e) {
                            SPDLOG_ERROR("frame handler failed | type={} error={}",
                                         type, e.what());
                        }
                    });
                } else {
                    auto payload = shm::decode_ext_json<Payload>(view);
                    poster_([h, payload = std::move(payload)]() {
                        try {
                            h("", payload);
                        } catch (const std::exception& e) {
                            SPDLOG_ERROR("frame handler failed | error={}", e.what());
                        }
                    });
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN("decode frame failed | type={} error={}",
                            static_cast<int>(view.type()), e.what());
            }
        };
    }

    void register_raw(DzFrameType type, RawHandler h) {
        handlers_[type] = std::move(h);
    }

    void dispatch(const shm::FrameView& view) {
        auto it = handlers_.find(view.type());
        if (it == handlers_.end()) {
            SPDLOG_DEBUG("unregistered frame type={}", static_cast<int>(view.type()));
            return;
        }
        it->second(view);
    }

    // ===== 线程语义（关键约定）=====
    // dispatch 在 EventMonitor 监听线程执行：
    // - register_json 的 handler：decode 在监听线程（FrameView 有效期内），投递到 IO 线程执行（值拷贝，安全）
    // - register_raw 的 handler：在监听线程**同步执行**（FrameView 有效）；
    //   若 handler 需要异步（如调用 drogon API），必须自行 queueInLoop 且只捕获已拷贝的数据
    //   （string 值等），**严禁捕获 FrameView 引用**（投递后 SHM 写入位置移动，指针失效）。
    //   ControlDomainService 已按此约定实现（方法内部 queueInLoop）。

private:
    static Poster default_poster() {
        return [](std::function<void()> f) { drogon::app().getLoop()->queueInLoop(std::move(f)); };
    }

    std::unordered_map<DzFrameType, std::function<void(const shm::FrameView&)>> handlers_;
    Poster poster_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_FRAME_ROUTER_H_
