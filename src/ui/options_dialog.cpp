// The Options dialog: one notebook page per group of settings.

#include "ui/dialogs.h"
#include "ui/ui_common.h"
#include "ui/main_frame.h"
#include "ui/hotkey_dialog.h"

#include "globals.h"
#include "app_ui.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "accessibility.h"
#include "effects.h"
#include "tempo_processor.h"
#include "convolution.h"
#include "database.h"
#include "file_assoc.h"
#include "youtube.h"

#include <algorithm>
#include <cwchar>
#include <wx/notebook.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>
#include <wx/valtext.h>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Seek amount checkbox labels, with their mnemonics, in g_seekAmounts order.
const wchar_t* const kSeekLabels[] = {
    L"&1 second", L"&5 seconds", L"1&0 seconds", L"&30 seconds", L"1 &minute", L"5 m&inutes",
    L"10 min&utes", L"3&0 minutes", L"1 &hour", L"1 &track", L"5 t&racks", L"10 trac&ks"
};

const int kVolumeSteps[] = {1, 2, 5, 10, 15, 20, 25};
const int kBitrates[] = {128, 160, 192, 224, 256, 320};
const int kAAFilterLengths[] = {8, 16, 32, 64, 128};

std::wstring FileNameOnly(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::wstring HotkeyListItem(const GlobalHotkey& hk) {
    return FormatHotkey(hk.modifiers, hk.vk) + L" - " + g_hotkeyActions[hk.actionIdx].name;
}

class OptionsDialog : public wxDialog {
public:
    explicit OptionsDialog(wxWindow* parent);

private:
    // Page building helpers. Each adds a label followed by the control it names.
    wxCheckBox* AddCheck(wxWindow* page, wxBoxSizer* sizer, const wxString& label, bool checked);
    wxChoice* AddChoice(wxWindow* page, wxSizer* sizer, const wxString& label, int width = -1);
    wxTextCtrl* AddEdit(wxWindow* page, wxSizer* sizer, const wxString& label, const wxString& value,
                        int width, long style = 0);
    void AddText(wxWindow* page, wxSizer* sizer, const wxString& text);
    wxBoxSizer* AddRow(wxSizer* sizer);

    void BuildPlaybackPage(wxNotebook* book);
    void BuildRecordingPage(wxNotebook* book);
    void BuildDownloadsPage(wxNotebook* book);
    void BuildSpeechPage(wxNotebook* book);
    void BuildMovementPage(wxNotebook* book);
    void BuildHotkeysPage(wxNotebook* book);
    void BuildEffectsPage(wxNotebook* book);
    void BuildAdvancedPage(wxNotebook* book);
    void BuildYouTubePage(wxNotebook* book);
    void BuildYouTubeDownloadsPage(wxNotebook* book);
    void BuildSoundTouchPage(wxNotebook* book);
    void BuildSpeedyPage(wxNotebook* book);
    void BuildSignalsmithPage(wxNotebook* book);
    void BuildMidiPage(wxNotebook* book);

    void OnOK(wxCommandEvent& event);
    void OnRecBrowse(wxCommandEvent& event);
    void OnDownloadBrowse(wxCommandEvent& event);
    void OnRecFormat(wxCommandEvent& event);
    void OnYtdlpBrowse(wxCommandEvent& event);
    void OnImportCookies(wxCommandEvent& event);
    void OnYtFolderBrowse(wxCommandEvent& event);
    void UpdateYtDownloadControls();
    void OnRemoveCookies(wxCommandEvent& event);
    void UpdateCookiesStatus();
    void OnMidiBrowse(wxCommandEvent& event);
    void OnConvBrowse(wxCommandEvent& event);
    void OnResetListOrder(wxCommandEvent& event);
    void OnHotkeyAdd(wxCommandEvent& event);
    void OnHotkeyEdit(wxCommandEvent& event);
    void OnHotkeyRemove(wxCommandEvent& event);
    void OnHotkeyEnabled(wxCommandEvent& event);

    wxNotebook* m_book = nullptr;

    // Playback
    wxChoice* m_soundcard = nullptr;
    std::vector<int> m_deviceIndexes;  // BASS device number for each sound card entry
    wxCheckBox* m_allowAmplify = nullptr;
    wxCheckBox* m_rememberState = nullptr;
    wxChoice* m_rememberPos = nullptr;
    wxCheckBox* m_bringToFront = nullptr;
    wxCheckBox* m_loadFolder = nullptr;
    wxCheckBox* m_minimizeToTray = nullptr;
    wxCheckBox* m_showTitle = nullptr;
    wxCheckBox* m_autoAdvance = nullptr;
    wxCheckBox* m_playlistFollow = nullptr;
    wxCheckBox* m_checkUpdates = nullptr;
    wxCheckBox* m_multiInstance = nullptr;
    wxCheckBox* m_registerFileTypes = nullptr;
    wxTextCtrl* m_rewindOnPause = nullptr;
    wxChoice* m_volumeStep = nullptr;
    wxChoice* m_replayGainMode = nullptr;
    wxTextCtrl* m_replayGainPreamp = nullptr;
    wxCheckBox* m_replayGainClip = nullptr;

    // Recording
    wxTextCtrl* m_recPath = nullptr;
    wxTextCtrl* m_recTemplate = nullptr;
    wxChoice* m_recFormat = nullptr;
    wxChoice* m_recBitrate = nullptr;

    // Downloads
    wxTextCtrl* m_downloadPath = nullptr;
    wxCheckBox* m_downloadOrganize = nullptr;

    // Speech
    wxCheckBox* m_speechTrackChange = nullptr;
    wxCheckBox* m_speechVolume = nullptr;
    wxCheckBox* m_speechEffect = nullptr;

    // Movement
    std::vector<wxCheckBox*> m_seekChecks;
    wxCheckBox* m_chapterSeek = nullptr;

    // Global Hotkeys
    wxCheckBox* m_hotkeyEnabled = nullptr;
    wxListBox* m_hotkeyList = nullptr;

    // Effects
    wxCheckBox* m_effectVolume = nullptr;
    wxCheckBox* m_effectPitch = nullptr;
    wxCheckBox* m_effectTempo = nullptr;
    wxCheckBox* m_effectRate = nullptr;
    wxChoice* m_rateStepMode = nullptr;
    wxChoice* m_reverb = nullptr;
    wxCheckBox* m_dspEcho = nullptr;
    wxCheckBox* m_dspEQ = nullptr;
    wxCheckBox* m_dspCompressor = nullptr;
    wxCheckBox* m_dspStereoWidth = nullptr;
    wxCheckBox* m_dspCenterCancel = nullptr;
    wxCheckBox* m_dspSpatial = nullptr;
    wxCheckBox* m_dspConvolution = nullptr;
    wxTextCtrl* m_convIR = nullptr;

    // Advanced
    wxChoice* m_bufferSize = nullptr;
    wxChoice* m_updatePeriod = nullptr;
    wxChoice* m_tempoAlgorithm = nullptr;
    wxTextCtrl* m_eqBassFreq = nullptr;
    wxTextCtrl* m_eqMidFreq = nullptr;
    wxTextCtrl* m_eqTrebleFreq = nullptr;
    wxCheckBox* m_legacyVolume = nullptr;
    wxCheckBox* m_disableBatch = nullptr;

    // YouTube
    wxTextCtrl* m_ytdlpPath = nullptr;
    wxStaticText* m_cookiesStatus = nullptr;
    wxChoice* m_ytAutoRefresh = nullptr;
    // YouTube downloads
    wxTextCtrl* m_ytFolder = nullptr;
    wxChoice* m_ytType = nullptr;
    wxChoice* m_ytAudioFormat = nullptr;
    wxChoice* m_ytAudioQuality = nullptr;
    wxChoice* m_ytVideoQuality = nullptr;
    wxChoice* m_ytVideoContainer = nullptr;
    wxChoice* m_ytVideoCodec = nullptr;
    wxChoice* m_ytNaming = nullptr;
    wxCheckBox* m_ytAddMetadata = nullptr;
    wxCheckBox* m_ytEmbedThumbnail = nullptr;
    wxCheckBox* m_ytWriteThumbnail = nullptr;
    wxCheckBox* m_ytWriteDescription = nullptr;
    wxCheckBox* m_ytWriteSubtitles = nullptr;
    wxCheckBox* m_ytEmbedSubtitles = nullptr;
    wxCheckBox* m_ytChannelFolder = nullptr;
    wxTextCtrl* m_ytExtraOptions = nullptr;
    wxButton* m_removeCookies = nullptr;
    wxTextCtrl* m_ytApiKey = nullptr;

    // SoundTouch
    wxCheckBox* m_stAAFilter = nullptr;
    wxChoice* m_stAALength = nullptr;
    wxCheckBox* m_stQuickAlgo = nullptr;
    wxCheckBox* m_stPreventClick = nullptr;
    wxChoice* m_stAlgorithm = nullptr;
    wxTextCtrl* m_stSequence = nullptr;
    wxTextCtrl* m_stSeekWindow = nullptr;
    wxTextCtrl* m_stOverlap = nullptr;

    // Speedy
    wxCheckBox* m_speedyNonlinear = nullptr;

    // Signalsmith
    wxChoice* m_ssPreset = nullptr;
    wxTextCtrl* m_ssTonality = nullptr;

    // MIDI
    wxTextCtrl* m_midiSoundFont = nullptr;
    wxTextCtrl* m_midiVoices = nullptr;
    wxCheckBox* m_midiSinc = nullptr;
};

OptionsDialog::OptionsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Options") {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Tab order: the tabs, then the selected page's controls, then OK / Cancel.
    m_book = new wxNotebook(this, wxID_ANY);
    BuildPlaybackPage(m_book);
    BuildRecordingPage(m_book);
    BuildDownloadsPage(m_book);
    BuildSpeechPage(m_book);
    BuildMovementPage(m_book);
    BuildHotkeysPage(m_book);
    BuildEffectsPage(m_book);
    BuildAdvancedPage(m_book);
    BuildYouTubePage(m_book);
    BuildYouTubeDownloadsPage(m_book);
    BuildSoundTouchPage(m_book);
    BuildSpeedyPage(m_book);
    BuildSignalsmithPage(m_book);
    BuildMidiPage(m_book);
    sizer->Add(m_book, 1, wxEXPAND | wxALL, 10);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* ok = new wxButton(this, wxID_OK, "OK");
    ok->SetDefault();
    buttons->Add(ok, 0, wxRIGHT, 6);
    buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    Bind(wxEVT_BUTTON, &OptionsDialog::OnOK, this, wxID_OK);

    SetSizerAndFit(sizer);
    CentreOnParent();

    // Show the Playback tab first
    m_book->SetSelection(0);
    m_book->SetFocus();
}

