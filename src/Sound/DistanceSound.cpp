#include "DistanceSound.h"
#include "DistanceModel.h"

#include <CCINIClass.h>
#include <VocClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <Fundamentals.h>
#include <GeneralDefinitions.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DistanceModel;

namespace
{
	const char* const CFG_SECTION = "CosmeticExt";

	// VocClass::Array (0xB1D378) holds HANDLES, not the sound data YRpp's
	// VocClass struct describes: each item is a 4-byte object whose only field
	// points at the data (see the handle constructor 0x750380 and LoadAll's
	// `mov ecx,[Items+i*4]; mov ecx,[ecx]` at 0x75130F). YRpp's VocClass::Find
	// reads Name through the handle and is wrong; never use it.
	VocClass* VocData(int index)
	{
		if (index < 0 || index >= VocClass::Array.Count)
			return nullptr;
		auto const pHandle = VocClass::Array.Items[index];
		return pHandle ? *reinterpret_cast<VocClass**>(pHandle) : nullptr;
	}

	struct SoundCfg
	{
		Params own;              // keys from the sound's own section
		Resolved model;          // after inheritance
		std::vector<int> bands;  // Distance.Bands, resolved to sound indexes
		std::vector<float> from; // Distance.Bands.From, ascending
		int echoSound = -1;      // Distance.Echo.Sound
		int parent = -1;         // the sound whose Distance.Bands lists this one
	};

	struct Pending
	{
		int index;
		CoordStruct coords;
		int frame;
		float gain;
	};

	struct GlobalCfg
	{
		bool enabled = true;
		int logBudget = 30; // runtime event lines per game session
		int queueMax = 128;
		Params defaults;    // Distance.* keys in [CosmeticExt]
	};

	GlobalCfg Global;
	std::vector<SoundCfg> Sounds;                     // by VocClass index
	std::unordered_map<const VocClass*, int> ByData; // data pointer -> index
	std::vector<Pending> Queue;
	bool Active = false;       // anything to do at all
	bool Dispatching = false;  // inside our own PlayAt call from the queue
	float DispatchGain = 1.0f; // echo level for the call being dispatched
	int LastFrame = -1;
	int LogLeft = 0;
	bool QueueFullLogged = false;
	unsigned int RngState = 0x2545F491u;

	// Unsynced on purpose: this only shapes what this client hears.
	float RandUnit()
	{
		RngState ^= RngState << 13;
		RngState ^= RngState >> 17;
		RngState ^= RngState << 5;
		return static_cast<float>(RngState) / 2147483648.0f - 1.0f; // [-1, 1)
	}

