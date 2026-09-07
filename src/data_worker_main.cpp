// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_import.hpp"
#include "flag_pack_import.hpp"
#include "natural_earth_acquisition.hpp"
#include "world_data_import.hpp"

#include "aeris/storage/project.hpp"

#include <QCoreApplication>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr int kUsageFailure = 2;
constexpr int kProjectOpenFailure = 3;
constexpr int kImportFailure = 4;

void print_usage() {
    std::cerr
        << "usage: aeris-data-worker <world|world-auto|flags|etopo> "
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
