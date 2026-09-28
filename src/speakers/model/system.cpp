#include "system.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

} // namespace

std::string PlacedSpeaker::Label(const RoomSpec &room) const {
    std::string where;
    if (!mountId.empty()) {
        if (const MountPoint *m = room.FindMount(mountId)) where = m->name;
    }
    if (where.empty()) where = "free placement";
    return where + ", " + spec.name;
}

// ---------------------------------------------------------------------------
// SpeakerSystem
// ---------------------------------------------------------------------------

SpeakerSystem::SpeakerSystem() {
    const std::vector<RoomSpec> &rooms = RoomCatalog();
    m_room = rooms.empty() ? RoomSpec{} : rooms.front();
    if (rooms.empty()) m_room.Derive();
    ResetListener();
}

int SpeakerSystem::SetRoom(const std::string &roomId) {
    const RoomSpec *spec = FindRoomSpec(roomId);
    if (!spec) return -1;
    m_room = *spec;

    // Carry speakers across to the mount of the same name where the new room
    // has one, and drop the rest: a rear deck does not exist in a hall.
    int dropped = 0;
    std::vector<PlacedSpeaker> kept;
    kept.reserve(m_speakers.size());
    for (auto &s : m_speakers) {
        if (s.mountId.empty()) {
            s.position = ClampToRoom(s.position);
            kept.push_back(s);
            continue;
        }
        if (const MountPoint *m = m_room.FindMount(s.mountId)) {
            ApplyMount(s, *m);
            kept.push_back(s);
        } else {
            ++dropped;
        }
    }
    m_speakers.swap(kept);
    ResetListener();
    Touch();
    return dropped;
}

void SpeakerSystem::ApplyMount(PlacedSpeaker &s, const MountPoint &m) {
    s.mountId = m.id;
    s.position = m.position;
    s.aimYawDeg = m.aimYawDeg;
    s.aimPitchDeg = m.aimPitchDeg;
    ResolveTweeter(s);
}

void SpeakerSystem::ResolveTweeter(PlacedSpeaker &s) const {
    s.separateTweeter = false;
    s.tweeterMountId.clear();
    s.tweeterPosition = s.position;
    if (s.spec.kind != SpeakerKind::Component) return;

    const MountPoint *woofer = m_room.FindMount(s.mountId);
    if (!woofer) return;

    // A tweeter mount is one that will only take something tiny. Take the one
    // on the same side of the room as the woofer.
    const MountPoint *best = nullptr;
    for (const auto &m : m_room.mounts) {
        if (m.maxConeInches <= 0.0f || m.maxConeInches > 2.5f) continue;
        if (m.side != woofer->side) continue;
        best = &m;
        break;
    }
    if (!best) return;

    s.separateTweeter = true;
    s.tweeterMountId = best->id;
    s.tweeterPosition = best->position;
}

int SpeakerSystem::AddSpeaker(const std::string &specId, const std::string &mountId,
                              ChannelFeed feed) {
    const SpeakerSpec *spec = FindSpeakerSpec(specId);
    if (!spec) return 0;

    PlacedSpeaker s;
    s.id = m_nextSpeakerId++;
    s.spec = *spec;
    s.spec.Derive();
    s.feed = feed;
    if (const MountPoint *m = m_room.FindMount(mountId)) {
        ApplyMount(s, *m);
    } else {
        // No such mount: drop it in the middle of the room rather than fail,
        // and leave mountId empty so it reads as a free placement.
        s.position = {0.0f, 0.0f, m_room.listenerHeight};
    }
    m_speakers.push_back(s);
    Touch();
    return s.id;
}

int SpeakerSystem::AddSpeaker(const std::string &specId, const std::string &mountId) {
    const SpeakerSpec *spec = FindSpeakerSpec(specId);
    ChannelFeed feed = spec ? spec->defaultFeed : ChannelFeed::Mono;
    // A mount that knows its side overrides the spec default, so dropping a
    // pair spec on the right hand door feeds it the right channel.
    if (const MountPoint *m = m_room.FindMount(mountId)) {
        if (spec && spec->kind != SpeakerKind::Subwoofer && spec->kind != SpeakerKind::Center) {
            if (m->side == MountSide::Left) feed = ChannelFeed::Left;
            else if (m->side == MountSide::Right) feed = ChannelFeed::Right;
            else feed = ChannelFeed::Mono;
        }
    }
    return AddSpeaker(specId, mountId, feed);
}

