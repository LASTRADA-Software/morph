// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/shell_parts_view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

ShellFrame::ShellFrame(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, ShellFrameOptions options)
    : _backend{backend},
      _root{backend.createStack(parent, Axis::Horizontal)},
      _sidebar{backend.createStack(_root.get(), Axis::Horizontal)},
      _sideStart{backend.createSpacer(_sidebar.get())},
      _side{backend.createStack(_sidebar.get(), Axis::Vertical)},
      _sideTop{backend.createSpacer(_side.get())},
      _name{backend.createText(_side.get())},
      _navigator{std::make_unique<Navigator>(backend, _side.get(), std::move(options.navigator))},
      _principal{backend.createText(_side.get())},
      _signOut{backend.createButton(_side.get())},
      _sideEnd{backend.createSpacer(_sidebar.get())},
      _main{backend.createStack(_root.get(), Axis::Vertical)},
      _topBar{backend.createStack(_main.get(), Axis::Vertical)},
      _topAbove{backend.createSpacer(_topBar.get())},
      _topLine{backend.createStack(_topBar.get(), Axis::Horizontal)},
      _topBelow{backend.createSpacer(_topBar.get())},
      _topStart{backend.createSpacer(_topLine.get())},
      _topMain{wrappingRow(backend, _topLine.get())},
      _navMenu{backend.createMenu(_topMain.get(), MenuStyle::Button)},
      _leadBlock{backend.createStack(_topMain.get(), Axis::Vertical)},
      _eyebrow{backend.createStack(_leadBlock.get(), Axis::Vertical)},
      _titleLine{wrappingRow(backend, _leadBlock.get())},
      _title{backend.createText(_titleLine.get())},
      _headInline{backend.createStack(_titleLine.get(), Axis::Horizontal)},
      _headLead{backend.createStack(_leadBlock.get(), Axis::Vertical)},
      _trail{wrappingRow(backend, _topMain.get())},
      _headSlot{backend.createStack(_trail.get(), Axis::Horizontal)},
      _topPrincipal{backend.createText(_trail.get())},
      _trailInset{backend.createStack(_topLine.get(), Axis::Horizontal)},
      _trailInsetStart{backend.createSpacer(_trailInset.get())},
      _trailInsetEnd{backend.createSpacer(_trailInset.get())},
      _topEnd{backend.createSpacer(_topLine.get())},
      _page{backend.createStack(_main.get(), Axis::Vertical)},
      _top{backend.createSpacer(_page.get())},
      _padded{backend.createStack(_page.get(), Axis::Horizontal)},
      _left{backend.createSpacer(_padded.get())},
      _content{backend.createStack(_padded.get(), Axis::Vertical)},
      _right{backend.createSpacer(_padded.get())},
      _chat{backend.createStack(_root.get(), Axis::Horizontal)},
      _chatStart{backend.createSpacer(_chat.get())},
      _chatColumn{backend.createStack(_chat.get(), Axis::Vertical)},
      _chatTop{backend.createSpacer(_chatColumn.get())},
      _chatSlot{backend.createStack(_chatColumn.get(), Axis::Vertical)},
      _chatBottom{backend.createSpacer(_chatColumn.get())},
      _chatEnd{backend.createSpacer(_chat.get())},
      _onSignOut{options.onSignOut} {
    _root->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _root->setTestId("shell");

    _sidebar->setSurface("sidebar");
    _sidebar->setLayout({.width = Sizing::fixed(kShellSidebarWidth), .height = Sizing::stretch()});
    _sidebar->setGap(3);
    _sidebar->setTestId("shell_sidebar");
    // Spacers either side and above pad the sidebar's content by its gaps.
    _sideStart->setLayout({.width = Sizing::fixed(0), .height = {}});
    _side->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _side->setGap(3);
    _sideTop->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _sideEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
    _name->setRole(TextRole::Heading);
    _name->setText(options.applicationName);
    _name->setTestId("shell_application");

    // The principal and the sign-out button sit at the sidebar's foot: the navigator takes the sidebar's spare
    // height above them. Left on the top line they would compete with the head's actions for its trailing edge.
    _navigator->root().setLayout({.width = {}, .height = Sizing::stretch()});
    _principal->setRole(TextRole::Muted);
    _principal->setTestId("shell_principal");
    _signOut->setLabel("Sign out");
    _signOut->setEmphasis(ButtonEmphasis::Ghost);
    _signOut->setTestId("shell_signOut");
    _signOut->setVisible(static_cast<bool>(options.onSignOut));
    _signOut->setOnClick(std::move(options.onSignOut));

    _main->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    // One line that wraps only when it is out of room: the title with the head's lead under it, and the head's
    // trail at the trailing edge, which drops to a line of its own where it does not fit beside the lead's
    // minimum. Wrapping is the one choice every backend can draw with the widgets it has; eliding would lose the
    // badges and actions the head exists for, and an overflow menu needs a node and a measuring pass the
    // vocabulary does not have.
    _topBar->setSurface("toolbar");
    _topBar->setTestId("shell_topbar");
    // Zero-sized spacers above and below pad the line by the bar's gap, as they do at the line's two ends by its
    // own gap; the line is centred across the bar.
    _topBar->setGap(2);
    _topBar->setHeadBar(true);
    _topAbove->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _topBelow->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _topLine->setGap(4);
    _topLine->setAlign(Align::Center);
    _topLine->setTestId("shell_topbar_line");
    // Spacers at both ends pad the line's content by its gap: expanded, the page's gutter, so the head's text and
    // the page's content share their edges.
    _topStart->setLayout({.width = Sizing::fixed(0), .height = {}});
    _topEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
    _topMain->setLayout({.width = Sizing::stretch(), .height = {}});
    _topMain->setGap(3);
    _topMain->setAlign(Align::Start);
    _topMain->setJustify(Justify::End);
    _topMain->setTestId("shell_topbar_main");
    // The compact form's navigation: the navigator's entries as one button-style menu, kept current with it.
    _navMenu->setTestId("shell_nav_menu");
    _navMenu->setAccessibleName("Navigation");
    _navMenu->setTooltip("Navigation");
    _navMenu->setTrigger(MenuTrigger{.label = {}, .icon = "nav.menu", .emphasis = ButtonEmphasis::Ghost});
    showNavEntries();
    _navMenu->setOnActivate([this](std::vector<std::size_t> const& path) { chooseNav(path); });
    _navMenu->setVisible(false);
    _navigator->setOnChanged([this] { showNavEntries(); });
    _leadBlock->setLayout({.width = Sizing::stretch(), .height = {}, .minWidth = kShellHeadLeadMinimum});
    _eyebrow->setTestId("shell_head_eyebrow");
    _eyebrow->setVisible(false);
    // The title and the inline lead share a line that wraps inside the lead block: where both do not fit, the
    // lead goes under the title, and the head's trail keeps the title's line until the block's minimum is gone.
    _titleLine->setGap(2);
    _titleLine->setAlign(Align::Center);
    _titleLine->setTestId("shell_title_line");
    _title->setRole(TextRole::Heading);
    _title->setTestId("shell_title");
    _headLead->setLayout({.width = Sizing::stretch(), .height = {}});
    _headLead->setTestId("shell_head_lead");
    _headLead->setVisible(false);
    _headInline->setTestId("shell_head_inline");
    _headInline->setVisible(false);
    // The trail: the head's actions, then the principal, as one wrapping row that stands where the actions did.
    // A wrapping row gives no gap to an item with no width, so a shown slot with nothing mounted in it leaves no
    // space before the principal; the principal wraps under the actions where the two do not fit one line.
    _trail->setGap(3);
    _trail->setAlign(Align::Center);
    _trail->setTestId("shell_head_trail");
    _trail->setVisible(false);
    _headSlot->setTestId("shell_head");
    _headSlot->setVisible(false);
    _topPrincipal->setRole(TextRole::Muted);
    _topPrincipal->setTestId("shell_head_principal");
    _topPrincipal->setVisible(false);
    // Two empty items a gap apart: with the line's gap on either side they add step 3 after the trail. The
    // compact line's end pad is step 1, which is too close for a button that draws a frame.
    _trailInset->setGap(2);
    _trailInset->setTestId("shell_trail_end_inset");
    _trailInset->setVisible(false);
    _trailInsetStart->setLayout({.width = Sizing::fixed(0), .height = {}});
    _trailInsetEnd->setLayout({.width = Sizing::fixed(0), .height = {}});

    // The page is padded by spacing step 4: zero-sized spacers above and either side, each a gap away.
    _page->setSurface("page");
    _page->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _page->setGap(4);
    _page->setTestId("shell_page");
    _top->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _padded->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _padded->setGap(4);
    _left->setLayout({.width = Sizing::fixed(0), .height = {}});
    _content->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _right->setLayout({.width = Sizing::fixed(0), .height = {}});

    // Spacers on every side pad the chat's content by its gaps, as the sidebar's do.
    _chat->setSurface("sidebar");
    _chat->setLayout({.width = Sizing::fixed(kShellChatWidth), .height = Sizing::stretch()});
    _chat->setGap(3);
    _chat->setTestId("shell_chat");
    _chatStart->setLayout({.width = Sizing::fixed(0), .height = {}});
    _chatColumn->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _chatColumn->setGap(3);
    _chatTop->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _chatSlot->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
    _chatBottom->setLayout({.width = {}, .height = Sizing::fixed(0)});
    _chatEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
    buildParts();
    showChat();
}

