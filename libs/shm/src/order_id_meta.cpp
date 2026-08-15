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
          name_{name},
          shm_dir_{shm_dir},
          is_creator_{true} {
        open_or_create_segment();
        std::scoped_lock<ProcessMutex> lk(lock_);
        init_shared_object_locked();
    }

    OrderIdMetaImpl(const std::string& name,
                    const std::filesystem::path& shm_dir,
                    int /*open_only_tag*/)
        : lock_{name + ".order_id"},
          name_{name},
          shm_dir_{shm_dir} {
        open_existing_segment();
        std::scoped_lock<ProcessMutex> lk(lock_);
        find_shared_object_locked();
    }

    ~OrderIdMetaImpl() = default;
    OrderIdMetaImpl(const OrderIdMetaImpl&) = delete;
    OrderIdMetaImpl& operator=(const OrderIdMetaImpl&) = delete;
    OrderIdMetaImpl(OrderIdMetaImpl&&) = delete;
    OrderIdMetaImpl& operator=(OrderIdMetaImpl&&) = delete;

    [[nodiscard]] ShmAtomicOrderId* order_id_ptr() const noexcept { return order_id_; }

    [[nodiscard]] ShmAtomicOrderId* order_id() {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_ORDER_ID_ACCESS_FAILED,
                            "order_id access requires creator role");
        }
        return order_id_;
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    void open_or_create_segment() {
        auto dir = shm_dir_ / name_;
        std::filesystem::create_directories(dir);
        auto file_path = dir / "order_id.dat";
        segment_ =
            bip::managed_mapped_file{bip::open_or_create, file_path.string().c_str(), FILE_SIZE};
    }

    void open_existing_segment() {
        auto file_path = shm_dir_ / name_ / "order_id.dat";
        segment_ = bip::managed_mapped_file{bip::open_only, file_path.string().c_str()};
    }

    void init_shared_object_locked() {
        order_id_ = segment_.find_or_construct<ShmAtomicOrderId>("order_id")(0);
        if (!order_id_) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "order_id is null");
        }
    }

    void find_shared_object_locked() {
        order_id_ = segment_.find<ShmAtomicOrderId>("order_id").first;
        if (!order_id_) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "order_id is null");
        }
    }

    static constexpr uint64_t FILE_SIZE = 4 * 1024;

    ShmAtomicOrderId* order_id_ = nullptr;
    ProcessMutex lock_;
    std::string name_;
    std::filesystem::path shm_dir_;
    bool is_creator_ = false;
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

OrderIdMeta OrderIdMeta::open_only(const std::string& name, const std::filesystem::path& shm_dir) {
    auto impl = std::make_unique<OrderIdMetaImpl>(name, shm_dir, 0);
    return OrderIdMeta(std::move(impl));
}

ShmAtomicOrderId* OrderIdMeta::order_id() { return impl_->order_id(); }

const std::string& OrderIdMeta::name() const noexcept { return impl_->name(); }

}  // namespace dztrader::shm
