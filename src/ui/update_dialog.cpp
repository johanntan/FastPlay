// Help > Check for Updates: the result messages, the offer to update, and the
// download progress window.

#include "ui/ui_common.h"

#include "updater.h"
#include "paths.h"
#include "version.h"
#include "accessibility.h"
#include "app_ui.h"

#include <wx/gauge.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace {

class UpdateProgressDialog;

// State shared between the progress window and the download thread. `dialog` is
// only touched on the UI thread.
struct DownloadState {
    std::atomic<bool> cancelled{false};
    std::atomic<size_t> downloaded{0};
    std::atomic<size_t> total{0};
    wxWeakRef<UpdateProgressDialog> dialog;
};

class UpdateProgressDialog : public wxDialog {
public:
    explicit UpdateProgressDialog(wxWindow* parent, std::shared_ptr<DownloadState> state)
        : wxDialog(parent, wxID_ANY, "Downloading Update"), m_state(std::move(state)) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(330, -1));
        sizer->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 20);
        m_text = new wxStaticText(this, wxID_ANY, "Preparing download...");
        sizer->Add(m_text, 0, wxEXPAND | wxALL, 20);
        auto* cancel = new wxButton(this, wxID_CANCEL, "Cancel");
        sizer->Add(cancel, 0, wxALIGN_CENTER | wxBOTTOM, 10);
        SetSizerAndFit(sizer);
        CentreOnParent();

        // Cancel asks the download to stop; the window closes when it has.
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_state->cancelled = true; });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { m_state->cancelled = true; });
    }

    void UpdateProgress() {
        size_t total = m_state->total, downloaded = m_state->downloaded;
        int percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
        m_gauge->SetValue(percent);
        m_text->SetLabel(wxString::Format("Downloading: %.1f MB / %.1f MB (%d%%)",
                                          downloaded / (1024.0 * 1024.0), total / (1024.0 * 1024.0), percent));
    }

private:
    std::shared_ptr<DownloadState> m_state;
    wxGauge* m_gauge;
    wxStaticText* m_text;
};

void DownloadAndApply(const UpdateInfo& info) {
    std::string downloadUrl;
    if (IsInstalledMode() && !info.installerUrl.empty()) {
        downloadUrl = info.installerUrl;
    } else if (!info.downloadUrl.empty()) {
        downloadUrl = info.downloadUrl;
    } else if (!info.installerUrl.empty()) {
        downloadUrl = info.installerUrl;
    }

    auto state = std::make_shared<DownloadState>();
    auto* dialog = new UpdateProgressDialog(GetMainWindow(), state);
    state->dialog = dialog;
    dialog->Show();

    std::thread([state, downloadUrl]() {
        bool success = DownloadUpdate(downloadUrl, [state](size_t downloaded, size_t total) {
            state->downloaded = downloaded;
            state->total = total;
            RunOnUiThread([state]() {
                if (state->dialog) state->dialog->UpdateProgress();
            });
            return !state->cancelled;
        });
        bool wasCancelled = state->cancelled;

        RunOnUiThread([state]() {
            if (state->dialog) state->dialog->Destroy();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (success && !wasCancelled) {
            RunOnUiThread([]() { ApplyUpdate(); });
        } else if (!success && !wasCancelled) {
            ShowMessage(L"Failed to download update.", L"Error", MessageIcon::Error);
        }
    }).detach();
}

void HandleUpdateCheckResult(const UpdateInfo& info, bool silent) {
    wxWindow* owner = GetActiveOwner();

    if (!info.errorMessage.empty()) {
        if (!silent) {
            wxMessageBox(wxString::FromUTF8(info.errorMessage), "Check for Updates", wxOK | wxICON_ERROR, owner);
        }
        return;
    }

    if (!info.available) {
        if (!silent) {
            Speak("No updates available. You are running the latest version.");
            wxMessageBox("No updates available. You are running the latest version.",
                         "Check for Updates", wxOK | wxICON_INFORMATION, owner);
        }
        return;
    }

    std::string message = "A new version of FastPlay is available!\n\n";
    message += "Current version: " + std::string(APP_VERSION);
    if (strlen(BUILD_COMMIT) > 0) {
        message += " (" + std::string(BUILD_COMMIT).substr(0, 7) + ")";
    }
    message += "\nLatest version: " + info.latestVersion;
    if (!info.latestCommit.empty()) {
        message += " (" + info.latestCommit.substr(0, 7) + ")";
    }
    message += "\n\nDo you want to download and install the update?";

    Speak("Update available. " + info.latestVersion);

    if (wxMessageBox(wxString::FromUTF8(message), "Update Available", wxYES_NO | wxICON_QUESTION, owner) == wxYES) {
        DownloadAndApply(info);
    }
}

}  // namespace

void ShowCheckForUpdatesDialog(bool silent) {
    std::thread([silent]() {
        UpdateInfo info = CheckForUpdates();
        RunOnUiThread([info, silent]() { HandleUpdateCheckResult(info, silent); });
    }).detach();
}
