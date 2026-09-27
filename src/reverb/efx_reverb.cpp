#include "efx_reverb.h"
#include <algorithm>
#include <cmath>

namespace fastplay::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSpeedOfSound = 343.3;          // m/s, for the air absorption limit on the HF decay

// ---- tuning ------------------------------------------------------------------------------------------------
// Times are at density 1; density shrinks them (size_scale). They are chosen here; the generation loss, the
// diffuser gains, the onset and tail balance and the two levels were fitted so decay, smear and level measure
// like OpenAL Soft's rendering of the same settings (tools/efx_reverb_compare.py).

// Early reflections: the first generation of taps (ms after reflections_delay) and the side each leans to.
constexpr float kTapMs[EfxReverb::kTaps] = { 4.1f, 8.9f, 13.3f, 19.7f };
constexpr bool kTapLeft[EfxReverb::kTaps] = { true, false, false, true };
constexpr float kLeanNear = 1.0f, kLeanFar = 0.3f;
// The two later generations: each tap again after this long (ms: a fixed part plus a part that scales with the
// room), mixed 4 x 4. The second generation is weaker by kGenLoss as well as by the decay, the third only by the
// decay, so with a long decay the reflections run on at an even level to about 110 ms (at density 1) and stop.
constexpr float kGenFixedMs = 7.0f;
constexpr float kGenMs[2][EfxReverb::kTaps] = { { 11.9f, 16.7f, 22.9f, 29.8f }, { 28.9f, 35.3f, 41.9f, 48.7f } };
constexpr float kGenLoss = 0.5f;
constexpr float kGenOut[2][2][EfxReverb::kTaps] = {
    { { 1, -1, 1, -1 }, { 1, 1, -1, -1 } },
    { { 1, 1, -1, -1 }, { 1, -1, -1, 1 } } };
// Two allpasses smear each tap when diffusion is up.
constexpr float kTapDiffMs[EfxReverb::kTaps][2] = { { 0.9f, 1.4f }, { 1.1f, 1.7f }, { 1.3f, 2.0f }, { 1.6f, 2.3f } };
constexpr float kTapDiffGain = 0.7f;
// Late reverb: input diffusers (ms), the two onset taps (ms) and the eight lines (samples at 48 kHz, primes,
// 27 to 56 ms).
constexpr float kDiffMs[EfxReverb::kDiffusers] = { 3.7f, 5.9f, 8.3f, 12.7f };
constexpr float kDiffGain = 0.65f;
// Onset taps (ms): at diffusion 0 two separate echoes, at diffusion 1 the smear from the start, offset a little
// between the sides to keep them apart.
constexpr float kOnsetMs[2][2] = { { 12.0f, 30.0f }, { 0.0f, 4.0f } };
constexpr int kLineTuning[EfxReverb::kLines] = { 1277, 1439, 1601, 1777, 1979, 2203, 2447, 2711 };
// The recirculating tail: fed this long (ms) after the onset starts, so it takes over as the onset dies away, and
// at this level against the onset (lower when diffusion is down, where each echo stands alone).
constexpr float kTailStartMs = 75.0f;
constexpr float kTailGain = 0.6f;
constexpr float kTailGainUndiffused = 0.8f;       // times kTailGain at diffusion 0
// Modulation depth 1 swings the pitch of the tail this much either way (0.3 %, about 5 cents).
constexpr double kModPitch = 0.003;
constexpr double kModMaxSeconds = kModPitch * 4.0 / (2.0 * kPi);   // the excursion at the longest period
// The tail's own treble loss at 8 kHz (in 1 / decay seconds: 0.22 adds 13 dB/s), on top of decay_hf_ratio.
constexpr double kTreblePerSecond = 0.22;
// Left and right leave with a correlation of 0.5, like a diffuse field on two speakers.
constexpr float kWidthMix = 0.268f;
constexpr float kEarlyWidthMix = 0.08f;           // the reflections, already apart by their lean, take less
// Levels against a centred dry sound, fitted to OpenAL Soft's output (boost 0).
constexpr float kEarlyLevel = 0.69f;
constexpr float kLateLevel = 1.0f;

// Every length scales with the cube root of density: density stands for the room's volume, lengths for its size.
float size_scale(float density) { return std::max(0.1f, std::cbrt(density)); }

