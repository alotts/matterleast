# MatterLeast developer documentation

This directory is the durable engineering context for MatterLeast. It is intentionally organized for **progressive disclosure**: start here, open one subsystem overview, then load only the detailed document needed for the current change.

## Start here by task

| If you are changing… | Start with… | Load deeper only when… |
| --- | --- | --- |
| virtualized message list, viewport stability, anchors, scrolling | [LongListWidget architecture](long-list-architecture.md) | geometry, seek, materialization or source-boundary details are involved |
| timeline source composition, filtering, logical indices, optimistic/local outgoing rows | [Post source architecture](post-source-architecture.md) | changing IndexedPostSource, FilteredPostSource, channel/thread sources or adding a presentation augmentation such as the outbox |
| durable post cache design | [Post cache architecture](post-cache.md) | changing admission, persistence, authority, memory policy or reconnect behavior |
| runtime cache loading/paging/causality | [Post cache runtime contract](post-cache-runtime.md) | debugging snapshot ordering, cache-first load, paging or invalidation |
| thread paging and sparse loading | [Thread timeline loading](thread-timeline-loading.md) | working on thread gaps, cursors, permalink islands or live replies |
| Following queue membership, sorting and click/navigation behavior | [Following behavior](following.md) | changing which rows appear, ordering, repeated activation, channel/thread navigation or manual-unread behavior |
| unread/read state or Attention | [Following, Attention and read tracking](following-attention-read-tracking.md) | changing scroll-derived reads, resume cursors or server acknowledgement |
| virtual sidebar views and collections | [Virtual sidebar destinations](sidebar-virtual-destinations.md) | changing Personal, Saved, Recent Mentions or search paging |
| central navigation tabs, docked/tabbed/detached thread presentation | [Central navigation tabs](navigation-tabs.md) | changing tab ownership, semantic destinations or thread reparenting |
| channel discovery, channel creation, DM/GM picker and sidebar category actions | [Channel and conversation discovery](channel-discovery.md) | changing public-channel browsing, participant matching, or sidebar + actions |
| native plugin ABI, loading, host/accessor services or extension points | [Plugin architecture](plugins.md) | changing plugin lifecycle, ABI versioning, discovery, or adding a new host/accessor/extension interface |
| quoted replies | [Quoted replies](quoted-replies.md) | changing wire fallback or thread interaction |
| composer @mentions and user-group suggestions | [Mention autocomplete](mention-autocomplete.md) | changing user/group lookup, DM/GM mention scope, or completion merging |
| user-group browser, creation, membership and editing | [User groups](user-groups.md) | changing custom-group CRUD, membership mutations, or sidebar entry points |
| reaction quick bar/ranking | [Reaction quick bar](reaction-quick-bar.md) | changing ranking, cooling, persistence or custom emoji behavior |
| appearance settings, message or code (monospace) font typography | [Appearance fonts](appearance-fonts.md) | changing font keys, default derivation, live-preview/Cancel semantics or which surfaces use the code font |
| emoji picker, custom emoji lookup/search, live theme propagation | [Emoji resolution and picker search](emoji-resolution.md) | changing custom emoji discovery, caching, registry synchronization or picker palette behavior |
| HTTP request headers, cookies, CSRF, HTTP/2 or TLS behavior | [HTTP client profile and transport](network-transport.md) | changing the shared request identity or transport negotiation |
| debugging from logs | [Debug logging](debug-logging.md) | selecting categories or interpreting upload/read/navigation traces |
| CI, packaging or release workflow | [Maintainer workflow](maintainer-workflow.md) | changing build matrices, package targets or release artifacts |

## Documentation structure

Top-level files in `docs/` are entry points. Large subsystems put detailed contracts in a subdirectory:

- `docs/long-list/`
- `docs/post-sources/`
- `docs/post-cache/`
- `docs/read-tracking/`

Historical architecture that must not be used as current design lives under [deprecated/](deprecated/).

## Context discipline

When starting work:

1. Read this index.
2. Read the single subsystem landing page relevant to the task.
3. Load only the detailed file(s) named by that landing page.
4. Inspect code after the invariants are known; do not preload unrelated architecture docs.
5. If a change introduces a non-obvious invariant, ownership rule, failure mode or workflow convention, document it close to the subsystem rather than relying on chat history.
6. Keep landing pages short. Put implementation detail in focused child documents.
7. Prefer links over duplicated explanations. One rule should have one authoritative home.
8. Move obsolete design documents to `deprecated/` instead of leaving conflicting current guidance.

## Maintainer workflow

Repository-wide PR/commit/documentation rules live in [maintainer-workflow.md](maintainer-workflow.md). Coding agents should enter through the repository-root [AGENTS.md](../AGENTS.md), which deliberately points back to this index instead of duplicating subsystem context.

Documentation should capture decisions that are expensive to rediscover: architectural ownership, invariants, CI/platform traps, persistence/restart semantics and deliberate exclusions. Routine code facts that are obvious from the implementation do not need to be duplicated.

For changes that span several subsystems, update each authoritative document rather than creating one large catch-all note. The purpose of this layout is to let both humans and tooling recover only the minimum context necessary for the current task.
