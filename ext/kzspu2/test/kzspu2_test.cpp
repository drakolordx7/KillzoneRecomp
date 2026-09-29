// kzspu2_test: proves that kzspu2 generates the right sound from register writes and IOP DMA, the way libsd drives the
// SPU2 on a PS2. Exit code 0 = pass.
//
//  1. A 1 kHz sine (48 samples per period, 7 periods) is encoded to SPU2 ADPCM (VAG) with a brute-force encoder,
//     as a looping sample (LOOP_START on the first block, LOOP|LOOP_END on the last).
//  2. It is uploaded twice by DMA: core 0 (IOP DMA ch4) to 0x5000 and core 1 (IOP DMA ch7) to 0x6000 (SPU2 halfword
//     addresses), with TSA set by register writes. Both DMA-end callbacks must fire, after a realistic delay.
//  3. DMA read (SPU2 -> IOP) of both regions must return the uploaded bytes.
//  4. Core 0 voice 0 plays it at pitch 0x1000 (48 kHz, left only) and core 1 voice 5 at pitch 0x0800 (24 kHz, right
//     only). After one second of IOP time the left channel must be a 1000 Hz tone and the right a 500 Hz tone
//     (zero crossings and a Goertzel check), both clearly non-silent.
//  5. IRQ: core 0 IRQA points into the sample; the SPU2 IRQ callback must fire, and fire again after an ATTR
//     IRQ-enable toggle (libsd's acknowledge).
//  6. Auto-DMA (ADMA, sceSdBlockTrans): 170 ms of 16-bit PCM (3 kHz left, 750 Hz right) streamed through core 0's
//     input. The tones must appear in the output, and the DMA-end callback must come only after the data has played.
//
// The mixed output of steps 4 and 6 is written to test/out/*.wav for listening.
#include "kzspu2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	constexpr double kPi = 3.14159265358979323846;
	constexpr uint32_t kSpu2 = 0xBF900000u; // KSEG1 mirror, as IRX code accesses it
	constexpr uint64_t kCyclesPerSample = 768;

	int g_failures = 0;
	void check(bool ok, const char* what)
	{
		std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
		if (!ok)
			++g_failures;
	}

	// ---- host state --------------------------------------------------------------------------------------------------------
	uint64_t g_cycle = 1000;
	std::vector<int16_t> g_capture;
	uint32_t g_irqs = 0;
	uint32_t g_dmaDone[2] = {};
	uint64_t g_dmaDoneCycle[2] = {};

	void onIrq(void*) { ++g_irqs; }
	void onDma(void*, int core)
	{
		++g_dmaDone[core];
		g_dmaDoneCycle[core] = g_cycle;
	}
	void onSamples(void*, const int16_t* s, uint32_t frames) { g_capture.insert(g_capture.end(), s, s + frames * 2); }
	void onLog(void*, int level, const char* msg) { std::printf("  [kzspu2 log %d] %s\n", level, msg); }

	void w16(uint32_t reg, uint16_t v) { kzspu2Write16(kSpu2 + reg, v, g_cycle); }
	uint16_t r16(uint32_t reg) { return kzspu2Read16(kSpu2 + reg, g_cycle); }
	void w32addr(uint32_t reg, uint32_t addr) // hi word (bits 19:16) then lo word, as libsd writes addresses
	{
		w16(reg, static_cast<uint16_t>((addr >> 16) & 0xF));
		w16(reg + 2, static_cast<uint16_t>(addr & 0xFFFF));
	}

	void advance(uint64_t cycles, uint64_t step = kCyclesPerSample * 64)
	{
		const uint64_t end = g_cycle + cycles;
		while (g_cycle < end)
		{
			g_cycle = std::min(end, g_cycle + step);
			kzspu2Advance(g_cycle);
			int16_t drain[2048 * 2];
			while (kzspu2ReadSamples(drain, 2048)) // keep the ring empty (the tap captures everything)
			{
			}
		}
	}

	bool init()
	{
		KzSpu2Config cfg;
		cfg.irq = onIrq;
		cfg.dmaComplete = onDma;
		cfg.samples = onSamples;
		cfg.log = onLog;
		g_capture.clear();
		g_irqs = 0;
		g_dmaDone[0] = g_dmaDone[1] = 0;
		return kzspu2Init(cfg);
	}

	// ---- SPU2 ADPCM encoder (brute force over the 5 filters and 13 shifts per 28-sample block) ------------------------------
	constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

	std::vector<uint8_t> encodeAdpcm(const std::vector<int16_t>& pcm)
	{
		std::vector<uint8_t> out;
		int p1 = 0, p2 = 0;
		const size_t blocks = (pcm.size() + 27) / 28;
		for (size_t b = 0; b < blocks; ++b)
		{
			double bestErr = 1e300;
			int bestF = 0, bestS = 0, bestP1 = 0, bestP2 = 0;
			uint8_t bestNib[28] = {};
			for (int f = 0; f < 5; ++f)
			{
				for (int s = 0; s <= 12; ++s)
				{
					int q1 = p1, q2 = p2;
					double err = 0;
					uint8_t nib[28];
					for (int i = 0; i < 28; ++i)
					{
						const size_t idx = b * 28 + i;
						const int x = idx < pcm.size() ? pcm[idx] : 0;
						const int pred = (kFilter[f][0] * q1 + kFilter[f][1] * q2 + 32) >> 6;
						const double step = static_cast<double>(1 << (12 - s));
						int n = static_cast<int>(std::lround((x - pred) / step));
						n = std::clamp(n, -8, 7);
						// decode exactly like the SPU2: (nibble << 28) >> (shift + 16)
						const int32_t data = static_cast<int32_t>(static_cast<uint32_t>(n & 0xF) << 28) >> (s + 16);
						int y = std::clamp(data + pred, -32768, 32767);
						err += static_cast<double>(x - y) * (x - y);
						nib[i] = static_cast<uint8_t>(n & 0xF);
						q2 = q1;
						q1 = y;
					}
					if (err < bestErr)
					{
						bestErr = err;
						bestF = f;
						bestS = s;
						bestP1 = q1;
						bestP2 = q2;
						std::memcpy(bestNib, nib, 28);
					}
				}
			}
			uint8_t flags = 0;
			if (b == 0)
				flags |= 4 | 2; // LOOP_START | LOOP
			if (b == blocks - 1)
				flags |= 1 | 2; // LOOP_END | LOOP
			out.push_back(static_cast<uint8_t>((bestF << 4) | bestS));
			out.push_back(flags);
			for (int i = 0; i < 28; i += 2)
				out.push_back(static_cast<uint8_t>(bestNib[i] | (bestNib[i + 1] << 4)));
			p1 = bestP1;
			p2 = bestP2;
		}
		return out;
	}

	// ---- analysis ------------------------------------------------------------------------------------------------------------
	struct ChannelStats
	{
		double rms = 0;
		int peak = 0;
		double zcFreq = 0;
	};

	ChannelStats analyze(const std::vector<int16_t>& st, int ch, size_t from, size_t to)
	{
		ChannelStats r;
		to = std::min(to, st.size() / 2);
		if (to <= from + 1)
			return r;
		double sum = 0;
		size_t crossings = 0;
		int prev = st[from * 2 + ch];
		for (size_t i = from; i < to; ++i)
		{
			const int v = st[i * 2 + ch];
			sum += static_cast<double>(v) * v;
			r.peak = std::max(r.peak, std::abs(v));
			if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0))
				++crossings;
			prev = v;
		}
		const double n = static_cast<double>(to - from);
		r.rms = std::sqrt(sum / n);
		r.zcFreq = (crossings / 2.0) / (n / 48000.0);
		return r;
	}

	// Relative power at `freq` (Goertzel) against total power; a pure tone gives ~1.
	double toneShare(const std::vector<int16_t>& st, int ch, size_t from, size_t to, double freq)
	{
		to = std::min(to, st.size() / 2);
		const double w = 2 * kPi * freq / 48000.0, coeff = 2 * std::cos(w);
		double s1 = 0, s2 = 0, total = 0;
		for (size_t i = from; i < to; ++i)
		{
			const double x = st[i * 2 + ch];
			const double s0 = x + coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
			total += x * x;
		}
		const double n = static_cast<double>(to - from);
		const double power = (s1 * s1 + s2 * s2 - coeff * s1 * s2) * 2.0 / n; // = n * A^2 / 2 for amplitude A
		return total > 0 ? power / total : 0;
	}

	void writeWav(const std::filesystem::path& path, const std::vector<int16_t>& st)
	{
		std::filesystem::create_directories(path.parent_path());
		FILE* f = std::fopen(path.string().c_str(), "wb");
		if (!f)
			return;
		const uint32_t data = static_cast<uint32_t>(st.size() * 2);
		auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
		auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
		std::fwrite("RIFF", 1, 4, f);
		u32(36 + data);
		std::fwrite("WAVEfmt ", 1, 8, f);
		u32(16);
		u16(1);
		u16(2);
		u32(48000);
		u32(48000 * 4);
		u16(4);
		u16(16);
		std::fwrite("data", 1, 4, f);
		u32(data);
		std::fwrite(st.data(), 2, st.size(), f);
		std::fclose(f);
	}

	// ---- voice helpers (core register block = core * 0x400) ---------------------------------------------------------------
	void setupVoice(int core, int voice, uint32_t ssa, uint16_t pitch, uint16_t volL, uint16_t volR)
	{
		const uint32_t c = core * 0x400u;
		w16(c + voice * 16 + 0x0, volL);
		w16(c + voice * 16 + 0x2, volR);
		w16(c + voice * 16 + 0x4, pitch);
		w16(c + voice * 16 + 0x6, 0x000F); // ADSR1: fastest linear attack, sustain level max
		w16(c + voice * 16 + 0x8, 0x1FC0); // ADSR2: sustain held, slow release
		w32addr(c + 0x1C0 + voice * 12, ssa);
	}

	void keyOn(int core, int voice)
	{
		const uint32_t c = core * 0x400u;
		if (voice < 16)
			w16(c + 0x1A0, static_cast<uint16_t>(1u << voice));
		else
			w16(c + 0x1A2, static_cast<uint16_t>(1u << (voice - 16)));
	}

	bool waitDma(int core, uint32_t expected, uint64_t limitCycles)
	{
		const uint64_t end = g_cycle + limitCycles;
		while (g_dmaDone[core] < expected && g_cycle < end)
		{
			const uint64_t next = kzspu2NextEventCycle();
			const uint64_t step = next != UINT64_MAX && next > g_cycle ? std::min<uint64_t>(next - g_cycle, 4096) : 256;
			advance(step, step);
		}
		return g_dmaDone[core] >= expected;
	}

	// Plain DMA write of `bytes` to SPU2 address `tsa` (halfwords), like sceSdVoiceTrans(SD_TRANS_MODE_WRITE|DMA).
	uint64_t dmaUpload(int core, uint32_t tsa, std::vector<uint16_t>& iopBuf, uint32_t madr)
	{
		const uint32_t c = core * 0x400u;
		w16(c + 0x19A, static_cast<uint16_t>((r16(c + 0x19A) & ~0x30) | 0x20)); // ATTR: DMA write mode
		w32addr(c + 0x1A8, tsa);
		const uint64_t start = g_cycle;
		const uint32_t before = g_dmaDone[core];
		kzspu2DmaWrite(core, iopBuf.data(), static_cast<uint32_t>(iopBuf.size()), madr, g_cycle);
		check(kzspu2DmaBusy(core), core ? "DMA ch7 busy after start" : "DMA ch4 busy after start");
		const bool done = waitDma(core, before + 1, 48000ull * kCyclesPerSample);
		check(done, core ? "DMA ch7 (core 1) write completion interrupt" : "DMA ch4 (core 0) write completion interrupt");
		check(!kzspu2DmaBusy(core), "DMA channel idle after completion");
		check(kzspu2DmaMadr(core) == madr + iopBuf.size() * 2, "MADR = TADR (end of transfer) after completion");
		w16(c + 0x19A, static_cast<uint16_t>(r16(c + 0x19A) & ~0x30));
		return g_dmaDoneCycle[core] - start;
	}
} // namespace

