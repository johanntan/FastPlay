// HRTF renderer.
//
// Data: the built in set is SADIE II, subject D1 (Neumann KU100 dummy head): 794 directions over the full
// sphere, rings every 15 degrees of elevation at 5 degree azimuth steps plus both poles, diffuse-field
// equalised, with each ear's onset removed and kept as a separate delay (see SADIE-README.txt). It is
// resampled to the output rate.
//
// Rendering, per voice:
//   * spectrum: the nearest direction of the set for each ear. Blending neighbours fills in the pinna
//     notches that carry elevation, so the measured response is used as it is; a change of direction
//     is crossfaded over one block (old filter -> new filter), which removes switching clicks.
//   * interaural delay: interpolated bilinearly between the four directions around the source and applied to
//     each ear with a cubic fractional delay line, glided across every block, so left/right movement is
//     continuous rather than stepped.
//   * near the listener: the filter itself is blended toward a flat one (a spatial blend), so the sound
//     loses its colouring but keeps its interaural timing. Each flat filter is its
//     measured one with the magnitude set to one and the phase kept, so the blend only moves the magnitude:
//     blending outputs, or blending toward a plain impulse, sums the sound with a copy offset in time and
//     comb filters it.
//   * convolution: uniformly partitioned in the frequency domain with 128 frame blocks. Each voice
//     costs two 256 point real FFTs (one per ear) plus a few complex multiply-accumulates; the
//     inverse transforms are shared by all voices on a bus.
//   * switching sets: a voice crossfades from its filter in the outgoing set to its filter in the new one over
//     one block, exactly as for a change of direction, so a set can be swapped while sounds play.
#pragma once
#include "fft.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastplay::audio {

constexpr int kHrtfBlock = 128;                  // frames per render block (B)
constexpr int kHrtfFftSize = kHrtfBlock * 2;     // N = 2B (overlap-save)
constexpr int kHrtfMaxPartitions = 2;            // 256 taps at the output rate (longer responses are trimmed)
constexpr int kHrtfMaxDelay = 256;               // longest per ear delay in output samples (data peaks near 52 at 48 kHz)
constexpr int kHrtfDelayMargin = 2;              // added to every delay so the cubic interpolator never reads ahead
constexpr int kHrtfLine = kHrtfMaxDelay + kHrtfDelayMargin + 2;   // input history kept for the delay line

// A data set as measured, at its own rate: rings of directions from the south pole up to the north pole, the
// directions of each ring evenly spaced clockwise from straight ahead (the first at azimuth 0), and for every
// direction both ears' impulse responses (onset removed) and onset delays.
struct HrtfSet {
    std::string name;                    // "SADIE II KU100"
    std::string file;                    // the path it was loaded from; empty for the built in set
    int sample_rate = 0;
    int taps = 0;                        // impulse response length
    std::vector<float> ring_elevation;   // degrees, ascending, -90 .. 90
    std::vector<int> ring_count;         // directions in each ring
    std::vector<int> ring_offset;        // index of each ring's first direction
    std::vector<float> ir;               // direction x ear (left, right) x taps
    std::vector<float> delay;            // direction x ear, samples at sample_rate
    // Render each ear's delay in whole output samples. A static source then
    // hears exactly the set's responses; a fractional delay line softens the top octave a little (about 2 dB at
    // 16 kHz halfway between samples). Moving sources still glide between the whole sample steps.
    bool whole_sample_delays = false;
    int count() const { return ring_offset.empty() ? 0 : ring_offset.back() + ring_count.back(); }
    const float* response(int dir, int ear) const { return ir.data() + (static_cast<size_t>(dir) * 2 + ear) * taps; }
};

// The built in set (SADIE II KU100), built from the embedded table.
std::shared_ptr<const HrtfSet> hrtf_builtin_set();

class HrtfDatabase {
public:
    struct Lookup {
        int index = 0;          // nearest direction (spectrum)
        float delay[2] = {};    // interpolated per ear delay, output samples (left, right)
    };

