// Host-compiled unit tests for src/Sound/DistanceModel.cpp (no game needed).
//   tests/run_tests.sh   -> builds with g++ and runs; exit 0 = all passed.

#include "../src/Sound/DistanceModel.h"

#include <cmath>
#include <cstdio>

using namespace DistanceModel;

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char* what, int line)
{
	++checks;
	if (!ok)
	{
		++failures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

#define CHECK(x) Check((x), #x, __LINE__)

static Resolved Inverse()
{
	Params p;
	p.falloff.Set(Falloff::Inverse);
	return Resolve(p);
}

int main()
{
	// --- defaults: an empty Params is vanilla -------------------------------
	{
		const Resolved r = Resolve(Params {});
		CHECK(r.falloff == Falloff::Linear);
		CHECK(!r.ChangesVolume() && !r.ChangesPan() && !r.Defers());
		CHECK(Near(Volume(r, 0.37f, 5.0f, 10.0f), 0.37f));
		CHECK(Pan(r, 1234, 9.0f, 10.0f) == 1234);
		CHECK(DelayFrames(r, 50.0f) == 0);
	}

	// --- inverse-square spreading -------------------------------------------
	{
		const Resolved r = Inverse(); // ref 8, 6.02 dB/doubling, fade 0.25
		CHECK(Near(Volume(r, 0.0f, 0.0f, 100.0f), 1.0f));      // on screen: full
		CHECK(Near(Volume(r, 0.0f, 8.0f, 100.0f), 0.5f));      // 2x distance: -6 dB
		CHECK(Near(Volume(r, 0.0f, 24.0f, 100.0f), 0.25f));    // 4x: -12 dB
		CHECK(Near(Volume(r, 0.0f, 56.0f, 100.0f), 0.125f));   // 8x: -18 dB
		CHECK(Volume(r, 0.9f, 100.0f, 100.0f) == 0.0f);         // Range is still the limit
		CHECK(Volume(r, 0.9f, 150.0f, 100.0f) == 0.0f);
		CHECK(Volume(r, 0.9f, 0.0f, 0.0f) == 0.0f);             // Range 0: inaudible like vanilla
	}

	// --- fade over the last part of Range -----------------------------------
	{
		const Resolved r = Inverse();
		const float atStart = Volume(r, 0, 75.0f, 100.0f);      // fade starts at 75
		const float spread75 = std::pow(8.0f / 83.0f, 1.0f);
		CHECK(Near(atStart, spread75));
		const float mid = Volume(r, 0, 87.5f, 100.0f);          // x = 0.5 -> smoothstep 0.5
		CHECK(Near(mid, 0.5f * (8.0f / 95.5f)));
		CHECK(Volume(r, 0, 99.99f, 100.0f) < 1e-4f);

		Params p;
		p.falloff.Set(Falloff::Inverse);
		p.fade.Set(0.0f);
		const Resolved hard = Resolve(p);
		CHECK(Near(Volume(hard, 0, 99.0f, 100.0f), 8.0f / 107.0f)); // no taper
	}

	// --- rolloff / reference --------------------------------------------------
	{
		Params p;
		p.falloff.Set(Falloff::Inverse);
		p.rolloff.Set(3.0103f); // 3 dB per doubling = sqrt law (line source)
		p.reference.Set(4.0f);
		p.fade.Set(0.0f);
		const Resolved r = Resolve(p);
		CHECK(Near(Volume(r, 0, 4.0f, 1000.0f), std::sqrt(0.5f)));
		CHECK(Near(Volume(r, 0, 12.0f, 1000.0f), 0.5f));
	}

	// --- scatter ---------------------------------------------------------------
	{
		Params p;
		p.scatter.Set(6.0206f);
		const Resolved r = Resolve(p);
		CHECK(r.ChangesVolume());
		CHECK(Near(ScatterGain(r, 0.0f, 20.0f, 1.0f), 1.0f));   // on screen: never
		CHECK(Near(ScatterGain(r, 20.0f, 20.0f, 1.0f), 2.0f));  // +6 dB at Range
		CHECK(Near(ScatterGain(r, 20.0f, 20.0f, -1.0f), 0.5f));
		CHECK(Near(ScatterGain(r, 10.0f, 20.0f, 1.0f), std::sqrt(2.0f))); // half range: +3 dB
		CHECK(Near(ScatterGain(r, 10.0f, 20.0f, 0.0f), 1.0f));
		CHECK(Near(ScatterGain(r, 5.0f, 0.0f, 1.0f), 2.0f));    // no Range: off screen = full
	}

	// --- diffuse pan -------------------------------------------------------------
	{
		Params p;
		p.diffuse.Set(1.0f);
		const Resolved r = Resolve(p);
		CHECK(Pan(r, 0x4000, 0.0f, 10.0f) == 0x4000);           // on screen untouched
		CHECK(Pan(r, 0x4000, 10.0f, 10.0f) == PanCenter);       // full range: centred
		CHECK(Pan(r, 0x0000, 5.0f, 10.0f) == 0x1000);           // halfway
		p.diffuse.Set(0.5f);
		const Resolved half = Resolve(p);
		CHECK(Pan(half, 0x4000, 10.0f, 10.0f) == 0x3000);
		p.diffuse.Set(7.0f);                                     // clamped to 1
		CHECK(Resolve(p).diffuse == 1.0f);
	}

	// --- travel delay -----------------------------------------------------------
	{
		Params p;
		p.delay.Set(0.5f);
		const Resolved r = Resolve(p);
		CHECK(r.Defers());
		CHECK(DelayFrames(r, 0.0f) == 0);
		CHECK(DelayFrames(r, 20.0f) == 10);
		CHECK(DelayFrames(r, 500.0f) == 45);                     // default cap
		p.delayMax.Set(12);
		CHECK(DelayFrames(Resolve(p), 500.0f) == 12);
		p.delay.Set(-3.0f);                                      // negative = off
		CHECK(DelayFrames(Resolve(p), 20.0f) == 0);
	}

	// --- echo resolve clamps ----------------------------------------------------
	{
		Params p;
		p.echo.Set(99);
		p.echoDelay.Set(0);
		p.echoVolume.Set(1.5f);
		const Resolved r = Resolve(p);
		CHECK(r.echo == 8 && r.echoDelay == 1 && r.echoVolume == 1.0f && r.Defers());
	}

	// --- band picking -----------------------------------------------------------
	{
		const std::vector<float> from { 3.0f, 15.0f };
		CHECK(PickBand(from, 0.0f) == -1);
		CHECK(PickBand(from, 3.0f) == -1);   // strictly beyond
		CHECK(PickBand(from, 3.5f) == 0);
		CHECK(PickBand(from, 15.0f) == 0);
		CHECK(PickBand(from, 40.0f) == 1);
		const std::vector<float> offscreen { 0.0f };
		CHECK(PickBand(offscreen, 0.0f) == -1); // on screen never banded
		CHECK(PickBand(offscreen, 0.01f) == 0);
		CHECK(PickBand({}, 99.0f) == -1);
	}

	// --- inheritance precedence -------------------------------------------------
	{
		Params own, parent, global;
		own.rolloff.Set(3.0f);
		parent.rolloff.Set(9.0f);
		parent.falloff.Set(Falloff::Inverse);
		global.falloff.Set(Falloff::Linear);
		global.delay.Set(0.25f);
		Params p = own;
		p.InheritFrom(parent);
		p.InheritFrom(global);
		const Resolved r = Resolve(p);
		CHECK(Near(r.rolloff, 3.0f));               // own wins
		CHECK(r.falloff == Falloff::Inverse);       // parent beats global
		CHECK(Near(r.delay, 0.25f));                // global fills the rest
		CHECK(!own.falloff.set && own.Any() && !Params {}.Any());
	}

	// --- the engine's distance metric (0x750AC0) ----------------------------------
	{
		// 1024 x 768 view: centre (512, 384), half sizes 512 / 384.
		const float W = 1024.0f, H = 768.0f;
		CHECK(Near(ViewDistance(512, 384, W, H, false), 0.0f));          // centre
		CHECK(Near(ViewDistance(1000, 700, W, H, false), 0.0f));         // anywhere on screen: 0
		CHECK(Near(ViewDistance(1024 + 120, 384, W, H, false), 2.0f));   // 120 px right of the edge
		CHECK(Near(ViewDistance(-120, 384, W, H, false), 2.0f));         // ... and left, symmetric
		CHECK(Near(ViewDistance(512, 768 + 30, W, H, false), 1.0f));     // vertical doubled
		CHECK(Near(ViewDistance(1024 + 60, 768 + 45, W, H, false), 1.5f)); // Chebyshev: the larger

		// Type=LOCAL: from the centre, nothing subtracted, on screen included.
		CHECK(Near(ViewDistance(512, 384, W, H, true), 0.0f));
		CHECK(Near(ViewDistance(1024, 384, W, H, true), 512.0f / 60.0f)); // the edge is ~8.5 cells
		CHECK(Near(ViewDistance(1024 + 120, 384, W, H, true), 632.0f / 60.0f));
		CHECK(Near(ViewDistance(512, 384 + 90, W, H, true), 3.0f));       // vertical doubled too
		CHECK(Near(ViewDistance(512, 384 - 90, W, H, true), 3.0f));
		// A LOCAL source is always at least as far as a normal one at the same point.
		CHECK(ViewDistance(1100, 50, W, H, true) >= ViewDistance(1100, 50, W, H, false));
		// So bands are picked differently on screen: From=4 is crossed at 240 px from the centre.
		CHECK(PickBand({ 4.0f }, ViewDistance(512 + 300, 384, W, H, true)) == 0);
		CHECK(PickBand({ 4.0f }, ViewDistance(512 + 300, 384, W, H, false)) == -1);
	}

	// --- parsing ------------------------------------------------------------------
	{
		Falloff f = Falloff::Linear;
		CHECK(ParseFalloff("Inverse", f) && f == Falloff::Inverse);
		CHECK(ParseFalloff("  linear ", f) && f == Falloff::Linear);
		CHECK(ParseFalloff("PHYSICAL", f) && f == Falloff::Inverse);
		CHECK(!ParseFalloff("cubic", f));

		auto const l = SplitList(" BoomNear, BoomFar  BoomVeryFar,, ");
		CHECK(l.size() == 3 && l[0] == "BoomNear" && l[1] == "BoomFar" && l[2] == "BoomVeryFar");
		CHECK(SplitList("").empty());

		float v = 0;
		CHECK(ParseFloat("2.5", v) && Near(v, 2.5f));
		CHECK(ParseFloat("-4", v) && Near(v, -4.0f));
		CHECK(!ParseFloat("2.5x", v));
		CHECK(!ParseFloat("", v));
		CHECK(!ParseFloat("nan", v));
	}

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
