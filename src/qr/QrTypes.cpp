#include "qr/QrTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace qr {

namespace {

// Each setting's values, lowest level first.
constexpr std::array DENSITIES = {QrDensity::Lowest, QrDensity::Low, QrDensity::Medium,
                                  QrDensity::High, QrDensity::Highest};
constexpr std::array SPEEDS = {QrSpeed::Slowest, QrSpeed::Slow, QrSpeed::Medium, QrSpeed::Fast,
                               QrSpeed::Fastest};
constexpr std::array CONTRASTS = {QrContrast::Lowest, QrContrast::Low, QrContrast::Medium,
                                  QrContrast::High, QrContrast::Highest};
constexpr std::array CAMERA_CONTRASTS = {QrCameraContrast::Lowest, QrCameraContrast::Low,
                                         QrCameraContrast::Medium, QrCameraContrast::High,
                                         QrCameraContrast::Highest};

auto indexOf(int level) -> std::size_t {
    return static_cast<std::size_t>(std::clamp(level, MIN_LEVEL, MAX_LEVEL) - MIN_LEVEL);
}

} // namespace

auto densityFromLevel(int level) -> QrDensity {
    return DENSITIES.at(indexOf(level));
}

auto speedFromLevel(int level) -> QrSpeed {
    return SPEEDS.at(indexOf(level));
}

auto contrastFromLevel(int level) -> QrContrast {
    return CONTRASTS.at(indexOf(level));
}

auto cameraContrastFromLevel(int level) -> QrCameraContrast {
    return CAMERA_CONTRASTS.at(indexOf(level));
}

auto cameraContrastLevel(QrCameraContrast contrast) -> int {
    const auto *found = std::ranges::find(CAMERA_CONTRASTS, contrast);
    return MIN_LEVEL + static_cast<int>(found - CAMERA_CONTRASTS.begin());
}

} // namespace qr
