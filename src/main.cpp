// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archiveentry.h"

#include "base/i18n.h"
#include "base/paths.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/file_chooser.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/window.h"
#include "ui/window_header.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace Lingmo;
using namespace Lingmo::ui;
using Lingmo::i18n::tr;

namespace {

std::string sizeText(const archivemanager::Entry& entry)
{
    if (entry.directory)
        return tr("Folder");
    if (entry.size < 1024)
        return std::to_string(entry.size) + " B";
    if (entry.size < 1024 * 1024)
        return std::to_string(entry.size / 1024) + " KB";
    return std::to_string(entry.size / (1024 * 1024)) + " MB";
}

std::string dateText(const archivemanager::Entry& entry)
{
    if (entry.mtime <= 0)
        return {};
    std::time_t time = std::time_t(entry.mtime);
    std::tm local{};
    localtime_r(&time, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

std::string humanSize(uint64_t bytes)
{
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ull * 1024 * 1024)
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
}

class EntryRow : public View
{
public:
    EntryRow(const archivemanager::Entry& entry, bool alternate, size_t index,
             std::function<void(size_t)> onSelected)
        : m_alternate(alternate)
        , m_index(index)
        , m_onSelected(std::move(onSelected))
    {
        setLayoutHints({.fillWidth = true});
        setCursorShape(CursorShape::Pointer);
        m_name = addChild<Label>(entry.relativePath.empty() ? entry.path : entry.relativePath);
        m_name->setFontSize(13);
        m_name->setElide(Label::Elide::Middle);
        m_size = addChild<Label>(sizeText(entry));
        m_size->setFontSize(12);
        m_size->setColor(ColorRecipe(ColorId::DisabledText));
        m_size->setTextAlignment(Label::TextAlignment::Right);
        m_date = addChild<Label>(dateText(entry));
        m_date->setFontSize(12);
        m_date->setColor(ColorRecipe(ColorId::DisabledText));
        m_date->setTextAlignment(Label::TextAlignment::Right);
    }

    void setSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        requestPaint();
    }

    bool isSelected() const { return m_selected; }

    SizeF sizeHint() const override { return {0, 34}; }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double nameWidth = std::max(80.0, width - 250);
        m_name->setGeometry({30, 8, nameWidth, 20});
        m_size->setGeometry({nameWidth + 44, 8, 90, 20});
        m_date->setGeometry({nameWidth + 144, 8, 100, 20});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        if (m_selected)
            painter.fillRect(localRect(), theme.highlight.withAlpha(0.16));
        else if (m_alternate)
            painter.fillRect(localRect(), theme.disabledText.withAlpha(0.04));
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left)
            return;
        if (m_onSelected)
            m_onSelected(m_index);
    }

private:
    bool m_alternate;
    bool m_selected = false;
    size_t m_index;
    std::function<void(size_t)> m_onSelected;
    Label* m_name = nullptr;
    Label* m_size = nullptr;
    Label* m_date = nullptr;
};