int next_pow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }
int ms_to_samples(double ms, int rate) { return std::max(1, static_cast<int>(std::lround(ms * rate / 1000.0))); }
// Gain per `samples` for a 60 dB fall in `seconds`.
double decay_gain(double samples, double seconds, int rate) { return std::pow(10.0, -3.0 * samples / (seconds * rate)); }

// Coefficient a of the one pole lowpass (1 - a) / (1 - a z^-1) whose gain at w (radians) is r (< 1).
float onepole_for_gain(double r, double w) {
    if (r >= 0.99999) return 0.0f;
    r = std::max(r, 1e-6);
    const double c = std::cos(std::min(w, kPi * 0.95));
    const double r2 = r * r;
    const double b = 1.0 - r2 * c;
    const double k = 1.0 - r2;
    const double a = (b - std::sqrt(std::max(0.0, b * b - k * k))) / k;
    return static_cast<float>(std::clamp(a, 0.0, 0.9999));
}

inline void hadamard4(float* v) {
    const float a = v[0] + v[1], b = v[0] - v[1], c = v[2] + v[3], d = v[2] - v[3];
    v[0] = (a + c) * 0.5f; v[1] = (b + d) * 0.5f; v[2] = (a - c) * 0.5f; v[3] = (b - d) * 0.5f;
}

// An 8 x 8 skew symmetric matrix of +-1 off the diagonal whose rows are orthogonal (S S^T = 7 I), by Paley's
// construction over the integers mod 7: row and column 0 border the quadratic residue pattern.
void skew8(float s[8][8]) {
    auto chi = [](int x) { x = ((x % 7) + 7) % 7; return x == 0 ? 0 : (x == 1 || x == 2 || x == 4) ? 1 : -1; };
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            s[i][j] = static_cast<float>(i == j ? 0 : i == 0 ? 1 : j == 0 ? -1 : chi(j - i));
}

// Left/right gains of a part leaving through `pan` (EFX pan vector: x right, magnitude 0 = all around, 1 = from
// that direction), with the width mix folded in. m = { ll, lr, rl, rr }: out_l = ll * l + lr * r, and so on.
void output_mix(const float pan[3], float level, float width_mix, float m[4]) {
    const float mag = std::min(1.0f, std::sqrt(pan[0] * pan[0] + pan[1] * pan[1] + pan[2] * pan[2]));
    const float x = mag > 1e-6f ? dsp::clampf(pan[0] / mag, -1.0f, 1.0f) : 0.0f;
    // Focused: the mono sum, constant power panned to x (unity each side at the centre).
    const double theta = (x + 1.0) * kPi / 4.0;
    const float gl = static_cast<float>(std::cos(theta) * std::sqrt(2.0)), gr = static_cast<float>(std::sin(theta) * std::sqrt(2.0));
    const float f = mag * 0.70710678f;
    const float p[4] = { (1.0f - mag) + f * gl, f * gl, f * gr, (1.0f - mag) + f * gr };
    // Width: each side takes a little of the other.
    const float wn = 1.0f / std::sqrt(1.0f + width_mix * width_mix);
    const float w[4] = { wn, width_mix * wn, width_mix * wn, wn };
    m[0] = level * (p[0] * w[0] + p[1] * w[2]);
    m[1] = level * (p[0] * w[1] + p[1] * w[3]);
    m[2] = level * (p[2] * w[0] + p[3] * w[2]);
    m[3] = level * (p[2] * w[1] + p[3] * w[3]);
}

} // namespace

void EfxReverb::Delay::alloc(int max_delay) {
    const int size = next_pow2(max_delay + 2);
    buf.assign(static_cast<size_t>(size), 0.0f);
    mask = size - 1;
    write = 0;
}

void EfxReverb::Delay::clear() {
    std::fill(buf.begin(), buf.end(), 0.0f);
    write = 0;
}

