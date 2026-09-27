#include "hrtf.h"
#include "reverb/dsp.h"
#include <cmath>
#include <complex>
#include <cstring>
#include <algorithm>
#include <atomic>

namespace fastplay::hrtf_data {
extern const int hrtf_taps;
extern const int hrtf_sample_rate;
extern const int hrtf_ring_total;
extern const int hrtf_ring_elevation[];
extern const int hrtf_ring_count[];
extern const int hrtf_ring_offset[];
extern const int hrtf_hrir_total;
extern const float hrtf_hrir_scale;
extern const float hrtf_delay[][2];
extern const int16_t hrtf_hrir[][2][128];
}

namespace fastplay::audio {

namespace {

// Windowed-sinc resampling of a short impulse response. Quality matters more
// than speed here: this runs once at start-up for 1588 tiny filters.
void resample_ir(const float* in, int in_len, double in_rate, float* out, int out_len, double out_rate) {
    const double pi = 3.14159265358979323846;
    const double ratio = in_rate / out_rate;              // input samples per output sample
    const int half = 24;                                  // kernel half width (input samples)
    const double cutoff = ratio < 1.0 ? 1.0 : 1.0 / ratio; // relative to input Nyquist
    for (int i = 0; i < out_len; ++i) {
        const double centre = i * ratio;
        const int k0 = static_cast<int>(std::floor(centre)) - half;
        const int k1 = k0 + 2 * half + 1;
        double acc = 0.0;
        for (int k = k0; k <= k1; ++k) {
            if (k < 0 || k >= in_len) continue;
            const double x = centre - k;
            const double px = pi * x;
            const double sinc = std::fabs(x) < 1e-9 ? cutoff : std::sin(px * cutoff) / px;
            const double w = 0.42 + 0.5 * std::cos(pi * x / (half + 1)) + 0.08 * std::cos(2.0 * pi * x / (half + 1));
            acc += in[k] * sinc * w;
        }
        // Sampling the response at a different rate scales its frequency response by the rate ratio; undo that
        // so a filter sounds equally loud at every output rate.
        out[i] = static_cast<float>(acc * ratio);
    }
}

// In place radix-2 complex FFT (size a power of two); inverse unscaled. Start-up only.
void fft_complex(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * 3.14159265358979323846 / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// The flat version of an impulse response: its spectrum at unit magnitude, phase kept, trimmed back to the
// response's length (the part that spills past it is small and faded out).
void flatten_ir(const float* ir, int len, float* out) {
    size_t n = 1;
    while (n < static_cast<size_t>(len) * 4) n <<= 1;
    std::vector<std::complex<double>> a(n);
    for (int i = 0; i < len; ++i) a[i] = ir[i];
    fft_complex(a, false);
    double peak = 0.0;
    for (const auto& c : a) peak = std::max(peak, std::abs(c));
    const double floor_mag = peak * 1e-4;
    for (auto& c : a) {
        const double m = std::abs(c);
        c = m > floor_mag ? c / m : std::complex<double>(1.0);
    }
    fft_complex(a, true);
    const int fade = std::min(16, len / 4);
    for (int i = 0; i < len; ++i) {
        double v = a[i].real() / static_cast<double>(n);
        if (i >= len - fade) v *= 0.5 + 0.5 * std::cos(3.14159265358979323846 * (i - (len - fade) + 1) / (fade + 1));
        out[i] = static_cast<float>(v);
    }
}

} // namespace

std::shared_ptr<const HrtfSet> hrtf_builtin_set() {
    using namespace hrtf_data;
    auto set = std::make_shared<HrtfSet>();
    set->name = "SADIE II KU100";
    set->sample_rate = hrtf_sample_rate;
    set->taps = hrtf_taps;
    for (int r = 0; r < hrtf_ring_total; ++r) {
        set->ring_elevation.push_back(static_cast<float>(hrtf_ring_elevation[r]));
        set->ring_count.push_back(hrtf_ring_count[r]);
        set->ring_offset.push_back(hrtf_ring_offset[r]);
    }
    set->ir.resize(static_cast<size_t>(hrtf_hrir_total) * 2 * hrtf_taps);
    set->delay.resize(static_cast<size_t>(hrtf_hrir_total) * 2);
    for (int idx = 0; idx < hrtf_hrir_total; ++idx) {
        for (int ear = 0; ear < 2; ++ear) {
            // The table is already normalised: a frontal source has unit energy per ear at 48 kHz.
            float* out = set->ir.data() + (static_cast<size_t>(idx) * 2 + ear) * hrtf_taps;
            for (int t = 0; t < hrtf_taps; ++t) out[t] = hrtf_hrir[idx][ear][t] * hrtf_hrir_scale;
            set->delay[static_cast<size_t>(idx) * 2 + ear] = hrtf_delay[idx][ear];
        }
    }
    return set;
}

bool HrtfDatabase::init(std::shared_ptr<const HrtfSet> set, int sample_rate) {
    static std::atomic<uint32_t> next_id{0};
    ready_ = false;
    if (!set || set->count() <= 0 || set->taps <= 0 || set->sample_rate <= 0 || sample_rate <= 0) return false;
    set_ = std::move(set);
    const HrtfSet& s = *set_;
    id_ = ++next_id;
    sample_rate_ = sample_rate;
    count_ = s.count();
    whole_sample_delays_ = s.whole_sample_delays;
    ring_elevation_ = s.ring_elevation;
    ring_count_ = s.ring_count;
    ring_offset_ = s.ring_offset;
    const double ratio = static_cast<double>(sample_rate) / s.sample_rate;
    ir_length_ = static_cast<int>(std::ceil(s.taps * ratio));
    const bool trimmed = ir_length_ > kHrtfMaxPartitions * kHrtfBlock;
    if (trimmed) ir_length_ = kHrtfMaxPartitions * kHrtfBlock;
    partitions_ = (ir_length_ + kHrtfBlock - 1) / kHrtfBlock;
    RealFft fft(kHrtfFftSize);
    spectra_.allocate(static_cast<size_t>(count_) * partitions_ * 2 * kHrtfFftSize);
    flat_.allocate(static_cast<size_t>(count_) * partitions_ * 2 * kHrtfFftSize);
    delays_.assign(static_cast<size_t>(count_) * 2, 0.0f);

    std::vector<float> ir(static_cast<size_t>(partitions_) * kHrtfBlock), flat(ir.size());
    AlignedFloats block(kHrtfFftSize);
    for (int idx = 0; idx < count_; ++idx) {
        for (int ear = 0; ear < 2; ++ear) {
            const float* src = s.response(idx, ear);
            std::fill(ir.begin(), ir.end(), 0.0f);
            if (sample_rate == s.sample_rate) std::copy(src, src + std::min(s.taps, ir_length_), ir.begin());
            else resample_ir(src, s.taps, s.sample_rate, ir.data(), ir_length_, sample_rate);
            if (trimmed) {
                // A response longer than the convolver takes loses its tail; fade the cut so it does not ring.
                const int fade = 32;
                for (int i = 0; i < fade; ++i) ir[static_cast<size_t>(ir_length_ - fade + i)] *= 0.5f + 0.5f * std::cos(3.14159265f * (i + 1) / (fade + 1));
            }
            flatten_ir(ir.data(), static_cast<int>(ir.size()), flat.data());
            for (int p = 0; p < partitions_; ++p) {
                const size_t at = ((static_cast<size_t>(idx) * partitions_ + p) * 2 + ear) * kHrtfFftSize;
                block.zero();
                std::memcpy(block.data(), ir.data() + static_cast<size_t>(p) * kHrtfBlock, kHrtfBlock * sizeof(float));
                fft.forward(block.data(), spectra_.data() + at);
                block.zero();
                std::memcpy(block.data(), flat.data() + static_cast<size_t>(p) * kHrtfBlock, kHrtfBlock * sizeof(float));
                fft.forward(block.data(), flat_.data() + at);
            }
            const float d = static_cast<float>(s.delay[static_cast<size_t>(idx) * 2 + ear] * ratio);
            delays_[static_cast<size_t>(idx) * 2 + ear] = dsp::clampf(d, 0.0f, static_cast<float>(kHrtfMaxDelay));
        }
    }
    ready_ = true;
    return true;
}

HrtfDatabase::Lookup HrtfDatabase::lookup(float azimuth_deg, float elevation_deg) const {
    float az = std::fmod(azimuth_deg, 360.0f);
    if (az < 0.0f) az += 360.0f;
    const float el = dsp::clampf(elevation_deg, -90.0f, 90.0f);
    const int rings = static_cast<int>(ring_elevation_.size());
    Lookup out;
    if (rings == 1) {
        const int c = ring_count_[0];
        out.index = c <= 1 ? 0 : static_cast<int>(std::lround(az * c / 360.0f)) % c;
        for (int ear = 0; ear < 2; ++ear) {
            const float d = delays_[static_cast<size_t>(out.index) * 2 + ear];
            out.delay[ear] = whole_sample_delays_ ? std::round(d) : d;
        }
        return out;
    }
    // The two rings around the source (the table runs from the south pole to the north pole).
    int r0 = 0;
    while (r0 < rings - 2 && ring_elevation_[static_cast<size_t>(r0) + 1] <= el) ++r0;
    const int r1 = r0 + 1;
    const float e0 = ring_elevation_[static_cast<size_t>(r0)], e1 = ring_elevation_[static_cast<size_t>(r1)];
    const float t = dsp::clampf((el - e0) / (e1 - e0), 0.0f, 1.0f);

    // Spectrum: the nearest direction.
    const int rn = t < 0.5f ? r0 : r1;
    const int cn = ring_count_[static_cast<size_t>(rn)];
    out.index = ring_offset_[static_cast<size_t>(rn)] + (cn <= 1 ? 0 : static_cast<int>(std::lround(az * cn / 360.0f)) % cn);
    // Delay: bilinear between the directions around the source.
    for (int ear = 0; ear < 2; ++ear) {
        float d = 0.0f;
        for (int k = 0; k < 2; ++k) {
            const int r = k == 0 ? r0 : r1;
            const float wr = k == 0 ? 1.0f - t : t;
            if (wr <= 0.0f) continue;
            const int c = ring_count_[static_cast<size_t>(r)], off = ring_offset_[static_cast<size_t>(r)];
            if (c <= 1) { d += wr * delays_[static_cast<size_t>(off) * 2 + ear]; continue; }
            const float a = az * c / 360.0f;
            const int i0 = static_cast<int>(std::floor(a)) % c, i1 = (i0 + 1) % c;
            const float f = a - std::floor(a);
            d += wr * ((1.0f - f) * delays_[static_cast<size_t>(off + i0) * 2 + ear] + f * delays_[static_cast<size_t>(off + i1) * 2 + ear]);
        }
        out.delay[ear] = whole_sample_delays_ ? std::round(d) : d;
    }
    return out;
}

void HrtfRenderer::init() {
    fft_.init(kHrtfFftSize);
    for (int e = 0; e < 2; ++e) {
        steady_[e].allocate(kHrtfFftSize);
        old_[e].allocate(kHrtfFftSize);
        fresh_[e].allocate(kHrtfFftSize);
    }
    time_in_.allocate(kHrtfFftSize);
    time_out_.allocate(kHrtfFftSize);
}

void HrtfRenderer::begin_block() {
    voices_ = 0;
    changed_ = false;
    for (int e = 0; e < 2; ++e) { steady_[e].zero(); old_[e].zero(); fresh_[e].zero(); }
}

void HrtfRenderer::render_voice(HrtfVoiceState& v, const float* in, const HrtfDatabase& db, const HrtfDatabase* previous,
                                const HrtfDatabase::Lookup& target, float blend) {
    if (!db.ready()) return;
    RealFft& fft = fft_;
    const float scale = 1.0f / kHrtfFftSize; // folds the inverse FFT normalisation into the MAC
    if (v.filter < 0) { v.delay[0] = target.delay[0]; v.delay[1] = target.delay[1]; }
    // The set the voice's current filter belongs to: this one, or the one it replaced (crossfaded out below).
    // A voice that sat out two changes of set (paused) has lost its filter and starts on the new one.
    const HrtfDatabase* was = nullptr;
    if (v.filter >= 0) {
        if (v.filter_set == db.id()) was = &db;
        else if (previous && previous->ready() && v.filter_set == previous->id()) was = previous;
    }
    const bool first = was == nullptr;
    blend = dsp::clampf(blend, 0.0f, 1.0f);

    // Delay line: [kHrtfLine samples of history | this block].
    float* line = v.line.data();
    std::memmove(line, line + kHrtfBlock, kHrtfLine * sizeof(float));
    std::memcpy(line + kHrtfLine, in, kHrtfBlock * sizeof(float));
    v.fdl_pos = (v.fdl_pos + 1) % kHrtfMaxPartitions;

    for (int ear = 0; ear < 2; ++ear) {
        // Each ear's delay glides from where the last block left it to this block's target, read with a
        // cubic (Catmull-Rom) interpolator.
        const float lo = static_cast<float>(kHrtfDelayMargin), hi = static_cast<float>(kHrtfMaxDelay + kHrtfDelayMargin);
        const float d0 = dsp::clampf(v.delay[ear] + lo, lo, hi), d1 = dsp::clampf(target.delay[ear] + lo, lo, hi);
        float* out = v.delayed.data();
        for (int i = 0; i < kHrtfBlock; ++i) {
            const float d = d0 + (d1 - d0) * (static_cast<float>(i + 1) / kHrtfBlock);
            const float p = static_cast<float>(kHrtfLine + i) - d;
            const int n = static_cast<int>(std::floor(p));
            const float fr = p - static_cast<float>(n);
            const float y0 = line[n - 1], y1 = line[n], y2 = line[n + 1], y3 = line[n + 2];
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            out[i] = ((c3 * fr + c2) * fr + c1) * fr + y1;
        }
        v.delay[ear] = target.delay[ear];
        // Overlap-save input frame for this ear: [previous block | current block].
        std::memcpy(time_in_.data(), v.history[ear].data(), kHrtfBlock * sizeof(float));
        std::memcpy(time_in_.data() + kHrtfBlock, out, kHrtfBlock * sizeof(float));
        std::memcpy(v.history[ear].data(), out, kHrtfBlock * sizeof(float));
        fft.forward(time_in_.data(), v.fdl[ear].data() + static_cast<size_t>(v.fdl_pos) * kHrtfFftSize);
    }

    // The filter is blend x measured + (1 - blend) x flat.
    auto accumulate = [&](const HrtfDatabase& set, int filter, float share, AlignedFloats* buckets) {
        const int P = set.partitions();
        for (int p = 0; p < P; ++p) {
            const int pos = (v.fdl_pos - p + kHrtfMaxPartitions) % kHrtfMaxPartitions;
            for (int ear = 0; ear < 2; ++ear) {
                const float* x = v.fdl[ear].data() + static_cast<size_t>(pos) * kHrtfFftSize;
                if (share > 0.0f) fft.convolve_accumulate(x, set.spectrum(filter, p, ear), buckets[ear].data(), scale * share);
                if (share < 1.0f) fft.convolve_accumulate(x, set.flat_spectrum(filter, p, ear), buckets[ear].data(), scale * (1.0f - share));
            }
        }
    };

    if (first || (was == &db && v.filter == target.index && v.blend == blend)) {
        accumulate(db, target.index, blend, steady_);
    } else {
        accumulate(*was, v.filter, v.blend, old_);
        accumulate(db, target.index, blend, fresh_);
        changed_ = true;
    }
    v.filter = target.index;
    v.filter_set = db.id();
    v.blend = blend;
    ++voices_;
}

void HrtfRenderer::end_block(float* out_left, float* out_right) {
    if (voices_ == 0) return;
    RealFft& fft = fft_;
    float* outs[2] = { out_left, out_right };
    const float* valid = time_out_.data() + kHrtfBlock; // second half is the valid overlap-save output
    for (int ear = 0; ear < 2; ++ear) {
        float* o = outs[ear];
        fft.inverse(steady_[ear].data(), time_out_.data());
        for (int i = 0; i < kHrtfBlock; ++i) o[i] += valid[i];
        if (changed_) {
            fft.inverse(old_[ear].data(), time_out_.data());
            for (int i = 0; i < kHrtfBlock; ++i) {
                const float w = static_cast<float>(i + 1) / kHrtfBlock;
                o[i] += valid[i] * (1.0f - w);
            }
            fft.inverse(fresh_[ear].data(), time_out_.data());
            for (int i = 0; i < kHrtfBlock; ++i) {
                const float w = static_cast<float>(i + 1) / kHrtfBlock;
                o[i] += valid[i] * w;
            }
        }
    }
}

} // namespace fastplay::audio