// The window's content: header with toolbar, file list and status line.
class ArchiveRoot : public View
{
public:
    explicit ArchiveRoot(std::function<void()> open,
                         std::function<void(const std::vector<size_t>&)> extract,
                         std::function<void()> compress)
        : m_openAction(std::move(open))
        , m_extractAction(std::move(extract))
        , m_compressAction(std::move(compress))
    {
        // Window header with title and action buttons.
        m_header = addChild<WindowHeader>(44.0);
        m_header->setButtonsCentered(true);
        m_header->setButtonMargin(4);
        auto* row = m_header->setContent<RowLayout>();
        row->setSpacing(8);
        row->setLayoutHints({.fillWidth = true});
        auto* titleLabel = row->addChild<Label>(tr("Archive Manager"));
        titleLabel->setFontSize(14.5);
        titleLabel->setWeight(600);
        titleLabel->setLayoutHints({.fillWidth = true});
        m_compress = row->addChild<Button>(tr("New Archive"));
        m_open     = row->addChild<Button>(tr("Open"));
        m_extract  = row->addChild<Button>(tr("Extract All"));
        m_extract->setEnabled(false);

        m_info = addChild<Label>(tr("No archive open"));
        m_info->setFontSize(13);
        m_info->setColor(ColorRecipe(ColorId::DisabledText));
        m_info->setElide(Label::Elide::Middle);
        m_columnName = addChild<Label>(tr("Name"));
        m_columnSize = addChild<Label>(tr("Size"));
        m_columnDate = addChild<Label>(tr("Modified"));
        for (Label* label : {m_columnName, m_columnSize, m_columnDate}) {
            label->setFontSize(11);
            label->setColor(ColorRecipe(ColorId::DisabledText));
        }
        m_columnSize->setTextAlignment(Label::TextAlignment::Right);
        m_columnDate->setTextAlignment(Label::TextAlignment::Right);
        m_emptyHint = addChild<Label>(tr("Open an archive to see its contents"));
        m_emptyHint->setFontSize(14);
        m_emptyHint->setColor(ColorRecipe(ColorId::DisabledText));
        m_emptyHint->setTextAlignment(Label::TextAlignment::Center);
        m_emptyHint->setVerticallyCentered(true);
        m_scroll = addChild<ScrollView>();
        m_scroll->setName("entryScroll");
        m_rows = m_scroll->setContent<Column>();
        m_rows->setSpacing(1);
        m_status = addChild<Label>();
        m_status->setFontSize(12);
        m_status->setColor(ColorRecipe(ColorId::DisabledText));
        m_status->setElide(Label::Elide::Middle);
        m_open->onClicked     = [this] { m_openAction(); };
        m_extract->onClicked  = [this] { m_extractAction(m_selected); };
        m_compress->onClicked = [this] { m_compressAction(); };
    }

    void showListing(const archivemanager::Listing& listing, const std::string& formatLabel = {})
    {
        while (!m_rows->children().empty())
            m_rows->takeChild(m_rows->children().front().get());
        m_entryRows.clear();
        m_selected.clear();
        if (!listing.ok) {
            m_info->setText(listing.error.empty() ? tr("Could not read this archive") : listing.error);
            m_status->setText({});
            m_extract->setText(tr("Extract All"));
            m_extract->setEnabled(false);
            m_emptyHint->setVisible(true);
            requestLayout();
            return;
        }
        std::string info = std::to_string(listing.entries.size()) + tr(" entries");
        if (!formatLabel.empty())
            info += "  " + formatLabel;
        if (listing.totalSize > 0)
            info += "  · " + humanSize(listing.totalSize);
        if (listing.unsafePaths > 0)
            info += "  · " + std::to_string(listing.unsafePaths) + tr(" unsafe paths");
        m_info->setText(info);
        m_emptyHint->setVisible(listing.entries.empty());
        m_extract->setEnabled(!listing.entries.empty());
        for (size_t i = 0; i < listing.entries.size(); ++i) {
            EntryRow* row = m_rows->addChild<EntryRow>(
                    listing.entries[i], i % 2 == 1, i,
                    [this](size_t index) { toggleSelection(index); });
            m_entryRows.push_back(row);
        }
        m_status->setText({});
        updateSelectionUi();
        requestLayout();
    }

    void setStatus(const std::string& status)
    {
        m_status->setText(status);
        requestLayout();
    }

    SizeF sizeHint() const override { return {1000, 720}; }

protected:
    void layout() override
    {
        const RectF rect = localRect();
        // Header fills the top bar.
        m_header->setGeometry({0, 0, rect.width, 44});
        // Info row below the header.
        m_info->setGeometry({24, 56, std::max(0.0, rect.width - 48), 20});
        // Column labels.
        const double listWidth = std::max(0.0, rect.width - 24);
        m_columnName->setGeometry({30, 84, std::max(0.0, listWidth - 250), 18});
        m_columnSize->setGeometry({listWidth - 206, 84, 90, 18});
        m_columnDate->setGeometry({listWidth - 106, 84, 100, 18});
        // Empty hint centred in the list area.
        m_emptyHint->setGeometry({24, 200, std::max(0.0, rect.width - 48), 32});
        // Scroll list.
        m_scroll->setGeometry({12, 104, listWidth, std::max(0.0, rect.height - 136)});
        // Status bar at the bottom.
        m_status->setGeometry({24, rect.height - 24, std::max(0.0, rect.width - 48), 18});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        painter.fillRect(localRect(), theme.background);
        // Separator under the header.
        painter.drawHorizontalLine(0, 44, geometry().width, theme.disabledText.withAlpha(0.08),
                                   painter.borderWidth(1));
        // Separator above the column labels.
        painter.drawHorizontalLine(0, 102, geometry().width, theme.disabledText.withAlpha(0.06),
                                   painter.borderWidth(1));
    }

private:
    void toggleSelection(size_t index)
    {
        const auto found = std::find(m_selected.begin(), m_selected.end(), index);
        if (found == m_selected.end())
            m_selected.push_back(index);
        else
            m_selected.erase(found);
        if (index < m_entryRows.size())
            m_entryRows[index]->setSelected(found == m_selected.end());
        updateSelectionUi();
    }

