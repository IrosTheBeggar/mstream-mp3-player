#pragma once
#include <cstdint>

// Which character dances on the dance screen. The crab (CrabPose) is the
// default; the stick figure (DancePose) is the first proof of concept.
// Console `m` or a tap on the character's box cycles them.
namespace dance {

enum class Skin : uint8_t { Crab, Stick };

constexpr Skin kDefaultSkin = Skin::Crab;

inline const char* skinName(Skin s) { return s == Skin::Crab ? "crab" : "stick"; }

// The next one in the cycle (crab -> stick -> crab).
inline Skin nextSkin(Skin s) { return s == Skin::Crab ? Skin::Stick : Skin::Crab; }

}  // namespace dance
