#include <dztrader/core/last_error.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/subscriber_list.h>

namespace dztrader::shm {

bool SubscriberList::sync_from_channel_meta(const ChannelMeta& meta) noexcept
{
    try {
        auto names = meta.reader_names();
        std::vector<NamedSemaphore> new_sems;
        new_sems.reserve(names.size());
        for (const auto& name : names) {
            new_sems.emplace_back(name);
        }
        sems_ = std::move(new_sems);
        return true;
    } catch (...) {
        LastError::set(DZ_EC_SHM_SUBSCRIBER_FAILED, "sync_from_channel_meta failed");
        return false;
    }
}

}  // namespace dztrader::shm
