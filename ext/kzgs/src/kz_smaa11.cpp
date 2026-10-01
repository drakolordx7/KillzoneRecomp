// SPDX-License-Identifier: GPL-3.0+
// kzgs: SMAA 1x for the Direct3D 11 renderer.
//
// PCSX2 has no SMAA. Its post chain (GSRenderer::Merge) runs GSDevice::FXAA() on the merged frame, at internal
// resolution and before the final scale to the window, when GSOptions::FXAA is set. That ends in the virtual
// GSDevice::DoFXAA(src, dst). kzgs sets the flag for SMAA and replaces the D3D11 device's DoFXAA with a three-pass SMAA
// 1x (Jimenez et al., MIT licence: the unmodified SMAA.hlsl is in shaders/, the Area/Search textures in third_party/smaa):
// luma edge detection -> blend weights -> neighbourhood blending, all with raw D3D11 calls and the pipeline state saved
// and restored around them (PCSX2's device caches its state and does not expect it to change under it).
// ext/pcsx2 is not modified: GSDevice11 is final and has no hook, so the live device gets a copy of its vtable with the
// DoFXAA slot replaced (the slot index comes from kz_smaa_vt.cpp). If anything fails the original FXAA runs instead.

#include "kzgs_internal.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/DX11/GSDevice11.h"
#include "GS/Renderers/DX11/GSTexture11.h"

#include "Config.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wil/com.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "AreaTex.h"
#include "SearchTex.h"

namespace kzgs
{
	int FxaaVtableIndex(); // kz_smaa_vt.cpp
}

namespace
{
	std::atomic<bool> s_enabled{false};
	bool s_failed = false;

	using DoFxaaFn = void (*)(GSDevice11*, GSTexture*, GSTexture*);
	DoFxaaFn s_origDoFxaa = nullptr;
	void** s_vtableCopy = nullptr; // one block: [-1] = RTTI slot, then the entries
	void** s_origVtable = nullptr;
	int s_slot = -1;