	// A draw that stays the same for one (controller, sound) pair. A sound
	// held by an AudioController is re-levelled every frame by 0x750D40, so
	// its scatter must be a property of the pair, not of the call.
	float StableUnit(const void* pCtrl, const void* pData)
	{
		unsigned int h = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pCtrl)) * 2654435761u;
		h ^= static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pData)) * 2246822519u;
		h ^= h >> 15;
		h *= 2246822519u;
		h ^= h >> 13;
		h *= 3266489917u;
		h ^= h >> 16;
		return static_cast<float>(h) / 2147483648.0f - 1.0f;
	}

	const char* NameOf(int index)
	{
		auto const pData = VocData(index);
		return pData ? pData->Name : "?";
	}

	const SoundCfg* FindCfg(const VocClass* pData)
	{
		auto const it = ByData.find(pData);
		return it == ByData.end() ? nullptr : &Sounds[it->second];
	}

	bool LogRuntime()
	{
		if (LogLeft <= 0)
			return false;
		--LogLeft;
		return true;
	}

	// ---- INI reading -------------------------------------------------------

	bool ReadRaw(CCINIClass* pINI, const char* section, const char* key, std::string& out)
	{
		char buffer[512];
		if (pINI->ReadString(section, key, "", buffer, sizeof(buffer)) <= 0)
			return false;
		out = buffer;
		while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
			out.pop_back();
		size_t lead = 0;
		while (lead < out.size() && (out[lead] == ' ' || out[lead] == '\t'))
			++lead;
		out.erase(0, lead);
		return !out.empty();
	}

	void ReadFloat(CCINIClass* pINI, const char* section, const char* key, Opt<float>& dst)
	{
		std::string raw;
		if (!ReadRaw(pINI, section, key, raw))
			return;
		float v = 0.0f;
		if (ParseFloat(raw, v))
			dst.Set(v);
		else
			Debug::Log("[CosmeticExt] [%s] %s=%s is not a number -- ignored.\n", section, key, raw.c_str());
	}

	void ReadInt(CCINIClass* pINI, const char* section, const char* key, Opt<int>& dst)
	{
		Opt<float> f;
		ReadFloat(pINI, section, key, f);
		if (f.set)
			dst.Set(static_cast<int>(std::lround(f.value)));
	}

	void ReadParams(CCINIClass* pINI, const char* section, Params& p)
	{
		std::string raw;
		if (ReadRaw(pINI, section, "Distance.Falloff", raw))
		{
			Falloff f;
			if (ParseFalloff(raw.c_str(), f))
				p.falloff.Set(f);
			else
				Debug::Log("[CosmeticExt] [%s] Distance.Falloff=%s unknown (linear|inverse) -- ignored.\n", section, raw.c_str());
		}
		ReadFloat(pINI, section, "Distance.Rolloff", p.rolloff);
		ReadFloat(pINI, section, "Distance.Reference", p.reference);
		ReadFloat(pINI, section, "Distance.Fade", p.fade);
		ReadFloat(pINI, section, "Distance.Scatter", p.scatter);
		ReadFloat(pINI, section, "Distance.Diffuse", p.diffuse);
		ReadFloat(pINI, section, "Distance.Delay", p.delay);
		ReadInt(pINI, section, "Distance.Delay.Max", p.delayMax);
		ReadInt(pINI, section, "Distance.Echo", p.echo);
		ReadInt(pINI, section, "Distance.Echo.Delay", p.echoDelay);
		ReadFloat(pINI, section, "Distance.Echo.Volume", p.echoVolume);
		ReadFloat(pINI, section, "Distance.Echo.From", p.echoFrom);
	}

	void ReadBands(CCINIClass* pINI, int index, SoundCfg& cfg)
	{
		const char* const section = NameOf(index);
		std::string rawNames;
		if (!ReadRaw(pINI, section, "Distance.Bands", rawNames))
			return;

		std::string rawFrom;
		ReadRaw(pINI, section, "Distance.Bands.From", rawFrom);
		auto const names = SplitList(rawNames.c_str());
		auto const froms = SplitList(rawFrom.c_str());

		if (froms.size() != names.size())
			Debug::Log("[CosmeticExt] [%s] Distance.Bands has %d entries but Distance.Bands.From has %d -- "
				"only bands with a From value are used.\n", section, int(names.size()), int(froms.size()));

		std::vector<std::pair<float, int>> bands;
		for (size_t i = 0; i < names.size() && i < froms.size(); ++i)
		{
			float from = 0.0f;
			if (!ParseFloat(froms[i], from) || from < 0.0f)
			{
				Debug::Log("[CosmeticExt] [%s] Distance.Bands.From value '%s' is not a distance -- band %s dropped.\n",
					section, froms[i].c_str(), names[i].c_str());
				continue;
			}
			const int band = VocClass::FindIndex(names[i].c_str());
			if (band < 0)
			{
				Debug::Log("[CosmeticExt] [%s] Distance.Bands names '%s', which is not in [SoundList] -- band dropped.\n",
					section, names[i].c_str());
				continue;
			}
			if (band == index)
			{
				Debug::Log("[CosmeticExt] [%s] Distance.Bands lists the sound itself -- band dropped.\n", section);
				continue;
			}
			bands.emplace_back(from, band);
		}

		if (!std::is_sorted(bands.begin(), bands.end(),
			[](auto const& a, auto const& b) { return a.first < b.first; }))
		{
			Debug::Log("[CosmeticExt] [%s] Distance.Bands.From is not ascending -- sorted nearest first.\n", section);
			std::stable_sort(bands.begin(), bands.end(),
				[](auto const& a, auto const& b) { return a.first < b.first; });
		}

		for (auto const& [from, band] : bands)
		{
			cfg.from.push_back(from);
			cfg.bands.push_back(band);
		}
	}

	void LogSound(int index, const SoundCfg& cfg)
	{
		auto const& m = cfg.model;
		std::string bands;
		for (size_t i = 0; i < cfg.bands.size(); ++i)
		{
			char item[96];
			_snprintf_s(item, _TRUNCATE, "%s%s>%g", i ? ", " : "", NameOf(cfg.bands[i]), cfg.from[i]);
			bands += item;
		}
		if (!bands.empty())
			bands = " bands=[" + bands + "]";
		if (cfg.parent >= 0)
			bands += std::string(" bandOf=") + NameOf(cfg.parent);
		if (cfg.echoSound >= 0)
			bands += std::string(" echoSound=") + NameOf(cfg.echoSound);

		Debug::Log("[CosmeticExt]   %s: falloff=%s rolloff=%.2fdB ref=%g fade=%g scatter=%gdB diffuse=%g "
			"delay=%g/cell (max %d) echo=%d every %d f x%g from %g%s\n",
			NameOf(index),
			m.falloff == Falloff::Inverse ? "inverse" : "linear", m.rolloff, m.reference, m.fade,
			m.scatter, m.diffuse, m.delay, m.delayMax, m.echo, m.echoDelay, m.echoVolume, m.echoFrom,
			bands.c_str());
	}

	// ---- the engine's distance metric ------------------------------------

	// Mirror of VocClass's volume function 0x750AC0, so band choice and travel
	// delay use exactly the distance the engine attenuates by: pixels beyond
	// the tactical view edge, vertical doubled, Chebyshev, in cells.
	bool EdgeDistance(const CoordStruct& coords, float& dist)
	{
		auto const pTactical = TacticalClass::Instance;
		if (!pTactical)
			return false;

		Point2D client {};
		pTactical->CoordsToClient(&coords, &client);

		const float halfW = DSurface::ViewBounds.Width * 0.5f;
		const float halfH = DSurface::ViewBounds.Height * 0.5f;
		const float dx = std::max(0.0f, std::fabs(client.X - halfW) - halfW);
		const float dy = std::max(0.0f, std::fabs(client.Y - halfH) - halfH);
		dist = std::max(dx, 2.0f * dy) / PixelsPerCell;
		return true;
	}

	bool Enqueue(int index, const CoordStruct& coords, int frame, float gain)
	{
		if (static_cast<int>(Queue.size()) >= Global.queueMax)
		{
			if (!QueueFullLogged)
			{
				QueueFullLogged = true;
				Debug::Log("[CosmeticExt] deferred-sound queue full (%d, [CosmeticExt] QueueMax=) -- "
					"further delays play at once and further echoes are skipped. Logged once.\n", Global.queueMax);
			}
			return false;
		}
		LastFrame = Unsorted::CurrentFrame;
		Queue.push_back({ index, coords, frame, gain });
		return true;
	}

	// PlayAt entry decision. Returns true when the sound was deferred (the
	// caller's PlayAt must then do nothing); otherwise `play` is the index to
	// play now -- the sound itself or one of its Distance.Bands.
	bool OnPlayAt(int index, const CoordStruct& coords, AudioController* pCtrl, int& play)
	{
		play = index;
		if (!Active || Dispatching || index < 0 || index >= static_cast<int>(Sounds.size()))
			return false;

		auto const& cfg = Sounds[index];
		bool defers = cfg.model.Defers();
		if (defers)
		{
			// What may be deferred: anything that is not a loop and whose
			// controller (if any) is idle. A deferred sound plays later as a
			// free one-shot, so the controller is simply never attached --
			// fine for an anim's Report (a fresh controller per anim, the main
			// explosion path, 0x42211A). A loop played free would never stop,
			// and a busy controller means vanilla wants to keep or replace
			// what it is playing, so both play now, untouched.
			auto const pData = VocData(index);
			const bool loops = pData && (pData->Control & SoundControl::Loop) != SoundControl::None;
			defers = !loops && (!pCtrl || !pCtrl->GetEvent());
		}
		if (cfg.bands.empty() && !defers)
			return false;

		float dist = 0.0f;
		if (!EdgeDistance(coords, dist))
			return false;

		const int band = PickBand(cfg.from, dist);
		if (band >= 0)
			play = cfg.bands[band];

		int delay = 0;
		int echoes = 0;
		if (defers)
		{
			const int now = Unsorted::CurrentFrame;
			delay = DelayFrames(cfg.model, dist);
			if (delay > 0 && !Enqueue(play, coords, now + delay, 1.0f))
				delay = 0;

			if (cfg.model.echo > 0 && dist >= cfg.model.echoFrom)
			{
				const int echoIndex = cfg.echoSound >= 0 ? cfg.echoSound : play;
				float gain = 1.0f;
				for (int k = 1; k <= cfg.model.echo; ++k)
				{
					gain *= cfg.model.echoVolume;
					if (!Enqueue(echoIndex, coords, now + delay + k * cfg.model.echoDelay, gain))
						break;
					++echoes;
				}
			}
		}

		if ((band >= 0 || delay > 0 || echoes > 0) && LogRuntime())
		{
			Debug::Log("[CosmeticExt] frame %d: %s at %.1f cells -> %s, delay %d frames, %d echo(es)\n",
				Unsorted::CurrentFrame, NameOf(index), dist, NameOf(play), delay, echoes);
		}

		return delay > 0;
	}

	// Volume seat. `vol` is vanilla's linear volume (0 beyond Range); the
	// distances are the engine's own locals, in pixels. `fromPlayAt` is false
	// for the per-frame controller update; `pCtrl` is the controller either
	// way (null for a free one-shot).
	float AdjustVolume(const VocClass* pData, float vol, float dxo, float dyo2, float rangePx,
		bool fromPlayAt, const AudioController* pCtrl)
	{
		// Only our own queue dispatch is a free PlayAt while Dispatching.
		const float gain = (Dispatching && fromPlayAt && !pCtrl) ? DispatchGain : 1.0f;
		auto const pCfg = FindCfg(pData);
		if (!pCfg || !pCfg->model.ChangesVolume())
			return std::clamp(vol * gain, 0.0f, 1.0f);

		const float dist = std::max(dxo, dyo2) / PixelsPerCell;
		const float range = rangePx / PixelsPerCell;
		float v = Volume(pCfg->model, vol, dist, range);

		if (pCfg->model.scatter > 0.0f && (pCtrl || fromPlayAt))
		{
			const float unit = pCtrl ? StableUnit(pCtrl, pData) : RandUnit();
			v *= ScatterGain(pCfg->model, dist, range, unit);
		}

		return std::clamp(v * gain, 0.0f, 1.0f);
	}

	int AdjustPan(const VocClass* pData, int pan, float dxo, float dyo2, float rangePx)
	{
		auto const pCfg = FindCfg(pData);
		if (!pCfg || !pCfg->model.ChangesPan())
			return pan;
		return Pan(pCfg->model, pan, std::max(dxo, dyo2) / PixelsPerCell, rangePx / PixelsPerCell);
	}
}

