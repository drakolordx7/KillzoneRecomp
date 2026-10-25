// kzgs shim: CPU features and the vector ISA for multi-ISA dispatch. Replaces PCSX2's GS/MultiISA.cpp (same detection
// through cpuinfo, same OVERRIDE_VECTOR_ISA / OVERRIDE_FMA / OVERRIDE_BMI2 / OVERRIDE_SLOW_GATHER variables), with two
// changes:
//   - a CPU with AVX-512 reports AVX2: PCSX2's MULTI_ISA_SELECT compares with ==, so AVX512F would select the SSE4
//     copies, and nothing kzgs compiles has AVX-512 code.
//   - the chosen ISA is printed once to stdout ("[kzgs] vector ISA: avx2").
// The GS/IPU/SPU2 per-ISA code (KZ_ISA=MULTI) and the software renderer's code generator follow g_cpu.vectorISA.
// SPDX-License-Identifier: GPL-3.0+

#include "GS/MultiISA.h"

#include "cpuinfo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool OverrideIs(const char* value, const char* name)
{
	return _stricmp(value, name) == 0;
}

static ProcessorFeatures::VectorISA GetCurrentISA()
{
	if (const char* over = std::getenv("OVERRIDE_VECTOR_ISA"))
	{
		// avx512f is accepted for compatibility with PCSX2 and treated as avx2 (see above).
		if (OverrideIs(over, "avx512f") || OverrideIs(over, "avx2"))
			return ProcessorFeatures::VectorISA::AVX2;
		if (OverrideIs(over, "avx"))
			return ProcessorFeatures::VectorISA::AVX;
		if (OverrideIs(over, "sse4"))
			return ProcessorFeatures::VectorISA::SSE4;
	}
	if (!cpuinfo_has_x86_avx())
		return ProcessorFeatures::VectorISA::SSE4;
	if (!cpuinfo_has_x86_avx2())
		return ProcessorFeatures::VectorISA::AVX;
	return ProcessorFeatures::VectorISA::AVX2;
}

static bool OverrideFlag(const char* name, bool value)
{
	if (const char* over = std::getenv(name))
		return over[0] == 'Y' || over[0] == 'y' || over[0] == '1';
	return value;
}

static ProcessorFeatures GetProcessorFeatures()
{
	cpuinfo_initialize();

	ProcessorFeatures features = {};
	features.vectorISA = GetCurrentISA();
	features.hasFMA = OverrideFlag("OVERRIDE_FMA", cpuinfo_has_x86_fma3());
	features.hasBMI2 = OverrideFlag("OVERRIDE_BMI2", cpuinfo_has_x86_bmi() && cpuinfo_has_x86_bmi2());
	features.hasSlowGather = false;
	if (std::getenv("OVERRIDE_SLOW_GATHER"))
	{
		features.hasSlowGather = OverrideFlag("OVERRIDE_SLOW_GATHER", false);
	}
	else if (features.vectorISA == ProcessorFeatures::VectorISA::AVX2)
	{
		// As PCSX2: VPGATHERDD is slow on Haswell and on every AMD CPU so far.
		if (cpuinfo_get_cores_count() > 0 && cpuinfo_get_core(0)->vendor == cpuinfo_vendor_intel)
			features.hasSlowGather = (cpuinfo_get_uarchs_count() == 0 || cpuinfo_get_uarch(0)->uarch == cpuinfo_uarch_haswell);
		else
			features.hasSlowGather = true;
	}

	static const char* const names[] = {"sse4", "avx", "avx2", "avx512f"};
	std::printf("[kzgs] vector ISA: %s%s\n", names[static_cast<int>(features.vectorISA)],
		std::getenv("OVERRIDE_VECTOR_ISA") ? " (OVERRIDE_VECTOR_ISA)" : "");
	return features;
}

const ProcessorFeatures g_cpu = GetProcessorFeatures();

// Defined after g_cpu in this file so they are initialised after it (as in PCSX2's MultiISA.cpp).

#include "GS/GSXXH.h"

u64 (&MultiISAFunctions::GSXXH3_64_Long)(const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Long);
u32 (&MultiISAFunctions::GSXXH3_64_Update)(void* state, const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Update);
u64 (&MultiISAFunctions::GSXXH3_64_Digest)(void* state) = MULTI_ISA_SELECT(GSXXH3_64_Digest);
