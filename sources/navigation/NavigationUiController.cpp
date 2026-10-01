#include "NavigationUiController.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "mainwindow.h"
#include "options/MLOptions.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ThreadPaneLayout.h"
#include "ui/IconUtils.h"
#include "ui/ThinSplitter.h"

namespace Mattermost {
namespace {

constexpr int MaxNavigationHistory = 100;

void trimHistory(QVector<NavigationUiController::Location>& history)
{
    if (history.size() > MaxNavigationHistory) {
        history.remove(0, history.size() - MaxNavigationHistory);
    }
}

QIcon threadPresentationIcon(const QPalette& palette)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QPen pen(palette.color(QPalette::ButtonText));
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Deliberately use the literal "two overlapping squares" detach glyph
    // rather than SP_TitleBarNormalButton: desktop styles are free to render
    // that standard pixmap as a platform-specific window-management symbol.
    painter.drawRect(QRect(2, 5, 9, 8));
    painter.drawRect(QRect(5, 2, 9, 8));
    return QIcon(pixmap);
}

QIcon threadTabIcon(const QPalette& palette)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QPen pen(palette.color(QPalette::ButtonText));
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRect(2, 3, 12, 10));
    painter.drawLine(QPoint(5, 6), QPoint(11, 6));
    painter.drawLine(QPoint(8, 3), QPoint(8, 9));
    return QIcon(pixmap);
}

} // namespace

NavigationUiController& NavigationUiController::instance(MainWindow& window)
{
    if (auto* existing = window.findChild<NavigationUiController*>(
            QStringLiteral("navigationUiController"), Qt::FindDirectChildrenOnly)) {
        return *existing;
    }
    return *new NavigationUiController(window);
}

NavigationUiController::NavigationUiController(MainWindow& mainWindow)
    : QObject(&mainWindow)
    , window(mainWindow)
{
    setObjectName(QStringLiteral("navigationUiController"));
    sessionSaveTimer = new QTimer(this);
    sessionSaveTimer->setSingleShot(true);
    sessionSaveTimer->setInterval(500);
    connect(sessionSaveTimer, &QTimer::timeout, this, &NavigationUiController::saveSession);
    setupMainWindow();
    qApp->installEventFilter(this);
}

NavigationUiController::~NavigationUiController()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    if (contentSplitter) {
        MLOptions::instance()->setValue(
            QStringLiteral("content_splitter_state"), contentSplitter->saveState());
    }
}

void NavigationUiController::setupMainWindow()
{
    channelTree = window.findChild<ChannelTree*>(QStringLiteral("channelList"));
    mainStack = window.findChild<QStackedWidget*>(QStringLiteral("chatAreaStackedWidget"));

    setupSidebarHeader();
    setupThreadPane();

    if (Backend* sourceBackend = backend()) {
        auto& sidebar = SidebarService::instance(*sourceBackend);
        connect(&sidebar, &SidebarService::channelActivityChanged,
                this, &NavigationUiController::refreshTabUnreadVisual,
                Qt::UniqueConnection);
        connect(&sidebar, &SidebarService::channelActivityReset,
                this, &NavigationUiController::refreshAllTabUnreadVisuals,
                Qt::UniqueConnection);
        connect(&FollowingModel::instance(*sourceBackend), &FollowingModel::changed,
                this, &NavigationUiController::refreshAllTabUnreadVisuals,
                Qt::UniqueConnection);
    }

    if (mainStack) {
        connect(mainStack, &QStackedWidget::currentChanged, this,
                [this](int index) {
            syncSplitterEdgeGutters();

            QWidget* page = mainStack->widget(index);
            const bool chatSurface = qobject_cast<ChatArea*>(page) != nullptr;
            if (!chatSurface) {
                // Saved/Drafts/Search are central surfaces just like channels.
                // They replace only the central tab surface; a docked thread on
                // the right remains an independent presentation pane.
                if (!switchingTabs && navigationSurfaceStack && mainStack) {
                    navigationSurfaceStack->setCurrentWidget(mainStack);
                }
                if (navigationTabs) {
                    navigationTabs->hide();
                }
                return;
            }

            refreshTabBarVisibility();
        });
    }

    if (channelTree) {
        connect(channelTree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem*, QTreeWidgetItem*) {
            if (switchingTabs || restoringSession) return;
            QTimer::singleShot(0, this, [this] {
                // A deferred sidebar selection must not steal an independently
                // activated thread tab (including startup session replay).
                if (channelTree && (!navigationSurfaceStack
                    || navigationSurfaceStack->currentWidget() == mainStack)) {
                    recordArea(channelTree->getCurrentPage());
                }
            });
        });
        channelTree->installEventFilter(this);
        if (channelTree->viewport()) {
            channelTree->viewport()->installEventFilter(this);
        }
        recordArea(channelTree->getCurrentPage());
    }

    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        saveSession();
        if (contentSplitter) {
            MLOptions::instance()->setValue(
                QStringLiteral("content_splitter_state"), contentSplitter->saveState());
        }
    });

    updateIdentityTooltip();
    updateHistoryButtons();
}

