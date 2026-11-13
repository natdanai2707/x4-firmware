#include "OtaUpdater.h"

#include <HalStorage.h>

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
// x4-firmware: update from this fork's releases, never from upstream CrossPoint.
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/natdanai2707/x4-firmware/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  // Heap-allocated: the parser embeds a 2 KB JSON token buffer.
  auto releaseParserPtr = makeUniqueNoThrow<ReleaseJsonParser>();
  if (!releaseParserPtr) {
    LOG_ERR("OTA", "OOM: release parser");
    return OOM_ERROR;
  }
  ReleaseJsonParser& releaseParser = *releaseParserPtr;
  releaseParser.setFirmwareAssetName("");
  // Each board updates from crosspoint-<version>-<device>.bin. The combined
  // C3 image uses x3-x4; other asset suffixes match their firmware board tag.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetSuffix[20] = "-x3-x4";
  if (!isX4) {
    snprintf(assetSuffix, sizeof(assetSuffix), "-%.*s", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  char assetName[48] = {};
  bool assetNameSet = false;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&](const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (!assetNameSet && offset < len) {
      releaseParser.feed(reinterpret_cast<const char*>(data + offset), 1);
      offset++;
      if (releaseParser.foundTag()) {
        snprintf(assetName, sizeof(assetName), "crosspoint-%s%s.bin", releaseParser.getTagName(), assetSuffix);
        releaseParser.setFirmwareAssetName(assetName);
        assetNameSet = true;
      }
    }
    if (offset < len) releaseParser.feed(reinterpret_cast<const char*>(data + offset), len - offset);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_INF("OTA", "No %s asset in latest release", assetName);
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // x4-firmware: same path as SD Card Firmware Update, which works on the X4 Pro.
  // Download the image to the SD card, validate it, then write it raw into the
  // next OTA slot and switch otadata (firmware_flash). esp_ota_end() and
  // esp_ota_set_boot_partition() run esp_image_verify, which rejected our image.
  constexpr char kCachePath[] = "/.crosspoint/ota-update.bin";
  Storage.ensureDirectoryExists("/.crosspoint");
  processedSize = 0;
  int lastReportedPct = -1;
  const auto dl = HttpDownloader::downloadToFile(otaUrl, kCachePath, [&](size_t done, size_t total) {
    processedSize = done;
    if (total > 0) totalSize = total;
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
  });
  if (dl != HttpDownloader::OK) {
    LOG_ERR("OTA", "Firmware download failed (HTTP %d)", HttpDownloader::lastStatus());
    Storage.remove(kCachePath);
    return HTTP_ERROR;
  }

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  const auto valid = firmware_flash::validateImageFile(kCachePath, updatePartition ? updatePartition->size : 0);
  if (valid != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Downloaded image invalid: %s", firmware_flash::resultName(valid));
    Storage.remove(kCachePath);
    return valid == firmware_flash::Result::WRONG_BOARD || valid == firmware_flash::Result::BAD_CHIP
               ? WRONG_DEVICE_ERROR
               : INTERNAL_UPDATE_ERROR;
  }
  const auto flashed = firmware_flash::flashFromSdPath(kCachePath, nullptr, nullptr, true);
  Storage.remove(kCachePath);
  if (flashed != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Flash failed: %s", firmware_flash::resultName(flashed));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
