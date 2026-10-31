// Engine glue for the rulesmd.ini ColorRules= tags: per-object remap colour
// and palette overrides. The cycle and precedence rules live in ColorModel;
// this file owns the INI tables, the per-object state, the creation contexts
// (factory, free unit, superweapon, deploy, attachment) and the hooks.
//
// Presentation only. Nothing here writes an object, a house or the rules: an
// object's owner, and so its losses, loyalty and AI, never change. The look is
// decided at the moment an object first enters the map, in simulation order,
// and "random" choices are hashed, so every client in an online game draws the
// same colours without touching the synced randomizer.

#include "ColorModel.h"

#include <BuildingClass.h>
#include <CCINIClass.h>
#include <ColorScheme.h>
#include <FactoryClass.h>
#include <FootClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Windows.h>
#include <objidl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ColorModel;

namespace
{
	// --- configuration, from the rules ---------------------------------------

	// One RemapColors= entry: a [Colors] name, or a keyword decided per object.
	enum class EntryKind : std::uint8_t { Colour, Owner, Invoker };

	struct Entry
	{
		EntryKind kind = EntryKind::Colour;
		std::string colour;
	};

	// A [Color_*] section.
	struct Rule
	{
		std::string key;            // the section name, upper case
		std::string name;           // as first written, for the log
		std::vector<Entry> remap;   // RemapColors=
		Pattern pattern = Pattern::Increment;
		Repeat repeat = Repeat::Beginning;
		Scope scope = Scope::House;
		std::string palette;        // PaletteSwitch=, without ".pal"
		bool replace = false;       // Replace=
		bool defined = false;       // some pass had a key of this section
	};

	// Tags on a TechnoType section. Rule names are upper case; empty = none.
	struct TypeCfg
	{
		std::string rule;             // ColorRules=
		std::string factoryRule;      // Factory.ColorRules=
		std::string freeUnitRule;     // FreeUnit.ColorRules=
		bool freeUnitInherit = false; // FreeUnit.ColorRules.Inherit=
		bool cloningInherit = false;  // Cloning.ColorRules.Inherit=
		bool deployInherit = false;   // Deploy.ColorRules.Inherit=
	};

	// Tags on a SuperWeaponType section.
	struct SWCfg
	{
		std::string rule;                 // ColorRules=
		std::vector<std::string> deliver; // Deliver.Types= (Ares UnitDelivery), upper case
		int deferment = 20;               // SW.Deferment=, Ares' default
	};

	std::unordered_map<std::string, Rule> Rules;  // by section name, upper case
	std::unordered_map<std::string, TypeCfg> Types;
	std::unordered_map<std::string, SWCfg> SWs;
	bool Active = false; // some section names a rule
	int RulesPass = 0;

	// --- runtime state, per game ---------------------------------------------

	// An object's look. Kept only for objects a rule touched, by UniqueID.
	struct ObjState
	{
		std::int16_t scheme = -1;  // ColorScheme::Array index; -1 = the owner's colour
		std::int16_t palette = -1; // PaletteNames index; -1 = the object's own palette
	};

	// A paradrop passenger waiting to land, or an Ares UnitDelivery waiting
	// for its deferment: the superweapon's rule reaches them later.
	struct PendingBirth
	{
		std::string rule; // the superweapon's rule, upper case; may be empty
		std::int32_t house = -1;
		std::uint64_t source = 0;
	};

	struct Delivery
	{
		std::string rule;
		std::int32_t house = -1;
		std::uint64_t source = 0;
		std::int32_t due = 0; // launch frame + SW.Deferment
		std::vector<std::string> types;
		std::vector<std::uint8_t> used;
	};

	// The object a deploy or undeploy is turning into, until it unlimbos.
	struct DeployRec
	{
		std::string parentType;
		bool hasState = false;
		ObjState state;
		HouseClass* owner = nullptr;
		std::uint64_t source = 0;
		int frame = 0;
	};

	// The factory product last kicked out, so the clones made in the same
	// call can find it.
	struct LastProduct
	{
		DWORD uid = 0;
		int frame = -1;
		std::string baseType;
		bool hasState = false;
		ObjState state;
	};

	// Creation contexts, open only within one call on one frame.
	enum class CtxKind : std::uint8_t { Super, GrandOpening };

	struct Ctx
	{
		CtxKind kind;
		void* who; // SuperClass* or BuildingClass*
		HouseClass* house;
		int frame;
		bool wasReady;
		std::vector<TechnoClass*> collected; // Super: born inside the launch
		std::vector<std::pair<TechnoClass*, TechnoClass*>> followers; // Super: (child, collected parent)
	};

	// An object between our Unlimbo seat and the end of TechnoClass::Unlimbo.
	// Whatever is born in that span is part of it: TechnoAttachmentExt
	// unlimbos an object's attachments from the same function's tail.
	struct Entering
	{
		TechnoClass* who;
		int frame;
	};

	std::unordered_map<DWORD, ObjState> States;
	std::unordered_set<DWORD> Seen; // look decided (first birth done)
	std::map<std::string, CycleBook> Books; // by rule, upper case
	std::vector<std::string> PaletteNames;
	std::unordered_map<DWORD, PendingBirth> Pending; // by passenger UniqueID
	std::vector<Delivery> Deliveries;
	std::unordered_map<DWORD, DeployRec> DeployChildren; // by the new object's UniqueID
	LastProduct Product;
	std::vector<Ctx> Stack;
	std::vector<Entering> EnteringMap;

	// Palette schemes from ColorScheme::GeneratePalette, by PaletteNames
	// index. The engine frees them when a theater rebuilds its palettes, so
	// the cache is dropped then (PaletteGeneration).
	std::vector<DynamicVectorClass<ColorScheme*>*> PaletteCache;
	int PaletteGeneration = 0;
	int PaletteCacheGeneration = -1;

	int LogBudget = 0;
	constexpr int LogBudgetPerGame = 40;
	int KickLogBudget = 0;
	constexpr int KickLogBudgetPerGame = 6;
	int DrawLogBudget = 0;
	constexpr int DrawLogBudgetPerGame = 20;
	std::unordered_set<DWORD> DrawLogged; // at most DrawLogBudgetPerGame entries
	bool LoggedTable = false;
	std::unordered_set<std::string> Warned;
	std::unordered_set<int> NoRemapWarned; // by PaletteNames index; checked while drawing

	// --- small helpers -------------------------------------------------------

	std::string Upper(const char* s)
	{
		std::string v(s ? s : "");
		for (auto& c : v)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return v;
	}

	bool EndsWithNoCase(const std::string& s, const char* suffix)
	{
		const size_t n = std::strlen(suffix);
		return s.size() >= n && EqualsNoCase(s.c_str() + (s.size() - n), suffix);
	}

	// CloningExt turns a product into "<X>.construct" and a clone into
	// "<X>.clone"; their tags fall back to X's.
	std::string BaseId(const char* id)
	{
		std::string v = Upper(id);
		for (const char* const suffix : { ".CLONE", ".CONSTRUCT" })
		{
			if (EndsWithNoCase(v, suffix))
			{
				v.resize(v.size() - std::strlen(suffix));
				break;
			}
		}
		return v;
	}

	void WarnOnce(const std::string& key, const char* fmt, const char* arg)
	{
		if (Warned.insert(key).second)
			Debug::Log(fmt, arg);
	}

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

	bool ReadYesNo(CCINIClass* pINI, const char* section, const char* key, bool& out)
	{
		std::string raw;
		if (!ReadRaw(pINI, section, key, raw))
			return false;
		switch (std::toupper(static_cast<unsigned char>(raw[0])))
		{
		case 'Y': case 'T': case '1':
			out = true;
			return true;
		case 'N': case 'F': case '0':
			out = false;
			return true;
		}
		return false;
	}

