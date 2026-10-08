#include "MessageFormatter.h"

#include <algorithm>

#include <QFont>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>

#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistry.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QPair>
#include <QRegularExpression>
#include <QStringList>
#include <QTextFragment>
#include <QVector>
#endif

namespace Mattermost {
namespace MessageFormatter {

#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
static void replaceEmojis(QString& text, EmojiRegistry* registry)
{
    int emojiStart = 0;
    int emojiEnd = 0;

    do {
        emojiStart = text.indexOf(':', emojiEnd);
        if (emojiStart == -1) {
            break;
        }

        emojiEnd = text.indexOf(':', emojiStart + 1);
        if (emojiEnd == -1) {
            break;
        }

        if (emojiEnd - emojiStart == 1) {
            ++emojiEnd;
            continue;
        }

        const int emojiNameSize = emojiEnd - emojiStart - 1;
        const QString emojiName = text.mid(emojiStart + 1, emojiNameSize);
        const auto emoji = registry
            ? registry->resolveByName(emojiName)
            : EmojiInfo::resolveBuiltInByName(emojiName);
        if (!emoji) {
            ++emojiEnd;
            continue;
        }

        text.replace(emojiStart, emojiNameSize + 2, emoji->unicodeString);
        emojiEnd = emojiStart + emoji->unicodeString.size();
    } while (emojiStart != -1);
}
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
namespace {

struct EmojiReplacement {
    int position = 0;
    int length = 0;
    Emoji emoji;
};

struct LinkReplacement {
    int position = 0;
    int length = 0;
    QString href;
};

bool isEscaped(const QString& text, int position)
{
    int backslashCount = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) {
        ++backslashCount;
    }
    return (backslashCount % 2) != 0;
}

int backtickRunLength(const QString& text, int position)
{
    int length = 0;
    while (position + length < text.size() && text.at(position + length) == QLatin1Char('`')) {
        ++length;
    }
    return length;
}

int longestBacktickRun(const QString& text)
{
    int longest = 0;
    for (int i = 0; i < text.size();) {
        if (text.at(i) != QLatin1Char('`')) {
            ++i;
            continue;
        }
        const int length = backtickRunLength(text, i);
        longest = std::max(longest, length);
        i += length;
    }
    return longest;
}

int fencedBlockEnd(const QString& text, int lineStart)
{
    if (lineStart != 0 && text.at(lineStart - 1) != QLatin1Char('\n')) {
        return -1;
    }

    int fenceStart = lineStart;
    int leadingSpaces = 0;
    while (fenceStart < text.size() && leadingSpaces < 3 && text.at(fenceStart) == QLatin1Char(' ')) {
        ++fenceStart;
        ++leadingSpaces;
    }
    if (fenceStart >= text.size()) {
        return -1;
    }

    const QChar fenceCharacter = text.at(fenceStart);
    if (fenceCharacter != QLatin1Char('`') && fenceCharacter != QLatin1Char('~')) {
        return -1;
    }

    int fenceLength = 0;
    while (fenceStart + fenceLength < text.size()
           && text.at(fenceStart + fenceLength) == fenceCharacter) {
        ++fenceLength;
    }
    if (fenceLength < 3) {
        return -1;
    }

    int nextLineStart = text.indexOf(QLatin1Char('\n'), fenceStart + fenceLength);
    if (nextLineStart == -1) {
        return text.size();
    }
    ++nextLineStart;

    while (nextLineStart < text.size()) {
        int candidate = nextLineStart;
        int closingLeadingSpaces = 0;
        while (candidate < text.size() && closingLeadingSpaces < 3
               && text.at(candidate) == QLatin1Char(' ')) {
            ++candidate;
            ++closingLeadingSpaces;
        }

        int closingLength = 0;
        while (candidate + closingLength < text.size()
               && text.at(candidate + closingLength) == fenceCharacter) {
            ++closingLength;
        }

        if (closingLength >= fenceLength) {
            const int lineEnd = text.indexOf(QLatin1Char('\n'), candidate + closingLength);
            const int contentEnd = lineEnd == -1 ? text.size() : lineEnd;
            bool onlyWhitespaceAfterFence = true;
            for (int i = candidate + closingLength; i < contentEnd; ++i) {
                if (text.at(i) != QLatin1Char(' ') && text.at(i) != QLatin1Char('\t')) {
                    onlyWhitespaceAfterFence = false;
                    break;
                }
            }
            if (onlyWhitespaceAfterFence) {
                return lineEnd == -1 ? text.size() : lineEnd + 1;
            }
        }

        const int lineEnd = text.indexOf(QLatin1Char('\n'), nextLineStart);
        if (lineEnd == -1) {
            break;
        }
        nextLineStart = lineEnd + 1;
    }

    // An unclosed Markdown fence owns the rest of the document. Keep it
    // untouched instead of trying to reinterpret backticks inside its body.
    return text.size();
}

bool startsMarkdownBlock(const QString& line)
{
    static const QRegularExpression blockStart(
        QStringLiteral(
            R"(^\s*(?:(?:[-+*]|\d{1,9}[.)])(?:\s+|$)|>(?:\s+|$)|#{1,6}(?:\s+|$)))"));
    static const QRegularExpression setextOrRule(
        QStringLiteral(R"(^\s*(?:={3,}|-{3,}|_{3,})\s*$)"));

    if (line.isEmpty()) {
        return true;
    }
    if (line.startsWith(QStringLiteral("    ")) || line.startsWith(QLatin1Char('\t'))) {
        return true;
    }
    return blockStart.match(line).hasMatch()
        || setextOrRule.match(line).hasMatch();
}

int quoteDepth(const QString& line, QString& content)
{
    int position = 0;
    while (position < line.size() && position < 3
           && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }

    int depth = 0;
    while (position < line.size() && line.at(position) == QLatin1Char('>')) {
        ++depth;
        ++position;
        if (position < line.size() && line.at(position) == QLatin1Char(' ')) {
            ++position;
        }
    }

    content = depth > 0 ? line.mid(position) : QString();
    return depth;
}

bool containsUnescapedPipe(const QString& line)
{
    for (int i = 0; i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char('|') && !isEscaped(line, i)) {
            return true;
        }
    }
    return false;
}

bool isTableDelimiter(const QString& line)
{
    static const QRegularExpression delimiter(
        QStringLiteral(
            R"(^\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*$)"));
    return delimiter.match(line).hasMatch();
}

QString preserveUserLineBreaks(const QString& text)
{
    QString result;
    result.reserve(text.size() + text.count(QLatin1Char('\n')) * 2);

    bool inTable = false;
    int position = 0;
    while (position < text.size()) {
        const int fenceEnd = fencedBlockEnd(text, position);
        if (fenceEnd != -1) {
            result += text.mid(position, fenceEnd - position);
            position = fenceEnd;
            continue;
        }

        const int newline = text.indexOf(QLatin1Char('\n'), position);
        if (newline == -1) {
            result += text.mid(position);
            break;
        }

        const QString currentLine = text.mid(position, newline - position);
        result += currentLine;

        const int nextLineEnd = text.indexOf(QLatin1Char('\n'), newline + 1);
        const QString nextLine = text.mid(
            newline + 1,
            nextLineEnd == -1 ? text.size() - newline - 1
                              : nextLineEnd - newline - 1);

        // preserveUserLineBreaks exists only to turn otherwise-soft prose
        // newlines into visible hard breaks. Structural boundaries normally
        // stay untouched. The exception is consecutive prose lines inside the
        // same blockquote: CommonMark treats their newlines as soft whitespace,
        // so add a hard-break marker without removing the quote structure.
        const bool blankBoundary = nextLine.isEmpty();
        const bool beforeFence = newline + 1 < text.size()
            && fencedBlockEnd(text, newline + 1) != -1;
        const bool tableStarts = containsUnescapedPipe(currentLine)
            && isTableDelimiter(nextLine);
        if (tableStarts) {
            inTable = true;
        }
        const bool tableBoundary = inTable
            && (containsUnescapedPipe(currentLine)
                || isTableDelimiter(currentLine));

        QString currentQuoteContent;
        QString nextQuoteContent;
        const int currentQuoteDepth = quoteDepth(currentLine, currentQuoteContent);
        const int nextQuoteDepth = quoteDepth(nextLine, nextQuoteContent);
        const bool quotedProseContinuation = currentQuoteDepth > 0
            && currentQuoteDepth == nextQuoteDepth
            && !currentQuoteContent.isEmpty()
            && !nextQuoteContent.isEmpty()
            && !startsMarkdownBlock(currentQuoteContent)
            && !startsMarkdownBlock(nextQuoteContent)
            && !containsUnescapedPipe(currentQuoteContent)
            && !containsUnescapedPipe(nextQuoteContent);

        const bool structuralBoundary =
            (startsMarkdownBlock(nextLine) && !quotedProseContinuation)
            || tableStarts || tableBoundary;

        if (!blankBoundary && !beforeFence && !structuralBoundary
            && newline + 1 < text.size()) {
            int trailingSpaces = 0;
            for (int i = result.size() - 1;
                 i >= 0 && result.at(i) == QLatin1Char(' '); --i) {
                ++trailingSpaces;
            }
            while (trailingSpaces++ < 2) {
                result += QLatin1Char(' ');
            }
        }

        if (inTable && !nextLine.isEmpty()
            && !containsUnescapedPipe(nextLine)
            && !isTableDelimiter(nextLine)) {
            inTable = false;
        }

        result += QLatin1Char('\n');
        position = newline + 1;
    }
    return result;
}

QString promoteMultilineCodeSpans(const QString& text)
{
    QString result;
    result.reserve(text.size());

    int position = 0;
    while (position < text.size()) {
        if (position == 0 || text.at(position - 1) == QLatin1Char('\n')) {
            const int fenceEnd = fencedBlockEnd(text, position);
            if (fenceEnd != -1) {
                result += text.mid(position, fenceEnd - position);
                position = fenceEnd;
                continue;
            }
        }

        if (text.at(position) != QLatin1Char('`') || isEscaped(text, position)) {
            result += text.at(position);
            ++position;
            continue;
        }

        const int delimiterLength = backtickRunLength(text, position);
        if (delimiterLength > 2) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        int closingPosition = position + delimiterLength;
        while (closingPosition < text.size()) {
            if (text.at(closingPosition) != QLatin1Char('`')) {
                ++closingPosition;
                continue;
            }

            const int closingLength = backtickRunLength(text, closingPosition);
            if (closingLength == delimiterLength && !isEscaped(text, closingPosition)) {
                break;
            }
            closingPosition += closingLength;
        }

        if (closingPosition >= text.size()) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        const int contentStart = position + delimiterLength;
        const QString content = text.mid(contentStart, closingPosition - contentStart);
        if (!content.contains(QLatin1Char('\n'))) {
            result += text.mid(position, closingPosition + delimiterLength - position);
            position = closingPosition + delimiterLength;
            continue;
        }

        // CommonMark intentionally collapses whitespace inside multiline code
        // spans. Mattermost messages in the wild also contain multiline snippets
        // wrapped in one or two backticks, so promote those spans to a fenced
        // code block before handing the text to QTextDocument's Markdown parser.
        const int fenceLength = std::max(3, longestBacktickRun(content) + 1);
        const QString fence(fenceLength, QLatin1Char('`'));
        const bool startsAtLineStart = position == 0 || text.at(position - 1) == QLatin1Char('\n');
        const int afterClosing = closingPosition + delimiterLength;
        const bool endsAtLineEnd = afterClosing == text.size() || text.at(afterClosing) == QLatin1Char('\n');

        if (!startsAtLineStart) {
            if (!result.endsWith(QLatin1Char('\n'))) {
                result += QLatin1Char('\n');
            }
            result += QLatin1Char('\n');
        }

        result += fence;
        result += QLatin1Char('\n');
        result += content;
        if (!content.endsWith(QLatin1Char('\n'))) {
            result += QLatin1Char('\n');
        }
        result += fence;

        if (!endsAtLineEnd) {
            result += QStringLiteral("\n\n");
        }

        position = afterClosing;
    }

    return result;
}

bool rangeAlreadyFormattedAsLinkOrCode(QTextDocument& document, int position, int length)
{
    QTextCursor cursor(&document);
    for (int i = 0; i < length; ++i) {
        cursor.setPosition(position + i);
        const QTextCharFormat format = cursor.charFormat();
        if (format.isAnchor() || format.fontFixedPitch()) {
            return true;
        }
    }
    return false;
}

void linkifyBareUrls(QTextDocument& document)
{
    // QTextDocument's GitHub Markdown parser handles ordinary autolinks, but
    // Qt 6.10 leaves some valid real-world URLs as plain text (notably long
    // percent-encoded paths containing '+'). Run a conservative second pass on
    // the already parsed document so Markdown links and code remain untouched.
    static const QRegularExpression urlExpression(
        QStringLiteral(R"(https?://[^\s<>"'`]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QList<LinkReplacement> replacements;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QString blockText = block.text();
        QRegularExpressionMatchIterator matches = urlExpression.globalMatch(blockText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            QString href = match.captured(0);

            // Sentence punctuation immediately after a URL is not part of the
            // address. Do not strip URL-significant dots from the middle/end of
            // a path such as a date; only obvious prose delimiters are removed.
            while (!href.isEmpty()) {
                const QChar tail = href.back();
                if (tail == QLatin1Char(',') || tail == QLatin1Char(';')
                    || tail == QLatin1Char('!') || tail == QLatin1Char('?')) {
                    href.chop(1);
                } else {
                    break;
                }
            }
            if (href.isEmpty()) {
                continue;
            }

            const int position = block.position() + static_cast<int>(match.capturedStart(0));
            const int length = href.size();
            if (rangeAlreadyFormattedAsLinkOrCode(document, position, length)) {
                continue;
            }

            replacements.push_back(LinkReplacement {position, length, href});
        }
    }

    // Formatting does not change positions, nevertheless apply from the end so
    // this remains safe if the implementation later needs textual cleanup.
    std::sort(replacements.begin(), replacements.end(),
              [](const LinkReplacement& left, const LinkReplacement& right) {
        return left.position > right.position;
    });

    for (const LinkReplacement& replacement : replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        cursor.setPosition(replacement.position + replacement.length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(replacement.href);
        format.setFontUnderline(true);
        cursor.mergeCharFormat(format);
    }
}

bool customEmojiImageFormat(const Emoji& emoji, QTextImageFormat& imageFormat)
{
    if (!emoji.unicodeString.contains(QStringLiteral("<img"))) {
        return false;
    }

    static const QRegularExpression imageExpression(
        QStringLiteral(R"(<img\s+src=["']([^"']+)["']\s+width=(\d+)\s+height=(\d+)\s*/?>)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = imageExpression.match(emoji.unicodeString);
    if (!match.hasMatch()) {
        return false;
    }

    imageFormat.setName(match.captured(1));
    imageFormat.setWidth(match.captured(2).toInt());
    imageFormat.setHeight(match.captured(3).toInt());
    return true;
}

void replaceEmojisInDocument(QTextDocument& document, EmojiRegistry* registry)
{
    static const QRegularExpression emojiExpression(QStringLiteral(R"(:([^:\s]+):)"));
    QList<EmojiReplacement> replacements;

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QString blockText = block.text();
        QRegularExpressionMatchIterator matches = emojiExpression.globalMatch(blockText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const auto emoji = registry
                ? registry->resolveByName(match.captured(1))
                : EmojiInfo::resolveBuiltInByName(match.captured(1));
            if (!emoji) {
                continue;
            }

            replacements.push_back(EmojiReplacement {
                block.position() + static_cast<int>(match.capturedStart(0)),
                static_cast<int>(match.capturedLength(0)),
                *emoji,
            });
        }
    }

    std::sort(replacements.begin(), replacements.end(), [](const EmojiReplacement& left, const EmojiReplacement& right) {
        return left.position > right.position;
    });

    for (const EmojiReplacement& replacement: replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        const QTextCharFormat textFormat = cursor.charFormat();
        cursor.setPosition(replacement.position + replacement.length, QTextCursor::KeepAnchor);

        QTextImageFormat imageFormat;
        if (customEmojiImageFormat(replacement.emoji, imageFormat)) {
            cursor.removeSelectedText();
            cursor.insertImage(imageFormat);
        } else {
            cursor.insertText(replacement.emoji.unicodeString, textFormat);
        }
    }
}

bool hasNonImageText(const QString& text)
{
    for (const QChar character: text) {
        if (!character.isSpace() && character != QChar::ObjectReplacementCharacter) {
            return true;
        }
    }
    return false;
}

bool shouldRenderImageAsBlock(const QTextImageFormat& imageFormat)
{
    constexpr qreal maxInlineImageSize = 64.0;

    const qreal width = imageFormat.width();
    const qreal height = imageFormat.height();
    if (width > maxInlineImageSize || height > maxInlineImageSize) {
        return true;
    }

    // Markdown images generally have no explicit dimensions. They are content
    // images, not emoji, and must not share a QTextLine with message text.
    return width <= 0.0 && height <= 0.0;
}

void clearBlockMargins(const QTextBlock& block)
{
    if (!block.isValid()) {
        return;
    }

    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setTopMargin(0);
    format.setBottomMargin(0);
    cursor.setBlockFormat(format);
}

void separateLargeImages(QTextDocument& document)
{
    // Split one mixed text/image block at a time and restart after each edit,
    // because QTextFragment positions are invalidated by insertBlock().
    for (;;) {
        bool changed = false;

        for (QTextBlock block = document.begin(); block.isValid() && !changed; block = block.next()) {
            const QString blockText = block.text();

            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                    continue;
                }

                const QTextImageFormat imageFormat = fragment.charFormat().toImageFormat();
                if (!shouldRenderImageAsBlock(imageFormat)) {
                    continue;
                }

                const int offset = fragment.position() - block.position();
                const bool hasTextBefore = hasNonImageText(blockText.left(offset));
                const bool hasTextAfter = hasNonImageText(blockText.mid(offset + fragment.length()));

                if (!hasTextBefore && !hasTextAfter) {
                    clearBlockMargins(block);
                    continue;
                }

                if (hasTextAfter) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position() + fragment.length());
                    cursor.insertBlock();
                }

                if (hasTextBefore) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position());
                    cursor.insertBlock();
                }

