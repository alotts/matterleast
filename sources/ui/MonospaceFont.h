#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QString>

#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost::MonospaceFont {

// Default family for code: the platform's fixed-pitch font, with a literal
// "monospace" as a last-resort fallback when the font database reports none.
inline QString systemFixedFamily()
{
    const QString family = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    return family.trimmed().isEmpty()
        ? QStringLiteral("monospace") : family;
}

/**
 * Resolve the effective monospace font for a given chat (message) font.
 *
 * The persisted option is a serialized QFont carrying an independent family
 * and point size. When the option is empty or unparseable we preserve the
 * historical behavior: use the system fixed-pitch family at the message
 * font's size.
 */
inline QFont resolved(const QFont& chatFont)
{
    QFont result = chatFont;

    const QString serialized = MLOptions::instance()
        ->optionObject<QString>(CHAT_MONOSPACE_FONT, QString())
        ->value().toString();

    QFont configured;
    if (!serialized.isEmpty() && configured.fromString(serialized) && !configured.family().isEmpty()) {
        result.setFamily(configured.family());
        if (configured.pointSizeF() > 0.0) {
            result.setPointSizeF(configured.pointSizeF());
        } else if (configured.pixelSize() > 0) {
            result.setPixelSize(configured.pixelSize());
        }
    } else {
        result.setFamily(systemFixedFamily());
    }

    result.setFixedPitch(true);
    return result;
}

} // namespace Mattermost::MonospaceFont