	// A ColorRules= value: a section name, or "none" to clear one set by an
	// earlier file.
	std::string RuleRef(const std::string& raw)
	{
		return EqualsNoCase(raw.c_str(), "none") ? std::string() : Upper(raw.c_str());
	}

	const Rule* FindRule(const std::string& key)
	{
		if (key.empty())
			return nullptr;
		auto const it = Rules.find(key);
		if (it == Rules.end() || !it->second.defined)
		{
			WarnOnce("rule:" + key, "[CosmeticExt] ColorRules: no [%s] section anywhere; the tag naming it is ignored.\n", key.c_str());
			return nullptr;
		}
		return &it->second;
	}

	const TypeCfg* FindType(const TechnoTypeClass* pType)
	{
		if (!pType || Types.empty())
			return nullptr;
		auto it = Types.find(Upper(pType->ID));
		if (it == Types.end())
			it = Types.find(BaseId(pType->ID));
		return it == Types.end() ? nullptr : &it->second;
	}

	const SWCfg* FindSW(const SuperWeaponTypeClass* pType)
	{
		if (!pType)
			return nullptr;
		auto const it = SWs.find(Upper(pType->ID));
		return it == SWs.end() ? nullptr : &it->second;
	}

	HouseClass* HouseAt(int index)
	{
		return index >= 0 && index < HouseClass::Array.Count ? HouseClass::Array.Items[index] : nullptr;
	}

	// Source scope identities. Building IDs differ between clients, but each
	// book turns an identity into its first-seen ordinal, which does not.
	std::uint64_t BuildingSource(const BuildingClass* pBuilding)
	{
		return pBuilding ? (1ull << 32) | pBuilding->UniqueID : 0;
	}

	std::uint64_t SuperSource(const HouseClass* pHouse, const SuperWeaponTypeClass* pType)
	{
		const std::uint32_t house = pHouse ? static_cast<std::uint32_t>(pHouse->ArrayIndex) & 0xFFFFu : 0xFFFFu;
		const std::uint32_t type = pType ? static_cast<std::uint32_t>(pType->ArrayIndex) & 0xFFFFu : 0xFFFFu;
		return (2ull << 32) | (house << 16) | type;
	}

	// --- colours and palettes ------------------------------------------------

	// [Colors] name -> ColorScheme::Array index, as the engine's own lookup
	// (0x474A90) does it: the first scheme of that name that is not the
	// single-shade twin.
	int FindScheme(const std::string& name)
	{
		auto const& schemes = ColorScheme::Array;
		for (int i = 0; i < schemes.Count; ++i)
		{
			auto const pScheme = schemes.Items[i];
			if (pScheme && pScheme->ID && pScheme->ShadeCount != 1 && EqualsNoCase(pScheme->ID, name.c_str()))
				return i;
		}
		WarnOnce("colour:" + Upper(name.c_str()), "[CosmeticExt] ColorRules: RemapColors names \"%s\", which is not under [Colors]; that entry keeps the owner's colour.\n", name.c_str());
		return -1;
	}

	const char* SchemeName(int index)
	{
		auto const& schemes = ColorScheme::Array;
		if (index < 0 || index >= schemes.Count || !schemes.Items[index] || !schemes.Items[index]->ID)
			return "owner";
		return schemes.Items[index]->ID;
	}

	ColorScheme* SchemeAt(const DynamicVectorClass<ColorScheme*>* pSchemes, int index)
	{
		if (!pSchemes || index < 0 || index >= pSchemes->Count)
			return nullptr;
		return pSchemes->Items[index];
	}

	std::int16_t PaletteIndex(const std::string& name)
	{
		for (size_t i = 0; i < PaletteNames.size(); ++i)
		{
			if (EqualsNoCase(PaletteNames[i].c_str(), name.c_str()))
				return static_cast<std::int16_t>(i);
		}
		PaletteNames.push_back(name);
		return static_cast<std::int16_t>(PaletteNames.size() - 1);
	}

	// The engine function art Palette= uses: it loads <name><theater>.pal
	// (desert01 -> DESERT01TEM.PAL in temperate) and builds one scheme per
	// colour. The engine caches it by name.
	DynamicVectorClass<ColorScheme*>* PaletteSchemes(int index)
	{
		if (index < 0 || index >= static_cast<int>(PaletteNames.size()))
			return nullptr;
		if (PaletteCacheGeneration != PaletteGeneration)
		{
			PaletteCache.clear();
			PaletteCacheGeneration = PaletteGeneration;
		}
		if (PaletteCache.size() < PaletteNames.size())
			PaletteCache.resize(PaletteNames.size(), nullptr);

		auto& cached = PaletteCache[static_cast<size_t>(index)];
		if (!cached)
		{
			char name[0x100];
			strncpy_s(name, PaletteNames[static_cast<size_t>(index)].c_str(), _TRUNCATE);
			cached = ColorScheme::GeneratePalette(name);
			if (!cached || cached->Count <= 0)
			{
				cached = nullptr;
				WarnOnce("palette:" + Upper(name), "[CosmeticExt] ColorRules: PaletteSwitch=%s found no <name><theater>.pal; those objects keep their own palette.\n", name);
			}
		}
		return cached;
	}

	// --- deciding a look -----------------------------------------------------

	struct Birth
	{
		const char* via = "type";        // for the log
		const Rule* source = nullptr;    // the creating source's rule, ranked over the type's
		bool inherit = false;            // an Inherit tag copies a parent's look
		ObjState inherited;
		HouseClass* invoker = nullptr;   // who spawned it; none = map, trigger, ...
		std::uint64_t sourceId = 0;      // Source scope identity
	};

	RuleSpec SpecOf(const Rule& rule)
	{
		return RuleSpec { !rule.remap.empty(), !rule.palette.empty(), rule.replace };
	}

	// Invoker with nobody behind the object: the colour a map-placed
	// civilian shows (Rex, 2026-10-30).
	int InvokerScheme(HouseClass* pInvoker)
	{
		if (pInvoker)
			return pInvoker->ColorSchemeIndex;

		const char* which = "Neutral";
		HouseClass* pHouse = HouseClass::FindNeutral();
		if (!pHouse)
		{
			which = "the Civilian side";
			pHouse = HouseClass::FindCivilianSide();
		}
		if (!pHouse)
		{
			which = "Special";
			pHouse = HouseClass::FindSpecial();
		}
		WarnOnce("invoker-fallback", "[CosmeticExt] ColorRules: Invoker with no invoker uses %s's colour.\n", pHouse ? which : "the owner (no Neutral, Civilian or Special house)");
		return pHouse ? pHouse->ColorSchemeIndex : -1;
	}

	std::int16_t TakeScheme(const Rule& rule, TechnoClass* pThis, const Birth& birth)
	{
		auto& book = Books[rule.key];

		std::int64_t key = 0;
		switch (rule.scope)
		{
		case Scope::House:
		{
			// Each player keeps a place: the invoker's when somebody spawned
			// it, else the owner's.
			HouseClass* const pHouse = birth.invoker ? birth.invoker : pThis->Owner;
			key = pHouse ? pHouse->ArrayIndex : -1;
			break;
		}
		case Scope::Game:
			key = 0;
			break;
		case Scope::Source:
			key = book.SourceKey(birth.sourceId);
			break;
		}

		CycleDef def;
		def.count = static_cast<int>(rule.remap.size());
		def.pattern = rule.pattern;
		def.repeat = rule.repeat;
		def.seed = Seed(rule.key.c_str(), rule.scope, key);
		const int entry = book.Take(def, key);
		if (entry < 0)
			return -1;

		auto const& e = rule.remap[static_cast<size_t>(entry)];
		switch (e.kind)
		{
		case EntryKind::Owner:
			return -1;
		case EntryKind::Invoker:
			return static_cast<std::int16_t>(InvokerScheme(birth.invoker));
		case EntryKind::Colour:
			return static_cast<std::int16_t>(FindScheme(e.colour));
		}
		return -1;
	}

