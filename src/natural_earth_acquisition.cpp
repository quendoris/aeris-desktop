// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "natural_earth_acquisition.hpp"

#include "aeris/util/sha256.hpp"

#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace aeris::desktop {
namespace {

constexpr std::string_view kCommit =
    "f1890d9f152c896d250a77557a5751a93d494776";
constexpr std::string_view kPhysicalBase =
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "f1890d9f152c896d250a77557a5751a93d494776/110m_physical/";
constexpr std::string_view kCulturalBase =
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "f1890d9f152c896d250a77557a5751a93d494776/110m_cultural/";

struct RemoteResource final {
    std::string filename;
    std::string url;
    std::string sha256;
    std::string phase;
};

struct LocalResource final {
    std::string filename;
    std::string bytes;
    std::string sha256;
};

[[nodiscard]] QString qt_path(const std::filesystem::path& path) {
    return QFile(path).fileName();
}

[[nodiscard]] bool verified_file(
    const std::filesystem::path& path,
    const std::string_view expected_sha
) {
    const util::Sha256FileResult hash = util::sha256_file(path);
    return hash.ok() && hash.digest.hex() == expected_sha;
}

[[nodiscard]] bool ensure_directory(
    const std::filesystem::path& root,
    std::string& diagnostic
) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        diagnostic = "could not create Natural Earth acquisition cache: " + error.message();
        return false;
    }
    if (!std::filesystem::is_directory(root, error) || error) {
        diagnostic = "Natural Earth acquisition cache is not a directory";
        return false;
    }
    return true;
}

[[nodiscard]] bool publish_verified_part(
    const std::filesystem::path& part,
    const std::filesystem::path& target,
    const std::string_view sha256,
    std::string& diagnostic
) {
    if (!verified_file(part, sha256)) {
        QFile::remove(qt_path(part));
        diagnostic = "downloaded Natural Earth resource failed SHA-256 verification: " +
            target.filename().string();
        return false;
    }

    QFile::remove(qt_path(target));
    if (!QFile::rename(qt_path(part), qt_path(target))) {
        diagnostic = "could not publish verified Natural Earth resource: " +
            target.filename().string();
        return false;
    }
    return true;
}

[[nodiscard]] bool download_resource(
    QNetworkAccessManager& network,
    const std::filesystem::path& root,
    const RemoteResource& resource,
    const DataJobProgressCallback& progress,
    std::string& diagnostic
) {
    const std::filesystem::path target = root / resource.filename;
    if (verified_file(target, resource.sha256)) {
        report_data_job_progress(progress, 1U, 1U, resource.phase + " · cached");
        return true;
    }

    QFile::remove(qt_path(target));
    const std::filesystem::path part = target.string() + ".part";

    std::error_code size_error;
    std::uintmax_t partial_size = 0U;
    if (std::filesystem::exists(part, size_error) && !size_error) {
        partial_size = std::filesystem::file_size(part, size_error);
        if (size_error) partial_size = 0U;
    }
    if (partial_size > static_cast<std::uintmax_t>(std::numeric_limits<qint64>::max())) {
        QFile::remove(qt_path(part));
        partial_size = 0U;
    }

    QFile output(qt_path(part));
    if (!output.open(QIODevice::ReadWrite)) {
        diagnostic = "could not open resumable download file: " + resource.filename;
        return false;
    }
    if (partial_size == 0U) {
        if (!output.resize(0)) {
            diagnostic = "could not reset resumable download file: " + resource.filename;
            return false;
        }
    }
    if (!output.seek(static_cast<qint64>(partial_size))) {
        diagnostic = "could not seek resumable download file: " + resource.filename;
        return false;
    }

    QNetworkRequest request(QUrl(QString::fromStdString(resource.url)));
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy
    );
    if (partial_size > 0U) {
        request.setRawHeader(
            QByteArrayLiteral("Range"),
            QByteArrayLiteral("bytes=") +
                QByteArray::number(static_cast<qulonglong>(partial_size)) +
                QByteArrayLiteral("-")
        );
    }

    QNetworkReply* reply = network.get(request);
    if (reply == nullptr) {
        diagnostic = "could not start Natural Earth download: " + resource.filename;
        return false;
    }

    qint64 base_offset = static_cast<qint64>(partial_size);
    bool response_mode_ready = false;
    bool write_failed = false;

    const auto prepare_response_mode = [&]() {
        if (response_mode_ready) return;
        const QVariant raw_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!raw_status.isValid()) return;
        const int status = raw_status.toInt();
        if (base_offset > 0 && status != 206) {
            // The immutable URL is safe to restart, but appending a full 200
            // response to an old partial file would corrupt it.
            if (!output.resize(0) || !output.seek(0)) {
                write_failed = true;
                return;
            }
            base_offset = 0;
        }
        response_mode_ready = true;
    };

    const auto drain = [&]() {
        prepare_response_mode();
        if (write_failed) {
            reply->readAll();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty() && output.write(chunk) != chunk.size()) {
            write_failed = true;
        }
    };

    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);
    QObject::connect(
        reply,
        &QNetworkReply::downloadProgress,
        reply,
        [&](const qint64 received, const qint64 total) {
            const std::uint64_t current = static_cast<std::uint64_t>(
                std::max<qint64>(0, base_offset + received)
            );
            const std::uint64_t expected = total > 0
                ? static_cast<std::uint64_t>(std::max<qint64>(0, base_offset + total))
                : 0U;
            report_data_job_progress(progress, current, expected, resource.phase);
        }
    );

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    drain();
    output.flush();
    output.close();

    const QVariant raw_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = raw_status.isValid() ? raw_status.toInt() : 0;
    const QNetworkReply::NetworkError network_error = reply->error();
    const QString error_text = reply->errorString();
    reply->deleteLater();

    if (write_failed) {
        diagnostic = "could not write Natural Earth partial download: " + resource.filename;
        return false;
    }

    if (network_error != QNetworkReply::NoError) {
        // 416 can mean the .part file already contains the complete immutable
        // object. Verify it before deciding whether this is actually failure.
        if (status == 416 && verified_file(part, resource.sha256)) {
            return publish_verified_part(part, target, resource.sha256, diagnostic);
        }
        diagnostic = "Natural Earth download interrupted for " + resource.filename +
            ": " + error_text.toStdString() +
            " · partial bytes retained for resume";
        return false;
    }

    if (status < 200 || status >= 300) {
        diagnostic = "Natural Earth server returned HTTP " + std::to_string(status) +
            " for " + resource.filename + " · partial bytes retained for resume";
        return false;
    }

    if (!publish_verified_part(part, target, resource.sha256, diagnostic)) return false;
    report_data_job_progress(progress, 1U, 1U, resource.phase + " · verified");
    return true;
}

