// Host-compiled unit tests for src/Color/ColorModel.cpp (no game needed).
//   tests/run_tests.sh   -> builds with g++ and runs; exit 0 = all passed.

#include "../src/Color/ColorModel.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

using namespace ColorModel;

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

#define CHECK(x) Check((x), #x, __LINE__)

static CycleDef Def(int count, Pattern pattern, Repeat repeat, std::uint32_t seed = 12345u)
{
	CycleDef d;
	d.count = count;
	d.pattern = pattern;
	d.repeat = repeat;
	d.seed = seed;
	return d;
}

static std::vector<int> Take(const CycleDef& def, int n)
{
	CycleState st;
	std::vector<int> out;
	for (int i = 0; i < n; ++i)
		out.push_back(Next(def, st));
	return out;
}

static bool IsPermutation(std::vector<int> v, int n)
{
	if (static_cast<int>(v.size()) != n)
		return false;
	std::sort(v.begin(), v.end());
	for (int i = 0; i < n; ++i)
		if (v[static_cast<size_t>(i)] != i)
			return false;
	return true;
}

int main()
{
	// --- Increment: the first colour is the list's first ----------------------
	{
		CHECK((Take(Def(3, Pattern::Increment, Repeat::Beginning), 7) == std::vector<int> { 0, 1, 2, 0, 1, 2, 0 }));
		// Restarting at the end keeps the last colour for good (by definition).
		CHECK((Take(Def(3, Pattern::Increment, Repeat::End), 6) == std::vector<int> { 0, 1, 2, 2, 2, 2 }));
		// Center of an even count rounds down: entry 1 of 0..3.
		CHECK((Take(Def(4, Pattern::Increment, Repeat::Center), 10) == std::vector<int> { 0, 1, 2, 3, 1, 2, 3, 1, 2, 3 }));
		CHECK((Take(Def(5, Pattern::Increment, Repeat::Center), 11) == std::vector<int> { 0, 1, 2, 3, 4, 2, 3, 4, 2, 3, 4 }));
		// First / Last are the colours actually produced: under Increment
		// those are the list's first and last entries.
		CHECK(Take(Def(3, Pattern::Increment, Repeat::First), 9) == Take(Def(3, Pattern::Increment, Repeat::Beginning), 9));
		CHECK(Take(Def(3, Pattern::Increment, Repeat::Last), 9) == Take(Def(3, Pattern::Increment, Repeat::End), 9));
	}

	// --- Decrement: the first colour is the list's last -----------------------
	{
		CHECK((Take(Def(3, Pattern::Decrement, Repeat::End), 7) == std::vector<int> { 2, 1, 0, 2, 1, 0, 2 }));
		// Restarting at the beginning keeps the first colour for good.
		CHECK((Take(Def(3, Pattern::Decrement, Repeat::Beginning), 6) == std::vector<int> { 2, 1, 0, 0, 0, 0 }));
		CHECK((Take(Def(4, Pattern::Decrement, Repeat::Center), 8) == std::vector<int> { 3, 2, 1, 0, 1, 0, 1, 0 }));
		CHECK(Take(Def(4, Pattern::Decrement, Repeat::First), 12) == Take(Def(4, Pattern::Decrement, Repeat::End), 12));
		CHECK(Take(Def(4, Pattern::Decrement, Repeat::Last), 12) == Take(Def(4, Pattern::Decrement, Repeat::Beginning), 12));
	}

	// --- Repeat=Random under Increment: each restart runs start..end ----------
	{
		const auto def = Def(5, Pattern::Increment, Repeat::Random);
		CycleState st;
		for (int i = 0; i < 5; ++i)
			CHECK(Next(def, st) == i);
		std::set<int> starts;
		for (int pass = 1; pass <= 40; ++pass)
		{
			const int first = Next(def, st);
			CHECK(first >= 0 && first < 5);
			CHECK(st.start == first);
			starts.insert(first);
			for (int e = first + 1; e < 5; ++e)
				CHECK(Next(def, st) == e);
		}
		CHECK(starts.size() >= 3); // the restart point really varies
	}

	// --- Random is a shuffle: every colour once per pass -----------------------
	{
		const int n = 6;
		for (auto repeat : { Repeat::Beginning, Repeat::End, Repeat::Random, Repeat::Center, Repeat::First, Repeat::Last })
		{
			const auto def = Def(n, Pattern::Random, repeat);
			CycleState st;
			std::vector<int> firstPass;
			int previousLast = -1;
			for (int pass = 0; pass < 12; ++pass)
			{
				std::vector<int> got;
				for (int i = 0; i < n; ++i)
					got.push_back(Next(def, st));
				CHECK(IsPermutation(got, n));
				if (pass == 0)
				{
					firstPass = got;
				}
				else
				{
					// Where the pass restarts, per Repeat=.
					if (repeat == Repeat::Beginning)
						CHECK(got[0] == 0);
					if (repeat == Repeat::End)
						CHECK(got[0] == n - 1);
					if (repeat == Repeat::Center)
						CHECK(got[0] == (n - 1) / 2);
					if (repeat == Repeat::First)
						CHECK(got[0] == firstPass[0]);
					if (repeat == Repeat::Last)
						CHECK(got[0] == previousLast);
				}
				previousLast = got.back();
			}
		}

		// First and Last are what the shuffle produced, which is why they
		// differ from Beginning and End under Random (Rex's note in the spec).
		bool firstDiffers = false;
		for (std::uint32_t seed = 1; seed < 40 && !firstDiffers; ++seed)
			firstDiffers = Take(Def(5, Pattern::Random, Repeat::Beginning, seed), 1)[0] != 0;
		CHECK(firstDiffers);

		// Deterministic: the same seed gives the same colours, on any client.
		CHECK(Take(Def(7, Pattern::Random, Repeat::Random, 99u), 50) == Take(Def(7, Pattern::Random, Repeat::Random, 99u), 50));
		CHECK(Take(Def(7, Pattern::Random, Repeat::Random, 99u), 50) != Take(Def(7, Pattern::Random, Repeat::Random, 100u), 50));
		// Different passes shuffle differently.
		const auto def = Def(8, Pattern::Random, Repeat::Random, 7u);
		CHECK(PassOrder(def, 1, 3) != PassOrder(def, 2, 3));
		CHECK(PassOrder(def, 1, 3)[0] == 3);
		CHECK(IsPermutation(PassOrder(def, 1, 3), 8));
	}

	// --- edge counts ------------------------------------------------------------
	{
		CHECK((Take(Def(1, Pattern::Increment, Repeat::Center), 4) == std::vector<int> { 0, 0, 0, 0 }));
		CHECK((Take(Def(1, Pattern::Random, Repeat::Random), 4) == std::vector<int> { 0, 0, 0, 0 }));
		CycleState st;
		CHECK(Next(Def(0, Pattern::Increment, Repeat::Beginning), st) == -1);
		CHECK(st.first == -1);
	}

	// --- the default scope: each player has their own place ------------------
	{
		const auto def = Def(3, Pattern::Increment, Repeat::Beginning);
		CycleBook book;
		// House 0 takes two, then house 1 starts at the first colour.
		CHECK(book.Take(def, 0) == 0);
		CHECK(book.Take(def, 0) == 1);
		CHECK(book.Take(def, 1) == 0);
		CHECK(book.Take(def, 0) == 2);
		CHECK(book.Take(def, 1) == 1);
		CHECK(book.Take(def, 0) == 0); // continuous: it restarts, never stops
	}

	// --- source ordinals: first-seen order, stable --------------------------
	{
		CycleBook book;
		CHECK(book.SourceKey(0x55550000ull) == 0);
		CHECK(book.SourceKey(0x11110000ull) == 1);
		CHECK(book.SourceKey(0x55550000ull) == 0);
		CHECK(book.SourceKey(0x99990000ull) == 2);
	}

	// --- seeds -------------------------------------------------------------------
	{
		CHECK(HashName("Color_Cycle01") == HashName("COLOR_CYCLE01"));
		CHECK(HashName("Color_Cycle01") != HashName("Color_Cycle02"));
		CHECK(Seed("Color_Cycle01", Scope::House, 1) != Seed("Color_Cycle01", Scope::House, 2));
		CHECK(Seed("Color_Cycle01", Scope::House, 1) != Seed("Color_Cycle01", Scope::Source, 1));
		CHECK(Seed("A", Scope::Game, 0) == Seed("a", Scope::Game, 0));
	}

	// --- precedence: which rule decides which channel ------------------------
	{
		RuleSpec remap { true, false, false };
		RuleSpec palette { false, true, false };
		RuleSpec both { true, true, false };
		RuleSpec nothing { false, false, false };
		RuleSpec replacePalette { false, true, true };
		RuleSpec replaceNothing { false, false, true };
		RuleSpec inherit { true, true, true };

		// No rules: vanilla.
		Plan p = Decide({});
		CHECK(p.remap == -1 && p.palette == -1);

		// A type rule alone decides what it has.
		p = Decide({ both });
		CHECK(p.remap == 0 && p.palette == 0);
		p = Decide({ nothing });
		CHECK(p.remap == -1 && p.palette == -1);

		// Source (rank 0) over type (rank 1), per channel: the two channels
		// are independent, so a palette from the type survives a remap from
		// the source.
		p = Decide({ remap, both });
		CHECK(p.remap == 0 && p.palette == 1);
		p = Decide({ palette, remap });
		CHECK(p.remap == 1 && p.palette == 0);
		p = Decide({ nothing, both });
		CHECK(p.remap == 1 && p.palette == 1);

		// Replace=yes replaces in full: the type's remap is not consulted.
		p = Decide({ replacePalette, both });
		CHECK(p.remap == 0 && p.palette == 0);
		// A Replace=yes rule with nothing in it resets to vanilla.
		p = Decide({ replaceNothing, both });
		CHECK(p.remap == 0 && p.palette == 0);
		// A higher rule still wins over a lower Replace=yes.
		p = Decide({ remap, replacePalette, both });
		CHECK(p.remap == 0 && p.palette == 1);

		// An inherited look (opt-in tags) beats source and type.
		p = Decide({ inherit, both, both });
		CHECK(p.remap == 0 && p.palette == 0);
	}

	// --- parsing --------------------------------------------------------------
	{
		Pattern pa {};
		CHECK(ParsePattern(" decrement ", pa) && pa == Pattern::Decrement);
		CHECK(ParsePattern("RANDOM", pa) && pa == Pattern::Random);
		CHECK(!ParsePattern("Shuffle", pa));

		Repeat re {};
		CHECK(ParseRepeat("center", re) && re == Repeat::Center);
		CHECK(ParseRepeat("Centre", re) && re == Repeat::Center);
		CHECK(ParseRepeat("Last", re) && re == Repeat::Last);
		CHECK(!ParseRepeat("Middle", re));

		Scope sc {};
		CHECK(ParseScope("Game", sc) && sc == Scope::Game);
		CHECK(ParseScope("player", sc) && sc == Scope::House);
		CHECK(!ParseScope("Team", sc));

		CHECK((SplitList("DarkRed, LightBlue ,Yellow") == std::vector<std::string> { "DarkRed", "LightBlue", "Yellow" }));
		CHECK((SplitList(" Owner ") == std::vector<std::string> { "Owner" }));
		CHECK((SplitList("a,,b,") == std::vector<std::string> { "a", "b" }));
		CHECK(SplitList("").empty());
		CHECK(EqualsNoCase("Invoker", "INVOKER"));
		CHECK(!EqualsNoCase("Invoke", "Invoker"));
	}

	std::printf("%d/%d color model checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
