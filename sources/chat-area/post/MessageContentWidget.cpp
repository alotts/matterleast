#include "MessageContentWidget.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QRunnable>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBoundaryFinder>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextOption>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include "MessageFormatter.h"
#include "Settings.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistry.h"
#include "options/MLOptions.h"
#include "ui/EmojiFont.h"
#include "ui/EmojiPresentation.h"
#include "ui/MonospaceFont.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include "qsourcehighliter.h"
#endif

namespace Mattermost {
namespace {

const QSet<QString>& unicodeEmojiStrings()
{
    static const QSet<QString> emojiStrings = [] {
        QSet<QString> result;

        for (int category = 0; category < EmojiCategory::COUNT; ++category) {
            if (category == EmojiCategory::custom) {
                continue;
            }

            const int skinToneCount = category == EmojiCategory::people
                ? EmojiSkinTone::COUNT
                : 1;
            for (int skinTone = 0; skinTone < skinToneCount; ++skinTone) {
                const QVector<Emoji> emojis = EmojiInfo::getAllBuiltInEmojis(category, skinTone);
                for (const Emoji& emoji : emojis) {
                    const QString glyph = emoji.unicodeString.trimmed();
                    if (!glyph.isEmpty() && !glyph.contains(QStringLiteral("<img"))) {
                        result.insert(glyph);
                    }
                }
            }
        }

        return result;
    }();

    return emojiStrings;
}

bool isEmojiOnlyMessage(const QString& message, EmojiRegistry* registry)
{
    if (message.trimmed().isEmpty()) {
        return false;
    }

    const QSet<QString>& emojiStrings = unicodeEmojiStrings();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, message);
    bool foundEmoji = false;
    int position = 0;

    while (position < message.size()) {
        if (message.at(position).isSpace()) {
            ++position;
            continue;
        }

        if (message.at(position) == QLatin1Char(':')) {
            const int end = message.indexOf(QLatin1Char(':'), position + 1);
            if (end > position + 1) {
                const QString name = message.mid(position + 1, end - position - 1);
                const auto emoji = registry
                    ? registry->resolveByName(name)
                    : EmojiInfo::resolveBuiltInByName(name);
                if (emoji) {
                    foundEmoji = true;
                    position = end + 1;
                    continue;
                }
            }
        }

        finder.setPosition(position);
        const int end = finder.toNextBoundary();
        if (end <= position) {
            return false;
        }

        if (!emojiStrings.contains(message.mid(position, end - position))) {
            return false;
        }
        foundEmoji = true;
        position = end;
    }

    return foundEmoji;
}

void applyEmojiPresentation(QTextDocument& document, bool jumbo,
                            const EmojiRegistry* registry)
{
    const QString text = document.toPlainText();
    if (text.isEmpty()) {
        return;
    }

    const EmojiPresentation::Mode mode = jumbo
        ? EmojiPresentation::Mode::Jumbo
        : EmojiPresentation::Mode::Inline;
    const qreal scale = EmojiPresentation::fontScale(mode);
    const QString emojiFamily = EmojiFont::legacyEmojiFontFamily();
    const QSet<QString>& emojiStrings = unicodeEmojiStrings();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toStart();

    int start = 0;
    while (true) {
        const int end = finder.toNextBoundary();
        if (end < 0) {
            break;
        }

        const QString grapheme = text.mid(start, end - start);
        if (emojiStrings.contains(grapheme)) {
            QTextCursor cursor(&document);
            cursor.setPosition(start);

            qreal pointSize = cursor.charFormat().fontPointSize();
            if (pointSize <= 0.0) {
                pointSize = document.defaultFont().pointSizeF();
            }

            if (pointSize > 0.0) {
                cursor.setPosition(end, QTextCursor::KeepAnchor);
                QTextCharFormat emojiFormat;
                emojiFormat.setFontPointSize(pointSize * scale);
                if (!emojiFamily.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    emojiFormat.setFontFamilies(QStringList {emojiFamily});
#else
                    emojiFormat.setFontFamily(emojiFamily);
#endif
                }
                cursor.mergeCharFormat(emojiFormat);
            }
        }

        start = end;
    }

    EmojiPresentation::apply(document, mode, registry);
}

bool isDraggableMessageLink(const QString& link)
{
    if (link.isEmpty()) {
        return false;
    }

    const QUrl url(link);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !scheme.isEmpty()
        && scheme != QStringLiteral("mattermost-user")
        && scheme != QStringLiteral("mattermost-group");
}

QString mattermostFileIdForImageSource(const QString& source)
{
    const QUrl url(source);
    const QString path = url.path();
    const QString prefix = QStringLiteral("/api/v4/files/");
    if (!path.startsWith(prefix)) {
        return {};
    }

    const QString fileId = path.mid(prefix.size());
    if (fileId.isEmpty() || fileId.contains(QLatin1Char('/'))) {
        return {};
    }
    return fileId;
}

class InlineImageDecodeTask final : public QRunnable
{
public:
    using Callback = std::function<void(QImage)>;