wxCheckBox* OptionsDialog::AddCheck(wxWindow* page, wxBoxSizer* sizer, const wxString& label, bool checked) {
    auto* check = new wxCheckBox(page, wxID_ANY, label);
    check->SetValue(checked);
    if (sizer->GetOrientation() == wxHORIZONTAL) {
        sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL);
    } else {
        sizer->Add(check, 0, wxTOP | wxBOTTOM, 3);
    }
    return check;
}

wxChoice* OptionsDialog::AddChoice(wxWindow* page, wxSizer* sizer, const wxString& label, int width) {
    if (!label.empty()) {
        sizer->Add(new wxStaticText(page, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    }
    auto* choice = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(width, -1));
    sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    return choice;
}

wxTextCtrl* OptionsDialog::AddEdit(wxWindow* page, wxSizer* sizer, const wxString& label, const wxString& value,
                                   int width, long style) {
    if (!label.empty()) {
        sizer->Add(new wxStaticText(page, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    }
    auto* edit = new wxTextCtrl(page, wxID_ANY, value, wxDefaultPosition, wxSize(width, -1), style);
    sizer->Add(edit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    return edit;
}

void OptionsDialog::AddText(wxWindow* page, wxSizer* sizer, const wxString& text) {
    sizer->Add(new wxStaticText(page, wxID_ANY, text), 0, wxTOP | wxBOTTOM, 3);
}

wxBoxSizer* OptionsDialog::AddRow(wxSizer* sizer) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(row, 0, wxTOP | wxBOTTOM, 3);
    return row;
}

void OptionsDialog::BuildPlaybackPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Populate sound card combo box
    m_soundcard = AddChoice(page, AddRow(sizer), "&Output device:", 330);
    int currentIndex = 0;
    for (const auto& dev : GetAudioDevices()) {
        int idx = m_soundcard->Append(WX(dev.name));
        m_deviceIndexes.push_back(dev.index);
        if (dev.current) {
            currentIndex = idx;
        }
    }
    if (!m_deviceIndexes.empty()) {
        m_soundcard->SetSelection(currentIndex);
    }

    m_allowAmplify = AddCheck(page, sizer, "&Allow volume above 100%", g_allowAmplify);
    m_rememberState = AddCheck(page, sizer, "&Remember playback state on exit", g_rememberState);

    // Populate remember position combo box
    {
        m_rememberPos = AddChoice(page, AddRow(sizer), "Remember p&osition if longer than:");
        const wchar_t* posLabels[] = {L"Off", L"5 minutes", L"10 minutes", L"20 minutes", L"30 minutes", L"45 minutes", L"60 minutes"};
        int posIndex = 0;
        for (int i = 0; i < g_posThresholdCount; i++) {
            m_rememberPos->Append(posLabels[i]);
            if (g_posThresholds[i] == g_rememberPosMinutes) {
                posIndex = i;
            }
        }
        m_rememberPos->SetSelection(posIndex);
    }

    m_bringToFront = AddCheck(page, sizer, "&Bring window to front when opening files", g_bringToFront);
    m_loadFolder = AddCheck(page, sizer, "&Load all files in folder when opening single file", g_loadFolder);
    m_minimizeToTray = AddCheck(page, sizer, "&Minimize to system tray", g_minimizeToTray);
    m_showTitle = AddCheck(page, sizer, "Show &track name in window title", g_showTitleInWindow);
    m_autoAdvance = AddCheck(page, sizer, "Auto-ad&vance to next playlist item", g_autoAdvance);
    m_playlistFollow = AddCheck(page, sizer, "&Follow playback in playlist dialog", g_playlistFollowPlayback);
    m_checkUpdates = AddCheck(page, sizer, "Check for &updates on startup", g_checkForUpdates);
    m_multiInstance = AddCheck(page, sizer, "Allow &multiple instances", g_allowMultipleInstances);
    m_registerFileTypes = AddCheck(page, sizer, "Register all supported &file types", g_registerFileTypes);

    m_rewindOnPause = AddEdit(page, AddRow(sizer), "Re&wind on pause (ms):",
                              wxString::Format("%u", static_cast<unsigned>(g_rewindOnPauseMs)), 60);
    SetDigitsOnly(m_rewindOnPause);

    // Populate volume step combo box
    {
        m_volumeStep = AddChoice(page, AddRow(sizer), "Volu&me step:");
        int stepIndex = 1;  // Default to 2%
        for (int i = 0; i < 7; i++) {
            m_volumeStep->Append(wxString::Format("%d%%", kVolumeSteps[i]));
            if (static_cast<int>(g_volumeStep * 100 + 0.5f) == kVolumeSteps[i]) {
                stepIndex = i;
            }
        }
        m_volumeStep->SetSelection(stepIndex);
    }

    // Populate ReplayGain controls
    {
        auto* row = AddRow(sizer);
        m_replayGainMode = AddChoice(page, row, "Replay&Gain:");
        m_replayGainMode->Append("Off");
        m_replayGainMode->Append("Track");
        m_replayGainMode->Append("Album");
        int rgMode = (g_replayGainMode >= 0 && g_replayGainMode <= 2) ? g_replayGainMode : 0;
        m_replayGainMode->SetSelection(rgMode);

        m_replayGainPreamp = AddEdit(page, row, "Pre&amp (dB):", wxString::Format("%g", g_replayGainPreamp), 50);

        m_replayGainClip = AddCheck(page, sizer, "Prevent clippin&g", g_replayGainPreventClip);
    }

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Playback");
}

void OptionsDialog::BuildRecordingPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Set default recording path to user's Music folder if not set
    if (g_recordPath.empty()) {
        wxString musicPath = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Music);
        if (!musicPath.empty()) {
            g_recordPath = WS(musicPath);
        }
    }

    AddText(page, sizer, "Record audio output to file. Press R to toggle recording.");

    auto* row = AddRow(sizer);
    m_recPath = AddEdit(page, row, "&Output folder:", WX(g_recordPath), 280);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnRecBrowse, this);

    m_recTemplate = AddEdit(page, AddRow(sizer), "Filename &template:", WX(g_recordTemplate), 250);
    AddText(page, sizer, "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)");

    // Format combo: WAV, MP3, OGG, FLAC
    row = AddRow(sizer);
    m_recFormat = AddChoice(page, row, "&Format:");
    m_recFormat->Append("WAV (lossless)");
    m_recFormat->Append("MP3");
    m_recFormat->Append("OGG Vorbis");
    m_recFormat->Append("FLAC (lossless)");
    m_recFormat->SetSelection(g_recordFormat);
    m_recFormat->Bind(wxEVT_CHOICE, &OptionsDialog::OnRecFormat, this);

    // Bitrate combo (for MP3/OGG)
    m_recBitrate = AddChoice(page, row, "&Bitrate:");
    int bitrateIndex = 2;  // Default to 192
    for (int i = 0; i < 6; i++) {
        m_recBitrate->Append(wxString::Format("%d kbps", kBitrates[i]));
        if (kBitrates[i] == g_recordBitrate) {
            bitrateIndex = i;
        }
    }
    m_recBitrate->SetSelection(bitrateIndex);

    // Enable bitrate only for lossy formats (MP3=1, OGG=2)
    m_recBitrate->Enable(g_recordFormat == 1 || g_recordFormat == 2);

    AddText(page, sizer, "(Bitrate only applies to MP3 and OGG formats)");

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Recording");
}

