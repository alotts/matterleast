# Channel and conversation discovery

This document owns the UI contract for finding/creating team channels and direct/group conversations. It is intentionally separate from sidebar ordering/reconciliation: discovery dialogs may materialize or open channels, but they do not own server sidebar category state.

## Entry points

The visible sidebar category headers are the primary discovery affordances:

- **Channels +** opens `TeamChannelsListDialog` for that category's team.
- **Direct Messages +** opens `UserSearchDialog` in `ConversationPicker` mode.
- The active-team selector menu also exposes **Browse public channels…**.

Category + controls are real `QToolButton` widgets installed in column 1 of the tree, not text painted into a narrow column. Keep a right gutter between the button and the vertical scrollbar so the action remains an easy click target.

## Public-channel browser

`TeamChannelsListDialog` uses `LongListWidget`; HTTP pagination is an input to the directory source, not a reason to use a table widget.

Rows follow the same visual identity as the sidebar:

- use `ChannelIcons::channel()` for public channels;
- show the channel display name as the primary line;
- show joined/member-count/purpose metadata as a subdued second line;
- do not render the channel header in the list.

Member counts are loaded in batches through `POST /channels/stats/member_count`. The ordinary team public-channel endpoint does not return a total count, so the directory may display a lower bound (for example `100+ channels`) until a short final page proves the end.

The dialog also owns **Create channel…**. `CreateChannelDialog` collects display name, URL name, purpose and public/private type; `Backend::createChannel()` materializes the returned team channel and normal navigation opens it.

## Conversation picker

`UserSearchDialog::Mode::ConversationPicker` is the shared DM/GM workflow. Generic user-selection uses of `UserSearchDialog` must remain unaffected.

The picker has two independent inputs:

1. the ordinary text filter/search field;
2. a **Participants:** chip row.

Filtering composes differently for DMs and GMs:

- non-empty text -> DMs use ordinary text search, independent of participant chips;
- non-empty text + selected participants -> GMs must both contain all selected participants and match the text;
- empty text + selected participants -> show existing DM/GM conversations containing all selected participants;
- both empty -> show the existing DM/GM switcher exactly as the default view.

Double-clicking a person adds that user to Participants. Double-clicking a GM resolves its small authoritative member set and adds all remote participants. Chips are removable with their × button.

A matching GM is an **exact** match only when its remote participant set equals the selected participant set. Superset matches are discovery results, not substitutes for the requested conversation.

Action rules:

- exact selected existing conversation -> **Open chat** only;
- partial/superset match, or no participant-filter result -> **New conversation**;
- without participant chips, selecting an existing DM/GM offers **Open chat**, while selecting a new person offers **New conversation**.

One selected remote participant creates/opens a DM. Two or more use `POST /channels/group`; Mattermost itself canonicalizes the participant set and returns an existing GM when one already exists.

## Ownership and persistence

Creation helpers in `Backend` materialize successful server responses into `Storage` before invoking UI callbacks. Navigation then goes through `AppNavigationService`, so opening a newly created DM/GM/team channel follows the same sidebar admission and semantic-navigation path as every other open operation.

Sidebar admission must never run while the channel tree is already structurally mutating. Removing or reordering a row during `ChannelTree::reconcileTeamSidebar()` re-emits `QTreeWidget::currentItemChanged`, which re-enters `activateChannelItem()` and from there a search-result DM/GM can call sidebar admission again, whose reconcile takes/reorders rows and recurses until the stack overflows (observed as a SIGSEGV when a channel-scoped search promotes a DM). `activateChannelItem()` therefore performs search-hit DM/GM admission only when `renderingSidebar` is clear: the outermost activation promotes the channel (via `promoteSidebarConversation`, before reconcile) and the in-flight render places it, while a nested activation simply defers. Keep `renderingSidebar` as the single gate for admission triggered from activation, and never gate `reconcileTeamSidebar()` itself on it or the sidebar will not populate.

Do not add a second DM-only dialog path. Sidebar + actions, legacy TeamItem context menus, and other "start conversation" affordances should all enter through `UserSearchDialog::showConversationPicker()`.