    InlineImageDecodeTask(QByteArray data, Callback callback)
        : data(std::move(data))
        , callback(std::move(callback))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QImage image = QImage::fromData(data);
        QObject* dispatcher = QCoreApplication::instance();
        if (!dispatcher) {
            return;
        }

        QMetaObject::invokeMethod(
            dispatcher,
            [callback = std::move(callback), image = std::move(image)]() mutable {
                if (callback) {
                    callback(std::move(image));
                }
            },
            Qt::QueuedConnection);
    }

private:
    QByteArray data;
    Callback callback;
};

void decodeInlineImageAsync(const QByteArray& data,
                            InlineImageDecodeTask::Callback callback)
{
    QThreadPool::globalInstance()->start(
        new InlineImageDecodeTask(data, std::move(callback)));
}

QImage fittedInlineImage(QImage image, const QTextBrowser& browser)
{
    if (image.isNull()) {
        return image;
    }

    int maxWidth = std::max(
        1,
        MLOptions::instance()
            ->optionObject<int>(
                DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
            ->value().toInt());
    const int maxHeight = std::max(
        1,
        MLOptions::instance()
            ->optionObject<int>(
                DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
            ->value().toInt());

    const int viewportWidth = browser.viewport()->width();
    if (viewportWidth > 0) {
        maxWidth = std::min(maxWidth, viewportWidth);
    }

    if (image.width() > maxWidth || image.height() > maxHeight) {
        image = image.scaled(
            QSize(maxWidth, maxHeight),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
    }
    return image;
}

class WrappedRichText final : public QTextBrowser
{
public:
    explicit WrappedRichText(std::function<void()> heightChanged,
                             EmojiRegistry* emojiRegistry,
                             QWidget* parent = nullptr)
        : QTextBrowser(parent)
        , heightChanged(std::move(heightChanged))
        , _emojiRegistry(emojiRegistry)
    {
        setObjectName(QStringLiteral("messageRichText"));
        setReadOnly(true);
        setOpenExternalLinks(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setContentsMargins(0, 0, 0, 0);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
        // Keep the viewport transparent without wrapping the QTextBrowser in
        // QStyleSheetStyle. A per-widget style sheet resolves palette roles at
        // construction time and prevents live application palette propagation.
        viewport()->setAutoFillBackground(false);
        document()->setDocumentMargin(0);
        applyWrapMode();
    }

    void setLinkDragHandler(std::function<void(const QString&)> handler)
    {
        linkDragHandler = std::move(handler);
    }

    void setContentHtml(const QString& html, bool jumboEmoji = false)
    {
        document()->setDefaultFont(font());
        setHtml(html);
        finishContent(jumboEmoji);
    }

    void setImageResource(const QUrl& resourceUrl, const QImage& image)
    {
        if (image.isNull()) {
            return;
        }

        QTextDocument* target = document();
        target->addResource(QTextDocument::ImageResource, resourceUrl, image);
        target->markContentsDirty(0, target->characterCount());
        viewport()->update();
        scheduleHeightUpdate();
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    void setContentFragment(const QTextDocumentFragment& fragment,
                            bool jumboEmoji = false)
    {
        QTextDocument* target = document();
        target->clear();
        target->setDefaultFont(font());
        target->setDocumentMargin(0);

        QTextCursor cursor(target);
        cursor.movePosition(QTextCursor::Start);
        cursor.insertFragment(fragment);
        finishContent(jumboEmoji);
    }

    void setContentMarkdown(const QString& markdown, bool jumboEmoji = false)
    {
        QTextDocument* target = document();
        target->setDefaultFont(font());
        MessageFormatter::buildMarkdownDocument(
            *target, markdown, _emojiRegistry);
        finishContent(jumboEmoji);
    }
#endif

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        dragLink.clear();
        dragConsumed = false;
        if (event && event->button() == Qt::LeftButton) {
            const QString candidate = anchorAt(event->pos());
            if (isDraggableMessageLink(candidate)) {
                dragLink = candidate;
                dragStartPosition = event->pos();
            }
        }
        QTextBrowser::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!dragLink.isEmpty() && event
            && (event->buttons() & Qt::LeftButton)
            && (event->pos() - dragStartPosition).manhattanLength()
                >= QApplication::startDragDistance()) {
            const QString link = std::exchange(dragLink, QString());
            dragConsumed = true;
            if (linkDragHandler) {
                linkDragHandler(link);
            }
            event->accept();
            return;
        }
        QTextBrowser::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        dragLink.clear();
        if (dragConsumed && event && event->button() == Qt::LeftButton) {
            dragConsumed = false;
            event->accept();
            return;
        }
        dragConsumed = false;
        QTextBrowser::mouseReleaseEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QTextBrowser::resizeEvent(event);
        // Width changes are the authoritative input for wrapped document
        // height. Resolve them synchronously so the parent PostWidget cannot be
        // measured once with QTextBrowser's transient default height and again
        // on the next event-loop turn.
        updateDocumentHeight();
    }

private:
    void finishContent(bool jumboEmoji)
    {
        document()->setDefaultFont(font());
        document()->setDocumentMargin(0);
        applyEmojiPresentation(*document(), jumboEmoji, _emojiRegistry);
        applyWrapMode();
        scheduleHeightUpdate();
    }

    void applyWrapMode()
    {
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDefaultTextOption(option);
    }

    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        if (updatingHeight || viewport()->width() <= 0) {
            return;
        }

        updatingHeight = true;
        document()->setTextWidth(viewport()->width());
        const int documentHeight =
            static_cast<int>(std::ceil(document()->size().height()));
        const int wantedHeight = std::max(
            fontMetrics().height(), documentHeight + 2 * frameWidth());
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
            if (heightChanged) {
                heightChanged();
            }
        }
        updatingHeight = false;
    }

    std::function<void()> heightChanged;
    std::function<void(const QString&)> linkDragHandler;
    QString dragLink;
    QPoint dragStartPosition;
    bool dragConsumed = false;
    bool updatingHeight = false;
    EmojiRegistry* _emojiRegistry = nullptr;
};

class QuoteBar final : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QColor barColor = palette().color(QPalette::Text);
        barColor.setAlphaF(0.50);
        painter.fillRect(rect(), barColor);
    }
};

