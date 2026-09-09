/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "PostReaction.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QStringList>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "chat-area/post/ReactionChipStyle.h"
#include "ui/EmojiFont.h"
#include "ui/EmojiPresentation.h"
#include "ui_PostReaction.h"

namespace Mattermost {
namespace {

bool looksLikeMattermostId(const QString& value)
{
    if (value.size() != 26) {
        return false;
    }
    for (const QChar ch : value) {
        if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('z'))) {
            return false;
        }
    }
    return true;
}

} // namespace

PostReaction::PostReaction(Backend& backend,
                           const QString& emojiName,
                           const QString& emojiValue,
                           const BackendPostReaction& reactionData,
                           QWidget* parent)
    : QWidget(parent)
    , backend_(backend)
    , emojiName_(emojiName)
    , emojiSource_(emojiValue)
    , reactionData_(reactionData)
    , ui_(new Ui::PostReaction)
{
    ui_->setupUi(this);
    // The whole chip is one click target. QLabel children otherwise win
    // hit-testing, which made clicks on later/more tightly packed chips
    // appear to do nothing.
    ui_->emoji->setAttribute(Qt::WA_TransparentForMouseEvents);
    ui_->count->setAttribute(Qt::WA_TransparentForMouseEvents);

    const QFont reactionFont = EmojiPresentation::emojiFontForMode(
        ui_->emoji->font(), EmojiPresentation::Mode::Reaction);
    ui_->emoji->setFont(reactionFont);
    emojiValue_ = EmojiPresentation::normalizeHtml(
        emojiValue,
        reactionFont,
        EmojiPresentation::Mode::Reaction);
    ui_->emoji->setText(emojiValue_);
    ui_->count->setText(QString::number(reactionData_.size()));

    QStringList unresolvedUserIds;
    for (const QString& value : reactionData_) {
        if (looksLikeMattermostId(value)) {
            unresolvedUserIds.push_back(value);
        }
    }

    ReactionChipStyle::apply(this, ui_->horizontalLayout,
                             QStringLiteral("postReaction"));
    applyPresentationFont();

    profileLookupFinished_ = unresolvedUserIds.isEmpty();
    updateToolTip();

    if (!unresolvedUserIds.isEmpty()) {
        QPointer<PostReaction> guard(this);
        UserProfileService::instance(backend_).ensureUsers(
            unresolvedUserIds,
            [guard] {
                if (!guard) {
                    return;
                }
                guard->profileLookupFinished_ = true;
                guard->updateToolTip();
            });
    }

}

PostReaction::~PostReaction()
{
    delete ui_;
}

void PostReaction::setPresentationFont(const QFont& presentationFont)
{
    if (font() != presentationFont) {
        QWidget::setFont(presentationFont);
    }
    applyPresentationFont();
}

void PostReaction::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::FontChange) {
        applyPresentationFont();
    }
}

void PostReaction::applyPresentationFont()
{
    if (!ui_ || !ui_->count) {
        return;
    }

    const int extent = ReactionChipStyle::iconExtent(font());
    const int boxExtent = ReactionChipStyle::iconBoxExtent(font());

    // Preserve the normal chat font size for Unicode emoji. Only swap in the
    // platform emoji family where older Qt versions need it.
    const QFont reactionFont = EmojiFont::applySystemEmojiFamily(font());
    ui_->emoji->setFont(reactionFont);
    ui_->emoji->setFixedSize(boxExtent, boxExtent);

    // Custom emoji is rich-text <img>. Size the image itself to the normal chat
    // line height, while the QLabel box above provides separate clipping room.
    QFont imageSizingFont = font();
    imageSizingFont.setPixelSize(extent);
    emojiValue_ = EmojiPresentation::normalizeHtml(
        emojiSource_, imageSizingFont, EmojiPresentation::Mode::Reaction);
    ui_->emoji->setText(emojiValue_);

    ui_->count->setFont(ReactionChipStyle::countFont(font()));
    ui_->count->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    ReactionChipStyle::updateMetrics(this, ui_->horizontalLayout, font());
    updateToolTip();
    updateGeometry();
}

void PostReaction::updateToolTip()
{
    QStringList names;
    int unresolved = 0;
    for (const QString& value : reactionData_) {
        if (!looksLikeMattermostId(value)) {
            names.push_back(value);
            continue;
        }

        const BackendUser* user = backend_.getStorage().getUserById(value);
        if (!user) {
            ++unresolved;
            continue;
        }

        QString name = user->getDisplayName().trimmed();
        if (name.isEmpty() && !user->username.isEmpty()) {
            name = QLatin1Char('@') + user->username;
        }
        if (!name.isEmpty()) {
            if (user->isLoginUser) {
                name += tr(" (you)");
            }
            names.push_back(name);
        } else {
            ++unresolved;
        }
    }

    QString tooltip = emojiName_ + QStringLiteral("  ") + emojiValue_;
    for (const QString& name : names) {
        tooltip += QLatin1Char('\n') + name;
    }
    if (unresolved > 0) {
        tooltip += QLatin1Char('\n');
        tooltip += profileLookupFinished_
            ? tr("Unknown user")
            : tr("Loading user names…");
        if (profileLookupFinished_ && unresolved > 1) {
            tooltip += QStringLiteral(" (%1)").arg(unresolved);
        }
    }
    const BackendUser* loginUser = backend_.getStorage().loginUser;
    const QString loginUserId = loginUser ? loginUser->id : QString();
    const bool ownReaction = !loginUserId.isEmpty()
        && reactionData_.contains(loginUserId);
    tooltip += QLatin1Char('\n')
        + (ownReaction ? tr("Click to remove your reaction")
                       : tr("Click to add this reaction"));
    setToolTip(tooltip);
}

void PostReaction::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton) {
        emit clicked(emojiName_);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

} /* namespace Mattermost */