	void LogTable()
	{
		LoggedTable = true;
		int n = 0;
		for (auto const& [key, rule] : Rules)
		{
			if (!rule.defined || ++n > 20)
				continue;
			std::string colours;
			for (auto const& e : rule.remap)
			{
				if (!colours.empty())
					colours += ",";
				colours += e.kind == EntryKind::Owner ? "Owner" : e.kind == EntryKind::Invoker ? "Invoker" : e.colour;
			}
			Debug::Log("[CosmeticExt] ColorRules: [%s] RemapColors=%s Pattern=%d Repeat=%d Scope=%d PaletteSwitch=%s Replace=%s\n",
				rule.name.c_str(), colours.empty() ? "-" : colours.c_str(),
				static_cast<int>(rule.pattern), static_cast<int>(rule.repeat), static_cast<int>(rule.scope),
				rule.palette.empty() ? "-" : rule.palette.c_str(), rule.replace ? "yes" : "no");
		}
	}

	void Resolve(TechnoClass* pThis, const Birth& birth)
	{
		auto const pType = pThis->GetTechnoType();
		auto const pCfg = FindType(pType);
		const Rule* const own = pCfg ? FindRule(pCfg->rule) : nullptr;

		// Highest rank first: an inherited look, the creating source, the type.
		std::vector<RuleSpec> ranked;
		std::vector<const Rule*> rules;
		if (birth.inherit)
		{
			ranked.push_back(RuleSpec { true, true, true });
			rules.push_back(nullptr);
		}
		if (birth.source)
		{
			ranked.push_back(SpecOf(*birth.source));
			rules.push_back(birth.source);
		}
		if (own)
		{
			ranked.push_back(SpecOf(*own));
			rules.push_back(own);
		}
		if (ranked.empty())
			return;

		if (!LoggedTable)
			LogTable();

		const Plan plan = Decide(ranked);
		ObjState st;
		if (plan.remap >= 0)
		{
			auto const pRule = rules[static_cast<size_t>(plan.remap)];
			if (!pRule)
				st.scheme = birth.inherited.scheme;
			else if (!pRule->remap.empty())
				st.scheme = TakeScheme(*pRule, pThis, birth);
			// else Replace=yes with no RemapColors=: the vanilla colour.
		}
		if (plan.palette >= 0)
		{
			auto const pRule = rules[static_cast<size_t>(plan.palette)];
			if (!pRule)
				st.palette = birth.inherited.palette;
			else if (!pRule->palette.empty())
				st.palette = PaletteIndex(pRule->palette);
		}

		const DWORD uid = pThis->UniqueID;
		if (st.scheme < 0 && st.palette < 0)
			States.erase(uid);
		else
			States[uid] = st;

		// Load the palette now, in the simulation, rather than mid-draw.
		if (st.palette >= 0)
			PaletteSchemes(st.palette);

		if (LogBudget > 0)
		{
			--LogBudget;
			Debug::Log("[CosmeticExt] ColorRules: %s #%u (%s) -> colour %s, palette %s\n",
				pType ? pType->ID : "?", uid, birth.via, SchemeName(st.scheme),
				st.palette >= 0 ? PaletteNames[static_cast<size_t>(st.palette)].c_str() : "own");
		}
	}

	bool StateOf(DWORD uid, ObjState& out)
	{
		auto const it = States.find(uid);
		if (it == States.end())
			return false;
		out = it->second;
		return true;
	}

	// --- creation contexts ---------------------------------------------------

	Ctx* Top()
	{
		const int now = Unsorted::CurrentFrame;
		while (!Stack.empty() && Stack.back().frame != now)
			Stack.pop_back(); // a context whose close we never saw
		return Stack.empty() ? nullptr : &Stack.back();
	}

	// Seen/States/Pending only grow with births; drop what died. Cheap enough
	// to run whenever Seen outgrows the live objects by a margin.
	void Prune()
	{
		const int now = Unsorted::CurrentFrame;
		std::unordered_set<DWORD> live;
		live.reserve(static_cast<size_t>(TechnoClass::Array.Count));
		for (auto const pTechno : TechnoClass::Array)
		{
			if (pTechno)
				live.insert(pTechno->UniqueID);
		}
		for (auto it = Seen.begin(); it != Seen.end();)
			it = live.count(*it) ? std::next(it) : Seen.erase(it);
		for (auto it = States.begin(); it != States.end();)
			it = live.count(it->first) ? std::next(it) : States.erase(it);
		for (auto it = Pending.begin(); it != Pending.end();)
			it = live.count(it->first) ? std::next(it) : Pending.erase(it);
		for (auto it = DeployChildren.begin(); it != DeployChildren.end();)
			it = now - it->second.frame > 30 ? DeployChildren.erase(it) : std::next(it);
	}

	void MaybePrune()
	{
		if (Seen.size() > static_cast<size_t>(TechnoClass::Array.Count) * 2 + 512)
			Prune();
	}

	// An Ares UnitDelivery places its types SW.Deferment frames after the
	// launch, outside any call we can see. Match by type in a window around
	// the due frame; each listed type matches once.
	bool MatchDelivery(TechnoClass* pThis, Birth& birth)
	{
		const int now = Unsorted::CurrentFrame;
		bool matched = false;
		for (auto it = Deliveries.begin(); it != Deliveries.end();)
		{
			auto& d = *it;
			if (now > d.due + 8)
			{
				it = Deliveries.erase(it);
				continue;
			}
			if (!matched && now >= d.due - 2)
			{
				const std::string id = Upper(pThis->GetTechnoType()->ID);
				for (size_t i = 0; i < d.types.size(); ++i)
				{
					if (!d.used[i] && d.types[i] == id)
					{
						d.used[i] = 1;
						matched = true;
						birth.via = "superweapon delivery";
						birth.source = FindRule(d.rule);
						birth.invoker = HouseAt(d.house);
						birth.sourceId = d.source;
						break;
					}
				}
			}
			++it;
		}
		return matched;
	}

	// The innermost object still inside TechnoClass::Unlimbo, if any.
	TechnoClass* EnteringParent()
	{
		const int now = Unsorted::CurrentFrame;
		while (!EnteringMap.empty() && EnteringMap.back().frame != now)
			EnteringMap.pop_back(); // an exit we never saw
		return EnteringMap.empty() ? nullptr : EnteringMap.back().who;
	}

	void BeginEntering(TechnoClass* pThis)
	{
		EnteringParent();
		EnteringMap.push_back(Entering { pThis, Unsorted::CurrentFrame });
	}

	void EndEntering(TechnoClass* pThis)
	{
		for (size_t i = EnteringMap.size(); i-- > 0;)
		{
			if (EnteringMap[i].who == pThis)
			{
				EnteringMap.resize(i);
				return;
			}
		}
	}