void DistanceSound::ReadINI(CCINIClass* pINI)
{
	static bool bannerLogged = false;
	if (!bannerLogged)
	{
		bannerLogged = true;
		Debug::Log("[CosmeticExt] loaded (built " __DATE__ " " __TIME__ ").\n");
	}

	Global = GlobalCfg {};
	Sounds.clear();
	ByData.clear();
	Queue.clear();
	Active = false;
	QueueFullLogged = false;
	RngState ^= GetTickCount() | 1u;

	{
		std::string raw;
		if (ReadRaw(pINI, CFG_SECTION, "Enabled", raw))
			Global.enabled = !(raw[0] == 'n' || raw[0] == 'N' || raw[0] == 'f' || raw[0] == 'F' || raw[0] == '0');
		Opt<int> v;
		ReadInt(pINI, CFG_SECTION, "LogBudget", v);
		if (v.set)
			Global.logBudget = std::max(0, v.value);
		v = {};
		ReadInt(pINI, CFG_SECTION, "QueueMax", v);
		if (v.set)
			Global.queueMax = std::clamp(v.value, 0, 4096);
		ReadParams(pINI, CFG_SECTION, Global.defaults);
	}
	LogLeft = Global.logBudget;

	const int count = VocClass::Array.Count;
	Sounds.resize(static_cast<size_t>(std::max(0, count)));

	for (int i = 0; i < count; ++i)
	{
		auto const pData = VocData(i);
		if (!pData)
			continue;
		ByData[pData] = i;
		ReadParams(pINI, pData->Name, Sounds[i].own);
		ReadBands(pINI, i, Sounds[i]);

		std::string echoName;
		if (ReadRaw(pINI, pData->Name, "Distance.Echo.Sound", echoName))
		{
			Sounds[i].echoSound = VocClass::FindIndex(echoName.c_str());
			if (Sounds[i].echoSound < 0)
				Debug::Log("[CosmeticExt] [%s] Distance.Echo.Sound=%s is not in [SoundList] -- echoes repeat the sound itself.\n",
					pData->Name, echoName.c_str());
		}
	}

	// A band variant inherits the model of the sound that lists it, so a far
	// variant attenuates like its base unless it says otherwise.
	for (int i = 0; i < count; ++i)
	{
		for (const int band : Sounds[i].bands)
		{
			if (Sounds[band].parent < 0)
				Sounds[band].parent = i;
			else if (Sounds[band].parent != i)
				Debug::Log("[CosmeticExt] %s is a band of both %s and %s -- it inherits from %s.\n",
					NameOf(band), NameOf(Sounds[band].parent), NameOf(i), NameOf(Sounds[band].parent));
		}
	}

	const bool globalModel = Global.enabled && Global.defaults.Any();
	int configured = 0;
	for (int i = 0; i < count; ++i)
	{
		auto& cfg = Sounds[i];
		Params p = cfg.own;
		// Walk up the band chain (bounded: a malformed INI could make a loop).
		for (int parent = cfg.parent, depth = 0; parent >= 0 && depth < 8; parent = Sounds[parent].parent, ++depth)
			p.InheritFrom(Sounds[parent].own);
		p.InheritFrom(Global.defaults);
		cfg.model = Resolve(p);

		const bool any = !cfg.bands.empty() || cfg.model.ChangesVolume() || cfg.model.ChangesPan() || cfg.model.Defers();
		if (any)
			++configured;
	}

	Active = Global.enabled && configured > 0;

	Debug::Log("[CosmeticExt] soundmd.ini: %d sound(s), %d with distance behaviour%s%s.\n",
		count, configured,
		globalModel ? " ([CosmeticExt] defaults apply to every sound)" : "",
		Global.enabled ? "" : " -- [CosmeticExt] Enabled=no, all of it OFF");

	// Per-sound detail is for verifying an INI; with global defaults on every
	// sound would print, so list only sounds with their own keys or bands.
	constexpr int DetailLines = 80;
	int detail = 0;
	for (int i = 0; i < count && detail < DetailLines; ++i)
	{
		auto const& cfg = Sounds[i];
		if (cfg.own.Any() || !cfg.bands.empty() || cfg.parent >= 0 || cfg.echoSound >= 0)
		{
			LogSound(i, cfg);
			++detail;
		}
	}
	if (detail == DetailLines)
		Debug::Log("[CosmeticExt]   (sound list truncated at %d)\n", DetailLines);
}