void ShellFrame::setChatShown(bool shown) {
    _chatWanted = shown;
    showChat();
}

void ShellFrame::setHeadShown(bool shown) {
    _headShown = shown;
    _eyebrow->setVisible(shown);
    _headLead->setVisible(shown);
    _headSlot->setVisible(shown);
    showLeadPlace();
    showTrail();
}

void ShellFrame::setLeadInline(bool inlineLead) {
    _leadInline = inlineLead;
    showLeadPlace();
}

void ShellFrame::setHeadOneLine(bool oneLine) {
    _headTall = !oneLine;
    _topMain->setAlign(oneLine ? Align::Center : Align::Start);
    showBarInset();
    showTrail();
}

void ShellFrame::setHeadJoined(bool joined) {
    _headJoined = joined;
    showTopBarSurface();
}

void ShellFrame::setHeadSurface(std::string_view surface) {
    _headSurface = std::string{surface};
    showTopBarSurface();
}

void ShellFrame::setPrincipalInHead(bool inHead) {
    _principalInHead = inHead;
    showChrome();
}

void ShellFrame::setTitle(std::string_view title) {
    _titleText = std::string{title};
    showTitle();
}

void ShellFrame::setPageInset(bool inset) {
    _top->setVisible(inset);
    _left->setVisible(inset);
    _right->setVisible(inset);
}

void ShellFrame::setTitleSpans(std::vector<TextSpan> const& spans) {
    _titleSpans = spans;
    showTitle();
}