                changed = true;
                break;
            }
        }

        if (!changed) {
            break;
        }
    }

    // QLabel parses the generated HTML into another QTextDocument. Explicitly
    // zero margins on every block so the Markdown -> HTML -> RichText roundtrip
    // cannot reintroduce a large gap around image-only paragraphs.
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        clearBlockMargins(block);
    }
}

// Mutual-exclusion guard so emoji and code restyling cannot race: both run on
// the same document in this translation unit and are only invoked from
// buildMarkdownDocument.
void applyMonospaceCodeFont(QTextDocument& document, const QFont& monospaceFont)
{
    if (monospaceFont.family().isEmpty()) {
        return;
    }

    // Collect every fixed-pitch range first. Merging a char format can split or
    // merge adjacent fragments, which invalidates an in-progress block/fragment
    // iterator, so apply the edits afterwards from the end.
    QVector<QPair<int, int>> ranges;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().fontFixedPitch()) {
                ranges.push_back({fragment.position(), fragment.length()});
            }
        }
    }

    if (ranges.isEmpty()) {
        return;
    }

    for (auto it = ranges.cend(); it != ranges.cbegin();) {
        --it;
        QTextCursor cursor(&document);
        cursor.setPosition(it->first);
        cursor.setPosition(it->first + it->second, QTextCursor::KeepAnchor);
        QTextCharFormat mono;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        mono.setFontFamilies(QStringList {monospaceFont.family()});
#else
        mono.setFontFamily(monospaceFont.family());
#endif
        if (monospaceFont.pointSizeF() > 0.0) {
            mono.setFontPointSize(monospaceFont.pointSizeF());
        } else if (monospaceFont.pixelSize() > 0) {
            mono.setProperty(QTextFormat::FontPixelSize, monospaceFont.pixelSize());
        }
        cursor.mergeCharFormat(mono);
    }
}

} // namespace

