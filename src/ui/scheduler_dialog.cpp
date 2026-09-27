// The Scheduler window (the list of scheduled events) and the Add / Edit
// Scheduled Event dialog.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "database.h"
#include "accessibility.h"

#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/timectrl.h>
#include <wx/valtext.h>
#include <ctime>
#include <string>
#include <vector>

namespace {

// Add/Edit schedule dialog. `editing` is the event being edited, or null to add a new one.
class SchedAddDialog : public wxDialog {
public:
    SchedAddDialog(wxWindow* parent, const ScheduledEvent* editing)
        : wxDialog(parent, wxID_ANY, editing ? "Edit Scheduled Event" : "Add Scheduled Event"),
          m_editing(editing) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        // Name
        auto* nameRow = new wxBoxSizer(wxHORIZONTAL);
        nameRow->Add(new wxStaticText(this, wxID_ANY, "&Name:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_name = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ConvertDialogToPixels(wxSize(248, -1)));
        nameRow->Add(m_name, 1, wxALIGN_CENTER_VERTICAL);
        sizer->Add(nameRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        // Action
        auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
        actionRow->Add(new wxStaticText(this, wxID_ANY, "&Action:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_action = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(100, -1)));
        actionRow->Add(m_action, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(actionRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        // Source
        auto* sourceRow = new wxBoxSizer(wxHORIZONTAL);
        sourceRow->Add(new wxStaticText(this, wxID_ANY, "&Source:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_source = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(80, -1)));
        sourceRow->Add(m_source, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(sourceRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        // File
        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(this, wxID_ANY, "&File:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_file = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ConvertDialogToPixels(wxSize(220, -1)));
        fileRow->Add(m_file, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        m_browse = new wxButton(this, wxID_ANY, "...", wxDefaultPosition, ConvertDialogToPixels(wxSize(20, 14)));
        fileRow->Add(m_browse, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(fileRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        // Radio
        auto* radioRow = new wxBoxSizer(wxHORIZONTAL);
        radioRow->Add(new wxStaticText(this, wxID_ANY, "Ra&dio:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_radio = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(238, -1)));
        radioRow->Add(m_radio, 1, wxALIGN_CENTER_VERTICAL);
        sizer->Add(radioRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        // Date and time
        auto* whenRow = new wxBoxSizer(wxHORIZONTAL);
        whenRow->Add(new wxStaticText(this, wxID_ANY, "&Date:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_date = new wxDatePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition,
                                      ConvertDialogToPixels(wxSize(100, -1)), wxDP_DROPDOWN | wxDP_SHOWCENTURY);
        whenRow->Add(m_date, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        whenRow->Add(new wxStaticText(this, wxID_ANY, "&Time:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_time = new wxTimePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition,
                                      ConvertDialogToPixels(wxSize(80, -1)));
        whenRow->Add(m_time, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(whenRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        // Repeat
        auto* repeatRow = new wxBoxSizer(wxHORIZONTAL);
        repeatRow->Add(new wxStaticText(this, wxID_ANY, "&Repeat:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_repeat = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(100, -1)));
        repeatRow->Add(m_repeat, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(repeatRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        // Duration and stop action
        auto* durationRow = new wxBoxSizer(wxHORIZONTAL);
        durationRow->Add(new wxStaticText(this, wxID_ANY, "D&uration:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_duration = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ConvertDialogToPixels(wxSize(40, -1)));
        SetDigitsOnly(m_duration);
        durationRow->Add(m_duration, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        durationRow->Add(new wxStaticText(this, wxID_ANY, "minutes (0 = no limit)"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        durationRow->Add(new wxStaticText(this, wxID_ANY, "S&top:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_stop = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(93, -1)));
        durationRow->Add(m_stop, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(durationRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        m_enabled = new wxCheckBox(this, wxID_ANY, "&Enabled");
        sizer->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        buttons->Add(ok, 0, wxRIGHT, 6);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 10);

        // Action combo
        m_action->Append("Playback");
        m_action->Append("Recording");
        m_action->Append("Both");
        m_action->SetSelection(0);

        // Source combo
        m_source->Append("File");
        m_source->Append("Radio");
        m_source->SetSelection(0);

        // Radio stations combo
        m_stations = GetRadioFavorites();
        for (const auto& rs : m_stations) {
            m_radio->Append(WX(rs.name));
        }
        if (!m_stations.empty()) {
            m_radio->SetSelection(0);
        }

        // Repeat combo
        m_repeat->Append("Once");
        m_repeat->Append("Daily");
        m_repeat->Append("Weekly");
        m_repeat->Append("Weekdays");
        m_repeat->Append("Weekends");
        m_repeat->Append("Monthly");
        m_repeat->SetSelection(0);

        // Enabled checkbox - default on
        m_enabled->SetValue(true);

        // Duration - default 0 (no limit)
        m_duration->ChangeValue("0");

        // Stop action combo (only relevant when action is "Both")
        m_stop->Append("Both");
        m_stop->Append("Playback only");
        m_stop->Append("Recording only");
        m_stop->SetSelection(0);

        // Pre-populate fields if editing
        if (m_editing) {
            m_name->ChangeValue(WX(m_editing->name));
            m_action->SetSelection(static_cast<int>(m_editing->action));
            m_source->SetSelection(static_cast<int>(m_editing->sourceType));

            if (m_editing->sourceType == ScheduleSource::File) {
                m_file->ChangeValue(WX(m_editing->sourcePath));
            } else {
                // Find and select the radio station
                for (int i = 0; i < static_cast<int>(m_stations.size()); i++) {
                    if (m_stations[i].id == m_editing->radioStationId) {
                        m_radio->SetSelection(i);
                        break;
                    }
                }
            }

            // The scheduled time, to the minute
            wxDateTime when(static_cast<time_t>(m_editing->scheduledTime));
            when.SetSecond(0);
            when.SetMillisecond(0);
            m_date->SetValue(when);
            m_time->SetValue(when);

            m_repeat->SetSelection(static_cast<int>(m_editing->repeat));
            m_enabled->SetValue(m_editing->enabled);
            m_duration->ChangeValue(wxString::Format("%u", static_cast<unsigned int>(m_editing->duration)));
            m_stop->SetSelection(static_cast<int>(m_editing->stopAction));
        } else {
            // Set default date/time to now + 1 hour
            wxDateTime when = wxDateTime::Now() + wxTimeSpan::Hour();
            m_date->SetValue(when);
            m_time->SetValue(when);

            // If currently playing, prefill file
            if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                m_file->ChangeValue(WX(g_playlist[g_currentTrack]));
            }
        }

        SetSizerAndFit(sizer);
        CentreOnParent();

        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateSchedSourceControls(); });
        m_browse->Bind(wxEVT_BUTTON, &SchedAddDialog::OnBrowse, this);
        ok->Bind(wxEVT_BUTTON, &SchedAddDialog::OnOK, this);

        UpdateSchedSourceControls();
        m_name->SetFocus();
    }

private:
    // Show/hide file vs radio controls based on source selection
    void UpdateSchedSourceControls() {
        bool isFile = (m_source->GetSelection() == 0);

        // Show/hide file controls
        m_file->Show(isFile);
        m_browse->Show(isFile);

        // Show/hide radio control
        m_radio->Show(!isFile);

        Layout();
    }

    void OnBrowse(wxCommandEvent&) {
        // Start from the current value
        wxFileName current(m_file->GetValue());
        wxFileDialog dlg(this, "Select Audio File", current.GetPath(), current.GetFullName(),
                         "All Supported|*.mp3;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.wma;*.aac;*.opus;*.aiff;*.ape;*.wv;*.mid;*.midi"
                         "|All Files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            m_file->ChangeValue(dlg.GetPath());
        }
    }

    void OnOK(wxCommandEvent&) {
        std::wstring name = WS(m_name->GetValue());

        if (name.empty()) {
            wxMessageBox("Please enter a name.", "Add Schedule", wxOK | wxICON_WARNING, this);
            m_name->SetFocus();
            return;
        }

        ScheduleAction action = static_cast<ScheduleAction>(m_action->GetSelection());

        int sourceIdx = m_source->GetSelection();
        ScheduleSource sourceType = static_cast<ScheduleSource>(sourceIdx);

        std::wstring sourcePath;
        int radioStationId = 0;

        if (sourceType == ScheduleSource::File) {
            std::wstring filePath = WS(m_file->GetValue());
            if (filePath.empty()) {
                wxMessageBox("Please select a file.", "Add Schedule", wxOK | wxICON_WARNING, this);
                m_file->SetFocus();
                return;
            }
            sourcePath = filePath;
        } else {
            int sel = m_radio->GetSelection();
            if (sel < 0) {
                wxMessageBox("Please select a radio station.", "Add Schedule", wxOK | wxICON_WARNING, this);
                m_radio->SetFocus();
                return;
            }
            radioStationId = m_stations[sel].id;
            // Get the URL from the radio stations
            std::vector<RadioStation> stations = GetRadioFavorites();
            for (const auto& rs : stations) {
                if (rs.id == radioStationId) {
                    sourcePath = rs.url;
                    break;
                }
            }
        }

        // Combine the date and the time (to the minute) and convert to a timestamp
        wxDateTime date = m_date->GetValue();
        wxDateTime timeOfDay = m_time->GetValue();
        struct tm tm = {0};
        tm.tm_year = date.GetYear() - 1900;
        tm.tm_mon = static_cast<int>(date.GetMonth());
        tm.tm_mday = date.GetDay();
        tm.tm_hour = timeOfDay.GetHour();
        tm.tm_min = timeOfDay.GetMinute();
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        int64_t scheduledTime = static_cast<int64_t>(mktime(&tm));

        ScheduleRepeat repeat = static_cast<ScheduleRepeat>(m_repeat->GetSelection());

        bool enabled = m_enabled->GetValue();

        // Get duration (an unsigned number; anything unreadable counts as 0)
        unsigned long value = 0;
        if (!m_duration->GetValue().ToULong(&value)) value = 0;
        int duration = static_cast<int>(static_cast<unsigned int>(value));
        if (duration < 0) duration = 0;

        // Get stop action
        ScheduleStopAction stopAction = static_cast<ScheduleStopAction>(m_stop->GetSelection());

        bool success = false;
        if (m_editing) {
            // Update existing event
            success = UpdateScheduledEvent(m_editing->id, name, action, sourceType, sourcePath,
                                           radioStationId, scheduledTime, repeat, enabled,
                                           duration, stopAction);
        } else {
            // Add new event
            int id = AddScheduledEvent(name, action, sourceType, sourcePath,
                                       radioStationId, scheduledTime, repeat, enabled,
                                       duration, stopAction);
            success = (id >= 0);
        }

        if (success) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox(m_editing ? "Failed to update scheduled event." : "Failed to add scheduled event.",
                         m_editing ? "Edit Schedule" : "Add Schedule", wxOK | wxICON_ERROR, this);
        }
    }

    const ScheduledEvent* m_editing;
    std::vector<RadioStation> m_stations;

    wxTextCtrl* m_name;
    wxChoice* m_action;
    wxChoice* m_source;
    wxTextCtrl* m_file;
    wxButton* m_browse;
    wxChoice* m_radio;
    wxDatePickerCtrl* m_date;
    wxTimePickerCtrl* m_time;
    wxChoice* m_repeat;
    wxTextCtrl* m_duration;
    wxChoice* m_stop;
    wxCheckBox* m_enabled;
};

// The Scheduler window: the list of scheduled events.
class SchedulerDialog : public wxDialog {
public:
    explicit SchedulerDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Scheduler", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        sizer->Add(new wxStaticText(this, wxID_ANY,
                       "Scheduled events (Enter = toggle, Delete = remove, Escape = close):"),
                   0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
        sizer->Add(m_list, 1, wxEXPAND | wxALL, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* add = new wxButton(this, wxID_ANY, "&Add...");
        buttons->Add(add, 0, wxRIGHT, 6);
        auto* edit = new wxButton(this, wxID_ANY, "&Edit...");
        buttons->Add(edit, 0);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, "Close"), 0, wxALIGN_BOTTOM);
        sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizer(sizer);
        SetClientSize(ConvertDialogToPixels(wxSize(380, 280)));
        SetMinSize(wxSize(300, 200));
        CentreOnParent();

        add->Bind(wxEVT_BUTTON, &SchedulerDialog::OnAdd, this);
        edit->Bind(wxEVT_BUTTON, &SchedulerDialog::OnEdit, this);
        // Double-click to toggle
        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { ToggleSelected(); });
        Bind(wxEVT_CHAR_HOOK, &SchedulerDialog::OnCharHook, this);

        RefreshScheduleList();

        m_list->SetFocus();
        if (m_list->GetCount() > 0) {
            m_list->SetSelection(0);
        }
    }

private:
    void RefreshScheduleList() {
        m_events = GetAllScheduledEvents();
        wxArrayString items;
        items.reserve(m_events.size());
        for (const auto& ev : m_events) {
            items.push_back(WX(ev.displayName));
        }
        m_list->Set(items);
    }

    // Select an item after the list was rebuilt
    void Reselect(int sel) {
        if (sel >= 0 && sel < static_cast<int>(m_list->GetCount())) {
            m_list->SetSelection(sel);
        }
    }

    // Toggle enabled state
    void ToggleSelected() {
        int sel = m_list->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_events.size())) {
            bool newState = !m_events[sel].enabled;
            if (UpdateScheduledEventEnabled(m_events[sel].id, newState)) {
                Speak(newState ? "Enabled" : "Disabled");
                RefreshScheduleList();
                Reselect(sel);
            }
        }
    }

    // Remove selected event
    void RemoveSelected() {
        int sel = m_list->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_events.size())) {
            if (RemoveScheduledEvent(m_events[sel].id)) {
                Speak("Schedule removed");
                RefreshScheduleList();
                int count = static_cast<int>(m_list->GetCount());
                if (count > 0) {
                    if (sel >= count) sel = count - 1;
                    m_list->SetSelection(sel);
                }
            }
        }
    }

    // The list's own keys: Enter toggles, Delete removes. Escape closes the
    // window through the Close button.
    void OnCharHook(wxKeyEvent& event) {
        if (FindFocus() == m_list && !(event.AltDown() && !event.ControlDown())) {
            int key = event.GetKeyCode();
            if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
                ToggleSelected();
                return;
            }
            if (key == WXK_DELETE || key == WXK_NUMPAD_DELETE) {
                RemoveSelected();
                return;
            }
        }
        event.Skip();
    }

    void OnAdd(wxCommandEvent&) {
        SchedAddDialog dlg(this, nullptr);
        if (dlg.ShowModal() == wxID_OK) {
            RefreshScheduleList();
            Speak("Schedule added");
        }
    }

    void OnEdit(wxCommandEvent&) {
        int sel = m_list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_events.size())) {
            Speak("No schedule selected");
            return;
        }
        ScheduledEvent editing = m_events[sel];
        SchedAddDialog dlg(this, &editing);
        if (dlg.ShowModal() == wxID_OK) {
            RefreshScheduleList();
            Reselect(sel);
            Speak("Schedule updated");
        }
    }

    wxListBox* m_list;
    std::vector<ScheduledEvent> m_events;
};

}  // namespace

// Show scheduler dialog
void ShowSchedulerDialog() {
    SchedulerDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
