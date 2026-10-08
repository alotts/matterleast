#include "QuotedPostPreview.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "QuotedReplyFormat.h"
#include "QuotedAttachmentSummary.h"
#include "backend/types/BackendPost.h"
#include "post/MessageFormatter.h"
#include "post/attachments/PostAttachmentList.h"

namespace Mattermost {

namespace {

// QTextBrowser still scrolls its document when both scrollbars are hidden.
// A quoted excerpt is a non-scrolling part of the surrounding chat timeline.
class FixedExcerptBrowser final : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        verticalScrollBar()->setValue(0);
        // Do not consume the wheel: let the enclosing chat viewport scroll.
        event->ignore();
    }
};

} // namespace

QuotedPostPreview::QuotedPostPreview(QWidget* parent, int maximumLinesValue)
    : QFrame(parent)
    , maximumLines(std::max(1, maximumLinesValue))
{
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(7);

    bar = new QFrame(this);
    bar->setFixedWidth(3);
    bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    bar->setBackgroundRole(QPalette::Window);
    bar->setAutoFillBackground(true);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(bar);

    contentLayout = new QVBoxLayout;
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    authorLabel = new QLabel(this);
    QFont authorFont = authorLabel->font();
    authorFont.setBold(true);
    authorLabel->setFont(authorFont);
    authorLabel->setTextFormat(Qt::PlainText);
    authorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    authorLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    messageBrowser = new FixedExcerptBrowser(this);
    messageBrowser->setObjectName(QStringLiteral("quotedPostExcerpt"));
    messageBrowser->setReadOnly(true);
    messageBrowser->setOpenLinks(false);
    messageBrowser->setOpenExternalLinks(false);
    messageBrowser->setFrameShape(QFrame::NoFrame);
    messageBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messageBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messageBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
    messageBrowser->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    messageBrowser->setFocusPolicy(Qt::NoFocus);
    messageBrowser->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    messageBrowser->setMaximumHeight(
        messageBrowser->fontMetrics().lineSpacing() * maximumLines + 2);
    messageBrowser->setContentsMargins(0, 0, 0, 0);
    messageBrowser->document()->setDocumentMargin(0);
    messageBrowser->viewport()->setAutoFillBackground(false);
    messageBrowser->viewport()->installEventFilter(this);
    // Selection changes, document reflow and Qt's hidden scrollbar handling
    // can move the visible excerpt even without a visible scrollbar.
    connect(messageBrowser->verticalScrollBar(), &QScrollBar::valueChanged,
            messageBrowser, [this](int value) {
                if (value != 0) {
                    messageBrowser->verticalScrollBar()->setValue(0);
                }
            });

    attachmentSummary = new QuotedAttachmentSummary(this);

    contentLayout->addWidget(authorLabel);
    contentLayout->addWidget(messageBrowser);
    contentLayout->addWidget(attachmentSummary);
    layout->addLayout(contentLayout, 1);

    refreshPalette();
}

void QuotedPostPreview::setPost(const BackendPost& post)
{
    setPreview(QObject::tr("Replying to %1").arg(post.getDisplayAuthorName()),
               post.message,
               !post.files.empty());

    if (attachmentSummary) {
        attachmentSummary->setFiles(post.files);
    }
}

void QuotedPostPreview::setPreview(const QString& title,
                                   const QString& message,
                                   bool hasAttachments)
{
    if (attachmentList) {
        contentLayout->removeWidget(attachmentList);
        delete attachmentList;
        attachmentList = nullptr;
    }

    authorLabel->setText(title);
    const QString visibleMessage = QuotedReplyFormat::stripFallback(message);
    if (visibleMessage.trimmed().isEmpty()) {
        fullText = hasAttachments ? QString() : QStringLiteral("[empty message]");
    } else {
        fullText = visibleMessage;
    }
    if (attachmentSummary) {
        attachmentSummary->setGenericAttachment(hasAttachments);
    }
    setToolTip(visibleMessage);
    if (messageBrowser) {
        messageBrowser->setToolTip(visibleMessage);
    }
    refreshText();
}

