# Following behavior

This document is the normative contract for the **Following** sidebar view.
It describes which semantic rows exist, how they are ordered, where activation
navigates, and what repeated clicks must do.

Read detection itself is defined separately by
[Following, Attention and read tracking](following-attention-read-tracking.md).
Following consumes that shared state; it is not allowed to invent a second read
state machine.

## Purpose

Following is a queue of conversations and threads the user may want to return to.
It is broader than Attention:

- **Following** shows the shared queue, including followed threads that are
  already read and conversation rows that currently belong in the queue.
- **Attention** is a filtered projection of the same `FollowingModel` entries
  whose `requiresAttention()` is true. Channel mute suppresses conversation and
  synthetic-attention rows, but it does **not** suppress an explicitly followed
  unread thread.

A Following click is **navigation**. It is never proof that a message was read.

## Semantic model and ownership

`FollowingModel` is the single semantic owner. `ChannelQuickList` renders
Following. The view may keep short-lived presentation state (selected key,
retained row geometry, last activated key), but must not own unread/resume truth.

Stable row identity is:

```text
conversation: c:<channelId>
thread:       t:<threadId>
```

An `Entry` contains, among other metadata:

```text
kind                 Conversation | Thread
channelId
threadId
lastReplyAt
attentionSince
unread / unreadReplies / unreadMentions
resumeState          Unknown | FirstUnread | AtEnd
firstUnreadPostId
readThroughPostId
readThroughCreateAt
```

The semantic read cursor is owned by `FollowingModel`; the selected item in the
tree is not a cursor.

## Which rows appear

### Conversation rows

Conversation rows are created from channel unread/manual-attention state.

Normal DM/GM behavior:

- an unread DM/GM is represented in Following;
- once it is fully read and no longer requires queue membership, it disappears
  from the shared model;
- the selected row may remain visible briefly as a **presentation-only retained
  row** so it does not vanish under the mouse during the click that consumed it.

A public/private channel is not generally a persistent Following conversation.
It can be projected there when an explicit local/server-backed **Mark as unread**
creates manual attention for that channel.

Muted conversation rows are not shown unless manual attention explicitly owns
the local projection according to `FollowingModel`. This mute rule does not
apply to a real followed thread: following is an explicit user opt-in, so an
unread followed thread remains in Attention even when its parent channel/DM is
muted. Synthetic thread-shaped mentions are not explicit follows and remain
subject to mute suppression.

Mute suppresses this Following/Attention projection, not the regular channel
sidebar's unread treatment: `ChannelActivityTracker` still reports a muted
channel as unread when it has new messages (`SidebarService::isChannelUnread`),
so the channel row appears unread until viewed. Mute only suppresses desktop
notifications, which is a separate policy in `MainWindow::messageNotify`.

### Thread rows

Thread rows primarily come from the Mattermost CRT followed-thread snapshot.
They remain in Following when read because "followed" is membership, not just
unread state.

DM/GM threads are part of the same CRT domain. The shared snapshot must include
them as well: with collapsed reply threads enabled, a parent DM/GM conversation
uses root-only unread counts, so an unread reply inside a DM/GM thread is
represented by the thread entry rather than by the parent conversation row.

A root mention can temporarily create a synthetic thread-shaped entry before a
real CRT snapshot contains that thread. The synthetic entry is replaced/reconciled
by the shared model; Following must not build a parallel synthetic store.

Unfollowing a thread removes its normal followed-thread membership.

## Presentation text

Thread rows may show a compact message snippet in the row label. Their tooltip must never expose the
entire message body: it contains only the first source line, trimmed and capped to the same compact
snippet length. Following and Attention share this presentation helper so a multiline or very large
post cannot create a screen-sized tooltip.

## Ordering

Following sorts unread/attention rows before non-attention rows.

Within each group it sorts by descending semantic activity time:

- attention rows use `attentionSince` when available, otherwise
  `lastReplyAt`;