void NavigationUiController::setupSidebarHeader()
{
    auto* header = window.findChild<QWidget*>(QStringLiteral("lefttop_frame"));
    auto* searchButton = window.findChild<QToolButton*>(QStringLiteral("searchButton"));
    auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    if (username) {
        username->hide();
    }
    if (status) {
        // PresenceStatusLabel keeps forwarding status changes into the avatar;
        // hiding the textual compatibility widget does not disable that path.
        status->hide();
    }
    if (!header || !searchButton) {
        return;
    }

    auto* layout = header->findChild<QHBoxLayout*>(QStringLiteral("horizontalLayout"));
    if (!layout) {
        return;
    }

    backButton = new QToolButton(header);
    backButton->setObjectName(QStringLiteral("navigationBackButton"));
    backButton->setAutoRaise(true);
    backButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    backButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowBack));
    backButton->setToolTip(tr("Back"));
    backButton->setAccessibleName(tr("Back"));

    forwardButton = new QToolButton(header);
    forwardButton->setObjectName(QStringLiteral("navigationForwardButton"));
    forwardButton->setAutoRaise(true);
    forwardButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    forwardButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowForward));
    forwardButton->setToolTip(tr("Forward"));
    forwardButton->setAccessibleName(tr("Forward"));

    int searchIndex = layout->indexOf(searchButton);
    if (searchIndex < 0) {
        searchIndex = layout->count();
    }
    layout->insertWidget(searchIndex, backButton);
    layout->insertWidget(searchIndex + 1, forwardButton);

    connect(backButton, &QToolButton::clicked, this, &NavigationUiController::goBack);
    connect(forwardButton, &QToolButton::clicked, this, &NavigationUiController::goForward);

    auto* backShortcut = new QShortcut(QKeySequence(QKeySequence::Back), &window);
    auto* forwardShortcut = new QShortcut(QKeySequence(QKeySequence::Forward), &window);
    connect(backShortcut, &QShortcut::activated, this, &NavigationUiController::goBack);
    connect(forwardShortcut, &QShortcut::activated, this, &NavigationUiController::goForward);
}

void NavigationUiController::setupThreadPane()
{
    if (!mainStack) {
        return;
    }

    const auto splitters = window.findChildren<QSplitter*>();
    for (QSplitter* candidate : splitters) {
        if (candidate && candidate->indexOf(mainStack) >= 0) {
            sidebarSplitter = candidate;
            break;
        }
    }
    if (!sidebarSplitter) {
        return;
    }
    sidebarSplitter->setObjectName(QStringLiteral("sidebarSplitter"));

    const int oldIndex = sidebarSplitter->indexOf(mainStack);
    const QList<int> outerSizes = sidebarSplitter->sizes();

    // Build the replacement pane parentless. MainWindow is already visible when
    // this controller is bootstrapped; constructing a QWidget directly under a
    // visible QSplitter leaves the new subtree explicitly hidden while mainStack
    // is reparented into it. insertWidget() below becomes the single ownership
    // transition, matching the previously working splitter setup.
    contentHost = new QWidget;
    contentHost->setObjectName(QStringLiteral("navigationContentHost"));
    auto* hostLayout = new QVBoxLayout(contentHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);

    navigationTabs = new QTabBar(contentHost);
    navigationTabs->setObjectName(QStringLiteral("navigationTabs"));
    navigationTabs->setDocumentMode(true);
    navigationTabs->setMovable(true);
    navigationTabs->setTabsClosable(true);
    navigationTabs->setExpanding(false);
    navigationTabs->setUsesScrollButtons(true);
    navigationTabs->setElideMode(Qt::ElideRight);
    navigationTabs->installEventFilter(this);
    navigationTabs->hide();
    hostLayout->addWidget(navigationTabs);

    // Central navigation and the right thread pane are independent siblings.
    // A channel or thread tab replaces only the left/central surface; it must
    // never hide a docked thread that happens to be open at the same time.
    contentSplitter = new ThinSplitter(Qt::Horizontal);
    contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter->setChildrenCollapsible(true);
    contentSplitter->setOpaqueResize(true);
    hostLayout->addWidget(contentSplitter, 1);

    navigationSurfaceStack = new QStackedWidget(contentSplitter);
    navigationSurfaceStack->setObjectName(QStringLiteral("navigationSurfaceStack"));
    navigationSurfaceStack->addWidget(mainStack);
    navigationSurfaceStack->setCurrentWidget(mainStack);
    contentSplitter->addWidget(navigationSurfaceStack);

    threadStack = new QStackedWidget(contentSplitter);
    threadStack->setObjectName(QStringLiteral("threadStack"));
    threadStack->setMinimumWidth(280);
    threadStack->hide();
    contentSplitter->addWidget(threadStack);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 1);

    sidebarSplitter->insertWidget(std::max(0, oldIndex), contentHost);
    if (!outerSizes.isEmpty()) {
        sidebarSplitter->setSizes(outerSizes);
    }

    // Reparenting a visible widget hides it. Establish the central surface and
    // splitter explicitly; thread presentation controls only the right sibling.
    contentHost->show();
    contentSplitter->show();
    navigationSurfaceStack->show();
    mainStack->show();

    const QByteArray state = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("content_splitter_state"));
    if (!state.isEmpty()) {
        threadSplitterStateRestored = contentSplitter->restoreState(state);
    }
    // QSplitter persists handle width in its state; do not let legacy 4 px
    // states reintroduce layout space between channel and thread panes.
    contentSplitter->setHandleWidth(ThinSplitter::VisibleHandleExtent);

    connect(navigationTabs, &QTabBar::currentChanged, this,
            [this](int index) {
        if (!switchingTabs && index >= 0) {
            activateTab(index);
        }
    });
    connect(navigationTabs, &QTabBar::tabCloseRequested,
            this, &NavigationUiController::closeTab);
    connect(navigationTabs, &QTabBar::tabMoved, this,
            [this](int from, int to) {
        tabModel.move(from, to);
        scheduleSessionSave();
        if (activeTabIndex == from) {
            activeTabIndex = to;
        } else if (from < activeTabIndex && activeTabIndex <= to) {
            --activeTabIndex;
        } else if (to <= activeTabIndex && activeTabIndex < from) {
            ++activeTabIndex;
        }
    });

    setupTabShortcuts();
}

