// kzvu0_test: VU0 micro mode (VCALLMS/VCALLMSR) on kzvu's microVU0, compared with PCSX2's VU0 interpreter and with
// PS2Recomp's VU1Interpreter in VU0 mode (the runtime's old path, PS2Runtime::executeVU0Microprogram).
//
//   kzvu0_test [--clamp N] [capture dir ...]   default dir: vu0_captures next to this source file; N = VU0 clamp mode
//
// Part 1, synthetic programs with known results: FMAC + load/store, the 4 KB data wrap, VU1 register access through
// VU0 addresses 0x4000+, an M-bit pause in the middle of a program, and a program longer than the old path's
// 4096-cycle budget.
// Part 2, captured game calls (Kzvu0Capture files written by the game with KZ_VU0_DUMP=<dir>): each call is replayed
// from its captured input state and the results are compared:
//   - kzvu JIT replay vs what the game got in-game from the same JIT (harness check: register sync, determinism);
//   - kzvu JIT vs PCSX2's VU0 interpreter (two PCSX2 models of the same hardware);
//   - kzvu JIT vs PS2Recomp's interpreter run exactly as the runtime ran it (reset, 4096-cycle budget);
//   - whether the program reads or writes VU1 registers (0x4000+), contains M bits, and how many cycles it takes.
// Compared state: VF1-31, VI1-15, ACC, Q, the 4 KB of data memory, VU1 VF/VI. Status/MAC/clip flags are listed
// separately (informational): microVU's flag hack only computes flags a later instruction reads.
// Exit code 0 = all synthetic checks pass and the JIT replay reproduces every in-game result bit for bit. Differences
// against the two interpreters are reported, not failed (the report explains them).

#include "kzvu.h"
#include "kzvu0_capture.h"

#ifdef KZVU_TEST_HAVE_PS2RECOMP
#include "runtime/ps2_vu1.h"
#include "ps2recomp_stubs.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <intrin.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;