	// Born while pParent was entering the map, so part of it (a
	// TechnoAttachmentExt attachment, say). No context's rule reaches it and
	// it takes no cycle step from one. Its own ColorRules= wins; without one
	// it wears the parent's look.
	void ResolveAttached(TechnoClass* pThis, TechnoClass* pParent)
	{
		Birth birth;
		birth.via = "attached";
		birth.invoker = pParent->Owner;
		birth.sourceId = (4ull << 32) | pParent->UniqueID;
		auto const pCfg = FindType(pThis->GetTechnoType());
		if (!pCfg || !FindRule(pCfg->rule))
		{
			if (!StateOf(pParent->UniqueID, birth.inherited))
				return; // the parent has no look to share
			birth.inherit = true;
		}
		Resolve(pThis, birth);
	}

	// A launch decides its objects when it returns, so a child of one waits too.
	bool DecidedAtClose(const Ctx& ctx, const TechnoClass* pParent)
	{
		if (std::find(ctx.collected.begin(), ctx.collected.end(), pParent) != ctx.collected.end())
			return true;
		for (auto const& follower : ctx.followers)
		{
			if (follower.first == pParent)
				return true;
		}
		return false;
	}

	// The object a factory is producing? Sidebar production belongs to the
	// house (HouseClass::Begin_Production, 0x4FA4F5) and never sets
	// BuildingClass::Factory, which only a building's own factory fills. So
	// every factory is asked; a clone or a free unit is no factory's object.
	bool IsFactoryProduct(const BuildingClass* pBuilding, const TechnoClass* pThis)
	{
		if (pBuilding->Factory && pBuilding->Factory->Object == pThis)
			return true;
		for (auto const pFactory : FactoryClass::Array)
		{
			if (pFactory && pFactory->Object == pThis)
				return true;
		}
		return false;
	}

	// The object is entering the map for the first time: decide its look.
	void OnUnlimbo(TechnoClass* pThis)
	{
		if (!Active || !pThis || !pThis->GetTechnoType())
			return;
		const DWORD uid = pThis->UniqueID;
		if (!Seen.insert(uid).second)
			return;
		MaybePrune();

		if (TechnoClass* const pParent = EnteringParent(); pParent && pParent != pThis)
		{
			if (auto const pCtx = Top(); pCtx && pCtx->kind == CtxKind::Super && DecidedAtClose(*pCtx, pParent))
				pCtx->followers.emplace_back(pThis, pParent);
			else
				ResolveAttached(pThis, pParent);
			return;
		}

		Birth birth;
		if (auto const pending = Pending.find(uid); pending != Pending.end())
		{
			// A paradrop passenger landing.
			birth.via = "superweapon drop";
			birth.source = FindRule(pending->second.rule);
			birth.invoker = HouseAt(pending->second.house);
			birth.sourceId = pending->second.source;
			Pending.erase(pending);
		}
		else if (auto const pCtx = Top())
		{
			if (pCtx->kind == CtxKind::Super)
			{
				// Decided when the launch returns: only then do we know
				// whether this object is a carrier.
				pCtx->collected.push_back(pThis);
				return;
			}

			// Grand_Opening: a free unit. It belongs to the building and
			// never to whatever made the building (Rex, 2026-10-30).
			auto const pBuilding = static_cast<BuildingClass*>(pCtx->who);
			auto const pCfg = FindType(pBuilding->Type);
			birth.via = "free unit";
			birth.invoker = pBuilding->Owner;
			birth.sourceId = BuildingSource(pBuilding);
			if (pCfg && pCfg->freeUnitInherit)
			{
				birth.inherit = true;
				StateOf(pBuilding->UniqueID, birth.inherited);
			}
			else if (pCfg)
			{
				birth.source = FindRule(pCfg->freeUnitRule);
			}
		}
		else if (auto const deploy = DeployChildren.find(uid); deploy != DeployChildren.end())
		{
			auto const& rec = deploy->second;
			birth.via = "deploy";
			birth.invoker = rec.owner;
			birth.sourceId = rec.source;
			auto const parent = Types.find(rec.parentType);
			if (parent != Types.end() && parent->second.deployInherit)
			{
				birth.inherit = true;
				if (rec.hasState)
					birth.inherited = rec.state;
			}
			DeployChildren.erase(deploy);
		}
		else
		{
			MatchDelivery(pThis, birth);
		}

		Resolve(pThis, birth);
	}

	// BuildingClass::KickOutUnit entry: factory products and clones are
	// decided here, before the exit (and its Unlimbo) runs.
	// The first kick-outs of a game, whatever they turn out to be, so a
	// factory rule that never fires can be told from one never reached.
	void LogKickOut(const BuildingClass* pBuilding, const TechnoClass* pThis, const char* what)
	{
		if (KickLogBudget <= 0)
			return;
		--KickLogBudget;
		Debug::Log("[CosmeticExt] ColorRules: kick-out of %s #%u from %s: %s\n",
			pThis->GetTechnoType()->ID, pThis->UniqueID, pBuilding->Type ? pBuilding->Type->ID : "?", what);
	}

	void OnKickOut(BuildingClass* pBuilding, TechnoClass* pThis)
	{
		if (!Active || !pBuilding || !pThis || !pThis->GetTechnoType())
			return;
		const DWORD uid = pThis->UniqueID;
		if (Seen.count(uid))
			return;

		const int now = Unsorted::CurrentFrame;
		auto const pCfg = FindType(pBuilding->Type);
		Birth birth;
		birth.invoker = pBuilding->Owner;
		birth.sourceId = BuildingSource(pBuilding);

		if (IsFactoryProduct(pBuilding, pThis))
		{
			LogKickOut(pBuilding, pThis, "factory product");
			birth.via = "factory";
			if (pCfg)
				birth.source = FindRule(pCfg->factoryRule);
			Seen.insert(uid);
			MaybePrune();
			Resolve(pThis, birth);

			Product.uid = uid;
			Product.frame = now;
			Product.baseType = BaseId(pThis->GetTechnoType()->ID);
			Product.hasState = StateOf(uid, Product.state);
			return;
		}

		if (auto const pCtx = Top(); pCtx && pCtx->kind == CtxKind::GrandOpening && pCtx->who == pBuilding)
		{
			LogKickOut(pBuilding, pThis, "free unit, decided as it unlimbos");
			return;
		}

		if (Product.frame == now && Product.uid != uid && Product.baseType == BaseId(pThis->GetTechnoType()->ID))
		{
			// A cloning vat copying the product kicked out in this call.
			// The factory's rule stays with the factory (Rex, 2026-10-30).
			LogKickOut(pBuilding, pThis, "clone of the product");
			birth.via = "clone";
			if (pCfg && pCfg->cloningInherit)
			{
				birth.inherit = true;
				if (Product.hasState)
					birth.inherited = Product.state;
			}
			Seen.insert(uid);
			MaybePrune();
			Resolve(pThis, birth);
			return;
		}
		LogKickOut(pBuilding, pThis, "no factory's product, decided as it unlimbos");
	}

	void OpenSuper(HouseClass* pHouse, SuperClass* pSuper)
	{
		if (!Active || !pSuper)
			return;
		Top();
		Stack.push_back(Ctx { CtxKind::Super, pSuper, pHouse, Unsorted::CurrentFrame, pSuper->IsReady, { } });
	}