void NavigationUiController::setupTabShortcuts()
{
    if (!navigationTabs) {
        return;
    }

    auto* closeShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), &window);
    auto* previousShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+PgUp")), &window);
    auto* nextShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+PgDn")), &window);

    connect(closeShortcut, &QShortcut::activated,
            this, &NavigationUiController::closeActiveTab);
    connect(previousShortcut, &QShortcut::activated,
            this, &NavigationUiController::activatePreviousTab);
    connect(nextShortcut, &QShortcut::activated,
            this, &NavigationUiController::activateNextTab);
}

ChatArea* NavigationUiController::centralTabArea() const
{
    if (!navigationSurfaceStack) {
        return nullptr;
    }
    QWidget* surface = navigationSurfaceStack->currentWidget();
    if (!surface) {
        return nullptr;
    }
    if (surface == mainStack) {
        return mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget())
            : nullptr;
    }
    return qobject_cast<ChatArea*>(surface);
}

void NavigationUiController::closeActiveTab()
{
    if (!navigationTabs || activeTabIndex < 0 || tabModel.count() <= 1) {
        return;
    }
    if (!centralTabArea()) {
        return;
    }
    closeTab(activeTabIndex);
}

void NavigationUiController::activatePreviousTab()
{
    if (!navigationTabs || tabModel.count() <= 1) {
        return;
    }
    const int next = tabModel.cycledIndex(activeTabIndex, -1);
    if (next >= 0 && next != activeTabIndex) {
        activateTab(next);
    }
}

void NavigationUiController::activateNextTab()
{
    if (!navigationTabs || tabModel.count() <= 1) {
        return;
    }
    const int next = tabModel.cycledIndex(activeTabIndex, 1);
    if (next >= 0 && next != activeTabIndex) {
        activateTab(next);
    }
}

void NavigationUiController::updateIdentityTooltip()
{
    auto* avatar = window.findChild<QWidget*>(QStringLiteral("usericon_label"));
    if (!avatar) {
        return;
    }

    Backend* sourceBackend = backend();
    if (!sourceBackend) {
        if (auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"))) {
            if (!username->text().isEmpty()) {
                avatar->setToolTip(username->text());
            }
        }
        QTimer::singleShot(500, this, &NavigationUiController::updateIdentityTooltip);
        return;
    }

    const BackendUser& user = sourceBackend->getLoginUser();
    const QString displayName = user.getDisplayName().trimmed();
    const QString accountName = user.username.trimmed();
    QStringList lines;
    if (!displayName.isEmpty()) {
        lines.push_back(displayName);
    }
    if (!accountName.isEmpty()
        && QString::compare(displayName, accountName, Qt::CaseInsensitive) != 0) {
        lines.push_back(QStringLiteral("@") + accountName);
    }
    if (lines.isEmpty() && !accountName.isEmpty()) {
        lines.push_back(accountName);
    }
    avatar->setToolTip(lines.join(QLatin1Char('\n')));
}

Backend* NavigationUiController::backend() const
{
    return channelTree ? channelTree->backendInstance() : nullptr;
}

NavigationUiController::Location
NavigationUiController::captureLocation(ChatArea* area) const
{
    Location location;
    if (!area) {
        return location;
    }

    location.channelId = area->getChannel().id;
    if (area->isThread) {
        location.rootId = area->root_id;
    }
    const QString pendingBookmark = area->property("sessionBookmark").toString();
    if (!pendingBookmark.isEmpty()) {
        location.postId = pendingBookmark;
        return location;
    }

    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        QString postId;
        if (log->captureViewportBookmark(postId)) {
            location.postId = postId;
            return location;
        }
    }

    location.postId = area->storedNavigationBookmark();
    return location;
}

NavigationTabsModel::Entry
NavigationUiController::tabEntry(const Location& location) const
{
    NavigationTabsModel::Entry entry;
    entry.channelId = location.channelId;
    entry.rootId = location.rootId;
    entry.postId = location.postId;
    entry.title = tabTitle(location);
    return entry;
}

NavigationUiController::Location
NavigationUiController::tabLocation(const NavigationTabsModel::Entry& entry) const
{
    Location location;
    location.channelId = entry.channelId;
    location.rootId = entry.rootId;
    location.postId = entry.postId;
    return location;
}

QString NavigationUiController::tabTitle(const Location& location) const
{
    QString title = location.channelId;
    if (Backend* sourceBackend = backend()) {
        if (BackendChannel* channel =
                sourceBackend->getStorage().getChannelById(location.channelId)) {
            title = channel->display_name.trimmed();
            if (title.isEmpty()) {
                title = channel->name.trimmed();
            }
            if (title.isEmpty()) {
                title = channel->id;
            }
        }
    }

    if (!location.rootId.isEmpty()) {
        title = tr("%1 · Thread").arg(title);
    }
    return title;
}

int NavigationUiController::appendNavigationTab(const Location& location)
{
    if (!navigationTabs || !location.isValid()) {
        return -1;
    }

    const int oldCount = tabModel.count();
    const int index = tabModel.append(tabEntry(location));
    if (index < 0) {
        return -1;
    }

    const auto* entry = tabModel.at(index);
    const QString title = entry ? entry->title : tabTitle(location);
    if (index >= oldCount) {
        navigationTabs->addTab(title);
    }
    setNavigationTabTitle(index, title);
    refreshTabUnreadVisual(location.channelId);
    refreshTabBarVisibility();
    return index;
}

void NavigationUiController::setNavigationTabTitle(
    int index, const QString& title)
{
    if (!navigationTabs || index < 0 || index >= navigationTabs->count()) {
        return;
    }

    navigationTabs->setTabText(index, title);
    navigationTabs->setTabToolTip(index, title);
}

