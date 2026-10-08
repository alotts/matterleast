# Quoted replies

Mattermost-Qt implements an inline quoted reply independently from Mattermost thread membership.

## Wire format

A quoted reply is an otherwise normal Mattermost post whose `props` object contains:

```json
{
  "mattermost_qt_reply_to_post_id": "<post-id>"
}
```

`mattermost_qt_reply_to_post_id` is a string containing the exact ID of the post being quoted. The constant is declared in `sources/backend/PostProps.h` as `PostProps::ReplyToPostId`.

The property is the authoritative structured reference. Do not infer the target from rendered text when the property is present.

For interoperability with clients that ignore unknown `props`, new Mattermost-Qt quoted replies also prepend a short Markdown fallback to `message`:

```markdown
> [Replying to Alice](/_redirect/pl/<post-id>)
> A compact excerpt of the quoted post…
>

The user's actual reply text starts here.
```

The excerpt is intentionally compact and flattened to a single Markdown quote line. The `/_redirect/pl/<post-id>` link is a Mattermost post permalink redirect and therefore remains useful in channels, DMs and group DMs without embedding a team-specific path.

Mattermost-Qt recognizes only the exact generated `> [Replying to ...] ... \n>\n\n` prefix as its interoperability fallback. It removes that prefix from the normal message renderer and edit box, then renders the structured quote from `mattermost_qt_reply_to_post_id`. Ordinary user-authored blockquotes are never stripped.

When editing a quoted reply, Mattermost-Qt keeps the existing fallback prefix on the wire while presenting only the user's reply body in the editor.

## Relationship to threads

The reply property is orthogonal to Mattermost's `root_id`:

- `root_id` keeps its native Mattermost meaning: the post belongs to a thread rooted at that post ID.
- `props.mattermost_qt_reply_to_post_id` means only: render this post with an inline quote that references the specified post.

Consequently:

- a quoted reply in a normal channel normally has an empty `root_id`;
- a quoted reply sent from a thread window keeps that thread's `root_id` and also carries `mattermost_qt_reply_to_post_id`;
- quoted reply handling must never synthesize or replace `root_id` merely to represent the quote.

This separation is required so quoted replies inside existing threads do not accidentally create nested or unrelated thread semantics.

## Client behaviour

When the user chooses **Reply** from a post context menu, the composer stores the target post ID as a dynamic property with the same key. The composer preview is UI-only; the persistent reference is written into post `props` when the message is sent.

Both composer and timeline use the compact `QuotedPostPreview` presentation: muted text, a vertical quote bar and a bounded excerpt. The timeline preview expands to the available post width and is limited to two lines rather than rendering the complete quoted post as rich message content.

When rendering a post, `PostWidget` first looks for `mattermost_qt_reply_to_post_id` in `props`. If the referenced post is already materialized in the channel it is rendered immediately. Otherwise the exact post is loaded through `PostRepository::loadPost()` and the preview is inserted after retrieval.

Clicking the rendered quote navigates to the referenced post through `ChatArea::goToPost(postId)`, so sparse timeline materialization remains centralized in the normal navigation path.

The older **Reply in thread** action is separate and continues to use Mattermost's native thread mechanism.

### Embedded HTML in quote surfaces

Quoted content frequently reaches the client from the server as raw HTML embedded in
the message body, rather than as a bare `>` blockquote line. To display that quote as
formatted text instead of escaped source, the two quote **surfaces** interpret HTML:

- `QuoteBlock` blockquote segments (`>`-prefixed content) — `MessageContentWidget`.
- `QuotedPostPreview` (the compact quoted-reply preview on both composer and timeline).

Both call `MessageFormatter::buildMarkdownDocument(..., allowHtml = true)`. Ordinary
post bodies (`addMarkdownContent`, `formatMessageText`, channel headers) keep
`MarkdownNoHTML`, so freely authored HTML/script in a normal message is still escaped,
never interpreted.

The escape is intentionally limited to visually delimited quote contexts, which lowers
the risk of confusing rendered HTML for raw source. Quote renderers share `QTextDocument`
HTML handling: no scripting is enabled for HTML content, inline images are resolved only
through the app's own emoji/attachment path (never arbitrary remote URLs), and link
navigation remains governed by each surface's existing link policy. HTML that appears
**outside** a `>` blockquote (a web-app quote embedded mid-body) is out of scope and
remains escaped. If a stricter posture is ever desired, sanitize a known tag set instead
of toggling `MarkdownNoHTML`.

## Interoperability

The property is Mattermost-Qt-specific metadata, while the Markdown prefix is the compatibility representation for standard Mattermost clients. Clients that ignore the property still show a conventional blockquote and a clickable post link, followed by the real reply body.

Posts created by early Mattermost-Qt builds that contain `mattermost_qt_reply_to_post_id` but no Markdown fallback still render correctly in Mattermost-Qt, but other clients have no quote representation for those already-stored historical posts.

Do not rename or repurpose this property or the generated fallback grammar without a migration strategy: posts already stored on the server may depend on them for restoring the quote after restart, history reload or cross-client display.
