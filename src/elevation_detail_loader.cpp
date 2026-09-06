// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_detail_loader.hpp"

#include "aeris/storage/project.hpp"
#include "aeris/storage/resource.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>

namespace aeris::desktop {
namespace {

class ElevationDetailTask final : public QRunnable {
public:
    ElevationDetailTask(
        QPointer<ElevationDetailLoader> target,
        std::filesystem::path project_path,
        std::vector<std::string> resource_ids,
        std::shared_ptr<std::atomic_bool> canceled,
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          project_path_(std::move(project_path)),
          resource_ids_(std::move(resource_ids)),
          canceled_(std::move(canceled)),
          generation_(generation) {
        setAutoDelete(true);
    }

    void run() override {
        std::vector<ElevationDetailLoadResult> results;
        results.reserve(resource_ids_.size());

        const auto token = canceled_;
        if (token->load(std::memory_order_relaxed)) return;

        storage::ProjectStoreResult opened = storage::ProjectStore::open(project_path_);
        if (!opened.ok()) {
            for (const std::string& resource_id : resource_ids_) {
                results.push_back({
                    resource_id,
                    std::nullopt,
                    opened.status.diagnostic.empty()
                        ? "unable to open durable project for terrain detail"
                        : opened.status.diagnostic,
                });
            }
            deliver(std::move(results));
            return;
        }

        const storage::ProjectResourceListResult listed =
            storage::list_project_resources(*opened.store);
        if (!listed.ok()) {
            for (const std::string& resource_id : resource_ids_) {
                results.push_back({
                    resource_id,
                    std::nullopt,
                    listed.status.diagnostic.empty()
                        ? "unable to enumerate durable terrain resources"
                        : listed.status.diagnostic,
                });
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

            ElevationDetailLoadResult result{};
            result.resource_id = resource_id;
            const auto found = records.find(resource_id);
            if (found == records.end()) {
                result.diagnostic = "durable terrain detail resource is missing";
                results.push_back(std::move(result));
                continue;
            }

            const storage::ProjectResourceRecord& record = *found->second;
            if (record.storage_mode != storage::ResourceStorageMode::embedded) {
                result.diagnostic = "terrain detail resource is not embedded";
                results.push_back(std::move(result));
                continue;
            }
            if (record.identity.media_type != elevation::kElevationTileMediaType) {
                result.diagnostic = "terrain detail resource has the wrong media type";
                results.push_back(std::move(result));
                continue;
            }
            if (record.identity.size_bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                result.diagnostic = "terrain detail resource is too large for this process";
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
                            "terrain detail load canceled",
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
                    ? "unable to stream durable terrain detail"
                    : streamed.diagnostic;
                results.push_back(std::move(result));
                continue;
            }
            if (bytes.size() != static_cast<std::size_t>(record.identity.size_bytes)) {
                result.diagnostic = "terrain detail resource size changed while streaming";
                results.push_back(std::move(result));
                continue;
            }

            elevation::ElevationTileDecodeResult decoded =
                elevation::decode_elevation_tile_v1(bytes);
            if (!decoded.ok()) {
                result.diagnostic = decoded.diagnostic.empty()
                    ? "unable to decode durable terrain detail"
                    : decoded.diagnostic;
                results.push_back(std::move(result));
                continue;
            }

            result.tile = std::move(*decoded.tile);
            results.push_back(std::move(result));
        }

        if (token->load(std::memory_order_relaxed)) return;
        deliver(std::move(results));
    }

private:
    void deliver(std::vector<ElevationDetailLoadResult> results) {
        if (canceled_->load(std::memory_order_relaxed) || !target_) return;
        QPointer<ElevationDetailLoader> target = target_;
        const std::filesystem::path project_path = project_path_;
        QMetaObject::invokeMethod(
            target_,
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

    QPointer<ElevationDetailLoader> target_;
    std::filesystem::path project_path_;
    std::vector<std::string> resource_ids_;
    std::shared_ptr<std::atomic_bool> canceled_;
    std::uint64_t generation_{0U};
};

}  // namespace

ElevationDetailLoader::ElevationDetailLoader(QObject* parent)
    : QObject(parent) {
    pool_.setMaxThreadCount(1);
}

ElevationDetailLoader::~ElevationDetailLoader() {
    cancel();
    pool_.waitForDone();
}

void ElevationDetailLoader::set_result_callback(ResultCallback callback) {
    result_callback_ = std::move(callback);
}

void ElevationDetailLoader::request(
    std::filesystem::path project_path,
    std::vector<std::string> resource_ids
) {
    resource_ids.erase(
        std::remove_if(
            resource_ids.begin(),
            resource_ids.end(),
            [](const std::string& resource_id) { return resource_id.empty(); }
        ),
        resource_ids.end()
    );
    std::sort(resource_ids.begin(), resource_ids.end());
    resource_ids.erase(
        std::unique(resource_ids.begin(), resource_ids.end()),
        resource_ids.end()
    );
    if (project_path.empty() || resource_ids.empty()) return;

    if (task_running_ &&
        active_project_path_ == project_path &&
        active_resource_ids_ == resource_ids) {
        return;
    }

    if (cancel_token_) cancel_token_->store(true, std::memory_order_relaxed);
    pool_.clear();
    ++generation_;
    task_running_ = false;
    active_project_path_ = std::move(project_path);
    active_resource_ids_ = std::move(resource_ids);
    start_request(active_project_path_, active_resource_ids_);
}

void ElevationDetailLoader::cancel() {
    if (cancel_token_) cancel_token_->store(true, std::memory_order_relaxed);
    pool_.clear();
    ++generation_;
    task_running_ = false;
    active_project_path_.clear();
    active_resource_ids_.clear();
}

void ElevationDetailLoader::accept_batch(
    const std::uint64_t generation,
    std::filesystem::path project_path,
    std::vector<ElevationDetailLoadResult> results
) {
    if (generation != generation_) return;
    task_running_ = false;
    active_resource_ids_.clear();
    if (result_callback_) {
        result_callback_(std::move(project_path), std::move(results));
    }
}

void ElevationDetailLoader::start_request(
    const std::filesystem::path& project_path,
    const std::vector<std::string>& resource_ids
) {
    ++generation_;
    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);
    task_running_ = true;
    pool_.start(new ElevationDetailTask(
        QPointer<ElevationDetailLoader>(this),
        project_path,
        resource_ids,
        cancel_token_,
        generation
    ));
}

}  // namespace aeris::desktop
