/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MatterLeast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MatterLeast. if not, see https://www.gnu.org/licenses/.
 */

#include "ChatArea.h"

#include <QApplication>
#include <QClipboard>
#include <QIcon>
#include <QMenu>
#include <QMargins>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include "AbstractPostSource.h"
#include "ChannelPostSource.h"
#include "ChatLogWidget.h"
#include "FilteredPostSource.h"
#include "OutboxPostSource.h"
#include "PostFilterPolicy.h"
#include "ThreadPostSource.h"
#include "backend/Backend.h"
#include "backend/PendingPostService.h"
#include "backend/NetworkRequest.h"
#include "backend/ThreadFollowService.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemWidget.h"
#include "channel-tree-dialogs/ViewChannelMembersListDialog.h"
#include "log.h"
#include "navigation/AppNavigationService.h"
#include "post-collection/PostCollectionView.h"
#include "ui/IconUtils.h"
#include "ui/ThemeIconWidgets.h"
#include "ui_ChatArea.h"

namespace Mattermost {

namespace {

constexpr int HeaderActionIconExtent = 16;

QString channelWebUrl(Backend& backend, const BackendChannel& channel)
{
    QString teamName;
    if (channel.team) {
        teamName = channel.team->name;
    } else if (const BackendTeam* team = backend.getStorage().getTeamById(
                   backend.getCurrentTeamContextId())) {
        // Mattermost's own web client resolves DM/GM URLs in the current team
        // context even though those conversations do not intrinsically belong
        // to a team.
        teamName = team->name;
    }

    if (teamName.isEmpty() || channel.name.isEmpty()) {
        return QString();
    }

    QString base = NetworkRequest::host();
    if (base.isEmpty()) {
        return QString();
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base += QLatin1Char('/');
    }
    return base + teamName + QStringLiteral("/channels/") + channel.name;
}

} // namespace

ChatArea::ChatArea(Backend& backend,
                   BackendChannel& channel,
                   ChannelItem* treeItem,
                   QWidget* parent,
                   bool initialize)
    : QWidget(parent)
    , parentArea(nullptr)
    , ui(new Ui::ChatArea)
    , backend(backend)
    , channel(channel)
    , treeItem(treeItem)
    , unreadMessagesCount(0)
    , isThread(false)
    , initialized(false)
{
    setAcceptDrops(true);
    ui->setupUi(this);
    {
        const QMargins outerMargins = ui->verticalLayout->contentsMargins();
        defaultOuterLeftMargin = outerMargins.left();
        defaultOuterRightMargin = outerMargins.right();
    }
    // The main channel surface always sits directly to the right of the
    // sidebar splitter, so its outer left gutter should not decorate that
    // one-pixel divider.
    setSplitterEdgeGutters(true, false);
    ui->listWidget->configure(backend, *this);
    setupHeaderUi();
    setupPinnedPostsView();
    setupComposerUi();

    ui->outgoingPostCreator->init(backend, channel, *ui->listWidget,
                                  ui->footerLayout, *ui->composerStatusLabel,
                                  *ui->attachButton, *ui->addEmojiButton,
                                  *ui->sendButton);
    ui->outgoingPostCreator->restorePersistentDraft();

    ui->titleLabel->setText(channel.display_name);
    ui->statusLabel->setText(channel.getChannelDescription());

    const BackendUser* user = backend.getStorage().getUserById(channel.name);
    if (user) {
        connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
            setUserAvatar(*user);
        });
        connect(user, &BackendUser::onStatusChanged, this, [this, user] {
            ui->statusLabel->setText(user->status);
        });

        if (!user->avatar.isNull()) {
            setUserAvatar(*user);
        } else {
            backend.retrieveUserAvatar(user->id);
        }
        if (ui->statusLabel->text().isEmpty()) {
            ui->statusLabel->setText(user->status);
        }
    } else {
        ui->userAvatar->clear();
        ui->userAvatar->hide();
        ui->usersButton->hide();
        requestChannelMemberCount();
    }

    // Lazy ChatAreas may be created after the application's eager startup work.
    // Pinned posts are independent from the virtualized timeline and can be
    // requested immediately.
    backend.retrieveChannelPinnedPosts(channel);

    if (initialize) {
        init();
    }
}

