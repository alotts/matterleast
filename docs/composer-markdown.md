# Markdown composer

MatterLeast edits message text as raw Mattermost Markdown. The composer does not maintain a second rich-text/WYSIWYG document and does not serialize through `QTextDocument::toMarkdown()`.

The text visible in `MessageTextEditWidget` is the same text used by drafts, post editing, retries, and post creation. Formatting controls are helpers that insert or wrap Markdown syntax; they do not introduce a second representation.

## Formatting toolbar

The toolbar appears above the composer. Its visibility is governed at all times by the persistent `composer/formattingToolbarVisible` preference stored through `MLOptions`, not by editor focus; losing focus keeps the toolbar visible according to the preference.

- `Ctrl/Cmd+Up` enables the toolbar preference and shows it.
- `Ctrl/Cmd+Down` disables the preference and hides it.
- Losing focus keeps the current toolbar visible per the preference.
- The editor context menu exposes **Show formatting toolbar** / **Hide formatting toolbar**.

The toolbar inserts these Markdown primitives:

- bold: `**text**`
- italic: `_text_`
- strikethrough: `~~text~~`
- code: the `</>` action inserts inline backticks in ordinary text, but inserts a fenced code block on a blank current line or for a multiline selection
- fenced code block: the separate block-code action always inserts triple-backtick fences around the selection
- quote: `> ` at the beginning of selected/current lines
- bulleted list: `- ` at the beginning of selected/current lines
- numbered list: sequential `1. `, `2. `, ...` prefixes
- link: `[label](https://...)`
- image: starts the normal Mattermost attachment upload immediately and inserts an inline Markdown image reference tied to that attachment

Conventional shortcuts are handled at the editor layer where practical: `Ctrl/Cmd+B`, `Ctrl/Cmd+I`, `Ctrl/Cmd+K`, and `Ctrl/Cmd+Shift+X`. Completion-popup navigation keeps priority over ordinary editor navigation.

The toolbar slot expands/collapses over 110 ms using `OutCubic` / `InCubic`, matching the interaction style used by other compact composer controls.

### Toolbar sizing and narrow windows

Formatting controls use `ThemeIconButton` for both SVG-backed actions and textual glyph actions. The toolbar font is the common optical scale: SVG-backed actions derive their rendered extent from its cap height, while textual actions such as **B**, **I**, **S**, `</>`, fenced code, and quote inherit its point/pixel size while preserving local bold/italic/strike traits. Button geometry is also derived from the same font metrics. This keeps SVGs and glyphs visually aligned across application-font and DPI changes instead of mixing unrelated fixed pixel sizes.

The Designer layout is only the construction-time ordering of formatting buttons. Once the toolbar is polished, those button items move into the shared height-for-width `FlowLayout`. If the composer becomes narrower than the combined fixed button widths, complete buttons wrap onto subsequent rows. They must never be squeezed into overlapping geometries or partially clipped merely to keep the toolbar on one row. Non-widget spacer items from the original horizontal layout are intentionally discarded during that transition.

`FlowLayout` is a header-only UI utility so lightweight widget tests that compile their dependencies directly can use the same production layout behavior without duplicating a source/link dependency.

### SVG raster cache

SVG-backed toolbar actions use the shared `SvgRasterCache` rather than asking `QIcon` to rescale an already-rasterized layer. The cache decodes each resource directly at the requested physical pixel size and keys entries by resource, logical size, physical size, and device-pixel ratio.

Cached rasters are deliberately untinted. `ThemeIconButton` applies the current palette/state color after lookup, so hover, checked, disabled, and theme variants share the same source raster. The cache is limited to 8 MiB with least-recently-used eviction, and a very-coarse hourly cleanup drops entries that have been idle for an hour.

When a button moves between screens with different DPR, its local tinted pixmap is invalidated and looked up again at the new device resolution. This keeps toolbar SVGs sharp without coupling the cache itself to composer or toolbar semantics.

## Links, images, and paste

The editor uses the standard Qt text-editor context menu. When the cursor or selection intersects a Markdown link, the menu additionally offers **Edit link…** and **Remove link**.

Pasting a single HTTP(S) URL while ordinary single-line text is selected turns the selection into `[label](url)` rather than replacing the label. Image paste remains owned by `OutgoingPostCreator` and continues to create an attachment.

The explicit image action also uses `OutgoingPostCreator` and the existing `AttachmentUploadService`; there is no second upload implementation. The editor inserts an internal reference of the form `matterleast-attachment:<attachment-id>@<index>` immediately, so the user can continue editing and can press Send without waiting for the upload. Removing an attachment removes its inline image reference and reindexes later references.

Internal attachment references are transport-only Markdown. `PostCreateService` is the final safety boundary: after the normal attachment upload has produced `file_id` values, it rewrites the internal URI to `/api/v4/files/<file_id>`. A missing, malformed, or unresolved internal reference fails locally and must never be included in an HTTP create/edit request.

### Delivered inline attachments

After delivery, the stored message contains ordinary Mattermost Markdown such as `![label](/api/v4/files/<file_id>)`; no MatterLeast-only marker is persisted on the server.

`MessageContentWidget` treats a Markdown image as a post attachment only when the referenced `file_id` is also one of that post's image files. This prevents an arbitrary image URL or an ordinary file link from claiming a post attachment. Recognition is performed on the parsed `QTextImageFormat`, so text that merely looks like image Markdown inside code is not treated as an inline attachment.

`MessageContentWidget` deliberately does not depend on the backend/network stack. It consumes an injected image-byte loader. `PostWidget` binds that loader to the existing `AttachmentService`, so the real application still uses authenticated requests, network-cache preference, and in-flight request coalescing while lightweight renderer tests stay backend-free. The loader requests the Mattermost preview first and falls back to the thumbnail. Image decoding runs off the GUI thread; the decoded image is fitted to the configured preview rectangle and current text viewport, then installed as a `QTextDocument::ImageResource` under the original `/api/v4/files/<file_id>` URL.

`PostWidget` owns the presentation contract between message content and the attachment list. It supplies both the set of image `file_id` candidates and the image loader to `MessageContentWidget`; IDs actually materialized as Markdown images are then considered claimed. `PostAttachmentList` omits claimed files, so an inline image is rendered at its Markdown position rather than appearing a second time below the message. The attachment-list shell stays hidden when every attached file was claimed inline.

When multiline plain text is pasted into an existing Markdown quote line, every inserted line after the first inherits that line's quote prefix. Nested quote prefixes such as `> > ` are preserved, CRLF input is normalized to LF, and a trailing pasted newline leaves the caret on a quoted blank line so the quote can continue naturally. Multiline replacements spanning multiple existing lines are left to the ordinary paste path rather than guessing a single quote depth.

## Message priority

Priority is independent of Markdown. Mattermost expects it in top-level `metadata.priority`, so it must never be encoded into message text or `props`.

The composer supports Standard, Important, and Urgent. Important/Urgent can request acknowledgement; Urgent can also enable persistent notifications. Priority is available only for new root posts.

Priority metadata is staged by `pending_post_id` in `PostCreateService`, so retries keep the same metadata and successful/terminal delivery clears it together with the pending request state.

## Rendering boundary

Raw-Markdown editing and the formatting toolbar remain independent of ordinary received-message formatting. The one post-aware bridge required by uploaded inline images is deliberately narrow: standard Mattermost `/api/v4/files/<file_id>` image nodes are resolved through the injected authenticated attachment loader and the same claimed file IDs are suppressed from the separate attachment list. No WYSIWYG/editor state is consumed by the message-log renderer.
