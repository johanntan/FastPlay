// Thin RAII wrapper over pffft. Spectra are kept in pffft's internal
// (unordered) layout, which is what its SIMD complex multiply-accumulate wants.
#pragma once
#include "pffft/pffft.h"
#include <cstddef>
#include <cstring>
#include <utility>

namespace fastplay::audio {

// 16 byte aligned float buffer (pffft requirement, also good for SIMD).
class AlignedFloats {
public:
    AlignedFloats() = default;
    explicit AlignedFloats(size_t n) { allocate(n); }
    ~AlignedFloats() { release(); }
    AlignedFloats(const AlignedFloats&) = delete;
    AlignedFloats& operator=(const AlignedFloats&) = delete;
    AlignedFloats(AlignedFloats&& o) noexcept : data_(o.data_), size_(o.size_) { o.data_ = nullptr; o.size_ = 0; }
    AlignedFloats& operator=(AlignedFloats&& o) noexcept {
        if (this != &o) { release(); data_ = o.data_; size_ = o.size_; o.data_ = nullptr; o.size_ = 0; }
        return *this;
    }
    void allocate(size_t n) {
        release();
        if (n) { data_ = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float))); size_ = n; zero(); }
    }
    void release() { if (data_) pffft_aligned_free(data_); data_ = nullptr; size_ = 0; }
    void zero() { if (data_) std::memset(data_, 0, size_ * sizeof(float)); }
    float* data() { return data_; }
    const float* data() const { return data_; }
    size_t size() const { return size_; }
    float& operator[](size_t i) { return data_[i]; }
    const float& operator[](size_t i) const { return data_[i]; }
private:
    float* data_ = nullptr;
    size_t size_ = 0;
};

class RealFft {
public:
    RealFft() = default;
    explicit RealFft(int n) { init(n); }
    ~RealFft() { if (setup_) pffft_destroy_setup(setup_); }
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;
    void init(int n) {
        if (setup_) pffft_destroy_setup(setup_);
        n_ = n;
        setup_ = pffft_new_setup(n, PFFFT_REAL);
        work_.allocate(static_cast<size_t>(n));
    }
    int size() const { return n_; }
    // Forward real transform, output in internal layout (n floats).
    void forward(const float* in, float* out) { pffft_transform(setup_, in, out, work_.data(), PFFFT_FORWARD); }
    // Inverse from internal layout. Unnormalised: scale by 1/n yourself (or fold it into convolve scaling).
    void inverse(const float* in, float* out) { pffft_transform(setup_, in, out, work_.data(), PFFFT_BACKWARD); }
    void forward_ordered(const float* in, float* out) { pffft_transform_ordered(setup_, in, out, work_.data(), PFFFT_FORWARD); }
    void inverse_ordered(const float* in, float* out) { pffft_transform_ordered(setup_, in, out, work_.data(), PFFFT_BACKWARD); }
    // out += a * b * scaling, all in internal layout.
    void convolve_accumulate(const float* a, const float* b, float* out, float scaling) {
        pffft_zconvolve_accumulate(setup_, a, b, out, scaling);
    }
private:
    PFFFT_Setup* setup_ = nullptr;
    AlignedFloats work_;
    int n_ = 0;
};

} // namespace fastplay::audio
