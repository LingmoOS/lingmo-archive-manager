// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archiveentry.h"

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "ui/application.h"
#include "ui/file_chooser.h"
#include "ui/label.h"
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
#include <map>
#include <system_reminder>
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

// ─── Global menu exporter ─────────────────────────────────────────────────────

class GlobalMenuExporter
{
public:
    struct Entry {
        std::string label;
        bool separator = false;
        bool enabled   = true;
        std::function<void()> action;
        std::vector<Entry> children;
    };

    GlobalMenuExporter(base::dbus::Connection* bus,
                       std::function<std::vector<Entry>()> menu)
        : m_bus(bus)
        , m_menu(std::move(menu))
        , m_object(bus, kPath, kInterface)
    {
        using namespace base::dbus;
        rebuild();

        m_object.addProperty("Version",       [] { return uint32_t(3); });
        m_object.addProperty("TextDirection", [] { return std::string("ltr"); });
        m_object.addProperty("Status",        [] { return std::string("normal"); });
        m_object.addProperty("IconThemePath", [] { return std::vector<std::string>(); });

        m_object.addMethod("GetLayout", "iias", "u(ia{sv}av)", [this](MethodCall& call) {
            int32_t parent = 0, depth = -1;
            std::vector<std::string> names;
            call.read(parent, depth, names);
            call.reply(m_revision, layout(parent, depth));
        });
        m_object.addMethod("GetGroupProperties", "aias", "a(ia{sv})", [this](MethodCall& call) {
            std::vector<int32_t> ids;
            std::vector<std::string> names;
            call.read(ids, names);
            std::vector<std::tuple<int32_t, VariantMap>> result;
            for (int32_t id : ids)
                if (const Entry* e = find(id))
                    result.emplace_back(id, props(*e));
            call.reply(result);
        });
        m_object.addMethod("GetProperty", "is", "v", [this](MethodCall& call) {
            int32_t id = 0; std::string name;
            call.read(id, name);
            const Entry* e = find(id);
            const auto map = e ? props(*e) : VariantMap{};
            const auto it  = map.find(name);
            call.reply(it != map.end() ? it->second : Variant(std::string()));
        });
        m_object.addMethod("Event", "isvu", "", [this](MethodCall& call) {
            int32_t id = 0; std::string event;
            call.read(id, event);
            call.reply();
            if (event != "clicked") return;
            const Entry* e = find(id);
            if (!e || !e->action || !e->enabled) return;
            base::EventLoop::current()->post(
                [action = e->action, alive = m_alive] { if (*alive) action(); });
        });
        m_object.addMethod("AboutToShow", "i", "b", [this](MethodCall& call) {
            int32_t id = 0;
            call.read(id);
            rebuild();
            call.reply(true);
        });
        m_object.addSignal("LayoutUpdated",          "ui");
        m_object.addSignal("ItemsPropertiesUpdated",  "a(ia{sv})a(ias)");
        m_object.addSignal("ItemActivationRequested", "iu");
        m_object.publish();

        m_watch = bus->watchName(kRegistrar, [this, alive = m_alive](const std::string& owner) {
            if (*alive && !owner.empty())
                registerWithBar();
        });
    }

    ~GlobalMenuExporter()
    {
        *m_alive = false;
        base::dbus::Proxy reg(m_bus, kRegistrar, kRegistrarPath, kRegistrar);
        reg.send("UnregisterWindow", uint32_t(1));
    }

    void update()
    {
        rebuild();
        ++m_revision;
        m_object.emitSignal("LayoutUpdated", m_revision, int32_t(0));
    }

private:
    static constexpr const char* kInterface    = "com.canonical.dbusmenu";
    static constexpr const char* kRegistrar    = "com.canonical.AppMenu.Registrar";
    static constexpr const char* kRegistrarPath= "/com/canonical/AppMenu/Registrar";
    static constexpr const char* kPath         = "/org/lingmo/ArchiveManager/Menu";

    using VariantMap = base::dbus::VariantMap;
    using Variant    = base::dbus::Variant;
    using MethodCall = base::dbus::MethodCall;
    using Layout     = std::tuple<int32_t, VariantMap, std::vector<Variant>>;

    void registerWithBar()
    {
        base::dbus::Proxy reg(m_bus, kRegistrar, kRegistrarPath, kRegistrar);
        reg.send("RegisterWindow", uint32_t(1),
                 base::dbus::ObjectPath{kPath});
    }

