#include "ColorModel.h"

#include <algorithm>
#include <cctype>

namespace ColorModel
{
	namespace
	{
		// A small PRNG with a fixed definition, so the host tests and the
		// game DLL shuffle identically.
		struct Rng
		{
			std::uint32_t state;

			explicit Rng(std::uint32_t seed) : state(seed) { }

			std::uint32_t operator()()
			{
				state += 0x9E3779B9u;
				return Mix(state, 0x85EBCA6Bu);
			}
		};

		int FirstStart(const CycleDef& def)
		{
			return def.pattern == Pattern::Decrement ? def.count - 1 : 0;
		}

		std::string Trim(const char* s)
		{
			while (*s == ' ' || *s == '\t')
				++s;
			std::string v(s);
			while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
				v.pop_back();
			return v;
		}
	}

	std::vector<int> PassOrder(const CycleDef& def, int pass, int start)
	{
		std::vector<int> order;
		const int n = def.count;
		if (n <= 0)
			return order;
		if (pass == 0)
			start = FirstStart(def);
		start = std::clamp(start, 0, n - 1);

		switch (def.pattern)
		{
		case Pattern::Increment:
			for (int i = start; i < n; ++i)
				order.push_back(i);
			break;

		case Pattern::Decrement:
			for (int i = start; i >= 0; --i)
				order.push_back(i);
			break;

		case Pattern::Random:
		{
			order.resize(static_cast<size_t>(n));
			for (int i = 0; i < n; ++i)
				order[static_cast<size_t>(i)] = i;
			Rng rng(Mix(def.seed, static_cast<std::uint32_t>(pass)));
			for (int i = n - 1; i > 0; --i)
				std::swap(order[static_cast<size_t>(i)], order[rng() % static_cast<std::uint32_t>(i + 1)]);
			// The first pass is the plain shuffle. A restart puts its entry
			// first and keeps the shuffled order of the rest.
			if (pass > 0)
			{
				auto const it = std::find(order.begin(), order.end(), start);
				std::rotate(order.begin(), it, it + 1);
			}
			break;
		}
		}
		return order;
	}

	int RestartPoint(const CycleDef& def, const CycleState& st)
	{
		const int n = def.count;
		if (n <= 0)
			return -1;
		switch (def.repeat)
		{
		case Repeat::Beginning:
			return 0;
		case Repeat::End:
			return n - 1;
		case Repeat::Center:
			return (n - 1) / 2;
		case Repeat::Random:
			return static_cast<int>(Mix(def.seed ^ 0x52455354u, static_cast<std::uint32_t>(st.pass))
				% static_cast<std::uint32_t>(n));
		case Repeat::First:
			return st.first >= 0 ? st.first : 0;
		case Repeat::Last:
			return st.last >= 0 ? st.last : n - 1;
		}
		return 0;
	}

	int Next(const CycleDef& def, CycleState& st)
	{
		if (def.count <= 0)
			return -1;

		auto order = PassOrder(def, st.pass, st.start);
		if (st.idx >= static_cast<int>(order.size()))
		{
			++st.pass;
			st.idx = 0;
			st.start = RestartPoint(def, st);
			order = PassOrder(def, st.pass, st.start);
		}

		const int entry = order[static_cast<size_t>(st.idx++)];
		if (st.first < 0)
			st.first = entry;
		st.last = entry;
		return entry;
	}

	std::int64_t CycleBook::SourceKey(std::uint64_t identity)
	{
		auto const it = ordinals.find(identity);
		if (it != ordinals.end())
			return it->second;
		const auto ordinal = static_cast<std::int32_t>(ordinals.size());
		ordinals.emplace(identity, ordinal);
		return ordinal;
	}

	int CycleBook::Take(const CycleDef& def, std::int64_t key)
	{
		return Next(def, places[key]);
	}

	std::uint32_t Mix(std::uint32_t a, std::uint32_t b)
	{
		std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
		h ^= h >> 16;
		h *= 0x85EBCA6Bu;
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 16;
		return h;
	}

	std::uint32_t HashName(const char* s)
	{
		std::uint32_t h = 2166136261u;
		for (; s && *s; ++s)
		{
			h ^= static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(*s)));
			h *= 16777619u;
		}
		return h;
	}

	std::uint32_t Seed(const char* ruleName, Scope scope, std::int64_t key)
	{
		const auto k = static_cast<std::uint64_t>(key);
		std::uint32_t h = Mix(HashName(ruleName), static_cast<std::uint32_t>(scope));
		h = Mix(h, static_cast<std::uint32_t>(k));
		return Mix(h, static_cast<std::uint32_t>(k >> 32));
	}

	Plan Decide(const std::vector<RuleSpec>& ranked)
	{
		Plan plan;
		for (int i = 0; i < static_cast<int>(ranked.size()); ++i)
		{
			auto const& rule = ranked[static_cast<size_t>(i)];
			if (plan.remap < 0 && (rule.remap || rule.replace))
				plan.remap = i;
			if (plan.palette < 0 && (rule.palette || rule.replace))
				plan.palette = i;
			if (rule.replace || (plan.remap >= 0 && plan.palette >= 0))
				break;
		}
		return plan;
	}

	bool EqualsNoCase(const char* a, const char* b)
	{
		for (; *a && *b; ++a, ++b)
		{
			if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
				return false;
		}
		return *a == *b;
	}

	bool ParsePattern(const char* s, Pattern& out)
	{
		const std::string v = Trim(s);
		if (EqualsNoCase(v.c_str(), "Increment"))
			out = Pattern::Increment;
		else if (EqualsNoCase(v.c_str(), "Decrement"))
			out = Pattern::Decrement;
		else if (EqualsNoCase(v.c_str(), "Random"))
			out = Pattern::Random;
		else
			return false;
		return true;
	}

	bool ParseRepeat(const char* s, Repeat& out)
	{
		const std::string v = Trim(s);
		if (EqualsNoCase(v.c_str(), "Beginning"))
			out = Repeat::Beginning;
		else if (EqualsNoCase(v.c_str(), "End"))
			out = Repeat::End;
		else if (EqualsNoCase(v.c_str(), "Random"))
			out = Repeat::Random;
		else if (EqualsNoCase(v.c_str(), "Center") || EqualsNoCase(v.c_str(), "Centre"))
			out = Repeat::Center;
		else if (EqualsNoCase(v.c_str(), "First"))
			out = Repeat::First;
		else if (EqualsNoCase(v.c_str(), "Last"))
			out = Repeat::Last;
		else
			return false;
		return true;
	}

	bool ParseScope(const char* s, Scope& out)
	{
		const std::string v = Trim(s);
		if (EqualsNoCase(v.c_str(), "House") || EqualsNoCase(v.c_str(), "Player"))
			out = Scope::House;
		else if (EqualsNoCase(v.c_str(), "Game"))
			out = Scope::Game;
		else if (EqualsNoCase(v.c_str(), "Source"))
			out = Scope::Source;
		else
			return false;
		return true;
	}

	std::vector<std::string> SplitList(const char* s)
	{
		std::vector<std::string> out;
		std::string cur;
		for (const char* p = s; ; ++p)
		{
			const char c = *p;
			if (c == ',' || c == '\0')
			{
				const std::string item = Trim(cur.c_str());
				if (!item.empty())
					out.push_back(item);
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
}