	// Pass entry points. SMAA.hlsl (SMAA_HLSL_4_1, HIGH preset) is read from resources/shaders/kzgs and prepended.
	const char* kPasses = R"HLSL(
Texture2D g_t0 : register(t0);
Texture2D g_t1 : register(t1);
Texture2D g_t2 : register(t2);

struct VSOut
{
	float4 p : SV_Position;
	float2 t : TEXCOORD0;
};

VSOut vs_tri(uint id : SV_VertexID)
{
	VSOut o;
	o.t = float2((id << 1) & 2, id & 2);
	o.p = float4(o.t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 ps_edge(VSOut i) : SV_Target
{
	float4 off[3];
	SMAAEdgeDetectionVS(i.t, off);
	return float4(SMAALumaEdgeDetectionPS(i.t, off, g_t0), 0.0, 0.0);
}

float4 ps_weights(VSOut i) : SV_Target
{
	float2 pix;
	float4 off[3];
	SMAABlendingWeightCalculationVS(i.t, pix, off);
	return SMAABlendingWeightCalculationPS(i.t, pix, off, g_t0, g_t1, g_t2, float4(0.0, 0.0, 0.0, 0.0));
}

float4 ps_blend(VSOut i) : SV_Target
{
	float4 off;
	SMAANeighborhoodBlendingVS(i.t, off);
	return SMAANeighborhoodBlendingPS(i.t, off, g_t0, g_t1);
}
)HLSL";

	struct Resources
	{
		ID3D11Device* dev = nullptr; // identity of the device these belong to
		int w = 0, h = 0;
		wil::com_ptr_nothrow<ID3D11VertexShader> vs;
		wil::com_ptr_nothrow<ID3D11PixelShader> psEdge, psWeights, psBlend;
		wil::com_ptr_nothrow<ID3D11ShaderResourceView> areaSRV, searchSRV;
		wil::com_ptr_nothrow<ID3D11Texture2D> edgesTex, blendTex;
		wil::com_ptr_nothrow<ID3D11ShaderResourceView> edgesSRV, blendSRV;
		wil::com_ptr_nothrow<ID3D11RenderTargetView> edgesRTV, blendRTV;
		wil::com_ptr_nothrow<ID3D11SamplerState> linear, point;
		wil::com_ptr_nothrow<ID3D11RasterizerState> rs;
		wil::com_ptr_nothrow<ID3D11DepthStencilState> dss;
		wil::com_ptr_nothrow<ID3D11BlendState> bs;
		std::string source; // SMAA.hlsl
	};
	Resources s_res;

	bool Compile(ID3D11Device* d, const std::string& code, const D3D_SHADER_MACRO* macros, const char* entry, const char* target,
		wil::com_ptr_nothrow<ID3DBlob>& out)
	{
		wil::com_ptr_nothrow<ID3DBlob> err;
		const HRESULT hr = D3DCompile(code.data(), code.size(), "kzgs_smaa", macros, nullptr, entry, target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.put(), err.put());
		if (FAILED(hr))
		{
			Console.Error("kzgs SMAA: shader %s failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
			return false;
		}
		return true;
	}

	bool MakeTarget(ID3D11Device* d, int w, int h, DXGI_FORMAT fmt, wil::com_ptr_nothrow<ID3D11Texture2D>& tex,
		wil::com_ptr_nothrow<ID3D11ShaderResourceView>& srv, wil::com_ptr_nothrow<ID3D11RenderTargetView>& rtv)
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		return SUCCEEDED(d->CreateTexture2D(&td, nullptr, tex.put())) && SUCCEEDED(d->CreateShaderResourceView(tex.get(), nullptr, srv.put())) &&
			   SUCCEEDED(d->CreateRenderTargetView(tex.get(), nullptr, rtv.put()));
	}

	bool MakeImmutable(ID3D11Device* d, int w, int h, DXGI_FORMAT fmt, const void* data, int pitch, wil::com_ptr_nothrow<ID3D11ShaderResourceView>& srv)
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{data, static_cast<UINT>(pitch), 0};
		wil::com_ptr_nothrow<ID3D11Texture2D> tex;
		return SUCCEEDED(d->CreateTexture2D(&td, &init, tex.put())) && SUCCEEDED(d->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
	}

	bool Prepare(ID3D11Device* d, int w, int h)
	{
		if (s_res.dev != d)
			s_res = Resources{};
		s_res.dev = d;
		if (!s_res.areaSRV)
		{
			const std::string path = Path::Combine(EmuFolders::Resources, "shaders/kzgs/SMAA.hlsl");
			const std::optional<std::string> src = FileSystem::ReadFileToString(path.c_str());
			if (!src.has_value())
			{
				Console.Error("kzgs SMAA: %s is missing", path.c_str());
				return false;
			}
			s_res.source = *src;
			if (!MakeImmutable(d, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, AREATEX_PITCH, s_res.areaSRV) ||
				!MakeImmutable(d, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, searchTexBytes, SEARCHTEX_PITCH, s_res.searchSRV))
				return false;
			D3D11_SAMPLER_DESC sd{};
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			if (FAILED(d->CreateSamplerState(&sd, s_res.linear.put())))
				return false;
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			if (FAILED(d->CreateSamplerState(&sd, s_res.point.put())))
				return false;
			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			D3D11_DEPTH_STENCIL_DESC dd{};
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(d->CreateRasterizerState(&rd, s_res.rs.put())) || FAILED(d->CreateDepthStencilState(&dd, s_res.dss.put())) ||
				FAILED(d->CreateBlendState(&bd, s_res.bs.put())))
				return false;
		}
		if (!s_res.psEdge || s_res.w != w || s_res.h != h)
		{
			char metrics[160];
			std::snprintf(metrics, sizeof(metrics), "float4(%.12f, %.12f, %d.0, %d.0)", 1.0 / w, 1.0 / h, w, h);
			// HIGH preset by default; KZGS_SMAA_THRESHOLD / _STEPS / _DIAG / _CORNER override it (measurement, docs/findings.md)
			const auto envf = [](const char* n, double def) { const char* v = std::getenv(n); return v && *v ? std::atof(v) : def; };
			char thr[32], steps[32], stepsDiag[32], corner[32];
			std::snprintf(thr, sizeof(thr), "%.4f", envf("KZGS_SMAA_THRESHOLD", 0.1));
			std::snprintf(steps, sizeof(steps), "%d", static_cast<int>(envf("KZGS_SMAA_STEPS", 16)));
			std::snprintf(stepsDiag, sizeof(stepsDiag), "%d", static_cast<int>(envf("KZGS_SMAA_DIAG", 8)));
			std::snprintf(corner, sizeof(corner), "%d", static_cast<int>(envf("KZGS_SMAA_CORNER", 25)));
			std::vector<D3D_SHADER_MACRO> macroList = {{"SMAA_HLSL_4_1", "1"}, {"SMAA_RT_METRICS", metrics}, {"SMAA_THRESHOLD", thr},
			                                           {"SMAA_MAX_SEARCH_STEPS", steps}, {"SMAA_MAX_SEARCH_STEPS_DIAG", stepsDiag}, {"SMAA_CORNER_ROUNDING", corner}};
			if (static_cast<int>(envf("KZGS_SMAA_DIAG", 8)) <= 0)
				macroList.push_back({"SMAA_DISABLE_DIAG_DETECTION", "1"});
			if (static_cast<int>(envf("KZGS_SMAA_CORNER", 25)) >= 100)
				macroList.push_back({"SMAA_DISABLE_CORNER_DETECTION", "1"});
			macroList.push_back({nullptr, nullptr});
			const D3D_SHADER_MACRO* macros = macroList.data();
			const std::string code = s_res.source + "\n" + kPasses;
			wil::com_ptr_nothrow<ID3DBlob> vsb, pe, pw, pb;
			if (!Compile(d, code, macros, "vs_tri", "vs_5_0", vsb) || !Compile(d, code, macros, "ps_edge", "ps_5_0", pe) ||
				!Compile(d, code, macros, "ps_weights", "ps_5_0", pw) || !Compile(d, code, macros, "ps_blend", "ps_5_0", pb))
				return false;
			if (FAILED(d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, s_res.vs.put())) ||
				FAILED(d->CreatePixelShader(pe->GetBufferPointer(), pe->GetBufferSize(), nullptr, s_res.psEdge.put())) ||
				FAILED(d->CreatePixelShader(pw->GetBufferPointer(), pw->GetBufferSize(), nullptr, s_res.psWeights.put())) ||
				FAILED(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, s_res.psBlend.put())))
				return false;
			s_res.edgesTex.reset();
			s_res.edgesSRV.reset();
			s_res.edgesRTV.reset();
			s_res.blendTex.reset();
			s_res.blendSRV.reset();
			s_res.blendRTV.reset();
			if (!MakeTarget(d, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s_res.edgesTex, s_res.edgesSRV, s_res.edgesRTV) ||
				!MakeTarget(d, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s_res.blendTex, s_res.blendSRV, s_res.blendRTV))
				return false;
			s_res.w = w;
			s_res.h = h;
		}
		return true;
	}

	// The part of the pipeline state the passes touch, so PCSX2's cached state stays true.
	struct SavedState
	{
		ID3D11InputLayout* il = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11Buffer* vb[2] = {};
		UINT vbStride[2] = {}, vbOffset[2] = {};
		ID3D11Buffer* ib = nullptr;
		DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
		UINT ibOffset = 0;
		ID3D11VertexShader* vs = nullptr;
		ID3D11Buffer* vsCb[2] = {};
		ID3D11GeometryShader* gs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		ID3D11Buffer* psCb[2] = {};
		ID3D11ShaderResourceView* psSrv[4] = {};
		ID3D11SamplerState* psSs[2] = {};
		ID3D11RasterizerState* rs = nullptr;
		D3D11_VIEWPORT vp[1] = {};
		UINT nvp = 1;
		D3D11_RECT sc[1] = {};
		UINT nsc = 1;
		ID3D11RenderTargetView* rtv[1] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		ID3D11BlendState* bs = nullptr;
		FLOAT bf[4] = {};
		UINT sm = 0;
		ID3D11DepthStencilState* dss = nullptr;
		UINT sref = 0;

		void Save(ID3D11DeviceContext* c)
		{
			c->IAGetInputLayout(&il);
			c->IAGetPrimitiveTopology(&topo);
			c->IAGetVertexBuffers(0, 2, vb, vbStride, vbOffset);
			c->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);
			c->VSGetShader(&vs, nullptr, nullptr);
			c->VSGetConstantBuffers(0, 2, vsCb);
			c->GSGetShader(&gs, nullptr, nullptr);
			c->PSGetShader(&ps, nullptr, nullptr);
			c->PSGetConstantBuffers(0, 2, psCb);
			c->PSGetShaderResources(0, 4, psSrv);
			c->PSGetSamplers(0, 2, psSs);
			c->RSGetState(&rs);
			c->RSGetViewports(&nvp, vp);
			c->RSGetScissorRects(&nsc, sc);
			c->OMGetRenderTargets(1, rtv, &dsv);
			c->OMGetBlendState(&bs, bf, &sm);
			c->OMGetDepthStencilState(&dss, &sref);
		}

		template <class T>
		static void Rel(T*& p)
		{
			if (p)
				p->Release();
			p = nullptr;
		}

		void Restore(ID3D11DeviceContext* c)
		{
			c->OMSetRenderTargets(0, nullptr, nullptr);
			c->IASetInputLayout(il);
			c->IASetPrimitiveTopology(topo);
			c->IASetVertexBuffers(0, 2, vb, vbStride, vbOffset);
			c->IASetIndexBuffer(ib, ibFmt, ibOffset);
			c->VSSetShader(vs, nullptr, 0);
			c->VSSetConstantBuffers(0, 2, vsCb);
			c->GSSetShader(gs, nullptr, 0);
			c->PSSetShader(ps, nullptr, 0);
			c->PSSetConstantBuffers(0, 2, psCb);
			c->PSSetShaderResources(0, 4, psSrv);
			c->PSSetSamplers(0, 2, psSs);
			c->RSSetState(rs);
			c->RSSetViewports(nvp, vp);
			c->RSSetScissorRects(nsc, sc);
			c->OMSetRenderTargets(1, rtv, dsv);
			c->OMSetBlendState(bs, bf, sm);
			c->OMSetDepthStencilState(dss, sref);
			Rel(il);
			Rel(ib);
			Rel(vs);
			Rel(gs);
			Rel(ps);
			Rel(rs);
			Rel(dsv);
			Rel(rtv[0]);
			Rel(bs);
			Rel(dss);
			for (auto& b : vb)
				Rel(b);
			for (auto& b : vsCb)
				Rel(b);
			for (auto& b : psCb)
				Rel(b);
			for (auto& b : psSrv)
				Rel(b);
			for (auto& b : psSs)
				Rel(b);
		}
	};

	void Draw(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, int w, int h)
	{
		c->OMSetRenderTargets(1, &rtv, nullptr);
		const D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
		c->RSSetViewports(1, &vp);
		c->PSSetShader(ps, nullptr, 0);
		c->Draw(3, 0);
	}

	bool RunSmaa(GSDevice11* dev, GSTexture* sTex, GSTexture* dTex)
	{
		ID3D11DeviceContext* c = dev->GetD3DContext();
		const int w = dTex->GetWidth(), h = dTex->GetHeight();
		// A texture that still holds a pending clear has no pixels to read: leave that rare case to the original FXAA.
		if (!c || sTex->GetState() != GSTexture::State::Dirty || sTex->GetWidth() != w || sTex->GetHeight() != h ||
			!Prepare(dev->GetD3DDevice(), w, h))
			return false;
		ID3D11ShaderResourceView* src = *static_cast<GSTexture11*>(sTex);
		ID3D11RenderTargetView* dst = *static_cast<GSTexture11*>(dTex);
		if (!src || !dst)
			return false;
		dTex->SetState(GSTexture::State::Dirty);

		SavedState st;
		st.Save(c);

		c->IASetInputLayout(nullptr);
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->VSSetShader(s_res.vs.get(), nullptr, 0);
		c->GSSetShader(nullptr, nullptr, 0);
		c->RSSetState(s_res.rs.get());
		c->RSSetScissorRects(0, nullptr);
		c->OMSetBlendState(s_res.bs.get(), nullptr, 0xFFFFFFFFu);
		c->OMSetDepthStencilState(s_res.dss.get(), 0);
		ID3D11SamplerState* ss[2] = {s_res.linear.get(), s_res.point.get()};
		c->PSSetSamplers(0, 2, ss);
		ID3D11ShaderResourceView* none[3] = {};
		const float zero[4] = {0, 0, 0, 0};

		// 1: luma edges (the shader discards non-edge pixels, so the target starts cleared).
		c->OMSetRenderTargets(0, nullptr, nullptr);
		c->PSSetShaderResources(0, 3, none);
		c->ClearRenderTargetView(s_res.edgesRTV.get(), zero);
		c->PSSetShaderResources(0, 1, &src);
		Draw(c, s_res.psEdge.get(), s_res.edgesRTV.get(), w, h);

		// 2: blend weights from the edges (t0), the area texture (t1) and the search texture (t2).
		c->OMSetRenderTargets(0, nullptr, nullptr);
		c->PSSetShaderResources(0, 3, none);
		c->ClearRenderTargetView(s_res.blendRTV.get(), zero);
		ID3D11ShaderResourceView* w3[3] = {s_res.edgesSRV.get(), s_res.areaSRV.get(), s_res.searchSRV.get()};
		c->PSSetShaderResources(0, 3, w3);
		Draw(c, s_res.psWeights.get(), s_res.blendRTV.get(), w, h);

		// 3: neighbourhood blending of the colour (t0) with the weights (t1).
		c->OMSetRenderTargets(0, nullptr, nullptr);
		c->PSSetShaderResources(0, 3, none);
		ID3D11ShaderResourceView* n2[2] = {src, s_res.blendSRV.get()};
		c->PSSetShaderResources(0, 2, n2);
		Draw(c, s_res.psBlend.get(), dst, w, h);

		c->OMSetRenderTargets(0, nullptr, nullptr);
		c->PSSetShaderResources(0, 3, none);
		st.Restore(c);
		return true;
	}

	// SMAA was asked for but cannot run: pass the frame through unchanged (the original FXAA would blur it, which is what
	// SMAA users are avoiding).
	void PassThrough(GSDevice11* dev, GSTexture* sTex, GSTexture* dTex)
	{
		ID3D11DeviceContext* c = dev->GetD3DContext();
		ID3D11Texture2D* src = *static_cast<GSTexture11*>(sTex);
		ID3D11Texture2D* dst = *static_cast<GSTexture11*>(dTex);
		if (c && src && dst && sTex->GetState() == GSTexture::State::Dirty && sTex->GetWidth() == dTex->GetWidth() &&
			sTex->GetHeight() == dTex->GetHeight())
		{
			c->CopyResource(dst, src);
			dTex->SetState(GSTexture::State::Dirty);
		}
		else
			s_origDoFxaa(dev, sTex, dTex);
	}

	void KzDoFXAA(GSDevice11* dev, GSTexture* sTex, GSTexture* dTex)
	{
		if (s_enabled.load())
		{
			if (!s_failed && RunSmaa(dev, sTex, dTex))
				return;
			// A pending-clear source is a one-off; a missing shader file or a compile error is not.
			if (!s_failed && sTex->GetState() == GSTexture::State::Dirty)
			{
				s_failed = true;
				Console.Error("kzgs: SMAA failed, frames pass through unchanged");
			}
			PassThrough(dev, sTex, dTex);
			return;
		}
		s_origDoFxaa(dev, sTex, dTex);
	}
} // namespace

