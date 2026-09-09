#include "ChatLogWidget.h"

#include <algorithm>

#include <QLoggingCategory>
#include <QResizeEvent>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QFrame>
#include <QClipboard>
#include <QApplication>
#include <QTimer>

#include "ChatArea.h"
#include "ThreadPostSource.h"
#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/ThreadFollowService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "post/InteractivePostWidget.h"
#include "post/PostWidget.h"
#include "post/PostSelectionPolicy.h"
#include "ui/IconUtils.h"
#include "ui/OverlayScrollBarManager.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcTimelineTrace, "mattermost.timeline.trace", QtWarningMsg)

const char* requestReasonName(LongListWidget::RequestReason reason)
{
    switch (reason) {
    case LongListWidget::RequestReason::Initial:
        return "initial";
    case LongListWidget::RequestReason::Scroll:
        return "scroll";
    case LongListWidget::RequestReason::Seek:
        return "seek";
    case LongListWidget::RequestReason::EnsureVisible:
        return "ensure-visible";
    }
    return "unknown";
}

const char* sourceName(const AbstractPostSource* source)
{
    return source ? source->metaObject()->className() : "none";
}

bool isAfter(const BackendPost& lhs, const BackendPost& rhs)
{
    if (lhs.create_at != rhs.create_at) {
        return lhs.create_at > rhs.create_at;
    }
    return lhs.id > rhs.id;
}

void acknowledgeChannelRead(Backend& backend, BackendChannel& channel)
{
    auto& sidebar = SidebarService::instance(backend);
    if (!sidebar.isChannelUnread(channel) && !sidebar.hasUnreadMention(channel.id)) {
        return;
    }

    sidebar.markChannelViewedLocally(channel);
    backend.markChannelAsViewed(channel);
}

// Mattermost emits join/leave events as system posts (e.g.
// system_join_channel, system_leave_channel, system_add_remove). These
// carry no conversational content and should not clutter the timeline, so
// they are collapsed to an invisible row instead of a full message.
// Invisible zero-height row used to occupy a logical slot whose post is hidden
// (join/leave system events) without contributing any visible geometry.
class HiddenPostRow final : public QWidget
{
public:
    using QWidget::QWidget;

    QSize sizeHint() const override { return QSize(0, 0); }
    QSize minimumSizeHint() const override { return QSize(0, 0); }
};