ChatArea::ChatArea(Backend& backend,
                   BackendChannel& channel,
                   QString rootId,
                   ChatArea* parentArea)
    : QWidget(nullptr)
    , parentArea(parentArea)
    , parentPostId(rootId)
    , ui(new Ui::ChatArea)
    , backend(backend)
    , channel(channel)
    , treeItem(nullptr)
    , unreadMessagesCount(0)
    , isThread(true)
    , initialized(false)
    , root_id(std::move(rootId))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    ui->setupUi(this);
    {
        const QMargins outerMargins = ui->verticalLayout->contentsMargins();
        defaultOuterLeftMargin = outerMargins.left();
        defaultOuterRightMargin = outerMargins.right();
    }
    ui->listWidget->configure(backend, *this);
    setupHeaderUi();
    setupComposerUi();

    ui->outgoingPostCreator->init(backend, channel, *ui->listWidget,
                                  ui->footerLayout, *ui->composerStatusLabel,
                                  *ui->attachButton, *ui->addEmojiButton,
                                  *ui->sendButton);
    ui->outgoingPostCreator->setRootId(root_id);
    ui->outgoingPostCreator->restorePersistentDraft();

    // Match the web client: a thread keeps the parent chat name in its header,
    // but the name links back to the root message that anchors this thread in
    // the parent timeline.
    ui->titleLabel->setTextFormat(Qt::RichText);
    ui->titleLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    ui->titleLabel->setText(QStringLiteral("<a href=\"channel\">%1</a>")
                                .arg(channel.display_name.toHtmlEscaped()));
    ui->titleLabel->installEventFilter(this);
    connect(ui->titleLabel, &QLabel::linkActivated, this,
            [this](const QString&) {
        AppNavigationService::instance(this->backend).openPost(this->root_id);
    });

    // The parent channel topic/header is useful on the channel itself, but
    // repeating it in every thread wastes vertical space and is not thread state.
    ui->statusLabel->hide();
    ui->userAvatar->hide();

    init();

    // Thread follow state is a per-user Mattermost resource. Query it lazily
    // when the thread window opens and use the official PUT/DELETE endpoint.
    QString teamId;
    if (channel.team) {
        teamId = channel.team->id;
    }
    if (teamId.isEmpty()) {
        teamId = this->backend.getCurrentTeamContextId();
    }

    if (!root_id.isEmpty() && !teamId.isEmpty()) {
        const QString threadId = root_id;
        threadFollowButton = new ThemeIconButton(this);
        threadFollowButton->setObjectName(QStringLiteral("threadFollowButton"));
        threadFollowButton->setFixedSize(HeaderActionIconExtent + 8,
                                         HeaderActionIconExtent + 8);
        threadFollowButton->setIconSize(QSize(HeaderActionIconExtent,
                                              HeaderActionIconExtent));
        threadFollowButton->setProperty(ThemeIconResourceProperty,
                                        QStringLiteral(":/icons/bell"));
        threadFollowButton->setEnabled(false);
        threadFollowButton->setProperty("following", false);
        threadFollowButton->setToolTip(tr("Follow thread"));
        threadFollowButton->setAccessibleName(tr("Follow thread"));
        ui->propertieslLayout->addWidget(threadFollowButton, 0, Qt::AlignVCenter);
        refreshHeaderActionIcons();

        QPointer<ChatArea> areaGuard(this);
        QPointer<ThemeIconButton> buttonGuard(threadFollowButton);
        auto setButtonState = [areaGuard, buttonGuard](bool following) {
            if (!areaGuard || !buttonGuard) {
                return;
            }
            buttonGuard->setProperty("following", following);
            const QString label = following ? areaGuard->tr("Unfollow thread")
                                            : areaGuard->tr("Follow thread");
            buttonGuard->setToolTip(label);
            buttonGuard->setAccessibleName(label);
            buttonGuard->setEnabled(true);
            areaGuard->refreshHeaderActionIcons();
        };

        auto& followService = ThreadFollowService::instance(backend);
        connect(&followService, &ThreadFollowService::followingChanged,
                threadFollowButton,
                [teamId, threadId, setButtonState](const QString& changedTeamId,
                                                   const QString& changedThreadId,
                                                   bool following) {
            if (changedTeamId == teamId && changedThreadId == threadId) {
                setButtonState(following);
            }
        });

        followService.queryFollowing(teamId, threadId, setButtonState);
        connect(threadFollowButton, &QPushButton::clicked, this,
                [this, teamId, threadId, buttonGuard, setButtonState] {
            if (!buttonGuard) {
                return;
            }
            const bool following = buttonGuard->property("following").toBool();
            const bool desired = !following;
            buttonGuard->setEnabled(false);
            ThreadFollowService::instance(this->backend).setFollowing(
                teamId, threadId, desired,
                [buttonGuard, following, desired, setButtonState](bool success) {
                    if (!buttonGuard) {
                        return;
                    }
                    setButtonState(success ? desired : following);
                });
        });
    }

    ui->pinnedPostsButton->hide();
    ui->usersButton->hide();
    ui->loadOldPosts->hide();
}