void ShellFrame::setPrincipal(std::string_view principal) {
    _principal->setText(principal);
    _topPrincipal->setText(principal);
    _principalName = std::string{principal};
    showNavEntries();
    showTrail();
}

void ShellFrame::showSignedIn(bool signedIn) {
    _signedIn = signedIn;
    showChrome();
}

void ShellFrame::setCompact(bool compact) {
    _compact = compact;
    // The compact top bar is tighter: a touch-sized menu square beside the title leaves no room for the full
    // steps, so the bar pads its line by step 1 above and below (more for a head that is not one plain line, see
    // showBarInset), and the line pads its ends by step 1 and spaces its items by step 2.
    showBarInset();
    _topLine->setGap(compact ? 1 : 4);
    _topMain->setGap(compact ? 2 : 3);
    showChrome();
}

void ShellFrame::setTrailEnd(HeadTrailEnd end) {
    _trailEnd = end;
    showBarInset();
    showTrail();
}

void ShellFrame::setParts(ShellPartsView view) {
    if (view == _parts) {
        return;
    }
    std::string const previousSignature = hitSignature();
    _parts = std::move(view);
    if (hitSignature() != previousSignature) {
        _highlight = hitCount() == 0 ? std::nullopt : std::optional<std::size_t>{0};
    }
    showParts();
}

void ShellFrame::focusSearch() {
    if (!_parts.search.shown) {
        return;
    }
    if (_compact) {
        _overlay->setOpen(true);
        _overlayInput->focus();
    } else {
        _search->focus();
    }
}

void ShellFrame::setSearchText(std::string_view text) {
    _searchText = std::string{text};
    _searchDismissed = false;
    _search->setText(text);
    _overlayInput->setText(text);
    showSearch();
}

void ShellFrame::confirm(std::string_view message, std::string_view yes, std::string_view no,
                         std::function<void(bool)> done) {
    if (_confirm == nullptr) {
        _confirm = _backend.createDialog(_main.get());
        _confirm->setTestId("shell_confirm");
        _confirm->setOnDismiss([this] { answerConfirm(false); });
        _confirmYes = _backend.createButton(&_confirm->footer());
        _confirmYes->setTestId("shell_confirm_yes");
        _confirmYes->setEmphasis(ButtonEmphasis::Danger);
        _confirmYes->setOnClick([this] { answerConfirm(true); });
        _confirmNo = _backend.createButton(&_confirm->footer());
        _confirmNo->setTestId("shell_confirm_no");
        _confirmNo->setOnClick([this] { answerConfirm(false); });
    }
    _confirm->setTitle(message);
    _confirmYes->setLabel(yes);
    _confirmNo->setLabel(no);
    _confirmDone = std::move(done);
    _confirm->setOpen(true);
}

void ShellFrame::showToast(std::string_view text, Tone tone) {
    if (_toast == nullptr) {
        _toast = _backend.createToast(_main.get());
    }
    _toast->show(text, tone);
}

std::unique_ptr<StackWidget> ShellFrame::wrappingRow(IViewBackend& backend, ContainerWidget* parent) {
    auto row = backend.createStack(parent, Axis::Horizontal);
    row->setWrap(true);
    return row;
}

void ShellFrame::showTitle() {
    if (_titleSpans.empty()) {
        _title->setText(_titleText);
    } else {
        _title->setSpans(_titleSpans);
    }
}

std::string ShellFrame::connectionText(ConnectionState state) const {
    ShellLabels const& labels = _parts.labels;
    return state == ConnectionState::Connected      ? labels.connected
           : state == ConnectionState::Reconnecting ? labels.reconnecting
                                                    : labels.offline;
}

std::string ShellFrame::footText() const {
    std::string text;
    if (_parts.connection) {
        text = connectionText(*_parts.connection);
    }
    if (_parts.brand.shown && !_parts.brand.version.empty()) {
        text += (text.empty() ? "" : " · ") + _parts.brand.version;
    }
    return text;
}

void ShellFrame::showNavEntries() {
    std::vector<MenuEntry> entries;
    _navTargets.clear();
    auto const add = [&](MenuEntry entry, NavTarget::Kind kind, std::size_t offset) {
        entries.push_back(std::move(entry));
        _navTargets.push_back(NavTarget{.kind = kind, .offset = offset});
    };
    if (_parts.context.shown) {
        MenuEntry submenu = contextMenuEntry();
        add(std::move(submenu), NavTarget::Kind::Context, 0);
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
    }
    std::vector<MenuEntry> navigation = _navigator->menuEntries();
    _navEntryCount = navigation.size();
    std::size_t const firstNavigation = entries.size();
    for (std::size_t index = 0; index < navigation.size(); ++index) {
        add(std::move(navigation.at(index)), NavTarget::Kind::Navigator, firstNavigation);
    }
    if (_parts.help.shown) {
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        add(MenuEntry{.label = _parts.labels.help}, NavTarget::Kind::Help, 0);
    }
    if (_parts.principal.shown) {
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        MenuEntry person = principalMenuEntry();
        add(std::move(person), NavTarget::Kind::Principal, 0);
    } else if (_onSignOut) {
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        add(MenuEntry{.label = _parts.labels.signOut, .description = _principalName}, NavTarget::Kind::SignOut, 0);
    }
    // The sidebar's foot is not on screen, so its text ends the menu as a heading. A separator target, because a
    // heading is inert on choose just as a separator is.
    if (std::string const foot = footText(); !foot.empty()) {
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        MenuEntry heading;
        heading.label = foot;
        heading.heading = true;
        heading.enabled = false;
        add(std::move(heading), NavTarget::Kind::Separator, 0);
    }
    _navMenu->setItems(entries);
}