void OptionsDialog::BuildDownloadsPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Configure podcast episode download settings.");
    AddText(page, sizer, "&Downloads folder:");

    auto* row = AddRow(sizer);
    m_downloadPath = new wxTextCtrl(page, wxID_ANY, WX(g_downloadPath), wxDefaultPosition, wxSize(380, -1),
                                    wxTE_READONLY);
    row->Add(m_downloadPath, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnDownloadBrowse, this);

    m_downloadOrganize = AddCheck(page, sizer, "&Organize downloads into folders by feed title", g_downloadOrganizeByFeed);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Downloads");
}

void OptionsDialog::BuildSpeechPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Configure speech feedback for various events.");
    m_speechTrackChange = AddCheck(page, sizer, "&Announce track changes", g_speechTrackChange);
    m_speechVolume = AddCheck(page, sizer, "Speak &volume when adjusted", g_speechVolume);
    m_speechEffect = AddCheck(page, sizer, "Speak &effect value when adjusted", g_speechEffect);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Speech");
}

void OptionsDialog::BuildMovementPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Seek amounts (use , and . to cycle):");

    // Seek amount checkboxes
    const int labelCount = static_cast<int>(sizeof(kSeekLabels) / sizeof(kSeekLabels[0]));
    for (int i = 0; i < g_seekAmountCount; i++) {
        wxString label = (i < labelCount) ? wxString(kSeekLabels[i]) : wxString(g_seekAmounts[i].label);
        m_seekChecks.push_back(AddCheck(page, sizer, label, g_seekEnabled[i]));
    }
    m_chapterSeek = AddCheck(page, sizer, "1 c&hapter (if available)", g_chapterSeekEnabled);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Movement");
}

void OptionsDialog::BuildHotkeysPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Populate hotkey list and set enabled checkbox
    m_hotkeyEnabled = AddCheck(page, sizer, "&Enable global hotkeys", g_hotkeysEnabled);
    m_hotkeyEnabled->Bind(wxEVT_CHECKBOX, &OptionsDialog::OnHotkeyEnabled, this);

    m_hotkeyList = new wxListBox(page, wxID_ANY, wxDefaultPosition, wxSize(430, 180), 0, nullptr, wxLB_SINGLE);
    for (const auto& hk : g_hotkeys) {
        m_hotkeyList->Append(WX(HotkeyListItem(hk)));
    }
    sizer->Add(m_hotkeyList, 1, wxEXPAND | wxTOP | wxBOTTOM, 3);

    auto* row = AddRow(sizer);
    auto* add = new wxButton(page, wxID_ANY, "&Add...");
    auto* edit = new wxButton(page, wxID_ANY, "&Edit...");
    auto* remove = new wxButton(page, wxID_ANY, "&Remove");
    row->Add(add, 0, wxRIGHT, 6);
    row->Add(edit, 0, wxRIGHT, 6);
    row->Add(remove);
    add->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyAdd, this);
    edit->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyEdit, this);
    remove->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyRemove, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Global Hotkeys");
}

