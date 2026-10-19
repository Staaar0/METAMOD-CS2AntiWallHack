#pragma once
namespace awh {
inline constexpr const char* DefaultConfig = "// AntiWallHack configuration. Reload the plugin after editing.\n\nEnabled                      1       // Enable visibility filtering.\nDebug                        0       // Temporary startup/map diagnostic breadcrumbs.\nSmokeOcclusion               1       // Enable smoke checks.\nVisibleGraceTicks            20      // Visibility hold.\nBoundsPadding                28      // Wall sample padding.\nCornerPrediction             1       // Bounded, wall-clipped viewer input/velocity prediction.\nCornerPredictionSeconds      0.2     // Viewer horizon, 0..0.2 seconds.\nCornerMaxDistance            64      // Maximum viewer lead, 0..64 units.\n";
}
