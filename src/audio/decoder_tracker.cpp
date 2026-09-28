// Tracker modules (MOD, XM, IT, S3M, MO3 and the many other formats libopenmpt
// reads), played by libopenmpt. A module is small, so it is read into memory.

#include "audio.h"
#include "audio_internal.h"
#include "utils.h"

#include <libopenmpt/libopenmpt.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace audio {
namespace {

const int kRate = 48000;

class TrackerDecoder : public Decoder {
public:
    ~TrackerDecoder() override {
        if (m_module) openmpt_module_destroy(m_module);
    }

    bool Open(const std::wstring& path, std::wstring& error) {
        FILE* f = FileOpen(path, "rb");
        if (!f) {
            error = L"The file could not be opened.";
            return false;
        }
        std::vector<uint8_t> data;
        uint8_t buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) data.insert(data.end(), buffer, buffer + n);
        fclose(f);

        int code = 0;
        const char* message = nullptr;
        m_module = openmpt_module_create_from_memory2(data.data(), data.size(), openmpt_log_func_silent, nullptr,
                                                      openmpt_error_func_ignore, nullptr, &code, &message, nullptr);
        if (!m_module) {
            error = L"It is not a module FastPlay can read.";
            if (message) {
                error += L" " + Utf8ToWide(message);
                openmpt_free_string(message);
            }
            return false;
        }
        if (message) openmpt_free_string(message);
        openmpt_module_set_repeat_count(m_module, 0);  // once through (FastPlay's repeat does the rest)
        m_length = openmpt_module_get_duration_seconds(m_module);
        m_title = Metadata("title");
        m_artist = Metadata("artist");
        m_comment = Metadata("message");
        m_type = Metadata("type_long");
        m_date = Metadata("date");
        return true;
    }

    int SampleRate() const override { return kRate; }

    int Read(float* out, int frames) override {
        return static_cast<int>(openmpt_module_read_interleaved_float_stereo(m_module, kRate, static_cast<size_t>(frames), out));
    }

    bool Seek(double seconds) override {
        openmpt_module_set_position_seconds(m_module, seconds);
        return true;
    }

    void Abort() override {}
    double Length() const override { return m_length; }
    bool IsLive() const override { return false; }

    std::string Tag(const std::string& name) const override {
        const char* key = name.c_str();
        if (StrICmp(key, "TITLE") == 0) return m_title;
        if (StrICmp(key, "ARTIST") == 0) return m_artist;
        if (StrICmp(key, "COMMENT") == 0) return m_comment;
        if (StrICmp(key, "DATE") == 0 || StrICmp(key, "YEAR") == 0) return m_date;
        if (StrICmp(key, "GENRE") == 0) return "";
        return "";
    }

    std::string StreamTitle() const override { return ""; }
    std::vector<Chapter> Chapters() const override { return {}; }
    int Bitrate() const override { return 0; }
    bool IsVbr() const override { return false; }
    int SourceChannels() const override { return 2; }
    int SourceSampleRate() const override { return kRate; }
    int SourceBits() const override { return 0; }
    std::string CodecName() const override { return m_type; }

private:
    std::string Metadata(const char* key) {
        const char* value = openmpt_module_get_metadata(m_module, key);
        if (!value) return "";
        std::string text = value;
        openmpt_free_string(value);
        return text;
    }

    openmpt_module* m_module = nullptr;
    double m_length = 0;
    std::string m_title, m_artist, m_comment, m_type, m_date;
};

}  // namespace

bool IsTrackerPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    std::string ext = WideToUtf8(path.substr(dot + 1));
    return !ext.empty() && openmpt_is_extension_supported(ext.c_str()) != 0;
}

std::unique_ptr<Decoder> OpenTrackerDecoder(const std::wstring& path, std::wstring& error) {
    auto decoder = std::make_unique<TrackerDecoder>();
    if (!decoder->Open(path, error)) return nullptr;
    return decoder;
}

}  // namespace audio