void DistanceSound::FrameTick()
{
	if (Queue.empty())
		return;

	const int now = Unsorted::CurrentFrame;
	if (now < LastFrame)
	{
		// The frame counter went backwards: a new game or a loaded save.
		// Whatever was queued belongs to the old one.
		Queue.clear();
		LastFrame = now;
		return;
	}
	LastFrame = now;

	// Take the due items out first: dispatching can enqueue nothing (we are
	// Dispatching), but keep the loop independent of that anyway.
	std::vector<Pending> due;
	auto const split = std::stable_partition(Queue.begin(), Queue.end(),
		[now](Pending const& p) { return p.frame > now; });
	due.assign(split, Queue.end());
	Queue.erase(split, Queue.end());

	for (auto const& item : due)
	{
		Dispatching = true;
		DispatchGain = item.gain;
		VocClass::PlayAt(item.index, item.coords, nullptr);
		Dispatching = false;
		DispatchGain = 1.0f;
	}
}

// ---------------------------------------------------------------------------
// Hooks. Seats and register maps verified by disassembling gamemd-spawn.exe;
// none of these addresses is hooked by Ares/Antares/Phobos/Kratos (YR Hook
// Encyclopedia registry, 2026-10-25). Full write-up: DESIGN.md.
// ---------------------------------------------------------------------------

