# Appearance fonts (message and code)

Two typography settings live under **Settings -> Appearance** and apply to materialized post content in the chat timeline (including quoted replies, thread summaries and permalink previews rendered through `MessageContentWidget`).

## Message font

- Key: `chat/font`, an `MLOptions` value storing a serialized `QFont`.
- Controls the body font (family and point size) of post message text.
- Editable via the **Chat** group (family combo + size control).
- Changes preview live in already-materialized posts; **Cancel** restores the previously stored value.
- Markdown headings scale relative to this base font.

## Code font

- Key: `chat/monospaceFont`, an `MLOptions` value storing a serialized `QFont`.
- Controls the monospace font (family and an **independent** point size) for code inside message content:
  - fenced code blocks (`QPlainTextEdit` widgets), and
  - inline `` `code` `` spans within rich text.
- Editable via the **Code** group (a monospace-only family combo + size control). Changes preview live and **Cancel** restores the previously stored value.

### Default when no code font is configured

An empty (or unparseable) `chat/monospaceFont` means: use the platform's fixed-pitch family
(`QFontDatabase::systemFont(FixedFont)`) at the **current message-font point size**. This preserves
the historical behavior where code always followed the message text size. Once a code font is stored,
its point size is independent and no longer follows `chat/font` size changes. Setting the value back to
empty restores the derived default.

The resolution and default derivation are centralized in `sources/ui/MonospaceFont.h`
(`MonospaceFont::resolved`); the Markdown code-span restyling lives in
`MessageFormatter::applyMonospaceCodeFont` (invoked from `buildMarkdownDocument`).

## Scope boundary

Inline code in a few compact surfaces that do **not** materialize through `MessageContentWidget`
still uses Qt's implicit fixed-pitch selection and is intentionally not driven by this setting:

- `QuotedPostPreview` and `ChannelHeaderTextLabel`, which render excerpts through
  `MessageFormatter::formatMessageText`.

These already ignore the message font, so extending the code font there is deliberately out of scope.

## Invariants

- `QTextCharFormat::fontFixedPitch` remains the identity of an inline code span; restyling merges
  only family and size onto the existing format and never replaces it, so URL linkification and
  mention linkification continue to skip code spans.
- Changing the code size changes post row height; `MessageContentWidget` re-emits `dimensionsChanged`
  so `LongListWidget` re-measures.