// ---- minimal VU assembler (same encodings as kzvu_test.cpp) ------------------------------------------------------------
namespace vu
{
	enum : uint32_t { X = 8, Y = 4, Z = 2, W = 1, XYZ = 14, XYZW = 15 };
	enum : uint32_t { bcX = 0, bcY = 1, bcZ = 2, bcW = 3 };
	constexpr uint32_t Ebit = 1u << 30, Mbit = 1u << 29;
	constexpr uint32_t up(uint32_t op, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd)
	{
		return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
	}
	constexpr uint32_t upS(uint32_t code, uint32_t dest, uint32_t ft, uint32_t fs)
	{
		return (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | (0x3C | (code & 3));
	}
	constexpr uint32_t NOPu = upS(0x2F, 0, 0, 0);
	constexpr uint32_t ADD(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x28, d, ft, fs, fd); }
	constexpr uint32_t MUL(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x2A, d, ft, fs, fd); }
	constexpr uint32_t MULbc(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return up(0x18 | bc, d, ft, fs, fd); }
	constexpr uint32_t ADDbc(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return up(0x00 | bc, d, ft, fs, fd); }
	constexpr uint32_t lo(uint32_t op7) { return op7 << 25; }
	constexpr uint32_t loS(uint32_t code, uint32_t dest, uint32_t ft, uint32_t fs)
	{
		return lo(0x40) | (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | (0x3C | (code & 3));
	}
	constexpr uint32_t NOPl = loS(0x30, 0, 0, 0);
	constexpr uint32_t LQ(uint32_t d, uint32_t ft, uint32_t is, int32_t imm) { return lo(0x00) | (d << 21) | (ft << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t SQ(uint32_t d, uint32_t fs, uint32_t it, int32_t imm) { return lo(0x01) | (d << 21) | (it << 16) | (fs << 11) | (imm & 0x7FF); }
	constexpr uint32_t ILW(uint32_t d, uint32_t it, uint32_t is, int32_t imm) { return lo(0x04) | (d << 21) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t IADDIU(uint32_t it, uint32_t is, uint32_t imm15) { return lo(0x08) | (((imm15 >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm15 & 0x7FF); }
	constexpr uint32_t IBNE(uint32_t it, uint32_t is, int32_t imm) { return lo(0x29) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t IADDI(uint32_t it, uint32_t is, int32_t imm5) { return lo(0x40) | (it << 16) | (is << 11) | ((imm5 & 0x1F) << 6) | 0x32; }
} // namespace vu

struct Pair
{
	uint32_t lower, upper;
};

// ---- one VU0 run's observable result ------------------------------------------------------------------------------------
struct Result
{
	Kzvu0Regs regs{};
	uint8_t data[kKzvu0DataSize] = {};
	uint32_t vu1Vf[32][4] = {};
	uint32_t vu1Vi[32] = {};
	uint32_t cycles = 0;
	uint32_t vpuStat = 0;
	bool ended = true;      // reached its E bit (false: budget ran out first)
	bool hasVu1 = true;     // vu1Vf/vu1Vi are meaningful
};

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool ok, const std::string& what)
{
	g_checks++;
	if (!ok)
	{
		g_failures++;
		std::printf("    FAIL: %s\n", what.c_str());
	}
}

static float F(uint32_t u)
{
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}

static uint32_t U(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

static int64_t Ulps(uint32_t a, uint32_t b)
{
	auto key = [](uint32_t v) -> int64_t { return (v & 0x80000000u) ? -static_cast<int64_t>(v & 0x7FFFFFFFu) : static_cast<int64_t>(v); };
	return std::llabs(key(a) - key(b));
}

struct DiffSummary
{
	std::vector<std::string> lines; // one per differing register / memory qword
	int regDiffs = 0;
	int memDiffs = 0;
	int vu1Diffs = 0;
	int64_t maxUlps = 0;
	bool onlyFloatUlps = true;      // every difference is a float lane off by a few ulps
	std::vector<std::string> flagLines;
};

static const char* kLane = "xyzw";

static void DiffVec(DiffSummary& d, const char* name, int idx, const uint32_t a[4], const uint32_t b[4], int* counter)
{
	for (int l = 0; l < 4; l++)
	{
		if (a[l] == b[l])
			continue;
		const int64_t u = Ulps(a[l], b[l]);
		d.maxUlps = std::max(d.maxUlps, u);
		if (u > 4)
			d.onlyFloatUlps = false;
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%s%d.%c: %08x (%g) vs %08x (%g), %lld ulp", name, idx, kLane[l], a[l], F(a[l]), b[l],
			F(b[l]), static_cast<long long>(u));
		d.lines.push_back(buf);
		(*counter)++;
	}
}

static DiffSummary Compare(const Result& a, const Result& b)
{
	DiffSummary d;
	for (int i = 1; i < 32; i++)
		DiffVec(d, "vf", i, a.regs.vf[i], b.regs.vf[i], &d.regDiffs);
	for (int i = 1; i < 16; i++)
		if ((a.regs.vi[i] & 0xffff) != (b.regs.vi[i] & 0xffff))
		{
			char buf[96];
			std::snprintf(buf, sizeof(buf), "vi%d: %04x vs %04x", i, a.regs.vi[i] & 0xffff, b.regs.vi[i] & 0xffff);
			d.lines.push_back(buf);
			d.regDiffs++;
			d.onlyFloatUlps = false;
		}
	DiffVec(d, "acc", 0, a.regs.acc, b.regs.acc, &d.regDiffs);
	if (a.regs.q != b.regs.q)
	{
		const uint32_t qa[4] = {a.regs.q, 0, 0, 0}, qb[4] = {b.regs.q, 0, 0, 0};
		DiffVec(d, "q", 0, qa, qb, &d.regDiffs);
	}
	for (uint32_t qw = 0; qw < kKzvu0DataSize / 16; qw++)
	{
		const uint32_t* pa = reinterpret_cast<const uint32_t*>(a.data + qw * 16);
		const uint32_t* pb = reinterpret_cast<const uint32_t*>(b.data + qw * 16);
		if (std::memcmp(pa, pb, 16) != 0)
		{
			char name[16];
			std::snprintf(name, sizeof(name), "mem[%03x]", qw);
			DiffVec(d, name, 0, pa, pb, &d.memDiffs);
		}
	}
	if (a.hasVu1 && b.hasVu1)
	{
		for (int i = 1; i < 32; i++)
			DiffVec(d, "vu1.vf", i, a.vu1Vf[i], b.vu1Vf[i], &d.vu1Diffs);
		for (int i = 1; i < 16; i++)
			if ((a.vu1Vi[i] & 0xffff) != (b.vu1Vi[i] & 0xffff))
			{
				char buf[96];
				std::snprintf(buf, sizeof(buf), "vu1.vi%d: %04x vs %04x", i, a.vu1Vi[i] & 0xffff, b.vu1Vi[i] & 0xffff);
				d.lines.push_back(buf);
				d.vu1Diffs++;
				d.onlyFloatUlps = false;
			}
	}
	auto flag = [&](const char* n, uint32_t x, uint32_t y) {
		if (x != y)
		{
			char buf[80];
			std::snprintf(buf, sizeof(buf), "%s %x vs %x", n, x, y);
			d.flagLines.push_back(buf);
		}
	};
	flag("status", a.regs.status & 0xfff, b.regs.status & 0xfff);
	flag("mac", a.regs.mac & 0xffff, b.regs.mac & 0xffff);
	flag("clip", a.regs.clip & 0xffffff, b.regs.clip & 0xffffff);
	return d;
}

static int TotalDiffs(const DiffSummary& d)
{
	return d.regDiffs + d.memDiffs + d.vu1Diffs;
}

// ---- runners -------------------------------------------------------------------------------------------------------------
struct Input
{
	uint32_t startPc = 0;
	uint32_t fbrst = 0;
	uint32_t itop = 0;
	Kzvu0Regs regs{};
	uint32_t vu1Vf[32][4] = {};
	uint32_t vu1Vi[32] = {};
	alignas(16) uint8_t code[kKzvu0CodeSize] = {};
	alignas(16) uint8_t data[kKzvu0DataSize] = {};
};

static void DefaultRegs(Kzvu0Regs& r)
{
	std::memset(&r, 0, sizeof(r));
	r.vf[0][3] = U(1.0f);
	r.q = U(1.0f);
	r.r = 0x3F800000u;
}

static Result RunKzvu0(const Input& in, bool jit, uint32_t budget = 1u << 20)
{
	KzvuConfig cfg = kzvuGetConfig();
	cfg.vu0UseJit = jit;
	kzvuSetConfig(cfg);
	std::memcpy(kzvu0CodeMem(), in.code, kKzvu0CodeSize);
	kzvu0MicroWritten(0, kKzvu0CodeSize);
	std::memcpy(kzvu0DataMem(), in.data, kKzvu0DataSize);
	for (int i = 1; i < 32; i++)
	{
		kzvuSetVF(i, in.vu1Vf[i]);
		kzvuSetVI(i, in.vu1Vi[i]);
	}
	kzvu0SetRegs(in.regs);
	kzvuSetFBRST(in.fbrst);

	Result r;
	r.cycles = kzvu0Execute(in.startPc, budget);
	r.ended = !kzvu0Running();
	if (!r.ended)
		kzvu0Execute(0xFFFFFFFFu, 1u << 24); // finish it so the next run starts clean
	kzvu0GetRegs(r.regs);
	std::memcpy(r.data, kzvu0DataMem(), kKzvu0DataSize);
	for (int i = 0; i < 32; i++)
	{
		kzvuGetVF(i, r.vu1Vf[i]);
		r.vu1Vi[i] = kzvuGetVI(i);
	}
	r.vpuStat = kzvu0VpuStat();
	return r;
}

// ---- kzvu0Call (the game's direct path): a stand-in for the R5900Context COP2 members ---------------------------------------
struct HostRegs
{
	alignas(16) uint32_t vf[32][4];
	uint16_t vi[16];
	alignas(16) uint32_t acc[4];
	uint16_t status;
	uint32_t mac, clip, clip2;
	alignas(16) uint32_t r[4];
	uint32_t i, q;
};

static void ToHost(const Kzvu0Regs& in, HostRegs& h)
{
	std::memset(&h, 0xA5, sizeof(h)); // poison: whatever kzvu0Call does not define stays visible
	std::memcpy(h.vf, in.vf, sizeof(h.vf));
	for (int k = 0; k < 16; k++)
		h.vi[k] = static_cast<uint16_t>(in.vi[k]);
	std::memcpy(h.acc, in.acc, sizeof(h.acc));
	h.status = static_cast<uint16_t>(in.status);
	h.mac = in.mac;
	h.clip = in.clip;
	h.clip2 = in.clip;
	h.r[0] = in.r;
	h.r[1] = h.r[2] = h.r[3] = 0x11111111u;
	h.i = in.i;
	h.q = in.q;
}

static Kzvu0Host BindHost(HostRegs& h)
{
	Kzvu0Host b;
	b.vf = h.vf;
	b.vi = h.vi;
	b.acc = h.acc;
	b.status = &h.status;
	b.mac = &h.mac;
	b.clip = &h.clip;
	b.clip2 = &h.clip2;
	b.r = h.r;
	b.i = &h.i;
	b.q = &h.q;
	return b;
}

// Same setup as RunKzvu0, but the run goes through kzvu0Call (registers straight from/to `h`).
static Result RunKzvu0Direct(const Input& in, HostRegs& h, Kzvu0CallOut* callOut = nullptr)
{
	KzvuConfig cfg = kzvuGetConfig();
	cfg.vu0UseJit = true;
	kzvuSetConfig(cfg);
	std::memcpy(kzvu0CodeMem(), in.code, kKzvu0CodeSize);
	kzvu0MicroWritten(0, kKzvu0CodeSize);
	std::memcpy(kzvu0DataMem(), in.data, kKzvu0DataSize);
	for (int i = 1; i < 32; i++)
	{
		kzvuSetVF(i, in.vu1Vf[i]);
		kzvuSetVI(i, in.vu1Vi[i]);
	}
	ToHost(in.regs, h);
	const Kzvu0CallOut co = kzvu0Call(BindHost(h), in.startPc, 1u << 20, in.fbrst);
	if (callOut)
		*callOut = co;
	Result r;
	r.ended = !kzvu0Running();
	if (!r.ended)
		kzvu0Execute(0xFFFFFFFFu, 1u << 24);
	std::memcpy(r.data, kzvu0DataMem(), kKzvu0DataSize);
	for (int i = 0; i < 32; i++)
	{
		kzvuGetVF(i, r.vu1Vf[i]);
		r.vu1Vi[i] = kzvuGetVI(i);
	}
	r.vpuStat = co.vpuStat;
	return r;
}

// kzvu0Call must leave exactly what SetRegs + Execute + GetRegs leave. Returns the number of mismatches.
static int CompareDirect(const Result& old, const HostRegs& h, const Kzvu0CallOut& co, const Result& direct, std::string& why)
{
	int bad = 0;
	auto note = [&](const char* what) {
		if (bad++ < 4)
			why += std::string(" ") + what;
	};
	for (int k = 0; k < 32; k++)
		if (std::memcmp(old.regs.vf[k], h.vf[k], 16) != 0)
			note((std::string("vf") + std::to_string(k)).c_str());
	for (int k = 0; k < 16; k++)
		if ((old.regs.vi[k] & 0xFFFFu) != h.vi[k])
			note((std::string("vi") + std::to_string(k)).c_str());
	if (std::memcmp(old.regs.acc, h.acc, 16) != 0)
		note("acc");
	if (static_cast<uint16_t>(old.regs.status) != h.status)
		note("status");
	if (old.regs.mac != h.mac)
		note("mac");
	if (old.regs.clip != h.clip || old.regs.clip != h.clip2)
		note("clip");
	for (int k = 0; k < 4; k++)
		if (old.regs.r != h.r[k])
			note("r");
	if (old.regs.i != h.i)
		note("i");
	if (old.regs.q != h.q)
		note("q");
	if (std::memcmp(old.data, direct.data, kKzvu0DataSize) != 0)
		note("data-mem");
	if (std::memcmp(old.vu1Vf, direct.vu1Vf, sizeof(old.vu1Vf)) != 0 || std::memcmp(old.vu1Vi, direct.vu1Vi, sizeof(old.vu1Vi)) != 0)
		note("vu1-regs");
	if (co.vpuStat != old.vpuStat)
		note("vpu-stat");
	return bad;
}

#ifdef KZVU_TEST_HAVE_PS2RECOMP
alignas(16) static uint8_t g_fakeGs[64];

// Same register marshalling as PS2Runtime::executeVU0Microprogram (copyVu0ContextToState / copyVu0StateToContext in
// ps2_runtime.cpp), same reset, same budget (4096 there).
static Result RunRecomp0(const Input& in, uint32_t budget)
{
	static VU1Interpreter* vu = new VU1Interpreter(VU1Interpreter::Unit::VU0);
	alignas(16) static uint8_t code[kKzvu0CodeSize];
	alignas(16) static uint8_t data[kKzvu0DataSize];
	std::memcpy(code, in.code, sizeof(code));
	std::memcpy(data, in.data, sizeof(data));

	vu->reset();
	VU1State& st = vu->state();
	std::memset(&st, 0, sizeof(st));
	std::memcpy(st.vf, in.regs.vf, sizeof(st.vf));
	for (int i = 0; i < 16; i++)
		st.vi[i] = static_cast<int16_t>(in.regs.vi[i]);
	std::memcpy(st.acc, in.regs.acc, sizeof(st.acc));
	std::memcpy(&st.q, &in.regs.q, 4);
	std::memcpy(&st.i, &in.regs.i, 4);
	st.r = 0x3F800000u | (in.regs.r & 0x007FFFFFu);
	st.pc = in.startPc;
	st.mac = in.regs.mac;
	st.clip = in.regs.clip;
	st.status = in.regs.status;
	st.itop = in.itop;
	st.dBitEnabled = (in.fbrst & 4u) != 0;
	st.tBitEnabled = (in.fbrst & 8u) != 0;
	st.vf[0][0] = st.vf[0][1] = st.vf[0][2] = 0.0f;
	st.vf[0][3] = 1.0f;
	st.vi[0] = 0;

	vu->execute(code, sizeof(code), data, sizeof(data), *reinterpret_cast<GS*>(g_fakeGs), nullptr, in.startPc, 0u, in.itop,
		budget);

	Result r;
	std::memcpy(r.regs.vf, st.vf, sizeof(r.regs.vf));
	for (int i = 0; i < 16; i++)
		r.regs.vi[i] = static_cast<uint16_t>(st.vi[i]);
	std::memcpy(r.regs.acc, st.acc, sizeof(r.regs.acc));
	std::memcpy(&r.regs.q, &st.q, 4);
	std::memcpy(&r.regs.i, &st.i, 4);
	r.regs.r = st.r;
	r.regs.mac = st.mac;
	r.regs.clip = st.clip;
	r.regs.status = st.status & 0xffff;
	std::memcpy(r.data, data, sizeof(data));
	r.cycles = static_cast<uint32_t>(st.cycles);
	r.ended = r.cycles < budget;
	r.hasVu1 = false; // PS2Recomp's VU0 has no view of VU1 registers
	return r;
}
#endif

// ---- synthetic programs ----------------------------------------------------------------------------------------------
static void Assemble(Input& in, const std::vector<Pair>& prog)
{
	std::memset(in.code, 0, sizeof(in.code));
	for (size_t i = 0; i < prog.size(); i++)
	{
		std::memcpy(in.code + i * 8, &prog[i].lower, 4);
		std::memcpy(in.code + i * 8 + 4, &prog[i].upper, 4);
	}
}

static void SetQwF(uint8_t* mem, uint32_t qw, float a, float b, float c, float d)
{
	const float v[4] = {a, b, c, d};
	std::memcpy(mem + qw * 16, v, 16);
}

static void ExpectVf(const char* impl, const Result& r, int reg, float x, float y, float z, float w)
{
	const float e[4] = {x, y, z, w};
	for (int l = 0; l < 4; l++)
	{
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s vf%d.%c = %g, expected %g", impl, reg, kLane[l], F(r.regs.vf[reg][l]), e[l]);
		Check(r.regs.vf[reg][l] == U(e[l]), buf);
	}
}

static void ExpectMem(const char* impl, const Result& r, uint32_t qw, float x, float y, float z, float w)
{
	const float e[4] = {x, y, z, w};
	for (int l = 0; l < 4; l++)
	{
		uint32_t v;
		std::memcpy(&v, r.data + qw * 16 + l * 4, 4);
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s mem[%03x].%c = %g, expected %g", impl, qw, kLane[l], F(v), e[l]);
		Check(v == U(e[l]), buf);
	}
}

static void Synthetic()
{
	using namespace vu;

	// 1. FMAC + load/store: vf1 = mem[0]; vf2 = vf1 * vf1.x + (1,1,1,1)... stored to mem[1]; VF regs passed in.
	{
		std::printf("[synthetic: FMAC + LQ/SQ, registers passed in from COP2]\n");
		Input in;
		DefaultRegs(in.regs);
		SetQwF(in.data, 0, 1.0f, 2.0f, 3.0f, 4.0f);
		const float v5[4] = {10.0f, 20.0f, 30.0f, 40.0f};
		std::memcpy(in.regs.vf[5], v5, 16);
		Assemble(in, {
			{LQ(XYZW, 1, 0, 0), NOPu},
			{NOPl, MULbc(XYZW, 2, 1, 1, bcY)},    // vf2 = vf1 * vf1.y = (2,4,6,8)
			{NOPl, ADD(XYZW, 3, 2, 5)},           // vf3 = vf2 + vf5 = (12,24,36,48)
			{SQ(XYZW, 3, 0, 1), NOPu},
			{NOPl, NOPu | Ebit},
			{NOPl, NOPu},
		});
		for (int jit = 1; jit >= 0; jit--)
		{
			const Result r = RunKzvu0(in, jit != 0);
			const char* n = jit ? "jit" : "pcsx2-interp";
			ExpectVf(n, r, 2, 2, 4, 6, 8);
			ExpectVf(n, r, 3, 12, 24, 36, 48);
			ExpectMem(n, r, 1, 12, 24, 36, 48);
			Check(r.ended, std::string(n) + " reached the E bit");
		}
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		const Result rr = RunRecomp0(in, 4096);
		ExpectVf("ps2recomp", rr, 3, 12, 24, 36, 48);
		ExpectMem("ps2recomp", rr, 1, 12, 24, 36, 48);
#endif
	}

	// 2. VU0 data memory wraps at 4 KB: qword 0x100 is qword 0.
	{
		std::printf("[synthetic: 4 KB data wrap]\n");
		Input in;
		DefaultRegs(in.regs);
		SetQwF(in.data, 0, 5.0f, 6.0f, 7.0f, 8.0f);
		Assemble(in, {
			{LQ(XYZW, 1, 0, 0x100), NOPu},
			{SQ(XYZW, 1, 0, 0x1FF), NOPu},        // 0x1FF & 0xFF = 0xFF
			{NOPl, NOPu | Ebit},
			{NOPl, NOPu},
		});
		for (int jit = 1; jit >= 0; jit--)
		{
			const Result r = RunKzvu0(in, jit != 0);
			const char* n = jit ? "jit" : "pcsx2-interp";
			ExpectVf(n, r, 1, 5, 6, 7, 8);
			ExpectMem(n, r, 0xFF, 5, 6, 7, 8);
		}
	}

	// 3. VU1 registers at VU0 addresses 0x4000+: qword 0x400+n = VU1 VFn, 0x420+n = VU1 VIn (lower 16 bits).
	{
		std::printf("[synthetic: VU1 VF/VI read and written through VU0 memory 0x4000+]\n");
		Input in;
		DefaultRegs(in.regs);
		const float v[4] = {1.5f, 2.5f, 3.5f, 4.5f};
		std::memcpy(in.vu1Vf[7], v, 16);
		in.vu1Vi[3] = 0x1234;
		const float v9[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
		std::memcpy(in.regs.vf[9], v9, 16);
		Assemble(in, {
			{LQ(XYZW, 1, 0, 0x407), NOPu},        // vf1 = VU1 vf7
			{ILW(X, 2, 0, 0x423), NOPu},          // vi2 = VU1 vi3
			{SQ(XYZW, 9, 0, 0x40C), NOPu},        // VU1 vf12 = vf9
			{NOPl, NOPu | Ebit},
			{NOPl, NOPu},
		});
		for (int jit = 1; jit >= 0; jit--)
		{
			const Result r = RunKzvu0(in, jit != 0);
			const char* n = jit ? "jit" : "pcsx2-interp";
			ExpectVf(n, r, 1, 1.5f, 2.5f, 3.5f, 4.5f);
			Check((r.regs.vi[2] & 0xffff) == 0x1234, std::string(n) + " vi2 = VU1 vi3");
			for (int l = 0; l < 4; l++)
				Check(r.vu1Vf[12][l] == U(v9[l]), std::string(n) + " VU1 vf12 written");
		}
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		const Result rr = RunRecomp0(in, 4096);
		const bool sees = rr.regs.vf[1][0] == U(1.5f);
		std::printf("    info: PS2Recomp's interpreter %s VU1 registers at 0x4000+ (it wraps the address into VU0 memory)\n",
			sees ? "reaches" : "does not reach");
#endif
	}

	// 4. M bit in the middle: the program must still run to its end in one kzvu0Execute.
	{
		std::printf("[synthetic: M-bit pause inside a program]\n");
		Input in;
		DefaultRegs(in.regs);
		SetQwF(in.data, 0, 1.0f, 1.0f, 1.0f, 1.0f);
		Assemble(in, {
			{LQ(XYZW, 1, 0, 0), NOPu},
			{NOPl, ADD(XYZW, 2, 1, 1) | Mbit},    // vf2 = 2
			{NOPl, ADD(XYZW, 3, 2, 2)},           // vf3 = 4
			{NOPl, NOPu | Mbit},
			{NOPl, ADD(XYZW, 4, 3, 3)},           // vf4 = 8
			{SQ(XYZW, 4, 0, 2), NOPu | Ebit},
			{NOPl, NOPu},
		});
		for (int jit = 1; jit >= 0; jit--)
		{
			const Result r = RunKzvu0(in, jit != 0);
			const char* n = jit ? "jit" : "pcsx2-interp";
			Check(r.ended, std::string(n) + " ran past the M bits to the E bit");
			ExpectVf(n, r, 4, 8, 8, 8, 8);
			ExpectMem(n, r, 2, 8, 8, 8, 8);
		}
	}

	// 6. The first two instructions of the game's program 0xd18: ITOF0 of integer bit patterns (they look like
	//    denormals as floats), then SUBAw. The integers must survive as integers.
	{
		std::printf("[synthetic: ITOF0 of small integers held in a VF register (game program 0xd18)]\n");
		Input in;
		DefaultRegs(in.regs);
		in.regs.vf[1][0] = 0x000000ff;
		in.regs.vf[1][1] = 0x000002fe;
		in.regs.vf[1][2] = 0x00000001;
		Assemble(in, {
			{NOPl, 0x01c2093c},                   // ITOF0.xyz vf2, vf1   (word taken from the game's micro memory)
			{NOPl, 0x01c0007f},                   // SUBAw.xyz ACC, vf0, vf0w
			{NOPl, NOPu | Ebit},
			{NOPl, NOPu},
		});
		for (int jit = 1; jit >= 0; jit--)
		{
			const Result r = RunKzvu0(in, jit != 0);
			const char* n = jit ? "jit" : "pcsx2-interp";
			ExpectVf(n, r, 2, 255.0f, 766.0f, 1.0f, 0.0f);
			Check(r.regs.vf[1][0] == 0xffu && r.regs.vf[1][1] == 0x2feu, std::string(n) + " vf1 unchanged");
		}
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		const Result rr = RunRecomp0(in, 4096);
		std::printf("    info: ps2recomp gives vf2 = (%g, %g, %g), vf1 = (%08x, %08x, %08x)\n", F(rr.regs.vf[2][0]),
			F(rr.regs.vf[2][1]), F(rr.regs.vf[2][2]), rr.regs.vf[1][0], rr.regs.vf[1][1], rr.regs.vf[1][2]);
#endif
	}

	// 5. A loop longer than the old path's 4096-cycle budget: 2000 iterations of an add.
	{
		std::printf("[synthetic: program longer than 4096 cycles]\n");
		Input in;
		DefaultRegs(in.regs);
		const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		std::memcpy(in.regs.vf[1], one, 16);
		in.regs.vi[1] = 2000;
		Assemble(in, {
			{IADDI(1, 1, -1), ADD(XYZW, 2, 2, 1)},  // loop: vi1--, vf2 += 1
			{IBNE(1, 0, -2), NOPu},
			{NOPl, NOPu},
			{NOPl, NOPu | Ebit},
			{NOPl, NOPu},
		});
		const Result r = RunKzvu0(in, true);
		Check(r.ended, "jit long loop reached the E bit");
		std::printf("    jit: %u cycles, vf2.x = %g, vi1 = %d\n", r.cycles, F(r.regs.vf[2][0]),
			static_cast<int16_t>(r.regs.vi[1]));
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		const Result rr = RunRecomp0(in, 4096);
		std::printf("    ps2recomp (4096-cycle budget, as the runtime ran it): ended=%d, %u cycles, vf2.x = %g\n", rr.ended,
			rr.cycles, F(rr.regs.vf[2][0]));
#endif
	}
}

// ---- captured game calls ------------------------------------------------------------------------------------------------
static int CountMbits(const uint8_t* code)
{
	int n = 0;
	for (uint32_t pc = 0; pc < kKzvu0CodeSize; pc += 8)
	{
		uint32_t upper;
		std::memcpy(&upper, code + pc + 4, 4);
		n += (upper >> 29) & 1;
	}
	return n;
}

static void PrintDiff(const char* label, const DiffSummary& d, int maxLines = 6)
{
	std::printf("      %s: %d reg, %d mem, %d vu1 differences", label, d.regDiffs, d.memDiffs, d.vu1Diffs);
	if (TotalDiffs(d))
		std::printf(" (max %lld ulp)", static_cast<long long>(d.maxUlps));
	if (!d.flagLines.empty())
	{
		std::printf("; flags:");
		for (const auto& f : d.flagLines)
			std::printf(" %s;", f.c_str());
	}
	std::printf("\n");
	for (int i = 0; i < static_cast<int>(d.lines.size()) && i < maxLines; i++)
		std::printf("        %s\n", d.lines[i].c_str());
	if (static_cast<int>(d.lines.size()) > maxLines)
		std::printf("        ... %zu more\n", d.lines.size() - maxLines);
}

struct CaptureStats
{
	int files = 0;
	int replayMismatch = 0;
	int interpDiffer = 0;
	int recompDiffer = 0;
	int recompDifferUlpOnly = 0;
	int recompTruncated = 0;
	int readsVu1 = 0;
	int writesVu1 = 0;
	int withMbits = 0;
	int nonFiniteIn = 0;
	std::vector<std::string> rows; // summary table
};

// The PS2 has no NaN/Inf; one in a captured input means the EE side (host float math) produced it.
static bool HasNonFinite(const Input& in)
{
	auto bad = [](uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u; };
	for (int i = 1; i < 32; i++)
		for (int l = 0; l < 4; l++)
			if (bad(in.regs.vf[i][l]))
				return true;
	for (int l = 0; l < 4; l++)
		if (bad(in.regs.acc[l]))
			return true;
	for (uint32_t off = 0; off < kKzvu0DataSize; off += 4)
	{
		uint32_t v;
		std::memcpy(&v, in.data + off, 4);
		if (bad(v))
			return true;
	}
	return false;
}

static void ReplayCaptures(const std::vector<fs::path>& dirs, CaptureStats& cs)
{
	std::vector<fs::path> files;
	for (const auto& dir : dirs)
	{
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(dir, ec))
			if (e.is_regular_file() && e.path().extension() == ".bin")
				files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	std::printf("\n[captured game calls: %zu files]\n", files.size());

	auto cap = std::make_unique<Kzvu0Capture>();
	for (const auto& path : files)
	{
		FILE* f = std::fopen(path.string().c_str(), "rb");
		if (!f)
			continue;
		const size_t n = std::fread(cap.get(), sizeof(Kzvu0Capture), 1, f);
		std::fclose(f);
		if (n != 1 || cap->magic != Kzvu0Capture::kMagic || cap->version != Kzvu0Capture::kVersion)
		{
			std::printf("  %s: not a v%u capture, skipped\n", path.filename().string().c_str(), Kzvu0Capture::kVersion);
			continue;
		}
		cs.files++;

		auto in = std::make_unique<Input>();
		in->startPc = cap->startPc;
		in->fbrst = cap->fbrst;
		in->itop = cap->itop;
		in->regs = cap->in;
		std::memcpy(in->vu1Vf, cap->vu1VfIn, sizeof(in->vu1Vf));
		std::memcpy(in->vu1Vi, cap->vu1ViIn, sizeof(in->vu1Vi));
		std::memcpy(in->code, cap->code, sizeof(in->code));
		std::memcpy(in->data, cap->dataIn, sizeof(in->data));

		auto game = std::make_unique<Result>();
		game->regs = cap->out;
		std::memcpy(game->data, cap->dataOut, sizeof(game->data));
		std::memcpy(game->vu1Vf, cap->vu1VfOut, sizeof(game->vu1Vf));
		std::memcpy(game->vu1Vi, cap->vu1ViOut, sizeof(game->vu1Vi));
		game->cycles = cap->cycles;

		auto jit = std::make_unique<Result>(RunKzvu0(*in, true));
		auto interp = std::make_unique<Result>(RunKzvu0(*in, false));

		// VU1 register use: rerun with different VU1 registers; any change in the VU0 results means the program read them.
		auto probe = std::make_unique<Input>(*in);
		for (int i = 1; i < 32; i++)
		{
			for (int l = 0; l < 4; l++)
				probe->vu1Vf[i][l] = U(1000.0f + i * 4 + l);
			probe->vu1Vi[i] = 0x5A00 + i;
		}
		auto jitProbe = std::make_unique<Result>(RunKzvu0(*probe, true));
		jitProbe->hasVu1 = false;
		auto jitNoVu1 = std::make_unique<Result>(*jit);
		jitNoVu1->hasVu1 = false;
		const bool readsVu1 = TotalDiffs(Compare(*jitNoVu1, *jitProbe)) != 0;
		bool writesVu1 = std::memcmp(jit->vu1Vf, in->vu1Vf, sizeof(jit->vu1Vf)) != 0;
		for (int i = 1; i < 16; i++)
			writesVu1 |= (jit->vu1Vi[i] & 0xffff) != (in->vu1Vi[i] & 0xffff);
		const int mbits = CountMbits(in->code);
		const bool nonFinite = HasNonFinite(*in);
		cs.nonFiniteIn += nonFinite;

		std::printf("  %s  start 0x%03x  caller 0x%06x  call #%u  jit %u cycles  (in-game %u)%s%s%s\n",
			path.filename().string().c_str(), cap->startPc, cap->callerPc, cap->callIndex, jit->cycles, cap->cycles,
			readsVu1 ? "  READS-VU1" : "", writesVu1 ? "  WRITES-VU1" : "", mbits ? "  M-bits-in-code" : "");
		cs.readsVu1 += readsVu1;
		cs.writesVu1 += writesVu1;
		cs.withMbits += mbits != 0;

		const DiffSummary dGame = Compare(*jit, *game);
		Check(TotalDiffs(dGame) == 0, path.filename().string() + ": JIT replay reproduces the in-game result");
		if (TotalDiffs(dGame))
		{
			cs.replayMismatch++;
			PrintDiff("jit replay vs in-game", dGame);
		}
		Check(jit->ended, path.filename().string() + ": JIT run reached the E bit");

		// kzvu0Call (the game's direct path) vs SetRegs/Execute/GetRegs: the captured input, then randomized variants.
		{
			auto host = std::make_unique<HostRegs>();
			Kzvu0CallOut co{};
			auto direct = std::make_unique<Result>(RunKzvu0Direct(*in, *host, &co));
			std::string why;
			const int bad = CompareDirect(*jit, *host, co, *direct, why);
			Check(bad == 0 && co.tpc == kzvu0TPC(), path.filename().string() + ": kzvu0Call == SetRegs/Execute/GetRegs" + why);
			uint64_t rng = 0x9E3779B97F4A7C15ull ^ cap->callIndex;
			auto next = [&rng]() {
				rng ^= rng << 13;
				rng ^= rng >> 7;
				rng ^= rng << 17;
				return static_cast<uint32_t>(rng >> 16);
			};
			int fuzzBad = 0;
			for (int v = 0; v < 16 && fuzzBad == 0; v++)
			{
				auto variant = std::make_unique<Input>(*in);
				for (int k = 1; k < 32; k++)
					for (int l = 0; l < 4; l++)
						if (next() & 3u) // mostly random bits (NaN/Inf/denormals included), some lanes stay as captured
							variant->regs.vf[k][l] = next();
				for (int k = 1; k < 16; k++)
					variant->regs.vi[k] = next() & 0xFFFFu;
				for (int l = 0; l < 4; l++)
					variant->regs.acc[l] = next();
				variant->regs.status = next() & 0xFFFFu;
				variant->regs.mac = next() & 0xFFFFu;
				variant->regs.clip = next() & 0xFFFFFFu;
				variant->regs.r = 0x3F800000u | (next() & 0x7FFFFFu);
				variant->regs.i = next();
				variant->regs.q = next();
				variant->fbrst = next() & 0x0C0Cu;
				auto a = std::make_unique<Result>(RunKzvu0(*variant, true));
				Kzvu0CallOut co2{};
				auto b = std::make_unique<Result>(RunKzvu0Direct(*variant, *host, &co2));
				std::string why2;
				fuzzBad = CompareDirect(*a, *host, co2, *b, why2);
				if (fuzzBad)
					Check(false, path.filename().string() + ": kzvu0Call fuzz variant " + std::to_string(v) + " differs:" + why2);
			}
			Check(fuzzBad == 0, path.filename().string() + ": kzvu0Call == old path on 16 randomized inputs");
		}

		const DiffSummary dInterp = Compare(*jit, *interp);
		if (TotalDiffs(dInterp))
			cs.interpDiffer++;
		PrintDiff("jit vs pcsx2-interp", dInterp);

#ifdef KZVU_TEST_HAVE_PS2RECOMP
		auto rec = std::make_unique<Result>(RunRecomp0(*in, 4096));
		auto jitCmp = std::make_unique<Result>(*jit);
		jitCmp->hasVu1 = false;
		const DiffSummary dRec = Compare(*jitCmp, *rec);
		if (TotalDiffs(dRec))
		{
			cs.recompDiffer++;
			if (dRec.onlyFloatUlps)
				cs.recompDifferUlpOnly++;
		}
		if (!rec->ended)
			cs.recompTruncated++;
		std::printf("      ps2recomp: %u cycles%s\n", rec->cycles, rec->ended ? "" : " - HIT THE 4096-CYCLE BUDGET, program cut short");
		PrintDiff("jit vs ps2recomp (runtime's old path)", dRec, 12);
		auto interpCmp = std::make_unique<Result>(*interp);
		interpCmp->hasVu1 = false;
		const int dIR = TotalDiffs(Compare(*interpCmp, *rec));
#else
		const int dIR = -1;
		const int dRecN = -1;
#endif
		char row[200];
		std::snprintf(row, sizeof(row), "  %-28s 0x%03x  %-3s  %6d %8d %8d %8d  %s",
			path.filename().string().c_str(), cap->startPc, nonFinite ? "yes" : "no", TotalDiffs(dGame),
			TotalDiffs(dInterp),
#ifdef KZVU_TEST_HAVE_PS2RECOMP
			TotalDiffs(dRec),
#else
			dRecN,
#endif
			dIR, readsVu1 ? "reads VU1" : "");
		cs.rows.push_back(row);
	}
}


// ---- --bench: host-side cost of one VU0 call (what src/kz_vu.cpp onVu0Call pays) --------------------------------------------
// Replays captured calls in a loop and times the register copy-in, the run and the copy-out with rdtsc (old path:
// kzvu0SetRegs / kzvu0Execute / kzvu0GetRegs through a Kzvu0Regs staging copy).
static double TscPerNs()
{
	LARGE_INTEGER f, a, b;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	const uint64_t t0 = __rdtsc();
	do
		QueryPerformanceCounter(&b);
	while ((b.QuadPart - a.QuadPart) * 1000 < f.QuadPart * 100); // 100 ms
	const uint64_t t1 = __rdtsc();
	return double(t1 - t0) / (double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart));
}

// contextToVu0 / vu0ToContext of src/kz_vu.cpp, on the HostRegs stand-in.
static void StageIn(const HostRegs& h, Kzvu0Regs& r)
{
	std::memcpy(r.vf, h.vf, sizeof(r.vf));
	for (int i = 0; i < 16; ++i)
		r.vi[i] = h.vi[i];
	std::memcpy(r.acc, h.acc, sizeof(r.acc));
	r.status = h.status;
	r.mac = h.mac;
	r.clip = h.clip;
	r.r = h.r[0];
	r.i = h.i;
	r.q = h.q;
}

static void StageOut(const Kzvu0Regs& r, HostRegs& h)
{
	std::memcpy(&h.vf[1], r.vf[1], 31 * 16);
	h.vf[0][0] = h.vf[0][1] = h.vf[0][2] = 0;
	h.vf[0][3] = 0x3F800000u;
	h.vi[0] = 0;
	for (int i = 1; i < 16; ++i)
		h.vi[i] = static_cast<uint16_t>(r.vi[i]);
	std::memcpy(h.acc, r.acc, sizeof(h.acc));
	h.status = static_cast<uint16_t>(r.status);
	h.mac = r.mac;
	h.clip = h.clip2 = r.clip;
	h.r[0] = h.r[1] = h.r[2] = h.r[3] = r.r;
	h.i = r.i;
	h.q = r.q;
}

static void Bench(const std::vector<fs::path>& dirs, int iters)
{
	std::vector<fs::path> files;
	for (const auto& dir : dirs)
	{
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(dir, ec))
			if (e.is_regular_file() && e.path().extension() == ".bin")
				files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	const double tsc = TscPerNs();
	std::printf("\n[bench: %d iterations per capture, TSC %.3f GHz]\n", iters, tsc);
	std::printf("  %-34s %-6s %8s %8s %8s %8s   %s\n", "file", "start", "in ns", "run ns", "out ns", "old total", "kzvu0Call ns");
	std::map<uint32_t, int> seen;
	auto cap = std::make_unique<Kzvu0Capture>();
	for (const auto& path : files)
	{
		FILE* f = std::fopen(path.string().c_str(), "rb");
		if (!f)
			continue;
		const size_t n = std::fread(cap.get(), sizeof(Kzvu0Capture), 1, f);
		std::fclose(f);
		if (n != 1 || cap->magic != Kzvu0Capture::kMagic || cap->version != Kzvu0Capture::kVersion)
			continue;
		if (seen[cap->startPc]++ >= 1)
			continue; // one capture per start PC
		std::memcpy(kzvu0CodeMem(), cap->code, kKzvu0CodeSize);
		kzvu0MicroWritten(0, kKzvu0CodeSize);
		std::memcpy(kzvu0DataMem(), cap->dataIn, kKzvu0DataSize);
		for (int i = 1; i < 32; i++)
		{
			kzvuSetVF(i, cap->vu1VfIn[i]);
			kzvuSetVI(i, cap->vu1ViIn[i]);
		}
		Kzvu0Regs regs;
		HostRegs host;
		ToHost(cap->in, host);
		uint64_t tIn = 0, tRun = 0, tOut = 0, tNew = 0;
		for (int it = 0; it < iters + 100; it++)
		{
			const bool timed = it >= 100;
			// Old path as src/kz_vu.cpp ran it: context -> Kzvu0Regs, SetRegs, Execute, GetRegs, Kzvu0Regs -> context.
			const uint64_t a0 = __rdtsc();
			StageIn(host, regs);
			const uint64_t a = __rdtsc();
			kzvu0SetRegs(regs);
			kzvuSetFBRST(cap->fbrst);
			const uint64_t b = __rdtsc();
			kzvu0Execute(cap->startPc, 1u << 20);
			const uint64_t c = __rdtsc();
			kzvu0GetRegs(regs);
			const uint64_t d = __rdtsc();
			StageOut(regs, host);
			const uint64_t d1 = __rdtsc();
			ToHost(cap->in, host);
			const uint64_t e = __rdtsc();
			kzvu0Call(BindHost(host), cap->startPc, 1u << 20, cap->fbrst);
			const uint64_t g = __rdtsc();
			if (timed)
			{
				tIn += b - a0;
				tRun += c - b;
				tOut += d1 - c;
				tNew += g - e;
			}
		}
		const double k = 1.0 / (tsc * iters);
		const double oldTotal = (tIn + tRun + tOut) * k;
		std::printf("  %-34s 0x%03x  %8.1f %8.1f %8.1f %8.1f   %8.1f  (%.0f%%)\n", path.filename().string().c_str(), cap->startPc,
			tIn * k, tRun * k, tOut * k, oldTotal, tNew * k, 100.0 * tNew * k / oldTotal);
	}
}

int main(int argc, char** argv)
{
	std::vector<fs::path> dirs;
	int vu0Clamp = 3; // what src/kz_vu.cpp uses in the game
	int benchIters = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--clamp") && i + 1 < argc)
			vu0Clamp = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--bench"))
			benchIters = 200000;
		else
			dirs.emplace_back(argv[i]);
	}
	if (dirs.empty())
		dirs.push_back(fs::path(__FILE__).parent_path() / "vu0_captures");

	KzvuConfig cfg; // Killzone GameIndex: clamp mode 0, IbitHack on
	cfg.vu0ClampMode = vu0Clamp;
	std::printf("VU0 clamp mode %d\n", vu0Clamp < 0 ? cfg.clampMode : vu0Clamp);
	std::string err;
	if (!kzvuInit(cfg, &err))
	{
		std::printf("kzvuInit failed: %s\n", err.c_str());
		return 1;
	}

	if (benchIters)
	{
		Bench(dirs, benchIters);
		kzvuShutdown();
		return 0;
	}
	Synthetic();
	CaptureStats cs;
	ReplayCaptures(dirs, cs);
	if (!cs.rows.empty())
	{
		std::printf("\n[summary: number of differing lanes/registers; a = jit, g = in-game result, i = PCSX2 interpreter, r = PS2Recomp]\n");
		std::printf("  %-28s %-6s %-4s %6s %8s %8s %8s\n", "file", "start", "NaN", "a-g", "a-i", "a-r", "i-r");
		for (const auto& r : cs.rows)
			std::printf("%s\n", r.c_str());
	}

	std::printf("\ncaptures: %d, JIT replay != in-game: %d, JIT != PCSX2 interpreter: %d, JIT != PS2Recomp: %d "
				"(%d of them only float lanes within 4 ulp), PS2Recomp hit its 4096-cycle budget: %d, reads VU1 regs: %d, "
				"writes VU1 regs: %d, code with M bits: %d, inputs holding NaN/Inf: %d\n",
		cs.files, cs.replayMismatch, cs.interpDiffer, cs.recompDiffer, cs.recompDifferUlpOnly, cs.recompTruncated,
		cs.readsVu1, cs.writesVu1, cs.withMbits, cs.nonFiniteIn);
	std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");
	kzvuShutdown();
	return g_failures ? 1 : 0;
}
