#pragma once

#include <QString>
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QFont>
#endif

class QTextDocument;

namespace Mattermost {

class EmojiRegistry;
namespace MessageFormatter {

QString formatMessageText(const QString& text, EmojiRegistry* registry = nullptr);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
void buildMarkdownDocument(QTextDocument& document, const QString& text,
                           EmojiRegistry* registry = nullptr,
                           bool allowHtml = false);
#endif

} // namespace MessageFormatter
} // namespace Mattermost
