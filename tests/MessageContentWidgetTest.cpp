#include <QtTest>

#include <cmath>

#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QImage>
#include <QLayout>
#include <QMouseEvent>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextOption>

#include "Settings.h"
#include "backend/emoji/EmojiRegistry.h"
#include "chat-area/post/MessageContentWidget.h"
#include "options/MLOptions.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include "qsourcehighliter.h"
#endif

using namespace Mattermost;

namespace {

int renderedLineCount(QTextBrowser& browser, int width)
{
    browser.resize(width, 100);
    browser.document()->setTextWidth(width);
    browser.document()->documentLayout()->documentSize();

    int count = 0;
    for (QTextBlock block = browser.document()->begin(); block.isValid(); block = block.next()) {
        if (const QTextLayout* layout = block.layout()) {
            count += layout->lineCount();
        }
    }
    return count;
}

void showAndSettle(QWidget& widget, const QSize& size = QSize(240, 200))
{
    widget.resize(size);
    widget.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QTextImageFormat firstImageFormat(const QTextBrowser& browser)
{
    for (QTextBlock block = browser.document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
                return fragment.charFormat().toImageFormat();
            }
        }
    }
    return {};
}

qreal firstImageWidth(const QTextBrowser& browser)
{
    return firstImageFormat(browser).width();
}

qreal emojiPointSize(const QTextBrowser& browser, const QString& emoji)
{
    const int position = browser.document()->toPlainText().indexOf(emoji);
    if (position < 0) {
        return -1.0;
    }

    QTextCursor cursor(browser.document());
    cursor.setPosition(position + emoji.size());
    qreal size = cursor.charFormat().fontPointSize();
    if (size <= 0.0) {
        size = browser.document()->defaultFont().pointSizeF();
    }
    return size;
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
qreal firstRenderedLineHeight(const QTextBlock& block)
{
    const QTextLayout* layout = block.layout();
    if (!layout || layout->lineCount() == 0) {
        return -1.0;
    }
    return layout->lineAt(0).height();
}

#endif

} // namespace

class MessageContentWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void longPlainTokenWrapsAnywhere()
    {
        MessageContentWidget widget;
        widget.setMessage(QString(1000, QLatin1Char('x')));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QCOMPARE(richText->document()->defaultTextOption().wrapMode(),
                 QTextOption::WrapAtWordBoundaryOrAnywhere);
        QVERIFY2(renderedLineCount(*richText, 120) > 1,
                 "A long unbroken normal-text token must wrap inside the message");
    }

    void oneLineHeightIsStableAfterOwnerLayoutSettles()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("short message"));
        showAndSettle(widget, QSize(320, 120));

        auto* richText =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        // The parent layout owns the child's final width. Once that width has
        // settled, deferred document/layout work must not keep changing the
        // row height on subsequent event-loop turns.
        const int settledHeight = richText->height();
        QVERIFY(settledHeight > 0);
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
        }
        QCOMPARE(richText->height(), settledHeight);
    }

    void wrappedTextReportsSettledHeight()
    {
        MessageContentWidget widget;
        QSignalSpy geometrySpy(&widget, &MessageContentWidget::dimensionsChanged);
        widget.setMessage(QString(1000, QLatin1Char('x')));
        showAndSettle(widget, QSize(120, 200));

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QVERIFY2(richText->height() > 2 * richText->fontMetrics().height(),
                 "Wrapped text must expand the real message widget height");
        QVERIFY2(geometrySpy.count() > 0,
                 "A settled text reflow must notify the containing post about its new geometry");
    }

    void draggingExternalLinkRequestsUriDrag()
    {
        MessageContentWidget widget;
        widget.setMessage(
            QStringLiteral("[Example](https://example.com/path?q=1)"));
        showAndSettle(widget, QSize(320, 120));

        auto* richText =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        QTextCursor linkCursor =
            richText->document()->find(QStringLiteral("Example"));
        QVERIFY(!linkCursor.isNull());
        linkCursor.setPosition(linkCursor.selectionStart() + 1);
        const QPoint pressPosition =
            richText->cursorRect(linkCursor).center();

        QSignalSpy dragSpy(
            &widget, &MessageContentWidget::linkDragRequested);
        QTest::mousePress(
            richText->viewport(), Qt::LeftButton, Qt::NoModifier,
            pressPosition);

        const QPoint dragPosition =
            pressPosition + QPoint(QApplication::startDragDistance() + 8, 0);
        QMouseEvent moveEvent(
            QEvent::MouseMove,
            dragPosition,
            richText->viewport()->mapToGlobal(dragPosition),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(richText->viewport(), &moveEvent);

        QCOMPARE(dragSpy.count(), 1);
        QCOMPARE(
            dragSpy.takeFirst().at(0).toString(),
            QStringLiteral("https://example.com/path?q=1"));
    }

    void internalMentionLinkIsNotDraggable()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("[user](mattermost-user://alice)"));
        showAndSettle(widget, QSize(320, 120));

        auto* richText =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        QTextCursor linkCursor =
            richText->document()->find(QStringLiteral("user"));
        QVERIFY(!linkCursor.isNull());
        linkCursor.setPosition(linkCursor.selectionStart() + 1);
        const QPoint pressPosition =
            richText->cursorRect(linkCursor).center();

        QSignalSpy dragSpy(
            &widget, &MessageContentWidget::linkDragRequested);
        QTest::mousePress(
            richText->viewport(), Qt::LeftButton, Qt::NoModifier,
            pressPosition);

        const QPoint dragPosition =
            pressPosition + QPoint(QApplication::startDragDistance() + 8, 0);
        QMouseEvent moveEvent(
            QEvent::MouseMove,
            dragPosition,
            richText->viewport()->mapToGlobal(dragPosition),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(richText->viewport(), &moveEvent);

        QCOMPARE(dragSpy.count(), 0);
    }

    void richTextBackgroundLetsPostHoverShowThrough()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("hover me"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QVERIFY2(!richText->viewport()->autoFillBackground(),
                 "The rich-text viewport must not cover the containing post hover background");
        QVERIFY2(richText->styleSheet().isEmpty(),
                 "Rich text transparency must not rely on a per-widget style sheet");
    }

    void markdownListRemainsStructuralAfterMaterialization()
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("- first\n- second"));
        showAndSettle(widget, QSize(480, 120));

        auto* richText =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        const QTextBlock first = richText->document()->firstBlock();
        const QTextBlock second = first.next();
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY2(first.textList(), qPrintable(richText->document()->toHtml()));
        QVERIFY2(second.textList(), qPrintable(richText->document()->toHtml()));
        QCOMPARE(second.textList(), first.textList());
#else
        QSKIP("Structured Markdown rendering requires Qt 5.14 or newer");
#endif
    }

    void blockquoteEmbeddedHtmlRendersAsText()
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("> <b>bold</b> quoted\n\nreply"));
        showAndSettle(widget, QSize(320, 120));

        const auto browsers = widget.findChildren<QTextBrowser*>();
        QVERIFY2(!browsers.isEmpty(), "A quoted segment must produce a text browser");
        QVERIFY2(browsers.size() >= 2,
                 "Quote and reply body must both render as text browsers");

        QString allText;
        for (QTextBrowser* browser : browsers) {
            allText += browser->document()->toPlainText();
        }

        // The embedded HTML must be interpreted, not shown as escaped markup.
        QVERIFY2(allText.contains(QStringLiteral("bold quoted")),
                 qPrintable(QStringLiteral("rendered text was: '%1'").arg(allText)));
        QVERIFY2(!allText.contains(QStringLiteral("<b>")),
                 qPrintable(QStringLiteral("escaped source leaked: '%1'").arg(allText)));
        QVERIFY2(!allText.contains(QStringLiteral("&lt;b&gt;")),
                 qPrintable(QStringLiteral("escaped entity leaked: '%1'").arg(allText)));
        QVERIFY2(!allText.contains(QStringLiteral("&lt;div")),
                 qPrintable(QStringLiteral("escaped tag leaked: '%1'").arg(allText)));
        QVERIFY(allText.contains(QStringLiteral("reply")));