bool ChatArea::eventFilter(QObject* watched, QEvent* event)
{
    if (isThread && ui && watched == ui->titleLabel && event) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress
            || type == QEvent::MouseButtonRelease
            || type == QEvent::MouseButtonDblClick) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::MiddleButton) {
                // The thread title is one semantic link: the parent chat name
                // points to this thread's root post in the channel timeline.
                // Consume the complete middle-click gesture so QLabel's rich
                // text interaction cannot partially handle it as a left-link
                // activation or leave selection/focus state behind.
                if (type == QEvent::MouseButtonRelease && !root_id.isEmpty()) {
                    AppNavigationService::instance(backend).openPostInTab(root_id);
                }
                event->accept();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

ChatArea::~ChatArea()
{
    deinit();
    if (isThread && parentArea) {
        parentArea->threadsAreas.remove(this);
    }
    delete ui;
}

void ChatArea::setupHeaderUi()
{
    auto configureCountButton = [](QToolButton& button) {
        button.setAutoRaise(true);
        button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button.setIconSize(QSize(HeaderActionIconExtent,
                                 HeaderActionIconExtent));
        button.setCursor(Qt::PointingHandCursor);
    };

    configureCountButton(*ui->usersButton);
    configureCountButton(*ui->pinnedPostsButton);
    ui->pinnedPostsButton->setCheckable(true);

    ui->titleLabel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->titleLabel, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(this);
        QAction* copyLinkAction = menu.addAction(tr("Copy link to chat"));
        const QString link = channelWebUrl(this->backend, this->channel);
        copyLinkAction->setEnabled(!link.isEmpty());
        if (menu.exec(ui->titleLabel->mapToGlobal(pos)) == copyLinkAction
            && !link.isEmpty()) {
            QApplication::clipboard()->setText(link);
        }
    });

    ui->loadOldPosts->setAutoRaise(true);
    ui->propertieslLayout->setSpacing(2);
    refreshHeaderActionIcons();
}

void ChatArea::setupPinnedPostsView()
{
    if (isThread || contentStack || !ui || !ui->listWidget) {
        return;
    }

    const int listIndex = ui->verticalLayout->indexOf(ui->listWidget);
    if (listIndex < 0) {
        return;
    }

    contentStack = new QStackedWidget(this);
    ui->verticalLayout->removeWidget(ui->listWidget);
    contentStack->addWidget(ui->listWidget);

    pinnedPostsView = new PostCollectionView(
        backend, PostCollectionView::Mode::Pinned, contentStack);
    contentStack->addWidget(pinnedPostsView);
    contentStack->setCurrentWidget(ui->listWidget);
    ui->verticalLayout->insertWidget(listIndex, contentStack, 1);

    connect(pinnedPostsView, &PostCollectionView::postActivated,
            this, [this](const QString& postId) {
        showPinnedPosts(false);
        AppNavigationService::instance(backend).openPost(postId);
    });
    pinnedPostsView->activatePinned(channel);
}

