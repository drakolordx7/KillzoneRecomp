// kzvu_test: runs hand-assembled VU1 microprograms on kzvu's microVU (JIT) and interpreter, checks the results against
// expected values, and (when built with KZVU_TEST_HAVE_PS2RECOMP) also runs them on PS2Recomp's VU1Interpreter and
// compares the final state of all implementations. Ends with a microbenchmark. Exit code 0 = pass.
//
//   kzvu_test [--bench-runs N] [--no-bench]

#include "kzvu.h"

#ifdef KZVU_TEST_HAVE_PS2RECOMP
#include "runtime/ps2_vu1.h"
#include "ps2recomp_stubs.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// =====================================================================================================================
// VU assembler (hand encoding; field layouts cross-checked against PCSX2's VUops.cpp opcode tables)
// =====================================================================================================================
namespace vu
{
	enum : uint32_t { X = 8, Y = 4, Z = 2, W = 1, XYZ = 14, XYZW = 15 }; // dest field (bits 24..21: x y z w)
	enum : uint32_t { bcX = 0, bcY = 1, bcZ = 2, bcW = 3 };
	constexpr uint32_t Ibit = 1u << 31, Ebit = 1u << 30, Mbit = 1u << 29, Dbit = 1u << 28, Tbit = 1u << 27;

