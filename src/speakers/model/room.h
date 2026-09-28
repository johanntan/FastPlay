#pragma once
#include <string>
#include <vector>

#include "../core/vec3.h"

namespace speakers {

// A room is the space you walk around in: its size, how dead or live it is,
// and the places a speaker can actually go. Car cabins and living rooms are the same structure with very
// different numbers.

enum class RoomKind {
    Vehicle,  // a cabin: tiny, dead, and enormous cabin gain down low
    Indoor,   // a normal room with walls you hear
    Outdoor,  // open air: no reflections, no cabin gain
};

const char *RoomKindName(RoomKind k);

// Which side of the space a mount point is on. Used to pair mounts up when you
// add a stereo pair, so "door pair" fills the left and right door at once.
enum class MountSide { Left, Right, Center };

// A place a speaker can be installed. Rooms ship with a named set of these so
// that placing a speaker is choosing from a list -- "front left door", "rear
// deck right" -- rather than typing coordinates.
struct MountPoint {
    std::string id;    // stable key, e.g. "door_front_left"
    std::string name;  // "front left door"
    Vec3 position;     // metres, room axes
    float aimYawDeg = 0.0f;    // where the driver faces
    float aimPitchDeg = 0.0f;
    MountSide side = MountSide::Center;
    std::string pairId;        // mount on the opposite side, if any
    float maxConeInches = 0.0f; // biggest driver that physically fits; 0 = no limit
    bool subOnly = false;       // a boot or a sub cavity, not a place for mains
};

struct RoomSpec {
    std::string id;
    std::string name;
    std::string description;
    RoomKind kind = RoomKind::Indoor;

    // Interior dimensions in metres: width (x), depth (y), height (z). The
    // room is centred on the origin in x and y, with the floor at z = 0.
    float width = 5.0f;
    float depth = 4.0f;
    float height = 2.5f;

    // Average absorption coefficient of the surfaces, 0 to 1. 0.05 is bare
    // concrete, 0.2 a furnished room, 0.45 a car interior full of seats.
    float absorption = 0.2f;

    // Small sealed spaces pressurise rather than radiate below a corner
    // frequency, which is why a car has so much bass. Both are derived from
    // the dimensions by Derive() when left at zero.
    float cabinGainHz = 0.0f;
    float cabinGainDb = 0.0f;

    // What the space does to the rest of the bottom end, on the way to your
    // ears. A car is not just a pressure vessel: the doors fire into a small
    // hard box a few feet from your head, and the midbass that comes back is
    // enormous -- far more than the pressure gain above accounts for, and
    // reaching much further up.
    //
    // Measured rather than assumed. See kCabinShape in room.cpp.
    struct CabinBand {
        float hz = 0.0f;
        float q = 0.7f;
        float db = 0.0f;
        bool peak = false; // false = low shelf
    };
    std::vector<CabinBand> cabinShape;

    // Where you start, and which way you are looking.
    Vec3 defaultListener{0.0f, 0.0f, 1.2f};
    float defaultYawDeg = 0.0f;
    // Ear height when standing or seated in this space.
    float listenerHeight = 1.2f;
    // How far from a wall you can get, so walking never puts your head inside
    // the plaster.
    float wallMargin = 0.35f;

    std::vector<MountPoint> mounts;

    // Fills cabin gain and anything else left at zero.
    void Derive();

    // Interior volume in cubic metres and total surface area in square metres.
    float Volume() const;
    float SurfaceArea() const;

    // Sabine reverberation time in seconds, derived from the above.
    float Rt60() const;

    const MountPoint *FindMount(const std::string &id) const;
};

// The built-in rooms, already Derive()d.
const std::vector<RoomSpec> &RoomCatalog();
const RoomSpec *FindRoomSpec(const std::string &id);

}  // namespace speakers