bool isHiddenSystemPost(const BackendPost& post)
{
    if (post.isDeleted) {
        return false;
    }
    static const QStringList joinLeavePrefixes = {
        QStringLiteral("system_join"),
        QStringLiteral("system_leave"),
        QStringLiteral("system_add_remove"),
    };
    for (const QString& prefix : joinLeavePrefixes) {
        if (post.type.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

} // namespace

ChatLogWidget::ChatLogWidget(QWidget* parent)
    : PostListWidget(parent)
{
    setDefaultItemHeight(96);

    connect(this, &LongListWidget::rangeRequested, this,
            [this](int first, int last, RequestReason reason, quint64 generation) {
        const Range visible = visibleRange();
        const Range concrete = materializedRange();
        qCDebug(lcTimelineTrace).nospace()
            << "RANGE_REQUEST list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " sourcePtr=" << static_cast<const void*>(postSource)
            << " requested=[" << first << ',' << last << ']'
            << " reason=" << requestReasonName(reason)
            << " generation=" << generation
            << " itemCount=" << itemCount()
            << " visible=[" << visible.first << ',' << visible.last << ']'
            << " materialized=[" << concrete.first << ',' << concrete.last << ']'
            << " materializedCount=" << materializedCount();

        if (!postSource) {
            finishRangeRequest(first, last);
            return;
        }
        postSource->requestRange(first, last, toSourceReason(reason), generation);
    });

    connect(this, &LongListWidget::visibleRangeChanged, this,
            [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "VISIBLE_RANGE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']'
            << " itemCount=" << itemCount();

        scheduleReadCursorUpdate();
        if (!postSource || first < 0 || first > 1 || !postSource->canRequestBeforeFirst()) {
            return;
        }
        postSource->requestBeforeFirst(AbstractPostSource::RequestReason::Scroll, 0);
    });

    connect(this, &LongListWidget::materializedRangeChanged, this,
            [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "MATERIALIZED_RANGE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']'
            << " count=" << materializedCount();
        if (_initialScrollBarPulsePending && first >= 0 && last >= first
            && OverlayScrollBarManager::pulse(*this)) {
            _initialScrollBarPulsePending = false;
        }
        scheduleNavigationFinalize();
        scheduleReadCursorUpdate();
    });

    // A direct user gesture wins even while semantic navigation is waiting for
    // the target widget to materialize. Once the real viewport lock exists,
    // LongListWidget releases it and the viewportLockReleased handler below
    // clears the same semantic state.
    connect(this, &LongListWidget::userViewportChanged, this, [this](bool) {
        scheduleReadCursorUpdate();
        if (!navigationLockPending) {
            return;
        }
        navigationPostId.clear();
        pendingHighlightPostId.clear();
        navigationLogicalIndex = -1;
        navigationLockPending = false;
        navigationRecenterPending = false;
    });

    // LongListWidget owns the lock lifetime and recognizes all real user scroll
    // gestures. ChatLogWidget only drops the corresponding semantic post ID.
    connect(this, &LongListWidget::viewportLockReleased, this, [this] {
        qCDebug(lcTimelineTrace).nospace()
            << "VIEWPORT_LOCK_RELEASED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " postId=" << navigationPostId
            << " index=" << navigationLogicalIndex;
        navigationPostId.clear();
        pendingHighlightPostId.clear();
        navigationLogicalIndex = -1;
        navigationLockPending = false;
        navigationRecenterPending = false;
        scheduleReadCursorUpdate();
    });
}

ChatLogWidget::~ChatLogWidget()
{
    for (const QMetaObject::Connection& connection : sourceConnections) {
        disconnect(connection);
    }
}

void ChatLogWidget::configure(Backend& backendInstance, ChatArea& chatAreaInstance)
{
    backend = &backendInstance;
    chatArea = &chatAreaInstance;
}

void ChatLogWidget::setSource(AbstractPostSource* sourceInstance)
{
    if (postSource == sourceInstance) {
        return;
    }

    qCDebug(lcTimelineTrace).nospace()
        << "SET_SOURCE list=" << static_cast<const void*>(this)
        << " old=" << sourceName(postSource)
        << " oldPtr=" << static_cast<const void*>(postSource)
        << " new=" << sourceName(sourceInstance)
        << " newPtr=" << static_cast<const void*>(sourceInstance)
        << " newCount=" << (sourceInstance ? sourceInstance->itemCount() : 0);

    clearNavigationLock();
    manualUnreadGate_.clear();
    manualUnreadHighWaterPostId_.clear();
    manualUnreadHighWaterCreateAt_ = 0;
    manualUnreadExitedViewport_ = false;
    for (const QMetaObject::Connection& connection : sourceConnections) {
        disconnect(connection);
    }
    sourceConnections.clear();
    postSource = sourceInstance;

    setItemCount(postSource ? postSource->itemCount() : 0);
    if (!postSource) {
        return;
    }

    reconnectSource();
    for (int index = 0; index < postSource->itemCount(); ++index) {
        if (postSource->isAvailable(index)) {
            setRangeAvailable(index, index, true);
        }
    }
    scheduleReadCursorUpdate();
}

PostWidget* ChatLogWidget::findPost(const QString& postId) const
{
    if (!postSource || postId.isEmpty()) {
        return nullptr;
    }
    const int index = postSource->indexOfPost(postId);
    return index >= 0 ? qobject_cast<PostWidget*>(itemWidget(index)) : nullptr;
}

bool ChatLogWidget::captureViewportBookmark(QString& postId) const
{
    postId.clear();
    if (!postSource || itemCount() <= 0 || viewport()->height() <= 0) {
        return false;
    }

    // Preserve a semantic identity rather than a logical index. Choosing the
    // message under the viewport centre gives a stable visual neighborhood when
    // the view is rebuilt later, even if posts were inserted while inactive.
    const int centerIndex = indexAtViewportPosition(viewport()->height() / 2);
    if (centerIndex >= 0) {
        if (BackendPost* post = postSource->postAt(centerIndex)) {
            if (!post->id.isEmpty()) {
                postId = post->id;
                return true;
            }
        }
    }

    // A sparse estimated window can have no resident body exactly at centre.
    // Fall back to the nearest concrete visible post.
    const Range visible = visibleRange();
    if (!visible.isValid()) {
        return false;
    }
    const int center = (visible.first + visible.last) / 2;
    for (int distance = 0; distance <= visible.count(); ++distance) {
        const int candidates[] = {center - distance, center + distance};
        for (int index : candidates) {
            if (index < visible.first || index > visible.last) {
                continue;
            }
            BackendPost* post = postSource->postAt(index);
            if (post && !post->id.isEmpty()) {
                postId = post->id;
                return true;
            }
        }
    }
    return false;
}

bool ChatLogWidget::restoreViewportBookmark(const QString& postId)
{
    if (!postSource || postId.isEmpty()) {
        return false;
    }

    // Reuse the measured-row semantic navigation path, but deliberately do not
    // request a navigation flash for an ordinary channel revisit.
    return lockNavigationToPost(postId, Alignment::Center, 0);
}

bool ChatLogWidget::ensurePostVisible(const QString& postId, Alignment alignment)
{
    if (!postSource || postId.isEmpty()) {
        return false;
    }
    const int index = postSource->ensurePostIndex(postId);
    if (index < 0) {
        return false;
    }

    // Once semantic navigation established a viewport lock, repeated
    // ensure/go-to calls must not re-apply Center/Top and move the post again.
    // Only an authoritative identity remap updates the locked logical index.
    if (navigationPostId == postId && hasViewportLock()) {
        if (index != navigationLogicalIndex) {
            if (!remapViewportLockedItem(index)) {
                return false;
            }
            navigationLogicalIndex = index;
        }
        return true;
    }

    scrollToIndex(index, alignment);
    return true;
}

void ChatLogWidget::highlightPost(const QString& postId)
{
    if (postId.isEmpty()) {
        return;
    }

    if (PostWidget* widget = findPost(postId)) {
        widget->setFocus(Qt::OtherFocusReason);
        if (auto* interactive = dynamic_cast<InteractivePostWidget*>(widget)) {
            interactive->animateNavigationHighlight();
        }
        widget->update();
        if (pendingHighlightPostId == postId) {
            pendingHighlightPostId.clear();
        }
        return;
    }

    // Semantic navigation can request the visual cue before LongListWidget has
    // materialized the newly loaded target. Keep only the latest target and
    // deliver the cue after the same post becomes a concrete widget.
    pendingHighlightPostId = postId;
    scheduleNavigationFinalize();
}

void ChatLogWidget::refreshPost(const QString& postId)
{
    if (!postSource || postId.isEmpty()) {
        return;
    }

    const int index = postSource->indexOfPost(postId);
    PostWidget* widget = findPost(postId);
    BackendPost* post = index >= 0 ? postSource->postAt(index) : nullptr;
    if (!widget || !post) {
        return;
    }

    // BackendPost is the model object already referenced by PostWidget. Refresh
    // presentation in-place so focus, selection, hover and navigation animation
    // survive ordinary data updates.
    if (post->isDeleted) {
        widget->markAsDeleted();
        return;
    }

    widget->setEdited(post->message);
    widget->updateReactions();
    if (chatArea && !chatArea->isThread && post->root_id.isEmpty()) {
        widget->addThreadButton();
    }
    itemsChanged(index, index);
}

void ChatLogWidget::followOwnPost(const QString& postId)
{
    // This policy belongs to the Mattermost-specific list, not to the generic
    // LongListWidget: a locally confirmed outgoing post means the user wants the
    // live edge even if the list was not considered sticky-bottom a moment ago.
    // Defer one event-loop turn so source insertion and availability signals have
    // completed before the final content height is used.
    QTimer::singleShot(0, this, [this, postId] {
        if (!postSource || postId.isEmpty() || postSource->indexOfPost(postId) < 0) {
            return;
        }
        qCDebug(lcTimelineTrace).nospace()
            << "FOLLOW_OWN_POST list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " postId=" << postId;
        clearNavigationLock();
        scrollToEnd();
        scheduleReadCursorUpdate();
    });
}

void ChatLogWidget::refreshReadState()
{
    scheduleReadCursorUpdate();
}

bool ChatLogWidget::lockNavigationToPost(const QString& postId,
                                         Alignment alignment,
                                         int quietPeriodMs)
{
    if (!postSource || postId.isEmpty()) {
        clearNavigationLock();
        return false;
    }

    const int index = postSource->ensurePostIndex(postId);
    if (index < 0) {
        clearNavigationLock();
        return false;
    }

    // Drop the old physical lock before assigning the new semantic target;
    // viewportLockReleased is allowed to clear only the previous navigation.
    clearViewportLock();
    navigationPostId = postId;
    navigationLogicalIndex = index;
    navigationAlignment = alignment;
    navigationQuietPeriodMs = std::max(0, quietPeriodMs);
    navigationLockPending = true;
    navigationRecenterPending = false;

    // First move the correct logical identity into the materialization window.
    // At this point its height may still be the generic 96px estimate. The real
    // Center/Top/Bottom lock is installed only after createItemWidget() has run
    // and LongListWidget has measured the concrete post.
    if (!itemWidget(index)) {
        scrollToIndex(index, alignment);
        scheduleNavigationFinalize();
        return true;
    }

    return finalizeNavigationLock();
}

bool ChatLogWidget::finalizeNavigationLock()
{
    if (!postSource || navigationPostId.isEmpty()) {
        return false;
    }

    const int index = postSource->indexOfPost(navigationPostId);
    if (index < 0) {
        return false;
    }
    navigationLogicalIndex = index;

    if (!itemWidget(index)) {
        if (navigationLockPending) {
            scrollToIndex(index, navigationAlignment);
        }
        return false;
    }

    if (navigationLockPending || navigationRecenterPending || !hasViewportLock()) {
        navigationLockPending = false;
        navigationRecenterPending = false;
        if (!lockViewportToItem(index, navigationAlignment, navigationQuietPeriodMs)) {
            return false;
        }
    }

    if (pendingHighlightPostId == navigationPostId) {
        highlightPost(pendingHighlightPostId);
    }
    scheduleReadCursorUpdate();
    return true;
}

void ChatLogWidget::scheduleNavigationFinalize()
{
    if ((!navigationLockPending && !navigationRecenterPending
         && pendingHighlightPostId.isEmpty())
        || (!postSource && pendingHighlightPostId.isEmpty())) {
        return;
    }

    QTimer::singleShot(0, this, [this] {
        if (navigationLockPending || navigationRecenterPending) {
            finalizeNavigationLock();
        }
        if (!pendingHighlightPostId.isEmpty()) {
            highlightPost(pendingHighlightPostId);
        }
    });
}

void ChatLogWidget::scheduleReadCursorUpdate()
{
    if (readCursorUpdatePending_) {
        return;
    }
    readCursorUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        readCursorUpdatePending_ = false;
        updateReadCursorFromViewport();
    });
}

