// The application: startup, the single-instance handoff, and the main window.

#include "ui/main_frame.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "settings.h"
#include "hotkeys.h"
#include "youtube.h"
#include "file_assoc.h"
#include "playlist_io.h"
#include "platform.h"
#include "app_ui.h"

#include <wx/ipc.h>
#include <wx/snglinst.h>
#include <wx/filename.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

// Files are handed from a newly started FastPlay to the one already running.
const char* kIpcService = "FastPlay";
const char* kIpcTopic = "files";

// Receives the files, one Execute() per path.
class FileConnection : public wxConnection {
public:
    bool OnExecute(const wxString&, const void* data, size_t size, wxIPCFormat format) override {
        std::wstring path = WS(GetTextFromData(data, size, format));
        // Handle it after the call returns, so the sender is not kept waiting on playback.
        RunOnUiThread([path]() {
            if (MainFrame* frame = GetMainFrame()) frame->ReceiveFile(path);
        });
        return true;
    }
};

class FileServer : public wxServer {
public:
    wxConnectionBase* OnAcceptConnection(const wxString& topic) override {
        return topic == kIpcTopic ? new FileConnection() : nullptr;
    }
};

// wxWidgets reports problems it considers worth a message box (a hotkey another
// program already owns, for instance). FastPlay has always carried on silently.
class SilentLog : public wxLog {
protected:
    void DoLogText(const wxString&) override {}
};

}  // namespace

class FastPlayApp : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;

private:
    std::vector<std::wstring> ExistingPathArgs() const;
    bool HandOverToRunningInstance(const std::vector<std::wstring>& files);

    std::unique_ptr<wxSingleInstanceChecker> m_instance;
    std::unique_ptr<FileServer> m_server;
};

std::vector<std::wstring> FastPlayApp::ExistingPathArgs() const {
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; i++) {
        wxString arg = argv[i];
        if (wxFileName::Exists(arg)) files.push_back(WS(arg));
    }
    return files;
}

bool FastPlayApp::HandOverToRunningInstance(const std::vector<std::wstring>& files) {
    wxClient client;
    std::unique_ptr<wxConnectionBase> connection(client.MakeConnection("localhost", kIpcService, kIpcTopic));
    if (!connection) return false;
    for (const auto& file : files) {
        connection->Execute(WX(file));
    }
    connection->Disconnect();
    return true;
}

bool FastPlayApp::OnInit() {
    wxDISABLE_ASSERTS_IN_RELEASE_BUILD();
    delete wxLog::SetActiveTarget(new SilentLog());
    SetAppName(APP_NAME);

    // Seed the RNG so shuffle order differs between runs.
    srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

    // Must run before any audio library DLL loads.
    PlatformStartup();

    // Single instance:
    // - multiple instances not allowed: always hand over to the running one
    // - multiple instances allowed and started with files: hand the files over
    // - multiple instances allowed and started without files: start a new one
    std::vector<std::wstring> files = ExistingPathArgs();
    bool useSingleInstance = !ReadAllowMultipleInstances() || !files.empty();

    m_instance = std::make_unique<wxSingleInstanceChecker>();
    m_instance->Create(MUTEX_NAME);
    bool anotherRunning = m_instance->IsAnotherRunning();
    if (anotherRunning && useSingleInstance && HandOverToRunningInstance(files)) {
        return false;
    }
    // Every running FastPlay accepts files, as any FastPlay window used to, so a
    // handoff still works after the first of several instances has closed.
    m_server = std::make_unique<FileServer>();
    m_server->Create(kIpcService);

    LoadSettings();
    if (g_registerFileTypes) {
        RegisterAllFileTypes();
    }
    LoadHotkeys();
    YouTubeCleanup();  // Clean up any leftover temp files from previous sessions

    // Files FastPlay was started with become the playlist.
    for (const auto& path : files) {
        if (IsPlaylistFile(path)) {
            auto entries = ParsePlaylist(path);
            g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
        } else {
            g_playlist.push_back(path);
        }
    }

    auto* frame = new MainFrame();
    if (!frame->Initialize()) {
        frame->Destroy();
        return false;
    }
    SetTopWindow(frame);
    frame->Show();

    if (g_playlist.empty()) {
        LoadPlaybackState();
    }
    return true;
}

int FastPlayApp::OnExit() {
    m_server.reset();
    m_instance.reset();
    return wxApp::OnExit();
}

wxIMPLEMENT_APP(FastPlayApp);