#else
        QSKIP("Quote HTML rendering requires Qt 5.14 or newer");
#endif
    }

    void paletteChangeDefersContentRebuild()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("theme-sensitive text"));
        showAndSettle(widget);

        QSignalSpy refreshSpy(&widget, &MessageContentWidget::paletteRefreshCompleted);
        auto* originalBrowser =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(originalBrowser != nullptr);
        QPointer<QTextBrowser> originalBrowserGuard(originalBrowser);

        QPalette firstPalette = widget.palette();
        firstPalette.setColor(QPalette::Text, Qt::red);
        widget.setPalette(firstPalette);
        QPalette secondPalette = firstPalette;
        secondPalette.setColor(QPalette::Text, Qt::blue);
        widget.setPalette(secondPalette);

        QVERIFY2(!originalBrowserGuard.isNull(),
                 "Palette propagation must not synchronously destroy message children");
        QCOMPARE(refreshSpy.count(), 0);

        QTRY_COMPARE(refreshSpy.count(), 1);
        QVERIFY2(originalBrowserGuard.isNull(),
                 "The deferred refresh should replace the old text widget afterwards");
        auto* rebuiltBrowser =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(rebuiltBrowser != nullptr);
        QCOMPARE(rebuiltBrowser->toPlainText(), QStringLiteral("theme-sensitive text"));
    }

    void inheritedTargetFontStillRebuildsRichText()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();

        QFont smallFont = QApplication::font();
        smallFont.setPointSizeF(10.0);
        fontOption->setValue(smallFont.toString());

        QWidget parent;
        MessageContentWidget widget(&parent);
        widget.setMessage(QStringLiteral(
            "A long line that should visibly require more vertical space "
            "after the chat font grows substantially."));
        parent.resize(360, 200);
        widget.resize(320, 120);
        parent.show();
        showAndSettle(widget);

        auto* beforeBrowser =
            widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(beforeBrowser);
        const qreal beforeSize = beforeBrowser->document()->defaultFont().pointSizeF();

        QSignalSpy geometrySpy(&widget, &MessageContentWidget::dimensionsChanged);
        QFont largeFont = smallFont;
        largeFont.setPointSizeF(20.0);

        // Reproduce the ordering that caused the post-row bug: QWidget font
        // inheritance reaches the message first, then the persistent option
        // notification arrives with the same target font.
        parent.setFont(largeFont);
        QApplication::processEvents();
        fontOption->setValue(largeFont.toString());

        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            auto* browser =
                widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
            return browser
                && browser->document()->defaultFont().pointSizeF() > beforeSize * 1.8;
        })(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(geometrySpy.count() > 0, 1000);

        fontOption->setValue(previousFont);
    }

    void chatFontChangesMaterializedContentLive()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();

        QFont firstFont = QApplication::font();
        firstFont.setPointSizeF(10.0);
        firstFont.setItalic(false);
        fontOption->setValue(firstFont.toString());

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("live font"));
        showAndSettle(widget);

        auto currentFont = [&widget]() {
            auto* browser =
                widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
            return browser ? browser->document()->defaultFont() : QFont();
        };

        QCOMPARE(currentFont().pointSizeF(), 10.0);
        QCOMPARE(currentFont().italic(), false);
        QSignalSpy geometrySpy(&widget, &MessageContentWidget::dimensionsChanged);

        QFont secondFont = firstFont;
        secondFont.setPointSizeF(15.0);
        secondFont.setItalic(true);
        fontOption->setValue(secondFont.toString());

        QTRY_COMPARE(currentFont().pointSizeF(), 15.0);
        QTRY_COMPARE(currentFont().italic(), true);
        QTRY_VERIFY_WITH_TIMEOUT(geometrySpy.count() > 0, 1000);

        fontOption->setValue(previousFont);
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    void markdownHeadingAndCodeScaleWithChatText()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();

        QFont smallFont = QApplication::font();
        smallFont.setPointSizeF(10.0);
        fontOption->setValue(smallFont.toString());

        MessageContentWidget widget;
        widget.setMessage(
            QStringLiteral("# Heading\nBody\n\n```cpp\nint answer = 42;\n```"));
        showAndSettle(widget, QSize(320, 260));

        struct Sizes {
            qreal heading = -1.0;
            qreal body = -1.0;
            qreal code = -1.0;
        };

        auto sizes = [&widget]() {
            Sizes result;
            auto* browser =
                widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
            if (browser) {
                QTextDocument* document = browser->document();
                document->documentLayout()->documentSize();

                QTextBlock block = document->begin();
                if (block.isValid()) {
                    result.heading = firstRenderedLineHeight(block);
                    block = block.next();
                }
                while (block.isValid() && block.text().trimmed().isEmpty()) {
                    block = block.next();
                }
                if (block.isValid()) {
                    result.body = firstRenderedLineHeight(block);
                }
            }

            auto* code =
                widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
            if (code) {
                result.code = code->font().pointSizeF();
            }
            return result;
        };

        const Sizes before = sizes();
        QVERIFY(before.heading > before.body);
        QVERIFY(before.body > 0.0);
        QVERIFY(before.code > 0.0);

        QFont largeFont = smallFont;
        largeFont.setPointSizeF(15.0);
        fontOption->setValue(largeFont.toString());
        Sizes after;
        QTRY_VERIFY(([&] {
            after = sizes();
            return after.body > before.body * 1.45
                && after.code > before.code * 1.45;
        })());

        // Qt represents Markdown heading sizes through a relative
        // FontSizeAdjustment rather than an absolute point size. Rendered
        // line-height ratios are font-engine dependent (notably on Fedora /
        // Qt 6.11), so protect the actual contract: the heading must remain
        // larger than body text and must grow when the chat font grows.
        QVERIFY2(after.heading > before.heading,
                 "Markdown heading must grow with the chat text font");
        QVERIFY2(after.heading > after.body,
                 "Markdown heading must remain larger than body text");
        QVERIFY2(after.code < before.code * 1.55,
                 "Fenced code should follow the same chat text scale");

        fontOption->setValue(previousFont);
    }