void ShellFrame::chooseNav(std::vector<std::size_t> const& path) {
    if (path.empty() || path.front() >= _navTargets.size()) {
        return;
    }
    NavTarget const target = _navTargets.at(path.front());
    switch (target.kind) {
        case NavTarget::Kind::Navigator: {
            std::vector<std::size_t> inner = path;
            inner.front() -= target.offset;
            _navigator->choose(inner);
            return;
        }
        case NavTarget::Kind::SignOut:
            if (_onSignOut && path.size() == 1) {
                _onSignOut();
            }
            return;
        case NavTarget::Kind::Help:
            if (path.size() == 1 && _handlers.onHelp) {
                _handlers.onHelp();
            }
            return;
        case NavTarget::Kind::Context:
            if (path.size() == 2 && path.at(1) < _contextMap.size() && _contextMap.at(path.at(1)) &&
                _handlers.onContext) {
                _handlers.onContext(*_contextMap.at(path.at(1)));
            }
            return;
        case NavTarget::Kind::Principal:
            if (path.size() == 2) {
                choosePrincipal(path.at(1));
            }
            return;
        case NavTarget::Kind::Separator:
        default:
            return;
    }
}

void ShellFrame::showChrome() {
    _sidebar->setVisible(_signedIn && !_compact);
    _topBar->setVisible(_signedIn);
    _navMenu->setVisible(_compact);
    showParts();
    showChat();
    showTrail();
}

void ShellFrame::showTrail() {
    bool const principalShown = _principalInHead && !_compact && !_principalName.empty() && !_parts.principal.shown;
    _topPrincipal->setVisible(principalShown);
    _trail->setVisible((_headShown || principalShown) && !barParts());
    _trailInset->setVisible(_compact && _trailEnd != HeadTrailEnd::Unframed);
}

void ShellFrame::showBarInset() {
    bool const plain = !_headTall && _trailEnd != HeadTrailEnd::Framed;
    int const compactStep = plain ? 1 : 3;
    int const step = _compact ? compactStep : 2;
    _topBar->setGap(step);
    // Only the expanded bar's gap is the theme's `head.paddingY`; a compact bar fits the touch target.
    _topBar->setHeadBar(!_compact);
}

void ShellFrame::showTopBarSurface() {
    if (_headJoined) {
        _topBar->setSurface("toolbarJoined");
    } else {
        _topBar->setSurface(_headSurface.empty() ? "toolbar" : _headSurface);
    }
}

void ShellFrame::ResultsColumn::clear() {
    rows.clear();
    texts.clear();
}

void ShellFrame::ResultsColumn::hide() {
    for (HitRow& row : rows) {
        row.button->setVisible(false);
        row.button->setTestId({});
    }
    for (auto& text : texts) {
        text->setVisible(false);
        text->setTestId({});
    }
}