class QuoteBlock final : public QWidget
{
public:
    QuoteBlock(const QString& markdown,
               std::function<void()> heightChanged,
               EmojiRegistry* emojiRegistry,
               QWidget* parent = nullptr)
        : QWidget(parent)
        , heightChanged(std::move(heightChanged))
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        auto* bar = new QuoteBar(this);
        bar->setFixedWidth(3);
        layout->addWidget(bar);

        text = new WrappedRichText([this] {
            updateGeometry();
            if (this->heightChanged) {
                this->heightChanged();
            }
        }, emojiRegistry, this);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        text->setContentMarkdown(markdown);
#else
        text->setContentHtml(
            MessageFormatter::formatMessageText(markdown, emojiRegistry));
#endif
        layout->addWidget(text, 1);
        updateMutedPalette();
    }

    WrappedRichText* browser() const { return text; }

    QSize sizeHint() const override
    {
        return QSize(0, text ? text->height() : fontMetrics().height());
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);
        if (event && (event->type() == QEvent::PaletteChange
                      || event->type() == QEvent::ApplicationPaletteChange)) {
            updateMutedPalette();
        }
    }

private:
    void updateMutedPalette()
    {
        if (!text) {
            return;
        }
        QPalette muted = text->palette();
        const QColor mutedText = palette().color(QPalette::Disabled, QPalette::Text);
        muted.setColor(QPalette::Text, mutedText);
        muted.setColor(QPalette::WindowText, mutedText);
        text->setPalette(muted);
        update();
    }

    WrappedRichText* text = nullptr;
    std::function<void()> heightChanged;
};

struct MessageSegment {
    bool quote = false;
    QString text;
};