	void CloseSuper(SuperClass* pSuper)
	{
		auto const pCtx = Top();
		if (!pCtx || pCtx->kind != CtxKind::Super || pCtx->who != pSuper)
			return;
		Ctx ctx = std::move(*pCtx);
		Stack.pop_back();

		auto const pSW = FindSW(pSuper->Type);
		auto const pRule = pSW ? FindRule(pSW->rule) : nullptr;
		const std::uint64_t source = SuperSource(ctx.house, pSuper->Type);

		for (auto const pThis : ctx.collected)
		{
			Birth birth;
			birth.invoker = ctx.house;
			birth.sourceId = source;
			if (pThis->Passengers.NumPassengers > 0)
			{
				// A paradrop plane: a carrier, not part of the drop. It
				// keeps its own rule; its passengers take the superweapon's
				// when they land.
				birth.via = "superweapon carrier";
				for (ObjectClass* p = pThis->Passengers.FirstPassenger; p; p = p->NextObject)
				{
					Pending[p->UniqueID] = PendingBirth { pSW ? pSW->rule : std::string(),
						ctx.house ? ctx.house->ArrayIndex : -1, source };
				}
			}
			else
			{
				birth.via = "superweapon";
				birth.source = pRule;
			}
			Resolve(pThis, birth);
		}
		for (auto const& [pChild, pParent] : ctx.followers)
			ResolveAttached(pChild, pParent);

		// Antares clears IsReady only when the launch really happened
		// (SuperClass_ClickFire); PreClick/PostClick weapons stay ready.
		if (pSW && !pSW->deliver.empty() && ctx.wasReady && !pSuper->IsReady)
		{
			Delivery d;
			d.rule = pSW->rule;
			d.house = ctx.house ? ctx.house->ArrayIndex : -1;
			d.source = source;
			d.due = ctx.frame + pSW->deferment;
			d.types = pSW->deliver;
			d.used.assign(d.types.size(), 0);
			Deliveries.push_back(std::move(d));
		}
	}

	void OpenGrandOpening(BuildingClass* pBuilding)
	{
		if (!Active || !pBuilding)
			return;
		Top();
		Stack.push_back(Ctx { CtxKind::GrandOpening, pBuilding, pBuilding->Owner, Unsorted::CurrentFrame, false, { } });
	}

	void CloseGrandOpening(BuildingClass* pBuilding)
	{
		auto const pCtx = Top();
		if (pCtx && pCtx->kind == CtxKind::GrandOpening && pCtx->who == pBuilding)
			Stack.pop_back();
	}

	void RecordDeploy(TechnoClass* pParent, TechnoClass* pChild)
	{
		if (!Active || !pParent || !pChild || !pParent->GetTechnoType())
			return;
		DeployRec rec;
		rec.parentType = Upper(pParent->GetTechnoType()->ID);
		rec.hasState = StateOf(pParent->UniqueID, rec.state);
		rec.owner = pParent->Owner;
		rec.source = (3ull << 32) | pParent->UniqueID;
		rec.frame = Unsorted::CurrentFrame;
		DeployChildren[pChild->UniqueID] = rec;
	}

	// --- reading the rules ---------------------------------------------------

	void ReadRuleSection(CCINIClass* pINI, Rule& rule)
	{
		const char* const sec = rule.name.c_str();
		std::string raw;

		if (ReadRaw(pINI, sec, "RemapColors", raw))
		{
			rule.defined = true;
			rule.remap.clear();
			for (auto const& item : SplitList(raw.c_str()))
			{
				Entry e;
				if (EqualsNoCase(item.c_str(), "Owner"))
					e.kind = EntryKind::Owner;
				else if (EqualsNoCase(item.c_str(), "Invoker"))
					e.kind = EntryKind::Invoker;
				else
					e.colour = item;
				rule.remap.push_back(e);
			}
		}
		if (ReadRaw(pINI, sec, "RemapColors.Pattern", raw))
		{
			rule.defined = true;
			if (!ParsePattern(raw.c_str(), rule.pattern))
				WarnOnce("pattern:" + rule.key, "[CosmeticExt] ColorRules: [%s] RemapColors.Pattern is not Increment, Decrement or Random; kept.\n", sec);
		}
		if (ReadRaw(pINI, sec, "RemapColors.Repeat", raw))
		{
			rule.defined = true;
			if (!ParseRepeat(raw.c_str(), rule.repeat))
				WarnOnce("repeat:" + rule.key, "[CosmeticExt] ColorRules: [%s] RemapColors.Repeat is not Beginning, End, Random, Center, First or Last; kept.\n", sec);
		}
		if (ReadRaw(pINI, sec, "RemapColors.Scope", raw))
		{
			rule.defined = true;
			if (!ParseScope(raw.c_str(), rule.scope))
				WarnOnce("scope:" + rule.key, "[CosmeticExt] ColorRules: [%s] RemapColors.Scope is not House, Game or Source; kept.\n", sec);
		}
		if (ReadRaw(pINI, sec, "PaletteSwitch", raw))
		{
			rule.defined = true;
			if (EqualsNoCase(raw.c_str(), "none"))
				raw.clear();
			else if (EndsWithNoCase(raw, ".pal"))
				raw.resize(raw.size() - 4);
			rule.palette = raw;
		}
		bool replace = false;
		if (ReadYesNo(pINI, sec, "Replace", replace))
		{
			rule.defined = true;
			rule.replace = replace;
		}
	}

	void NoteRef(const std::string& key, const std::string& raw)
	{
		if (key.empty())
			return;
		auto& rule = Rules[key];
		if (rule.key.empty())
		{
			rule.key = key;
			rule.name = raw;
		}
	}

	// RulesClass::Read_File tail, once per pass (rulesmd, game mode, map).
	// A tag missing from a pass leaves the earlier value alone, so later
	// files override rather than wipe.
	void ReadRulesPass(CCINIClass* pINI)
	{
		if (!pINI)
			return;
		const bool rulesFile = pINI == CCINIClass::INI_Rules;
		if (rulesFile)
		{
			Rules.clear();
			Types.clear();
			SWs.clear();
			RulesPass = 0;
		}
		++RulesPass;

		std::string raw;
		bool flag = false;
		for (auto const pType : TechnoTypeClass::Array)
		{
			if (!pType)
				continue;
			const char* const sec = pType->ID;
			TypeCfg* pCfg = nullptr;
			auto cfg = [&]() -> TypeCfg& {
				if (!pCfg)
					pCfg = &Types[Upper(sec)];
				return *pCfg;
			};
			if (ReadRaw(pINI, sec, "ColorRules", raw))
			{
				cfg().rule = RuleRef(raw);
				NoteRef(cfg().rule, raw);
			}
			if (ReadRaw(pINI, sec, "Factory.ColorRules", raw))
			{
				cfg().factoryRule = RuleRef(raw);
				NoteRef(cfg().factoryRule, raw);
			}
			if (ReadRaw(pINI, sec, "FreeUnit.ColorRules", raw))
			{
				cfg().freeUnitRule = RuleRef(raw);
				NoteRef(cfg().freeUnitRule, raw);
			}
			if (ReadYesNo(pINI, sec, "FreeUnit.ColorRules.Inherit", flag))
				cfg().freeUnitInherit = flag;
			if (ReadYesNo(pINI, sec, "Cloning.ColorRules.Inherit", flag))
				cfg().cloningInherit = flag;
			if (ReadYesNo(pINI, sec, "Deploy.ColorRules.Inherit", flag))
				cfg().deployInherit = flag;
		}

		for (auto const pType : SuperWeaponTypeClass::Array)
		{
			if (!pType)
				continue;
			const char* const sec = pType->ID;
			SWCfg* pCfg = nullptr;
			auto cfg = [&]() -> SWCfg& {
				if (!pCfg)
					pCfg = &SWs[Upper(sec)];
				return *pCfg;
			};
			if (ReadRaw(pINI, sec, "ColorRules", raw))
			{
				cfg().rule = RuleRef(raw);
				NoteRef(cfg().rule, raw);
			}
			if (ReadRaw(pINI, sec, "Deliver.Types", raw))
			{
				cfg().deliver.clear();
				for (auto const& item : SplitList(raw.c_str()))
					cfg().deliver.push_back(Upper(item.c_str()));
			}
			if (ReadRaw(pINI, sec, "SW.Deferment", raw))
				cfg().deferment = pINI->ReadInteger(sec, "SW.Deferment", cfg().deferment);
		}

		// Rule sections. One first named by a later file may still be
		// defined in rulesmd.ini, so read that first.
		for (auto& [key, rule] : Rules)
		{
			if (!rulesFile && !rule.defined && CCINIClass::INI_Rules)
				ReadRuleSection(CCINIClass::INI_Rules, rule);
			ReadRuleSection(pINI, rule);
		}

		Active = false;
		int typeRefs = 0;
		for (auto const& [id, cfg] : Types)
		{
			if (!cfg.rule.empty() || !cfg.factoryRule.empty() || !cfg.freeUnitRule.empty())
				++typeRefs;
		}
		int swRefs = 0;
		for (auto const& [id, cfg] : SWs)
		{
			if (!cfg.rule.empty())
				++swRefs;
		}
		Active = typeRefs + swRefs > 0;

		int defined = 0;
		for (auto const& [key, rule] : Rules)
			defined += rule.defined ? 1 : 0;
		Debug::Log("[CosmeticExt] ColorRules: rules pass %d: %d rule sections, %d types and %d superweapons name one%s\n",
			RulesPass, defined, typeRefs, swRefs, Active ? "" : " (feature idle)");
	}