void OptionsDialog::BuildEffectsPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Set effect checkboxes
    AddText(page, sizer, "Stream effects ([ ] to cycle, Up/Down to adjust):");
    m_effectVolume = AddCheck(page, sizer, "&Volume (0-400%)", g_effectEnabled[0]);
    m_effectPitch = AddCheck(page, sizer, "&Pitch (-12 to +12 semitones)", g_effectEnabled[1]);
    m_effectTempo = AddCheck(page, sizer, "&Tempo (-50% to +100%)", g_effectEnabled[2]);

    auto* row = AddRow(sizer);
    m_effectRate = AddCheck(page, row, "Playback &Rate (0.5x - 2x)", g_effectEnabled[3]);
    row->AddSpacer(12);

    // Set rate step mode combobox
    m_rateStepMode = AddChoice(page, row, "Step:");
    m_rateStepMode->Append("0.01x");
    m_rateStepMode->Append("Semitone");
    m_rateStepMode->SetSelection(g_rateStepMode);

    AddText(page, sizer, "DSP effects (enable to add their parameters to cycle list):");

    // Set reverb algorithm combobox
    m_reverb = AddChoice(page, AddRow(sizer), "Re&verb:", 200);
    m_reverb->Append("Off");
    m_reverb->Append("Simple");
    m_reverb->Append("Advanced (EFX)");
    m_reverb->SetSelection(g_reverbAlgorithm);

    // Set DSP effect checkboxes
    m_dspEcho = AddCheck(page, sizer, "&Echo", IsDSPEffectEnabled(DSPEffectType::Echo));
    m_dspEQ = AddCheck(page, sizer, "E&Q (Bass/Mid/Treble)", IsDSPEffectEnabled(DSPEffectType::EQ));
    m_dspCompressor = AddCheck(page, sizer, "&Compressor", IsDSPEffectEnabled(DSPEffectType::Compressor));
    m_dspStereoWidth = AddCheck(page, sizer, "&Stereo Width (0-200%)", IsDSPEffectEnabled(DSPEffectType::StereoWidth));
    m_dspCenterCancel = AddCheck(page, sizer, "Ce&nter Cancel (-100 to +100%)", IsDSPEffectEnabled(DSPEffectType::CenterCancel));
    m_dspSpatial = AddCheck(page, sizer, "&3D Audio (HRTF/Binaural)", IsDSPEffectEnabled(DSPEffectType::SpatialAudio));

    row = AddRow(sizer);
    m_dspConvolution = AddCheck(page, row, "Co&nvolution Reverb", IsDSPEffectEnabled(DSPEffectType::Convolution));
    row->AddSpacer(12);

    // Display current IR file path (just filename)
    m_convIR = AddEdit(page, row, "IR File:", g_convolutionIRPath.empty() ? wxString() : WX(FileNameOnly(g_convolutionIRPath)),
                       160, wxTE_READONLY);
    auto* browse = new wxButton(page, wxID_ANY, "...", wxDefaultPosition, wxSize(30, -1));
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnConvBrowse, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Effects");
}

void OptionsDialog::BuildAdvancedPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Audio buffer settings (changes apply on next file load):");

    // Populate buffer size combo box
    {
        m_bufferSize = AddChoice(page, AddRow(sizer), "&Buffer size:", 150);
        int bufferIndex = 3;  // Default to 500ms
        for (int i = 0; i < g_bufferSizeCount; i++) {
            m_bufferSize->Append(wxString::Format("%d ms", g_bufferSizes[i]));
            if (g_bufferSizes[i] == g_bufferSize) {
                bufferIndex = i;
            }
        }
        m_bufferSize->SetSelection(bufferIndex);
    }

    // Populate update period combo box
    {
        m_updatePeriod = AddChoice(page, AddRow(sizer), "&Update period:", 150);
        int updateIndex = 4;  // Default to 100ms
        for (int i = 0; i < g_updatePeriodCount; i++) {
            m_updatePeriod->Append(wxString::Format("%d ms", g_updatePeriods[i]));
            if (g_updatePeriods[i] == g_updatePeriod) {
                updateIndex = i;
            }
        }
        m_updatePeriod->SetSelection(updateIndex);
    }

    AddText(page, sizer, "Lower values reduce latency but may cause audio glitches.");

    // Populate tempo algorithm combo box
    AddText(page, sizer, "Tempo/pitch &algorithm (changes apply on next file load):");
    m_tempoAlgorithm = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(330, -1));
    sizer->Add(m_tempoAlgorithm, 0, wxTOP | wxBOTTOM, 3);
    m_tempoAlgorithm->Append("SoundTouch (BASS_FX) - Fast, good for speech");
#ifdef USE_SPEEDY
    m_tempoAlgorithm->Append("Speedy (Google) - Nonlinear speech speedup");
#else
    m_tempoAlgorithm->Append("Speedy (coming soon)");
#endif
#ifdef USE_SIGNALSMITH
    m_tempoAlgorithm->Append("Signalsmith Stretch - High quality time/pitch");
#else
    m_tempoAlgorithm->Append("Signalsmith (coming soon)");
#endif
    m_tempoAlgorithm->SetSelection(g_tempoAlgorithm);

    // Initialize EQ frequency edit controls
    AddText(page, sizer, "EQ frequencies (Hz) - changes apply on next EQ enable:");
    auto* row = AddRow(sizer);
    m_eqBassFreq = AddEdit(page, row, "Bass (20-500):", wxString::Format("%.0f", g_eqBassFreq), 60);
    SetDigitsOnly(m_eqBassFreq);
    m_eqMidFreq = AddEdit(page, row, "Mid (200-5k):", wxString::Format("%.0f", g_eqMidFreq), 60);
    SetDigitsOnly(m_eqMidFreq);
    m_eqTrebleFreq = AddEdit(page, row, "Treble (2k-20k):", wxString::Format("%.0f", g_eqTrebleFreq), 60);
    SetDigitsOnly(m_eqTrebleFreq);

    // Initialize legacy volume and disable batch delay checkboxes
    m_legacyVolume = AddCheck(page, sizer, "&Legacy volume (faster, but affects recordings)", g_legacyVolume);
    m_disableBatch = AddCheck(page, sizer, "Disable &batch delay (only catches one file at a time)", g_disableBatchDelay);

    auto* reset = new wxButton(page, wxID_ANY, "Reset station/podcast &order to alphabetical");
    sizer->Add(reset, 0, wxTOP | wxBOTTOM, 3);
    reset->Bind(wxEVT_BUTTON, &OptionsDialog::OnResetListOrder, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Advanced");
}