int markdownFenceRun(const QString& line, QChar& fenceCharacter, int& contentStart)
{
    int position = 0;
    while (position < line.size() && position < 3 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    contentStart = position;
    if (position >= line.size()) {
        return 0;
    }

    const QChar character = line.at(position);
    if (character != QLatin1Char('`') && character != QLatin1Char('~')) {
        return 0;
    }

    int run = 0;
    while (position + run < line.size() && line.at(position + run) == character) {
        ++run;
    }
    if (run < 3) {
        return 0;
    }
    fenceCharacter = character;
    contentStart = position + run;
    return run;
}

bool extractQuoteLine(const QString& line, QString& content)
{
    int position = 0;
    while (position < line.size() && position < 3 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    if (position >= line.size() || line.at(position) != QLatin1Char('>')) {
        return false;
    }

    ++position;
    if (position < line.size() && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    content = line.mid(position);
    return true;
}

QVector<MessageSegment> splitMessageSegments(const QString& message)
{
    QVector<MessageSegment> result;
    const QStringList lines = message.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    bool inFence = false;
    QChar fenceCharacter;
    int fenceLength = 0;

    auto append = [&result](bool quote, const QString& line) {
        if (result.isEmpty() || result.back().quote != quote) {
            result.push_back(MessageSegment {quote, line});
        } else {
            result.back().text += QLatin1Char('\n');
            result.back().text += line;
        }
    };

    for (const QString& line : lines) {
        QString renderedLine = line;

        QChar candidateCharacter;
        int afterFence = 0;
        const int candidateLength = markdownFenceRun(line, candidateCharacter, afterFence);

        if (inFence) {
            append(false, renderedLine);
            if (candidateLength >= fenceLength && candidateCharacter == fenceCharacter
                && line.mid(afterFence).trimmed().isEmpty()) {
                inFence = false;
                fenceCharacter = QChar();
                fenceLength = 0;
            }
            continue;
        }

        if (candidateLength >= 3) {
            inFence = true;
            fenceCharacter = candidateCharacter;
            fenceLength = candidateLength;
            append(false, renderedLine);
            continue;
        }

        append(extractQuoteLine(line, renderedLine), renderedLine);
    }

    return result;
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)

using SourceLanguage = QSourceHighlite::QSourceHighliter::Language;

std::optional<SourceLanguage> sourceLanguageForName(QString language)
{
    language = language.trimmed().toLower();
    const int whitespace = [&language] {
        const int space = language.indexOf(QLatin1Char(' '));
        const int tab = language.indexOf(QLatin1Char('\t'));
        if (space < 0) {
            return tab;
        }
        if (tab < 0) {
            return space;
        }
        return std::min(space, tab);
    }();
    if (whitespace >= 0) {
        language.truncate(whitespace);
    }

    if (language == QLatin1String("cpp") || language == QLatin1String("c++")
        || language == QLatin1String("cxx") || language == QLatin1String("cc")) {
        return SourceLanguage::CodeCpp;
    }
    if (language == QLatin1String("c")) {
        return SourceLanguage::CodeC;
    }
    if (language == QLatin1String("js") || language == QLatin1String("javascript")) {
        return SourceLanguage::CodeJs;
    }
    if (language == QLatin1String("bash") || language == QLatin1String("sh")
        || language == QLatin1String("shell")) {
        return SourceLanguage::CodeBash;
    }
    if (language == QLatin1String("php")) {
        return SourceLanguage::CodePHP;
    }
    if (language == QLatin1String("qml")) {
        return SourceLanguage::CodeQML;
    }
    if (language == QLatin1String("py") || language == QLatin1String("python")) {
        return SourceLanguage::CodePython;
    }
    if (language == QLatin1String("rs") || language == QLatin1String("rust")) {
        return SourceLanguage::CodeRust;
    }
    if (language == QLatin1String("java")) {
        return SourceLanguage::CodeJava;
    }
    if (language == QLatin1String("cs") || language == QLatin1String("c#")
        || language == QLatin1String("csharp")) {
        return SourceLanguage::CodeCSharp;
    }
    if (language == QLatin1String("go")) {
        return SourceLanguage::CodeGo;
    }
    if (language == QLatin1String("v")) {
        return SourceLanguage::CodeV;
    }
    if (language == QLatin1String("sql")) {
        return SourceLanguage::CodeSQL;
    }
    if (language == QLatin1String("json")) {
        return SourceLanguage::CodeJSON;
    }
    if (language == QLatin1String("xml") || language == QLatin1String("html")) {
        return SourceLanguage::CodeXML;
    }
    if (language == QLatin1String("css")) {
        return SourceLanguage::CodeCSS;
    }
    if (language == QLatin1String("ts") || language == QLatin1String("typescript")) {
        return SourceLanguage::CodeTypeScript;
    }
    if (language == QLatin1String("yaml") || language == QLatin1String("yml")) {
        return SourceLanguage::CodeYAML;
    }
    if (language == QLatin1String("ini")) {
        return SourceLanguage::CodeINI;
    }
    if (language == QLatin1String("vex")) {
        return SourceLanguage::CodeVex;
    }
    if (language == QLatin1String("cmake")) {
        return SourceLanguage::CodeCMake;
    }
    if (language == QLatin1String("make") || language == QLatin1String("makefile")) {
        return SourceLanguage::CodeMake;
    }
    if (language == QLatin1String("asm") || language == QLatin1String("assembly")) {
        return SourceLanguage::CodeAsm;
    }
    if (language == QLatin1String("lua")) {
        return SourceLanguage::CodeLua;
    }
    if (language == QLatin1String("rhai")) {
        return SourceLanguage::CodeRhai;
    }

    return std::nullopt;
}

class CodeBlockEdit final : public QPlainTextEdit
{
public:
    CodeBlockEdit(const QString& code,
                  const QString& language,
                  const QFont& codeFont,
                  std::function<void()> heightChanged,
                  QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
        , heightChanged(std::move(heightChanged))
    {
        setObjectName(QStringLiteral("messageCodeBlock"));
        setProperty("codeLanguage", language);
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setFont(codeFont);
        document()->setDefaultFont(codeFont);
        document()->setDocumentMargin(6);

        QPalette codePalette = palette();
        codePalette.setColor(QPalette::Base, QColor(39, 40, 34));
        codePalette.setColor(QPalette::Text, QColor(227, 226, 214));
        setPalette(codePalette);

        setPlainText(code);

        if (const auto sourceLanguage = sourceLanguageForName(language)) {
            auto* highlighter = new QSourceHighlite::QSourceHighliter(
                document(), QSourceHighlite::QSourceHighliter::Themes::Monokai);
            highlighter->setCurrentLanguage(*sourceLanguage);
            highlighter->rehighlight();
            setProperty("sourceHighliteLanguage", static_cast<int>(*sourceLanguage));
        }

        scheduleHeightUpdate();
    }

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QPlainTextEdit::resizeEvent(event);
        scheduleHeightUpdate();
    }

private:
    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        const int lineHeight = fontMetrics().lineSpacing();
        const int lineCount = std::max(1, document()->blockCount());
        const int documentMargins =
            static_cast<int>(std::ceil(document()->documentMargin() * 2.0));
        const int scrollBarHeight = horizontalScrollBar()->maximum() > horizontalScrollBar()->minimum()
            ? horizontalScrollBar()->sizeHint().height()
            : 0;
        const QMargins widgetMargins = contentsMargins();
        const int wantedHeight = lineCount * lineHeight + documentMargins
            + widgetMargins.top() + widgetMargins.bottom()
            + 2 * frameWidth() + scrollBarHeight;
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
            if (heightChanged) {
                heightChanged();
            }
        }
    }

    std::function<void()> heightChanged;
};

bool isCodeBlock(const QTextBlock& block)
{
    const QTextBlockFormat format = block.blockFormat();
    return format.nonBreakableLines()
        || format.hasProperty(QTextFormat::BlockCodeFence)
        || format.hasProperty(QTextFormat::BlockCodeLanguage);
}

QString codeLanguage(const QTextBlock& block)
{
    return block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
}

QTextDocumentFragment fragmentForRange(QTextDocument& document, int start, int end)
{
    if (end <= start) {
        return {};
    }

    QTextCursor cursor(&document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    return QTextDocumentFragment(cursor);
}

#endif

QString formatRichTextForFont(const QString& message,
                              const QFont& font,
                              const QFont& monospaceFont)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QTextDocument document;
    document.setDefaultFont(font);
    MessageFormatter::buildMarkdownDocument(document, message, monospaceFont);
    return document.toHtml();
#else
    Q_UNUSED(font);
    Q_UNUSED(monospaceFont);
    return MessageFormatter::formatMessageText(message);
#endif
}

} // namespace