void NavigationUiController::refreshTabUnreadVisual(const QString& channelId)
{
    if (!navigationTabs || channelId.isEmpty()) {
        return;
    }

    Backend* sourceBackend = backend();
    BackendChannel* channel = sourceBackend
        ? sourceBackend->getStorage().getChannelById(channelId)
        : nullptr;
    if (!sourceBackend || !channel) {
        return;
    }

    auto& sidebar = SidebarService::instance(*sourceBackend);
    auto& following = FollowingModel::instance(*sourceBackend);
    const bool channelMarked =
        sidebar.isChannelUnread(*channel)
        || sidebar.hasUnreadMention(channelId);

    for (int index = 0; index < tabModel.count(); ++index) {
        const auto* entry = tabModel.at(index);
        if (!entry || entry->channelId != channelId) {
            continue;
        }

        // A thread is an independent read domain. Reading it cannot consume
        // parent-channel activity, so its tab must use the same CRT/manual
        // attention state as its own Following row.
        const auto* thread = entry->rootId.isEmpty()
            ? nullptr : following.findEntry(channelId, entry->rootId);
        const bool marked = entry->rootId.isEmpty()
            ? channelMarked : thread && thread->requiresAttention();

        navigationTabs->setTabText(
            index,
            marked ? QStringLiteral("★ ") + entry->title : entry->title);
    }
}

void NavigationUiController::refreshAllTabUnreadVisuals()
{
    QSet<QString> channelIds;
    for (int index = 0; index < tabModel.count(); ++index) {
        if (const auto* entry = tabModel.at(index);
            entry && !entry->channelId.isEmpty()) {
            channelIds.insert(entry->channelId);
        }
    }
    for (const QString& channelId : channelIds) {
        refreshTabUnreadVisual(channelId);
    }
}

void NavigationUiController::ensureInitialTab()
{
    if (!navigationTabs || !tabModel.isEmpty()) {
        return;
    }

    auto* area = mainStack
        ? qobject_cast<ChatArea*>(mainStack->currentWidget())
        : nullptr;
    const Location location = captureLocation(area);
    if (!location.isValid()) {
        return;
    }

    const int index = appendNavigationTab(location);
    if (index < 0) {
        return;
    }

    activeTabIndex = index;
    switchingTabs = true;
    navigationTabs->setCurrentIndex(index);
    switchingTabs = false;
}

void NavigationUiController::refreshTabBarVisibility()
{
    if (navigationTabs) {
        navigationTabs->setVisible(tabModel.shouldShowTabBar());
    }
}

void NavigationUiController::updateTab(int index, const Location& location)
{
    if (!navigationTabs || !location.isValid()
        || !tabModel.replace(index, tabEntry(location))) {
        return;
    }
    setNavigationTabTitle(index, tabTitle(location));
    refreshTabUnreadVisual(location.channelId);
}

void NavigationUiController::saveActiveTabLocation()
{
    const auto* entry = tabModel.at(activeTabIndex);
    if (!entry) {
        return;
    }

    ChatArea* area = nullptr;
    if (!entry->rootId.isEmpty()) {
        area = findThread(entry->channelId, entry->rootId);
    } else if (!navigationSurfaceStack
               || navigationSurfaceStack->currentWidget() == mainStack) {
        area = mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget())
            : nullptr;
    }

    if (!area) {
        return;
    }

    Location location = captureLocation(area);
    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        log && log->source() && log->isAtEnd()
        && area->property("sessionBookmark").toString().isEmpty()) {
        location.postId.clear();
    }
    const Location expected = tabLocation(*entry);
    if (location.isValid() && location.sameDestination(expected)) {
        updateTab(activeTabIndex, location);
    }
}

int NavigationUiController::firstChannelTab() const
{
    for (int i = 0; i < tabModel.count(); ++i) {
        const auto* entry = tabModel.at(i);
        if (entry && entry->rootId.isEmpty()) {
            return i;
        }
    }
    return -1;
}

int NavigationUiController::tabIndexForThread(ChatArea* area) const
{
    if (!area || !area->isThread) {
        return -1;
    }
    return tabModel.findDestination(area->getChannel().id, area->root_id);
}

int NavigationUiController::releaseThreadFromTabSurface(ChatArea* area)
{
    if (!area || !area->isThread
        || !area->property("threadTabbed").toBool()) {
        return -1;
    }

    const int tabIndex = tabIndexForThread(area);
    if (tabIndex < 0) {
        return -1;
    }

    // Presentation transitions own QWidget reparenting. Remove the thread from
    // the tab surface first and clear the tabbed marker so removeTab() will only
    // remove semantic/tab-bar state and cannot perform a second widget move.
    area->hide();
    if (navigationSurfaceStack
        && navigationSurfaceStack->indexOf(area) >= 0) {
        navigationSurfaceStack->removeWidget(area);
    }
    area->setProperty("threadTabbed", false);
    return tabIndex;
}

void NavigationUiController::activateTab(int index, bool restoreBookmark)
{
    const auto* entry = tabModel.at(index);
    if (!entry || !navigationTabs) {
        return;
    }

    if (activeTabIndex != index) {
        saveActiveTabLocation();
    }

    Location location = tabLocation(*entry);
    if (!restoreBookmark) {
        location.postId.clear();
    }
    activeTabIndex = index;

    if (navigationTabs->currentIndex() != index) {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }

    const bool previousSwitching = switchingTabs;
    switchingTabs = true;

    if (!location.rootId.isEmpty()) {
        ChatArea* area = findThread(location.channelId, location.rootId);
        Backend* sourceBackend = backend();
        BackendChannel* channel = sourceBackend
            ? sourceBackend->getStorage().getChannelById(location.channelId)
            : nullptr;
        if (!area && sourceBackend && channel) {
            ChatArea* parent = channelTree ? channelTree->getCurrentPage() : nullptr;
            if (!parent || &parent->getChannel() != channel) {
                parent = nullptr;
            }
            area = new ChatArea(*sourceBackend, *channel, location.rootId, parent);
            if (parent) {
                parent->threadsAreas.insert(area);
            }
        }
        if (area) {
            tabifyThread(area, index);
            currentLocation = location;
            activeArea = area;
            if (!location.postId.isEmpty()) {
                restoreSessionBookmark(area, location.postId);
            }
        }
    } else {
        if (navigationSurfaceStack && mainStack) {
            navigationSurfaceStack->setCurrentWidget(mainStack);
            mainStack->show();
        }
        navigateTo(location);
    }

    switchingTabs = previousSwitching;
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
}