void ChatLogWidget::updateReadCursorFromViewport()
{
    if (!backend || !chatArea || !postSource || !chatArea->isVisible()
        || !chatArea->isActiveWindow() || viewport()->height() <= 0) {
        return;
    }

    // Reading is a viewport fact, not a navigation fact. Among concrete posts
    // whose lower edge has entered the viewport, advance through the newest
    // semantic (create_at, id) boundary. Wheel scrolling, dragging/clicking the
    // scrollbar and programmatic navigation all converge on this same test.
    int readIndex = -1;
    BackendPost* readPost = nullptr;
    const int viewportHeight = viewport()->height();
    for (int index : materializedIndices()) {
        QWidget* widget = itemWidget(index);
        if (!widget) {
            continue;
        }
        const int bottom = widget->y() + widget->height();
        if (bottom <= 0 || bottom > viewportHeight) {
            continue;
        }
        BackendPost* post = postSource->postAt(index);
        if (!post || post->id.isEmpty()) {
            continue;
        }
        if (!readPost || isAfter(*post, *readPost)) {
            readIndex = index;
            readPost = post;
        }
    }

    if (manualUnreadGate_.active()) {
        const QString gatedPostId = manualUnreadGate_.postId();
        const int gatedIndex = postSource->indexOfPost(gatedPostId);
        if (gatedIndex < 0) {
            manualUnreadGate_.clear();
            manualUnreadHighWaterPostId_.clear();
            manualUnreadHighWaterCreateAt_ = 0;
            manualUnreadExitedViewport_ = false;
        } else {
            const auto phaseBefore = manualUnreadGate_.phase();
            const bool lowerEdgeVisible = isPostLowerEdgeVisible(gatedPostId);
            const bool blocked = manualUnreadGate_.update(lowerEdgeVisible);
            if (phaseBefore == ManualUnreadVisibilityGate::Phase::WaitForExit
                && manualUnreadGate_.phase()
                    == ManualUnreadVisibilityGate::Phase::WaitForEntry) {
                manualUnreadExitedViewport_ = true;
            }

            if (blocked) {
                BackendPost* gatedPost = postSource->postAt(gatedIndex);
                if (manualUnreadExitedViewport_ && readPost && gatedPost
                    && isAfter(*readPost, *gatedPost)) {
                    const bool afterHighWater = manualUnreadHighWaterPostId_.isEmpty()
                        || readPost->create_at > manualUnreadHighWaterCreateAt_
                        || (readPost->create_at == manualUnreadHighWaterCreateAt_
                            && readPost->id > manualUnreadHighWaterPostId_);
                    if (afterHighWater) {
                        manualUnreadHighWaterPostId_ = readPost->id;
                        manualUnreadHighWaterCreateAt_ = readPost->create_at;
                    }
                }

                qCDebug(lcTimelineTrace).nospace()
                    << "READ_CURSOR_MANUAL_UNREAD_BLOCK list="
                    << static_cast<const void*>(this)
                    << " post=" << gatedPostId
                    << " lowerEdgeVisible=" << lowerEdgeVisible
                    << " highWater=" << manualUnreadHighWaterPostId_;
                return;
            }

            // Mark-as-unread intentionally suppresses cursor updates while its
            // visible marker is being moved out and back into the viewport. Do
            // not throw away the newer posts that were genuinely read during
            // that excursion: once the marker is re-entered, resume from the
            // newest lower edge observed after it left the viewport.
            if (!manualUnreadHighWaterPostId_.isEmpty()) {
                const int highWaterIndex =
                    postSource->indexOfPost(manualUnreadHighWaterPostId_);
                BackendPost* highWaterPost = highWaterIndex >= 0
                    ? postSource->postAt(highWaterIndex) : nullptr;
                if (highWaterPost && (!readPost || isAfter(*highWaterPost, *readPost))) {
                    readIndex = highWaterIndex;
                    readPost = highWaterPost;
                }
                qCDebug(lcTimelineTrace).nospace()
                    << "READ_CURSOR_MANUAL_UNREAD_RELEASE list="
                    << static_cast<const void*>(this)
                    << " marker=" << gatedPostId
                    << " highWater=" << manualUnreadHighWaterPostId_
                    << " effective=" << (readPost ? readPost->id : QString());
            }
            manualUnreadHighWaterPostId_.clear();
            manualUnreadHighWaterCreateAt_ = 0;
            manualUnreadExitedViewport_ = false;
        }
    }

    if (!readPost || readIndex < 0) {
        return;
    }

    BackendChannel& channel = chatArea->getChannel();
    auto& followingModel = FollowingModel::instance(*backend);
    auto& sidebar = SidebarService::instance(*backend);
    const bool sourceTailRead = readIndex == postSource->itemCount() - 1;
    const bool rootUnreadConversation = sidebar.usesRootUnreadCounts(channel);

    qCDebug(lcTimelineTrace).nospace()
        << "READ_CURSOR list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " channel=" << channel.id
        << " thread=" << (chatArea->isThread ? chatArea->root_id : QString())
        << " post=" << readPost->id
        << " createAt=" << readPost->create_at
        << " index=" << readIndex
        << " sourceTailRead=" << sourceTailRead;

    if (chatArea->isThread) {
        bool threadAtEnd = sourceTailRead;
        if (auto* threadSource = qobject_cast<ThreadPostSource*>(postSource.data())) {
            threadAtEnd = threadAtEnd
                && threadSource->isPostPositionAuthoritative(readPost->id);
        }

        const FollowingModel::Entry* threadEntry =
            followingModel.findEntry(channel.id, chatArea->root_id);
        const bool shouldAcknowledgeThread = threadAtEnd && threadEntry
            && (threadEntry->requiresAttention()
                || threadEntry->resumeState == FollowingModel::ResumeState::FirstUnread);
        const QString threadTeamId = threadEntry ? threadEntry->teamId : QString();

        followingModel.observeReadThrough(channel.id, chatArea->root_id,
                                          *readPost, threadAtEnd);

        if (shouldAcknowledgeThread && !threadTeamId.isEmpty()) {
            const FollowingModel::Entry* current =
                followingModel.findEntry(channel.id, chatArea->root_id);
            if (current && current->resumeState == FollowingModel::ResumeState::AtEnd) {
                followingModel.markThreadRead(threadTeamId, chatArea->root_id);
            }
        }

        // With CRT root counters, replies are owned by the thread unread
        // domain and must not advance or acknowledge the parent DM/GM. On
        // servers/modes where replies still count for the channel, preserve
        // the legacy whole-conversation behavior.
        if ((channel.type == BackendChannel::directChannel
             || channel.type == BackendChannel::groupChannel)
            && !rootUnreadConversation) {
            const bool channelAtEnd = threadAtEnd
                && (channel.last_post_at == 0
                    || readPost->create_at >= channel.last_post_at);
            followingModel.observeReadThrough(channel.id, QString(),
                                              *readPost, channelAtEnd);
            if (channelAtEnd) {
                acknowledgeChannelRead(*backend, channel);
            }
        }
        return;
    }

    // The main ChannelPostSource contains roots only. When CRT root counters
    // define parent unread state, compare its tail with last_root_post_at; a
    // newer hidden reply belongs to the thread domain and must not keep a
    // DM/GM permanently unread. With CRT off (or root counters unavailable),
    // replies still belong to the whole conversation and last_post_at remains
    // the correct acknowledgement watermark.
    bool channelAtEnd = sourceTailRead;
    if (channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel) {
        const uint64_t conversationTail = rootUnreadConversation
            ? channel.last_root_post_at : channel.last_post_at;
        channelAtEnd = channelAtEnd
            && (conversationTail == 0
                || readPost->create_at >= conversationTail);
    }

    followingModel.observeReadThrough(
        channel.id, QString(), *readPost, channelAtEnd, rootUnreadConversation);
    if (channelAtEnd) {
        acknowledgeChannelRead(*backend, channel);
    }
}

