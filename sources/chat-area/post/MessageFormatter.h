#pragma once

#include <QFont>
#include <QString>
#include <QtGlobal>

class QTextDocument;

namespace Mattermost {

class EmojiRegistry;
namespace MessageFormatter {

QString formatMessageText(const QString& text, EmojiRegistry* registry = nullptr);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
void buildMarkdownDocument(QTextDocument& document, const QString& text,
                           EmojiRegistry* registry = nullptr,
                           const QFont& monospaceFont = QFont());
#endif

} // namespace MessageFormatter
} // namespace Mattermost
