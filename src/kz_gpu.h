#pragma once

#include <string>

// Name of the high-performance GPU (DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE), e.g. the discrete card on a laptop or a
// desktop that also has an iGPU. Empty if it cannot be determined (the renderer then uses its default adapter).
std::string kzPreferredGpuName();