bool ChatLogWidget::isPostLowerEdgeVisible(const QString& postId) const
{
    if (!postSource || postId.isEmpty() || viewport()->height() <= 0) {
        return false;
    }
    const int index = postSource->indexOfPost(postId);
    QWidget* widget = index >= 0 ? itemWidget(index) : nullptr;
    if (!widget) {
        return false;
    }
    const int bottom = widget->y() + widget->height();
    return bottom > 0 && bottom <= viewport()->height();
}

void ChatLogWidget::markPostUnread(const QString& postId)
{
    if (!backend || !chatArea || !postSource || postId.isEmpty()) {
        return;
    }
    const int index = postSource->indexOfPost(postId);
    BackendPost* post = index >= 0 ? postSource->postAt(index) : nullptr;
    if (!post) {
        return;
    }

    const QString channelId = chatArea->getChannel().id;
    const QString threadId = chatArea->isThread ? chatArea->root_id : QString();
    const uint64_t createAt = post->create_at;
    manualUnreadHighWaterPostId_.clear();
    manualUnreadHighWaterCreateAt_ = 0;
    manualUnreadExitedViewport_ = false;
    manualUnreadGate_.markUnread(postId, isPostLowerEdgeVisible(postId));

    QPointer<ChatLogWidget> guard(this);
    const auto completed =
        [guard, channelId, threadId, postId, createAt](bool success) {
            if (!guard || !guard->backend) {
                return;
            }
            if (!success) {
                if (guard->manualUnreadGate_.postId() == postId) {
                    guard->manualUnreadGate_.clear();
                    guard->manualUnreadHighWaterPostId_.clear();
                    guard->manualUnreadHighWaterCreateAt_ = 0;
                    guard->manualUnreadExitedViewport_ = false;
                    guard->scheduleReadCursorUpdate();
                }
                return;
            }

            auto& following = FollowingModel::instance(*guard->backend);
            following.markPostUnread(channelId, threadId, postId, createAt);
            if (!threadId.isEmpty()) {
                following.refreshThreads();
            }
            guard->scheduleReadCursorUpdate();
        };

    if (threadId.isEmpty()) {
        SidebarService::instance(*backend).markPostUnread(postId, completed);
        return;
    }

    // CRT owns thread unread state independently from the parent channel.
    // Mattermost exposes a dedicated thread endpoint; using the channel
    // set_unread endpoint here creates state that markThreadRead() can never
    // acknowledge. Prefer the model's authoritative team identity and fall
    // back to the channel/current team for a newly synthesized thread entry.
    QString teamId;
    auto& following = FollowingModel::instance(*backend);
    if (const auto* entry = following.findEntry(channelId, threadId)) {
        teamId = entry->teamId;
    }
    if (teamId.isEmpty() && chatArea->getChannel().team) {
        teamId = chatArea->getChannel().team->id;
    }
    if (teamId.isEmpty()) {
        teamId = backend->getCurrentTeamContextId();
    }
    ThreadFollowService::instance(*backend).markThreadUnread(
        teamId, threadId, postId, completed);
}