    // Builds the filters for `set` at the output rate. Read only afterwards.
    bool init(std::shared_ptr<const HrtfSet> set, int sample_rate);
    bool ready() const { return ready_; }
    uint32_t id() const { return id_; }   // unique per init, so a voice can tell which set its filter is from
    const std::shared_ptr<const HrtfSet>& set() const { return set_; }
    int sample_rate() const { return sample_rate_; }
    int partitions() const { return partitions_; }
    int ir_length() const { return ir_length_; }
    int direction_count() const { return count_; }
    bool whole_sample_delays() const { return whole_sample_delays_; }
    size_t memory_bytes() const { return (spectra_.size() + flat_.size()) * sizeof(float); }

    // azimuth: degrees clockwise from straight ahead (90 = right). elevation: degrees, positive up.
    Lookup lookup(float azimuth_deg, float elevation_deg) const;

    // Spectrum of one partition for one ear (0 = left, 1 = right) in pffft internal layout.
    const float* spectrum(int index, int partition, int ear) const {
        return spectra_.data() + ((static_cast<size_t>(index) * partitions_ + partition) * 2 + ear) * kHrtfFftSize;
    }
    // The flat filter for a direction and ear: the measured phase at unit magnitude, same layout.
    const float* flat_spectrum(int index, int partition, int ear) const {
        return flat_.data() + ((static_cast<size_t>(index) * partitions_ + partition) * 2 + ear) * kHrtfFftSize;
    }

private:
    bool ready_ = false;
    uint32_t id_ = 0;
    std::shared_ptr<const HrtfSet> set_;
    int sample_rate_ = 0;
    int partitions_ = 0;
    int ir_length_ = 0;
    int count_ = 0;
    bool whole_sample_delays_ = false;
    std::vector<float> ring_elevation_;
    std::vector<int> ring_count_, ring_offset_;
    AlignedFloats spectra_;
    AlignedFloats flat_;
    std::vector<float> delays_;   // count x 2, output samples
};

// Per voice state. Owned by the voice, reset when it starts.
struct HrtfVoiceState {
    AlignedFloats line;        // mono input: kHrtfLine samples of history, then the current block
    AlignedFloats history[2];  // last B delayed samples per ear (overlap-save)
    AlignedFloats fdl[2];      // frequency delay line per ear: kHrtfMaxPartitions x N spectra
    AlignedFloats delayed;     // scratch: one ear's delayed block
    int fdl_pos = 0;
    int filter = -1;           // current filter index, -1 = none yet
    uint32_t filter_set = 0;   // HrtfDatabase::id() of the set `filter` indexes
    float blend = 1.0f;        // current share of the measured filter (the rest is flat)
    float delay[2] = {};       // delay reached at the end of the last block
    void init() {
        line.allocate(kHrtfLine + kHrtfBlock);
        for (int e = 0; e < 2; ++e) {
            history[e].allocate(kHrtfBlock);
            fdl[e].allocate(static_cast<size_t>(kHrtfMaxPartitions) * kHrtfFftSize);
        }
        delayed.allocate(kHrtfBlock);
        reset();
    }
    void reset() {
        line.zero();
        for (int e = 0; e < 2; ++e) { history[e].zero(); fdl[e].zero(); delay[e] = 0.0f; }
        fdl_pos = 0;
        filter = -1;
        filter_set = 0;
        blend = 1.0f;
    }
};

// Per bus renderer: accumulates all voices of a block, then produces stereo.
class HrtfRenderer {
public:
    void init();
    void begin_block();
    // in: B mono samples (already gain-scaled). db: the current set; previous: the set it replaced (or null),
    // which a voice still on one of its filters crossfades out of. target: direction for this block (looked
    // up in db). blend: 0 (flat, near the listener) .. 1 (the measured filter).
    void render_voice(HrtfVoiceState& v, const float* in, const HrtfDatabase& db, const HrtfDatabase* previous,
                      const HrtfDatabase::Lookup& target, float blend = 1.0f);
    // Adds the rendered stereo block to out_left/out_right (B samples each).
    void end_block(float* out_left, float* out_right);
    bool active() const { return voices_ > 0; }

private:
    RealFft fft_;
    AlignedFloats steady_[2];   // bucket S per ear: voices whose filter did not change
    AlignedFloats old_[2];      // bucket A: outgoing filters of changing voices
    AlignedFloats fresh_[2];    // bucket B: incoming filters of changing voices
    AlignedFloats time_in_;     // scratch: 2B input frame
    AlignedFloats time_out_;    // scratch: N output
    int voices_ = 0;
    bool changed_ = false;
};

} // namespace fastplay::audio