[[nodiscard]] bool materialize_local_resource(
    const std::filesystem::path& root,
    const LocalResource& resource,
    std::string& diagnostic
) {
    const std::filesystem::path target = root / resource.filename;
    if (verified_file(target, resource.sha256)) return true;

    const std::filesystem::path temporary = target.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            diagnostic = "could not create Natural Earth companion: " + resource.filename;
            return false;
        }
        output.write(
            resource.bytes.data(),
            static_cast<std::streamsize>(resource.bytes.size())
        );
        if (!output) {
            diagnostic = "could not write Natural Earth companion: " + resource.filename;
            return false;
        }
    }

    if (!verified_file(temporary, resource.sha256)) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        diagnostic = "built-in Natural Earth companion bytes failed SHA-256 verification: " +
            resource.filename;
        return false;
    }

    std::error_code error;
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temporary, target, error);
    if (error) {
        diagnostic = "could not publish Natural Earth companion: " + resource.filename +
            ": " + error.message();
        return false;
    }
    return true;
}

}  // namespace

NaturalEarthAcquisitionResult acquire_natural_earth_110m_world(
    const std::filesystem::path& cache_root,
    const DataJobProgressCallback& progress
) {
    if (cache_root.empty()) {
        return {false, {}, "Natural Earth acquisition requires a cache directory"};
    }

    std::string diagnostic;
    if (!ensure_directory(cache_root, diagnostic)) return {false, {}, std::move(diagnostic)};

    const std::array<RemoteResource, 3> remote{{
        {
            "ne_110m_land.shp",
            std::string(kPhysicalBase) + "ne_110m_land.shp",
            "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de",
            "Downloading base land geometry",
        },
        {
            "ne_110m_admin_0_countries.shp",
            std::string(kCulturalBase) + "ne_110m_admin_0_countries.shp",
            "08e341606e8391e458c3f08deb312de664b56bfae376064c5aa0aee6681a5f55",
            "Downloading political boundaries",
        },
        {
            "ne_110m_admin_0_countries.dbf",
            std::string(kCulturalBase) + "ne_110m_admin_0_countries.dbf",
            "1fee677cd4e03b367876e03861eb10197e4022a846bf92060e0313432863785b",
            "Downloading political attributes",
        },
    }};

    const std::string wgs84_prj =
        "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\","
        "6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\","
        "0.017453292519943295]]";
    const std::array<LocalResource, 5> local{{
        {"ne_110m_land.prj", wgs84_prj,
         "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"},
        {"ne_110m_admin_0_countries.prj", wgs84_prj,
         "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"},
        {"ne_110m_land.VERSION.txt", "4.1.0\n",
         "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7"},
        {"ne_110m_admin_0_countries.cpg", "UTF-8",
         "3ad3031f5503a4404af825262ee8232cc04d4ea6683d42c5dd0a2f2a27ac9824"},
        {"ne_110m_admin_0_countries.VERSION.txt", "5.1.1\n",
         "f9893302cd3158f3b5aea394dcd2a91574869e9e6ff69e9235b10a3bf8c983fb"},
    }};

    QNetworkAccessManager network;
    for (const RemoteResource& resource : remote) {
        if (!download_resource(network, cache_root, resource, progress, diagnostic)) {
            return {false, cache_root, std::move(diagnostic)};
        }
    }

    report_data_job_progress(progress, 0U, 0U, "Materializing verified metadata companions");
    for (const LocalResource& resource : local) {
        if (!materialize_local_resource(cache_root, resource, diagnostic)) {
            return {false, cache_root, std::move(diagnostic)};
        }
    }

    report_data_job_progress(
        progress,
        1U,
        1U,
        "Pinned Natural Earth snapshot verified"
    );
    return {
        true,
        cache_root,
        "pinned Natural Earth snapshot acquired from immutable upstream commit " +
            std::string(kCommit),
    };
}

}  // namespace aeris::desktop
