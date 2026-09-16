// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "aeris/storage/resource.hpp"

#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aeris::desktop {

struct FlagResourceLoadResult final {
    std::string resource_id;
    QImage image;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return !image.isNull(); }
};

// Reads and decodes only the flag resources selected by the current viewport.
// Project open therefore stays independent of the number of embedded flags and
// paintEvent never performs SQLite I/O. Like terrain detail loading, this is a
// rebuildable presentation cache over immutable durable project resources.
class FlagResourceLoader final : public QObject {
public:
    using ResultCallback = std::function<void(
        std::filesystem::path,
        std::vector<FlagResourceLoadResult>)>;

    explicit FlagResourceLoader(QObject* parent = nullptr);
    ~FlagResourceLoader() override;

    FlagResourceLoader(const FlagResourceLoader&) = delete;
    FlagResourceLoader& operator=(const FlagResourceLoader&) = delete;

    void set_result_callback(ResultCallback callback);
    void request(
        std::filesystem::path project_path,
        std::vector<std::string> resource_ids);
    void cancel();

    [[nodiscard]] bool busy() const noexcept { return task_running_; }

    // Queued worker delivery boundary.
    void accept_batch(
        std::uint64_t generation,
        std::filesystem::path project_path,
        std::vector<FlagResourceLoadResult> results);

private:
    void start_request(
        const std::filesystem::path& project_path,
        const std::vector<std::string>& resource_ids);
    void invalidate_active_task();

    QThreadPool* pool_{nullptr};
    ResultCallback result_callback_;
    std::shared_ptr<std::atomic_bool> cancel_token_;
    std::shared_ptr<std::mutex> delivery_mutex_;
    std::filesystem::path active_project_path_;
    std::vector<std::string> active_resource_ids_;
    std::uint64_t generation_{0U};
    bool task_running_{false};
};