void OptionsDialog::BuildYouTubePage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* row = AddRow(sizer);
    m_ytdlpPath = AddEdit(page, row, "&yt-dlp path:", WX(g_ytdlpPath), 300);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnYtdlpBrowse, this);
    AddText(page, sizer, "Optional. Leave it empty and FastPlay downloads yt-dlp itself and keeps it up to date.");

    AddText(page, sizer, "YouTube Data &API key (optional, enables search):");
    m_ytApiKey = new wxTextCtrl(page, wxID_ANY, WX(g_ytApiKey), wxDefaultPosition, wxSize(430, -1), wxTE_PASSWORD);
    sizer->Add(m_ytApiKey, 0, wxTOP | wxBOTTOM, 3);
    AddText(page, sizer, "Get an API key from: console.cloud.google.com");
    AddText(page, sizer, "Without API key, yt-dlp will be used for search (slower).");

    m_ytAutoRefresh = AddChoice(page, sizer, "Refresh &favorites:", 260);
    for (const char* item : {"Off", "At startup", "Every 30 minutes", "Every hour", "Every 2 hours", "Every 4 hours",
                             "Every 8 hours"}) {
        m_ytAutoRefresh->Append(item);
    }
    m_ytAutoRefresh->SetSelection(std::clamp(g_ytAutoRefresh, 0, 6));
    AddText(page, sizer, "Checks your favorite channels and playlists for new videos, and says when there are some.");

    // Cookies, for videos YouTube only shows to a signed-in account
    m_cookiesStatus = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(m_cookiesStatus, 0, wxTOP, 10);
    auto* cookieRow = AddRow(sizer);
    auto* importCookies = new wxButton(page, wxID_ANY, "Import &cookies.txt...");
    cookieRow->Add(importCookies, 0, wxRIGHT, 6);
    m_removeCookies = new wxButton(page, wxID_ANY, "&Remove cookies");
    cookieRow->Add(m_removeCookies);
    importCookies->Bind(wxEVT_BUTTON, &OptionsDialog::OnImportCookies, this);
    m_removeCookies->Bind(wxEVT_BUTTON, &OptionsDialog::OnRemoveCookies, this);
    AddText(page, sizer, "Export your youtube.com cookies with a browser extension (cookies.txt format) while signed in.");
    UpdateCookiesStatus();

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "YouTube");
}

void OptionsDialog::BuildYouTubeDownloadsPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const YouTubeDownloadSettings& s = g_ytDownload;

    AddText(page, sizer, "How the YouTube window's Download button saves videos.");
    AddText(page, sizer, "Download &folder:");
    auto* row = AddRow(sizer);
    m_ytFolder = new wxTextCtrl(page, wxID_ANY, WX(s.folder.empty() ? YouTubeDownloadFolder() : s.folder),
                                wxDefaultPosition, wxSize(380, -1));
    row->Add(m_ytFolder, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnYtFolderBrowse, this);

    auto choice = [&](const char* label, std::initializer_list<const char*> items, int selection) {
        wxChoice* c = AddChoice(page, sizer, label, 260);
        for (const char* item : items) c->Append(item);
        c->SetSelection(selection);
        return c;
    };
    m_ytType = choice("Download &type:", {"Audio only", "Video"}, s.type);
    m_ytAudioFormat = choice("&Audio format:",
                             {"M4A (AAC)", "Best available, as YouTube has it", "MP3", "Opus", "FLAC", "WAV"},
                             s.audioFormat);
    m_ytAudioQuality = choice("Audio &quality (when converting):",
                              {"Best", "320 kbps", "256 kbps", "192 kbps", "128 kbps"}, s.audioQuality);
    m_ytVideoQuality = choice("&Video quality:", {"Best", "2160p (4K)", "1440p", "1080p", "720p", "480p", "360p"},
                              s.videoQuality);
    m_ytVideoContainer = choice("Video &container:", {"MP4", "MKV", "WebM"}, s.videoContainer);
    m_ytVideoCodec = choice("Video co&dec:", {"Any", "H.264", "VP9", "AV1"}, s.videoCodec);
    m_ytNaming = choice("File &naming:", {"Title", "Title [video ID]", "Channel - Title", "Upload date - Title"},
                        s.naming);

    m_ytAddMetadata = AddCheck(page, sizer, "Add &metadata (title, artist, date)", s.addMetadata);
    m_ytEmbedThumbnail = AddCheck(page, sizer, "&Embed thumbnail", s.embedThumbnail);
    m_ytWriteThumbnail = AddCheck(page, sizer, "Save t&humbnail as a file", s.writeThumbnail);
    m_ytWriteDescription = AddCheck(page, sizer, "Save descr&iption as a file", s.writeDescription);
    m_ytWriteSubtitles = AddCheck(page, sizer, "Download &subtitles", s.writeSubtitles);
    m_ytEmbedSubtitles = AddCheck(page, sizer, "Embed subtit&les in videos", s.embedSubtitles);
    m_ytChannelFolder = AddCheck(page, sizer, "Put each channel in its own f&older", s.channelFolder);

    m_ytExtraOptions = AddEdit(page, sizer, "E&xtra yt-dlp options:", WX(s.extraOptions), 380);
    AddText(page, sizer, "Converting, video, metadata and thumbnails need ffmpeg, which FastPlay downloads once if it is not installed.");

    m_ytType->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateYtDownloadControls(); });
    m_ytAudioFormat->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateYtDownloadControls(); });
    UpdateYtDownloadControls();

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "YouTube Downloads");
}

// Only the settings for what is being downloaded can be changed.
void OptionsDialog::UpdateYtDownloadControls() {
    bool video = m_ytType->GetSelection() == 1;
    m_ytAudioFormat->Enable(!video);
    m_ytAudioQuality->Enable(!video && m_ytAudioFormat->GetSelection() >= 2 && m_ytAudioFormat->GetSelection() <= 3);
    m_ytVideoQuality->Enable(video);
    m_ytVideoContainer->Enable(video);
    m_ytVideoCodec->Enable(video);
    m_ytEmbedSubtitles->Enable(video);
}

void OptionsDialog::OnYtFolderBrowse(wxCommandEvent&) {
    wxDirDialog dlg(this, "Select YouTube download folder", m_ytFolder->GetValue(), wxDD_DEFAULT_STYLE);
    if (dlg.ShowModal() == wxID_OK) {
        m_ytFolder->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::BuildSoundTouchPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "SoundTouch settings (changes apply on next file load):");
    m_stAAFilter = AddCheck(page, sizer, "&Anti-alias filter", g_stAntiAliasFilter);

    // AA filter length combo (8, 16, 32, 64, 128)
    m_stAALength = AddChoice(page, AddRow(sizer), "AA filter &length:", 80);
    int aaIndex = 2;  // Default to 32
    for (int i = 0; i < 5; i++) {
        m_stAALength->Append(wxString::Format("%d", kAAFilterLengths[i]));
        if (kAAFilterLengths[i] == g_stAAFilterLength) aaIndex = i;
    }
    m_stAALength->SetSelection(aaIndex);

    m_stQuickAlgo = AddCheck(page, sizer, "&Quick algorithm (lower quality, less CPU)", g_stQuickAlgorithm);
    m_stPreventClick = AddCheck(page, sizer, "&Prevent click (reduces artifacts)", g_stPreventClick);

    // Interpolation algorithm combo
    m_stAlgorithm = AddChoice(page, AddRow(sizer), "&Interpolation:", 110);
    m_stAlgorithm->Append("Linear");
    m_stAlgorithm->Append("Cubic");
    m_stAlgorithm->Append("Shannon");
    m_stAlgorithm->SetSelection(g_stAlgorithm);

    // Sequence, seek window, overlap edit controls
    auto* row = AddRow(sizer);
    m_stSequence = AddEdit(page, row, "&Sequence (ms):", wxString::Format("%d", g_stSequenceMs), 50);
    SetDigitsOnly(m_stSequence);
    m_stSeekWindow = AddEdit(page, row, "See&k window:", wxString::Format("%d", g_stSeekWindowMs), 50);
    SetDigitsOnly(m_stSeekWindow);
    m_stOverlap = AddEdit(page, row, "&Overlap:", wxString::Format("%d", g_stOverlapMs), 40);
    SetDigitsOnly(m_stOverlap);

    AddText(page, sizer, "(0 = automatic for Sequence/Seek window)");

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "SoundTouch");
}

