// SPDX-License-Identifier: GPL-3.0+
// kzgs: read-only peeks at PCSX2 renderer state that has no public accessor (measurement aids). Like kz_smaa_vt.cpp this
// translation unit sees the classes with the access specifiers macro-defined away; ext/pcsx2 is not modified.
#include "kzgs_internal.h"

#define private public
#define protected public
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/Renderers/Common/GSTexture.h"
#undef protected
#undef private

#include <cstdio>
#include <string>

struct KzTcAccess : GSTextureCache
{
	using GSTextureCache::m_dst;
};

namespace kzgs
{
	int ScanmaskUsed()
	{
		return g_gs_renderer ? static_cast<int>(g_gs_renderer->m_scanmask_used) : -1;
	}

	// One line per render target in PCSX2's texture cache (measurement aid).
	std::string DescribeTargets()
	{
		std::string out;
		if (!g_texture_cache)
			return out;
		KzTcAccess& tc = static_cast<KzTcAccess&>(*g_texture_cache);
		int idx = 0;
		char line[320];
		for (GSTextureCache::Target* t : tc.m_dst[GSTextureCache::RenderTarget])
		{
			std::snprintf(line, sizeof(line), "target #%d: TBP0 %05x TBW %u PSM %02x tex %dx%d age %d valid (%d,%d,%d,%d) drawn (%d,%d,%d,%d) used %d frame %d\n", idx++,
				t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, t->m_texture ? t->m_texture->GetWidth() : 0, t->m_texture ? t->m_texture->GetHeight() : 0, t->m_age,
				t->m_valid.x, t->m_valid.y, t->m_valid.z, t->m_valid.w, t->m_drawn_since_read.x, t->m_drawn_since_read.y, t->m_drawn_since_read.z,
				t->m_drawn_since_read.w, t->m_used, t->m_is_frame);
			out += line;
		}
		return out;
	}
} // namespace kzgs
