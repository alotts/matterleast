#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "NavigationTabsModel.h"

class QEvent;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTimer;
class QWidget;

namespace Mattermost {

class Backend;
class ChatArea;
class ChannelTree;
class MainWindow;
class PostCollectionView;

/**
 * Main-window presentation state that is orthogonal to Mattermost transport:
 * browser-like semantic navigation history and the docked/right-hand thread
 * surface. Kept outside MainWindow so every navigation producer uses the same
 * presentation policy.
 */
class NavigationUiController final : public QObject
{
    Q_OBJECT
public:
    struct Location {
        enum class Kind {
            Chat,
            Saved,
            Drafts,
            RecentMentions,
            Search,
        };

        Location() = default;
        Location(const QString& channel,
                 const QString& root = QString(),
                 const QString& post = QString())
            : channelId(channel)
            , rootId(root)
            , postId(post)
        {
        }

        Kind kind = Kind::Chat;
        QString channelId;
        QString rootId;
        QString postId;

        bool isChat() const { return kind == Kind::Chat; }
        bool isValid() const { return !isChat() || !channelId.isEmpty(); }
        bool sameDestination(const Location& other) const
        {
            if (kind != other.kind) {
                return false;
            }
            if (!isChat()) {
                return true;
            }
            return channelId == other.channelId && rootId == other.rootId;
        }
    };

    static NavigationUiController& instance(MainWindow& window);

    explicit NavigationUiController(MainWindow& window);
    ~NavigationUiController() override;

    ChatArea* findThread(const QString& channelId, const QString& rootId) const;
    void presentChannel(ChatArea* area);
    void presentThread(ChatArea* area);
    void openInTab(const QString& channelId,
                   const QString& rootId,
                   const QString& postId,
                   const QStringList& contextPostIds,
                   bool reachedOldest,
                   bool reachedNewest);
    bool activateExistingTab(const QString& channelId,
                             const QString& rootId = QString(),
                             bool restoreBookmark = true);
    void saveSession();
    void restoreSession();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupMainWindow();
    QString sessionSettingsKey() const;
    void scheduleSessionSave();
    void trackSessionArea(ChatArea* area);
    void restoreSessionBookmark(ChatArea* area, const QString& postId);
    void setupSidebarHeader();
    void setupThreadPane();
    void setupTabShortcuts();
    void updateIdentityTooltip();
    void updateHistoryButtons();

    ChatArea* centralTabArea() const;
    void closeActiveTab();
    void activatePreviousTab();
    void activateNextTab();

    Location captureLocation(ChatArea* area) const;
    Location captureCollection(PostCollectionView* page) const;
    void recordArea(ChatArea* area);
    void recordCollection(PostCollectionView* page);
    void recordLocation(const Location& location, ChatArea* area);
    void navigateTo(const Location& location);
    PostCollectionView* findCollection(Location::Kind kind) const;

    NavigationTabsModel::Entry tabEntry(const Location& location) const;
    Location tabLocation(const NavigationTabsModel::Entry& entry) const;
    QString tabTitle(const Location& location) const;
    int appendNavigationTab(const Location& location);
    void ensureInitialTab();
    void refreshTabBarVisibility();
    void setNavigationTabTitle(int index, const QString& title);
    void refreshTabUnreadVisual(const QString& channelId);
    void refreshAllTabUnreadVisuals();
    void updateTab(int index, const Location& location);
    void saveActiveTabLocation();
    void activateTab(int index, bool restoreBookmark = true);
    void closeTab(int index);
    void removeTab(int index, bool closeThread);
    int tabIndexForThread(ChatArea* area) const;
    int firstChannelTab() const;
    int releaseThreadFromTabSurface(ChatArea* area);
    void tabifyThread(ChatArea* area, int tabIndex);
    void goBack();
    void goForward();

    void ensureTabPinAction(ChatArea* area);
    void updateTabPinAction(ChatArea* area);
    void ensureThreadButton(ChatArea* area);
    void attachThread(ChatArea* area);
    void detachThread(ChatArea* area);
    void updateThreadButton(ChatArea* area);
    void syncSplitterEdgeGutters();

    Backend* backend() const;

    MainWindow& window;
    ChannelTree* channelTree = nullptr;
    QStackedWidget* mainStack = nullptr;
    QSplitter* sidebarSplitter = nullptr;
    QPointer<QSplitter> contentSplitter;
    QWidget* contentHost = nullptr;
    QStackedWidget* navigationSurfaceStack = nullptr;
    QTabBar* navigationTabs = nullptr;
    QStackedWidget* threadStack = nullptr;
    QToolButton* backButton = nullptr;
    QToolButton* forwardButton = nullptr;

    QPointer<ChatArea> activeArea;
    Location currentLocation;
    QVector<Location> backStack;
    QVector<Location> forwardStack;
    NavigationTabsModel tabModel;
    int activeTabIndex = -1;
    bool switchingTabs = false;
    bool replayingHistory = false;
    bool threadSplitterStateRestored = false;
    QTimer* sessionSaveTimer = nullptr;
    bool sessionRestored = false;
    bool restoringSession = false;
};

} // namespace Mattermost