void EfxReverb::init(int sample_rate) {
    rate_ = sample_rate;
    const double ms = rate_ / 1000.0;
    input_.alloc(static_cast<int>((300.0 + 100.0 + kTapMs[kTaps - 1]) * ms) + 4);
    for (int k = 0; k < kTaps; ++k) {
        for (int j = 0; j < 2; ++j) er_diff_[k][j].d.alloc(static_cast<int>(kTapDiffMs[k][j] * ms) + 2);
        for (int g = 0; g < 2; ++g) er_gen_[g][k].alloc(static_cast<int>((kGenFixedMs + kGenMs[g][k]) * ms) + 2);
    }
    echo_.alloc(static_cast<int>(0.25 * rate_) + 2);
    for (int i = 0; i < kDiffusers; ++i) diff_[i].d.alloc(static_cast<int>(kDiffMs[i] * ms) + 2);
    onset_.alloc(static_cast<int>(std::max(kOnsetMs[0][1], kTailStartMs) * ms) + 2);
    const int mod_max = static_cast<int>(kModMaxSeconds * rate_) + 2;
    for (int i = 0; i < kLines; ++i) lines_[i].d.alloc(static_cast<int>(static_cast<double>(kLineTuning[i]) * rate_ / 48000.0) + mod_max + 2);
    reset();
    update_derived();
    for (int i = 0; i < 4; ++i) { early_mix_[i].reset(early_mix_[i].target()); late_mix_[i].reset(late_mix_[i].target()); }
}

void EfxReverb::reset() {
    input_.clear();
    echo_.clear();
    onset_.clear();
    for (int k = 0; k < kTaps; ++k) {
        for (int j = 0; j < 2; ++j) er_diff_[k][j].d.clear();
        for (int g = 0; g < 2; ++g) er_gen_[g][k].clear();
    }
    for (int i = 0; i < kDiffusers; ++i) diff_[i].d.clear();
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        l.d.clear();
        l.hf_state = l.hf_state2 = l.lf_state = l.lf_state2 = 0.0f;
        // Every line sways from its own point of the cycle.
        const double phase = 2.0 * kPi * i / kLines;
        l.mod_sin = static_cast<float>(std::sin(phase));
        l.mod_cos = static_cast<float>(std::cos(phase));
    }
    shelf_hf_.reset();
    shelf_lf_.reset();
    silent_frames_ = quiet_frames_ = 0;
    idle_ = true;
}

void EfxReverb::set_params(const EfxReverbParams& in) {
    EfxReverbParams& p = p_;
    p = in;
    p.density = dsp::clampf(p.density, 0.0f, 1.0f);
    p.diffusion = dsp::clampf(p.diffusion, 0.0f, 1.0f);
    p.gain = dsp::clampf(p.gain, 0.0f, 1.0f);
    p.gain_hf = dsp::clampf(p.gain_hf, 0.0f, 1.0f);
    p.gain_lf = dsp::clampf(p.gain_lf, 0.0f, 1.0f);
    p.decay_time = dsp::clampf(p.decay_time, 0.1f, 20.0f);
    p.decay_hf_ratio = dsp::clampf(p.decay_hf_ratio, 0.1f, 2.0f);
    p.decay_lf_ratio = dsp::clampf(p.decay_lf_ratio, 0.1f, 2.0f);
    p.reflections_gain = dsp::clampf(p.reflections_gain, 0.0f, 3.16f);
    p.reflections_delay = dsp::clampf(p.reflections_delay, 0.0f, 0.3f);
    p.late_reverb_gain = dsp::clampf(p.late_reverb_gain, 0.0f, 10.0f);
    p.late_reverb_delay = dsp::clampf(p.late_reverb_delay, 0.0f, 0.1f);
    p.echo_time = dsp::clampf(p.echo_time, 0.075f, 0.25f);
    p.echo_depth = dsp::clampf(p.echo_depth, 0.0f, 1.0f);
    p.modulation_time = dsp::clampf(p.modulation_time, 0.04f, 4.0f);
    p.modulation_depth = dsp::clampf(p.modulation_depth, 0.0f, 1.0f);
    p.air_absorption_gain_hf = dsp::clampf(p.air_absorption_gain_hf, 0.892f, 1.0f);
    p.hf_reference = dsp::clampf(p.hf_reference, 1000.0f, 20000.0f);
    p.lf_reference = dsp::clampf(p.lf_reference, 20.0f, 1000.0f);
    p.room_rolloff_factor = dsp::clampf(p.room_rolloff_factor, 0.0f, 10.0f);
    p.boost_db = dsp::clampf(p.boost_db, -24.0f, 24.0f);
    for (int i = 0; i < 3; ++i) {
        p.reflections_pan[i] = dsp::clampf(p.reflections_pan[i], -1.0f, 1.0f);
        p.late_reverb_pan[i] = dsp::clampf(p.late_reverb_pan[i], -1.0f, 1.0f);
    }
    if (!input_.buf.empty()) update_derived();
}