std::vector<int> SpeakerSystem::AddPair(const std::string &specId, const std::string &mountId) {
    const MountPoint *left = m_room.FindMount(mountId);
    if (!left || left->pairId.empty()) return {};
    const MountPoint *right = m_room.FindMount(left->pairId);
    if (!right) return {};

    // Always feed the left channel to the left hand mount whichever of the
    // pair was named.
    const MountPoint *a = left->side == MountSide::Right ? right : left;
    const MountPoint *b = left->side == MountSide::Right ? left : right;

    int idA = AddSpeaker(specId, a->id, ChannelFeed::Left);
    int idB = AddSpeaker(specId, b->id, ChannelFeed::Right);
    if (!idA || !idB) {
        if (idA) RemoveSpeaker(idA);
        if (idB) RemoveSpeaker(idB);
        return {};
    }
    int pair = m_nextPairId++;
    if (PlacedSpeaker *s = Find(idA)) s->pairId = pair;
    if (PlacedSpeaker *s = Find(idB)) s->pairId = pair;

    // A pair of subs is still a pair of subs: both take the mono sum.
    const SpeakerSpec *spec = FindSpeakerSpec(specId);
    if (spec && (spec->kind == SpeakerKind::Subwoofer || spec->kind == SpeakerKind::Center)) {
        if (PlacedSpeaker *s = Find(idA)) s->feed = ChannelFeed::Mono;
        if (PlacedSpeaker *s = Find(idB)) s->feed = ChannelFeed::Mono;
    }
    Touch();
    return {idA, idB};
}

void SpeakerSystem::AdoptSpeakers(std::vector<PlacedSpeaker> speakers) {
    m_speakers = std::move(speakers);
    // Where the top end comes from is derived from the room, not saved, so a
    // loaded system has to work it out again.
    for (auto &s : m_speakers) ResolveTweeter(s);
    for (const auto &s : m_speakers) {
        if (s.id >= m_nextSpeakerId) m_nextSpeakerId = s.id + 1;
        if (s.pairId >= m_nextPairId) m_nextPairId = s.pairId + 1;
    }
    Touch();
}

bool SpeakerSystem::RemoveSpeaker(int id) {
    for (size_t i = 0; i < m_speakers.size(); ++i) {
        if (m_speakers[i].id == id) {
            m_speakers.erase(m_speakers.begin() + (long)i);
            Touch();
            return true;
        }
    }
    return false;
}

int SpeakerSystem::RemovePair(int id) {
    const PlacedSpeaker *s = Find(id);
    if (!s) return 0;
    if (s->pairId == 0) return RemoveSpeaker(id) ? 1 : 0;

    int pair = s->pairId;
    size_t before = m_speakers.size();
    m_speakers.erase(std::remove_if(m_speakers.begin(), m_speakers.end(),
                                    [pair](const PlacedSpeaker &p) { return p.pairId == pair; }),
                     m_speakers.end());
    Touch();
    return (int)(before - m_speakers.size());
}

PlacedSpeaker *SpeakerSystem::Find(int id) {
    for (auto &s : m_speakers)
        if (s.id == id) return &s;
    return nullptr;
}

const PlacedSpeaker *SpeakerSystem::Find(int id) const {
    for (const auto &s : m_speakers)
        if (s.id == id) return &s;
    return nullptr;
}

bool SpeakerSystem::AnySoloed() const {
    for (const auto &s : m_speakers)
        if (s.soloed) return true;
    return false;
}

bool SpeakerSystem::IsAudible(const PlacedSpeaker &s) const {
    if (s.muted) return false;
    if (AnySoloed()) return s.soloed;
    return true;
}

bool SpeakerSystem::MoveToMount(int id, const std::string &mountId) {
    PlacedSpeaker *s = Find(id);
    const MountPoint *m = m_room.FindMount(mountId);
    if (!s || !m) return false;
    ApplyMount(*s, *m);
    Touch();
    return true;
}

bool SpeakerSystem::MoveToPosition(int id, Vec3 position) {
    PlacedSpeaker *s = Find(id);
    if (!s) return false;
    s->mountId.clear();
    s->position = position;
    Touch();
    return true;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

void SpeakerSystem::ResetListener() {
    m_listener.position = ClampToRoom(m_room.defaultListener);
    m_listener.yawDeg = m_room.defaultYawDeg;
    Touch();
}

Vec3 SpeakerSystem::ClampToRoom(Vec3 p) const {
    float margin = m_room.wallMargin;
    float hx = std::max(0.0f, m_room.width * 0.5f - margin);
    float hy = std::max(0.0f, m_room.depth * 0.5f - margin);
    p.x = std::max(-hx, std::min(hx, p.x));
    p.y = std::max(-hy, std::min(hy, p.y));
    p.z = m_room.listenerHeight;
    return p;
}

void SpeakerSystem::Walk(float forwardMetres, float rightMetres) {
    float yaw = m_listener.yawDeg * kDegToRad;
    // Forward is the way you are facing; right is ninety degrees clockwise.
    Vec3 fwd{std::sin(yaw), std::cos(yaw), 0.0f};
    Vec3 right{std::cos(yaw), -std::sin(yaw), 0.0f};
    Vec3 p = m_listener.position + fwd * forwardMetres + right * rightMetres;
    m_listener.position = ClampToRoom(p);
    Touch();
}

void SpeakerSystem::SetListenerPosition(Vec3 p) {
    m_listener.position = ClampToRoom(p);
    Touch();
}

void SpeakerSystem::Turn(float degrees) { SetYaw(m_listener.yawDeg + degrees); }

void SpeakerSystem::SetYaw(float degrees) {
    while (degrees >= 360.0f) degrees -= 360.0f;
    while (degrees < 0.0f) degrees += 360.0f;
    m_listener.yawDeg = degrees;
    Touch();
}

}  // namespace speakers