void ChatLogWidget::clearNavigationLock()
{
    navigationPostId.clear();
    pendingHighlightPostId.clear();
    navigationLogicalIndex = -1;
    navigationLockPending = false;
    navigationRecenterPending = false;
    clearViewportLock();
}

bool ChatLogWidget::editLastOwnPost()
{
    if (!postSource) {
        return false;
    }

    QVector<int> indices = materializedIndices();
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int index : indices) {
        BackendPost* post = postSource->postAt(index);
        if (!post || !post->isOwnPost()) {
            continue;
        }
        editedPostWidget = qobject_cast<PostWidget*>(itemWidget(index));
        if (editedPostWidget) {
            emit postEditInitiated(*post);
            return true;
        }
    }
    return false;
}

void ChatLogWidget::postEditFinished()
{
    editedPostWidget.clear();
}

QWidget* ChatLogWidget::createItemWidget(int index)
{
    if (!backend || !chatArea || !postSource) {
        return nullptr;
    }

    BackendPost* post = postSource->postAt(index);
    if (!post) {
        return nullptr;
    }

    if (isHiddenSystemPost(*post)) {
        // Keep the logical index occupied but render a zero-height transparent
        // row so the join/leave event is not visible and does not disturb the
        // virtualized layout geometry.
        return new HiddenPostRow(viewport());
    }

    BackendPost* lastRootPost = nullptr;
    if (index > 0) {
        if (BackendPost* previous = postSource->postAt(index - 1)) {
            lastRootPost = previous->rootPost;
        }
    }

    const QString postId = post->id;
    auto* widget = new InteractivePostWidget(
        *backend, *post, viewport(), chatArea, lastRootPost);
    qCDebug(lcTimelineTrace).nospace()
        << "CREATE_WIDGET list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " index=" << index
        << " postId=" << postId
        << " rootId=" << post->root_id
        << " widget=" << static_cast<const void*>(widget)
        << " sizeHint=" << widget->sizeHint().height()
        << " minHint=" << widget->minimumSizeHint().height();

    connect(widget, &PostWidget::dimensionsChanged, this, [this, postId, widget] {
        if (!postSource) {
            return;
        }
        const int currentIndex = postSource->indexOfPost(postId);
        qCDebug(lcTimelineTrace).nospace()
            << "DIMENSIONS_CHANGED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " index=" << currentIndex
            << " postId=" << postId
            << " widget=" << static_cast<const void*>(widget)
            << " y=" << widget->y()
            << " height=" << widget->height()
            << " sizeHint=" << widget->sizeHint().height()
            << " minHint=" << widget->minimumSizeHint().height();
        if (currentIndex >= 0) {
            itemsChanged(currentIndex, currentIndex);
            scheduleReadCursorUpdate();
            if (postId == navigationPostId && hasViewportLock()) {
                // Geometry commit is queued by itemsChanged() first. Re-center
                // one event-loop turn later using the new measured target height.
                navigationRecenterPending = true;
                scheduleNavigationFinalize();
            }
        }
    });
    widget->setWholeMessageSelectionMode(messageSelectionMode_);
    widget->setWholeMessageSelected(selectedPostIds_.contains(postId));
    connect(widget, &PostWidget::wholeMessageSelectionToggled,
            this, [this](const QString& id, bool selected) {
        setMessagePostSelected(id, selected);
    });
    connect(widget, &PostWidget::markUnreadRequested,
            this, &ChatLogWidget::markPostUnread);
    if (selectedPostIds_.contains(postId)) {
        cacheSelectedPost(postId);
    }
    return widget;
}

