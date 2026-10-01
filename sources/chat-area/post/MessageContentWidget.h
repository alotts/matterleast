#pragma once

#include <functional>

#include <QByteArray>
#include <QSet>
#include <QFont>
#include <QString>
#include <QtGlobal>
#include <QWidget>

class QEvent;
class QTextBrowser;
class QTextDocumentFragment;
class QUrl;
class QVBoxLayout;

namespace Mattermost {

class EmojiRegistry;

class MessageContentWidget : public QWidget
{
    Q_OBJECT

public:
    using InlineImageDataCallback = std::function<void(const QByteArray&)>;
    using InlineImageLoader = std::function<void(
        const QString& fileId,
        bool thumbnail,
        InlineImageDataCallback callback)>;

    explicit MessageContentWidget(QWidget* parent = nullptr);

    void setEmojiRegistry(EmojiRegistry* registry);
    void setInlineAttachmentContext(const QSet<QString>& imageFileIds,
                                    InlineImageLoader imageLoader);
    QSet<QString> inlineAttachmentFileIds() const;
    void setMessage(const QString& message);
    void clear();
    QString selectedText() const;
    void clearSelection();

signals:
    void linkHovered(const QString& link);
    void linkDragRequested(const QString& link);
    void dimensionsChanged();
    void paletteRefreshCompleted();

protected:
    void changeEvent(QEvent* event) override;

private:
    void clearContent();
    void applyChatFont(const QString& serializedFont);
    void addRichText(const QString& html);
    void addQuote(const QString& markdown);
    void resolveInlineImages(QTextBrowser* browser);
    void requestInlineImage(QTextBrowser* browser,
                            const QUrl& resourceUrl,
                            const QString& fileId,
                            bool thumbnailFallback = false);
    void scheduleDimensionsChanged();

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    void addRichTextFragment(const QTextDocumentFragment& fragment);
    void addMarkdownContent(const QString& message);
    void addCodeBlock(const QString& code, const QString& language);
#endif

    QVBoxLayout* contentLayout;
    bool dimensionsChangePending = false;
    bool paletteRefreshPending = false;
    QString _sourceMessage;
    bool _jumboEmojiMessage = false;
    InlineImageLoader _inlineImageLoader;
    QSet<QString> _inlineImageCandidates;
    QSet<QString> _inlineAttachmentFileIds;
    EmojiRegistry* _emojiRegistry = nullptr;
    QMetaObject::Connection _emojiAddedConnection;
    QFont monospaceFont_;
};

} // namespace Mattermost