MessageContentWidget::MessageContentWidget(QWidget* parent)
    : QWidget(parent)
    , contentLayout(new QVBoxLayout(this))
{
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setMinimumWidth(0);

    auto* chatFontOption = MLOptions::instance()->optionObject<QString>(
        CHAT_FONT, font().toString());
    monospaceFont_ = MonospaceFont::resolved(font());
    auto* monospaceFontOption = MLOptions::instance()->optionObject<QString>(
        CHAT_MONOSPACE_FONT, QString());
    applyChatFont(chatFontOption->value().toString());
    connect(chatFontOption, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        applyChatFont(value.toString());
    });
    connect(monospaceFontOption, &MLOptionObject::changed, this,
            [this](const QVariant&) {
        // Recompute before rebuilding: the code font has its own family and
        // point size and is not otherwise refreshed by applyChatFont.
        monospaceFont_ = MonospaceFont::resolved(font());
        if (!_sourceMessage.isEmpty()) {
            const QString sourceMessage = _sourceMessage;
            setMessage(sourceMessage);
        }
    });

}

void MessageContentWidget::setEmojiRegistry(EmojiRegistry* registry)
{
    if (_emojiRegistry == registry) {
        return;
    }

    if (_emojiAddedConnection) {
        disconnect(_emojiAddedConnection);
        _emojiAddedConnection = {};
    }
    _emojiRegistry = registry;
    if (!_emojiRegistry) {
        return;
    }

    _emojiAddedConnection = connect(
        _emojiRegistry, &EmojiRegistry::customEmojiAdded,
        this, [this](const QString& name) {
            if (_sourceMessage.isEmpty()) {
                return;
            }
            const QString token =
                QLatin1Char(':') + name + QLatin1Char(':');
            if (_sourceMessage.contains(token)) {
                setMessage(_sourceMessage);
            }
        });
}