// VocClass::LoadAll(CCINIClass*) 0x7510D0, single exit. EDI = the soundmd INI.
// Stolen: pop edi; pop esi; pop ebp; add esp,0x824 -- no relative branch.
DEFINE_HOOK(0x7513DD, VocClass_LoadAll_CosmeticExt, 0x9)
{
	GET(CCINIClass*, pINI, EDI);
	DistanceSound::ReadINI(pINI);
	return 0;
}

// VocClass::PlayAt(int index, CoordStruct const&, AudioController*) entry,
// __fastcall: ECX = index, EDX = &coords, [esp+4] = controller.
// Stolen: mov al,[0x8464AC] (VoicesEnabled) -- no relative branch.
// Returning 0x7509EF lands on PlayAt's own `ret 4` with nothing pushed yet,
// which is exactly a "played nothing" return (EAX = 0; no caller reads it).
DEFINE_HOOK(0x7509E0, VocClass_PlayAt_CosmeticExt, 0x5)
{
	GET(int, index, ECX);
	GET(const CoordStruct*, pCoords, EDX);
	GET_STACK(AudioController*, pCtrl, 0x4);

	int play = index;
	if (pCoords && OnPlayAt(index, *pCoords, pCtrl, play))
	{
		R->EAX(0);
		return 0x7509EF; // CHAIN-OK: no other framework hooks 0x7509E0
	}

	R->ECX(play);
	return 0;
}