void EfxReverb::update_derived() {
    const EfxReverbParams& p = p_;
    const float rate = static_cast<float>(rate_);
    const double t_mid = p.decay_time;
    const float size = size_scale(p.density);

    // Input shelves: gain_hf is the gain at hf_reference, going on down to its square above (gain_lf likewise).
    use_hf_shelf_ = p.gain_hf < 0.9999f;
    if (use_hf_shelf_) shelf_hf_.set(dsp::Biquad::Type::HighShelf, std::min(p.hf_reference, rate * 0.45f), 0.7071f, 40.0f * std::log10(std::max(p.gain_hf, 1e-3f)), rate);
    use_lf_shelf_ = p.gain_lf < 0.9999f;
    if (use_lf_shelf_) shelf_lf_.set(dsp::Biquad::Type::LowShelf, p.lf_reference, 0.7071f, 40.0f * std::log10(std::max(p.gain_lf, 1e-3f)), rate);

    // Early reflections.
    const int pre = static_cast<int>(std::lround(p.reflections_delay * rate_));
    for (int k = 0; k < kTaps; ++k) {
        tap_delay_[k] = pre + ms_to_samples(kTapMs[k] * size, rate_);
        for (int j = 0; j < 2; ++j) {
            er_diff_[k][j].length = ms_to_samples(kTapDiffMs[k][j] * size, rate_);
            er_diff_[k][j].gain = kTapDiffGain * p.diffusion;
        }
        for (int g = 0; g < 2; ++g) {
            er_len_[g][k] = ms_to_samples(kGenFixedMs + kGenMs[g][k] * size, rate_);
            er_gen_gain_[g][k] = static_cast<float>(decay_gain(er_len_[g][k], t_mid, rate_) * (g == 0 ? kGenLoss : 1.0f));
        }
    }

    // Late reverb: echo, diffusers, onset.
    late_delay_ = std::max(1, static_cast<int>(std::lround((p.reflections_delay + p.late_reverb_delay) * rate_)));
    echo_len_ = std::max(1, static_cast<int>(std::lround(p.echo_time * rate_)));
    echo_fb_ = static_cast<float>(decay_gain(echo_len_, t_mid, rate_));
    echo_mix_ = p.echo_depth;
    echo_norm_ = 1.0f / std::sqrt(1.0f + echo_mix_ * echo_mix_ / std::max(1e-3f, 1.0f - echo_fb_ * echo_fb_));
    for (int i = 0; i < kDiffusers; ++i) {
        diff_[i].length = ms_to_samples(kDiffMs[i] * size, rate_);
        diff_[i].gain = kDiffGain * p.diffusion;
    }
    // Diffusion sets how long the allpasses smear, and crossfades each part between its plain taps and their
    // smeared version (at equal power), so at 0 nothing is smeared and nothing is delayed by the allpasses.
    raw_mix_ = std::sqrt(1.0f - p.diffusion);
    diffused_mix_ = std::sqrt(p.diffusion);
    tail_start_ = ms_to_samples(kTailStartMs * size, rate_) + 1;
    for (int c = 0; c < 2; ++c) onset_len_[c] = std::max(1, static_cast<int>(std::lround(dsp::lerp(kOnsetMs[0][c], kOnsetMs[1][c], p.diffusion) * size * rate_ / 1000.0)) + 1);

    // The lines. Band decay times: the HF one limited by what the air lets through when decay_hf_limit is set.
    double t_hf = t_mid * p.decay_hf_ratio;
    if (p.decay_hf_limit && p.air_absorption_gain_hf < 1.0f) {
        const double db_per_second = -20.0 * std::log10(p.air_absorption_gain_hf) * kSpeedOfSound;
        t_hf = std::min(t_hf, 60.0 / db_per_second);
    }
    // The tail also loses treble on its own, rising with the square of frequency, as air takes it.
    const double hf_khz = std::min(p.hf_reference, rate * 0.45f) / 1000.0;
    t_hf = 1.0 / (1.0 / t_hf + kTreblePerSecond * (hf_khz / 8.0) * (hf_khz / 8.0));
    const double t_lf = t_mid * p.decay_lf_ratio;
    const double w_hf = 2.0 * kPi * std::min(p.hf_reference, rate * 0.45f) / rate_;
    const int mod_max = static_cast<int>(kModMaxSeconds * rate_);
    double mean_len = 0.0;
    lf_on_ = std::fabs(p.decay_lf_ratio - 1.0f) > 1e-4f;
    int shortest = 1 << 30;
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        l.length = std::max(16, static_cast<int>(std::lround(kLineTuning[i] * (rate_ / 48000.0) * size)));
        shortest = std::min(shortest, l.length);
        mean_len += l.length;
        const double g = decay_gain(l.length, t_mid, rate_);
        l.gain = static_cast<float>(g);
        const double r_hf = decay_gain(l.length, t_hf, rate_) / g;
        if (r_hf <= 1.0) {
            l.hf_lift = false;
            l.hf_coef = onepole_for_gain(r_hf, w_hf);
        } else {
            // HF dies slower than the mids: a first order high shelf with r_hf at hf_reference, its midpoint, and
            // r_hf^2 above, held below unity loop gain.
            l.hf_lift = true;
            l.hf_coef = 0.0f;
            const double shelf = std::min(r_hf * r_hf, 0.995 / g);
            const double fp = std::min(std::sqrt(shelf) * std::min(p.hf_reference, rate * 0.45f), rate * 0.45);
            const double k = std::tan(kPi * fp / rate_);
            l.hf_b0 = static_cast<float>((shelf + k) / (1.0 + k));
            l.hf_b1 = static_cast<float>((k - shelf) / (1.0 + k));
            l.hf_a1 = static_cast<float>((k - 1.0) / (1.0 + k));
        }
        // The low band likewise: a first order low shelf with r_lf at lf_reference, its midpoint, and r_lf^2 below.
        const double r_lf = decay_gain(l.length, t_lf, rate_) / g;
        const double shelf = std::min(r_lf * r_lf, 0.995 / g);
        const double k = std::tan(kPi * p.lf_reference / std::sqrt(shelf) / rate_);
        l.lf_b0 = static_cast<float>((1.0 + shelf * k) / (1.0 + k));
        l.lf_b1 = static_cast<float>((shelf * k - 1.0) / (1.0 + k));
        l.lf_a1 = static_cast<float>((k - 1.0) / (1.0 + k));
    }
    mean_len /= kLines;
    mod_depth_ = static_cast<float>(std::min<double>(p.modulation_depth * kModPitch * p.modulation_time * rate_ / (2.0 * kPi),
                                                     std::min(mod_max, shortest - 4)));
    const double step = 2.0 * kPi / (p.modulation_time * rate_);
    mod_step_sin_ = static_cast<float>(std::sin(step));
    mod_step_cos_ = static_cast<float>(std::cos(step));

    // Feedback: each line feeds the next (a ring, one echo at a time: diffusion 0) turned towards a full mix, the
    // skew Hadamard matrix (I + S) / sqrt(8), as diffusion rises. cos(t) I + sin(t) S / sqrt(7) stays orthogonal
    // all the way, so diffusion changes the echo density and not the decay. The input likewise goes from one line
    // to all of them.
    float skew[kLines][kLines];
    skew8(skew);
    const double theta = p.diffusion * std::acos(1.0 / std::sqrt(8.0));
    const float cs = static_cast<float>(std::cos(theta)), sn = static_cast<float>(std::sin(theta) / std::sqrt(7.0));
    for (int i = 0; i < kLines; ++i) {
        const int from = (i + kLines - 1) % kLines;        // the ring: line i hears line i - 1
        for (int j = 0; j < kLines; ++j) mix_[i][j] = (from == j ? cs : 0.0f) + sn * skew[from][j];
    }
    double norm = 0.0;
    for (int i = 0; i < kLines; ++i) {
        inject_gain_[i] = static_cast<float>(p.diffusion / std::sqrt(8.0) + (i == 0 ? 1.0 - p.diffusion : 0.0));
        norm += static_cast<double>(inject_gain_[i]) * inject_gain_[i];
    }
    // The feed starts the tail where the onset's decay would have brought it by then.
    const double feed_gain = decay_gain(tail_start_, t_mid, rate_);
    for (int i = 0; i < kLines; ++i) inject_gain_[i] = static_cast<float>(inject_gain_[i] / std::sqrt(norm) * feed_gain);

    // The late level: the onset plus a tail whose energy grows with the decay time as g^2 / (1 - g^2) for the mean
    // pass. Normalised to the same total whatever the decay, as in EFX, reckoning the decay as the geometric mean
    // of the mid and treble decay times (most of a broadband sound's energy is treble), so a damped room is not
    // quieter at its start than an undamped one.
    const double gm = decay_gain(mean_len, std::sqrt(t_mid * t_hf), rate_);
    tail_gain_ = kTailGain * dsp::lerp(kTailGainUndiffused, 1.0f, p.diffusion);
    const float late_norm = static_cast<float>(1.0 / std::sqrt(1.0 + tail_gain_ * tail_gain_ * feed_gain * feed_gain * 0.5 * gm * gm / (1.0 - gm * gm)));

    // Outputs.
    const float out = p.gain * dsp::db_to_gain(p.boost_db);
    float m[4];
    const int ramp = rate_ / 50;
    output_mix(p.reflections_pan, out * p.reflections_gain * kEarlyLevel, kEarlyWidthMix, m);
    for (int i = 0; i < 4; ++i) early_mix_[i].set_target(m[i], ramp);
    output_mix(p.late_reverb_pan, out * p.late_reverb_gain * kLateLevel * late_norm, kWidthMix, m);
    for (int i = 0; i < 4; ++i) late_mix_[i].set_target(m[i], ramp);

    // The longest way through, for going idle: input delay, the taps and their generations, the echo, the
    // diffusers, the onset and the longest line.
    int er_path = 0;
    for (int k = 0; k < kTaps; ++k) er_path = std::max(er_path, tap_delay_[k] + er_diff_[k][0].length + er_diff_[k][1].length + er_len_[0][k] + er_len_[1][k]);
    int late_path = late_delay_ + (echo_mix_ > 0.0f ? echo_len_ : 0) + std::max(std::max(onset_len_[0], onset_len_[1]), tail_start_);
    for (int i = 0; i < kDiffusers; ++i) late_path += diff_[i].length;
    late_path += lines_[kLines - 1].length + static_cast<int>(mod_depth_) + 2;
    settle_frames_ = std::max(er_path, late_path);
}

