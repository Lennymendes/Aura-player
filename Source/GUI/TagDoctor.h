// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// Tag & Cover Doctor (Extensions): duplicates ranked by quality (hide the lower versions in the library, or move them
// to the system trash), covers missing / under 500 px, incomplete tags, albums split between artists / years.
// Nothing happens to a file or to the library without a click; deleting, replacing a cover and rewriting an album go
// through a confirmation bar in the panel first. The tags inside the files are never written.
#pragma once

#include "AppContext.h"
#include "Widgets.h"
#include "../Library/LibraryDoctor.h"

namespace nx
{

class TagDoctor : public juce::Component
{
public:
    enum class Tab { Duplicates, Audit };
    explicit TagDoctor (AppContext& c);
    ~TagDoctor() override;
    void open();
    void close();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override { if (! panel.contains (e.getPosition())) close(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }

    void setTab (Tab t);
    Tab getTab() const noexcept { return tab; }
    /** Scans the library now (synchronously; the button does it in the background). */
    void scanNow();
    const std::vector<doctor::DuplicateGroup>& getDuplicates() const noexcept { return dups; }
    const doctor::Audit& getAudit() const noexcept { return auditResult; }

    // actions (buttons); the destructive ones only arm the confirmation bar
    int hideLowerVersions();
    int showHiddenAgain();
    void requestTrashLowerVersions();
    void requestCoverFromFile (const juce::File& image);   // for the selected album
    void requestOnlineCover();                              // iTunes Search API, opt-in
    void requestUniform();                                  // for the selected inconsistent album
    bool hasPendingAction() const noexcept { return pending != nullptr; }
    juce::String getPendingText() const { return pendingText; }
    /** The confirmation bar's buttons. Returns the number of files / tracks changed. */
    int confirmPending();
    void cancelPending();

    /** Rows of the current tab (tests: "kind|text"); selecting one enables its actions. */
    juce::StringArray rowTexts() const;
    void selectRow (int index);
    int getSelectedRow() const noexcept { return selected; }

    juce::TextButton scanButton, dupTab, auditTab, hideButton, unhideButton, trashButton, coverButton, onlineButton, uniformButton,
                     confirmButton, cancelButton;
    juce::ToggleButton onlineToggle;
    IconButton closeButton;

private:
    struct Row { juce::String kind, text, sub; int group = -1; int item = -1; bool header = false; };
    struct Body : juce::Component
    {
        explicit Body (TagDoctor& d) : doctor (d) {}
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent& e) override;
        TagDoctor& doctor;
    };
    void rebuildRows();
    void updateButtons();
    void arm (const juce::String& text, std::function<int()> action);
    const AlbumInfo* selectedAlbum() const;
    AppContext& ctx;
    Tab tab = Tab::Duplicates;
    std::vector<doctor::DuplicateGroup> dups;
    doctor::Audit auditResult;
    bool scanned = false;
    std::vector<Row> rows;
    int selected = -1;
    std::function<int()> pending;
    juce::String pendingText;
    juce::Rectangle<int> panel, confirmBar;
    juce::Viewport viewport;
    Body body { *this };
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<int> alive = std::make_shared<int> (0);
    static constexpr int rowH = 30;
};

} // namespace nx