int main(int argc, char** argv)
{
	const std::filesystem::path outDir = std::filesystem::path(argc > 0 ? argv[0] : ".").parent_path() / "out";

	// ======================================================================================================================
	std::printf("Voice test: ADPCM sample via DMA, played on both cores\n");
	check(init(), "kzspu2Init");

	// 1 kHz sine, 336 samples = 12 ADPCM blocks = 7 periods, amplitude 0.6
	std::vector<int16_t> sine(336);
	for (size_t i = 0; i < sine.size(); ++i)
		sine[i] = static_cast<int16_t>(std::lround(0.6 * 32767 * std::sin(2 * kPi * 1000.0 * i / 48000.0)));
	const std::vector<uint8_t> adpcm = encodeAdpcm(sine);
	check(adpcm.size() == 12 * 16, "ADPCM encoder: 12 blocks");

	std::vector<uint16_t> iopBuf(adpcm.size() / 2);
	std::memcpy(iopBuf.data(), adpcm.data(), adpcm.size());

	// What sceSdInit does for the mix: MMIX routes core 0's voices/input to its output and core 0's output into
	// core 1's external input (PCSX2 keeps the gates closed until MMIX is written), master volumes up.
	w16(0x198, 0xFF0);  // core 0 MMIX
	w16(0x598, 0xFFC);  // core 1 MMIX
	w16(0x760, 0x3FFF); // core 0 MVOLL
	w16(0x762, 0x3FFF); // core 0 MVOLR
	w16(0x788, 0x3FFF); // core 1 MVOLL
	w16(0x78A, 0x3FFF); // core 1 MVOLR

	const uint64_t dma0 = dmaUpload(0, 0x5000, iopBuf, 0x00100000);
	const uint64_t dma1 = dmaUpload(1, 0x6000, iopBuf, 0x00108000);
	std::printf("  DMA duration: ch4 %llu, ch7 %llu IOP cycles for %zu halfwords (%.1f cycles/halfword)\n",
		(unsigned long long)dma0, (unsigned long long)dma1, iopBuf.size(), double(dma0) / iopBuf.size());
	check(dma0 >= iopBuf.size() * 12 && dma0 <= iopBuf.size() * 48 + 2000, "DMA takes ~24 IOP cycles per halfword");

	const uint16_t* mem = kzspu2Memory();
	check(std::memcmp(mem + 0x5000, adpcm.data(), adpcm.size()) == 0, "core 0 DMA landed at 0x5000");
	check(std::memcmp(mem + 0x6000, adpcm.data(), adpcm.size()) == 0, "core 1 DMA landed at 0x6000");

	// DMA read-back (SPU2 -> IOP), both cores
	for (int core = 0; core < 2; ++core)
	{
		const uint32_t c = core * 0x400u;
		std::vector<uint16_t> back(iopBuf.size(), 0xCDCD);
		w16(c + 0x19A, static_cast<uint16_t>((r16(c + 0x19A) & ~0x30) | 0x30)); // ATTR: DMA read mode
		w32addr(c + 0x1A8, core ? 0x6000 : 0x5000);
		const uint32_t before = g_dmaDone[core];
		kzspu2DmaRead(core, back.data(), static_cast<uint32_t>(back.size()), 0x00110000, g_cycle);
		check(waitDma(core, before + 1, 48000ull * kCyclesPerSample), "DMA read completion interrupt");
		check(back == iopBuf, core ? "core 1 DMA read-back matches" : "core 0 DMA read-back matches");
		w16(c + 0x19A, static_cast<uint16_t>(r16(c + 0x19A) & ~0x30));
	}

	// IRQ on core 0 at block 5 of the sample
	w32addr(0x19C, 0x5000 + 5 * 8);
	w16(0x19A, static_cast<uint16_t>(r16(0x19A) | 0x40)); // ATTR.IRQE

	setupVoice(0, 0, 0x5000, 0x1000, 0x3FFF, 0x0000); // left only, native rate
	setupVoice(1, 5, 0x6000, 0x0800, 0x0000, 0x3FFF); // right only, half rate
	const size_t playStart = g_capture.size() / 2;
	keyOn(0, 0);
	keyOn(1, 5);
	advance(48000ull * kCyclesPerSample / 10);
	const KzSpu2Status st = kzspu2GetStatus();
	check(st.keyedOnVoices[0] == 1 && st.keyedOnVoices[1] == 1, "one voice keyed on per core");
	check(g_irqs >= 1, "SPU2 IRQ fired when the voice read IRQA");
	const uint32_t irqsBefore = g_irqs;
	advance(48000ull * kCyclesPerSample / 20);
	check(g_irqs == irqsBefore, "IRQ latched until acknowledged (no repeat)");
	w16(0x19A, static_cast<uint16_t>(r16(0x19A) & ~0x40)); // libsd acknowledge: IRQE off, on
	w16(0x19A, static_cast<uint16_t>(r16(0x19A) | 0x40));
	advance(48000ull * kCyclesPerSample / 20);
	check(g_irqs > irqsBefore, "IRQ fires again after the ATTR.IRQE toggle");
	advance(48000ull * kCyclesPerSample * 8 / 10);

	const size_t from = playStart + 4800, to = playStart + 48000;
	const ChannelStats L = analyze(g_capture, 0, from, to), R = analyze(g_capture, 1, from, to);
	const double lShare = toneShare(g_capture, 0, from, to, 1000.0), rShare = toneShare(g_capture, 1, from, to, 500.0);
	std::printf("  left : rms %.0f peak %d zero-crossing freq %.2f Hz, 1000 Hz share %.3f\n", L.rms, L.peak, L.zcFreq, lShare);
	std::printf("  right: rms %.0f peak %d zero-crossing freq %.2f Hz,  500 Hz share %.3f\n", R.rms, R.peak, R.zcFreq, rShare);
	check(L.rms > 3000 && R.rms > 3000, "both channels clearly non-silent");
	check(std::abs(L.zcFreq - 1000.0) < 10.0, "left = 1000 Hz (core 0 voice 0, pitch 0x1000)");
	check(std::abs(R.zcFreq - 500.0) < 5.0, "right = 500 Hz (core 1 voice 5, pitch 0x0800)");
	check(lShare > 0.9 && rShare > 0.9, "tones are clean (>90% of the power at the expected frequency)");
	writeWav(outDir / "voices_1000L_500R.wav", g_capture);

	// key off: the release must bring the output down
	w16(0x1A4, 1);
	w16(0x400 + 0x1A4, 1 << 5);
	advance(48000ull * kCyclesPerSample / 2);
	const size_t n = g_capture.size() / 2;
	const ChannelStats tail = analyze(g_capture, 0, n - 4800, n);
	std::printf("  after key off: left rms %.0f\n", tail.rms);
	check(tail.rms < L.rms / 4, "key off releases the voice");

	// ======================================================================================================================
	std::printf("ADMA test: PCM streamed through core 0 input (sceSdBlockTrans)\n");
	check(init(), "kzspu2Init");
	w16(0x198, 0xFF0);
	w16(0x598, 0xFFC);
	w16(0x760, 0x3FFF);
	w16(0x762, 0x3FFF);
	w16(0x788, 0x3FFF);
	w16(0x78A, 0x3FFF);

	constexpr uint32_t kPairs = 32; // 32 x (256 L + 256 R) = 8192 frames = 170.7 ms
	std::vector<uint16_t> pcm(kPairs * 512);
	for (uint32_t p = 0; p < kPairs; ++p)
		for (uint32_t i = 0; i < 256; ++i)
		{
			const double t = (p * 256 + i) / 48000.0;
			pcm[p * 512 + i] = static_cast<uint16_t>(static_cast<int16_t>(std::lround(0.5 * 32767 * std::sin(2 * kPi * 3000.0 * t))));
			pcm[p * 512 + 256 + i] = static_cast<uint16_t>(static_cast<int16_t>(std::lround(0.5 * 32767 * std::sin(2 * kPi * 750.0 * t))));
		}
	w16(0x1B0, 1);                                            // core 0 ADMAS
	w16(0x19A, static_cast<uint16_t>((r16(0x19A) & ~0x30) | 0x20)); // DMA write mode
	w32addr(0x1A8, 0x2000);
	const size_t admaStart = g_capture.size() / 2;
	const uint64_t admaCycle = g_cycle;
	kzspu2DmaWrite(0, pcm.data(), static_cast<uint32_t>(pcm.size()), 0x00120000, g_cycle);
	advance(48000ull * kCyclesPerSample / 2, kCyclesPerSample * 16);
	check(g_dmaDone[0] == 1, "ADMA completion interrupt");
	const double admaMs = (g_dmaDoneCycle[0] - admaCycle) / 36864.0;
	std::printf("  ADMA completion after %.1f ms of IOP time (data = %.1f ms)\n", admaMs, 8192 / 48.0);
	check(admaMs > 8192 / 48.0 * 0.8 && admaMs < 8192 / 48.0 * 1.3, "ADMA completes when its data has played");
	const size_t af = admaStart + 1200, at = admaStart + 8192;
	const ChannelStats AL = analyze(g_capture, 0, af, at), AR = analyze(g_capture, 1, af, at);
	std::printf("  left : rms %.0f zero-crossing freq %.1f Hz; right: rms %.0f freq %.1f Hz\n", AL.rms, AL.zcFreq, AR.rms, AR.zcFreq);
	check(AL.rms > 3000 && AR.rms > 3000, "ADMA output non-silent");
	check(std::abs(AL.zcFreq - 3000.0) < 45.0 && std::abs(AR.zcFreq - 750.0) < 12.0, "ADMA tones 3000 Hz L / 750 Hz R");
	writeWav(outDir / "adma_3000L_750R.wav", g_capture);

	kzspu2Shutdown();
	std::printf("%s (%d failure%s)\n", g_failures ? "FAIL" : "PASS", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