void MessageContentWidget::setInlineAttachmentContext(
    const QSet<QString>& imageFileIds,
    InlineImageLoader imageLoader)
{
    _inlineImageCandidates = imageFileIds;
    _inlineImageLoader = std::move(imageLoader);
    if (!_sourceMessage.isEmpty()) {
        setMessage(_sourceMessage);
    }
}

QSet<QString> MessageContentWidget::inlineAttachmentFileIds() const
{
    return _inlineAttachmentFileIds;
}

void MessageContentWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || (event->type() != QEvent::PaletteChange
                   && event->type() != QEvent::ApplicationPaletteChange)
        || paletteRefreshPending || _sourceMessage.isEmpty()) {
        return;
    }

    // Rebuilding the QTextBrowser/QPlainTextEdit children synchronously from a
    // PaletteChange handler invalidates QWidgetPrivate's palette-propagation
    // traversal. Defer and coalesce the rebuild until that traversal has
    // completed; the context object also cancels the callback on destruction.
    paletteRefreshPending = true;
    QTimer::singleShot(0, this, [this] {
        paletteRefreshPending = false;
        if (_sourceMessage.isEmpty()) {
            return;
        }

        const QString sourceMessage = _sourceMessage;
        setMessage(sourceMessage);
        emit paletteRefreshCompleted();
    });
}

void MessageContentWidget::setMessage(const QString& message)
{
    _sourceMessage = message;
    _jumboEmojiMessage = isEmojiOnlyMessage(message, _emojiRegistry);
    _inlineAttachmentFileIds.clear();
    clearContent();

    if (message.isEmpty()) {
        setVisible(false);
        scheduleDimensionsChanged();
        return;
    }

    setVisible(true);
    const QVector<MessageSegment> segments = splitMessageSegments(message);
    for (const MessageSegment& segment : segments) {
        if (segment.quote) {
            addQuote(formatRichTextForFont(segment.text, font(), monospaceFont_));
            continue;
        }
        if (segment.text.isEmpty()) {
            continue;
        }
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        addMarkdownContent(segment.text);
#else
        addRichText(MessageFormatter::formatMessageText(segment.text, _emojiRegistry));
#endif
    }
    scheduleDimensionsChanged();
}

void MessageContentWidget::clear()
{
    _sourceMessage.clear();
    _jumboEmojiMessage = false;
    _inlineAttachmentFileIds.clear();
    clearContent();
    setVisible(false);
    scheduleDimensionsChanged();
}