#endif

    void inlineUnicodeEmojiUsesLargerFont()
    {
        const QString fire = QString::fromUtf8("\xF0\x9F\x94\xA5");

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("A ") + fire + QStringLiteral(" B"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        const QString plainText = richText->document()->toPlainText();
        const int emojiPosition = plainText.indexOf(fire);
        QVERIFY(emojiPosition >= 0);

        QTextCursor textCursor(richText->document());
        textCursor.setPosition(1);
        qreal textSize = textCursor.charFormat().fontPointSize();
        if (textSize <= 0.0) {
            textSize = richText->document()->defaultFont().pointSizeF();
        }

        const qreal emojiSize = emojiPointSize(*richText, fire);

        QVERIFY(textSize > 0.0);
        QVERIFY2(emojiSize > textSize * 1.25 && emojiSize < textSize * 1.35,
                 "Inline Unicode emoji should render at approximately 1.3x the surrounding text size");
    }

    void inlineCustomEmojiUsesFontRelativeSizeAndMiddleAlignment()
    {
        const QString name = QStringLiteral("mattermost_qt_inline_custom_test");
        EmojiRegistry registry;
        registry.addCustomEmoji(
            name,
            QStringLiteral("/nonexistent/mattermost-qt-inline-test.png"));

        MessageContentWidget widget;
        widget.setEmojiRegistry(&registry);
        widget.setMessage(QStringLiteral("A :") + name + QStringLiteral(": B"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        const QTextImageFormat imageFormat = firstImageFormat(*richText);
        QVERIFY(imageFormat.isValid());
        const int expected = qRound(
            QFontMetrics(richText->document()->defaultFont()).ascent() * 1.3);
        QVERIFY2(std::abs(imageFormat.height() - expected) <= 1.0,
                 qPrintable(QStringLiteral("inline custom emoji height %1, expected %2")
                     .arg(imageFormat.height()).arg(expected)));
        QCOMPARE(imageFormat.verticalAlignment(), QTextCharFormat::AlignMiddle);
    }

    void emojiOnlyUnicodeUsesJumboFont()
    {
        const QString fire = QString::fromUtf8("\xF0\x9F\x94\xA5");

        MessageContentWidget inlineWidget;
        inlineWidget.setMessage(QStringLiteral("A ") + fire + QStringLiteral(" B"));
        showAndSettle(inlineWidget);
        auto* inlineText = inlineWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(inlineText != nullptr);

        MessageContentWidget jumboWidget;
        jumboWidget.setMessage(QStringLiteral("  ") + fire + QStringLiteral("  "));
        showAndSettle(jumboWidget);
        auto* jumboText = jumboWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(jumboText != nullptr);

        const qreal inlineSize = emojiPointSize(*inlineText, fire);
        const qreal jumboSize = emojiPointSize(*jumboText, fire);
        QVERIFY(inlineSize > 0.0);
        QVERIFY2(jumboSize > inlineSize * 2.8,
                 "An emoji-only message should render its Unicode emoji at roughly 4em");
    }

    void emojiOnlyCustomEmojiUsesJumboImage()
    {
        const QString name = QStringLiteral("mattermost_qt_jumbo_test");
        EmojiRegistry registry;
        registry.addCustomEmoji(
            name,
            QStringLiteral("/nonexistent/mattermost-qt-jumbo-test.png"));

        MessageContentWidget inlineWidget;
        inlineWidget.setEmojiRegistry(&registry);
        inlineWidget.setMessage(QStringLiteral("A :") + name + QStringLiteral(": B"));
        showAndSettle(inlineWidget);
        auto* inlineText = inlineWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(inlineText != nullptr);

        MessageContentWidget jumboWidget;
        jumboWidget.setEmojiRegistry(&registry);
        jumboWidget.setMessage(QStringLiteral("  :") + name + QStringLiteral(":  "));
        showAndSettle(jumboWidget);
        auto* jumboText = jumboWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(jumboText != nullptr);

        const qreal inlineWidth = firstImageWidth(*inlineText);
        const QTextImageFormat jumboFormat = firstImageFormat(*jumboText);
        QVERIFY(inlineWidth > 0.0);
        QVERIFY2(jumboFormat.width() > inlineWidth * 2.8,
                 "An emoji-only custom emoji should render at roughly 4em too");
        QCOMPARE(jumboFormat.verticalAlignment(), QTextCharFormat::AlignMiddle);
    }

    void jumboCustomEmojiDoesNotUpscalePastNativeBitmap()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("small-custom.png"));
        QImage image(18, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QVERIFY(image.save(path));

        const QString name = QStringLiteral("mattermost_qt_native_cap_test");
        EmojiRegistry registry;
        registry.addCustomEmoji(name, path);

        MessageContentWidget widget;
        widget.setEmojiRegistry(&registry);
        widget.setMessage(QLatin1Char(':') + name + QLatin1Char(':'));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        const QTextImageFormat imageFormat = firstImageFormat(*richText);
        QCOMPARE(qRound(imageFormat.width()), 18);
        QCOMPARE(qRound(imageFormat.height()), 12);
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    void longInlineCodeWrapsAnywhere()
    {
        MessageContentWidget widget;
        const QString token(1000, QLatin1Char('a'));
        widget.setMessage(QLatin1Char('`') + token + QLatin1Char('`'));
        showAndSettle(widget);

        QVERIFY(widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock")) == nullptr);
        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QCOMPARE(richText->document()->defaultTextOption().wrapMode(),
                 QTextOption::WrapAtWordBoundaryOrAnywhere);
        QVERIFY2(renderedLineCount(*richText, 120) > 1,
                 "Inline code must wrap instead of widening the whole chat window");
    }

    void fencedCodeGetsOwnHorizontalScrollArea()
    {
        MessageContentWidget widget;
        const QString longLine = QStringLiteral("const char *value = \"")
            + QString(1000, QLatin1Char('x')) + QStringLiteral("\";");
        widget.setMessage(QStringLiteral("```cpp\n") + longLine + QStringLiteral("\n```"));
        showAndSettle(widget, QSize(220, 200));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->lineWrapMode(), QPlainTextEdit::NoWrap);
        QCOMPARE(codeBlock->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QCOMPARE(codeBlock->property("codeLanguage").toString(), QStringLiteral("cpp"));
        QCOMPARE(codeBlock->property("sourceHighliteLanguage").toInt(),
                 static_cast<int>(QSourceHighlite::QSourceHighliter::CodeCpp));

        codeBlock->resize(180, codeBlock->height());
        QCoreApplication::processEvents();
        QVERIFY2(codeBlock->horizontalScrollBar()->maximum() > 0,
                 "A long code line must scroll inside its own code block");
        QVERIFY2(widget.minimumSizeHint().width() < 180,
                 "Code content must not impose its unwrapped width on the parent message");
    }

    void multilineCodeKeepsAllLinesVisibleAboveScrollbar()
    {
        MessageContentWidget widget;
        const QString longLine(800, QLatin1Char('x'));
        widget.setMessage(QStringLiteral("```cpp\nline one\n") + longLine
                          + QStringLiteral("\nline three\nline four\nline five\n```"));
        showAndSettle(widget, QSize(220, 300));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        codeBlock->resize(180, codeBlock->height());
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(codeBlock->document()->blockCount(), 5);
        QVERIFY2(codeBlock->horizontalScrollBar()->maximum() > 0,
                 "The long code line must produce a local horizontal scrollbar");
        const int textHeight = codeBlock->document()->blockCount()
            * codeBlock->fontMetrics().lineSpacing();
        QVERIFY2(codeBlock->viewport()->height() >= textHeight,
                 "The code viewport must be tall enough to show every code line, not only the scrollbar");
    }

    void fencedJsonSelectsJsonHighlighter()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```json\n{\"answer\": 42}\n```"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->property("sourceHighliteLanguage").toInt(),
                 static_cast<int>(QSourceHighlite::QSourceHighliter::CodeJSON));
    }

    void unknownLanguageStillGetsCodeWidget()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```made-up-language\nabcdef\n```"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->property("codeLanguage").toString(), QStringLiteral("made-up-language"));
        QVERIFY(!codeBlock->property("sourceHighliteLanguage").isValid());
    }

    void promotedMultilineBackticksUseCodeWidget()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("`first line\nsecond line\nthird line`"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->toPlainText(), QStringLiteral("first line\nsecond line\nthird line"));
    }

    void monospaceFontSettingChangesCodeFont()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();
        auto* monoOption = MLOptions::instance()->optionObject<QString>(
            CHAT_MONOSPACE_FONT, QString());
        const QString previousMono = monoOption->value().toString();

        const QString fixedFamily =
            QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

        QFont chatFont = QApplication::font();
        chatFont.setPointSizeF(10.0);
        fontOption->setValue(chatFont.toString());

        QFont codeFont(fixedFamily);
        codeFont.setPointSizeF(14.0);
        monoOption->setValue(codeFont.toString());

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral(
            "Body `inlineCode` here\n\n```cpp\nint answer = 42;\n```"));
        showAndSettle(widget, QSize(320, 260));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QVERIFY2(codeBlock->font().family().compare(fixedFamily, Qt::CaseInsensitive) == 0,
                 qPrintable(QStringLiteral("fenced code must use the chosen monospace family (%1), got %2")
                     .arg(fixedFamily, codeBlock->font().family())));
        QVERIFY2(std::abs(codeBlock->font().pointSizeF() - 14.0) <= 1.0,
                 qPrintable(QStringLiteral("fenced code must use the chosen size, got %1")
                     .arg(codeBlock->font().pointSizeF())));

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        const int pos = richText->document()->toPlainText().indexOf(QStringLiteral("inlineCode"));
        QVERIFY2(pos >= 0, "inline code token must be rendered");
        QTextCursor cursor(richText->document());
        cursor.setPosition(pos + 1);
        const QTextCharFormat format = cursor.charFormat();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QStringList families = format.fontFamilies().toStringList();
        const bool matchesFamily = std::any_of(
            families.cbegin(), families.cend(),
            [&fixedFamily](const QString& f) {
            return f.compare(fixedFamily, Qt::CaseInsensitive) == 0;
        });
