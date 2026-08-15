#ifndef DZTRADER_SHM_SUBSCRIBER_LIST_H_
#define DZTRADER_SHM_SUBSCRIBER_LIST_H_
#include <dztrader/shm/named_semaphore.h>
#include <vector>

namespace dztrader::shm {

class ChannelMeta;

class SubscriberList {
public:
    SubscriberList() = default;
    ~SubscriberList() = default;

    SubscriberList(const SubscriberList&) = delete;
    SubscriberList& operator=(const SubscriberList&) = delete;
    SubscriberList(SubscriberList&&) = default;
    SubscriberList& operator=(SubscriberList&&) = default;

    bool sync_from_channel_meta(const ChannelMeta& meta) noexcept;

    void notify_all() noexcept
    {
        for (auto& sem : sems_) {
            sem.notify();
        }
    }

private:
    std::vector<NamedSemaphore> sems_;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_SUBSCRIBER_LIST_H_
