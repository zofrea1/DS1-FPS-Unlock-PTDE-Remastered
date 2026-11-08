#pragma once

// Diagnostic (BulletTrace): logs the dragons' acid and the pool it leaves, to find why the pool does not show at
// 60 FPS and above (the Valley of Drakes undead dragon, c5260, and the Gaping Dragon, c5290).
//
// Bullet 52600 (the undead dragon's acid; the Gaping Dragon's 52900 and 52920 are its kin) lives 0.28 s, falls with
// gravity 100, flies through the map and characters (isPenetrateMap, isPenetrate), and leaves HitBullet 52601 (the
// pool: life 6 s, its hit radius spreading from 0.1 to 20 over 5 s) where it is when its life ends. Its hits never
// make the pool (0x1005C90: a bullet that goes through what it hit makes no hit bullet), so the pool is where the
// acid ends. Each line here shows one update of such a bullet (BulletIns::update, 0xDAC240): the frame time the
// game passed it, its life left, its position and velocity, and its state; the player's position (whose feet are on
// the floor nearby) on the first line; and where its hit bullet is made (0xDA9600).
bool bullettrace_install();