QString ChatLogWidget::itemIdentity(const QWidget* widget) const
{
    const auto* postWidget = qobject_cast<const PostWidget*>(widget);
    return postWidget ? postWidget->post.id : QString();
}

int ChatLogWidget::indexOfItemIdentity(const QString& identity) const
{
    return postSource && !identity.isEmpty()
        ? postSource->indexOfPost(identity) : -1;
}

bool ChatLogWidget::isModelItemAvailable(int index) const
{
    return postSource && postSource->isAvailable(index);
}

void ChatLogWidget::destroyItemWidget(int index, QWidget* widget)
{
    const auto* postWidget = qobject_cast<const PostWidget*>(widget);
    qCDebug(lcTimelineTrace).nospace()
        << "DESTROY_WIDGET list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " index=" << index
        << " postId=" << (postWidget ? postWidget->post.id : QString())
        << " widget=" << static_cast<const void*>(widget)
        << " y=" << (widget ? widget->y() : 0)
        << " height=" << (widget ? widget->height() : 0);

    if (editedPostWidget == widget) {
        editedPostWidget.clear();
    }
    LongListWidget::destroyItemWidget(index, widget);
}

AbstractPostSource::RequestReason ChatLogWidget::toSourceReason(RequestReason reason)
{
    switch (reason) {
    case RequestReason::Initial:
        return AbstractPostSource::RequestReason::Initial;
    case RequestReason::Seek:
        return AbstractPostSource::RequestReason::Seek;
    case RequestReason::EnsureVisible:
        return AbstractPostSource::RequestReason::EnsureVisible;
    case RequestReason::Scroll:
    default:
        return AbstractPostSource::RequestReason::Scroll;
    }
}

void ChatLogWidget::reconnectSource()
{
    if (!postSource) {
        return;
    }

    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemCountChanged,
                                        this, [this](int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_ITEM_COUNT list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " count=" << count;
        setItemCount(count);
        restoreNavigationTarget();
        scheduleReadCursorUpdate();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsInserted,
                                        this, [this](int first, int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_INSERTED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " first=" << first
            << " count=" << count;
        insertItems(first, count);
        restoreNavigationTarget();
        scheduleReadCursorUpdate();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsRemoved,
                                        this, [this](int first, int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_REMOVED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " first=" << first
            << " count=" << count;
        removeItems(first, count);
        restoreNavigationTarget();
        scheduleReadCursorUpdate();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_AVAILABLE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';
        setRangeAvailable(first, last, true);
        restoreNavigationTarget();
        scheduleReadCursorUpdate();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::bodyAvailabilityChanged,
                                        this, [this](int first, int last, bool bodyAvailable) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_BODY_AVAILABILITY list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']'
            << " available=" << bodyAvailable;
        setRangeAvailable(first, last, bodyAvailable);
        if (bodyAvailable) {
            restoreNavigationTarget();
            scheduleReadCursorUpdate();
        }
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::seekTargetResolved,
                                        this, &LongListWidget::resolveSeekTarget));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::layoutChanged,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_LAYOUT_CHANGED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';
        reconcileItemLayout(first, last);
        restoreNavigationTarget();
        scheduleReadCursorUpdate();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeRequestFinished,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_REQUEST_FINISHED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';

        // Source and view availability can momentarily diverge when an exact
        // identity/body is already resident by the time LongList's sparse range
        // request reaches the source. A source is then allowed to finish without
        // another network operation, but the view still needs the availability
        // transition that caused the request to become unnecessary.
        if (postSource && itemCount() > 0) {
            const int boundedFirst = std::max(0, first);
            const int boundedLast = std::min(itemCount() - 1, last);
            int availableRunFirst = -1;
            for (int index = boundedFirst; index <= boundedLast; ++index) {
                const bool reconcile = postSource->isAvailable(index)
                    && !isItemAvailable(index);
                if (reconcile) {
                    if (availableRunFirst < 0) {
                        availableRunFirst = index;
                    }
                    continue;
                }
                if (availableRunFirst >= 0) {
                    qCDebug(lcTimelineTrace).nospace()
                        << "SOURCE_RECONCILE_AVAILABLE list="
                        << static_cast<const void*>(this)
                        << " source=" << sourceName(postSource)
                        << " range=[" << availableRunFirst << ',' << (index - 1) << ']';
                    setRangeAvailable(availableRunFirst, index - 1, true);
                    availableRunFirst = -1;
                }
            }
            if (availableRunFirst >= 0) {
                qCDebug(lcTimelineTrace).nospace()
                    << "SOURCE_RECONCILE_AVAILABLE list="
                    << static_cast<const void*>(this)
                    << " source=" << sourceName(postSource)
                    << " range=[" << availableRunFirst << ',' << boundedLast << ']';
                setRangeAvailable(availableRunFirst, boundedLast, true);
            }
        }

        finishRangeRequest(first, last);
        scheduleReadCursorUpdate();
    }));

    if (!chatArea) {
        return;
    }

    BackendChannel& channel = chatArea->getChannel();
    sourceConnections.push_back(connect(&channel, &BackendChannel::onPostEdited,
                                        this, [this](BackendPost& post) {
        if (PostWidget* widget = findPost(post.id)) {
            widget->setEdited(post.message);
            const int index = postSource ? postSource->indexOfPost(post.id) : -1;
            if (index >= 0) {
                itemsChanged(index, index);
            }
        }
    }));
    sourceConnections.push_back(connect(&channel, &BackendChannel::onPostReactionUpdated,
                                        this, [this](BackendPost& post) {
        if (PostWidget* widget = findPost(post.id)) {
            widget->updateReactions();
            const int index = postSource ? postSource->indexOfPost(post.id) : -1;
            if (index >= 0) {
                itemsChanged(index, index);
            }
        }
    }));
    sourceConnections.push_back(connect(&channel, &BackendChannel::onPostDeleted,
                                        this, [this](const QString& postId) {
        if (PostWidget* widget = findPost(postId)) {
            widget->markAsDeleted();
        }
    }));
    sourceConnections.push_back(connect(&channel, &BackendChannel::onThreadSummaryChanged,
                                        this, [this](BackendPost& rootPost) {
        if (!chatArea || chatArea->isThread) {
            return;
        }
        if (PostWidget* widget = findPost(rootPost.id)) {
            widget->addThreadButton();
            const int index = postSource ? postSource->indexOfPost(rootPost.id) : -1;
            if (index >= 0) {
                itemsChanged(index, index);
            }
        }
    }));
    sourceConnections.push_back(connect(&channel, &BackendChannel::onNewPost,
                                        this, [this](BackendPost& post) {
        // A genuinely new row is materialized from the already-current model.
        // If this is instead a duplicate/confirmation for an existing resident
        // identity, refresh that same physical widget in place.
        if (findPost(post.id)) {
            refreshPost(post.id);
        }
    }));
}