void NavigationUiController::removeTab(int index, bool closeThread)
{
    if (!navigationTabs) {
        return;
    }
    const auto* entryPtr = tabModel.at(index);
    if (!entryPtr) {
        return;
    }

    const NavigationTabsModel::Entry removedEntry = *entryPtr;
    const bool wasActive = index == activeTabIndex;
    if (wasActive) {
        saveActiveTabLocation();
    }

    ChatArea* thread = nullptr;
    if (!removedEntry.rootId.isEmpty()) {
        thread = findThread(removedEntry.channelId, removedEntry.rootId);
        if (thread && thread->property("threadTabbed").toBool()) {
            thread->hide();
            if (navigationSurfaceStack
                && navigationSurfaceStack->indexOf(thread) >= 0) {
                navigationSurfaceStack->removeWidget(thread);
            }
            thread->setProperty("threadTabbed", false);
        }
    }

    int nextIndex = -1;
    {
        QSignalBlocker blocker(navigationTabs);
        tabModel.remove(index);
        navigationTabs->removeTab(index);

        if (tabModel.isEmpty()) {
            Location fallback;
            if (ChatArea* channelArea =
                    channelTree ? channelTree->getCurrentPage() : nullptr) {
                fallback = captureLocation(channelArea);
            }
            if (!fallback.isValid()) {
                fallback.channelId = removedEntry.channelId;
            }
            nextIndex = appendNavigationTab(fallback);
        } else if (wasActive) {
            nextIndex = std::min(index, tabModel.count() - 1);
        } else {
            activeTabIndex -= activeTabIndex > index ? 1 : 0;
            nextIndex = activeTabIndex;
        }

        if (nextIndex >= 0) {
            navigationTabs->setCurrentIndex(nextIndex);
        }
    }

    refreshTabBarVisibility();

    if (thread) {
        if (closeThread) {
            thread->close();
            thread = nullptr;
        } else {
            updateThreadButton(thread);
        }
    }

    if (wasActive && nextIndex >= 0) {
        activeTabIndex = -1;
        activateTab(nextIndex);
    }
}

void NavigationUiController::closeTab(int index)
{
    removeTab(index, true);
    scheduleSessionSave();
}

void NavigationUiController::tabifyThread(ChatArea* area, int tabIndex)
{
    if (!area || !area->isThread || !navigationSurfaceStack) {
        return;
    }

    ensureThreadButton(area);

    const bool wasDockedCurrent =
        threadStack && threadStack->currentWidget() == area;
    area->hide();
    if (threadStack && threadStack->indexOf(area) >= 0) {
        threadStack->removeWidget(area);
        if (wasDockedCurrent) {
            threadStack->hide();
        }
    }

    if (area->isWindow() || area->parentWidget() != navigationSurfaceStack) {
        area->setParent(navigationSurfaceStack, Qt::Widget);
    }
    if (navigationSurfaceStack->indexOf(area) < 0) {
        navigationSurfaceStack->addWidget(area);
    }

    area->setProperty("threadDetached", false);
    area->setProperty("threadTabbed", true);
    area->setSplitterEdgeGutters(false, false);
    navigationSurfaceStack->setCurrentWidget(area);
    area->show();

    activeTabIndex = tabIndex;
    updateThreadButton(area);
    recordArea(area);
    syncSplitterEdgeGutters();
}

bool NavigationUiController::activateExistingTab(
    const QString& channelId,
    const QString& rootId,
    bool restoreBookmark)
{
    if (!navigationTabs || channelId.isEmpty()) {
        return false;
    }

    int index = -1;
    const auto* activeEntry = tabModel.at(activeTabIndex);
    if (activeEntry
        && activeEntry->channelId == channelId
        && activeEntry->rootId == rootId) {
        index = activeTabIndex;
    } else {
        index = tabModel.findDestination(channelId, rootId);
    }

    if (index < 0) {
        return false;
    }

    {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }
    activateTab(index, restoreBookmark);
    return true;
}

void NavigationUiController::openInTab(
    const QString& channelId,
    const QString& rootId,
    const QString& postId,
    const QStringList& contextPostIds,
    bool reachedOldest,
    bool reachedNewest)
{
    Backend* sourceBackend = backend();
    BackendChannel* channel = sourceBackend
        ? sourceBackend->getStorage().getChannelById(channelId)
        : nullptr;
    if (!navigationTabs || !channel) {
        return;
    }

    ensureInitialTab();

    // The tab entry contains passive revisit state only. An explicit post target
    // must not be converted into a bookmark because bookmark restoration is
    // intentionally silent (no navigation highlight).
    Location destination;
    destination.channelId = channelId;
    destination.rootId = rootId;

    const int index = appendNavigationTab(destination);
    if (index < 0) {
        return;
    }

    {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }
    activateTab(index, postId.isEmpty());

    if (postId.isEmpty()) {
        return;
    }

    // Use the exact explicit-post path used outside tabs. It owns context
    // installation, viewport locking and highlightPostWhenReady().
    window.openChannelPost(channelId, postId, rootId, contextPostIds,
                           reachedOldest, reachedNewest, false);
}