    void updateSelectionUi()
    {
        if (m_selected.empty()) {
            m_extract->setText(tr("Extract All"));
            if (m_status->text().empty())
                m_status->setText({});
        } else {
            m_extract->setText(tr("Extract ") + std::to_string(m_selected.size()));
            m_status->setText(tr("Selected ") + std::to_string(m_selected.size()));
        }
        requestLayout();
    }

    std::function<void()> m_openAction;
    std::function<void(const std::vector<size_t>&)> m_extractAction;
    std::function<void()> m_compressAction;
    WindowHeader* m_header = nullptr;
    Button* m_compress = nullptr;
    Button* m_open = nullptr;
    Button* m_extract = nullptr;
    Label* m_info = nullptr;
    Label* m_columnName = nullptr;
    Label* m_columnSize = nullptr;
    Label* m_columnDate = nullptr;
    Label* m_emptyHint = nullptr;
    ScrollView* m_scroll = nullptr;
    Column* m_rows = nullptr;
    Label* m_status = nullptr;
    std::vector<EntryRow*> m_entryRows;
    std::vector<size_t> m_selected;
};

// One window and the archive it shows.
class ArchiveWindow
{
public:
    ArchiveWindow()
    {
        m_window.setTitle(tr("Archive Manager"));
        m_window.resize({1000, 720});
        m_window.setMinimumSize({640, 440});
        auto root = std::make_unique<ArchiveRoot>(
                [this] { openWithChooser(); },
                [this](const std::vector<size_t>& selected) { extractSelected(selected); },
                [this] { compressFolder(); });
        m_root = root.get();
        m_window.setContentView(std::move(root));
    }

    Window& window() { return m_window; }

    void openArchive(const std::string& path)
    {
        m_path = path;
        m_listing = archivemanager::listArchive(path);
        m_root->showListing(m_listing,
                            archivemanager::formatLabel(path, m_listing.formatName, m_listing.filterName));
    }

    void openWithChooser()
    {
        m_chooser = std::make_unique<FileChooser>(&m_window);
        m_chooser->setMode(FileChooser::Mode::OpenFile);
        m_chooser->setTitle(tr("Open Archive"));
        m_chooser->setFilters({{tr("Archives"),
                                {"*.zip", "*.tar", "*.tar.gz", "*.tgz", "*.tar.bz2", "*.tar.xz", "*.7z",
                                 "*.rar"},
                                {}},
                               {tr("All Files"), {"*"}, {}}});
        m_chooser->onAccepted = [this](const std::vector<std::string>& paths, const std::string&) {
            if (!paths.empty())
                openArchive(paths.front());
        };
        m_chooser->open();
    }

