#include "DistanceModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace DistanceModel
{
	void Params::InheritFrom(const Params& o)
	{
		falloff.InheritFrom(o.falloff);
		rolloff.InheritFrom(o.rolloff);
		reference.InheritFrom(o.reference);
		fade.InheritFrom(o.fade);
		scatter.InheritFrom(o.scatter);
		diffuse.InheritFrom(o.diffuse);
		delay.InheritFrom(o.delay);
		delayMax.InheritFrom(o.delayMax);
		echo.InheritFrom(o.echo);
		echoDelay.InheritFrom(o.echoDelay);
		echoVolume.InheritFrom(o.echoVolume);
		echoFrom.InheritFrom(o.echoFrom);
	}

	bool Params::Any() const
	{
		return falloff.set || rolloff.set || reference.set || fade.set
			|| scatter.set || diffuse.set || delay.set || delayMax.set
			|| echo.set || echoDelay.set || echoVolume.set || echoFrom.set;
	}

	Resolved Resolve(const Params& p)
	{
		Resolved r;
		if (p.falloff.set) r.falloff = p.falloff.value;
		if (p.rolloff.set) r.rolloff = std::max(0.0f, p.rolloff.value);
		if (p.reference.set) r.reference = std::max(0.01f, p.reference.value);
		if (p.fade.set) r.fade = std::clamp(p.fade.value, 0.0f, 1.0f);
		if (p.scatter.set) r.scatter = std::max(0.0f, p.scatter.value);
		if (p.diffuse.set) r.diffuse = std::clamp(p.diffuse.value, 0.0f, 1.0f);
		if (p.delay.set) r.delay = std::max(0.0f, p.delay.value);
		if (p.delayMax.set) r.delayMax = std::max(0, p.delayMax.value);
		if (p.echo.set) r.echo = std::clamp(p.echo.value, 0, 8);
		if (p.echoDelay.set) r.echoDelay = std::max(1, p.echoDelay.value);
		if (p.echoVolume.set) r.echoVolume = std::clamp(p.echoVolume.value, 0.0f, 1.0f);
		if (p.echoFrom.set) r.echoFrom = std::max(0.0f, p.echoFrom.value);
		return r;
	}

	float RangeFraction(float dist, float range)
	{
		if (range <= 0.0f)
			return dist > 0.0f ? 1.0f : 0.0f;
		return std::clamp(dist / range, 0.0f, 1.0f);
	}

	float Volume(const Resolved& r, float engineVol, float dist, float range)
	{
		if (r.falloff == Falloff::Linear)
			return engineVol;

		if (range <= 0.0f || dist >= range)
			return 0.0f;

		// 6.0206 dB = 20*log10(2): one doubling of distance under the inverse
		// square law. Distance is counted from a listener `reference` cells
		// before the screen edge, so an on-screen source (0) plays at full level.
		const float d = std::max(0.0f, dist);
		float gain = std::pow(r.reference / (r.reference + d), r.rolloff / 6.0206f);

		// Smoothstep to silence over the last `fade` of Range, so a source
		// crossing Range (a moving loop) never cuts off abruptly.
		if (r.fade > 0.0f)
		{
			const float start = range * (1.0f - r.fade);
			if (d > start)
			{
				const float x = std::clamp((range - d) / (range - start), 0.0f, 1.0f);
				gain *= x * x * (3.0f - 2.0f * x);
			}
		}

		return std::clamp(gain, 0.0f, 1.0f);
	}

	float ScatterGain(const Resolved& r, float dist, float range, float unit)
	{
		if (r.scatter <= 0.0f)
			return 1.0f;
		const float dB = std::clamp(unit, -1.0f, 1.0f) * r.scatter * RangeFraction(dist, range);
		return std::pow(10.0f, dB / 20.0f);
	}

	int Pan(const Resolved& r, int pan, float dist, float range)
	{
		if (r.diffuse <= 0.0f)
			return pan;
		const float keep = 1.0f - r.diffuse * RangeFraction(dist, range);
		const float p = PanCenter + (pan - PanCenter) * keep;
		return std::clamp(static_cast<int>(std::lround(p)), 0, PanMax);
	}

	int DelayFrames(const Resolved& r, float dist)
	{
		if (r.delay <= 0.0f || dist <= 0.0f)
			return 0;
		const long frames = std::lround(dist * r.delay);
		return static_cast<int>(std::clamp<long>(frames, 0, r.delayMax));
	}

	int PickBand(const std::vector<float>& from, float dist)
	{
		int band = -1;
		for (int i = 0; i < static_cast<int>(from.size()); ++i)
		{
			if (dist > from[i])
				band = i;
			else
				break;
		}
		return band;
	}

	namespace
	{
		bool EqualsNoCase(const char* a, const char* b)
		{
			for (; *a && *b; ++a, ++b)
			{
				if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
					return false;
			}
			return *a == *b;
		}
	}

	bool ParseFalloff(const char* s, Falloff& out)
	{
		while (*s == ' ' || *s == '\t')
			++s;
		std::string v(s);
		while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
			v.pop_back();

		if (EqualsNoCase(v.c_str(), "linear") || EqualsNoCase(v.c_str(), "vanilla"))
		{
			out = Falloff::Linear;
			return true;
		}
		if (EqualsNoCase(v.c_str(), "inverse") || EqualsNoCase(v.c_str(), "physical"))
		{
			out = Falloff::Inverse;
			return true;
		}
		return false;
	}

	std::vector<std::string> SplitList(const char* s)
	{
		std::vector<std::string> out;
		std::string cur;
		for (const char* p = s; ; ++p)
		{
			const char c = *p;
			if (c == ',' || c == ' ' || c == '\t' || c == '\0')
			{
				if (!cur.empty())
					out.push_back(cur);
				cur.clear();
				if (c == '\0')
					break;
			}
			else
			{
				cur.push_back(c);
			}
		}
		return out;
	}

	bool ParseFloat(const std::string& s, float& out)
	{
		if (s.empty())
			return false;
		char* end = nullptr;
		const double v = std::strtod(s.c_str(), &end);
		if (end == s.c_str() || *end != '\0' || !std::isfinite(v))
			return false;
		out = static_cast<float>(v);
		return true;
	}
}