    void rebuild()
    {
        m_entries = m_menu();
        m_ids.clear();
        m_entryIds.clear();
        int32_t next = 1;
        std::function<void(const std::vector<Entry>&)> number =
            [&](const std::vector<Entry>& entries) {
                for (const Entry& e : entries) {
                    m_ids[next] = &e;
                    m_entryIds[&e] = next++;
                    number(e.children);
                }
            };
        number(m_entries);
    }

    const Entry* find(int32_t id) const
    {
        const auto it = m_ids.find(id);
        return it == m_ids.end() ? nullptr : it->second;
    }

    VariantMap props(const Entry& e) const
    {
        VariantMap p;
        if (e.separator) {
            p["type"] = Variant(std::string("separator"));
        } else {
            p["label"] = Variant(e.label);
        }
        if (!e.enabled)          p["enabled"] = Variant(false);
        if (!e.children.empty()) p["children-display"] = Variant(std::string("submenu"));
        return p;
    }

    Layout layout(int32_t id, int depth) const
    {
        VariantMap itemProps;
        if (id == 0) {
            itemProps["children-display"] = Variant(std::string("submenu"));
        } else if (const Entry* e = find(id)) {
            itemProps = props(*e);
        }
        const std::vector<Entry>& children =
            (id == 0) ? m_entries
                      : (find(id) ? find(id)->children : m_entries);
        std::vector<Variant> items;
        if (depth != 0) {
            for (const Entry& child : children) {
                const auto it = m_entryIds.find(&child);
                if (it == m_entryIds.end()) continue;
                items.push_back(Variant(layout(it->second, depth - 1)));
            }
        }
        return {id, itemProps, items};
    }

    base::dbus::Connection*               m_bus;
    std::function<std::vector<Entry>()>   m_menu;
    base::dbus::Object                    m_object;
    base::dbus::NameWatch                 m_watch;
    std::vector<Entry>                    m_entries;
    std::map<int32_t, const Entry*>       m_ids;
    std::map<const Entry*, int32_t>       m_entryIds;
    uint32_t                              m_revision = 1;
    std::shared_ptr<bool>                 m_alive = std::make_shared<bool>(true);
};

// ─── Entry row ────────────────────────────────────────────────────────────────

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
        if (m_selected == selected) return;
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
        if (event.button != MouseButton::Left) return;
        if (m_onSelected) m_onSelected(m_index);
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

// ─── Archive root view ────────────────────────────────────────────────────────

class ArchiveRoot : public View
{
public:
    explicit ArchiveRoot()
    {
        // Plain window header: title only, no in-window action buttons.
        m_header = addChild<WindowHeader>(44.0);
        m_header->setButtonsCentered(true);
        m_header->setButtonMargin(4);
        auto* titleLabel = m_header->setContent<Label>(tr("Archive Manager"));
        titleLabel->setFontSize(14.5);
        titleLabel->setWeight(600);

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
            m_hasEntries = false;
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
        m_hasEntries = !listing.entries.empty();
        for (size_t i = 0; i < listing.entries.size(); ++i) {
            EntryRow* row = m_rows->addChild<EntryRow>(
                    listing.entries[i], i % 2 == 1, i,
                    [this](size_t index) { toggleSelection(index); });
            m_entryRows.push_back(row);
        }
        m_status->setText({});
        m_selected.clear();
        requestLayout();
    }

    void setStatus(const std::string& status)
    {
        m_status->setText(status);
        requestLayout();
    }

    bool hasEntries() const { return m_hasEntries; }
    const std::vector<size_t>& selected() const { return m_selected; }

    SizeF sizeHint() const override { return {1000, 720}; }

protected:
    void layout() override
    {
        const RectF rect = localRect();
        m_header->setGeometry({0, 0, rect.width, 44});
        m_info->setGeometry({24, 56, std::max(0.0, rect.width - 48), 20});
        const double listWidth = std::max(0.0, rect.width - 24);
        m_columnName->setGeometry({30, 84, std::max(0.0, listWidth - 250), 18});
        m_columnSize->setGeometry({listWidth - 206, 84, 90, 18});
        m_columnDate->setGeometry({listWidth - 106, 84, 100, 18});
        m_emptyHint->setGeometry({24, 200, std::max(0.0, rect.width - 48), 32});
        m_scroll->setGeometry({12, 104, listWidth, std::max(0.0, rect.height - 136)});
        m_status->setGeometry({24, rect.height - 24, std::max(0.0, rect.width - 48), 18});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        painter.fillRect(localRect(), theme.background);
        painter.drawHorizontalLine(0, 44, geometry().width, theme.disabledText.withAlpha(0.08),
                                   painter.borderWidth(1));
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
        const size_t n = m_selected.size();
        m_status->setText(n > 0 ? tr("Selected ") + std::to_string(n) : std::string{});
        requestLayout();
    }

