#include <boost/interprocess/managed_mapped_file.hpp>
#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <mutex>

namespace bip = boost::interprocess;

namespace dztrader::shm {

namespace order_id_meta_internal {

class OrderIdMetaImpl {
public:
    OrderIdMetaImpl(const std::string& name, const std::filesystem::path& shm_dir)
        : lock_{name + ".order_id"},
          shm_dir_{shm_dir} {
        open_or_create_segment(name);
        std::scoped_lock<ProcessMutex> lk(lock_);
        init_shared_object_locked();
    }

    ~OrderIdMetaImpl() = default;
    OrderIdMetaImpl(const OrderIdMetaImpl&) = delete;
    OrderIdMetaImpl& operator=(const OrderIdMetaImpl&) = delete;
    OrderIdMetaImpl(OrderIdMetaImpl&&) = delete;
    OrderIdMetaImpl& operator=(OrderIdMetaImpl&&) = delete;

    [[nodiscard]] ShmAtomicOrderId* order_id_ptr() const noexcept { return order_id_; }

private:
    void open_or_create_segment(const std::string& name) {
        auto dir = shm_dir_ / name;
        std::filesystem::create_directories(dir);
        auto file_path = dir / "order_id.dat";
        segment_ =
            bip::managed_mapped_file{bip::open_or_create, file_path.string().c_str(), FILE_SIZE};
    }

    void init_shared_object_locked() {
        order_id_ = segment_.find_or_construct<ShmAtomicOrderId>("order_id")(0);
        if (!order_id_) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "order_id is null");
        }
    }

    static constexpr uint64_t FILE_SIZE = 4 * 1024;

    ShmAtomicOrderId* order_id_ = nullptr;
    ProcessMutex lock_;
    std::filesystem::path shm_dir_;
    bip::managed_mapped_file segment_;
};

}  // namespace order_id_meta_internal

OrderIdMeta::~OrderIdMeta() = default;
OrderIdMeta::OrderIdMeta(OrderIdMeta&&) noexcept = default;
OrderIdMeta& OrderIdMeta::operator=(OrderIdMeta&&) noexcept = default;

OrderIdMeta::OrderIdMeta(std::unique_ptr<OrderIdMetaImpl> impl)
    : id_{impl->order_id_ptr()},
      impl_{std::move(impl)} {}

OrderIdMeta OrderIdMeta::open_or_create(const std::string& name,
                                        const std::filesystem::path& shm_dir) {
    auto impl = std::make_unique<OrderIdMetaImpl>(name, shm_dir);
    return OrderIdMeta(std::move(impl));
}

}  // namespace dztrader::shm