void OptionsDialog::BuildSpeedyPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Google Speedy algorithm settings:");
    AddText(page, sizer, "Speedy uses nonlinear speedup optimized for speech.");
    AddText(page, sizer, "It compresses vowels more than consonants for clarity.");
    m_speedyNonlinear = AddCheck(page, sizer, "&Enable nonlinear speedup (recommended for speech)", g_speedyNonlinear);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Speedy");
}

void OptionsDialog::BuildSignalsmithPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Signalsmith Stretch settings (changes apply on next file load):");

    m_ssPreset = AddChoice(page, AddRow(sizer), "&Quality preset:", 180);
    m_ssPreset->Append("Default (higher quality)");
    m_ssPreset->Append("Cheaper (lower CPU)");
    m_ssPreset->SetSelection(g_ssPreset);

    m_ssTonality = AddEdit(page, AddRow(sizer), "&Tonality limit (Hz, 0 = auto):",
                           wxString::Format("%d", g_ssTonalityLimit), 70);
    SetDigitsOnly(m_ssTonality);

    AddText(page, sizer, "Higher tonality limits preserve more harmonics during pitch shift.");
    AddText(page, sizer, "Use 0 for automatic, or 4000-8000 for speech, 8000-16000 for music.");

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "Signalsmith");
}

void OptionsDialog::BuildMidiPage(wxNotebook* book) {
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "MIDI playback settings (BASSMIDI):");
    AddText(page, sizer, "&SoundFont (.sf2/.sf3):");

    auto* row = AddRow(sizer);
    m_midiSoundFont = AddEdit(page, row, wxString(), WX(g_midiSoundFont), 340);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnMidiBrowse, this);

    row = AddRow(sizer);
    m_midiVoices = AddEdit(page, row, "Ma&x voices (polyphony):", wxString::Format("%d", g_midiMaxVoices), 60);
    SetDigitsOnly(m_midiVoices);
    row->Add(new wxStaticText(page, wxID_ANY, "(1-1000, default 128)"), 0, wxALIGN_CENTER_VERTICAL);

    m_midiSinc = AddCheck(page, sizer, "Use s&inc interpolation (higher quality, more CPU)", g_midiSincInterp);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    book->AddPage(page, "MIDI");
}

