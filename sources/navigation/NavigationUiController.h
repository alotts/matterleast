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
class QWidget;

namespace Mattermost {

class Backend;
class ChatArea;
class ChannelTree;
class MainWindow;

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
    // Saved/Drafts share the ordinary Channel kind. The discriminator below
    // (never a new enum member) tells an ordinary channel apart from a virtual
    // collection destination within that same kind. The shared Kind enum lives
    // in NavigationTabsModel so Location and Entry use the same type.
    struct Location {
        QString channelId;
        QString rootId;
        QString postId;

        // Empty for an ordinary channel; "virtual:saved" / "virtual:drafts"
        // for a Saved/Drafts collection destination.
        QString destination;

        NavigationTabsModel::Kind kind = NavigationTabsModel::Kind::Channel;

        bool isValid() const
        {
            return kind == NavigationTabsModel::Kind::Channel
                && (!channelId.isEmpty() || !destination.isEmpty());
        }
        bool sameDestination(const Location& other) const
        {
            return kind == other.kind
                && channelId == other.channelId
                && rootId == other.rootId
                && destination == other.destination;
        }
    };

    static NavigationUiController& instance(MainWindow& window);

    explicit NavigationUiController(MainWindow& window);
    ~NavigationUiController() override;

    ChatArea* findThread(const QString& channelId, const QString& rootId) const;
    void presentChannel(ChatArea* area);
    void presentThread(ChatArea* area);
    void presentCollection(const QString& destination);
    void openInTab(const QString& channelId,
                   const QString& rootId,
                   const QString& postId,
                   const QStringList& contextPostIds,
                   bool reachedOldest,
                   bool reachedNewest);
    bool activateExistingTab(const QString& channelId,
                             const QString& rootId = QString(),
                             bool restoreBookmark = true);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupMainWindow();
    void setupSidebarHeader();
    void setupThreadPane();
    void updateIdentityTooltip();
    void updateHistoryButtons();

    Location captureLocation(ChatArea* area) const;
    void recordArea(ChatArea* area);
    void navigateTo(const Location& location);

    NavigationTabsModel::Entry tabEntry(const Location& location) const;
    Location tabLocation(const NavigationTabsModel::Entry& entry) const;
    QString tabTitle(const Location& location) const;
    int appendNavigationTab(const Location& location);
    void ensureInitialTab();
    void updateCollectionTab(const Location& location);
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
};

} // namespace Mattermost