void NavigationUiController::recordArea(ChatArea* area)
{
    if (!area) {
        return;
    }
    trackSessionArea(area);
    if (!area->isThread) {
        ensureTabPinButton(area);
    }
    scheduleSessionSave();

    const Location next = captureLocation(area);
    if (!next.isValid()) {
        return;
    }

    if (navigationTabs && !switchingTabs) {
        const int existingIndex =
            tabModel.findDestination(next.channelId, next.rootId);
        if (existingIndex >= 0 && existingIndex != activeTabIndex) {
            {
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(existingIndex);
            }
            activateTab(existingIndex);
            return;
        }

        if (area->isThread && area->property("threadTabbed").toBool()) {
            const int index = tabIndexForThread(area);
            if (index >= 0) {
                updateTab(index, next);
                activeTabIndex = index;
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
            }
        } else if (!area->isThread) {
            int index = -1;
            if (existingIndex == activeTabIndex && activeTabIndex >= 0) {
                index = activeTabIndex;
                updateTab(index, next);
            } else if (tabModel.isEmpty()) {
                index = appendNavigationTab(next);
            } else {
                const auto* activeEntry = tabModel.at(activeTabIndex);
                if (activeEntry && activeEntry->rootId.isEmpty()) {
                    if (activeEntry->pinned) {
                        // A pinned current tab behaves like a browser pinned tab:
                        // ordinary navigation may activate an existing target,
                        // but a new destination gets its own new tab.
                        index = appendNavigationTab(next);
                    } else {
                        index = activeTabIndex;
                        updateTab(index, next);
                    }
                } else {
                    // Thread tabs are never channel reuse targets. Reuse an
                    // existing ordinary channel slot if one is available, but
                    // never overwrite a pinned background channel tab.
                    index = tabModel.findReusableChannelTab();
                    if (index >= 0) {
                        updateTab(index, next);
                    } else {
                        index = appendNavigationTab(next);
                    }
                }
            }

            if (index >= 0) {
                activeTabIndex = index;
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
                if (navigationSurfaceStack && mainStack) {
                    navigationSurfaceStack->setCurrentWidget(mainStack);
                }
            }
            updateTabPinButton(area);
        }
        refreshTabBarVisibility();
    }

    if (!currentLocation.isValid()) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (currentLocation.sameDestination(next)) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }

    if (!replayingHistory && currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
        forwardStack.clear();
    }

    currentLocation = next;
    activeArea = area;
    updateHistoryButtons();
}

void NavigationUiController::updateHistoryButtons()
{
    if (backButton) {
        backButton->setEnabled(!backStack.isEmpty());
    }
    if (forwardButton) {
        forwardButton->setEnabled(!forwardStack.isEmpty());
    }
}

void NavigationUiController::goBack()
{
    if (backStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        forwardStack.push_back(currentLocation);
        trimHistory(forwardStack);
    }

    const Location target = backStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::goForward()
{
    if (forwardStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
    }

    const Location target = forwardStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::navigateTo(const Location& location)
{
    Backend* sourceBackend = backend();
    if (!sourceBackend || !channelTree || !location.isValid()) {
        return;
    }

    BackendChannel* channel = sourceBackend->getStorage().getChannelById(location.channelId);
    if (!channel) {
        return;
    }

    ChatArea* area = nullptr;
    if (location.rootId.isEmpty()) {
        channelTree->openStoredChannel(location.channelId);
        area = channelTree->getCurrentPage();
        if (!area || &area->getChannel() != channel) {
            return;
        }
        recordArea(area);
    } else {
        area = findThread(location.channelId, location.rootId);
        if (!area) {
            ChatArea* parent = channelTree->getCurrentPage();
            if (!parent || &parent->getChannel() != channel) {
                parent = nullptr;
            }
            area = new ChatArea(*sourceBackend, *channel, location.rootId, parent);
            if (parent) {
                parent->threadsAreas.insert(area);
            }
        }
        presentThread(area);
    }

    currentLocation = location;
    activeArea = area;
    if (location.postId.isEmpty() || !area) {
        return;
    }

    // Passive bookmarks resolve cold bodies without flashing a permalink or
    // changing the current semantic destination.
    restoreSessionBookmark(area, location.postId);
}

ChatArea* NavigationUiController::findThread(const QString& channelId,
                                             const QString& rootId) const
{
    if (channelId.isEmpty() || rootId.isEmpty()) {
        return nullptr;
    }

    const auto widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || area->root_id != rootId) {
            continue;
        }
        if (area->getChannel().id == channelId) {
            return area;
        }
    }
    return nullptr;
}

void NavigationUiController::ensureTabPinButton(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    auto* layout = area->findChild<QHBoxLayout*>(QStringLiteral("propertieslLayout"));
    if (!layout) {
        return;
    }

    auto* button = area->findChild<QToolButton*>(QStringLiteral("tabPinButton"));
    if (!button) {
        button = new QToolButton(area);
        button->setObjectName(QStringLiteral("tabPinButton"));
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(16, 16));
        button->setCursor(Qt::PointingHandCursor);
        button->installEventFilter(this);
        layout->addWidget(button, 0, Qt::AlignVCenter);

        connect(button, &QToolButton::clicked, this,
                [this, area](bool checked) {
            const int index = tabModel.findDestination(
                area->getChannel().id, QString());
            if (index >= 0) {
                tabModel.setPinned(index, checked);
                scheduleSessionSave();
            }
            updateTabPinButton(area);
        });
    }

    updateTabPinButton(area);
}

void NavigationUiController::updateTabPinButton(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    auto* button = area->findChild<QToolButton*>(QStringLiteral("tabPinButton"));
    if (!button) {
        return;
    }

    const int index = tabModel.findDestination(area->getChannel().id, QString());
    const auto* entry = tabModel.at(index);
    const bool available = entry && entry->rootId.isEmpty();
    const bool pinned = available && entry->pinned;

    QSignalBlocker blocker(button);
    button->setVisible(available);
    button->setEnabled(available);
    button->setChecked(pinned);

    auto iconColor = button->palette().color(QPalette::ButtonText);
    if (!pinned) {
        iconColor.setAlpha(150);
    }
    button->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/tab-pin"), iconColor));

    const QString label = pinned ? tr("Unpin tab") : tr("Pin tab");
    button->setToolTip(label);
    button->setAccessibleName(label);
}