void OptionsDialog::OnOK(wxCommandEvent&) {
    // Get selected device
    int sel = m_soundcard->GetSelection();
    int newDevice = (sel >= 0 && sel < static_cast<int>(m_deviceIndexes.size())) ? m_deviceIndexes[sel] : -1;

    // Get amplify setting
    bool newAmplify = m_allowAmplify->GetValue();

    // Get remember playback state setting
    g_rememberState = m_rememberState->GetValue();

    // Get bring to front setting
    g_bringToFront = m_bringToFront->GetValue();
    g_loadFolder = m_loadFolder->GetValue();
    g_minimizeToTray = m_minimizeToTray->GetValue();
    g_showTitleInWindow = m_showTitle->GetValue();
    g_autoAdvance = m_autoAdvance->GetValue();
    g_playlistFollowPlayback = m_playlistFollow->GetValue();
    g_checkForUpdates = m_checkUpdates->GetValue();
    g_allowMultipleInstances = m_multiInstance->GetValue();
    UpdateWindowTitle();  // Apply immediately

    {
        unsigned int rewind = 0;
        if (!m_rewindOnPause->GetValue().ToUInt(&rewind)) rewind = 0;
        g_rewindOnPauseMs = static_cast<int>(rewind);
        if (g_rewindOnPauseMs < 0) g_rewindOnPauseMs = 0;
    }

    // Get download settings
    g_downloadPath = WS(m_downloadPath->GetValue());
    g_downloadOrganizeByFeed = m_downloadOrganize->GetValue();

    // Get volume step setting
    {
        int volStepSel = m_volumeStep->GetSelection();
        if (volStepSel >= 0 && volStepSel < 7) {
            g_volumeStep = kVolumeSteps[volStepSel] / 100.0f;
        }
    }

    // Get ReplayGain settings
    {
        int rgSel = m_replayGainMode->GetSelection();
        if (rgSel >= 0 && rgSel <= 2) g_replayGainMode = rgSel;

        float preamp = static_cast<float>(std::wcstod(WS(m_replayGainPreamp->GetValue()).c_str(), nullptr));
        if (preamp < -15.0f) preamp = -15.0f;
        if (preamp > 15.0f) preamp = 15.0f;
        g_replayGainPreamp = preamp;

        g_replayGainPreventClip = m_replayGainClip->GetValue();

        // Re-apply to the currently playing track so the change is audible immediately
        RefreshReplayGain();
    }

    // Get remember position threshold
    int posSel = m_rememberPos->GetSelection();
    if (posSel >= 0 && posSel < g_posThresholdCount) {
        g_rememberPosMinutes = g_posThresholds[posSel];
    }

    // Apply device change if needed
    if (newDevice != g_selectedDevice) {
        ReinitBass(newDevice);
    }

    // Apply amplify setting
    g_allowAmplify = newAmplify;

    // Clamp volume if amplify was disabled
    if (!g_allowAmplify && g_volume > MAX_VOLUME_NORMAL) {
        SetVolume(MAX_VOLUME_NORMAL);
    }

    // Get seek amount checkboxes
    for (int i = 0; i < g_seekAmountCount; i++) {
        g_seekEnabled[i] = m_seekChecks[i]->GetValue();
    }
    g_chapterSeekEnabled = m_chapterSeek->GetValue();

    // Validate current seek index - ensure it points to an enabled amount
    if (!g_seekEnabled[g_currentSeekIndex]) {
        for (int i = 0; i < g_seekAmountCount; i++) {
            if (g_seekEnabled[i]) {
                g_currentSeekIndex = i;
                break;
            }
        }
    }

    // Update file types registration setting
    {
        bool newRegister = m_registerFileTypes->GetValue();
        if (newRegister && !g_registerFileTypes) {
            // Just enabled - register all file types now
            RegisterAllFileTypes();
        } else if (!newRegister && g_registerFileTypes) {
            // Just disabled - unregister all file types now
            UnregisterAllFileTypes();
        }
        g_registerFileTypes = newRegister;
    }

    // Get effect checkboxes
    g_effectEnabled[0] = m_effectVolume->GetValue();
    g_effectEnabled[1] = m_effectPitch->GetValue();
    g_effectEnabled[2] = m_effectTempo->GetValue();
    g_effectEnabled[3] = m_effectRate->GetValue();

    // Get rate step mode
    {
        int rateStepSel = m_rateStepMode->GetSelection();
        if (rateStepSel >= 0 && rateStepSel <= 1) {
            g_rateStepMode = rateStepSel;
        }
    }

    // Validate current effect index - ensure it points to an enabled effect
    if (!g_effectEnabled[g_currentEffectIndex]) {
        for (int i = 0; i < 4; i++) {
            if (g_effectEnabled[i]) {
                g_currentEffectIndex = i;
                break;
            }
        }
    }

    // Get reverb algorithm from combobox
    {
        int reverbSel = m_reverb->GetSelection();
        if (reverbSel >= 0 && reverbSel < static_cast<int>(ReverbAlgorithm::COUNT)) {
            SetReverbAlgorithm(reverbSel);
        }
    }

    // Get DSP effect checkboxes and enable/disable effects
    EnableDSPEffect(DSPEffectType::Echo, m_dspEcho->GetValue());
    EnableDSPEffect(DSPEffectType::EQ, m_dspEQ->GetValue());
    EnableDSPEffect(DSPEffectType::Compressor, m_dspCompressor->GetValue());
    EnableDSPEffect(DSPEffectType::StereoWidth, m_dspStereoWidth->GetValue());
    EnableDSPEffect(DSPEffectType::CenterCancel, m_dspCenterCancel->GetValue());
    EnableDSPEffect(DSPEffectType::Convolution, m_dspConvolution->GetValue());
    EnableDSPEffect(DSPEffectType::SpatialAudio, m_dspSpatial->GetValue());

    // Get buffer settings
    {
        int bufferSel = m_bufferSize->GetSelection();
        if (bufferSel >= 0 && bufferSel < g_bufferSizeCount) {
            g_bufferSize = g_bufferSizes[bufferSel];
            BASS_SetConfig(BASS_CONFIG_BUFFER, g_bufferSize);
        }

        int updateSel = m_updatePeriod->GetSelection();
        if (updateSel >= 0 && updateSel < g_updatePeriodCount) {
            g_updatePeriod = g_updatePeriods[updateSel];
            BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, g_updatePeriod);
        }

        int algoSel = m_tempoAlgorithm->GetSelection();
        if (algoSel >= 0 && algoSel < static_cast<int>(TempoAlgorithm::COUNT)) {
            g_tempoAlgorithm = algoSel;
        }

        // Get EQ frequencies
        float bassFreq = static_cast<float>(std::wcstod(WS(m_eqBassFreq->GetValue()).c_str(), nullptr));
        if (bassFreq >= 20.0f && bassFreq <= 500.0f) g_eqBassFreq = bassFreq;

        float midFreq = static_cast<float>(std::wcstod(WS(m_eqMidFreq->GetValue()).c_str(), nullptr));
        if (midFreq >= 200.0f && midFreq <= 5000.0f) g_eqMidFreq = midFreq;

        float trebleFreq = static_cast<float>(std::wcstod(WS(m_eqTrebleFreq->GetValue()).c_str(), nullptr));
        if (trebleFreq >= 2000.0f && trebleFreq <= 20000.0f) g_eqTrebleFreq = trebleFreq;

        // Get legacy volume setting
        bool wasLegacy = g_legacyVolume;
        g_legacyVolume = m_legacyVolume->GetValue();

        // Get disable batch delay setting
        g_disableBatchDelay = m_disableBatch->GetValue();

        // Handle mode switch
        if (wasLegacy != g_legacyVolume && g_fxStream) {
            if (g_legacyVolume) {
                // Switching TO legacy: apply volume via BASS_ATTRIB_VOL
                float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
            } else {
                // Switching FROM legacy: reset BASS_ATTRIB_VOL to 1.0 so DSP works
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, 1.0f);
                // Ensure volume DSP is set up
                ApplyDSPEffects();
            }
        }
    }

    // Get YouTube settings
    g_ytdlpPath = WS(m_ytdlpPath->GetValue());
    g_ytApiKey = WS(m_ytApiKey->GetValue());
    g_ytAutoRefresh = m_ytAutoRefresh->GetSelection();
    StartYouTubeAutoRefresh(false);

    // Get YouTube download settings. The folder is only stored when it is not
    // the default, so a moved Downloads folder is still followed.
    {
        YouTubeDownloadSettings& s = g_ytDownload;
        std::wstring folder = WS(m_ytFolder->GetValue());
        s.folder.clear();  // so YouTubeDownloadFolder() gives the default to compare with
        s.folder = folder == YouTubeDownloadFolder() ? L"" : folder;
        s.type = m_ytType->GetSelection();
        s.audioFormat = m_ytAudioFormat->GetSelection();
        s.audioQuality = m_ytAudioQuality->GetSelection();
        s.videoQuality = m_ytVideoQuality->GetSelection();
        s.videoContainer = m_ytVideoContainer->GetSelection();
        s.videoCodec = m_ytVideoCodec->GetSelection();
        s.naming = m_ytNaming->GetSelection();
        s.addMetadata = m_ytAddMetadata->GetValue();
        s.embedThumbnail = m_ytEmbedThumbnail->GetValue();
        s.writeThumbnail = m_ytWriteThumbnail->GetValue();
        s.writeDescription = m_ytWriteDescription->GetValue();
        s.writeSubtitles = m_ytWriteSubtitles->GetValue();
        s.embedSubtitles = m_ytEmbedSubtitles->GetValue();
        s.channelFolder = m_ytChannelFolder->GetValue();
        s.extraOptions = WS(m_ytExtraOptions->GetValue());
    }

    // Get Recording settings
    {
        g_recordPath = WS(m_recPath->GetValue());
        g_recordTemplate = WS(m_recTemplate->GetValue());

        int formatSel = m_recFormat->GetSelection();
        if (formatSel >= 0 && formatSel <= 3) g_recordFormat = formatSel;

        int bitrateSel = m_recBitrate->GetSelection();
        if (bitrateSel >= 0 && bitrateSel < 6) g_recordBitrate = kBitrates[bitrateSel];
    }

    // Get Speech settings
    g_speechTrackChange = m_speechTrackChange->GetValue();
    g_speechVolume = m_speechVolume->GetValue();
    g_speechEffect = m_speechEffect->GetValue();

    // Get SoundTouch settings
    {
        g_stAntiAliasFilter = m_stAAFilter->GetValue();
        g_stQuickAlgorithm = m_stQuickAlgo->GetValue();
        g_stPreventClick = m_stPreventClick->GetValue();

        int aaLen = static_cast<int>(std::wcstol(WS(m_stAALength->GetStringSelection()).c_str(), nullptr, 10));
        if (aaLen >= 8 && aaLen <= 128) g_stAAFilterLength = aaLen;

        int seq = static_cast<int>(std::wcstol(WS(m_stSequence->GetValue()).c_str(), nullptr, 10));
        if (seq >= 0 && seq <= 200) g_stSequenceMs = seq;

        int seek = static_cast<int>(std::wcstol(WS(m_stSeekWindow->GetValue()).c_str(), nullptr, 10));
        if (seek >= 0 && seek <= 100) g_stSeekWindowMs = seek;

        int overlap = static_cast<int>(std::wcstol(WS(m_stOverlap->GetValue()).c_str(), nullptr, 10));
        if (overlap >= 0 && overlap <= 50) g_stOverlapMs = overlap;

        int algoSel = m_stAlgorithm->GetSelection();
        if (algoSel >= 0 && algoSel <= 2) g_stAlgorithm = algoSel;
    }

    // Get Speedy settings
    g_speedyNonlinear = m_speedyNonlinear->GetValue();

    // Get Signalsmith settings
    {
        int presetSel = m_ssPreset->GetSelection();
        if (presetSel >= 0 && presetSel <= 1) g_ssPreset = presetSel;

        int tonality = static_cast<int>(std::wcstol(WS(m_ssTonality->GetValue()).c_str(), nullptr, 10));
        if (tonality >= 0 && tonality <= 20000) g_ssTonalityLimit = tonality;
    }

    // Get MIDI settings
    {
        g_midiSoundFont = WS(m_midiSoundFont->GetValue());

        int voices = static_cast<int>(std::wcstol(WS(m_midiVoices->GetValue()).c_str(), nullptr, 10));
        if (voices >= 1 && voices <= 1000) g_midiMaxVoices = voices;

        g_midiSincInterp = m_midiSinc->GetValue();
    }

    // Save settings
    SaveSettings();

    EndModal(wxID_OK);
}