void buildMarkdownDocument(QTextDocument& document, const QString& text,
                           EmojiRegistry* registry,
                           const QFont& monospaceFont, bool allowHtml)
{
    // QTextDocument::clear() is allowed to reset document-level state. Preserve
    // the caller's base font explicitly because Markdown heading sizes are
    // relative to it and chat typography can change at runtime.
    const QFont baseFont = document.defaultFont();
    document.clear();
    document.setDefaultFont(baseFont);
    document.setDocumentMargin(0);

    // Parse the original Markdown verbatim. In particular, do not HTML-escape
    // quotes or ampersands before parsing: entities inside code spans are not
    // decoded by CommonMark, which used to turn a literal `"` into &quot;.
    // Raw HTML is disabled at the parser level for ordinary user prose. Quote
    // surfaces (blockquote segments and the quoted-reply preview) opt in via
    // allowHtml because they are already visually delimited as quoted content,
    // and quoted text frequently arrives from the server as raw HTML.
    QTextDocument::MarkdownFeatures features(QTextDocument::MarkdownDialectGitHub);
    if (!allowHtml) {
        features.setFlag(QTextDocument::MarkdownNoHTML);
    }
    const QString markdown = promoteMultilineCodeSpans(text);
    document.setMarkdown(preserveUserLineBreaks(markdown), features);

    // Qt's GFM autolinker still misses some valid long percent-encoded URLs.
    // Complete only bare http(s) links after Markdown parsing so explicit links
    // and code spans keep their existing semantics.
    linkifyBareUrls(document);

    // Emoji are applied after Markdown parsing. Custom emoji are inserted as
    // QTextImageFormat objects, so enabling raw user HTML is unnecessary.
    replaceEmojisInDocument(document, registry);
    separateLargeImages(document);

    // Code spans are marked FixedPitch by the Markdown parser. Restyle them to
    // the user-selected monospace font (family and independent size) after all
    // other document edits so positions are stable.
    applyMonospaceCodeFont(document, monospaceFont);
}
#endif

