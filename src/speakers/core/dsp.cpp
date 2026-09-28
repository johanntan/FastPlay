#include "dsp.h"

namespace speakers {

namespace dsp {

namespace {

// Shared tail of the RBJ cookbook: normalise by a0 and store.
void Normalize(Biquad &f, float b0, float b1, float b2, float a0, float a1, float a2) {
    float inv = 1.0f / a0;
    f.b0 = b0 * inv;
    f.b1 = b1 * inv;
    f.b2 = b2 * inv;
    f.a1 = a1 * inv;
    f.a2 = a2 * inv;
}

struct Omega {
    float w0, sn, cs, alpha;
};

Omega MakeOmega(float sr, float freq, float q) {
    Omega o{};
    freq = Clampf(freq, 1.0f, sr * 0.49f);
    q = std::max(q, 0.01f);
    o.w0 = 2.0f * kPi * freq / sr;
    o.sn = std::sin(o.w0);
    o.cs = std::cos(o.w0);
    o.alpha = o.sn / (2.0f * q);
    return o;
}

} // namespace

void Biquad::SetLowpass(float sr, float freq, float q) {
    Omega o = MakeOmega(sr, freq, q);
    float num = 1.0f - o.cs;
    Normalize(*this, num * 0.5f, num, num * 0.5f, 1.0f + o.alpha, -2.0f * o.cs, 1.0f - o.alpha);
}

void Biquad::SetHighpass(float sr, float freq, float q) {
    Omega o = MakeOmega(sr, freq, q);
    float num = 1.0f + o.cs;
    Normalize(*this, num * 0.5f, -num, num * 0.5f, 1.0f + o.alpha, -2.0f * o.cs, 1.0f - o.alpha);
}

void Biquad::SetBandpass(float sr, float freq, float q) {
    Omega o = MakeOmega(sr, freq, q);
    Normalize(*this, o.alpha, 0.0f, -o.alpha, 1.0f + o.alpha, -2.0f * o.cs, 1.0f - o.alpha);
}

void Biquad::SetPeaking(float sr, float freq, float q, float gainDb) {
    Omega o = MakeOmega(sr, freq, q);
    float A = std::pow(10.0f, gainDb / 40.0f);
    Normalize(*this, 1.0f + o.alpha * A, -2.0f * o.cs, 1.0f - o.alpha * A, 1.0f + o.alpha / A,
              -2.0f * o.cs, 1.0f - o.alpha / A);
}

void Biquad::SetLowShelf(float sr, float freq, float q, float gainDb) {
    Omega o = MakeOmega(sr, freq, q);
    float A = std::pow(10.0f, gainDb / 40.0f);
    float sqrtA = std::sqrt(A);
    float beta = 2.0f * sqrtA * o.alpha;
    float ap1 = A + 1.0f, am1 = A - 1.0f;
    Normalize(*this, A * (ap1 - am1 * o.cs + beta), 2.0f * A * (am1 - ap1 * o.cs),
              A * (ap1 - am1 * o.cs - beta), ap1 + am1 * o.cs + beta,
              -2.0f * (am1 + ap1 * o.cs), ap1 + am1 * o.cs - beta);
}

void Biquad::SetHighShelf(float sr, float freq, float q, float gainDb) {
    Omega o = MakeOmega(sr, freq, q);
    float A = std::pow(10.0f, gainDb / 40.0f);
    float sqrtA = std::sqrt(A);
    float beta = 2.0f * sqrtA * o.alpha;
    float ap1 = A + 1.0f, am1 = A - 1.0f;
    Normalize(*this, A * (ap1 + am1 * o.cs + beta), -2.0f * A * (am1 + ap1 * o.cs),
              A * (ap1 + am1 * o.cs - beta), ap1 - am1 * o.cs + beta,
              2.0f * (am1 - ap1 * o.cs), ap1 - am1 * o.cs - beta);
}

void Biquad::SetAllpass(float sr, float freq, float q) {
    Omega o = MakeOmega(sr, freq, q);
    Normalize(*this, 1.0f - o.alpha, -2.0f * o.cs, 1.0f + o.alpha, 1.0f + o.alpha, -2.0f * o.cs,
              1.0f - o.alpha);
}

// ---------------------------------------------------------------------------
// Cascade
// ---------------------------------------------------------------------------

void Cascade::Reset() {
    for (auto &b : m_stages) b.Reset();
}

namespace {

// Pole Q values of an order-N Butterworth, one per biquad section.
std::vector<float> ButterworthQs(int order) {
    if (order < 2) order = 2;
    if (order % 2) order++; // only even orders are expressible as biquads
    int sections = order / 2;
    std::vector<float> qs;
    qs.reserve(sections);
    for (int k = 0; k < sections; ++k) {
        float theta = kPi * (float)(2 * k + 1) / (float)(2 * order);
        qs.push_back(1.0f / (2.0f * std::cos(theta)));
    }
    return qs;
}

} // namespace

void Cascade::SetButterworthLowpass(float sr, float freq, int order) {
    m_stages.clear();
    for (float q : ButterworthQs(order)) {
        Biquad b;
        b.SetLowpass(sr, freq, q);
        m_stages.push_back(b);
    }
}

void Cascade::SetButterworthHighpass(float sr, float freq, int order) {
    m_stages.clear();
    for (float q : ButterworthQs(order)) {
        Biquad b;
        b.SetHighpass(sr, freq, q);
        m_stages.push_back(b);
    }
}

void Cascade::SetLinkwitzRileyLowpass(float sr, float freq, int order) {
    // LR-N is two cascaded Butterworth-N/2 filters at the same corner.
    int half = std::max(order / 2, 1);
    if (half % 2) half++;
    m_stages.clear();
    for (int pass = 0; pass < 2; ++pass)
        for (float q : ButterworthQs(half)) {
            Biquad b;
            b.SetLowpass(sr, freq, q);
            m_stages.push_back(b);
        }
}

void Cascade::SetLinkwitzRileyHighpass(float sr, float freq, int order) {
    int half = std::max(order / 2, 1);
    if (half % 2) half++;
    m_stages.clear();
    for (int pass = 0; pass < 2; ++pass)
        for (float q : ButterworthQs(half)) {
            Biquad b;
            b.SetHighpass(sr, freq, q);
            m_stages.push_back(b);
        }
}

// ---------------------------------------------------------------------------
// DelayLine
// ---------------------------------------------------------------------------

void DelayLine::Init(int maxSamples) {
    if (maxSamples < 4) maxSamples = 4;
    m_buf.assign((size_t)maxSamples, 0.0f);
    m_write = 0;
}

void DelayLine::Reset() {
    std::fill(m_buf.begin(), m_buf.end(), 0.0f);
    m_write = 0;
}

} // namespace dsp

}  // namespace speakers