namespace detail {

class FlagResourceTask final : public QRunnable {
public:
    FlagResourceTask(
        QPointer<FlagResourceLoader> target,
        std::filesystem::path project_path,
        std::vector<std::string> resource_ids,
        std::shared_ptr<std::atomic_bool> canceled,
        std::shared_ptr<std::mutex> delivery_mutex,
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          project_path_(std::move(project_path)),
          resource_ids_(std::move(resource_ids)),
          canceled_(std::move(canceled)),
          delivery_mutex_(std::move(delivery_mutex)),
          generation_(generation) {
        setAutoDelete(true);
    }

    void run() override {
        std::vector<FlagResourceLoadResult> results;
        results.reserve(resource_ids_.size());
        const auto token = canceled_;
        if (token->load(std::memory_order_relaxed)) return;

        storage::ProjectStoreResult opened = storage::ProjectStore::open(project_path_);
        if (!opened.ok()) {
            const std::string diagnostic = opened.status.diagnostic.empty()
                ? "unable to open durable project for flag resources"
                : opened.status.diagnostic;
            for (const std::string& resource_id : resource_ids_) {
                results.push_back({resource_id, {}, diagnostic});
            }
            deliver(std::move(results));
            return;
        }

        const storage::ProjectResourceListResult listed =
            storage::list_project_resources(*opened.store);
        if (!listed.ok()) {
            const std::string diagnostic = listed.status.diagnostic.empty()
                ? "unable to enumerate durable flag resources"
                : listed.status.diagnostic;
            for (const std::string& resource_id : resource_ids_) {
                results.push_back({resource_id, {}, diagnostic});
            }
            deliver(std::move(results));
            return;
        }

        std::unordered_map<std::string, const storage::ProjectResourceRecord*> records;
        records.reserve(listed.records.size());
        for (const storage::ProjectResourceRecord& record : listed.records) {
            records.emplace(record.identity.resource_id, &record);
        }

        for (const std::string& resource_id : resource_ids_) {
            if (token->load(std::memory_order_relaxed)) return;

            FlagResourceLoadResult result{};
            result.resource_id = resource_id;
            const auto found = records.find(resource_id);
            if (found == records.end()) {
                result.diagnostic = "durable flag resource is missing";
                results.push_back(std::move(result));
                continue;
            }

            const storage::ProjectResourceRecord& record = *found->second;
            if (record.storage_mode != storage::ResourceStorageMode::embedded) {
                result.diagnostic = "flag resource is not embedded";
                results.push_back(std::move(result));
                continue;
            }
            if (record.identity.media_type != "image/png") {
                result.diagnostic = "flag resource has the wrong media type";
                results.push_back(std::move(result));
                continue;
            }
            if (record.identity.size_bytes == 0U ||
                record.identity.size_bytes >
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
                record.identity.size_bytes >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                result.diagnostic = "flag resource has an unsupported byte size";
                results.push_back(std::move(result));
                continue;
            }

            std::vector<std::uint8_t> bytes;
            bytes.reserve(static_cast<std::size_t>(record.identity.size_bytes));
            const storage::Status streamed = storage::stream_embedded_resource(
                *opened.store,
                resource_id,
                [&](const void* data, const std::size_t size) {
                    if (token->load(std::memory_order_relaxed)) {
                        return storage::Status{
                            storage::StorageError::filesystem_failure,
                            "flag resource load canceled",
                        };
                    }
                    const auto* begin = static_cast<const std::uint8_t*>(data);
                    bytes.insert(bytes.end(), begin, begin + size);
                    return storage::Status::success();
                }
            );
            if (token->load(std::memory_order_relaxed)) return;
            if (!streamed.ok()) {
                result.diagnostic = streamed.diagnostic.empty()
                    ? "unable to stream durable flag resource"
                    : streamed.diagnostic;
                results.push_back(std::move(result));
                continue;
            }
            if (bytes.size() != static_cast<std::size_t>(record.identity.size_bytes)) {
                result.diagnostic = "flag resource size changed while streaming";
                results.push_back(std::move(result));
                continue;
            }

            QImage image;
            if (!image.loadFromData(
                    reinterpret_cast<const uchar*>(bytes.data()),
                    static_cast<int>(bytes.size()),
                    "PNG"
                ) || image.isNull()) {
                result.diagnostic = "unable to decode durable flag PNG";
                results.push_back(std::move(result));
                continue;
            }
            result.image = std::move(image);
            results.push_back(std::move(result));
        }

        if (token->load(std::memory_order_relaxed)) return;
        deliver(std::move(results));
    }

private:
    void deliver(std::vector<FlagResourceLoadResult> results) {
        std::lock_guard<std::mutex> delivery_guard(*delivery_mutex_);
        if (canceled_->load(std::memory_order_relaxed)) return;
        QPointer<FlagResourceLoader> target = target_;
        if (!target) return;
        const std::filesystem::path project_path = project_path_;
        QMetaObject::invokeMethod(
            target.data(),
            [
                target,
                generation = generation_,
                project_path,
                results = std::move(results)
            ]() mutable {
                if (target) {
                    target->accept_batch(
                        generation,
                        std::move(project_path),
                        std::move(results)
                    );
                }
            },
            Qt::QueuedConnection
        );
    }

    QPointer<FlagResourceLoader> target_;
    std::filesystem::path project_path_;
    std::vector<std::string> resource_ids_;
    std::shared_ptr<std::atomic_bool> canceled_;
    std::shared_ptr<std::mutex> delivery_mutex_;
    std::uint64_t generation_{0U};
};

}  // namespace detail

inline FlagResourceLoader::FlagResourceLoader(QObject* parent)
    : QObject(parent),
      pool_(new QThreadPool),
      delivery_mutex_(std::make_shared<std::mutex>()) {
    pool_->setMaxThreadCount(1);
    pool_->setExpiryTimeout(1000);
}

inline FlagResourceLoader::~FlagResourceLoader() {
    cancel();
    if (pool_ == nullptr) return;
    pool_->clear();
    if (pool_->waitForDone(0)) delete pool_;
    pool_ = nullptr;
}

inline void FlagResourceLoader::set_result_callback(ResultCallback callback) {
    result_callback_ = std::move(callback);
}

inline void FlagResourceLoader::request(
    std::filesystem::path project_path,
    std::vector<std::string> resource_ids
) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique;
    unique.reserve(resource_ids.size());
    for (std::string& resource_id : resource_ids) {
        if (resource_id.empty() || !seen.emplace(resource_id).second) continue;
        unique.push_back(std::move(resource_id));
    }
    if (project_path.empty() || unique.empty() || pool_ == nullptr) return;

    if (task_running_ &&
        active_project_path_ == project_path &&
        active_resource_ids_ == unique) {
        return;
    }

    invalidate_active_task();
    active_project_path_ = std::move(project_path);
    active_resource_ids_ = std::move(unique);
    start_request(active_project_path_, active_resource_ids_);
}

inline void FlagResourceLoader::cancel() {
    invalidate_active_task();
    active_project_path_.clear();
    active_resource_ids_.clear();
}

inline void FlagResourceLoader::accept_batch(
    const std::uint64_t generation,
    std::filesystem::path project_path,
    std::vector<FlagResourceLoadResult> results
) {
    if (generation != generation_) return;
    task_running_ = false;
    active_resource_ids_.clear();
    if (result_callback_) {
        result_callback_(std::move(project_path), std::move(results));
    }
}

inline void FlagResourceLoader::start_request(
    const std::filesystem::path& project_path,
    const std::vector<std::string>& resource_ids
) {
    if (pool_ == nullptr) return;
    ++generation_;
    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);
    task_running_ = true;
    pool_->start(new detail::FlagResourceTask(
        QPointer<FlagResourceLoader>(this),
        project_path,
        resource_ids,
        cancel_token_,
        delivery_mutex_,
        generation
    ));
}

inline void FlagResourceLoader::invalidate_active_task() {
    if (cancel_token_) {
        std::lock_guard<std::mutex> delivery_guard(*delivery_mutex_);
        cancel_token_->store(true, std::memory_order_relaxed);
    }
    if (pool_ != nullptr) pool_->clear();
    ++generation_;
    task_running_ = false;
}

}  // namespace aeris::desktop