void NavigationUiController::ensureThreadButton(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }
    trackSessionArea(area);

    auto* layout = area->findChild<QHBoxLayout*>(QStringLiteral("propertieslLayout"));
    if (!layout) {
        return;
    }

    auto* tabButton = area->findChild<QToolButton*>(
        QStringLiteral("threadTabButton"));
    if (!tabButton) {
        tabButton = new QToolButton(area);
        tabButton->setObjectName(QStringLiteral("threadTabButton"));
        tabButton->setAutoRaise(true);
        tabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        tabButton->setIconSize(QSize(16, 16));
        tabButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(tabButton, 0, Qt::AlignVCenter);

        connect(tabButton, &QToolButton::clicked, this, [this, area] {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (!guard) {
                    return;
                }
                if (guard->property("threadTabbed").toBool()) {
                    attachThread(guard);
                    return;
                }
                if (Backend* sourceBackend = backend()) {
                    AppNavigationService::instance(*sourceBackend).openThreadInTab(
                        guard->getChannel().id, guard->root_id);
                }
            });
        });
    }

    auto* presentationButton = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!presentationButton) {
        presentationButton = new QToolButton(area);
        presentationButton->setObjectName(QStringLiteral("threadPresentationButton"));
        presentationButton->setAutoRaise(true);
        presentationButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        presentationButton->setIconSize(QSize(16, 16));
        presentationButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(presentationButton, 0, Qt::AlignVCenter);

        connect(presentationButton, &QToolButton::clicked, this, [this, area] {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (!guard) {
                    return;
                }
                if (guard->property("threadDetached").toBool()) {
                    attachThread(guard);
                } else {
                    detachThread(guard);
                }
            });
        });
    }

    auto* closeButton = area->findChild<QToolButton*>(QStringLiteral("threadCloseButton"));
    if (!closeButton) {
        closeButton = new QToolButton(area);
        closeButton->setObjectName(QStringLiteral("threadCloseButton"));
        closeButton->setAutoRaise(true);
        closeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        closeButton->setIconSize(QSize(16, 16));
        closeButton->setCursor(Qt::PointingHandCursor);
        closeButton->setIcon(area->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        const QString label = tr("Close thread");
        closeButton->setToolTip(label);
        closeButton->setAccessibleName(label);
        layout->addWidget(closeButton, 0, Qt::AlignVCenter);

        connect(closeButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }

            if (area->property("threadTabbed").toBool()) {
                const int index = tabIndexForThread(area);
                if (index >= 0) {
                    closeTab(index);
                    return;
                }
            }

            const bool wasActive = activeArea == area;
            bool wasDockedCurrent = false;
            if (threadStack && threadStack->indexOf(area) >= 0) {
                wasDockedCurrent = threadStack->currentWidget() == area;
                area->hide();
                threadStack->removeWidget(area);
                if (wasDockedCurrent) {
                    threadStack->hide();
                }
            }

            if (wasDockedCurrent) {
                syncSplitterEdgeGutters();
            }
            area->setSplitterEdgeGutters(false, false);
            area->close();
            if (wasActive) {
                QTimer::singleShot(0, this, [this] {
                    if (channelTree) {
                        recordArea(channelTree->getCurrentPage());
                    }
                });
            }
        });
    }

    updateThreadButton(area);
}

void NavigationUiController::updateThreadButton(ChatArea* area)
{
    if (!area) {
        return;
    }
    auto* button = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!button) {
        return;
    }

    const bool detached = area->property("threadDetached").toBool();
    const QString label = detached ? tr("Attach thread") : tr("Detach thread");
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setIcon(threadPresentationIcon(button->palette()));

    if (auto* tabButton = area->findChild<QToolButton*>(
            QStringLiteral("threadTabButton"))) {
        const bool tabbed = area->property("threadTabbed").toBool();
        const QString tabLabel = tabbed ? tr("Attach thread")
                                       : tr("Open thread in new tab");
        tabButton->setToolTip(tabLabel);
        tabButton->setAccessibleName(tabLabel);
        tabButton->setIcon(threadTabIcon(tabButton->palette()));
    }
}

void NavigationUiController::attachThread(ChatArea* area)
{
    if (!area || !threadStack || !contentSplitter) {
        return;
    }

    ensureThreadButton(area);

    const int releasedTabIndex = releaseThreadFromTabSurface(area);
    if (releasedTabIndex >= 0) {
        // Removing the tab already selects the next central destination. Do not
        // force a channel tab here: a thread tab in the central surface is fully
        // compatible with another thread docked on the right.
        removeTab(releasedTabIndex, false);
    }

    if (auto* previous = qobject_cast<ChatArea*>(threadStack->currentWidget());
        previous && previous != area) {
        previous->setSplitterEdgeGutters(false, false);
    }

    area->hide();
    if (threadStack->indexOf(area) < 0) {
        area->setParent(threadStack, Qt::Widget);
        threadStack->addWidget(area);
    }
    area->setProperty("threadDetached", false);
    area->setProperty("threadTabbed", false);
    threadStack->setCurrentWidget(area);

    // A hidden splitter child is reported as size 0 regardless of the width
    // encoded in restoreState(). Make the pane visible first, then decide
    // whether the restored/current geometry is actually usable.
    threadStack->show();
    area->show();
    syncSplitterEdgeGutters();

    if (ensureThreadPaneExpanded(*contentSplitter, !threadSplitterStateRestored)) {
        threadSplitterStateRestored = true;
    }
    updateThreadButton(area);
    recordArea(area);
}