namespace kzgs
{
	void SmaaSetEnabled(bool on)
	{
		s_enabled.store(on);
		if (on)
			s_failed = false;
	}

	void SmaaInstall()
	{
		if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D11)
			return;
		void** live = *reinterpret_cast<void***>(g_gs_device.get());
		if (s_vtableCopy && live == s_vtableCopy + 1)
			return; // already installed on this device
		if (s_vtableCopy && s_origVtable != live)
		{
			// A new device (renderer switch): start over with its vtable. The old copy is leaked on purpose (tiny).
			s_vtableCopy = nullptr;
			s_slot = -1;
		}
		if (!s_vtableCopy)
		{
			s_slot = FxaaVtableIndex();
			if (s_slot < 0)
			{
				Console.Error("kzgs: SMAA unavailable (DoFXAA vtable slot not found)");
				return;
			}
			constexpr int kEntries = 192;
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(live - 1, &mbi, sizeof(mbi)) ||
				static_cast<size_t>(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - reinterpret_cast<uint8_t*>(live - 1)) < (kEntries + 1) * sizeof(void*))
			{
				Console.Error("kzgs: SMAA unavailable (vtable region too small)");
				return;
			}
			s_vtableCopy = static_cast<void**>(std::calloc(kEntries + 1, sizeof(void*)));
			std::memcpy(s_vtableCopy, live - 1, (kEntries + 1) * sizeof(void*));
			s_origVtable = live;
			s_origDoFxaa = reinterpret_cast<DoFxaaFn>(live[s_slot]);
			s_vtableCopy[1 + s_slot] = reinterpret_cast<void*>(&KzDoFXAA);
		}
		*reinterpret_cast<void***>(g_gs_device.get()) = s_vtableCopy + 1;
	}
} // namespace kzgs
