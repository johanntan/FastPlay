// Which decoder plays what: SpessaSynth for MIDI, FDK AAC for xHE-AAC MP4 files,
// FFmpeg for the rest.

#include "audio.h"
#include "audio_internal.h"
#include "utils.h"
#include "xheaac.h"

#include <cwchar>

namespace audio {
namespace {

bool HasExtension(const std::wstring& path, std::initializer_list<const wchar_t*> extensions) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    for (const wchar_t* ext : extensions) {
        if (WStrICmp(path.c_str() + dot, ext) == 0) return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<Decoder> OpenDecoder(const std::wstring& pathOrUrl, std::wstring& error) {
    if (!IsNetworkPath(pathOrUrl)) {
        if (HasExtension(pathOrUrl, {L".mid", L".midi", L".kar", L".rmi", L".smf", L".xmi", L".mus", L".hmi",
                                     L".hmp", L".mids", L".xmf", L".mxmf"})) {
            return OpenMidiDecoder(pathOrUrl, error);
        }
        if (HasExtension(pathOrUrl, {L".mod", L".xm", L".it", L".s3m", L".mtm", L".umx", L".mo3"})) {
            error = L"Tracker modules cannot be played by this build yet.";
            return nullptr;
        }
        if (HasExtension(pathOrUrl, {L".m4a", L".m4b", L".mp4"}) && IsXheAacFile(pathOrUrl)) {
            return OpenXheAacDecoder(pathOrUrl, error);
        }
    }
    return OpenFfmpegDecoder(pathOrUrl, error);
}

}  // namespace audio
