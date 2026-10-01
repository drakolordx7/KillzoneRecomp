// SPDX-License-Identifier: GPL-3.0+
// kzgs: capture of the pixels that actually reach the Direct3D 11 swap chain (measurement aid, KZ_SHOT_BURST_PRESENT=2).
//
// PCSX2's GSDevice11 presents with IDXGISwapChain::Present right after the final scale to the window. When capture is on,
// the swap chain object's vtable pointer is switched to a copy whose Present slot first copies back buffer 0 to a staging
// texture (back buffer 0 holds the finished frame at that point, nothing is bound to it) and then calls the real Present.
// Unlike GSSaveSnapshotToMemory this sees the real final image (Bilinear Sharp pre-scale, CAS, crop, aspect) and takes
// no render target out of PCSX2's pool. ext/pcsx2 is not modified; GSDevice11::m_swap_chain is private, hence the macro.

#include "kzgs_internal.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/DX11/GSTexture11.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wil/com.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#define private public
#include "GS/Renderers/DX11/GSDevice11.h"
#undef private

namespace
{
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

	void** s_origVtable = nullptr;
	void** s_vtableCopy = nullptr;
	void*** s_object = nullptr; // the swap chain object whose vptr we switched
	PresentFn s_origPresent = nullptr;
	kzgs::PresentSink s_sink = nullptr;
	std::vector<uint8_t> s_rgba;

	void Capture(IDXGISwapChain* sc)
	{
		wil::com_ptr_nothrow<ID3D11Texture2D> bb;
		if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(bb.put()))))
			return;
		D3D11_TEXTURE2D_DESC d = {};
		bb->GetDesc(&d);
		if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
			return;
		wil::com_ptr_nothrow<ID3D11Device> dev;
		bb->GetDevice(dev.put());
		wil::com_ptr_nothrow<ID3D11DeviceContext> ctx;
		dev->GetImmediateContext(ctx.put());
		D3D11_TEXTURE2D_DESC sd = d;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.MiscFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		wil::com_ptr_nothrow<ID3D11Texture2D> staging;
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging.put())))
			return;
		ctx->CopyResource(staging.get(), bb.get());
		D3D11_MAPPED_SUBRESOURCE m = {};
		if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
			return;
		s_rgba.resize(static_cast<size_t>(d.Width) * d.Height * 4);
		const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
		for (UINT y = 0; y < d.Height; y++)
		{
			const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
			uint8_t* dst = &s_rgba[static_cast<size_t>(y) * d.Width * 4];
			if (!bgra)
				std::memcpy(dst, src, static_cast<size_t>(d.Width) * 4);
			else
				for (UINT x = 0; x < d.Width; x++)
				{
					dst[x * 4 + 0] = src[x * 4 + 2];
					dst[x * 4 + 1] = src[x * 4 + 1];
					dst[x * 4 + 2] = src[x * 4 + 0];
					dst[x * 4 + 3] = 255;
				}
		}
		ctx->Unmap(staging.get(), 0);
		if (s_sink)
			s_sink(s_rgba.data(), static_cast<int>(d.Width), static_cast<int>(d.Height));
	}

	HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags)
	{
		if (s_sink)
			Capture(self);
		return s_origPresent(self, sync, flags);
	}
}

namespace kzgs
{
	bool PresentCapture(PresentSink sink)
	{
		s_sink = sink;
		auto* dev = g_gs_device ? dynamic_cast<GSDevice11*>(g_gs_device.get()) : nullptr;
		if (!dev || !dev->m_swap_chain)
			return false;
		void*** obj = reinterpret_cast<void***>(dev->m_swap_chain.get());
		if (sink)
		{
			if (s_object == obj && *obj == s_vtableCopy)
				return true;
			if (s_object && *s_object == s_vtableCopy)
				*s_object = s_origVtable;
			delete[] s_vtableCopy;
			s_object = obj;
			s_origVtable = *obj;
			s_vtableCopy = new void*[32];
			std::memcpy(s_vtableCopy, s_origVtable, 32 * sizeof(void*));
			s_origPresent = reinterpret_cast<PresentFn>(s_origVtable[8]);
			s_vtableCopy[8] = reinterpret_cast<void*>(&HookPresent);
			*obj = s_vtableCopy;
		}
		return true;
	}
} // namespace kzgs