#else
        const bool matchesFamily = format.fontFamily().compare(fixedFamily, Qt::CaseInsensitive) == 0;
#endif
        QVERIFY2(matchesFamily,
                 qPrintable(QStringLiteral("inline code must use the chosen monospace family (%1)")
                     .arg(fixedFamily)));
        qreal inlineSize = format.fontPointSize();
        if (inlineSize <= 0.0) {
            inlineSize = richText->document()->defaultFont().pointSizeF();
        }
        QVERIFY2(std::abs(inlineSize - 14.0) <= 2.0,
                 qPrintable(QStringLiteral("inline code must use the chosen size, got %1")
                     .arg(inlineSize)));

        monoOption->setValue(previousMono);
        fontOption->setValue(previousFont);
    }

    void monospaceFontDefaultsToSystemFixedAtChatSize()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();
        auto* monoOption = MLOptions::instance()->optionObject<QString>(
            CHAT_MONOSPACE_FONT, QString());
        const QString previousMono = monoOption->value().toString();

        const QString fixedFamily =
            QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

        monoOption->setValue(QString());

        QFont chatFont = QApplication::font();
        chatFont.setPointSizeF(12.0);
        fontOption->setValue(chatFont.toString());

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```cpp\nint answer = 42;\n```"));
        showAndSettle(widget, QSize(320, 260));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->font().family().compare(fixedFamily, Qt::CaseInsensitive), 0);
        QVERIFY2(std::abs(codeBlock->font().pointSizeF() - 12.0) <= 1.0,
                 qPrintable(QStringLiteral("unset monospace must follow the chat size, got %1")
                     .arg(codeBlock->font().pointSizeF())));

        monoOption->setValue(previousMono);
        fontOption->setValue(previousFont);
    }

    void monospaceFontChangesMaterializedContentLive()
    {
        auto* fontOption = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString());
        const QString previousFont = fontOption->value().toString();
        auto* monoOption = MLOptions::instance()->optionObject<QString>(
            CHAT_MONOSPACE_FONT, QString());
        const QString previousMono = monoOption->value().toString();

        const QString fixedFamily =
            QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

        QFont chatFont = QApplication::font();
        chatFont.setPointSizeF(10.0);
        fontOption->setValue(chatFont.toString());

        QFont firstCode(fixedFamily);
        firstCode.setPointSizeF(11.0);
        monoOption->setValue(firstCode.toString());

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```cpp\nint answer = 42;\n```"));
        showAndSettle(widget, QSize(320, 260));

        auto codeBlock = [&widget]() {
            return widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        };
        QVERIFY(codeBlock() != nullptr);
        QCOMPARE(codeBlock()->font().family().compare(fixedFamily, Qt::CaseInsensitive), 0);
        QVERIFY(std::abs(codeBlock()->font().pointSizeF() - 11.0) <= 1.0);

        QFont secondCode(fixedFamily);
        secondCode.setPointSizeF(17.0);
        QSignalSpy geometrySpy(&widget, &MessageContentWidget::dimensionsChanged);

        // The already-materialized post must update without switching tab or
        // channel, exactly like a message-font change does.
        monoOption->setValue(secondCode.toString());

        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            return codeBlock() != nullptr
                && std::abs(codeBlock()->font().pointSizeF() - 17.0) <= 1.5;
        })(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(geometrySpy.count() > 0, 1000);

        monoOption->setValue(previousMono);
        fontOption->setValue(previousFont);
    }
#endif
};

QTEST_MAIN(MessageContentWidgetTest)

#include "MessageContentWidgetTest.moc"
