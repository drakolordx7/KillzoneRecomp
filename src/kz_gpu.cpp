#include "kz_gpu.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "dxgi.lib")

std::string kzPreferredGpuName()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        return name;
    }
    return {};
}