- read followed threads use `lastReplyAt`;
- stable semantic key is the final tie breaker.

While a selected row is being retained for visual stability, its previous unread
position/sort time may also be retained temporarily. This is presentation only:
it must not keep the semantic entry unread or extend its resume cursor lifetime.

## Resume cursor

The shared model maintains a monotonic read-through boundary:

```text
readThroughCreateAt
readThroughPostId
```

and one of three resume states:

| State | Meaning | Activation when entering/re-entering |
| --- | --- | --- |
| `FirstUnread` | a concrete next semantic post is known | navigate to `firstUnreadPostId` |
| `Unknown` | local cache has no next identity, but source end is not proven | ask server/current model for a resume target; never guess AtEnd |
| `AtEnd` | authoritative tail has been consumed | present the conversation/thread without a post jump |

Viewport read progress may advance `FirstUnread` while the user is looking at
the destination. That is expected. It does **not** mean the selected Following
row has turned into a "Next unread" button.

## Conversation activation contract

For a conversation row, distinguish **enter/re-enter** from **repeat activation**.

### First activation / re-entry

If the destination is not the same currently-active Following conversation:

1. `FirstUnread` → open that post.
2. `AtEnd` → open/present the channel without repositioning to a post.
3. `Unknown` → resolve a server/model resume target.
4. A server target at or behind local `readThrough` is stale and must not move
   the user backwards.

This remains true even if the channel is already visible for some other reason:
the first activation of the Following row is a semantic navigation request and
must honor its resume target.

### Repeated activation of the same already-open row

If all of the following are true:

- the same conversation Following/Attention row was already activated;
- its channel is still the active channel;
- the destination is a conversation, not a thread;

then another click is **presentation-only**:

```text
same Following row + same active channel
    -> present/refresh current channel
    -> preserve current viewport
    -> do not resolve mutable FirstUnread again
```

Why: viewport reads can advance `firstUnreadPostId` while the row remains
selected. Re-resolving it on every click would make one visible row behave as a
"Next unread" button and walk the user down the channel on repeated clicks.

If the user leaves that channel and later activates the Following row again,
that is re-entry and the current resume cursor is resolved normally.

The presentation-only repeated-click identity must be cleared when selection/tab
context is released or external navigation takes ownership. It is not persisted
and is not read state.

## Thread activation contract

Threads deliberately do **not** use the conversation repeat-click preservation
rule.

A followed thread is an ordered unread-reply domain:

- synthetic root mention → navigate to the root post and request thread snapshot
  reconciliation;
- `AtEnd` → present the thread;
- otherwise resume after local `readThroughCreateAt` when available, falling
  back to server `lastViewedAt`;
- `firstUnreadPostId` is a fallback concrete target when known.

Following thread presentation may preserve an already-open thread surface where
the navigation API requests that behavior, but the thread's semantic resume
sequence remains authoritative.

## Mark as unread

Mark as unread is server-backed and also creates a local manual marker so the UI
does not wait for a round trip before projecting attention.

Important distinctions:

- marking a post unread establishes a semantic unread boundary;
- the read cursor still advances only from the concrete viewport lower-edge rule;
- navigation/highlight alone does not consume the marker;
- if the marked post was already visible when Mark as unread was invoked, the
  `ManualUnreadVisibilityGate` requires its lower edge to leave and re-enter
  before it can be consumed immediately;
- for a normal channel, repeated clicks on its Following row must not use the
  moving `FirstUnread` cursor as "Next unread";
- leaving the channel and activating the row again does resume from the current
  unread boundary;
- thread Mark as unread remains independent CRT unread state and retains thread
  resume semantics.

## Server fallback and stale targets

For a conversation in `Unknown`, Following may ask Mattermost for the channel
unread post. That answer can lag behind local viewport progress.

If the returned post is cached and is at or behind
`(readThroughCreateAt, readThroughPostId)`, it is stale and must be rejected.
An uncached post cannot be ordered locally and is left to the normal navigation
resolver.