	// ---- upper -----------------------------------------------------------------------------------------------------
	constexpr uint32_t up(uint32_t op, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd)
	{
		return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
	}
	// 11-bit "special" upper ops (opcode 0x3C-0x3F): bits 10..6 = code >> 2, bits 1..0 = code & 3
	constexpr uint32_t upS(uint32_t code, uint32_t dest, uint32_t ft, uint32_t fs)
	{
		return (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | (0x3C | (code & 3));
	}
	constexpr uint32_t NOPu = upS(0x2F, 0, 0, 0); // 0x000002FF
	constexpr uint32_t ADD(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x28, d, ft, fs, fd); }
	constexpr uint32_t SUB(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x2C, d, ft, fs, fd); }
	constexpr uint32_t MUL(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x2A, d, ft, fs, fd); }
	constexpr uint32_t MADD(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft) { return up(0x29, d, ft, fs, fd); }
	constexpr uint32_t ADDbc(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return up(0x00 | bc, d, ft, fs, fd); }
	constexpr uint32_t MULbc(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return up(0x18 | bc, d, ft, fs, fd); }
	constexpr uint32_t MADDbc(uint32_t d, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return up(0x08 | bc, d, ft, fs, fd); }
	constexpr uint32_t MULq(uint32_t d, uint32_t fd, uint32_t fs) { return up(0x1C, d, 0, fs, fd); }
	constexpr uint32_t ADDq(uint32_t d, uint32_t fd, uint32_t fs) { return up(0x20, d, 0, fs, fd); }
	constexpr uint32_t MULi(uint32_t d, uint32_t fd, uint32_t fs) { return up(0x1E, d, 0, fs, fd); }
	constexpr uint32_t MULA(uint32_t d, uint32_t fs, uint32_t ft) { return upS(0x2A, d, ft, fs); }
	constexpr uint32_t MULAbc(uint32_t d, uint32_t fs, uint32_t ft, uint32_t bc) { return upS(0x18 | bc, d, ft, fs); }
	constexpr uint32_t MADDAbc(uint32_t d, uint32_t fs, uint32_t ft, uint32_t bc) { return upS(0x08 | bc, d, ft, fs); }
	// ITOF/FTOI: ft = destination, fs = source
	constexpr uint32_t ITOF0(uint32_t d, uint32_t ft, uint32_t fs) { return upS(0x10, d, ft, fs); }
	constexpr uint32_t ITOF4(uint32_t d, uint32_t ft, uint32_t fs) { return upS(0x11, d, ft, fs); }
	constexpr uint32_t ITOF12(uint32_t d, uint32_t ft, uint32_t fs) { return upS(0x12, d, ft, fs); }
	constexpr uint32_t FTOI0(uint32_t d, uint32_t ft, uint32_t fs) { return upS(0x14, d, ft, fs); }
	constexpr uint32_t FTOI4(uint32_t d, uint32_t ft, uint32_t fs) { return upS(0x15, d, ft, fs); }

	// ---- lower -----------------------------------------------------------------------------------------------------
	constexpr uint32_t lo(uint32_t op7) { return op7 << 25; }
	constexpr uint32_t loS(uint32_t code, uint32_t dest, uint32_t ft, uint32_t fs)
	{
		return lo(0x40) | (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | (0x3C | (code & 3));
	}
	constexpr uint32_t NOPl = loS(0x30, 0, 0, 0); // MOVE with empty dest = 0x8000033C
	constexpr uint32_t LQ(uint32_t d, uint32_t ft, uint32_t is, int32_t imm) { return lo(0x00) | (d << 21) | (ft << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t SQ(uint32_t d, uint32_t fs, uint32_t it, int32_t imm) { return lo(0x01) | (d << 21) | (it << 16) | (fs << 11) | (imm & 0x7FF); }
	constexpr uint32_t ILW(uint32_t d, uint32_t it, uint32_t is, int32_t imm) { return lo(0x04) | (d << 21) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t ISW(uint32_t d, uint32_t it, uint32_t is, int32_t imm) { return lo(0x05) | (d << 21) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t IADDIU(uint32_t it, uint32_t is, uint32_t imm15) { return lo(0x08) | (((imm15 >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm15 & 0x7FF); }
	constexpr uint32_t ISUBIU(uint32_t it, uint32_t is, uint32_t imm15) { return lo(0x09) | (((imm15 >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm15 & 0x7FF); }
	constexpr uint32_t B(int32_t imm) { return lo(0x20) | (imm & 0x7FF); }
	constexpr uint32_t IBEQ(uint32_t it, uint32_t is, int32_t imm) { return lo(0x28) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t IBNE(uint32_t it, uint32_t is, int32_t imm) { return lo(0x29) | (it << 16) | (is << 11) | (imm & 0x7FF); }
	constexpr uint32_t IADD(uint32_t id, uint32_t is, uint32_t it) { return lo(0x40) | (it << 16) | (is << 11) | (id << 6) | 0x30; }
	constexpr uint32_t ISUB(uint32_t id, uint32_t is, uint32_t it) { return lo(0x40) | (it << 16) | (is << 11) | (id << 6) | 0x31; }
	constexpr uint32_t IADDI(uint32_t it, uint32_t is, int32_t imm5) { return lo(0x40) | (it << 16) | (is << 11) | ((imm5 & 0x1F) << 6) | 0x32; }
	constexpr uint32_t IAND(uint32_t id, uint32_t is, uint32_t it) { return lo(0x40) | (it << 16) | (is << 11) | (id << 6) | 0x34; }
	constexpr uint32_t IOR(uint32_t id, uint32_t is, uint32_t it) { return lo(0x40) | (it << 16) | (is << 11) | (id << 6) | 0x35; }
	constexpr uint32_t LQI(uint32_t d, uint32_t ft, uint32_t is) { return loS(0x34, d, ft, is); }
	constexpr uint32_t SQI(uint32_t d, uint32_t fs, uint32_t it) { return loS(0x35, d, it, fs); }
	constexpr uint32_t DIV(uint32_t fs, uint32_t fsf, uint32_t ft, uint32_t ftf) { return loS(0x38, (ftf << 2) | fsf, ft, fs); }
	constexpr uint32_t SQRT(uint32_t ft, uint32_t ftf) { return loS(0x39, ftf << 2, ft, 0); }
	constexpr uint32_t RSQRT(uint32_t fs, uint32_t fsf, uint32_t ft, uint32_t ftf) { return loS(0x3A, (ftf << 2) | fsf, ft, fs); }
	constexpr uint32_t WAITQ = loS(0x3B, 0, 0, 0);
	constexpr uint32_t MFIR(uint32_t d, uint32_t ft, uint32_t is) { return loS(0x3D, d, ft, is); }
	constexpr uint32_t XTOP(uint32_t it) { return loS(0x68, 0, it, 0); }
	constexpr uint32_t XITOP(uint32_t it) { return loS(0x69, 0, it, 0); }
	constexpr uint32_t XGKICK(uint32_t is) { return loS(0x6C, 0, 0, is); }
} // namespace vu

struct Pair
{
	uint32_t lower, upper;
};

// =====================================================================================================================
// Test harness
// =====================================================================================================================
struct Program
{
	std::string name;
	std::vector<Pair> code;
	std::vector<uint8_t> data = std::vector<uint8_t>(kKzvuDataSize, 0);
	uint32_t startPc = 0;
	uint32_t top = 0, itop = 0;
	uint32_t budget = 1u << 20;  // per kzvuExecute/kzvuContinue call
	bool compareFloatsExactly = true;

	void setQw(uint32_t qw, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
	{
		uint32_t v[4] = {a, b, c, d};
		std::memcpy(&data[(qw & 0x3ff) * 16], v, 16);
	}
	void setQwF(uint32_t qw, float a, float b, float c, float d)
	{
		float v[4] = {a, b, c, d};
		std::memcpy(&data[(qw & 0x3ff) * 16], v, 16);
	}
	void setQw64(uint32_t qw, uint64_t lo, uint64_t hi)
	{
		std::memcpy(&data[(qw & 0x3ff) * 16], &lo, 8);
		std::memcpy(&data[(qw & 0x3ff) * 16 + 8], &hi, 8);
	}
};

struct GifPacket
{
	std::vector<uint8_t> bytes;
	uint32_t startQw;
};

struct State
{
	uint32_t vf[32][4] = {};
	uint32_t vi[16] = {};
	uint32_t acc[4] = {};
	uint32_t q = 0;
	std::vector<uint8_t> mem;
	std::vector<GifPacket> gif;
	uint32_t cycles = 0;
	int calls = 0;
	bool running = false;
};

static std::vector<GifPacket> g_packets;
static void OnXgkick(void* user, const uint8_t* packet, uint32_t bytes, uint32_t startQw)
{
	g_packets.push_back({std::vector<uint8_t>(packet, packet + bytes), startQw});
}
static uint64_t g_benchPackets = 0;
static void OnXgkickBench(void*, const uint8_t*, uint32_t, uint32_t)
{
	g_benchPackets++;
}

static std::vector<uint32_t> Flatten(const std::vector<Pair>& code)
{
	std::vector<uint32_t> w;
	for (const Pair& p : code)
	{
		w.push_back(p.lower);
		w.push_back(p.upper);
	}
	return w;
}

static KzvuConfig MakeConfig(bool jit)
{
	KzvuConfig cfg;
	cfg.useJit = jit;
	cfg.clampMode = 0;   // Killzone GameIndex
	cfg.iBitHack = true; // Killzone GameIndex
	cfg.xgkick = OnXgkick;
	return cfg;
}

static State CaptureKzvu()
{
	State s;
	for (int i = 0; i < 32; i++)
		kzvuGetVF(i, s.vf[i]);
	for (int i = 0; i < 16; i++)
		s.vi[i] = kzvuGetVI(i) & 0xffff;
	kzvuGetACC(s.acc);
	s.q = kzvuGetVI(kKzvuQ);
	s.mem.assign(kzvuDataMem(), kzvuDataMem() + kKzvuDataSize);
	s.running = kzvuRunning();
	return s;
}

static State RunKzvu(const Program& p, bool jit)
{
	kzvuSetConfig(MakeConfig(jit));
	kzvuReset();
	std::memcpy(kzvuDataMem(), p.data.data(), kKzvuDataSize);
	const std::vector<uint32_t> words = Flatten(p.code);
	kzvuWriteMicro(0, words.data(), static_cast<uint32_t>(words.size() * 4));
	kzvuSetTop(p.top, p.itop);
	g_packets.clear();

	uint32_t cycles = kzvuExecute(p.startPc, p.budget);
	int calls = 1;
	while (kzvuRunning() && calls < 1000000)
	{
		cycles += kzvuContinue(p.budget);
		calls++;
	}
	State s = CaptureKzvu();
	s.gif = g_packets;
	s.cycles = cycles;
	s.calls = calls;
	return s;
}

#ifdef KZVU_TEST_HAVE_PS2RECOMP
alignas(64) static uint8_t g_rcode[0x4000];
alignas(64) static uint8_t g_rdata[0x4000];
static PS2Memory* g_rmem = nullptr;
alignas(16) static uint8_t g_fakeGs[64];
static GS& FakeGs() { return *reinterpret_cast<GS*>(g_fakeGs); }

static State RunRecomp(VU1Interpreter& vu, const Program& p)
{
	vu.reset();
	std::memset(g_rcode, 0, sizeof(g_rcode));
	const std::vector<uint32_t> words = Flatten(p.code);
	std::memcpy(g_rcode, words.data(), words.size() * 4);
	recompMarkCodeModified(g_rmem);
	std::memcpy(g_rdata, p.data.data(), sizeof(g_rdata));
	g_recompGifPackets.clear();

	const uint64_t c0 = vu.state().cycles;
	vu.execute(g_rcode, sizeof(g_rcode), g_rdata, sizeof(g_rdata), FakeGs(), g_rmem, p.startPc, p.top, p.itop, 1u << 24);

	State s;
	const VU1State& st = vu.state();
	std::memcpy(s.vf, st.vf, sizeof(s.vf));
	for (int i = 0; i < 16; i++)
		s.vi[i] = static_cast<uint32_t>(st.vi[i]) & 0xffff;
	std::memcpy(s.acc, st.acc, sizeof(s.acc));
	std::memcpy(&s.q, &st.q, 4);
	s.mem.assign(g_rdata, g_rdata + sizeof(g_rdata));
	for (auto& pk : g_recompGifPackets)
		s.gif.push_back({pk, 0xFFFFFFFFu});
	s.cycles = static_cast<uint32_t>(st.cycles - c0);
	s.calls = 1;
	s.running = !st.ebit ? false : false;
	return s;
}
#endif

// ---- checking ---------------------------------------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;

static void Fail(const std::string& msg)
{
	std::printf("    FAIL: %s\n", msg.c_str());
	g_failures++;
}

static float F(uint32_t bits)
{
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}
static uint32_t U(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

struct Expect
{
	const char* impl;
	const State& s;

	void vf(int r, float x, float y, float z, float w) const
	{
		const float e[4] = {x, y, z, w};
		for (int i = 0; i < 4; i++)
		{
			g_checks++;
			if (s.vf[r][i] != U(e[i]))
				Fail(std::string(impl) + ": vf" + std::to_string(r) + "." + "xyzw"[i] + " = " + std::to_string(F(s.vf[r][i])) +
					 " (0x" + [&] { char b[16]; std::snprintf(b, 16, "%08X", s.vf[r][i]); return std::string(b); }() +
					 "), expected " + std::to_string(e[i]));
		}
	}
	void vfi(int r, int32_t x, int32_t y, int32_t z, int32_t w) const
	{
		const int32_t e[4] = {x, y, z, w};
		for (int i = 0; i < 4; i++)
		{
			g_checks++;
			if (static_cast<int32_t>(s.vf[r][i]) != e[i])
				Fail(std::string(impl) + ": vf" + std::to_string(r) + "." + "xyzw"[i] + " = int " +
					 std::to_string(static_cast<int32_t>(s.vf[r][i])) + ", expected " + std::to_string(e[i]));
		}
	}
	void vi(int r, uint32_t v) const
	{
		g_checks++;
		if (s.vi[r] != (v & 0xffff))
			Fail(std::string(impl) + ": vi" + std::to_string(r) + " = " + std::to_string(s.vi[r]) + ", expected " + std::to_string(v & 0xffff));
	}
	void q(float v) const
	{
		g_checks++;
		if (s.q != U(v))
			Fail(std::string(impl) + ": Q = " + std::to_string(F(s.q)) + ", expected " + std::to_string(v));
	}
	void memF(uint32_t qw, float x, float y, float z, float w) const
	{
		const float e[4] = {x, y, z, w};
		for (int i = 0; i < 4; i++)
		{
			g_checks++;
			uint32_t got;
			std::memcpy(&got, &s.mem[qw * 16 + i * 4], 4);
			if (got != U(e[i]))
				Fail(std::string(impl) + ": mem[qw " + std::to_string(qw) + "]." + "xyzw"[i] + " = " + std::to_string(F(got)) + ", expected " + std::to_string(e[i]));
		}
	}
	void memU(uint32_t qw, int lane, uint32_t v) const
	{
		g_checks++;
		uint32_t got;
		std::memcpy(&got, &s.mem[qw * 16 + lane * 4], 4);
		if (got != v)
			Fail(std::string(impl) + ": mem[qw " + std::to_string(qw) + "]." + "xyzw"[lane] + " = " + std::to_string(got) + ", expected " + std::to_string(v));
	}
};

// Compares two final states; returns a list of differences.
static std::vector<std::string> Diff(const State& a, const State& b, bool exactFloats, bool compareGifStart)
{
	std::vector<std::string> d;
	auto hex = [](uint32_t v) { char buf[16]; std::snprintf(buf, sizeof(buf), "%08X", v); return std::string(buf); };
	auto fdiff = [&](uint32_t x, uint32_t y) {
		if (x == y)
			return false;
		if (exactFloats)
			return true;
		// benchmark data: allow 1 ulp (PS2 FMAC rounding vs IEEE is implementation-specific)
		return std::abs(static_cast<int64_t>(static_cast<int32_t>(x)) - static_cast<int32_t>(y)) > 1;
	};
	for (int r = 1; r < 32; r++)
		for (int i = 0; i < 4; i++)
			if (fdiff(a.vf[r][i], b.vf[r][i]))
				d.push_back("vf" + std::to_string(r) + "." + "xyzw"[i] + ": " + hex(a.vf[r][i]) + " (" + std::to_string(F(a.vf[r][i])) + ") vs " +
							hex(b.vf[r][i]) + " (" + std::to_string(F(b.vf[r][i])) + ")");
	for (int r = 1; r < 16; r++)
		if (a.vi[r] != b.vi[r])
			d.push_back("vi" + std::to_string(r) + ": " + std::to_string(a.vi[r]) + " vs " + std::to_string(b.vi[r]));
	for (int i = 0; i < 4; i++)
		if (fdiff(a.acc[i], b.acc[i]))
			d.push_back(std::string("acc.") + "xyzw"[i] + ": " + hex(a.acc[i]) + " vs " + hex(b.acc[i]));
	if (a.q != b.q)
		d.push_back("Q: " + hex(a.q) + " (" + std::to_string(F(a.q)) + ") vs " + hex(b.q) + " (" + std::to_string(F(b.q)) + ")");
	int memDiffs = 0;
	for (uint32_t i = 0; i < kKzvuDataSize; i += 4)
	{
		uint32_t x, y;
		std::memcpy(&x, &a.mem[i], 4);
		std::memcpy(&y, &b.mem[i], 4);
		if (fdiff(x, y))
		{
			if (memDiffs++ < 8)
				d.push_back("mem[0x" + hex(i) + "]: " + hex(x) + " vs " + hex(y));
		}
	}
	if (memDiffs > 8)
		d.push_back("... " + std::to_string(memDiffs) + " differing memory words in total");
	if (a.gif.size() != b.gif.size())
		d.push_back("XGKICK packets: " + std::to_string(a.gif.size()) + " vs " + std::to_string(b.gif.size()));
	else
		for (size_t i = 0; i < a.gif.size(); i++)
		{
			if (a.gif[i].bytes.size() != b.gif[i].bytes.size())
				d.push_back("XGKICK packet " + std::to_string(i) + " size: " + std::to_string(a.gif[i].bytes.size()) + " vs " + std::to_string(b.gif[i].bytes.size()));
			else
			{
				size_t nd = 0;
				for (size_t j = 0; j + 4 <= a.gif[i].bytes.size(); j += 4)
				{
					uint32_t x, y;
					std::memcpy(&x, &a.gif[i].bytes[j], 4);
					std::memcpy(&y, &b.gif[i].bytes[j], 4);
					if (fdiff(x, y))
						nd++;
				}
				if (nd)
					d.push_back("XGKICK packet " + std::to_string(i) + ": " + std::to_string(nd) + " differing words");
			}
			if (compareGifStart && a.gif[i].startQw != b.gif[i].startQw)
				d.push_back("XGKICK packet " + std::to_string(i) + " startQw: " + std::to_string(a.gif[i].startQw) + " vs " + std::to_string(b.gif[i].startQw));
		}
	return d;
}

// =====================================================================================================================
// Programs
// =====================================================================================================================
using namespace vu;

// 1) Upper FMAC ops with fields and broadcasts, ACC chains, ITOF/FTOI, LQ/SQ with immediate offsets.
static Program ProgFmac()
{
	Program p;
	p.name = "fmac: ADD/MUL/MADD/MULA/MADDA + broadcast/fields, ITOF/FTOI, LQ/SQ";
	p.setQwF(0x10, 1.0f, 2.0f, 3.0f, 4.0f);
	p.setQwF(0x11, 10.0f, 20.0f, 30.0f, 40.0f);
	p.setQw(0x12, 5, static_cast<uint32_t>(-3), 100, 7);   // integers for ITOF
	p.setQwF(0x13, 1.5f, -2.75f, 1000.25f, 0.0625f);       // for FTOI
	p.code = {
		{LQ(XYZW, 1, 0, 0x10), NOPu},
		{LQ(XYZW, 2, 0, 0x11), NOPu},
		{LQ(XYZW, 7, 0, 0x12), NOPu},
		{LQ(XYZW, 11, 0, 0x13), NOPu},
		{NOPl, ADD(XYZW, 3, 1, 2)},              // vf3 = (11,22,33,44)
		{NOPl, MULbc(XYZ, 4, 1, 2, bcX)},        // vf4.xyz = vf1.xyz * 10 = (10,20,30), w stays 0
		{NOPl, MULA(XYZW, 1, 2)},                // ACC = (10,40,90,160)
		{NOPl, MADD(XYZW, 5, 1, 1)},             // vf5 = ACC + vf1*vf1 = (11,44,99,176)
		{NOPl, MADDbc(X | Y, 6, 2, 1, bcW)},     // vf6.xy = ACC.xy + vf2.xy*4 = (50,120), zw stay 0
		{NOPl, MULAbc(XYZW, 1, 2, bcY)},         // ACC = vf1 * 20 = (20,40,60,80)
		{NOPl, MADDAbc(XYZW, 2, 1, bcX)},        // ACC += vf2 * 1 = (30,60,90,120)
		{NOPl, MADDbc(XYZW, 12, 1, 0, bcW)},     // vf12 = ACC + vf1 * vf0.w(1) = (31,62,93,124)
		{NOPl, SUB(Y | W, 13, 2, 1)},            // vf13.yw = (18, 36)
		{NOPl, ITOF0(XYZW, 8, 7)},               // vf8 = (5,-3,100,7)
		{NOPl, ITOF12(XYZW, 10, 7)},             // vf10 = vf7 / 4096
		{NOPl, FTOI0(XYZW, 9, 11)},              // vf9 = trunc(1.5,-2.75,1000.25,0.0625) = (1,-2,1000,0)
		{NOPl, FTOI4(XYZW, 14, 11)},             // vf14 = trunc(x*16) = (24,-44,16004,1)
		{SQ(XYZW, 3, 0, 0x20), NOPu},
		{SQ(XYZW, 5, 0, 0x21), NOPu},
		{SQ(X | Z, 6, 0, 0x22), NOPu},           // only x,z written: (50, -, 0, -)
		{SQ(XYZW, 9, 0, 0x23), NOPu},
		{NOPl, NOPu | Ebit},
		{NOPl, NOPu},
	};
	p.setQwF(0x22, -1.0f, -1.0f, -1.0f, -1.0f); // pre-fill to see the field mask
	return p;
}

static void CheckFmac(const Expect& e)
{
	e.vf(3, 11, 22, 33, 44);
	e.vf(4, 10, 20, 30, 0);
	e.vf(5, 11, 44, 99, 176);
	e.vf(6, 50, 120, 0, 0);
	e.vf(12, 31, 62, 93, 124);
	e.vf(13, 0, 18, 0, 36);
	e.vf(8, 5, -3, 100, 7);
	e.vf(10, 5.0f / 4096.0f, -3.0f / 4096.0f, 100.0f / 4096.0f, 7.0f / 4096.0f);
	e.vfi(9, 1, -2, 1000, 0);
	e.vfi(14, 24, -44, 16004, 1);
	e.memF(0x20, 11, 22, 33, 44);
	e.memF(0x21, 11, 44, 99, 176);
	e.memF(0x22, 50, -1, 0, -1);
	e.memU(0x23, 0, 1);
	e.memU(0x23, 1, static_cast<uint32_t>(-2));
}

// 2) Integer ops, a counted loop with IBNE + delay slot, ISW/ILW.
static Program ProgIntLoop(uint32_t count)
{
	Program p;
	p.name = "integer: IADDIU/IADD/IADDI/ISUB/IAND/IOR, IBNE loop with delay slot, ISW/ILW";
	p.code = {
		/* 0 */ {IADDIU(1, 0, count), NOPu},
		/* 1 */ {IADDIU(2, 0, 0), NOPu},
		/* 2 */ {IADD(2, 2, 1), NOPu},      // loop: vi2 += vi1
		/* 3 */ {IADDI(1, 1, -1), NOPu},    // vi1--
		/* 4 */ {IBNE(1, 0, -3), NOPu},     // -> 2 (target = 4*8 + 8 - 3*8 = 16)
		/* 5 */ {IADDIU(3, 3, 1), NOPu},    // delay slot, runs every iteration
		/* 6 */ {ISW(X, 2, 0, 0x30), NOPu}, // mem[0x30].x = vi2
		/* 7 */ {ISW(W, 3, 0, 0x30), NOPu}, // mem[0x30].w = vi3
		/* 8 */ {ILW(Y, 4, 0, 0x31), NOPu}, // vi4 = mem[0x31].y
		/* 9 */ {ISUB(5, 2, 3), NOPu},
		/*10 */ {IAND(6, 2, 3), NOPu},
		/*11 */ {IOR(7, 2, 3), NOPu},
		/*12 */ {ISUBIU(8, 2, 0x1234), NOPu},
		/*13 */ {IADDI(9, 0, -16), NOPu},
		/*14 */ {NOPl, NOPu | Ebit},
		/*15 */ {NOPl, NOPu},
	};
	p.setQw(0x31, 0, 0xBEEF, 0, 0);
	return p;
}

static void CheckIntLoop(const Expect& e, uint32_t count)
{
	const uint32_t sum = count * (count + 1) / 2;
	e.vi(1, 0);
	e.vi(2, sum);
	e.vi(3, count);
	e.vi(4, 0xBEEF);
	e.vi(5, sum - count);
	e.vi(6, sum & count);
	e.vi(7, sum | count);
	e.vi(8, sum - 0x1234);
	e.vi(9, static_cast<uint32_t>(-16));
	e.memU(0x30, 0, sum & 0xffff);
	e.memU(0x30, 3, count);
}

// 3) LQI/SQI with VI post-increment in a loop.
static Program ProgLqiSqi()
{
	Program p;
	p.name = "memory: LQI/SQI post-increment loop";
	for (uint32_t i = 0; i < 4; i++)
		p.setQwF(0x40 + i, 1.0f + i, 2.0f + i, 3.0f + i, 4.0f + i);
	p.code = {
		/* 0 */ {IADDIU(1, 0, 0x40), NOPu},
		/* 1 */ {IADDIU(2, 0, 0x80), NOPu},
		/* 2 */ {IADDIU(3, 0, 4), NOPu},
		/* 3 */ {LQI(XYZW, 1, 1), NOPu},           // loop: vf1 = mem[vi1++]
		/* 4 */ {NOPl, ADD(XYZW, 2, 1, 1)},        // vf2 = vf1 * 2
		/* 5 */ {SQI(XYZW, 2, 2), NOPu},           // mem[vi2++] = vf2
		/* 6 */ {IADDI(3, 3, -1), NOPu},
		/* 7 */ {IBNE(3, 0, -5), NOPu},            // -> 3
		/* 8 */ {NOPl, NOPu},
		/* 9 */ {NOPl, NOPu | Ebit},
		/*10 */ {NOPl, NOPu},
	};
	return p;
}

static void CheckLqiSqi(const Expect& e)
{
	e.vi(1, 0x44);
	e.vi(2, 0x84);
	e.vi(3, 0);
	for (uint32_t i = 0; i < 4; i++)
		e.memF(0x80 + i, 2.0f * (1 + i), 2.0f * (2 + i), 2.0f * (3 + i), 2.0f * (4 + i));
	e.vf(2, 8, 10, 12, 14);
}

// 4) FDIV unit: DIV/SQRT/RSQRT into Q, WAITQ, Q-broadcast ops.
static Program ProgDiv()
{
	Program p;
	p.name = "fdiv: DIV/SQRT/RSQRT + WAITQ, MULq/ADDq";
	p.setQwF(0x10, 9.0f, 2.0f, 16.0f, 0.0f);
	p.code = {
		{LQ(XYZW, 1, 0, 0x10), NOPu},
		{DIV(1, bcX, 1, bcY), NOPu},             // Q = 9 / 2
		{WAITQ, NOPu},
		{NOPl, MULq(X, 2, 1)},                   // vf2.x = 9 * 4.5 = 40.5
		{SQRT(1, bcZ), NOPu},                    // Q = sqrt(16) = 4
		{WAITQ, NOPu},
		{NOPl, ADDq(Y, 2, 0)},                   // vf2.y = 0 + 4
		{RSQRT(1, bcX, 1, bcZ), NOPu},           // Q = 9 / sqrt(16) = 2.25
		{WAITQ, NOPu},
		{NOPl, MULq(Z, 2, 1)},                   // vf2.z = 16 * 2.25 = 36
		{SQ(XYZW, 2, 0, 0x11), NOPu},
		{NOPl, NOPu | Ebit},
		{NOPl, NOPu},
	};
	return p;
}

static void CheckDiv(const Expect& e)
{
	e.vf(2, 40.5f, 4.0f, 36.0f, 0.0f);
	e.q(2.25f);
	e.memF(0x11, 40.5f, 4.0f, 36.0f, 0.0f);
}

// 5) XGKICK: one plain PACKED A+D packet and one that wraps past the end of VU1 data memory.
static constexpr uint64_t GifTag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg)
{
	return static_cast<uint64_t>(nloop & 0x7fff) | (static_cast<uint64_t>(eop) << 15) | (static_cast<uint64_t>(flg) << 58) |
		   (static_cast<uint64_t>(nreg & 0xf) << 60);
}

static Program ProgXgkick()
{
	Program p;
	p.name = "xgkick: PATH1 packet + packet wrapping the 16KB boundary";
	// packet A at qw 0x100: 2 A+D writes
	p.setQw64(0x100, GifTag(2, true, 0, 1), 0xE);
	p.setQw64(0x101, 0x1111222233334444ull, 0x3F);
	p.setQw64(0x102, 0x5555666677778888ull, 0x40);
	// packet B at qw 0x3FE: tag, data at 0x3FF and 0x000 (wraps)
	p.setQw64(0x3FE, GifTag(2, true, 0, 1), 0xE);
	p.setQw64(0x3FF, 0xAAAABBBBCCCCDDDDull, 0x42);
	p.setQw64(0x000, 0x0123456789ABCDEFull, 0x43);
	p.code = {
		{IADDIU(1, 0, 0x100), NOPu},
		{IADDIU(2, 0, 0x3FE), NOPu},
		{XGKICK(1), NOPu},
		{NOPl, NOPu},
		{NOPl, NOPu},
		{XGKICK(2), NOPu},
		{NOPl, NOPu},
		{NOPl, NOPu | Ebit},
		{NOPl, NOPu},
	};
	return p;
}

static void CheckXgkick(const char* impl, const State& s, const Program& p, bool checkStart)
{
	g_checks++;
	if (s.gif.size() != 2)
	{
		Fail(std::string(impl) + ": expected 2 XGKICK packets, got " + std::to_string(s.gif.size()));
		return;
	}
	auto expectPacket = [&](size_t idx, std::vector<uint32_t> qws, uint32_t startQw) {
		std::vector<uint8_t> want;
		for (uint32_t q : qws)
			want.insert(want.end(), &p.data[q * 16], &p.data[q * 16] + 16);
		g_checks++;
		if (s.gif[idx].bytes != want)
			Fail(std::string(impl) + ": XGKICK packet " + std::to_string(idx) + " bytes differ (size " +
				 std::to_string(s.gif[idx].bytes.size()) + ", expected " + std::to_string(want.size()) + ")");
		if (checkStart)
		{
			g_checks++;
			if (s.gif[idx].startQw != startQw)
				Fail(std::string(impl) + ": XGKICK packet " + std::to_string(idx) + " startQw " + std::to_string(s.gif[idx].startQw) +
					 ", expected " + std::to_string(startQw));
		}
	};
	expectPacket(0, {0x100, 0x101, 0x102}, 0x100);
	expectPacket(1, {0x3FE, 0x3FF, 0x000}, 0x3FE);
}

// 6) XTOP/XITOP read the VIF1 TOP/ITOP the host provided.
static Program ProgXtop()
{
	Program p;
	p.name = "xtop: XTOP/XITOP";
	p.top = 0x1A0;
	p.itop = 0x155;
	p.code = {
		{XTOP(1), NOPu},
		{XITOP(2), NOPu},
		{IADD(3, 1, 2), NOPu},
		{NOPl, NOPu | Ebit},
		{NOPl, NOPu},
	};
	return p;
}

static void CheckXtop(const Expect& e)
{
	e.vi(1, 0x1A0);
	e.vi(2, 0x155);
	e.vi(3, 0x1A0 + 0x155);
}

// Benchmark / transform: 4x4 matrix * N vertices (MULA/MADDA/MADD), perspective divide (DIV + WAITQ + MULq),
// FTOI4, SQI into a GIF packet, XGKICK. 16 vertices -> ~220 instruction pairs per run.
static Program ProgTransform(uint32_t nverts)
{
	Program p;
	p.name = "transform: matrix*vertex loop + divide + FTOI4 + XGKICK";
	p.compareFloatsExactly = false;
	// matrix rows (column-major style: out = row0*x + row1*y + row2*z + row3*w)
	p.setQwF(0, 1.5f, 0.25f, -0.5f, 0.0f);
	p.setQwF(1, -0.25f, 1.25f, 0.75f, 0.0f);
	p.setQwF(2, 0.5f, -0.75f, 1.0f, 0.125f);
	p.setQwF(3, 320.0f, 224.0f, 16.0f, 1.0f);
	for (uint32_t i = 0; i < nverts; i++)
		p.setQwF(0x10 + i, 10.0f + 3.3f * i, -20.0f + 1.7f * i, 5.0f + 0.9f * i, 1.0f);
	p.setQw64(0x100, GifTag(nverts, true, 0, 1), 0x5); // PACKED, NREG=1, XYZ2
	p.code = {
		/* 0 */ {LQ(XYZW, 1, 0, 0), NOPu},
		/* 1 */ {LQ(XYZW, 2, 0, 1), NOPu},
		/* 2 */ {LQ(XYZW, 3, 0, 2), NOPu},
		/* 3 */ {LQ(XYZW, 4, 0, 3), NOPu},
		/* 4 */ {IADDIU(1, 0, 0x10), NOPu},
		/* 5 */ {IADDIU(2, 0, 0x101), NOPu},
		/* 6 */ {IADDIU(3, 0, nverts), NOPu},
		/* 7 */ {LQI(XYZW, 5, 1), NOPu},                // loop
		/* 8 */ {NOPl, MULAbc(XYZW, 1, 5, bcX)},
		/* 9 */ {NOPl, MADDAbc(XYZW, 2, 5, bcY)},
		/*10 */ {NOPl, MADDAbc(XYZW, 3, 5, bcZ)},
		/*11 */ {NOPl, MADDbc(XYZW, 6, 4, 5, bcW)},
		/*12 */ {DIV(0, bcW, 6, bcW), NOPu},            // Q = 1 / w
		/*13 */ {WAITQ, NOPu},
		/*14 */ {NOPl, MULq(XYZ, 6, 6)},
		/*15 */ {NOPl, FTOI4(XYZW, 7, 6)},
		/*16 */ {SQI(XYZW, 7, 2), NOPu},
		/*17 */ {IADDI(3, 3, -1), NOPu},
		/*18 */ {IBNE(3, 0, -12), NOPu},               // -> 7
		/*19 */ {NOPl, NOPu},
		/*20 */ {IADDIU(4, 0, 0x100), NOPu},
		/*21 */ {XGKICK(4), NOPu},
		/*22 */ {NOPl, NOPu | Ebit},
		/*23 */ {NOPl, NOPu},
	};
	return p;
}

// Host-side reference for the transform (double precision, then truncated like FTOI4).
static void CheckTransform(const Expect& e, const Program& p, uint32_t nverts)
{
	auto rd = [&](uint32_t qw, int lane) { float f; std::memcpy(&f, &p.data[qw * 16 + lane * 4], 4); return static_cast<double>(f); };
	int bad = 0;
	for (uint32_t v = 0; v < nverts; v++)
	{
		double o[4];
		for (int l = 0; l < 4; l++)
			o[l] = rd(0, l) * rd(0x10 + v, 0) + rd(1, l) * rd(0x10 + v, 1) + rd(2, l) * rd(0x10 + v, 2) + rd(3, l) * rd(0x10 + v, 3);
		const double q = 1.0 / o[3];
		for (int l = 0; l < 4; l++)
		{
			const double val = (l < 3 ? o[l] * q : o[l]) * 16.0;
			int32_t got;
			std::memcpy(&got, &e.s.mem[(0x101 + v) * 16 + l * 4], 4);
			g_checks++;
			if (std::abs(static_cast<double>(got) - std::trunc(val)) > 1.0)
			{
				if (bad++ < 4)
					Fail(std::string(e.impl) + ": transform vertex " + std::to_string(v) + "." + "xyzw"[l] + " = " + std::to_string(got) +
						 ", reference " + std::to_string(std::trunc(val)));
			}
		}
	}
	g_checks++;
	if (e.s.gif.size() != 1 || e.s.gif[0].bytes.size() != (nverts + 1) * 16)
		Fail(std::string(e.impl) + ": transform XGKICK packet missing or wrong size");
}

// =====================================================================================================================
// main
// =====================================================================================================================
int main(int argc, char** argv)
{
	int benchRuns = 10000;
	bool bench = true;
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--bench-runs") && i + 1 < argc)
			benchRuns = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--no-bench"))
			bench = false;
	}

	std::string err;
	if (!kzvuInit(MakeConfig(true), &err))
	{
		std::printf("kzvuInit failed: %s\n", err.c_str());
		return 1;
	}

#ifdef KZVU_TEST_HAVE_PS2RECOMP
	g_rmem = recompCreateMemory(g_rcode, g_rdata);
	VU1Interpreter recomp(VU1Interpreter::Unit::VU1);
	std::printf("kzvu_test: microVU (JIT) vs PCSX2 interpreter vs PS2Recomp VU1Interpreter\n\n");
#else
	std::printf("kzvu_test: microVU (JIT) vs PCSX2 interpreter (PS2Recomp comparison not built)\n\n");
#endif

	struct Case
	{
		Program prog;
		std::function<void(const char*, const State&)> check;
	};
	const uint32_t loopCount = 10;
	const uint32_t nverts = 16;
	std::vector<Case> cases;
	cases.push_back({ProgFmac(), [](const char* impl, const State& s) { CheckFmac({impl, s}); }});
	cases.push_back({ProgIntLoop(loopCount), [&](const char* impl, const State& s) { CheckIntLoop({impl, s}, loopCount); }});
	cases.push_back({ProgLqiSqi(), [](const char* impl, const State& s) { CheckLqiSqi({impl, s}); }});
	cases.push_back({ProgDiv(), [](const char* impl, const State& s) { CheckDiv({impl, s}); }});
	{
		Program px = ProgXgkick();
		cases.push_back({px, [px](const char* impl, const State& s) { CheckXgkick(impl, s, px, std::strcmp(impl, "ps2recomp") != 0); }});
	}
	cases.push_back({ProgXtop(), [](const char* impl, const State& s) { CheckXtop({impl, s}); }});
	{
		Program pt = ProgTransform(nverts);
		cases.push_back({pt, [pt, nverts](const char* impl, const State& s) { CheckTransform({impl, s}, pt, nverts); }});
	}

	int mismatchesRecomp = 0;
	for (Case& c : cases)
	{
		std::printf("[%s]\n", c.prog.name.c_str());
		const int before = g_failures;
		const State jit = RunKzvu(c.prog, true);
		const State interp = RunKzvu(c.prog, false);
		c.check("jit", jit);
		c.check("interp", interp);
		for (const std::string& d : Diff(jit, interp, c.prog.compareFloatsExactly, true))
			Fail("jit vs interp: " + d);
		std::printf("    kzvu jit: %u cycles   kzvu interp: %u cycles", jit.cycles, interp.cycles);
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		const State rc = RunRecomp(recomp, c.prog);
		c.check("ps2recomp", rc);
		const std::vector<std::string> d = Diff(jit, rc, c.prog.compareFloatsExactly, false);
		std::printf("   ps2recomp: %u cycles\n", rc.cycles);
		for (const std::string& line : d)
		{
			std::printf("    MISMATCH jit vs ps2recomp: %s\n", line.c_str());
			mismatchesRecomp++;
		}
#else
		std::printf("\n");
#endif
		std::printf("    %s\n", g_failures == before ? "ok" : "FAILED");
	}

	// ---- budget / resume: a long loop run in 64-cycle slices must end in the same state as one call -----------------
	{
		std::printf("[budget: 500-iteration loop in 64-cycle slices via kzvuContinue]\n");
		const int before = g_failures;
		Program p = ProgIntLoop(500);
		const State whole = RunKzvu(p, true);
		p.budget = 64;
		for (bool jit : {true, false})
		{
			const State sliced = RunKzvu(p, jit);
			const char* impl = jit ? "jit" : "interp";
			CheckIntLoop({impl, sliced}, 500);
			g_checks++;
			if (sliced.calls < 10)
				Fail(std::string(impl) + ": expected the program to need many slices, took " + std::to_string(sliced.calls));
			for (const std::string& d : Diff(whole, sliced, true, true))
				Fail(std::string(impl) + " sliced vs whole: " + d);
			std::printf("    %s: %d calls, %u cycles (single call: %u cycles)\n", impl, sliced.calls, sliced.cycles, whole.cycles);
		}
		std::printf("    %s\n", g_failures == before ? "ok" : "FAILED");
	}

	// ---- JIT invalidation ---------------------------------------------------------------------------------------------
	{
		std::printf("[invalidation: kzvuWriteMicro / kzvuMicroWritten]\n");
		const int before = g_failures;
		kzvuSetConfig(MakeConfig(true));
		kzvuReset();
		auto prog = [](uint32_t value) {
			return Flatten({{IADDIU(1, 0, value), NOPu}, {NOPl, NOPu | Ebit}, {NOPl, NOPu}});
		};
		const std::vector<uint32_t> a = prog(111), b = prog(222);
		auto run = [] { kzvuExecute(0, 1000); return kzvuGetVI(1); };
		kzvuWriteMicro(0, a.data(), static_cast<uint32_t>(a.size() * 4));
		g_checks++;
		if (run() != 111) Fail("program A did not return 111");
		kzvuWriteMicro(0, b.data(), static_cast<uint32_t>(b.size() * 4));
		g_checks++;
		if (run() != 222) Fail("after kzvuWriteMicro(B) the JIT did not run B");
		std::memcpy(kzvuCodeMem(), a.data(), a.size() * 4);
		kzvuMicroWritten(0, static_cast<uint32_t>(a.size() * 4));
		g_checks++;
		if (run() != 111) Fail("after direct write of A + kzvuMicroWritten the JIT did not run A");
		// Contract check: a direct write WITHOUT notification is not seen by the JIT (it keeps running cached code).
		std::memcpy(kzvuCodeMem(), b.data(), b.size() * 4);
		const uint32_t stale = run();
		std::printf("    direct write without kzvuMicroWritten -> JIT returned %u (%s)\n", stale,
			stale == 111 ? "stale cached code, as documented" : "fresh");
		kzvuMicroWritten(0, static_cast<uint32_t>(b.size() * 4));
		g_checks++;
		if (run() != 222) Fail("after kzvuMicroWritten the JIT did not pick up B");
		std::printf("    %s\n", g_failures == before ? "ok" : "FAILED");
	}

	std::printf("\n%d checks, %d failures", g_checks, g_failures);
#ifdef KZVU_TEST_HAVE_PS2RECOMP
	std::printf(", %d jit-vs-ps2recomp differences (informational, see above)", mismatchesRecomp);
#endif
	std::printf("\n");

	// ---- benchmark ----------------------------------------------------------------------------------------------------
	if (bench && benchRuns > 0)
	{
		const Program p = ProgTransform(nverts);
		const std::vector<uint32_t> words = Flatten(p.code);
		std::printf("\nbenchmark: %s, %u vertices, %d runs\n", p.name.c_str(), nverts, benchRuns);
		for (bool jit : {true, false})
		{
			KzvuConfig cfg = MakeConfig(jit);
			cfg.xgkick = OnXgkickBench;
			kzvuSetConfig(cfg);
			kzvuReset();
			std::memcpy(kzvuDataMem(), p.data.data(), kKzvuDataSize);
			kzvuWriteMicro(0, words.data(), static_cast<uint32_t>(words.size() * 4));
			for (int i = 0; i < 50; i++)
				kzvuExecute(0, 1u << 20);
			g_benchPackets = 0;
			uint64_t cycles = 0;
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < benchRuns; i++)
				cycles += kzvuExecute(0, 1u << 20);
			const auto t1 = std::chrono::steady_clock::now();
			const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / benchRuns;
			std::printf("  kzvu %-7s %10.0f ns/run  (%llu VU cycles/run, %.2f ns/VU cycle, %llu packets)\n", jit ? "jit" : "interp", ns,
				static_cast<unsigned long long>(cycles / benchRuns), ns / (static_cast<double>(cycles) / benchRuns),
				static_cast<unsigned long long>(g_benchPackets));
		}
#ifdef KZVU_TEST_HAVE_PS2RECOMP
		{
			recomp.reset();
			std::memset(g_rcode, 0, sizeof(g_rcode));
			std::memcpy(g_rcode, words.data(), words.size() * 4);
			recompMarkCodeModified(g_rmem);
			std::memcpy(g_rdata, p.data.data(), sizeof(g_rdata));
			for (int i = 0; i < 50; i++)
				recomp.execute(g_rcode, sizeof(g_rcode), g_rdata, sizeof(g_rdata), FakeGs(), g_rmem, 0, 0, 0, 1u << 24);
			g_recompGifPackets.clear();
			const uint64_t c0 = recomp.state().cycles;
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < benchRuns; i++)
			{
				recomp.execute(g_rcode, sizeof(g_rcode), g_rdata, sizeof(g_rdata), FakeGs(), g_rmem, 0, 0, 0, 1u << 24);
				if (g_recompGifPackets.size() > 64)
					g_recompGifPackets.clear();
			}
			const auto t1 = std::chrono::steady_clock::now();
			const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / benchRuns;
			const uint64_t cycles = recomp.state().cycles - c0;
			std::printf("  ps2recomp      %10.0f ns/run  (%llu VU cycles/run)\n", ns, static_cast<unsigned long long>(cycles / benchRuns));
		}
#endif
	}

	kzvuShutdown();
	std::printf("\n%s\n", g_failures ? "FAIL" : "PASS");
	return g_failures ? 1 : 0;
}
