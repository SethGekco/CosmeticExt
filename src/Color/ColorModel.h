#pragma once

// The rules behind the rulesmd.ini ColorRules= tags: colour cycles, their
// restart points, and which rule decides which channel of an object's look.
//
// Pure C++: no YRpp, no Windows headers. tests/test_color_model.cpp builds
// this file with a host compiler. ColorRules.cpp is the only place that talks
// to the engine.
//
// Determinism. Every client in an online game must pick the same colour for
// the same object, so nothing here draws from a randomizer. "Random" patterns
// and restart points come from a hash of the rule's name, the cycle's scope
// key and the pass number, through a PRNG written out below (not std::shuffle,
// whose results differ between standard libraries).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ColorModel
{
	enum class Pattern : std::uint8_t
	{
		Increment = 0, // list order
		Decrement = 1, // reverse list order
		Random = 2,    // a shuffle: every colour once per pass
	};

	// Where a cycle restarts after the last colour of a pass.
	enum class Repeat : std::uint8_t
	{
		Beginning = 0, // the list's first entry
		End = 1,       // the list's last entry
		Random = 2,    // a hashed entry, new at every restart
		Center = 3,    // the middle entry; an even count rounds down
		First = 4,     // the first colour the cycle ever produced
		Last = 5,      // the last colour the cycle produced
	};

	// Who shares one place in a cycle.
	enum class Scope : std::uint8_t
	{
		House = 0,  // each player has their own place (the default)
		Game = 1,   // one place for the whole game
		Source = 2, // each factory, building or superweapon has its own
	};

	struct CycleDef
	{
		int count = 0; // entries in RemapColors=
		Pattern pattern = Pattern::Increment;
		Repeat repeat = Repeat::Beginning;
		std::uint32_t seed = 0; // rule name + scope key, see Seed()
	};

	// One place in one cycle. Plain data so it can be saved as is.
	struct CycleState
	{
		std::int32_t pass = 0;   // passes completed before the current one
		std::int32_t idx = 0;    // colours already taken from the current pass
		std::int32_t start = -1; // entry the current pass started at (pass >= 1)
		std::int32_t first = -1; // first entry the cycle produced
		std::int32_t last = -1;  // last entry the cycle produced
	};

	// The entries of pass `pass`, in the order they are handed out. Pass 0
	// starts where the pattern starts (`start` is ignored); a later pass
	// starts at `start`. Increment runs start..count-1, Decrement start..0,
	// and Random is a full shuffle with `start` moved to the front.
	std::vector<int> PassOrder(const CycleDef& def, int pass, int start);

	// The entry a new pass starts at. `st.pass` is the pass being started.
	int RestartPoint(const CycleDef& def, const CycleState& st);

	// Takes the next entry (a list index) and advances `st`. -1 if the list
	// is empty.
	int Next(const CycleDef& def, CycleState& st);

	// All places of one rule. Keys are the scope's deterministic key: a house
	// index, 0 for the whole game, or a source's ordinal.
	struct CycleBook
	{
		std::map<std::int64_t, CycleState> places;
		// Source scope: first-seen order of source identities. Identities
		// (object IDs) may differ between clients; the order they appear in
		// does not, so the ordinal is what keys and seeds the cycle.
		std::map<std::uint64_t, std::int32_t> ordinals;

		std::int64_t SourceKey(std::uint64_t identity);
		int Take(const CycleDef& def, std::int64_t key);
	};

	// --- hashing -----------------------------------------------------------

	std::uint32_t Mix(std::uint32_t a, std::uint32_t b);
	std::uint32_t HashName(const char* s); // FNV-1a, case-insensitive
	std::uint32_t Seed(const char* ruleName, Scope scope, std::int64_t key);

	// --- precedence --------------------------------------------------------

	// What one rule can decide. Rules are ranked highest first: an inherited
	// look, then the creation source (superweapon, factory, free unit), then
	// the object's own type.
	struct RuleSpec
	{
		bool remap = false;   // has RemapColors=
		bool palette = false; // has PaletteSwitch=
		bool replace = false; // Replace=yes: decides every channel it reaches
	};

	// For each channel, the index of the deciding rule, or -1 when no rule
	// decides it (the object looks vanilla on that channel). A rule decides a
	// channel it has, if no higher rule did. A Replace=yes rule also decides
	// the channels it lacks, as "the vanilla look", and nothing below it is
	// consulted. The caller checks the deciding rule's spec to tell the two.
	struct Plan
	{
		int remap = -1;
		int palette = -1;
	};

	Plan Decide(const std::vector<RuleSpec>& ranked);

	// --- INI value parsing (exposed for the tests) -------------------------

	bool ParsePattern(const char* s, Pattern& out);
	bool ParseRepeat(const char* s, Repeat& out);
	bool ParseScope(const char* s, Scope& out);
	// Comma-separated list, items trimmed, empties dropped.
	std::vector<std::string> SplitList(const char* s);
	bool EqualsNoCase(const char* a, const char* b);
}