    WindowHeader* m_header = nullptr;
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
    bool m_hasEntries = false;
};

// ─── Archive window ───────────────────────────────────────────────────────────

class ArchiveWindow
{
public:
    ArchiveWindow()
    {
        m_window.setTitle(tr("Archive Manager"));
        m_window.resize({1000, 720});
        m_window.setMinimumSize({640, 440});

        auto root = std::make_unique<ArchiveRoot>();
        m_root = root.get();
        m_window.setContentView(std::move(root));

        // Register the File menu with the compositor's global menu bar.
        auto* bus = base::dbus::Connection::session();
        if (bus) {
            m_menuExporter = std::make_unique<GlobalMenuExporter>(bus, [this] {
                return buildMenuEntries();
            });
        }
    }

    Window& window() { return m_window; }

    void openArchive(const std::string& path)
    {
        m_path = path;
        m_listing = archivemanager::listArchive(path);
        m_root->showListing(m_listing,
                            archivemanager::formatLabel(path, m_listing.formatName, m_listing.filterName));
        if (m_menuExporter)
            m_menuExporter->update();
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
        m_extractChooser->setCurrentFolder(
                base::paths::userDirectory(base::paths::UserDirectory::Downloads));
        m_extractChooser->onAccepted =
                [this, selected](const std::vector<std::string>& paths, const std::string&) {
                    if (paths.empty()) return;
                    const std::string destination = paths.front();
                    m_root->setStatus(tr("Extracting\xe2\x80\xa6"));
                    const archivemanager::ExtractResult result = archivemanager::extractArchive(
                            m_path, m_listing.entries, selected, destination,
                            archivemanager::ExtractOptions{},
                            [this](const archivemanager::Progress& progress) {
                                if (progress.total > 0)
                                    m_root->setStatus(tr("Extracting ") + progress.current);
                            });
                    if (!result.ok) {
                        m_root->setStatus(result.error.empty() ? tr("Extraction failed") : result.error);
                        return;
                    }
                    std::string message = tr("Extracted ") + std::to_string(result.files)
                            + tr(" files to ") + destination;
                    if (result.skipped > 0)
                        message += "  \xc2\xb7 " + std::to_string(result.skipped) + tr(" skipped");
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
            if (paths.empty()) return;
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
                if (files.empty()) return;
                archivemanager::CreateFormat format = archivemanager::CreateFormat::Zip;
                if (!archivemanager::createFormatForPath(files.front(), &format)) {
                    m_root->setStatus(tr("Use a .zip, .tar or .tar.gz file name"));
                    return;
                }
                const archivemanager::CreateResult result =
                        archivemanager::createArchive(files.front(), {source}, format);
                if (!result.ok) {
                    m_root->setStatus(result.error.empty()
                                              ? tr("Could not create the archive")
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

private:
    std::vector<GlobalMenuExporter::Entry> buildMenuEntries()
    {
        using Entry = GlobalMenuExporter::Entry;
        const bool hasEntries = m_root->hasEntries();
        const size_t selCount = m_root->selected().size();

        Entry newArchive;
        newArchive.label  = tr("New Archive");
        newArchive.action = [this] { compressFolder(); };

        Entry open;
        open.label  = tr("Open");
        open.action = [this] { openWithChooser(); };

        Entry sep;
        sep.separator = true;

        std::string extractLabel = selCount > 0
                ? tr("Extract ") + std::to_string(selCount)
                : tr("Extract All");
        Entry extract;
        extract.label   = std::move(extractLabel);
        extract.enabled = hasEntries;
        extract.action  = [this] { extractSelected(m_root->selected()); };

        Entry file;
        file.label    = tr("File");
        file.children = { std::move(newArchive), std::move(open),
                          std::move(sep), std::move(extract) };

        return { std::move(file) };
    }

    Window m_window;
    ArchiveRoot* m_root = nullptr;
    std::unique_ptr<GlobalMenuExporter> m_menuExporter;
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