void ShellFrame::buildParts() {
    // The product mark stands where the application's name does, above the switcher.
    _brand = _backend.createStack(_side.get(), Axis::Horizontal);
    _side->moveChild(*_brand, 2);
    _brand->setGap(2);
    _brand->setAlign(Align::Center);
    _brand->setTestId("shell_brand");
    _brand->setVisible(false);
    makeBrandMark({});
    _brandTitle = _backend.createText(_brand.get());
    _brandTitle->setRole(TextRole::Heading);
    _brandTitle->setTestId("shell_brand_title");
    _context = _backend.createMenu(_side.get(), MenuStyle::Button);
    _side->moveChild(*_context, 3);
    _context->setTestId("shell_context");
    _context->setVisible(false);
    _context->setOnActivate([this](std::vector<std::size_t> const& path) {
        if (path.size() == 1 && path.front() < _contextMap.size() && _contextMap.at(path.front()) &&
            _handlers.onContext) {
            _handlers.onContext(*_contextMap.at(path.front()));
        }
    });

    // The top bar's tools sit in the wrapping row after the navigation menu: the search, the inbox, the person.
    _tools = _backend.createStack(_topMain.get(), Axis::Horizontal);
    _topMain->moveChild(*_tools, 1);
    _tools->setGap(3);
    _tools->setAlign(Align::Center);
    _tools->setTestId("shell_topbar_tools");
    _tools->setVisible(false);
    _searchBox = _backend.createStack(_tools.get(), Axis::Vertical);
    _searchBox->setLayout(
        {.width = Sizing::stretch(), .height = {}, .minWidth = std::nullopt, .maxWidth = kShellSearchWidth});
    _searchBox->setTestId("shell_search_box");
    _search = _backend.createTextInput(_searchBox.get(), TextInputMode::SingleLine);
    _search->setTestId("shell_search");
    _search->setOnChange([this](std::string const& text) { searchEdited(text, false); });
    _search->setOnSubmit([this](std::string const&) { openHighlighted(); });
    _search->setKeys({"Down", "Up", "Escape", "Shift+Enter", "Alt+Enter"},
                     [this](std::string const& chord) { searchChord(chord); });
    _popover = _backend.createPopover(_searchBox.get());
    _popover->setTestId("shell_search_popover");
    _popover->setWidth(kShellSearchWidth);
    _popover->setOnDismiss([this] {
        _searchDismissed = true;
        showSearch();
    });
    _resultsBody = _backend.createStack(_popover.get(), Axis::Vertical);
    _resultsBody->setTestId("shell_search_results");
    _resultsBody->setGap(1);
    _searchOpenButton = _backend.createButton(_tools.get());
    _searchOpenButton->setTestId("shell_search_open");
    _searchOpenButton->setLabel("⌕");
    _searchOpenButton->setEmphasis(ButtonEmphasis::Ghost);
    _searchOpenButton->setVisible(false);
    _searchOpenButton->setOnClick([this] { focusSearch(); });
    _inboxGroup = _backend.createStack(_tools.get(), Axis::Horizontal);
    _inboxGroup->setGap(1);
    _inboxGroup->setAlign(Align::Center);
    _inboxGroup->setTestId("shell_inbox_group");
    _inboxGroup->setVisible(false);
    _inbox = _backend.createButton(_inboxGroup.get());
    _inbox->setTestId("shell_inbox");
    _inbox->setOnClick([this] {
        if (_handlers.onInbox) {
            _handlers.onInbox();
        }
    });
    _inboxCount = _backend.createBadge(_inboxGroup.get());
    _inboxCount->setTestId("shell_inbox_count");
    _inboxCount->setVisible(false);
    _inboxError = _backend.createText(_inboxGroup.get());
    _inboxError->setTestId("shell_inbox_error");
    _inboxError->setText("!");
    _inboxError->setVisible(false);
    _help = _backend.createButton(_tools.get());
    _help->setTestId("shell_help");
    _help->setLabel("?");
    _help->setVisible(false);
    _help->setOnClick([this] {
        if (_handlers.onHelp) {
            _handlers.onHelp();
        }
    });
    _principalMenu = _backend.createMenu(_tools.get(), MenuStyle::Button);
    _principalMenu->setTestId("shell_principal_menu");
    _principalMenu->setVisible(false);
    _principalMenu->setOnActivate([this](std::vector<std::size_t> const& path) {
        if (path.size() == 1) {
            choosePrincipal(path.front());
        }
    });

    // The compact search: a sheet over the window with its own field and the same results.
    _overlay = _backend.createDialog(_main.get());
    _overlay->setTestId("shell_search_overlay");
    _overlay->setSheet(true);
    _overlay->setOnDismiss([this] { _overlay->setOpen(false); });
    _overlayInput = _backend.createTextInput(_overlay.get(), TextInputMode::SingleLine);
    _overlayInput->setTestId("shell_search_overlay_input");
    _overlayInput->setOnChange([this](std::string const& text) { searchEdited(text, true); });
    _overlayInput->setOnSubmit([this](std::string const&) { openHighlighted(); });
    _overlayInput->setKeys({"Down", "Up", "Escape", "Shift+Enter", "Alt+Enter"},
                           [this](std::string const& chord) { searchChord(chord); });
    _overlayBody = _backend.createStack(_overlay.get(), Axis::Vertical);
    _overlayBody->setTestId("shell_search_overlay_results");
    _overlayBody->setGap(1);

    // The sidebar's foot: the connection state, then the application's version at the far end.
    _footer = _backend.createStack(_side.get(), Axis::Horizontal);
    _footer->setGap(2);
    _footer->setAlign(Align::Center);
    _footer->setTestId("shell_footer");
    _footer->setVisible(false);
    _footerDot = _backend.createText(_footer.get());
    _footerDot->setText("●");
    _footerDot->setTestId("shell_connection_dot");
    _footerState = _backend.createText(_footer.get());
    _footerState->setRole(TextRole::Muted);
    _footerState->setTestId("shell_connection");
    _footerState->setLayout({.width = Sizing::stretch(), .height = {}});
    _footerVersion = _backend.createText(_footer.get());
    _footerVersion->setRole(TextRole::Muted);
    _footerVersion->setTestId("shell_version");

    _root->setKeys({"Ctrl+K"}, [this](std::string const&) { focusSearch(); });
}

void ShellFrame::makeBrandMark(std::string const& icon) {
    _brandMark = _backend.createBadge(_brand.get());
    _brand->moveChild(*_brandMark, 0);
    _brandMark->setTestId("shell_brand_mark");
    _brandMark->setTone(Tone::Ok);
    _brandMark->setShape(ControlShape::Round);
    if (!icon.empty()) {
        _brandMark->setIcon(icon);
    }
    _brandIcon = icon;
}

void ShellFrame::showBrand() {
    ShellBrandView const& brand = _parts.brand;
    _name->setVisible(!brand.shown);
    _brand->setVisible(brand.shown);
    if (brand.shown) {
        if (brand.icon != _brandIcon) {
            makeBrandMark(brand.icon);
        }
        // Without an icon the mark holds the title's first letter.
        _brandMark->setText(brand.icon.empty() && !brand.title.empty() ? detail::firstLetter(brand.title)
                                                                       : std::string{});
        _brandTitle->setText(brand.title);
    }
    bool const connection = _parts.connection.has_value();
    _footer->setVisible(connection || (brand.shown && !brand.version.empty()));
    _footerDot->setVisible(connection);
    _footerState->setVisible(connection);
    if (connection) {
        _footerState->setText(connectionText(*_parts.connection));
        _footerDot->setRole(*_parts.connection == ConnectionState::Connected      ? TextRole::Success
                            : *_parts.connection == ConnectionState::Reconnecting ? TextRole::Muted
                                                                                  : TextRole::Error);
    }
    _footerVersion->setVisible(brand.shown && !brand.version.empty());
    _footerVersion->setText(brand.version);
}