QString formatMessageText(const QString& text, EmojiRegistry* registry)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QTextDocument document;
    buildMarkdownDocument(document, text, registry);
    return document.toHtml();
#else
    QString result(text.toHtmlEscaped());
    result.replace("\n", "<br>");

    int linkStart = 0;
    int linkEnd = 0;

    replaceEmojis(result, registry);

    do {
        QLatin1String lookups[2] = { QLatin1String("http://"), QLatin1String("https://") };
        QLatin1String* useLookup = nullptr;

        for (auto& lookup: lookups) {
            linkStart = result.indexOf(lookup, linkEnd);
            if (linkStart != -1) {
                useLookup = &lookup;
                break;
            }
        }

        if (!useLookup) {
            break;
        }

        for (linkEnd = linkStart + useLookup->size(); linkEnd < result.size(); ++linkEnd) {
            if (result.at(linkEnd) == ' ' || result.at(linkEnd) == '<') {
                break;
            }
        }

        const int size = linkEnd - linkStart;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        result.insert(linkEnd, "\">" + QStringRef(&result, linkStart, size) + "</a>");
#else
        const QStringView stringView(result);
        result.insert(linkEnd, "\">" + stringView.sliced(linkStart, size).toString() + "</a>");
#endif
        result.insert(linkStart, "<a href=\"");

        linkEnd += size + 15;
    } while (linkStart != -1);

    return result;
#endif
}

} // namespace MessageFormatter
} // namespace Mattermost