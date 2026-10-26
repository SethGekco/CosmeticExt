#pragma once

class CCINIClass;

// Engine glue for the soundmd.ini Distance.* tags. The formulas live in
// DistanceModel; this file owns the hooks, the per-sound tables and the queue
// of deferred (travel-delayed / echoed) sounds.
//
// Everything here is client-local audio. Sound playback in gamemd only ever
// draws from the unsynced Randomizer::Global and is already viewport-dependent
// in vanilla, so playing a sound later, more often, or as a different entry
// cannot desync a multiplayer game.
namespace DistanceSound
{
	// After soundmd.ini's [SoundList] has been fully read: parse [CosmeticExt]
	// and every sound's Distance.* keys, resolve names and inheritance.
	void ReadINI(CCINIClass* pINI);

	// Once per main-loop frame: play deferred sounds whose frame has come.
	void FrameTick();
}