void ShellFrame::showParts() {
    bool const bar = barParts();
    ShellLabels const& labels = _parts.labels;
    // The switcher, at the sidebar's top.
    _context->setVisible(_parts.context.shown);
    if (_parts.context.shown) {
        _context->setItems(contextEntries());
        ShellOptionView const* const chosen =
            _parts.context.chosen && *_parts.context.chosen < _parts.context.options.size()
                ? &_parts.context.options.at(*_parts.context.chosen)
                : nullptr;
        std::string const label = chosen == nullptr ? std::string{} : chosen->label;
        std::string const detail = chosen == nullptr ? std::string{} : chosen->detail;
        _context->setTrigger(MenuTrigger{.label = label.empty() ? labels.scope : label,
                                         .icon = {},
                                         .emphasis = ButtonEmphasis::Normal,
                                         .detail = detail,
                                         .avatar = {},
                                         .caret = MenuCaret::UpDown});
        _context->setEnabled(_parts.context.enabled);
        _context->setAccessibleName(labels.scope + ": " + label + (detail.empty() ? "" : ", " + detail));
    }
    // The top bar.
    _tools->setVisible(bar);
    _leadBlock->setVisible(!bar);
    _signOut->setLabel(labels.signOut);
    _searchBox->setVisible(_parts.search.shown && !_compact);
    _searchOpenButton->setVisible(_parts.search.shown && _compact);
    _searchOpenButton->setAccessibleName(labels.search);
    _search->setAccessibleName(labels.search);
    _search->setPlaceholder(_parts.search.hint.empty() ? labels.search + " (Ctrl K)"
                                                       : _parts.search.hint + " (Ctrl K)");
    _overlay->setTitle(labels.search);
    _overlay->setDismissLabel(labels.close);
    _overlayInput->setAccessibleName(labels.search);
    _resultsBody->setAccessibleName(labels.searchResults);
    _overlayBody->setAccessibleName(labels.searchResults);
    _overlayInput->setPlaceholder(_parts.search.hint.empty() ? labels.search : _parts.search.hint);
    showSearch();
    // The inbox.
    _inboxGroup->setVisible(_parts.inbox.shown);
    _inbox->setLabel(_compact ? std::string{"✉"} : _parts.inbox.title);
    _inbox->setEmphasis(_compact ? ButtonEmphasis::Ghost : ButtonEmphasis::Normal);
    std::string inboxName = _parts.inbox.title;
    bool const counted = _parts.inbox.count && *_parts.inbox.count > 0;
    if (counted) {
        inboxName += ", " + shellCountText(*_parts.inbox.count) + " " + labels.waiting;
    }
    _inbox->setAccessibleName(inboxName);
    _inboxCount->setVisible(counted);
    if (counted) {
        _inboxCount->setText(shellCountText(*_parts.inbox.count));
        _inboxCount->setTone(Tone::Err);
    }
    _inboxError->setVisible(_parts.inbox.failed);
    _inboxError->setAccessibleName(_parts.inbox.title + ": " + labels.countFailed);
    // The person's menu.
    ShellPrincipalView const& person = _parts.principal;
    _principalMenu->setVisible(person.shown && !_compact);
    if (person.shown) {
        _principalMenu->setItems(principalEntries());
        _principalMenu->setTrigger(MenuTrigger{.label = person.name,
                                               .icon = {},
                                               .emphasis = ButtonEmphasis::Ghost,
                                               .detail = person.subtitle,
                                               .avatar = person.initials,
                                               .caret = MenuCaret::Down});
        _principalMenu->setAccessibleName(labels.signedInAs + " " + person.name);
    }
    // The help button.
    _help->setVisible(_parts.help.shown && !_compact);
    _help->setAccessibleName(labels.help);
    showBrand();
    _principal->setVisible(!_principalInHead && !person.shown);
    _signOut->setVisible(static_cast<bool>(_onSignOut) && !person.shown);
    _navigator->setSearchShown(!_parts.search.shown);
    showNavEntries();
    showTrail();
}

std::vector<MenuEntry> ShellFrame::contextEntries() {
    std::vector<MenuEntry> entries;
    _contextMap.clear();
    std::string group;
    for (std::size_t index = 0; index < _parts.context.options.size(); ++index) {
        ShellOptionView const& option = _parts.context.options.at(index);
        if (!option.group.empty() && option.group != group) {
            MenuEntry heading;
            heading.label = option.group;
            heading.heading = true;
            heading.enabled = false;
            entries.push_back(std::move(heading));
            _contextMap.push_back(std::nullopt);
        }
        group = option.group;
        MenuEntry entry;
        entry.label = option.label;
        entry.description = option.detail;
        entry.checked = _parts.context.chosen == index;
        entries.push_back(std::move(entry));
        _contextMap.push_back(index);
    }
    return entries;
}

MenuEntry ShellFrame::contextMenuEntry() {
    MenuEntry submenu;
    std::string label = _parts.labels.scope;
    if (_parts.context.chosen && *_parts.context.chosen < _parts.context.options.size()) {
        label = _parts.context.options.at(*_parts.context.chosen).label;
    }
    submenu.label = label;
    submenu.items = contextEntries();
    submenu.enabled = _parts.context.enabled;
    return submenu;
}

