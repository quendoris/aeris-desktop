// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/view/scene.hpp"

#include <QImage>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QPainter;

namespace aeris::desktop {

inline constexpr std::size_t kFlagResourceLoadBatchLimit = 16U;

// Frontend-only decoded image cache. Durable PNG bytes remain in .aeris and are
// loaded separately by FlagResourceLoader after the renderer has selected a
// small viewport/collision-bounded set of symbols.
struct FlagRenderCache final {
    std::unordered_map<std::string, QImage> images;
    std::unordered_set<std::string> failed_resources;
    std::vector<std::string> pending_resources;
};

void begin_flag_render_pass(FlagRenderCache& cache);

void draw_country_flags(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const view::SceneGeometry& scene,
    const source::Result& source_result,
    FlagRenderCache& cache);

[[nodiscard]] const std::vector<std::string>& flag_resource_requests(
    const FlagRenderCache& cache) noexcept;

bool accept_flag_resource(
    FlagRenderCache& cache,
    std::string resource_id,
    QImage image);

void reject_flag_resource(
    FlagRenderCache& cache,
    std::string_view resource_id);

}  // namespace aeris::desktop