void EfxReverb::process(const float* in_l, const float* in_r, float* out_l, float* out_r, int frames) {
    float in_peak = 0.0f;
    for (int i = 0; i < frames; ++i) in_peak = std::max(in_peak, std::max(std::fabs(in_l[i]), std::fabs(in_r[i])));
    if (in_peak > 1e-7f) {
        idle_ = false;
        silent_frames_ = 0;
    } else {
        silent_frames_ = std::min(silent_frames_ + frames, 1 << 30);
        if (idle_) return;
    }

    const bool modulate = mod_depth_ > 0.0f;
    float tail_peak = 0.0f;
    float y[kLines];
    for (int n = 0; n < frames; ++n) {
        float x = (in_l[n] + in_r[n]) * 0.5f;
        if (use_hf_shelf_) x = shelf_hf_.process(x);
        if (use_lf_shelf_) x = shelf_lf_.process(x);
        input_.push(x);

        // Early reflections: the taps, smeared, then two more generations of them.
        float el = 0.0f, er = 0.0f;
        float a[kTaps], b[kTaps];
        for (int k = 0; k < kTaps; ++k) {
            const float raw = input_.tap(tap_delay_[k] + 1);
            const float t = raw * raw_mix_ + er_diff_[k][1].process(er_diff_[k][0].process(raw)) * diffused_mix_;
            a[k] = t;
            if (kTapLeft[k]) { el += t * kLeanNear; er += t * kLeanFar; }
            else { el += t * kLeanFar; er += t * kLeanNear; }
            b[k] = er_gen_[0][k].tap(er_len_[0][k]) * er_gen_gain_[0][k];
            const float c = er_gen_[1][k].tap(er_len_[1][k]) * er_gen_gain_[1][k];
            el += b[k] * kGenOut[0][0][k] + c * kGenOut[1][0][k];
            er += b[k] * kGenOut[0][1][k] + c * kGenOut[1][1][k];
        }
        hadamard4(a);
        hadamard4(b);
        for (int k = 0; k < kTaps; ++k) { er_gen_[0][k].push(a[k]); er_gen_[1][k].push(b[k]); }

        // Late reverb: echo, diffusers, the onset taps, then the lines.
        float u = input_.tap(late_delay_ + 1);
        if (echo_mix_ > 0.0f) {
            const float w = echo_.tap(echo_len_);
            echo_.push(dsp::undenormal(u + echo_fb_ * w));
            u = (u + echo_mix_ * w) * echo_norm_;
        }
        float smeared = u;
        for (int i = 0; i < kDiffusers; ++i) smeared = diff_[i].process(smeared);
        u = u * raw_mix_ + smeared * diffused_mix_;
        onset_.push(u);
        float ol = onset_.tap(onset_len_[0]), orr = onset_.tap(onset_len_[1]);
        const float feed = onset_.tap(tail_start_);
        float tl = 0.0f, tr = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[i];
            float v;
            if (modulate) {
                const float s = l.mod_sin * mod_step_cos_ + l.mod_cos * mod_step_sin_;
                l.mod_cos = l.mod_cos * mod_step_cos_ - l.mod_sin * mod_step_sin_;
                l.mod_sin = s;
                const float pos = static_cast<float>(l.length) - mod_depth_ * s;
                const int i0 = static_cast<int>(pos);
                const float frac = pos - static_cast<float>(i0);
                const float y0 = l.d.tap(i0), y1 = l.d.tap(i0 + 1);
                v = y0 + (y1 - y0) * frac;
            } else {
                v = l.d.tap(l.length);
            }
            v = dsp::undenormal(v);
            // This pass's share of the decay, per band.
            if (lf_on_) {
                const float shelved = dsp::undenormal(l.lf_b0 * v + l.lf_b1 * l.lf_state - l.lf_a1 * l.lf_state2);
                l.lf_state = v;
                l.lf_state2 = shelved;
                v = shelved;
            }
            if (l.hf_lift) {
                const float lifted = dsp::undenormal(l.hf_b0 * v + l.hf_b1 * l.hf_state - l.hf_a1 * l.hf_state2);
                l.hf_state = v;
                l.hf_state2 = lifted;
                v = lifted;
            } else if (l.hf_coef > 0.0f) {
                l.hf_state = dsp::undenormal(v + (l.hf_state - v) * l.hf_coef);
                v = l.hf_state;
            }
            y[i] = v * l.gain;
            if (i & 1) tr += y[i]; else tl += y[i];
        }
        for (int i = 0; i < kLines; ++i) {
            const float* row = mix_[i];
            float acc = feed * inject_gain_[i];
            for (int j = 0; j < kLines; ++j) acc += row[j] * y[j];
            lines_[i].d.push(acc);
        }
        ol += tl * tail_gain_;
        orr += tr * tail_gain_;

        const float e0 = early_mix_[0].next(), e1 = early_mix_[1].next(), e2 = early_mix_[2].next(), e3 = early_mix_[3].next();
        const float l0 = late_mix_[0].next(), l1 = late_mix_[1].next(), l2 = late_mix_[2].next(), l3 = late_mix_[3].next();
        const float wl = e0 * el + e1 * er + l0 * ol + l1 * orr;
        const float wr = e2 * el + e3 * er + l2 * ol + l3 * orr;
        tail_peak = std::max(tail_peak, std::max(std::fabs(wl), std::fabs(wr)));
        out_l[n] += wl;
        out_r[n] += wr;
    }
    if (modulate) {
        for (Line& l : lines_) {
            const float r = 1.0f / std::sqrt(l.mod_sin * l.mod_sin + l.mod_cos * l.mod_cos);
            l.mod_sin *= r;
            l.mod_cos *= r;
        }
    }

    // Idle once the input has been silent for longer than the longest way through and the tail has stayed below
    // -140 dB as long: a click shorter than the first reflection must still get its reverb.
    quiet_frames_ = tail_peak < 1e-7f ? std::min(quiet_frames_ + frames, 1 << 30) : 0;
    if (in_peak <= 1e-7f && silent_frames_ > settle_frames_ && quiet_frames_ > settle_frames_) idle_ = true;
}

} // namespace fastplay::audio