std::vector<MenuEntry> ShellFrame::principalEntries() {
    std::vector<MenuEntry> entries;
    _principalMap.clear();
    ShellPrincipalView const& person = _parts.principal;
    ShellLabels const& labels = _parts.labels;
    auto const add = [&](MenuEntry entry, PrincipalChoice choice) {
        entries.push_back(std::move(entry));
        _principalMap.push_back(choice);
    };
    auto const heading = [&](std::string const& text) {
        MenuEntry entry;
        entry.label = text;
        entry.heading = true;
        entry.enabled = false;
        add(std::move(entry), PrincipalChoice{});
    };
    auto const separator = [&] { add(MenuEntry{.separator = true}, PrincipalChoice{}); };
    MenuEntry name;
    name.label = person.name;
    name.description = person.detail;
    name.enabled = false;
    add(std::move(name), PrincipalChoice{});
    if (!person.roles.empty()) {
        separator();
        heading(labels.roles);
        for (ShellRoleView const& role : person.roles) {
            MenuEntry entry;
            entry.label = role.label;
            entry.description = role.detail;
            entry.enabled = false;
            add(std::move(entry), PrincipalChoice{});
        }
    }
    if (!person.locales.empty()) {
        separator();
        heading(labels.language);
        for (std::size_t index = 0; index < person.locales.size(); ++index) {
            MenuEntry entry;
            entry.label = person.locales.at(index).label;
            entry.checked = person.chosenLocale == index;
            add(std::move(entry), PrincipalChoice{.kind = PrincipalChoice::Kind::Locale, .index = index});
        }
    }
    if (!person.formats.empty()) {
        separator();
        heading(labels.formats);
        for (std::size_t index = 0; index < person.formats.size(); ++index) {
            MenuEntry entry;
            entry.label = person.formats.at(index).label;
            entry.checked = person.chosenFormat == index;
            add(std::move(entry), PrincipalChoice{.kind = PrincipalChoice::Kind::Format, .index = index});
        }
    }
    if (_onSignOut) {
        separator();
        add(MenuEntry{.label = labels.signOut}, PrincipalChoice{.kind = PrincipalChoice::Kind::SignOut, .index = 0});
    }
    return entries;
}

MenuEntry ShellFrame::principalMenuEntry() {
    MenuEntry submenu;
    submenu.label = _parts.principal.name;
    submenu.description = _parts.principal.subtitle;
    submenu.items = principalEntries();
    return submenu;
}

void ShellFrame::answerConfirm(bool yes) {
    _confirm->setOpen(false);
    if (auto done = std::exchange(_confirmDone, {})) {
        done(yes);
    }
}

void ShellFrame::choosePrincipal(std::size_t entry) {
    if (entry >= _principalMap.size()) {
        return;
    }
    PrincipalChoice const choice = _principalMap.at(entry);
    if (choice.kind == PrincipalChoice::Kind::Locale && _handlers.onLocale) {
        _handlers.onLocale(choice.index);
    } else if (choice.kind == PrincipalChoice::Kind::Format && _handlers.onFormat) {
        _handlers.onFormat(choice.index);
    } else if (choice.kind == PrincipalChoice::Kind::SignOut && _onSignOut) {
        _onSignOut();
    }
}

std::size_t ShellFrame::hitCount() const {
    std::size_t total = 0;
    for (ShellSourceView const& source : _parts.search.sources) {
        total += source.hits.size();
    }
    return total;
}

std::string ShellFrame::hitSignature() const {
    std::string out;
    for (ShellSourceView const& source : _parts.search.sources) {
        out += source.title + "\x1f";
        for (ShellHitView const& hit : source.hits) {
            out += hit.kind + "\x1e" + hit.title + "\x1e" + hit.detail + "\x1d";
        }
    }
    return out;
}

std::optional<std::pair<std::size_t, std::size_t>> ShellFrame::hitAt(std::size_t flat) const {
    for (std::size_t source = 0; source < _parts.search.sources.size(); ++source) {
        std::size_t const size = _parts.search.sources.at(source).hits.size();
        if (flat < size) {
            return std::pair{_parts.search.sources.at(source).source, flat};
        }
        flat -= size;
    }
    return std::nullopt;
}

void ShellFrame::searchEdited(std::string const& text, bool fromOverlay) {
    _searchText = text;
    _searchDismissed = false;
    // The two fields are one search: the one not typed in follows.
    (fromOverlay ? _search : _overlayInput)->setText(text);
    if (_handlers.onSearchText) {
        _handlers.onSearchText(text);
    }
    showSearch();
}

void ShellFrame::clearSearch() {
    _searchDismissed = false;
    _searchText.clear();
    _search->setText({});
    _overlayInput->setText({});
    if (_handlers.onSearchText) {
        _handlers.onSearchText({});
    }
    _overlay->setOpen(false);
    showSearch();
}

void ShellFrame::openHighlighted() {
    if (!_highlight) {
        return;
    }
    if (auto const place = hitAt(*_highlight)) {
        if (_handlers.onSearchOpen) {
            _handlers.onSearchOpen(place->first, place->second);
        }
        clearSearch();
    }
}

void ShellFrame::searchChord(std::string const& chord) {
    std::size_t const total = hitCount();
    if (chord == "Down" || chord == "Up") {
        if (total != 0) {
            std::size_t const at = _highlight.value_or(chord == "Down" ? total - 1 : 0);
            _highlight = chord == "Down" ? (at + 1) % total : (at + total - 1) % total;
            showSearch();
        }
    } else if (chord == "Shift+Enter" || chord == "Alt+Enter") {
        if (total != 0 && _handlers.onSearchOpenAll) {
            _handlers.onSearchOpenAll();
            clearSearch();
        }
    } else if (chord == "Escape") {
        if (_popoverOpen) {
            _searchDismissed = true;
            showSearch();
        } else if (!_searchText.empty()) {
            clearSearch();
        } else {
            if (_compact) {
                _overlay->setOpen(false);
            }
            _content->focus();
        }
    }
}

