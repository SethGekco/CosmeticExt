#pragma once

// The distance-acoustics model behind the soundmd.ini Distance.* tags.
//
// Pure C++: no YRpp, no Windows headers. tests/test_distance_model.cpp builds
// this file with a host compiler, so every formula here is unit-tested without
// the game. DistanceSound.cpp is the only place that talks to the engine.
//
// Units. "Distance" is always the engine's own sound metric (VocClass volume
// function 0x750AC0): Chebyshev distance in screen pixels beyond the tactical
// view's edge, vertical component doubled (isometric), divided by the 60 px
// width of a cell. So 0 = on screen, and Range= in soundmd.ini is measured in
// the same unit. Time is game frames, so delays scale with game speed exactly
// like projectiles and animations do.

#include <string>
#include <vector>

namespace DistanceModel
{
	constexpr float PixelsPerCell = 60.0f;
	constexpr int PanCenter = 0x2000;   // engine pan: 0 = left, 0x4000 = right
	constexpr int PanMax = 0x4000;

	enum class Falloff : int
	{
		Linear = 0,  // vanilla: (Range - d) / Range
		Inverse = 1, // spherical spreading: -Rolloff dB per doubling of distance
	};

	// A value that remembers whether an INI key supplied it, so inheritance can
	// tell "absent" from "explicitly set to the default".
	template <typename T>
	struct Opt
	{
		T value {};
		bool set = false;

		void Set(T v) { value = v; set = true; }
		void InheritFrom(const Opt& o) { if (!set && o.set) *this = o; }
	};

	// The inheritable part of a sound's distance model. Precedence when
	// resolving: the sound's own keys > the keys of the sound that lists it in
	// Distance.Bands > [CosmeticExt] defaults > vanilla behaviour.
	struct Params
	{
		Opt<Falloff> falloff;
		Opt<float> rolloff;    // dB lost per doubling of distance (Inverse)
		Opt<float> reference;  // cells: listener's distance to the screen edge
		Opt<float> fade;       // fraction of Range over which Inverse fades to 0
		Opt<float> scatter;    // +- dB of random level change at full Range
		Opt<float> diffuse;    // 0..1: how far stereo collapses to centre at Range
		Opt<float> delay;      // frames per cell (sound travel time)
		Opt<int> delayMax;     // frames
		Opt<int> echo;         // number of repeats
		Opt<int> echoDelay;    // frames between repeats
		Opt<float> echoVolume; // level of each repeat relative to the previous
		Opt<float> echoFrom;   // cells: echoes only beyond this distance

		void InheritFrom(const Params& o);
		bool Any() const;
	};

	// Concrete values after inheritance. The defaults are the ones documented
	// in docs/soundmd-tags.md -- change both together.
	struct Resolved
	{
		Falloff falloff = Falloff::Linear;
		float rolloff = 6.0206f; // inverse-square law
		float reference = 8.0f;
		float fade = 0.25f;
		float scatter = 0.0f;
		float diffuse = 0.0f;
		float delay = 0.0f;
		int delayMax = 45;
		int echo = 0;
		int echoDelay = 8;
		float echoVolume = 0.35f;
		float echoFrom = 0.0f;

		bool ChangesVolume() const { return falloff != Falloff::Linear || scatter > 0.0f; }
		bool ChangesPan() const { return diffuse > 0.0f; }
		bool Defers() const { return delay > 0.0f || echo > 0; }
	};

	Resolved Resolve(const Params& p);

	// Fraction of Range covered, clamped to [0, 1]. A sound with no Range
	// counts as "at full range" as soon as it is off screen.
	float RangeFraction(float dist, float range);

	// Spreading loss. `engineVol` is the vanilla linear volume and is returned
	// unchanged for Falloff::Linear. Range stays the audibility limit either
	// way (vanilla is silent at d >= Range; so is Inverse).
	float Volume(const Resolved& r, float engineVol, float dist, float range);

	// Per-play atmospheric level variation. `unit` is a uniform draw in
	// [-1, 1]; the swing grows linearly from 0 on screen to Scatter dB at Range.
	float ScatterGain(const Resolved& r, float dist, float range, float unit);

	// Stereo diffusion: pull the pan toward centre in proportion to distance.
	int Pan(const Resolved& r, int pan, float dist, float range);

	// Sound travel time in frames, capped at DelayMax.
	int DelayFrames(const Resolved& r, float dist);

	// Distance.Bands selection. `from` is ascending; a band applies once the
	// source is MORE than from[i] cells beyond the edge, so From=0 means "any
	// off-screen source" and an on-screen source (0) never changes band.
	// Returns the band index, or -1 for "play the sound itself".
	int PickBand(const std::vector<float>& from, float dist);

	// --- INI value parsing (exposed for the tests) -------------------------

	bool ParseFalloff(const char* s, Falloff& out);
	// Comma- or whitespace-separated list, items trimmed, empties dropped.
	std::vector<std::string> SplitList(const char* s);
	// Strict float parse of a whole token (trailing junk = failure).
	bool ParseFloat(const std::string& s, float& out);
}
