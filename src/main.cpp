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
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

class EntryRow : public View
{
public:
    explicit EntryRow(const archivemanager::Entry& entry, bool alternate)
        : m_alternate(alternate)
    {
        setLayoutHints({.fillWidth = true});
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

    SizeF sizeHint() const override { return {0, 34}; }

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
        if (m_alternate)
            painter.fillRect(localRect(), Theme::current().disabledText.withAlpha(0.04));
    }

private:
    bool m_alternate;
    Label* m_name = nullptr;
    Label* m_size = nullptr;
    Label* m_date = nullptr;
};

// The window's content: the title, the actions, the list and the status line.
class ArchiveRoot : public View
{
public:
    explicit ArchiveRoot(std::function<void()> open, std::function<void()> extract)
        : m_openAction(std::move(open))
        , m_extractAction(std::move(extract))
    {
        m_title = addChild<Label>(tr("Archive Manager"));
        m_title->setFontSize(18);
        m_title->setWeight(600);
        m_open = addChild<Button>(tr("Open"));
        m_extract = addChild<Button>(tr("Extract All"));
        m_info = addChild<Label>(tr("No archive open"));
        m_info->setFontSize(13);
        m_info->setColor(ColorRecipe(ColorId::DisabledText));
        m_info->setElide(Label::Elide::Middle);
        m_scroll = addChild<ScrollView>();
        m_scroll->setName("entryScroll");
        m_rows = m_scroll->setContent<Column>();
        m_rows->setSpacing(1);
        m_status = addChild<Label>();
        m_status->setFontSize(12);
        m_status->setColor(ColorRecipe(ColorId::DisabledText));
        m_status->setElide(Label::Elide::Middle);
        m_open->onClicked = [this] { m_openAction(); };
        m_extract->onClicked = [this] { m_extractAction(); };
    }

    void showListing(const archivemanager::Listing& listing)
    {
        while (!m_rows->children().empty())
            m_rows->takeChild(m_rows->children().front().get());
        if (!listing.ok) {
            m_info->setText(listing.error.empty() ? tr("Could not read this archive") : listing.error);
            m_status->setText({});
            requestLayout();
            return;
        }
        std::string info = std::to_string(listing.entries.size()) + tr(" entries");
        if (!listing.formatName.empty())
            info += "  " + listing.formatName;
        if (!listing.filterName.empty())
            info += " / " + listing.filterName;
        m_info->setText(info);
        for (size_t i = 0; i < listing.entries.size(); ++i)
            m_rows->addChild<EntryRow>(listing.entries[i], i % 2 == 1);
        m_status->setText({});
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
        m_title->setGeometry({24, 16, 240, 28});
        m_open->setGeometry({rect.width - 300, 12, 84, 34});
        m_extract->setGeometry({rect.width - 204, 12, 112, 34});
        m_info->setGeometry({24, 68, std::max(0.0, rect.width - 48), 20});
        m_scroll->setGeometry({12, 96, std::max(0.0, rect.width - 24),
                               std::max(0.0, rect.height - 134)});
        m_status->setGeometry({24, rect.height - 30, std::max(0.0, rect.width - 48), 18});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        painter.fillRect(localRect(), theme.background);
        painter.drawHorizontalLine(0, 60, geometry().width, theme.disabledText.withAlpha(0.08),
                                   painter.borderWidth(1));
    }

private:
    std::function<void()> m_openAction;
    std::function<void()> m_extractAction;
    Label* m_title = nullptr;
    Button* m_open = nullptr;
    Button* m_extract = nullptr;
    Label* m_info = nullptr;
    ScrollView* m_scroll = nullptr;
    Column* m_rows = nullptr;
    Label* m_status = nullptr;
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
        auto root = std::make_unique<ArchiveRoot>([this] { openWithChooser(); },
                                                 [this] { extractAll(); });
        m_root = root.get();
        m_window.setContentView(std::move(root));
    }

    Window& window() { return m_window; }

    void openArchive(const std::string& path)
    {
        m_path = path;
        m_listing = archivemanager::listArchive(path);
        m_root->showListing(m_listing);
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

    void extractAll()
    {
        if (!m_listing.ok || m_listing.entries.empty()) {
            m_root->setStatus(tr("Open an archive first"));
            return;
        }
        std::filesystem::path destination =
                std::filesystem::path(base::paths::home()) / "Downloads" / std::filesystem::path(m_path).stem();
        std::error_code error;
        std::filesystem::create_directories(destination, error);
        if (error) {
            m_root->setStatus(tr("Could not create the extraction folder"));
            return;
        }
        std::vector<size_t> selected;
        selected.reserve(m_listing.entries.size());
        for (size_t i = 0; i < m_listing.entries.size(); ++i)
            selected.push_back(i);
        const archivemanager::ExtractResult result = archivemanager::extractArchive(
                m_path, m_listing.entries, selected, destination.string(), archivemanager::ExtractOptions {});
        if (!result.ok)
            m_root->setStatus(result.error.empty() ? tr("Extraction failed") : result.error);
        else
            m_root->setStatus(tr("Extracted to ") + destination.string());
    }

    const archivemanager::Listing& listing() const { return m_listing; }

private:
    Window m_window;
    ArchiveRoot* m_root = nullptr;
    std::unique_ptr<FileChooser> m_chooser;
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
