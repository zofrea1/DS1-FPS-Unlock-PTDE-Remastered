#pragma once

// Diagnostic (BulletTrace): logs the dragons' acid. The globs do land at 120 FPS (the ground factor holds); what is
// left is the drawn particle, and a hit that still lands at 30 and 60 but not at 120 (user, 2026-11-07).
// [bullet] step is one particle cluster after the integrator 0xA4D420 returns (the same arrays read on entry never
// moved). [bullet] "on you" / "near you" is a sweep contact on the local player, with the hit entry's shape.
//
// Corrected: the Valley of Drakes undead dragon is c3420 (also in the Painted World); its acid is 34200-34209
// (FollowType 4: falls under gravity 5, hits the map and is put on it by 0xDAA1D0, which reflects it along the
// surface and sets its ground factor +0x50 to 1, gravity off; 0xDA6CD0 multiplies that factor by 0.4 every frame
// and gravity comes back as it fades). Below, as first written: c5390 fires 53901 from two mouth points (animation 3009, behavior 190): six globs each, no
// gravity, 6 m/s speeding up to 16, 3 s of life, through the map (isPenetrateMap) and with no hit bullet. What lands
// and spreads is its hit effect (sfxId_Hit 15391), played where a glob touches the map: the hit system's sweep of
// the glob's hit shape (0xDF1E70, each contact through 0xDF1CB0) reports the map, 0x1025950 passes it, and 0xDAC120
// plays the effect at the contact (0xDA37E0). 53900 is its breath, 53902 a slow cloud. The Gaping Dragon (c5260)
// fires 52600, which leaves its pool (HitBullet 52601) where its 0.28 s of life ends (0xDA9600).
//
// Logged ([bullet] lines): each update of those bullets (the frame time the game passed it, life, position,
// velocity, state, its hit entry's current and previous position, the sweep's two ends), the player's position on
// a bullet's first line, every contact the sweep finds for them, every map contact 0x1025950 sees and every one
// 0xDAC120 takes, and every hit bullet made.
bool bullettrace_install();