void OptionsDialog::OnRecBrowse(wxCommandEvent&) {
    // Browse for recording output folder
    wxDirDialog dlg(this, "Select recording output folder", wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_recPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnDownloadBrowse(wxCommandEvent&) {
    // Browse for downloads folder
    wxDirDialog dlg(this, "Select downloads folder", wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_downloadPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnRecFormat(wxCommandEvent&) {
    // Enable bitrate only for lossy formats (MP3=1, OGG=2)
    int format = m_recFormat->GetSelection();
    m_recBitrate->Enable(format == 1 || format == 2);
}

void OptionsDialog::UpdateCookiesStatus() {
    bool has = YouTubeHasCookies();
    m_cookiesStatus->SetLabel(has ? "YouTube cookies: imported" : "YouTube cookies: none");
    m_removeCookies->Enable(has);
}

// Cookies are imported (or removed) at once, not when the options are saved.
void OptionsDialog::OnImportCookies(wxCommandEvent&) {
    wxFileDialog dlg(this, "Import cookies.txt", wxEmptyString, "cookies.txt",
                     "Cookie files (*.txt)|*.txt|All Files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    std::wstring error;
    if (YouTubeImportCookies(WS(dlg.GetPath()), error)) {
        UpdateCookiesStatus();
        wxMessageBox("Cookies imported. YouTube will now treat FastPlay as signed in to that account.",
                     "YouTube Cookies", wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(WX(error), "YouTube Cookies", wxOK | wxICON_ERROR, this);
    }
}

void OptionsDialog::OnRemoveCookies(wxCommandEvent&) {
    YouTubeRemoveCookies();
    UpdateCookiesStatus();
    Speak("Cookies removed");
}

void OptionsDialog::OnYtdlpBrowse(wxCommandEvent&) {
#ifdef __WXMSW__
    const char* filter = "Executables (*.exe)|*.exe|All Files (*.*)|*.*";
#else
    const char* filter = "All Files (*)|*";
#endif
    wxFileDialog dlg(this, "Select yt-dlp executable", wxEmptyString, wxEmptyString, filter,
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_ytdlpPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnMidiBrowse(wxCommandEvent&) {
    wxFileDialog dlg(this, "Select SoundFont file", wxEmptyString, wxEmptyString,
                     "SoundFont Files (*.sf2;*.sf3;*.sfz)|*.sf2;*.sf3;*.sfz|All Files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_midiSoundFont->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnConvBrowse(wxCommandEvent&) {
    wxFileDialog dlg(this, "Select Impulse Response file", wxEmptyString, wxEmptyString,
                     "IR Files (*.wav;*.flac;*.ogg;*.mp3)|*.wav;*.flac;*.ogg;*.mp3|"
                     "WAV Files (*.wav)|*.wav|"
                     "FLAC Files (*.flac)|*.flac|"
                     "All Files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        std::wstring filePath = WS(dlg.GetPath());
        g_convolutionIRPath = filePath;
        // Display just the filename
        m_convIR->SetValue(WX(FileNameOnly(filePath)));
        // Load the IR file
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv) {
            conv->LoadIR(filePath.c_str());
        }
    }
}

void OptionsDialog::OnResetListOrder(wxCommandEvent&) {
    ResetRadioSortOrder();
    ResetPodcastSortOrder();
    Speak("Order reset to alphabetical");
}

void OptionsDialog::OnHotkeyAdd(wxCommandEvent&) {
    HotkeyDlgData data = {0, 0, 0, false};
    if (ShowHotkeyDialog(this, data)) {
        MainFrame* frame = GetMainFrame();
        if (frame) frame->UnregisterGlobalHotkeys();

        // Add new hotkey
        GlobalHotkey hk;
        hk.id = g_nextHotkeyId++;
        hk.modifiers = data.modifiers;
        hk.vk = data.vk;
        hk.actionIdx = data.actionIdx;
        g_hotkeys.push_back(hk);

        // Update list
        m_hotkeyList->Append(WX(HotkeyListItem(hk)));

        // Re-register hotkeys
        if (frame) frame->RegisterGlobalHotkeys();
        SaveHotkeys();
    }
}

void OptionsDialog::OnHotkeyEdit(wxCommandEvent&) {
    int sel = m_hotkeyList->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
        HotkeyDlgData data;
        data.actionIdx = g_hotkeys[sel].actionIdx;
        data.modifiers = g_hotkeys[sel].modifiers;
        data.vk = g_hotkeys[sel].vk;
        data.isEdit = true;

        if (ShowHotkeyDialog(this, data)) {
            // Update hotkey, re-registering it when hotkeys are on
            MainFrame* frame = GetMainFrame();
            if (frame && g_hotkeysEnabled) frame->UnregisterGlobalHotkeys();
            g_hotkeys[sel].modifiers = data.modifiers;
            g_hotkeys[sel].vk = data.vk;
            g_hotkeys[sel].actionIdx = data.actionIdx;
            if (frame && g_hotkeysEnabled) frame->RegisterGlobalHotkeys();

            // Update list item
            m_hotkeyList->SetString(sel, WX(HotkeyListItem(g_hotkeys[sel])));
            m_hotkeyList->SetSelection(sel);

            SaveHotkeys();
        }
    }
}

void OptionsDialog::OnHotkeyRemove(wxCommandEvent&) {
    int sel = m_hotkeyList->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
        // Unregister and remove
        MainFrame* frame = GetMainFrame();
        if (frame && g_hotkeysEnabled) frame->UnregisterGlobalHotkeys();
        g_hotkeys.erase(g_hotkeys.begin() + sel);
        if (frame && g_hotkeysEnabled) frame->RegisterGlobalHotkeys();
        m_hotkeyList->Delete(sel);

        // Select next item or previous
        if (sel >= static_cast<int>(g_hotkeys.size())) {
            sel = static_cast<int>(g_hotkeys.size()) - 1;
        }
        if (sel >= 0) {
            m_hotkeyList->SetSelection(sel);
        }

        SaveHotkeys();
    }
}

void OptionsDialog::OnHotkeyEnabled(wxCommandEvent&) {
    bool newEnabled = m_hotkeyEnabled->GetValue();
    if (newEnabled != g_hotkeysEnabled) {
        g_hotkeysEnabled = newEnabled;
        MainFrame* frame = GetMainFrame();
        if (frame) {
            if (g_hotkeysEnabled) {
                frame->UnregisterGlobalHotkeys();
                frame->RegisterGlobalHotkeys();
            } else {
                frame->UnregisterGlobalHotkeys();
            }
        }
        SaveHotkeys();
    }
}

}  // namespace

// Show options dialog
void ShowOptionsDialog() {
    OptionsDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