bool ChatLogWidget::restoreNavigationTarget()
{
    if (!postSource || navigationPostId.isEmpty()) {
        return false;
    }

    const int index = postSource->indexOfPost(navigationPostId);
    if (index < 0) {
        return false;
    }

    if (navigationLockPending || !hasViewportLock()) {
        if (index != navigationLogicalIndex) {
            navigationLogicalIndex = index;
            scrollToIndex(index, navigationAlignment);
        }
        scheduleNavigationFinalize();
        return true;
    }

    if (index == navigationLogicalIndex) {
        return true;
    }

    qCDebug(lcTimelineTrace).nospace()
        << "NAV_REMAP list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " postId=" << navigationPostId
        << " oldIndex=" << navigationLogicalIndex
        << " newIndex=" << index;

    if (!remapViewportLockedItem(index)) {
        return false;
    }
    navigationLogicalIndex = index;
    return true;
}

void ChatLogWidget::resizeEvent(QResizeEvent* event)
{
    PostListWidget::resizeEvent(event);
    positionSelectionToolbar();
}

void ChatLogWidget::beginMessageSelectionDrag(const QString& anchorPostId,
                                              const QString& currentPostId)
{
    if (!postSource || anchorPostId.isEmpty() || currentPostId.isEmpty()) {
        return;
    }
    messageSelectionMode_ = true;
    messageSelectionDragActive_ = true;
    messageSelectionAnchorPostId_ = anchorPostId;
    setMessageSelectionRange(currentPostId);
}

void ChatLogWidget::updateMessageSelectionDrag(const QString& currentPostId)
{
    if (!messageSelectionDragActive_ || currentPostId.isEmpty()) {
        return;
    }
    setMessageSelectionRange(currentPostId);
}

void ChatLogWidget::finishMessageSelectionDrag()
{
    messageSelectionDragActive_ = false;
}

void ChatLogWidget::setMessageSelectionRange(const QString& currentPostId)
{
    if (!postSource) {
        return;
    }
    const int anchor = postSource->indexOfPost(messageSelectionAnchorPostId_);
    const int current = postSource->indexOfPost(currentPostId);
    const PostSelectionRange range = postSelectionRange(anchor, current);
    if (!range.isValid()) {
        return;
    }

    selectedPostIds_.clear();
    selectedOwnPostIds_.clear();
    selectedFormattedPosts_.clear();
    for (int index = range.first; index <= range.last; ++index) {
        BackendPost* selected = postSource->postAt(index);
        if (!selected || selected->id.isEmpty()) {
            continue;
        }
        selectedPostIds_.insert(selected->id);
        if (selected->isOwnPost() && !selected->isDeleted) {
            selectedOwnPostIds_.insert(selected->id);
        }
    }
    applyMessageSelectionVisuals();
}

void ChatLogWidget::setMessagePostSelected(const QString& postId, bool selected)
{
    if (!messageSelectionMode_ || postId.isEmpty()) {
        return;
    }
    if (selected) {
        selectedPostIds_.insert(postId);
        cacheSelectedPost(postId);
        if (postSource) {
            const int index = postSource->indexOfPost(postId);
            BackendPost* post = index >= 0 ? postSource->postAt(index) : nullptr;
            if (post && post->isOwnPost() && !post->isDeleted) {
                selectedOwnPostIds_.insert(postId);
            }
        }
    } else {
        selectedPostIds_.remove(postId);
        selectedOwnPostIds_.remove(postId);
        selectedFormattedPosts_.remove(postId);
    }

    if (postSelectionShouldExit(selectedPostIds_.size())) {
        cancelMessageSelection();
        return;
    }
    applyMessageSelectionVisuals();
}