void NavigationUiController::detachThread(ChatArea* area)
{
    if (!area || !threadStack) {
        return;
    }

    const int releasedTabIndex = releaseThreadFromTabSurface(area);

    const bool wasCurrent = threadStack->currentWidget() == area;
    area->hide();
    if (threadStack->indexOf(area) >= 0) {
        threadStack->removeWidget(area);
    }

    area->setSplitterEdgeGutters(false, false);

    // Complete the QWidget state transition before removing the semantic tab.
    // removeTab() can then activate the replacement central tab without racing
    // another reparent of this thread.
    area->setParent(nullptr, Qt::Window);
    area->setAttribute(Qt::WA_DeleteOnClose, true);
    area->setProperty("threadDetached", true);
    area->setProperty("threadTabbed", false);
    updateThreadButton(area);

    if (releasedTabIndex >= 0) {
        removeTab(releasedTabIndex, false);
    }

    if (wasCurrent) {
        threadStack->hide();
    }
    syncSplitterEdgeGutters();
    area->show();
    area->raise();
    area->activateWindow();
    recordArea(area);
}

void NavigationUiController::syncSplitterEdgeGutters()
{
    ChatArea* centralArea = nullptr;
    if (navigationSurfaceStack) {
        QWidget* centralSurface = navigationSurfaceStack->currentWidget();
        if (centralSurface == mainStack) {
            centralArea = mainStack
                ? qobject_cast<ChatArea*>(mainStack->currentWidget())
                : nullptr;
        } else {
            centralArea = qobject_cast<ChatArea*>(centralSurface);
        }
    } else if (mainStack) {
        centralArea = qobject_cast<ChatArea*>(mainStack->currentWidget());
    }

    auto* threadArea = threadStack
        ? qobject_cast<ChatArea*>(threadStack->currentWidget())
        : nullptr;
    const bool threadDocked = threadStack && !threadStack->isHidden()
        && threadArea && !threadArea->property("threadDetached").toBool();

    if (centralArea) {
        // Whatever occupies the central surface touches the outer/sidebar edge
        // on the left and the thread splitter only while a docked pane is open.
        centralArea->setSplitterEdgeGutters(true, threadDocked);
    }
    if (threadArea && threadDocked) {
        threadArea->setSplitterEdgeGutters(true, false);
    }
}

void NavigationUiController::presentChannel(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    if (navigationSurfaceStack && mainStack) {
        navigationSurfaceStack->setCurrentWidget(mainStack);
        mainStack->show();
    }

    recordArea(area);
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
}

void NavigationUiController::presentThread(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }

    ensureThreadButton(area);
    if (area->property("threadTabbed").toBool()) {
        const int index = tabIndexForThread(area);
        if (index >= 0) {
            {
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
            }
            activateTab(index);
        }
        return;
    }
    if (area->property("threadDetached").toBool()) {
        area->show();
        area->raise();
        area->activateWindow();
        recordArea(area);
        return;
    }

    attachThread(area);
}

bool NavigationUiController::eventFilter(QObject* watched, QEvent* event)
{
    if (event && event->type() == QEvent::PaletteChange
        && watched && watched->objectName() == QStringLiteral("tabPinButton")) {
        if (auto* button = qobject_cast<QToolButton*>(watched)) {
            if (auto* area = qobject_cast<ChatArea*>(button->parentWidget())) {
                updateTabPinButton(area);
            }
        }
    }

    if (event && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        auto* area = qobject_cast<ChatArea*>(watched);
        if (area && area->property("sessionTracked").toBool()
            && area->property("threadDetached").toBool()) scheduleSessionSave();
    }
    if (watched == navigationTabs && event
        && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = navigationTabs->tabAt(mouse->pos());
            if (index >= 0) {
                // Thread tabs may need reparenting as they close. Let Qt finish
                // dispatching the tab-bar mouse gesture before changing parents.
                QTimer::singleShot(0, this, [this, index] { closeTab(index); });
                event->accept();
                return true;
            }
        }
    }

    // Only a mouse release needs the sidebar viewport. Other events include
    // child destruction while the main window is tearing down that sidebar.
    if (event && event->type() == QEvent::MouseButtonRelease && channelTree
        && (watched == channelTree || watched == channelTree->viewport())) {
        QTimer::singleShot(0, this, [this] {
            if (channelTree) {
                recordArea(channelTree->getCurrentPage());
            }
        });
    }

    if (event && event->type() == QEvent::Show) {
        auto* area = qobject_cast<ChatArea*>(watched);
        if (area && area->isThread
            && !area->property("threadDetached").toBool()
            && !area->property("threadTabbed").toBool()
            && (!threadStack || threadStack->indexOf(area) < 0)) {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (guard
                    && !guard->property("threadDetached").toBool()
                    && !guard->property("threadTabbed").toBool()) {
                    attachThread(guard);
                }
            });
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost

namespace {

class NavigationUiBootstrap final : public QObject
{
public:
    explicit NavigationUiBootstrap(QObject* parent)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event && event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<Mattermost::MainWindow*>(watched)) {
                if (!window->property("navigationUiSetupScheduled").toBool()) {
                    window->setProperty("navigationUiSetupScheduled", true);
                    QPointer<Mattermost::MainWindow> guard(window);
                    QTimer::singleShot(0, window, [guard] {
                        if (guard) {
                            Mattermost::NavigationUiController::instance(*guard);
                        }
                    });
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void installNavigationUiBootstrap()
{
    if (!qApp) {
        return;
    }
    auto* bootstrap = new NavigationUiBootstrap(qApp);
    qApp->installEventFilter(bootstrap);
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installNavigationUiBootstrap)