void ShellFrame::showSearch() {
    if (_dispatching == 0) {
        _retired.clear();
    }
    bool const wanted = _parts.search.shown && !_searchText.empty();
    bool const open = wanted && !_searchDismissed && !_compact;
    bool const overlay = wanted && _compact;
    _popoverOpen = open;
    _popover->setOpen(open);
    std::string key = resultsKey(open, overlay);
    if (key == _resultsKey) {
        return;
    }
    _resultsKey = std::move(key);
    retire(_panelResults);
    retire(_overlayResults);
    if (open) {
        renderResults(*_resultsBody, _panelResults);
    }
    if (overlay) {
        renderResults(*_overlayBody, _overlayResults);
    }
}

std::string ShellFrame::resultsKey(bool panel, bool overlay) const {
    std::string key = std::string{panel ? "1" : "0"} + (overlay ? "1" : "0");
    if (!panel && !overlay) {
        return key;
    }
    ShellLabels const& labels = _parts.labels;
    key += labels.searching + "\x1f" + labels.searchEmpty + "\x1f" + labels.searchFailed + "\x1f" + _searchText +
           "\x1f" + (_highlight ? std::to_string(*_highlight) : std::string{"-"}) + "\x1f";
    for (ShellSourceView const& source : _parts.search.sources) {
        key += std::to_string(source.source) + "\x1e" + source.title + "\x1e" + (source.failed ? "F" : "-") +
               (source.pending ? "P" : "-");
        for (ShellHitView const& hit : source.hits) {
            key += "\x1e" + hit.kind + "\x1d" + hit.title + "\x1d" + hit.detail;
        }
        key += "\x1f";
    }
    return key;
}

void ShellFrame::retire(ResultsColumn& column) {
    if (_dispatching == 0) {
        column.clear();
        return;
    }
    column.hide();
    _retired.push_back(std::move(column));
    column = ResultsColumn{};
}

void ShellFrame::renderResults(StackWidget& into, ResultsColumn& column) {
    ShellLabels const& labels = _parts.labels;
    auto const text = [&](std::string const& value, TextRole role, std::string const& id) {
        auto widget = _backend.createText(&into);
        widget->setText(value);
        widget->setRole(role);
        widget->setTestId(id);
        column.texts.push_back(std::move(widget));
    };
    bool pending = false;
    bool failed = false;
    for (ShellSourceView const& source : _parts.search.sources) {
        pending = pending || source.pending;
        failed = failed || source.failed;
    }
    if (pending) {
        text(labels.searching, TextRole::Muted, "shell_search_busy");
    }
    std::size_t flat = 0;
    for (std::size_t index = 0; index < _parts.search.sources.size(); ++index) {
        ShellSourceView const& source = _parts.search.sources.at(index);
        if (source.hits.empty() && !source.failed) {
            continue;
        }
        text(source.title, TextRole::Overline, "shell_search_source_" + std::to_string(source.source));
        if (source.failed) {
            text(source.title + " " + labels.searchFailed, TextRole::Muted,
                 "shell_search_failed_" + std::to_string(source.source));
        }
        for (std::size_t hit = 0; hit < source.hits.size(); ++hit, ++flat) {
            addHitRow(into, column, source.hits.at(hit), source.source, hit, _highlight == flat);
        }
    }
    if (!pending && !failed && hitCount() == 0) {
        text(labels.searchEmpty + " “" + _searchText + "”", TextRole::Muted, "shell_search_empty");
    }
}

void ShellFrame::addHitRow(StackWidget& into, ResultsColumn& column, ShellHitView const& hit, std::size_t source,
                           std::size_t place, bool highlighted) {
    HitRow row;
    row.button = _backend.createButton(&into);
    row.button->setTestId("shell_search_hit_" + std::to_string(source) + "_" + std::to_string(place));
    row.button->setStyle(ButtonStyle::Item);
    row.button->setLayout({.width = Sizing::stretch(), .height = {}});
    row.button->setSelected(highlighted);
    std::string name = hit.kind;
    name += (name.empty() ? "" : ", ") + hit.title;
    name += hit.detail.empty() ? "" : ", " + hit.detail;
    row.button->setAccessibleName(name);
    row.button->setOnClick([this, source, place] {
        DispatchScope const running{_dispatching};
        if (_handlers.onSearchOpen) {
            _handlers.onSearchOpen(source, place);
        }
        clearSearch();
    });
    row.content = _backend.createStack(&row.button->content(), Axis::Horizontal);
    row.content->setGap(2);
    row.content->setAlign(Align::Center);
    row.content->setLayout({.width = Sizing::stretch(), .height = {}});
    if (!hit.kind.empty()) {
        row.kind = _backend.createText(row.content.get());
        row.kind->setText(hit.kind);
        row.kind->setRole(TextRole::Muted);
    }
    row.title = _backend.createText(row.content.get());
    row.title->setText(hit.title);
    if (!hit.detail.empty()) {
        row.detail = _backend.createText(row.content.get());
        row.detail->setText(hit.detail);
        row.detail->setRole(TextRole::Muted);
    }
    column.rows.push_back(std::move(row));
}
}  // namespace morph::ui