A stale server fallback must never move the viewport backwards.

## Read acknowledgement

Following does not mark rows read on activation.

The only proof of reading is the active `ChatLogWidget` viewport:

```text
0 < postWidget.bottom <= viewport.height
```

The newest semantic post satisfying that condition advances
`FollowingModel::observeReadThrough()`.

At an authoritative channel/thread tail, the appropriate server acknowledgement
is issued by the read-tracking subsystem. See:

- [Scrolling and resume cursor](read-tracking/scroll-and-resume.md)
- [Projections and server acknowledgement](read-tracking/projections-and-acknowledgement.md)

## Follow-state synchronization

Thread follow membership is server-owned. MatterLeast must not assume that posting
a reply always enables following because the server can disable
`ThreadAutoFollow`.

After a confirmed websocket `posted` event for the logged-in user's thread
reply, `ThreadFollowService` re-queries the thread membership and publishes the
authoritative result through `followingChanged`. This point is intentionally
later than the local outbox acknowledgement: pending/outbox rows can exist before
the server has created the post and applied auto-follow.

MatterLeast also consumes Mattermost's `thread_follow_changed` websocket event.
That keeps an already-open thread bell and `FollowingModel` synchronized when
follow/unfollow is changed from another client or another server path.

MatterLeast also consumes the user-scoped `thread_read_changed` event. Mattermost
emits three forms: one concrete thread, all threads in one channel (notably
DM/GM bulk reads), or all threads in one team. All three invalidate the shared
CRT snapshot and are coalesced through `FollowingModel`; the websocket layer
must not create a second unread-counter state machine. This lets reads from
another client update Following/Attention while preserving the local
manual-unread visibility gate.

## Required invariants

Future changes to Following must preserve all of these:

1. Following and Attention use the same `FollowingModel` read/resume state.
2. A click is navigation, never read acknowledgement.
3. Read progress comes only from concrete viewport geometry.
4. Back-scrolling cannot regress the semantic read-through high-water mark.
5. A stale server unread fallback cannot move navigation backwards.
6. A repeated click on the same already-open **conversation** row preserves the
   viewport; it is not "Next unread".
7. Leaving and re-entering the destination resolves the current cursor normally.
8. Thread resume semantics remain independent from the conversation repeat-click
   rule.
9. Selection retention / last-activated identity is presentation-only, ephemeral,
   and must never become durable unread state.
10. Read DM/GM disappearance must not remove the selected tree row underneath an
    in-progress click; temporary retained presentation is allowed.
11. Followed-thread membership and unreadness are different concepts: read
    followed threads may remain in Following.
12. Manual unread must not be consumed merely because the command was invoked on
    a post whose lower edge was already visible.
13. Local outbox acknowledgement must not be treated as proof of server
    auto-follow; reconcile follow membership only after confirmed server state.

## Implementation map

Primary code:

- `sources/backend/FollowingModel.{h,cpp}` — semantic entries, resume cursor,
  manual markers and CRT reconciliation.
- `sources/channel-tree/ChannelQuickList.cpp` — Following projection, ordering,
  row retention and activation.
- `sources/channel-tree/FollowingActivationPolicy.h` — pure repeated
  conversation activation policy.
- `sources/channel-tree/FollowingNavigation.h` — backend-aware stale resume
  target protection.
- `sources/channel-tree/AttentionList.cpp` — Attention projection of the same
  model.
- `sources/chat-area/ChatLogWidget.cpp` — viewport-derived read progress and
  Mark as unread entry point.
- `sources/backend/SidebarService.cpp` — channel unread/server acknowledgement.
- `sources/backend/ThreadFollowService.cpp` — CRT followed/unread operations.

Regression coverage should live near:

- `tests/FollowingPresentationTest.cpp` for pure Following presentation /
  activation policy;
- `tests/ManualUnreadReadStateTest.cpp` for shared resume/manual-unread state;
- `tests/ManualUnreadVisibilityGateTest.cpp` for visible-marker gating.