void QuotedPostPreview::setInteractiveAttachments(
    Backend& backend,
    const std::list<BackendFile>& files,
    const QString& authorName)
{
    if (files.empty() || !contentLayout) {
        return;
    }

    if (attachmentSummary) {
        attachmentSummary->setGenericAttachment(false);
    }

    if (attachmentList) {
        contentLayout->removeWidget(attachmentList);
        delete attachmentList;
    }

    attachmentList = new PostAttachmentList(backend, this);
    connect(attachmentList,
            &PostAttachmentList::dimensionsChanged,
            this,
            [this] {
                updateGeometry();
                emit dimensionsChanged();
            });

    for (const BackendFile& file : files) {
        attachmentList->addFile(file, authorName);
    }
    // Embedded file cards should stay clickable, but their inner list is not
    // a second scrollable timeline. Ignore wheels over its viewport so the
    // surrounding chat continues to scroll, including over download cards.
    if (auto* list = attachmentList->findChild<QListWidget*>()) {
        list->viewport()->installEventFilter(this);
    }

    // Qt::AlignLeft suppresses horizontal expansion: PostAttachmentList's
    // initial size hint comes from a very narrow, not-yet-laid-out viewport.
    // Its QText labels then wrap almost character by character, and the
    // download/open controls overlap. Let the parent preview give the list
    // its full available width; the list handles height-for-width itself.
    contentLayout->addWidget(attachmentList);
    updateGeometry();
    emit dimensionsChanged();
}

void QuotedPostPreview::setActivatedCallback(std::function<void()> callback)
{
    activatedCallback = std::move(callback);
    setCursor(activatedCallback ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (messageBrowser) {
        messageBrowser->viewport()->setCursor(
            activatedCallback ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}

void QuotedPostPreview::setLinkActivatedCallback(
    std::function<void(const QUrl&)> callback)
{
    linkActivatedCallback = std::move(callback);
}

bool QuotedPostPreview::eventFilter(QObject* watched, QEvent* event)
{
    if (event && event->type() == QEvent::Wheel && attachmentList) {
        if (auto* list = attachmentList->findChild<QListWidget*>()) {
            if (watched == list->viewport()) {
                event->ignore();
                return true;
            }
        }
    }
    if (messageBrowser && watched == messageBrowser->viewport() && event
        && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint position = mouseEvent->position().toPoint();
#else
            const QPoint position = mouseEvent->pos();
#endif
            const QString anchor = messageBrowser->anchorAt(position);
            if (!anchor.isEmpty() && linkActivatedCallback) {
                linkActivatedCallback(QUrl(anchor));
                mouseEvent->accept();
                return true;
            }
            if (activatedCallback) {
                activatedCallback();
                mouseEvent->accept();
                return true;
            }
        }
    }
    return QFrame::eventFilter(watched, event);
}

void QuotedPostPreview::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange
                  || event->type() == QEvent::FontChange)) {
        if (messageBrowser) {
            messageBrowser->setMaximumHeight(
                messageBrowser->fontMetrics().lineSpacing() * maximumLines + 2);
        }
        refreshPalette();
        refreshText();
    }
}

void QuotedPostPreview::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton && activatedCallback) {
        activatedCallback();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void QuotedPostPreview::refreshPalette()
{
    if (!authorLabel || !messageBrowser) {
        return;
    }

    const QColor textColor = palette().color(QPalette::Text);
    QColor mutedColor = palette().color(QPalette::PlaceholderText);

    if (!mutedColor.isValid() || mutedColor.rgba() == textColor.rgba()) {
        mutedColor = textColor;
        mutedColor.setAlphaF(mutedColor.alphaF() * 0.65);
    }

    QPalette mutedPalette = palette();
    mutedPalette.setColor(QPalette::Text, mutedColor);
    mutedPalette.setColor(QPalette::WindowText, mutedColor);
    authorLabel->setForegroundRole(QPalette::WindowText);
    authorLabel->setPalette(mutedPalette);
    messageBrowser->setPalette(mutedPalette);
    messageBrowser->viewport()->setAutoFillBackground(false);

    if (bar) {
        QColor barColor = textColor;
        barColor.setAlphaF(0.50);
        QPalette barPalette = bar->palette();
        barPalette.setColor(QPalette::Window, barColor);
        bar->setPalette(barPalette);
    }
}

void QuotedPostPreview::refreshText()
{
    if (!messageBrowser || fullText.isEmpty()) {
        if (messageBrowser) {
            messageBrowser->clear();
            messageBrowser->hide();
        }
        return;
    }

    messageBrowser->show();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    // Keep the parsed Markdown document intact. Serializing it through HTML and
    // feeding that HTML into QTextBrowser loses structural/soft line boundaries
    // (notably consecutive quote lines), which made the edit/reply preview
    // collapse user-visible newlines even though the composer source was intact.
    messageBrowser->document()->setDefaultFont(messageBrowser->font());
    MessageFormatter::buildMarkdownDocument(*messageBrowser->document(), fullText,
                                            nullptr, true);
#else
    messageBrowser->setHtml(MessageFormatter::formatMessageText(fullText));
#endif
    messageBrowser->document()->setDocumentMargin(0);
    messageBrowser->verticalScrollBar()->setValue(0);
}

} // namespace Mattermost
