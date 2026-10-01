#pragma once

#include <QFont>
#include <QString>
#include <QtGlobal>

class QTextDocument;

namespace Mattermost {
namespace MessageFormatter {

QString formatMessageText(const QString& text);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
void buildMarkdownDocument(QTextDocument& document, const QString& text,
                           const QFont& monospaceFont = QFont());
#endif

} // namespace MessageFormatter
} // namespace Mattermost
