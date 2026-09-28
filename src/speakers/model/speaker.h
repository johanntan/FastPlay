#pragma once
#include <string>
#include <vector>

namespace speakers {

// A speaker is described by what you would read off the box: how big the cone
// is, what it is mounted in, how loud it plays and how much power it takes.
// Everything the audio engine needs -- corner frequencies, slopes, the point
// where the cone starts to beam, the SPL ceiling -- is derived from those by
// Derive(), so the catalog stays short and a user-edited speaker stays honest.

enum class Enclosure {
    Sealed,          // a proper sealed box: 12 dB/oct below the corner
    Ported,          // vented/bass-reflex: tuned lower, 24 dB/oct below tuning
    InfiniteBaffle,  // a door or a baffle wall: no box control, leaky and higher
    Horn,            // loud and efficient, but cuts off hard below the flare
};

enum class SpeakerKind {
    FullRange,  // one driver doing everything
    Coaxial,    // woofer with a tweeter on the same axis
    Component,  // woofer with a separately mounted tweeter
    Midbass,    // no tweeter; hands the top end to something else
    Subwoofer,  // bass only
    Center,     // a horizontal home-theatre centre channel
};

// Which part of the source material a speaker reproduces.
enum class ChannelFeed {
    Left,
    Right,
    Mono,  // (L+R)/2 -- the normal feed for a subwoofer or a centre channel
};

const char *EnclosureName(Enclosure e);
const char *SpeakerKindName(SpeakerKind k);
const char *ChannelFeedName(ChannelFeed c);
bool ParseEnclosure(const std::string &s, Enclosure &out);
bool ParseSpeakerKind(const std::string &s, SpeakerKind &out);
bool ParseChannelFeed(const std::string &s, ChannelFeed &out);

struct SpeakerSpec {
    // ---- identity -------------------------------------------------------
    std::string id;           // stable catalog key, e.g. "car_sub_12_ported"
    std::string name;         // "12 inch subwoofer, ported"
    std::string description;  // one line, spoken when browsing the catalog

    // ---- what you would read off the box --------------------------------
    float coneInches = 6.5f;                       // nominal driver diameter
    Enclosure enclosure = Enclosure::Sealed;
    SpeakerKind kind = SpeakerKind::Coaxial;
    float sensitivityDb = 0.0f;                    // dB SPL at 1 W / 1 m; 0 = derive
    float powerWatts = 0.0f;                       // rated continuous power; 0 = derive
    float boxLitres = 0.0f;                        // 0 = derive from cone size
    float portTuningHz = 0.0f;                     // ported only; 0 = derive

    // ---- Thiele-Small (Derive() fills anything left at 0) ---------------
    // The electro-mechanical description of the driver, derived from the
    // physical one like everything else here. It is not decoration: it is what
    // says how far the cone has to move to make a given sound, and therefore
    // when it stops being able to. A cone has to move four times as far for
    // the same loudness an octave down, which is why a subwoofer distorts at
    // thirty hertz and is clean at ninety, and no amount of filtering can
    // express that because it is not a matter of frequency response.
    float fsHz = 0.0f;       // free air resonance
    float qts = 0.0f;        // total Q at that resonance
    float vasLitres = 0.0f;  // compliance, as the volume of air with the same springiness
    float sdCm2 = 0.0f;      // effective radiating area -- the surround only half counts
    float xmaxMm = 0.0f;     // how far the coil can leave the gap and still be linear

    // ---- derived response (Derive() fills anything left at 0) -----------
    float lowCornerHz = 0.0f;   // -3 dB point at the bottom
    int lowOrder = 0;           // 2 = 12 dB/oct, 4 = 24 dB/oct
    float lowQ = 0.0f;          // resonant bump at the corner; >0.707 = boomy
    float topHz = 0.0f;         // -3 dB point at the top
    int topOrder = 0;
    float beamingHz = 0.0f;     // above this the cone narrows off axis

    // Default feed when this spec is placed without an explicit choice.
    ChannelFeed defaultFeed = ChannelFeed::Left;

    // Fills every derived field that is still zero, from the physical ones.
    // Safe to call repeatedly; it never overwrites a value you set yourself.
    void Derive();

    // Peak SPL at 1 m on full power, the ceiling the engine drives the
    // amplifier model against.
    float MaxSplDb() const;
};

// The built-in catalog. Every entry is already Derive()d.
const std::vector<SpeakerSpec> &SpeakerCatalog();

// Looks a spec up by id. Returns nullptr when there is no such id.
const SpeakerSpec *FindSpeakerSpec(const std::string &id);

}  // namespace speakers