// Inside the positional volume function 0x750AC0, right after vanilla's
// linear volume is computed and before the Global/MinVolume floor and the
// 0.05 audibility cut. Locals: [esp+0x18] volume, [esp+0x38] px beyond the
// left/right edge, [esp+0x20] 2 * px beyond top/bottom, [esp+0x14] Range*60.
// EDI = sound data, EBX = its Type flags. [esp+0x34] = return address:
// 0x750A48 from PlayAt, whose controller (its EBP) was saved at [esp+0x8];
// 0x750D7B from the per-frame controller update 0x750D40, whose controller
// (its EBX) was saved at [esp+0xC].
// Stolen: test bl,0x10; je 0x750CBD -- a RELATIVE branch, so this hook never
// returns 0: both exits are taken explicitly.
DEFINE_HOOK(0x750C9B, VocClass_ComputeVolume_CosmeticExt, 0x5)
{
	GET(const VocClass*, pData, EDI);
	GET(DWORD, flags, EBX);
	GET_STACK(DWORD, retAddr, 0x34);
	REF_STACK(float, volume, 0x18);
	GET_STACK(float, dxo, 0x38);
	GET_STACK(float, dyo2, 0x20);
	GET_STACK(float, rangePx, 0x14);

	if (Active)
	{
		const bool fromPlayAt = retAddr == 0x750A48;
		const AudioController* pCtrl = fromPlayAt ? R->Stack<const AudioController*>(0x8)
			: retAddr == 0x750D7B ? R->Stack<const AudioController*>(0xC)
			: nullptr;
		volume = AdjustVolume(pData, volume, dxo, dyo2, rangePx, fromPlayAt, pCtrl);
	}

	return (flags & static_cast<DWORD>(SoundType::Global)) ? 0x750CA0 : 0x750CBD;
}

// Same function, pan just computed into EAX (0..0x4000), before it is stored
// to the caller's out-parameter. Stack frame as above.
// Stolen: fld [esp+0x18]; pop edi -- no relative branch; ESP-relative read is
// replayed with ESP unchanged.
DEFINE_HOOK(0x750D29, VocClass_ComputePan_CosmeticExt, 0x5)
{
	if (!Active)
		return 0;

	GET(const VocClass*, pData, EDI);
	GET(int, pan, EAX);
	GET_STACK(float, dxo, 0x38);
	GET_STACK(float, dyo2, 0x20);
	GET_STACK(float, rangePx, 0x14);

	R->EAX(AdjustPan(pData, pan, dxo, dyo2, rangePx));
	return 0;
}

// Main loop, once per frame, right after the frame's timeGetTime().
// Stolen: mov [0xA8B55C],eax -- no relative branch.
DEFINE_HOOK(0x55D3DA, MainLoop_CosmeticExt_Tick, 0x5)
{
	DistanceSound::FrameTick();
	return 0;
}
