#pragma once

// Engine glue for the rulesmd.ini ColorRules= tags: per-object remap colour
// and palette overrides. The cycle and precedence rules live in ColorModel;
// this file owns the INI tables, the per-object state, the creation contexts
// (factory, free unit, superweapon, deploy) and the hooks.
//
// Presentation only. Nothing here writes an object, a house or the rules: an
// object's owner, and so its losses, loyalty and AI, never change. The look is
// decided at the moment an object first enters the map, in simulation order,
// and "random" choices are hashed, so every client in an online game draws the
// same colours without touching the synced randomizer.
namespace ColorRules
{
	// Called from DllMain: wraps TechnoClass::GetRemapColour in the four
	// concrete techno vtables (slot 0x1E4). Logged later, at the first rules
	// read, because debug.log is not open yet.
	void PatchVtables();
}