void ChatArea::showPinnedPosts(bool show)
{
    if (!contentStack || !pinnedPostsView || !ui || !ui->listWidget) {
        return;
    }

    if (show && channel.pinnedPosts.empty()) {
        show = false;
    }
    ui->pinnedPostsButton->setChecked(show);
    contentStack->setCurrentWidget(show
        ? static_cast<QWidget*>(pinnedPostsView)
        : static_cast<QWidget*>(ui->listWidget));
}

void ChatArea::refreshHeaderActionIcons()
{
    if (!ui) {
        return;
    }

    if (ui->usersButton) {
        ui->usersButton->setIcon(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/members")));
    }
    if (ui->pinnedPostsButton) {
        ui->pinnedPostsButton->setIcon(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/pin")));
    }
    if (threadFollowButton) {
        const bool following = threadFollowButton->property("following").toBool();
        threadFollowButton->setProperty(
            ThemeIconResourceProperty,
            following ? QStringLiteral(":/icons/bell-filled")
                      : QStringLiteral(":/icons/bell"));
        threadFollowButton->update();
    }
}

void ChatArea::requestChannelMemberCount()
{
    if (isThread || channel.type == BackendChannel::directChannel) {
        return;
    }

    QPointer<ChatArea> guard(this);
    UserProfileService::instance(backend).queryChannelMemberCount(
        channel, [guard](int count) {
            if (!guard || !guard->ui) {
                return;
            }
            guard->channelMemberCount = std::max(0, count);
            guard->updateUsersButton();
            guard->ui->usersButton->show();
        });
}

void ChatArea::updateUsersButton()
{
    if (!ui || !ui->usersButton || channelMemberCount < 0) {
        return;
    }

    const qulonglong memberCount = static_cast<qulonglong>(channelMemberCount);
    ui->usersButton->setText(QString::number(memberCount));
    const QString tooltip = memberCount == 1
        ? tr("1 member")
        : tr("%1 members").arg(memberCount);
    ui->usersButton->setToolTip(tooltip);
    ui->usersButton->setAccessibleName(tooltip);
}

void ChatArea::setupPostSource()
{
    if (!postSource) {
        AbstractPostSource* presentedServerSource = nullptr;
        PendingPostService::VisibilityPredicate visibilityPredicate =
            [](const BackendPost&) { return true; };

        if (isThread) {
            presentedServerSource =
                new ThreadPostSource(backend, channel, root_id, this);
        } else {
            // ChannelPostSource keeps the unfiltered server coordinate system;
            // only its view-facing model projection applies presentation policy.
            Backend* backendPtr = &backend;
            visibilityPredicate = [backendPtr](const BackendPost& post) {
                const BackendUser* loginUser =
                    backendPtr->getStorage().loginUser;
                return shouldShowMainTimelinePost(
                    post.type,
                    post.props.toObject(),
                    loginUser ? loginUser->username : QString());
            };
            auto* channelSource = new ChannelPostSource(backend, channel, this);
            presentedServerSource = new FilteredPostSource(
                *channelSource, visibilityPredicate, this);
        }

        auto& outbox = PendingPostService::instance(backend);
        const QString timelineRoot = isThread ? root_id : QString();
        // The projection owns "visible conversation" semantics. Delivery uses
        // the same policy only for its bounded auto-retry cutoff.
        outbox.setVisibilityPredicate(
            channel.id, timelineRoot, visibilityPredicate);

        // Optimistic/failed local messages are an additive presentation tail.
        // They never enter authoritative channel/thread topology, so paging,
        // reply counts, navigation and read state stay server-backed.
        postSource = new OutboxPostSource(
            *presentedServerSource,
            outbox,
            channel.id,
            timelineRoot,
            this);
    }
    ui->listWidget->setSource(postSource);

    // setSource()/setItemCount() queues LongList's first synchronize() for the
    // next event-loop turn. Establish this chat's semantic starting viewport
    // immediately, otherwise that queued sync observes the default scrollbar
    // value 0 and can start loading the oldest history before the later
    // newest/bookmark timer gets a chance to run.
    if (!isThread && !storedViewportPostId.isEmpty()) {
        if (!ui->listWidget->restoreViewportBookmark(storedViewportPostId)) {
            ui->listWidget->scrollToEnd();
        }
    } else {
        ui->listWidget->scrollToEnd();
    }
}

void ChatArea::scheduleNewestPosition()
{
    const std::uint64_t generation = viewportNavigationGeneration;
    QPointer<ChatArea> guard(this);
    QTimer::singleShot(0, this, [guard, generation] {
        if (!guard || !guard->ui || !guard->ui->listWidget
            || generation != guard->viewportNavigationGeneration) {
            return;
        }
        guard->ui->listWidget->scrollToEnd();
    });
}

void ChatArea::scheduleStoredPosition()
{
    if (storedViewportPostId.isEmpty()) {
        scheduleNewestPosition();
        return;
    }

    const std::uint64_t generation = viewportNavigationGeneration;
    const QString postId = storedViewportPostId;
    QPointer<ChatArea> guard(this);
    QTimer::singleShot(0, this, [guard, generation, postId] {
        if (!guard || !guard->ui || !guard->ui->listWidget
            || generation != guard->viewportNavigationGeneration) {
            return;
        }

        if (!guard->ui->listWidget->restoreViewportBookmark(postId)) {
            guard->scheduleNewestPosition();
        }
    });
}

void ChatArea::init()
{
    if (initialized) {
        return;
    }

    if (!isThread && channel.member_count >= 0) {
        channelMemberCount = channel.member_count;
    }

    setupPostSource();

    // The semantic source object remains alive while a channel page is inactive,
    // but ChatLogWidget detaches from it so all concrete PostWidgets and their
    // repository residency leases can be released. Rebuild only orchestration
    // connections and the view attachment on activation.
    if (!isThread) {
        signalConnections.push_back(connect(&channel, &BackendChannel::onViewed,
                                            this, [this] {
            LOG_DEBUG("Channel viewed: " << channel.display_name);
            setUnreadMessagesCount(0);
        }));

        signalConnections.push_back(connect(&channel, &BackendChannel::onUpdated,
                                            this, [this] {
            ui->titleLabel->setText(channel.display_name);
            ui->statusLabel->setText(channel.getChannelDescription());
            if (treeItem) {
                treeItem->setLabel(channel.display_name);
            }
        }));

        signalConnections.push_back(connect(&channel,
                                            &BackendChannel::onPinnedPostsReceived,
                                            this, [this] {
            updatePinnedPostsButton();
        }));

        signalConnections.push_back(connect(&channel, &BackendChannel::onUserAdded,
                                            this, [this](const BackendUser&) {
            if (channel.member_count >= 0) {
                channelMemberCount = channel.member_count;
                updateUsersButton();
            } else {
                requestChannelMemberCount();
            }
        }));
        signalConnections.push_back(connect(&channel, &BackendChannel::onUserRemoved,
                                            this, [this](const BackendUser&) {
            if (channel.member_count >= 0) {
                channelMemberCount = channel.member_count;
                updateUsersButton();
            } else {
                requestChannelMemberCount();
            }
        }));

        signalConnections.push_back(connect(ui->usersButton, &QToolButton::clicked,
                                            this, [this] {
            auto* dialog = new ViewChannelMembersListDialog(backend, channel, this);
            dialog->show();
        }));

        signalConnections.push_back(connect(ui->pinnedPostsButton,
                                            &QToolButton::clicked,
                                            this, [this](bool checked) {
            showPinnedPosts(checked);
        }));
    }

    auto& composer = *ui->outgoingPostCreator;
    signalConnections.push_back(connect(&channel, &BackendChannel::onNewPost,
                                        &composer,
                                        &OutgoingPostCreator::onPostReceived));
    signalConnections.push_back(connect(&channel, &BackendChannel::onPostEdited,
                                        &composer,
                                        &OutgoingPostCreator::onPostReceived));
    signalConnections.push_back(connect(&channel, &BackendChannel::onUserTyping,
                                        this, &ChatArea::handleUserTyping));

    signalConnections.push_back(connect(ui->listWidget,
                                        &ChatLogWidget::postEditInitiated,
                                        &composer,
                                        &OutgoingPostCreator::postEditInitiated));
    signalConnections.push_back(connect(&composer,
                                        &OutgoingPostCreator::postEditFinished,
                                        ui->listWidget,
                                        &ChatLogWidget::postEditFinished));

    ui->loadOldPosts->hide();
    if (!isThread) {
        updatePinnedPostsButton();
        if (channel.type == BackendChannel::directChannel || channelMemberCount < 0) {
            ui->usersButton->hide();
        } else {
            updateUsersButton();
            ui->usersButton->show();
        }
    }

    initialized = true;
    if (!isThread && !storedViewportPostId.isEmpty()) {
        scheduleStoredPosition();
    } else {
        scheduleNewestPosition();
    }
}

void ChatArea::deinit()
{
    if (!initialized) {
        return;
    }

    for (const QMetaObject::Connection& connection : signalConnections) {
        disconnect(connection);
    }
    signalConnections.clear();
    initialized = false;
}

void ChatArea::setUserAvatar(const BackendUser& user)
{
    ui->userAvatar->setPixmap(user.avatar);
    if (channel.type == BackendChannel::directChannel && !isThread && treeItem) {
        treeItem->setIcon(QIcon(user.avatar));
    }
}

Ui::ChatArea* ChatArea::getUi()
{
    return ui;
}

Backend& ChatArea::getBackend()
{
    return backend;
}

BackendChannel& ChatArea::getChannel()
{
    return channel;
}

void ChatArea::setSplitterEdgeGutters(bool suppressLeft, bool suppressRight)
{
    if (!ui || !ui->verticalLayout) {
        return;
    }

    QMargins margins = ui->verticalLayout->contentsMargins();
    margins.setLeft(suppressLeft ? 0 : defaultOuterLeftMargin);
    margins.setRight(suppressRight ? 0 : defaultOuterRightMargin);
    ui->verticalLayout->setContentsMargins(margins);
}

void ChatArea::updatePinnedPostsButton()
{
    if (isThread) {
        ui->pinnedPostsButton->hide();
        return;
    }

    if (pinnedPostsView) {
        pinnedPostsView->activatePinned(channel);
    }

    if (channel.pinnedPosts.empty()) {
        showPinnedPosts(false);
        ui->pinnedPostsButton->hide();
        return;
    }

    const qulonglong pinnedPostCount =
        static_cast<qulonglong>(channel.pinnedPosts.size());
    ui->pinnedPostsButton->setText(QString::number(pinnedPostCount));
    const QString tooltip = pinnedPostCount == 1
        ? tr("1 pinned post")
        : tr("%1 pinned posts").arg(pinnedPostCount);
    ui->pinnedPostsButton->setToolTip(tooltip);
    ui->pinnedPostsButton->setAccessibleName(tooltip);
    ui->pinnedPostsButton->show();
}

void ChatArea::handleUserTyping(const BackendUser& user)
{
    LOG_DEBUG("Channel " << channel.display_name << ": "
                          << user.getDisplayName() << " is typing");
}

void ChatArea::onActivate()
{
    backend.setCurrentChannel(channel);

    // Pinned is an inspection mode, not durable channel state. Entering a chat
    // always returns to the canonical conversation surface.
    if (!isThread) {
        showPinnedPosts(false);
    }

    init();
}

void ChatArea::onDeactivate()
{
    if (ui && ui->outgoingPostCreator) {
        ui->outgoingPostCreator->flushPersistentDraft();
    }

    if (!isThread && ui && ui->listWidget) {
        showPinnedPosts(false);

        // The live edge is a viewport state of its own. Reducing it to the
        // semantic center of the currently visible rows makes reopening a
        // channel restore that center with Alignment::Center and pulls the
        // viewport away from the newest post. An empty bookmark means
        // "reopen at newest", so also clear any older stored bookmark here.
        storedViewportPostId.clear();
        if (!ui->listWidget->isAtEnd()) {
            QString postId;
            if (ui->listWidget->captureViewportBookmark(postId)) {
                storedViewportPostId = postId;
            }
        }
    }

    deinit();

    if (!isThread && ui && ui->listWidget) {
        // Keep ChannelPostSource alive as the semantic timeline authority while
        // dropping the concrete view. setItemCount(0) inside setSource(nullptr)
        // destroys every materialized PostWidget and releases its residency lease.
        ui->listWidget->setSource(nullptr);
    }
}

void ChatArea::restoreDraftAndFocus(const QString& draftKey)
{
    goToNewest();
    if (ui && ui->outgoingPostCreator) {
        ui->outgoingPostCreator->restorePersistentDraft(draftKey);
    }
    focusComposer();
}

void ChatArea::onMainWindowActivate()
{
    // Merely restoring/focusing the application is not a reading gesture.
}

void ChatArea::onMove(QPoint)
{
    // Pinned messages are embedded in the ChatArea; no floating child follows
    // the main-window position anymore.
}

void ChatArea::moveOnListTop()
{
    if (!treeItem || !treeItem->parent() || !treeItem->treeWidget()) {
        return;
    }

    QTreeWidgetItem* parent = treeItem->parent();
    QTreeWidget* tree = treeItem->treeWidget();
    if (parent->indexOfChild(treeItem) == 0) {
        return;
    }

    const bool isCurrent = tree->currentItem() == treeItem;
    auto* thisItemWidget = static_cast<ChannelItemWidget*>(
        tree->itemWidget(treeItem, 0));
    if (!thisItemWidget) {
        return;
    }

    auto* newItemWidget = new ChannelItemWidget(thisItemWidget->parentWidget());
    newItemWidget->setLabel(channel.display_name);
    if (!thisItemWidget->getPixmap().isNull()) {
        newItemWidget->setIcon(QIcon(thisItemWidget->getPixmap()));
    }

    tree->blockSignals(true);
    QTreeWidgetItem* child = parent->takeChild(parent->indexOfChild(treeItem));
    parent->insertChild(0, child);
    tree->blockSignals(false);

    if (child != treeItem) {
        return;
    }

    tree->setItemWidget(child, 0, newItemWidget);
    treeItem->setWidget(newItemWidget);
    if (isCurrent) {
        tree->setCurrentItem(child);
    }
}

void ChatArea::setUnreadMessagesCount(uint32_t count)
{
    unreadMessagesCount = count;
    if (!treeItem) {
        return;
    }
    treeItem->setText(1, count == 0 ? QString() : QString::number(count));
}

void ChatArea::resizeEvent(QResizeEvent* event)
{
    // ChatLogWidget/LongListWidget owns its own durable viewport anchor and
    // remeasures visible rows from its resizeEvent(). ChatArea must not mutate
    // the scrollbar as a side effect of outer layout changes.
    QWidget::resizeEvent(event);
}

void ChatArea::dragEnterEvent(QDragEnterEvent* event)
{
    ui->outgoingPostCreator->onDragEnterEvent(event);
}

void ChatArea::dragMoveEvent(QDragMoveEvent* event)
{
    ui->outgoingPostCreator->onDragMoveEvent(event);
}

void ChatArea::dropEvent(QDropEvent* event)
{
    ui->outgoingPostCreator->onDropEvent(event);
}

} /* namespace Mattermost */