	// --- per-game reset and the savegame block -------------------------------

	void ClearRuntime()
	{
		States.clear();
		Seen.clear();
		Books.clear();
		PaletteNames.clear();
		PaletteCache.clear();
		NoRemapWarned.clear();
		Pending.clear();
		Deliveries.clear();
		DeployChildren.clear();
		Product = LastProduct { };
		Stack.clear();
		EnteringMap.clear();
		DrawLogged.clear();
		LogBudget = LogBudgetPerGame;
		KickLogBudget = KickLogBudgetPerGame;
		DrawLogBudget = DrawLogBudgetPerGame;
		LoggedTable = false;
	}

	constexpr std::uint32_t BlockMagic = 0x52434543; // 'CECR'
	constexpr std::uint32_t BlockVersion = 1;

	struct Writer
	{
		std::vector<char> buf;

		void Raw(const void* p, size_t n)
		{
			auto const c = static_cast<const char*>(p);
			buf.insert(buf.end(), c, c + n);
		}
		void U32(std::uint32_t v) { Raw(&v, sizeof(v)); }
		void I32(std::int32_t v) { Raw(&v, sizeof(v)); }
		void I64(std::int64_t v) { Raw(&v, sizeof(v)); }
		void U64(std::uint64_t v) { Raw(&v, sizeof(v)); }
		void Str(const std::string& s)
		{
			U32(static_cast<std::uint32_t>(s.size()));
			Raw(s.data(), s.size());
		}
	};

	struct Reader
	{
		const char* p;
		const char* end;
		bool ok = true;

		bool Raw(void* out, size_t n)
		{
			if (!ok || static_cast<size_t>(end - p) < n)
				return ok = false;
			std::memcpy(out, p, n);
			p += n;
			return true;
		}
		std::uint32_t U32() { std::uint32_t v = 0; Raw(&v, sizeof(v)); return v; }
		std::int32_t I32() { std::int32_t v = 0; Raw(&v, sizeof(v)); return v; }
		std::int64_t I64() { std::int64_t v = 0; Raw(&v, sizeof(v)); return v; }
		std::uint64_t U64() { std::uint64_t v = 0; Raw(&v, sizeof(v)); return v; }
		std::string Str()
		{
			const std::uint32_t n = U32();
			if (!ok || static_cast<size_t>(end - p) < n)
			{
				ok = false;
				return { };
			}
			std::string s(p, n);
			p += n;
			return s;
		}
		// A count of items of at least `size` bytes each that can still fit.
		std::uint32_t Count(size_t size)
		{
			const std::uint32_t n = U32();
			if (ok && static_cast<size_t>(end - p) / size < n)
				ok = false;
			return ok ? n : 0;
		}
	};

	void WriteState(Writer& w)
	{
		Prune();

		w.U32(static_cast<std::uint32_t>(PaletteNames.size()));
		for (auto const& name : PaletteNames)
			w.Str(name);

		w.U32(static_cast<std::uint32_t>(States.size()));
		for (auto const& [uid, st] : States)
		{
			w.U32(uid);
			w.I32(st.scheme);
			w.I32(st.palette);
		}

		w.U32(static_cast<std::uint32_t>(Seen.size()));
		for (auto const uid : Seen)
			w.U32(uid);

		w.U32(static_cast<std::uint32_t>(Books.size()));
		for (auto const& [key, book] : Books)
		{
			w.Str(key);
			w.U32(static_cast<std::uint32_t>(book.places.size()));
			for (auto const& [place, cs] : book.places)
			{
				w.I64(place);
				w.I32(cs.pass);
				w.I32(cs.idx);
				w.I32(cs.start);
				w.I32(cs.first);
				w.I32(cs.last);
			}
			w.U32(static_cast<std::uint32_t>(book.ordinals.size()));
			for (auto const& [identity, ordinal] : book.ordinals)
			{
				w.U64(identity);
				w.I32(ordinal);
			}
		}

		w.U32(static_cast<std::uint32_t>(Pending.size()));
		for (auto const& [uid, pb] : Pending)
		{
			w.U32(uid);
			w.Str(pb.rule);
			w.I32(pb.house);
			w.U64(pb.source);
		}

		w.U32(static_cast<std::uint32_t>(Deliveries.size()));
		for (auto const& d : Deliveries)
		{
			w.Str(d.rule);
			w.I32(d.house);
			w.U64(d.source);
			w.I32(d.due);
			w.U32(static_cast<std::uint32_t>(d.types.size()));
			for (size_t i = 0; i < d.types.size(); ++i)
			{
				w.Str(d.types[i]);
				w.U32(d.used[i]);
			}
		}
	}

	bool ReadState(Reader& r)
	{
		for (std::uint32_t n = r.Count(4); n--;)
			PaletteNames.push_back(r.Str());

		for (std::uint32_t n = r.Count(12); n--;)
		{
			const DWORD uid = r.U32();
			ObjState st;
			st.scheme = static_cast<std::int16_t>(r.I32());
			st.palette = static_cast<std::int16_t>(r.I32());
			if (st.palette >= static_cast<int>(PaletteNames.size()))
				st.palette = -1;
			States[uid] = st;
		}

		for (std::uint32_t n = r.Count(4); n--;)
			Seen.insert(r.U32());

		for (std::uint32_t n = r.Count(4); n--;)
		{
			auto& book = Books[r.Str()];
			for (std::uint32_t m = r.Count(28); m--;)
			{
				const std::int64_t place = r.I64();
				CycleState cs;
				cs.pass = r.I32();
				cs.idx = r.I32();
				cs.start = r.I32();
				cs.first = r.I32();
				cs.last = r.I32();
				book.places[place] = cs;
			}
			for (std::uint32_t m = r.Count(12); m--;)
			{
				const std::uint64_t identity = r.U64();
				book.ordinals[identity] = r.I32();
			}
		}

		for (std::uint32_t n = r.Count(20); n--;)
		{
			const DWORD uid = r.U32();
			PendingBirth pb;
			pb.rule = r.Str();
			pb.house = r.I32();
			pb.source = r.U64();
			Pending[uid] = pb;
		}

		for (std::uint32_t n = r.Count(24); n--;)
		{
			Delivery d;
			d.rule = r.Str();
			d.house = r.I32();
			d.source = r.U64();
			d.due = r.I32();
			for (std::uint32_t m = r.Count(8); m--;)
			{
				d.types.push_back(r.Str());
				d.used.push_back(static_cast<std::uint8_t>(r.U32() != 0));
			}
			Deliveries.push_back(std::move(d));
		}
		return r.ok;
	}