void MessageContentWidget::clearSelection()
{
    const auto textEdits = findChildren<QTextEdit*>();
    for (QTextEdit* edit : textEdits) {
        if (!edit) {
            continue;
        }
        QTextCursor cursor = edit->textCursor();
        cursor.setPosition(cursor.position());
        edit->setTextCursor(cursor);
    }
    const auto plainEdits = findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* edit : plainEdits) {
        if (!edit) {
            continue;
        }
        QTextCursor cursor = edit->textCursor();
        cursor.setPosition(cursor.position());
        edit->setTextCursor(cursor);
    }
}

QString MessageContentWidget::selectedText() const
{
    QStringList selections;
    for (int i = 0; i < contentLayout->count(); ++i) {
        QWidget* widget = contentLayout->itemAt(i)->widget();
        if (!widget) {
            continue;
        }
        if (const auto* browser = qobject_cast<QTextBrowser*>(widget)) {
            if (browser->textCursor().hasSelection()) {
                selections.push_back(browser->textCursor().selectedText());
            }
        } else if (const auto* editor = qobject_cast<QPlainTextEdit*>(widget)) {
            if (editor->textCursor().hasSelection()) {
                selections.push_back(editor->textCursor().selectedText());
            }
        }
        for (const auto* browser : widget->findChildren<QTextBrowser*>()) {
            if (browser->textCursor().hasSelection()) {
                selections.push_back(browser->textCursor().selectedText());
            }
        }
        for (const auto* editor : widget->findChildren<QPlainTextEdit*>()) {
            if (editor->textCursor().hasSelection()) {
                selections.push_back(editor->textCursor().selectedText());
            }
        }
    }
    return selections.join(QLatin1Char('\n'));
}

void MessageContentWidget::clearContent()
{
    while (QLayoutItem* item = contentLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void MessageContentWidget::applyChatFont(const QString& serializedFont)
{
    QFont nextFont;
    if (serializedFont.isEmpty() || !nextFont.fromString(serializedFont)) {
        nextFont = QApplication::font();
    }
    if (font() != nextFont) {
        setFont(nextFont);
    }
    // The monospace default derives its size from the chat font when the user
    // has not chosen an independent code size, so recompute it here.
    monospaceFont_ = MonospaceFont::resolved(nextFont);
    // The same target font may already have reached us through QWidget font
    // inheritance before the option notification. The rich-text documents keep
    // their own default font/height state, so the option change must still
    // rebuild materialized content.
    if (!_sourceMessage.isEmpty()) {
        const QString sourceMessage = _sourceMessage;
        setMessage(sourceMessage);
    }
}

void MessageContentWidget::scheduleDimensionsChanged()
{
    updateGeometry();
    if (dimensionsChangePending) {
        return;
    }

    dimensionsChangePending = true;
    QTimer::singleShot(0, this, [this] {
        dimensionsChangePending = false;
        updateGeometry();
        if (parentWidget()) {
            parentWidget()->updateGeometry();
        }
        emit dimensionsChanged();
    });
}

void MessageContentWidget::addQuote(const QString& markdown)
{
    if (markdown.isEmpty()) {
        return;
    }

    auto* quote = new QuoteBlock(
        markdown,
        [this] { scheduleDimensionsChanged(); },
        _emojiRegistry,
        this);
    quote->browser()->setLinkDragHandler([this](const QString& link) {
        emit linkDragRequested(link);
    });
    resolveInlineImages(quote->browser());
    connect(quote->browser(),
            QOverload<const QUrl&>::of(&QTextBrowser::highlighted),
            this,
            [this](const QUrl& url) {
                emit linkHovered(url.toString());
            });
    contentLayout->addWidget(quote);
}

void MessageContentWidget::addRichText(const QString& html)
{
    if (html.isEmpty()) {
        return;
    }

    auto* richText = new WrappedRichText(
        [this] { scheduleDimensionsChanged(); }, _emojiRegistry, this);
    richText->setLinkDragHandler([this](const QString& link) {
        emit linkDragRequested(link);
    });
    richText->setContentHtml(html, _jumboEmojiMessage);
    resolveInlineImages(richText);
    connect(richText,
            QOverload<const QUrl&>::of(&QTextBrowser::highlighted),
            this,
            [this](const QUrl& url) {
                emit linkHovered(url.toString());
            });
    contentLayout->addWidget(richText);
}

void MessageContentWidget::resolveInlineImages(QTextBrowser* browser)
{
    if (!browser || _inlineImageCandidates.isEmpty()) {
        return;
    }

    QSet<QString> requestedResources;
    QTextDocument* document = browser->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                continue;
            }

            const QString source =
                fragment.charFormat().toImageFormat().name();
            const QString fileId = mattermostFileIdForImageSource(source);
            if (fileId.isEmpty() || !_inlineImageCandidates.contains(fileId)) {
                continue;
            }

            _inlineAttachmentFileIds.insert(fileId);
            if (!_inlineImageLoader || requestedResources.contains(source)) {
                continue;
            }
            requestedResources.insert(source);
            requestInlineImage(browser, QUrl(source), fileId);
        }
    }
}