void ChatLogWidget::cacheSelectedPost(const QString& postId)
{
    PostWidget* widget = findPost(postId);
    if (!widget) {
        return;
    }
    selectedFormattedPosts_.insert(
        postId, widget->formatForClipboardSelection(PostWidget::entirePost));
    if (widget->post.isOwnPost() && !widget->post.isDeleted) {
        selectedOwnPostIds_.insert(postId);
    }
}

void ChatLogWidget::applyMessageSelectionVisuals()
{
    const Range range = materializedRange();
    if (range.isValid()) {
        for (int index = range.first; index <= range.last; ++index) {
            auto* widget = qobject_cast<PostWidget*>(itemWidget(index));
            if (!widget) {
                continue;
            }
            widget->setWholeMessageSelectionMode(messageSelectionMode_);
            widget->setWholeMessageSelected(selectedPostIds_.contains(widget->post.id));
            if (messageSelectionMode_) {
                widget->clearTextSelection();
                if (selectedPostIds_.contains(widget->post.id)) {
                    cacheSelectedPost(widget->post.id);
                }
            }
        }
        // LongListWidget captures/restores the viewport anchor for remeasurement;
        // widgets are retained and only their geometry changes for the checkbox gutter.
        itemsChanged(range.first, range.last);
    }
    updateSelectionToolbar();
}

void ChatLogWidget::cancelMessageSelection()
{
    if (!messageSelectionMode_ && selectedPostIds_.isEmpty()) {
        return;
    }
    messageSelectionMode_ = false;
    messageSelectionDragActive_ = false;
    messageSelectionAnchorPostId_.clear();
    selectedPostIds_.clear();
    selectedOwnPostIds_.clear();
    selectedFormattedPosts_.clear();
    applyMessageSelectionVisuals();
}

void ChatLogWidget::ensureSelectionToolbar()
{
    if (selectionToolbar_) {
        return;
    }
    selectionToolbar_ = new QFrame(viewport());
    selectionToolbar_->setFrameShape(QFrame::StyledPanel);
    selectionToolbar_->setAutoFillBackground(true);
    auto* layout = new QHBoxLayout(selectionToolbar_);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    selectionCountLabel_ = new QLabel(selectionToolbar_);
    layout->addWidget(selectionCountLabel_);

    selectionDeleteButton_ = new QPushButton(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/trash")), tr("Delete"), selectionToolbar_);
    selectionCopyButton_ = new QPushButton(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/copy")), tr("Copy"), selectionToolbar_);
    auto* cancelButton = new QPushButton(tr("Cancel"), selectionToolbar_);
    layout->addWidget(selectionDeleteButton_);
    layout->addWidget(selectionCopyButton_);
    layout->addWidget(cancelButton);

    connect(selectionDeleteButton_, &QPushButton::clicked,
            this, &ChatLogWidget::deleteSelectedOwnPosts);
    connect(selectionCopyButton_, &QPushButton::clicked,
            this, &ChatLogWidget::copySelectedPosts);
    connect(cancelButton, &QPushButton::clicked,
            this, &ChatLogWidget::cancelMessageSelection);
    selectionToolbar_->hide();
}

void ChatLogWidget::updateSelectionToolbar()
{
    if (!messageSelectionMode_) {
        if (selectionToolbar_) {
            selectionToolbar_->hide();
        }
        return;
    }
    ensureSelectionToolbar();
    selectionCountLabel_->setText(tr("%1 selected").arg(selectedPostIds_.size()));
    selectionDeleteButton_->setEnabled(!selectedOwnPostIds_.isEmpty());
    selectionCopyButton_->setEnabled(!selectedPostIds_.isEmpty());
    selectionToolbar_->adjustSize();
    positionSelectionToolbar();
    selectionToolbar_->show();
    selectionToolbar_->raise();
}

void ChatLogWidget::positionSelectionToolbar()
{
    if (!selectionToolbar_ || !viewport()) {
        return;
    }
    selectionToolbar_->adjustSize();
    const int x = std::max(8, (viewport()->width() - selectionToolbar_->width()) / 2);
    selectionToolbar_->move(x, 8);
}

void ChatLogWidget::copySelectedPosts()
{
    if (!postSource || selectedPostIds_.isEmpty()) {
        return;
    }
    QStringList blocks;
    for (int index = 0; index < postSource->itemCount(); ++index) {
        BackendPost* post = postSource->postAt(index);
        if (!post || !selectedPostIds_.contains(post->id)) {
            continue;
        }
        QString formatted = selectedFormattedPosts_.value(post->id);
        if (formatted.isEmpty()) {
            if (PostWidget* widget = findPost(post->id)) {
                formatted = widget->formatForClipboardSelection(PostWidget::entirePost);
            }
        }
        if (!formatted.isEmpty()) {
            blocks.push_back(formatted);
        }
    }
    if (!blocks.isEmpty()) {
        QApplication::clipboard()->setText(blocks.join(QStringLiteral("\n\n")));
    }
}

void ChatLogWidget::deleteSelectedOwnPosts()
{
    if (!backend) {
        return;
    }
    const QSet<QString> ownPosts = selectedOwnPostIds_;
    for (const QString& postId : ownPosts) {
        backend->deletePost(postId);
    }
    cancelMessageSelection();
}


} // namespace Mattermost