	bool StreamWrite(IStream* pStm, const void* p, ULONG n)
	{
		ULONG written = 0;
		return SUCCEEDED(pStm->Write(p, n, &written)) && written == n;
	}

	bool StreamRead(IStream* pStm, void* p, ULONG n)
	{
		ULONG read = 0;
		return SUCCEEDED(pStm->Read(p, n, &read)) && read == n;
	}

	void SaveBlock(IStream* pStm)
	{
		Writer w;
		WriteState(w);
		const std::uint32_t head[3] = { BlockMagic, BlockVersion, static_cast<std::uint32_t>(w.buf.size()) };
		if (!StreamWrite(pStm, head, sizeof(head))
			|| (!w.buf.empty() && !StreamWrite(pStm, w.buf.data(), static_cast<ULONG>(w.buf.size()))))
		{
			Debug::Log("[CosmeticExt] ColorRules: failed to write the savegame block.\n");
			return;
		}
		Debug::Log("[CosmeticExt] ColorRules: saved %u object looks, %u cycles.\n",
			static_cast<unsigned>(States.size()), static_cast<unsigned>(Books.size()));
	}

	// The block trails the whole save, after every other DLL's (CosmeticExt
	// is last in the injection list), so a save made without it simply ends
	// here: the read comes back short and nothing else is disturbed.
	void LoadBlock(IStream* pStm)
	{
		ClearRuntime();

		std::uint32_t magic = 0;
		if (!StreamRead(pStm, &magic, sizeof(magic)))
		{
			Debug::Log("[CosmeticExt] ColorRules: this save has no ColorRules block (made before it existed); objects keep their vanilla look.\n");
			return;
		}
		if (magic != BlockMagic)
		{
			Debug::Log("[CosmeticExt] ColorRules: unexpected data where the ColorRules block should be; state not restored.\n");
			return;
		}
		std::uint32_t head[2] = { };
		if (!StreamRead(pStm, head, sizeof(head)) || head[1] > (64u << 20))
		{
			Debug::Log("[CosmeticExt] ColorRules: truncated savegame block; state not restored.\n");
			return;
		}
		std::vector<char> payload(head[1]);
		if (!payload.empty() && !StreamRead(pStm, payload.data(), static_cast<ULONG>(payload.size())))
		{
			Debug::Log("[CosmeticExt] ColorRules: truncated savegame block; state not restored.\n");
			return;
		}
		if (head[0] != BlockVersion)
		{
			Debug::Log("[CosmeticExt] ColorRules: savegame block version %u, expected %u; state not restored.\n", head[0], BlockVersion);
			return;
		}

		Reader r { payload.data(), payload.data() + payload.size() };
		if (!ReadState(r))
		{
			ClearRuntime();
			Debug::Log("[CosmeticExt] ColorRules: malformed savegame block; state not restored.\n");
			return;
		}
		for (int i = 0; i < static_cast<int>(PaletteNames.size()); ++i)
			PaletteSchemes(i);
		Debug::Log("[CosmeticExt] ColorRules: restored %u object looks, %u cycles.\n",
			static_cast<unsigned>(States.size()), static_cast<unsigned>(Books.size()));
	}

	// --- the draw side: TechnoClass::GetRemapColour ---------------------------

	// The scheme pThis is drawn with, or nullptr for the engine's own answer.
	// Its LightConvert is what GetRemapColour returns.
	ColorScheme* StyledScheme(const TechnoClass* pThis)
	{
		if (States.empty() || !pThis)
			return nullptr;
		auto const it = States.find(pThis->UniqueID);
		if (it == States.end())
			return nullptr;
		const ObjState& st = it->second;
		HouseClass* const pOwner = pThis->Owner;
		if (!pOwner)
			return nullptr;

		// A disguise the local player can't see through keeps the vanilla
		// colour, which is the disguise's.
		if (HouseClass::CurrentPlayer && pThis->IsDisguised() && !pThis->IsClearlyVisibleTo(HouseClass::CurrentPlayer))
			return nullptr;

		const int scheme = st.scheme >= 0 ? st.scheme : pOwner->ColorSchemeIndex;
		const DynamicVectorClass<ColorScheme*>* pSchemes = nullptr;
		if (st.palette >= 0)
		{
			pSchemes = PaletteSchemes(st.palette);
			if (!pSchemes)
				return nullptr;
		}
		else if (st.scheme < 0)
		{
			return nullptr;
		}
		else if (auto const pType = pThis->GetTechnoType())
		{
			pSchemes = pType->Palette; // art Palette=, as the engine does
		}
		if (!pSchemes)
			pSchemes = &ColorScheme::Array;

		ColorScheme* pScheme = SchemeAt(pSchemes, scheme);
		if ((!pScheme || !pScheme->LightConvert) && st.palette >= 0)
		{
			// The palette has no remap for this colour: show it in the owner's.
			if (NoRemapWarned.insert(st.palette).second)
				Debug::Log("[CosmeticExt] ColorRules: PaletteSwitch=%s has no remap for that colour; showing the owner's.\n", PaletteNames[static_cast<size_t>(st.palette)].c_str());
			pScheme = SchemeAt(pSchemes, pOwner->ColorSchemeIndex);
		}
		if (!pScheme || !pScheme->LightConvert)
			return nullptr;

		if (DrawLogBudget > 0 && DrawLogged.insert(pThis->UniqueID).second)
		{
			--DrawLogBudget;
			auto const pType = pThis->GetTechnoType();
			Debug::Log("[CosmeticExt] ColorRules: drawing %s #%u in colour %s, palette %s\n",
				pType ? pType->ID : "?", pThis->UniqueID, SchemeName(st.scheme),
				st.palette >= 0 ? PaletteNames[static_cast<size_t>(st.palette)].c_str() : "own");
		}
		return pScheme;
	}
}

// ---------------------------------------------------------------------------
// Hooks. Seats and register maps verified by disassembling gamemd-spawn.exe;
// every one returns 0 except the two GetRemapColour exits, which nobody
// else hooks, and none of the stolen windows holds a relative branch or
// call. Sharing per the YR Hook Encyclopedia registry and the
// deployed DLLs' .syhks00 sections (2026-10-30). Full write-up: DESIGN.md.
// ---------------------------------------------------------------------------

// RulesClass::Read_File tail, once per pass. ESI = this pass's INI.
// Stolen: mov eax,[0xA8B238]. Shared with Phobos (two hooks, both return 0).
DEFINE_HOOK(0x668F6A, RulesClass_Read_File_CosmeticExt_ColorRules, 0x5)
{
	GET(CCINIClass*, pINI, ESI);
	ReadRulesPass(pINI);
	return 0;
}

// TechnoClass::Unlimbo, right after ObjectClass::Unlimbo succeeded.
// ESI = this. Stolen: mov eax,[edi]; push 1; cdq. From here to the two
// exits below, whatever else unlimbos is part of this object.
DEFINE_HOOK(0x6F6CBE, TechnoClass_Unlimbo_CosmeticExt_ColorRules, 0x5)
{
	GET(TechnoClass*, pThis, ESI);
	OnUnlimbo(pThis);
	BeginEntering(pThis);
	return 0;
}