void MessageContentWidget::requestInlineImage(QTextBrowser* browser,
                                              const QUrl& resourceUrl,
                                              const QString& fileId,
                                              bool thumbnailFallback)
{
    if (!_inlineImageLoader || !browser || fileId.isEmpty()) {
        return;
    }

    QPointer<MessageContentWidget> self(this);
    QPointer<QTextBrowser> target(browser);
    const auto received =
        [self, target, resourceUrl, fileId, thumbnailFallback](const QByteArray& payload) {
        if (!self || !target) {
            return;
        }
        if (payload.isEmpty()) {
            if (!thumbnailFallback) {
                self->requestInlineImage(target, resourceUrl, fileId, true);
            }
            return;
        }

        decodeInlineImageAsync(
            payload,
            [self, target, resourceUrl, fileId, thumbnailFallback](QImage image) mutable {
                if (!self || !target) {
                    return;
                }
                if (image.isNull()) {
                    if (!thumbnailFallback) {
                        self->requestInlineImage(target, resourceUrl, fileId, true);
                    }
                    return;
                }

                image = fittedInlineImage(std::move(image), *target);
                auto* wrapped = static_cast<WrappedRichText*>(target.data());
                wrapped->setImageResource(resourceUrl, image);
                self->scheduleDimensionsChanged();
            });
    };

    _inlineImageLoader(fileId, thumbnailFallback, received);
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
void MessageContentWidget::addRichTextFragment(const QTextDocumentFragment& fragment)
{
    if (fragment.isEmpty()) {
        return;
    }

    auto* richText = new WrappedRichText(
        [this] { scheduleDimensionsChanged(); }, _emojiRegistry, this);
    richText->setLinkDragHandler([this](const QString& link) {
        emit linkDragRequested(link);
    });
    richText->setContentFragment(fragment, _jumboEmojiMessage);
    resolveInlineImages(richText);
    connect(richText,
            QOverload<const QUrl&>::of(&QTextBrowser::highlighted),
            this,
            [this](const QUrl& url) {
                emit linkHovered(url.toString());
            });
    contentLayout->addWidget(richText);
}

void MessageContentWidget::addMarkdownContent(const QString& message)
{
    QTextDocument document;
    document.setDefaultFont(font());
    MessageFormatter::buildMarkdownDocument(
        document, message, _emojiRegistry, monospaceFont_);

    int richStart = 0;
    QTextBlock block = document.begin();
    while (block.isValid()) {
        if (!isCodeBlock(block)) {
            block = block.next();
            continue;
        }

        const int codeStart = block.position();
        addRichTextFragment(fragmentForRange(document, richStart, codeStart));

        QString language = codeLanguage(block);
        QStringList codeLines;
        do {
            if (language.isEmpty()) {
                language = codeLanguage(block);
            }
            codeLines.push_back(block.text());
            block = block.next();
        } while (block.isValid() && isCodeBlock(block));

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        // Qt 5's Markdown parser marks one synthetic empty QTextBlock as part
        // of every fenced code block. Remove only that parser-added tail so
        // intentional blank lines inside the fence remain intact.
        if (!codeLines.isEmpty() && codeLines.constLast().isEmpty()) {
            codeLines.removeLast();
        }
#endif

        addCodeBlock(codeLines.join(QLatin1Char('\n')), language);
        richStart = block.isValid() ? block.position() : document.characterCount() - 1;
    }

    addRichTextFragment(fragmentForRange(
        document, richStart, document.characterCount() - 1));
}

void MessageContentWidget::addCodeBlock(const QString& code, const QString& language)
{
    contentLayout->addWidget(new CodeBlockEdit(
        code, language, monospaceFont_,
        [this] { scheduleDimensionsChanged(); }, this));
}
#endif

} // namespace Mattermost