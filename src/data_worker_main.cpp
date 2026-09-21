// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_import.hpp"
#include "etopo_acquisition.hpp"
#include "etopo15_import.hpp"
#include "etopo15_tile.hpp"
#include "flag_pack_import.hpp"
#include "natural_earth_acquisition.hpp"
#include "world_data_import.hpp"

#include "aeris/storage/project.hpp"

#include <QCoreApplication>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr int kUsageFailure = 2;
constexpr int kProjectOpenFailure = 3;
constexpr int kImportFailure = 4;

void print_usage() {
    std::cerr
        << "usage: aeris-data-worker "
        << "<world|world-auto|flags|etopo|etopo-auto-surface|etopo-auto-bed|etopo15-auto-surface-rROW-cCOL> "
        << "<project.aeris> <source-or-cache-path> <modified-utc>\n";
}

void report_progress(const aeris::desktop::DataJobProgress& progress) {
    std::cout
        << "AERIS_DATA_JOB_PROGRESS"
        << " current=" << progress.current
        << " total=" << progress.total
        << " phase=" << progress.phase << '\n'
        << std::flush;
}

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
parse_etopo15_operation(const std::string_view operation) {
    constexpr std::string_view prefix = "etopo15-auto-surface-r";
    if (operation.size() <= prefix.size() ||
        operation.compare(0U, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    const std::size_t column_marker = operation.find("-c", prefix.size());
    if (column_marker == std::string_view::npos) return std::nullopt;

    std::uint32_t row = 0U;
    std::uint32_t column = 0U;
    const std::string_view row_text =
        operation.substr(prefix.size(), column_marker - prefix.size());
    const std::string_view column_text =
        operation.substr(column_marker + 2U);
    if (row_text.empty() || column_text.empty()) return std::nullopt;

    const auto row_result = std::from_chars(
        row_text.data(),
        row_text.data() + row_text.size(),
        row
    );
    const auto column_result = std::from_chars(
        column_text.data(),
        column_text.data() + column_text.size(),
        column
    );
    if (row_result.ec != std::errc{} ||
        row_result.ptr != row_text.data() + row_text.size() ||
        column_result.ec != std::errc{} ||
        column_result.ptr != column_text.data() + column_text.size()) {
        return std::nullopt;
    }
    return std::pair<std::uint32_t, std::uint32_t>{row, column};
}

[[nodiscard]] int report_result(
    const bool ok,
    const bool changed,
    const std::string& diagnostic,
    const std::size_t detail_tiles = 0U
) {
    std::cout
        << (ok ? "AERIS_DATA_JOB_OK" : "AERIS_DATA_JOB_FAIL")
        << " changed=" << (changed ? 1 : 0)
        << " detail_tiles=" << detail_tiles << '\n'
        << std::flush;
    if (!ok) {
        std::cerr << diagnostic << '\n';
        return kImportFailure;
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    if (argc != 5) {
        print_usage();
        return kUsageFailure;
    }

    const std::string_view operation(argv[1]);
    const std::filesystem::path project_path(argv[2]);
    const std::filesystem::path source_path(argv[3]);
    const std::string modified_utc(argv[4]);

    aeris::storage::ProjectStoreResult opened =
        aeris::storage::ProjectStore::open(project_path);
    if (!opened.ok()) {
        std::cerr
            << "could not open target .aeris project: "
            << opened.status.diagnostic << '\n';
        return kProjectOpenFailure;
    }

    if (operation == "world-auto") {
        const aeris::desktop::NaturalEarthAcquisitionResult acquired =
            aeris::desktop::acquire_natural_earth_110m_world(
                source_path,
                report_progress
            );
        if (!acquired.ok()) {
            return report_result(false, false, acquired.diagnostic);
        }
        report_progress({0U, 0U, "Importing verified base world into .aeris"});
        const aeris::desktop::WorldDataImportResult result =
            aeris::desktop::import_natural_earth_110m_world(
                *opened.store,
                acquired.snapshot_root,
                modified_utc
            );
        return report_result(result.ok(), result.changed, result.diagnostic);
    }

    if (operation == "world") {
        report_progress({0U, 0U, "Importing verified local Natural Earth snapshot"});
        const aeris::desktop::WorldDataImportResult result =
            aeris::desktop::import_natural_earth_110m_world(
                *opened.store,
                source_path,
                modified_utc
            );
        return report_result(
            result.ok(),
            result.changed,
            result.diagnostic
        );
    }

    if (operation == "flags") {
        report_progress({0U, 0U, "Verifying and embedding country flags"});
        const aeris::desktop::FlagPackImportResult result =
            aeris::desktop::import_country_flag_png_pack(
                *opened.store,
                source_path,
                modified_utc
            );
        return report_result(
            result.ok(),
            result.changed,
            result.diagnostic
        );
    }

    if (const auto cell = parse_etopo15_operation(operation);
        cell.has_value()) {
        const auto tile = aeris::desktop::etopo15_surface_tile_for_indices(
            cell->first,
            cell->second
        );
        if (!tile.has_value()) {
            return report_result(
                false,
                false,
                "ETOPO 15s operation references an out-of-range grid cell"
            );
        }

        const aeris::desktop::Etopo2022AcquisitionResult acquired =
            aeris::desktop::acquire_etopo2022_surface_15s_tile(
                *tile,
                source_path,
                report_progress
            );
        if (!acquired.ok()) {
            return report_result(false, false, acquired.diagnostic);
        }

        report_progress({
            0U,
            0U,
            "Decoding and materializing one ETOPO 15s viewport tile"
        });
        const aeris::desktop::Etopo15TileImportResult result =
            aeris::desktop::import_etopo2022_surface_15s_tile(
                *opened.store,
                *tile,
                acquired.geotiff_path,
                modified_utc
            );
        return report_result(
            result.ok(),
            result.changed,
            result.diagnostic,
            result.ok() ? 1U : 0U
        );
    }

    if (operation == "etopo-auto-surface" || operation == "etopo-auto-bed") {
        const aeris::desktop::Etopo2022Variant variant =
            operation == "etopo-auto-bed"
                ? aeris::desktop::Etopo2022Variant::bedrock
                : aeris::desktop::Etopo2022Variant::ice_surface;
        const aeris::desktop::Etopo2022AcquisitionResult acquired =
            aeris::desktop::acquire_etopo2022_global_60s(
                variant,
                source_path,
                report_progress
            );
        if (!acquired.ok()) {
            return report_result(false, false, acquired.diagnostic);
        }
        report_progress({0U, 0U, "Decoding, tiling and embedding acquired ETOPO elevation"});
        const aeris::desktop::ElevationImportResult result =
            aeris::desktop::import_etopo2022_global_60s(
                *opened.store,
                acquired.geotiff_path,
                modified_utc
            );
        return report_result(
            result.ok(),
            result.changed,
            result.diagnostic,
            result.detail_tiles
        );
    }

    if (operation == "etopo") {
        report_progress({0U, 0U, "Decoding, tiling and embedding ETOPO elevation"});
        const aeris::desktop::ElevationImportResult result =
            aeris::desktop::import_etopo2022_global_60s(
                *opened.store,
                source_path,
                modified_utc
            );
        return report_result(
            result.ok(),
            result.changed,
            result.diagnostic,
            result.detail_tiles
        );
    }

    std::cerr << "unknown data operation: " << operation << '\n';
    print_usage();
    return kUsageFailure;
}