// TechnoClass::Unlimbo's early success exit, ESI = this. Stolen: pop edi;
// mov al,1; pop esi; add esp,0x18. Shared with Antares (TechnoClass_Put_1,
// returns 0).
DEFINE_HOOK(0x6F6D0E, TechnoClass_Unlimbo_CosmeticExt_ColorRules_Done1, 0x7)
{
	GET(TechnoClass*, pThis, ESI);
	EndEntering(pThis);
	return 0;
}

// TechnoClass::Unlimbo's last exit, ESI = this. Stolen: mov al,1; pop esi;
// add esp,0x18. Shared with Antares, TechnoAttachmentExt (which unlimbos
// the attachments here) and SquadExt, all returning 0 and all ahead of us.
DEFINE_HOOK(0x6F6F20, TechnoClass_Unlimbo_CosmeticExt_ColorRules_Done2, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	EndEntering(pThis);
	return 0;
}

// BuildingClass::KickOutUnit entry, __thiscall: ECX = building,
// [esp+4] = the object leaving. Stolen: sub esp,0x130. Shared with
// CloningExt and GiftBoxHost (both return 0; CloningExt has already turned
// a product into its .construct type when we run, hence BaseId).
DEFINE_HOOK(0x443C60, BuildingClass_KickOutUnit_CosmeticExt_ColorRules, 0x6)
{
	GET(BuildingClass*, pThis, ECX);
	GET_STACK(TechnoClass*, pObj, 0x4);
	OnKickOut(pThis, pObj);
	return 0;
}

// HouseClass::Fire_SW, just before SuperClass::ClickFire. EBX = house,
// EDI = the SuperClass. Stolen: mov eax,[0xA83D4C].
DEFINE_HOOK(0x4FAE85, HouseClass_Fire_SW_CosmeticExt_ColorRules_Open, 0x5)
{
	GET(HouseClass*, pHouse, EBX);
	GET(SuperClass*, pSuper, EDI);
	OpenSuper(pHouse, pSuper);
	return 0;
}

// HouseClass::Fire_SW, its only epilogue. EDI = the SuperClass still.
// Stolen: pop edi; pop esi; pop ebp; mov al,1.
DEFINE_HOOK(0x4FAEED, HouseClass_Fire_SW_CosmeticExt_ColorRules_Close, 0x5)
{
	GET(SuperClass*, pSuper, EDI);
	CloseSuper(pSuper);
	return 0;
}

// BuildingClass::Grand_Opening (0x445F80, EBP = this throughout), where the
// FreeUnit= block starts. Stolen: mov ecx,[ebp+0x520].
DEFINE_HOOK(0x446AA9, BuildingClass_Grand_Opening_CosmeticExt_ColorRules_Open, 0x6)
{
	GET(BuildingClass*, pThis, EBP);
	OpenGrandOpening(pThis);
	return 0;
}

// Grand_Opening epilogue. Stolen: pop edi; pop esi; pop ebp; pop ebx;
// add esp,0x58 -- the hook runs before them, so EBP is still the building.
DEFINE_HOOK(0x446FB6, BuildingClass_Grand_Opening_CosmeticExt_ColorRules_Close, 0x7)
{
	GET(BuildingClass*, pThis, EBP);
	CloseGrandOpening(pThis);
	return 0;
}

// UnitClass deploying into a building (0x7393C0, EBP = this throughout),
// right after the BuildingClass constructor. EBX = the new building.
// Stolen: mov eax,[ebx]; push edi; push 0x12. Shared with Phobos (returns 0).
DEFINE_HOOK(0x7396D2, UnitClass_Deploy_CosmeticExt_ColorRules, 0x5)
{
	GET(TechnoClass*, pUnit, EBP);
	GET(TechnoClass*, pBuilding, EBX);
	RecordDeploy(pUnit, pBuilding);
	return 0;
}

// BuildingClass::Mi_Selling undeploying (0x449C30, EBP = this throughout),
// after the UnitClass constructor. EBX = the new unit, or 0.
// Stolen: mov ecx,[0xA8E7AC].
DEFINE_HOOK(0x449E4F, BuildingClass_Mi_Selling_CosmeticExt_ColorRules, 0x6)
{
	GET(TechnoClass*, pBuilding, EBP);
	GET(TechnoClass*, pUnit, EBX);
	if (pUnit)
		RecordDeploy(pBuilding, pUnit);
	return 0;
}

// TechnoClass::GetRemapColour (0x705D70, virtual slot 0x1E4), the exit for
// a type with art Palette=. ESI = this. Hooked inside the function, not
// wrapped in the vtable, because Phobos calls 0x705D70 directly for SHP
// vehicles and a few other draws. Stolen: mov edx,[eax+4];
// mov eax,[edx+edi*4]. 0x705D98 is the function's own tail: pop edi;
// pop esi; mov eax,[eax+0x30C]; ret -- so EAX = our scheme returns its
// LightConvert. Phobos' hook at 0x705D74 resumes at 0x705D7C, before both.
DEFINE_HOOK(0x705D92, TechnoClass_GetRemapColour_CosmeticExt_ColorRules_TypePalette, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	if (auto const pScheme = StyledScheme(pThis))
	{
		R->EAX(pScheme);
		return 0x705D98; // CHAIN-OK: no other framework hooks 0x705D92
	}
	return 0;
}

// The house-colour exit (also taken when a disguise is seen through).
// ESI = this. Stolen: mov eax,[0xB054D4]. 0x705DF1 is the tail: pop edi;
// pop esi; mov eax,[ecx+0x30C]; ret -- so ECX = our scheme.
DEFINE_HOOK(0x705DE9, TechnoClass_GetRemapColour_CosmeticExt_ColorRules_Owner, 0x5)
{
	GET(TechnoClass*, pThis, ESI);
	if (auto const pScheme = StyledScheme(pThis))
	{
		R->ECX(pScheme);
		return 0x705DF1; // CHAIN-OK: no other framework hooks 0x705DE9
	}
	return 0;
}

// The theater has just rebuilt its palettes, freeing the generated palette
// schemes we cache. Stolen: mov ecx,0xA8B238.
DEFINE_HOOK(0x534DBE, Theater_Palettes_CosmeticExt_ColorRules, 0x5)
{
	++PaletteGeneration;
	return 0;
}

// SaveGame, after the whole object stream (the end-of-save append point
// Antares, GiftBoxHost and Kratos share; CosmeticExt is last). ESI = the
// IStream. Stolen: test eax,eax; pop edi; pop esi; pop ebp -- ESI is
// still the stream when we run.
DEFINE_HOOK(0x67E42E, SaveGame_CosmeticExt_ColorRules, 0x5)
{
	GET(IStream*, pStm, ESI);
	SaveBlock(pStm);
	return 0;
}

// LoadGame, after the whole object stream, the matching read point. ESI =
// the IStream. Stolen: pop edi; pop esi; pop ebp; mov al,1.
DEFINE_HOOK(0x67F7C8, LoadGame_CosmeticExt_ColorRules, 0x5)
{
	GET(IStream*, pStm, ESI);
	LoadBlock(pStm);
	return 0;
}

// Scenario::ClearClasses: a new game, or the reset before a load.
// Stolen: mov dword [edx+0x214],1000000 (IntelExt's seat, size 0xA).
DEFINE_HOOK(0x685659, Scenario_ClearClasses_CosmeticExt_ColorRules, 0xA)
{
	ClearRuntime();
	return 0;
}