    void extractSelected(const std::vector<size_t>& requested)
    {
        if (!m_listing.ok || m_listing.entries.empty()) {
            m_root->setStatus(tr("Open an archive first"));
            return;
        }
        std::vector<size_t> selected = requested;
        if (selected.empty()) {
            selected.reserve(m_listing.entries.size());
            for (size_t i = 0; i < m_listing.entries.size(); ++i)
                selected.push_back(i);
        }

        m_extractChooser = std::make_unique<FileChooser>(&m_window);
        m_extractChooser->setMode(FileChooser::Mode::OpenFolder);
        m_extractChooser->setTitle(tr("Choose where to extract"));
        m_extractChooser->setAcceptLabel(tr("Extract Here"));
        m_extractChooser->setCurrentFolder(base::paths::userDirectory(
                base::paths::UserDirectory::Downloads));
        m_extractChooser->onAccepted =
                [this, selected](const std::vector<std::string>& paths, const std::string&) {
                    if (paths.empty())
                        return;
                    const std::string destination = paths.front();
                    m_root->setStatus(tr("Extracting…"));
                    const archivemanager::ExtractResult result = archivemanager::extractArchive(
                            m_path, m_listing.entries, selected, destination,
                            archivemanager::ExtractOptions {},
                            [this](const archivemanager::Progress& progress) {
                                if (progress.total > 0)
                                    m_root->setStatus(tr("Extracting ") + progress.current);
                            });
                    if (!result.ok) {
                        m_root->setStatus(result.error.empty() ? tr("Extraction failed")
                                                               : result.error);
                        return;
                    }
                    std::string message = tr("Extracted ") + std::to_string(result.files)
                            + tr(" files to ") + destination;
                    if (result.skipped > 0)
                        message += "  · " + std::to_string(result.skipped) + tr(" skipped");
                    m_root->setStatus(message);
                };
        m_extractChooser->open();
    }

    void compressFolder()
    {
        m_chooser = std::make_unique<FileChooser>(&m_window);
        m_chooser->setMode(FileChooser::Mode::OpenFolder);
        m_chooser->setTitle(tr("Choose a folder to compress"));
        m_chooser->onAccepted = [this](const std::vector<std::string>& paths, const std::string&) {
            if (paths.empty())
                return;
            const std::string source = paths.front();
            m_saveChooser = std::make_unique<FileChooser>(&m_window);
            m_saveChooser->setMode(FileChooser::Mode::SaveFile);
            m_saveChooser->setTitle(tr("Save Archive"));
            m_saveChooser->setCurrentFolder(std::filesystem::path(source).parent_path().string());
            m_saveChooser->setCurrentFile(
                    (std::filesystem::path(source).filename().string().empty()
                             ? std::string("archive")
                             : std::filesystem::path(source).filename().string())
                    + ".zip");
            m_saveChooser->setFilters({{tr("ZIP archive"), {"*.zip"}, {}},
                                      {tr("Tar archive"), {"*.tar", "*.tar.gz", "*.tgz"}, {}}});
            m_saveChooser->onAccepted = [this, source](const std::vector<std::string>& files,
                                                       const std::string&) {
                if (files.empty())
                    return;
                archivemanager::CreateFormat format = archivemanager::CreateFormat::Zip;
                if (!archivemanager::createFormatForPath(files.front(), &format)) {
                    m_root->setStatus(tr("Use a .zip, .tar or .tar.gz file name"));
                    return;
                }
                const archivemanager::CreateResult result =
                        archivemanager::createArchive(files.front(), {source}, format);
                if (!result.ok) {
                    m_root->setStatus(result.error.empty() ? tr("Could not create the archive")
                                                           : result.error);
                    return;
                }
                openArchive(files.front());
                m_root->setStatus(tr("Archive created"));
            };
            m_saveChooser->open();
        };
        m_chooser->open();
    }

    const archivemanager::Listing& listing() const { return m_listing; }

private:
    Window m_window;
    ArchiveRoot* m_root = nullptr;
    std::unique_ptr<FileChooser> m_chooser;
    std::unique_ptr<FileChooser> m_saveChooser;
    std::unique_ptr<FileChooser> m_extractChooser;
    archivemanager::Listing m_listing;
    std::string m_path;
};

} // namespace

#ifndef ARCHIVE_MANAGER_NO_MAIN
int main(int argc, char** argv)
{
    Application app(argc, argv);
    app.setApplicationId("lingmo-archive-manager");
    i18n::setDomain("lingmo-archive-manager");

    ArchiveWindow window;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            std::printf("Usage: lingmo-archive-manager [ARCHIVE]\nLingmo Archive Manager\n");
            return 0;
        }
        if (!argument.starts_with("-"))
            window.openArchive(argument);
    }
    window.window().show();
    return app.run();
}
#endif
