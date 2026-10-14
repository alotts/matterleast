/**
 * @file MessageTextEditWidget.cpp
 * @brief Plain-Markdown message composer with formatting helpers.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "MessageTextEditWidget.h"

#include <algorithm>
#include <cmath>

#include <QAbstractAnimation>
#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QEasingCurve>
#include <QFocusEvent>
#include <QFrame>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {

constexpr int ComposerMaximumHeight = 300;
constexpr int FormattingToolbarAnimationMs = 110;
constexpr int FormattingToolbarGap = 2;
constexpr char FormattingToolbarObjectName[] = "formattingToolbar";
constexpr char FormattingAnimationObjectName[] =
    "_matterleast_formatting_toolbar_animation";
constexpr char FormattingNormalizedProperty[] =
    "_matterleast_formatting_layout_normalized";

struct MarkdownLinkMatch {
    int start = -1;
    int end = -1;
    QString label;
    QString url;

    bool isValid() const { return start >= 0 && end > start; }
};

bool hasPrimaryModifier(Qt::KeyboardModifiers modifiers)
{
#ifdef Q_OS_MACOS
    return modifiers.testFlag(Qt::MetaModifier);
#else
    return modifiers.testFlag(Qt::ControlModifier);
#endif
}

QString normalizedHttpUrl(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()
        || QRegularExpression(QStringLiteral("\\s")).match(trimmed).hasMatch()) {
        return {};
    }

    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid()
        || (scheme != QStringLiteral("http")
            && scheme != QStringLiteral("https"))) {
        return {};
    }
    return url.toString(QUrl::FullyEncoded);
}

QString httpUrlFromMimeData(const QMimeData* source)
{
    if (!source) {
        return {};
    }

    if (source->hasUrls()) {
        const QList<QUrl> urls = source->urls();
        if (urls.size() == 1 && !urls.first().isLocalFile()) {
            const QString url = normalizedHttpUrl(urls.first().toString());
            if (!url.isEmpty()) {
                return url;
            }
        }
    }
    if (source->hasText()) {
        return normalizedHttpUrl(source->text());
    }
    return {};
}

QString escapedMarkdownLinkLabel(QString label)
{
    label.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    label.replace(QLatin1Char('['), QStringLiteral("\\["));
    label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return label;
}

MarkdownLinkMatch markdownLinkAt(const QString& text,
                                 int selectionStart,
                                 int selectionEnd)
{
    static const QRegularExpression expression(
        QStringLiteral("\\[([^\\]\\n]+)\\]\\((https?://[^)\\s]+)\\)"),
        QRegularExpression::CaseInsensitiveOption);

    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int start = match.capturedStart(0);
        const int end = match.capturedEnd(0);
        const bool intersects = selectionStart == selectionEnd
            ? selectionStart >= start && selectionStart <= end
            : selectionStart < end && selectionEnd > start;
        if (!intersects) {
            continue;
        }

        return MarkdownLinkMatch {
            start,
            end,
            match.captured(1),
            match.captured(2),
        };
    }
    return {};
}

QString markdownQuotePrefixAt(const QString& text, int position)
{
    const int textSize = static_cast<int>(text.size());
    const int clampedPosition = std::min(std::max(position, 0), textSize);

    int lineStart = 0;
    if (clampedPosition > 0) {
        const int previousNewline = text.lastIndexOf(
            QLatin1Char('\n'), clampedPosition - 1);
        if (previousNewline >= 0) {
            lineStart = previousNewline + 1;
        }
    }

    int lineEnd = text.indexOf(QLatin1Char('\n'), clampedPosition);
    if (lineEnd < 0) {
        lineEnd = textSize;
    }

    const QString line = text.mid(lineStart, lineEnd - lineStart);
    static const QRegularExpression quotePrefix(
        QStringLiteral("^([ \\t]*(?:>[ \\t]*)+)"));
    const QRegularExpressionMatch match = quotePrefix.match(line);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString quoteMultilinePaste(QString text, const QString& quotePrefix)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (quotePrefix.isEmpty() || !text.contains(QLatin1Char('\n'))) {
        return {};
    }

    text.replace(QStringLiteral("\n"),
                 QStringLiteral("\n") + quotePrefix);
    return text;
}

} // namespace

MessageTextEditWidget::MessageTextEditWidget(QWidget* parent)
    : InteractiveTextEdit(parent)
{
    setSubmitOnEnter(true);
    setAcceptRichText(false);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    auto* sendWithCtrlEnter = MLOptions::instance()->optionObject<bool>(
        COMPOSER_SEND_WITH_CTRL_ENTER,
        COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT);
    setSubmitOnCtrlEnter(sendWithCtrlEnter->value().toBool());
    connect(sendWithCtrlEnter, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        setSubmitOnCtrlEnter(value.toBool());
    });

    auto* toolbarVisible = MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT);
    connect(toolbarVisible, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        setFormattingToolbarVisible(value.toBool());
    });

    setSubmitHandler([this] { emit enterPressed(); });

    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Window);
    viewport()->setAutoFillBackground(true);
    document()->setDocumentMargin(3.0);

    connect(this, &QTextEdit::textChanged, this, [this] {
        updateHeightToContents();
        QTimer::singleShot(0, this, &MessageTextEditWidget::updateHeightToContents);
    });
    connect(document(), &QTextDocument::blockCountChanged, this,
            [this](int) { updateHeightToContents(); });
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateHeightToContents(); });

    QTimer::singleShot(0, this, [this] {
        normalizeFormattingToolbar();
        updateHeightToContents();
    });
}

MessageTextEditWidget::~MessageTextEditWidget() = default;

bool MessageTextEditWidget::formattingToolbarPreferredVisible() const
{
    return MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT)->value().toBool();
}

void MessageTextEditWidget::normalizeFormattingToolbar()
{
    if (!parentWidget()) {
        return;
    }
    QWidget* toolbar = parentWidget()->findChild<QWidget*>(
        QString::fromLatin1(FormattingToolbarObjectName));
    if (!toolbar || toolbar->property(FormattingNormalizedProperty).toBool()) {
        return;
    }

    toolbar->setProperty(FormattingNormalizedProperty, true);
    if (QWidget* container = toolbar->parentWidget()) {
        if (QLayout* layout = container->layout()) {
            layout->setSpacing(0);
        }
    }
    if (QLayout* layout = toolbar->layout()) {
        QMargins margins = layout->contentsMargins();
        margins.setBottom(std::max(margins.bottom(), FormattingToolbarGap));
        layout->setContentsMargins(margins);
    }

    toolbar->setMinimumHeight(0);
    toolbar->setMaximumHeight(0);
    toolbar->show();
    toolbar->updateGeometry();
}

void MessageTextEditWidget::animateFormattingToolbar(bool visible)
{
    normalizeFormattingToolbar();
    if (!parentWidget()) {
        return;
    }
    QWidget* toolbar = parentWidget()->findChild<QWidget*>(
        QString::fromLatin1(FormattingToolbarObjectName));
    if (!toolbar) {
        return;
    }

    if (auto* previous = toolbar->findChild<QPropertyAnimation*>(
            QString::fromLatin1(FormattingAnimationObjectName),
            Qt::FindDirectChildrenOnly)) {
        previous->stop();
        previous->deleteLater();
    }

    if (visible) {
        // The Designer connection hides the toolbar after a completed collapse.
        // Make it visible again before expanding its real layout slot.
        toolbar->show();
    }

    int currentHeight = toolbar->maximumHeight();
    if (currentHeight >= QWIDGETSIZE_MAX) {
        currentHeight = toolbar->height();
    }
    const int expandedHeight = toolbar->layout()
        ? std::max(1, toolbar->layout()->sizeHint().height())
        : std::max(1, toolbar->sizeHint().height());
    const int targetHeight = visible ? expandedHeight : 0;
    if (currentHeight == targetHeight) {
        toolbar->updateGeometry();
        emit formattingToolbarVisibilityChanged(visible);
        return;
    }

    auto* animation = new QPropertyAnimation(toolbar, "maximumHeight", toolbar);
    animation->setObjectName(QString::fromLatin1(FormattingAnimationObjectName));
    animation->setDuration(FormattingToolbarAnimationMs);
    animation->setStartValue(std::max(0, currentHeight));
    animation->setEndValue(targetHeight);
    animation->setEasingCurve(
        visible ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    connect(animation, &QPropertyAnimation::valueChanged,
            toolbar, [toolbar](const QVariant&) { toolbar->updateGeometry(); });
    connect(animation, &QPropertyAnimation::finished,
            this, [this, toolbar, visible] {
        toolbar->updateGeometry();
        emit formattingToolbarVisibilityChanged(visible);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MessageTextEditWidget::setFormattingToolbarVisible(bool visible)
{
    if (formattingToolbarVisible_ == visible) {
        return;
    }
    formattingToolbarVisible_ = visible;
    animateFormattingToolbar(visible);
}

void MessageTextEditWidget::setFormattingToolbarPreferredVisible(bool visible)
{
    MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT)->setValue(visible);
    setFormattingToolbarVisible(visible);
}

void MessageTextEditWidget::wrapMarkdownSelection(const QString& before,
                                                   const QString& after)
{
    QTextCursor cursor = textCursor();
    const int selectionStart = cursor.selectionStart();
    QString selected = cursor.selectedText();
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

    if (cursor.hasSelection()) {
        cursor.insertText(before + selected + after);
        cursor.setPosition(selectionStart + before.size());
        cursor.setPosition(selectionStart + before.size() + selected.size(),
                           QTextCursor::KeepAnchor);
    } else {
        cursor.insertText(before + after);
        cursor.setPosition(selectionStart + before.size());
    }
    setTextCursor(cursor);
}

void MessageTextEditWidget::prefixMarkdownLines(const QString& prefix,
                                                bool numbered)
{
    QTextCursor cursor = textCursor();
    const QString text = toPlainText();
    const int selectionStart = cursor.hasSelection()
        ? cursor.selectionStart() : cursor.position();
    const int selectionEnd = cursor.hasSelection()
        ? cursor.selectionEnd() : cursor.position();

    int lineStart = text.lastIndexOf(
        QLatin1Char('\n'), std::max(0, selectionStart - 1));
    lineStart = lineStart < 0 ? 0 : lineStart + 1;
    int lineEnd = text.indexOf(QLatin1Char('\n'), selectionEnd);
    if (lineEnd < 0) {
        lineEnd = text.size();
    }

    QStringList lines = text.mid(lineStart, lineEnd - lineStart)
                            .split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        lines[index].prepend(numbered
            ? QString::number(index + 1) + QStringLiteral(". ")
            : prefix);
    }

    cursor.setPosition(lineStart);
    cursor.setPosition(lineEnd, QTextCursor::KeepAnchor);
    cursor.insertText(lines.join(QLatin1Char('\n')));
    setTextCursor(cursor);
}

void MessageTextEditWidget::toggleBold()
{
    wrapMarkdownSelection(QStringLiteral("**"), QStringLiteral("**"));
}

void MessageTextEditWidget::toggleItalic()
{
    wrapMarkdownSelection(QStringLiteral("_"), QStringLiteral("_"));
}

void MessageTextEditWidget::toggleStrikeOut()
{
    wrapMarkdownSelection(QStringLiteral("~~"), QStringLiteral("~~"));
}

void MessageTextEditWidget::toggleInlineCode()
{
    wrapMarkdownSelection(QStringLiteral("`"), QStringLiteral("`"));
}

void MessageTextEditWidget::toggleCodeBlock()
{
    wrapMarkdownSelection(QStringLiteral("```\n"), QStringLiteral("\n```"));
}

void MessageTextEditWidget::toggleQuote()
{
    prefixMarkdownLines(QStringLiteral("> "));
}

void MessageTextEditWidget::toggleBulletList()
{
    prefixMarkdownLines(QStringLiteral("- "));
}

void MessageTextEditWidget::toggleNumberedList()
{
    prefixMarkdownLines(QString(), true);
}

bool MessageTextEditWidget::editLinkAtCursor()
{
    const QTextCursor current = textCursor();
    const MarkdownLinkMatch link = markdownLinkAt(
        toPlainText(), current.selectionStart(), current.selectionEnd());
    if (!link.isValid()) {
        return false;
    }

    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, tr("Edit link"), tr("URL:"), QLineEdit::Normal,
        link.url, &accepted).trimmed();
    if (!accepted || url.isEmpty()) {
        return true;
    }

    QTextCursor replacement(document());
    replacement.setPosition(link.start);
    replacement.setPosition(link.end, QTextCursor::KeepAnchor);
    replacement.insertText(QStringLiteral("[%1](%2)").arg(link.label, url));
    setTextCursor(replacement);
    return true;
}

bool MessageTextEditWidget::removeLinkAtCursor()
{
    const QTextCursor current = textCursor();
    const MarkdownLinkMatch link = markdownLinkAt(
        toPlainText(), current.selectionStart(), current.selectionEnd());
    if (!link.isValid()) {
        return false;
    }

    QTextCursor replacement(document());
    replacement.setPosition(link.start);
    replacement.setPosition(link.end, QTextCursor::KeepAnchor);
    replacement.insertText(link.label);
    setTextCursor(replacement);
    return true;
}

bool MessageTextEditWidget::selectionContainsLink() const
{
    const QTextCursor current = textCursor();
    return current.hasSelection()
        && markdownLinkAt(
            toPlainText(), current.selectionStart(), current.selectionEnd())
               .isValid();
}

void MessageTextEditWidget::insertLink()
{
    if (editLinkAtCursor()) {
        return;
    }

    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, tr("Insert link"), tr("URL:"), QLineEdit::Normal,
        QString(), &accepted).trimmed();
    if (!accepted || url.isEmpty()) {
        return;
    }

    QTextCursor cursor = textCursor();
    QString label = cursor.selectedText();
    label.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
    if (label.isEmpty()) {
        label = url;
    }

    cursor.insertText(QStringLiteral("[%1](%2)")
                          .arg(escapedMarkdownLinkLabel(label), url));
    setTextCursor(cursor);
}

void MessageTextEditWidget::insertFromMimeData(const QMimeData* source)
{
    QTextCursor cursor = textCursor();
    const QString url = httpUrlFromMimeData(source);
    if (!url.isEmpty() && cursor.hasSelection() && !selectionContainsLink()) {
        QString label = cursor.selectedText();
        label.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        if (!label.contains(QLatin1Char('\n'))) {
            cursor.insertText(QStringLiteral("[%1](%2)")
                                  .arg(escapedMarkdownLinkLabel(label), url));
            setTextCursor(cursor);
            return;
        }
    }

    if (source && source->hasText()) {
        QString selected = cursor.selectedText();
        selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        const bool replacesMultipleLines = selected.contains(QLatin1Char('\n'));
        const QString quotePrefix = replacesMultipleLines
            ? QString()
            : markdownQuotePrefixAt(toPlainText(), cursor.selectionStart());
        const QString quotedText = quoteMultilinePaste(source->text(), quotePrefix);
        if (!quotedText.isEmpty()) {
            cursor.insertText(quotedText);
            setTextCursor(cursor);
            return;
        }
    }

    if (source) {
        InteractiveTextEdit::insertFromMimeData(source);
    }
}

void MessageTextEditWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event) {
        return;
    }

    QMenu* menu = createStandardContextMenu();
    if (!menu) {
        return;
    }

    const QTextCursor current = textCursor();
    const MarkdownLinkMatch link = markdownLinkAt(
        toPlainText(), current.selectionStart(), current.selectionEnd());
    if (link.isValid()) {
        menu->addSeparator();
        QAction* editLinkAction = menu->addAction(tr("Edit link…"));
        connect(editLinkAction, &QAction::triggered,
                this, [this] { editLinkAtCursor(); });
        QAction* removeLinkAction = menu->addAction(tr("Remove link"));
        connect(removeLinkAction, &QAction::triggered,
                this, [this] { removeLinkAtCursor(); });
    }

    const bool toolbarPreferred = formattingToolbarPreferredVisible();
    menu->addSeparator();
    QAction* toolbarAction = menu->addAction(
        toolbarPreferred
            ? tr("Hide formatting toolbar")
            : tr("Show formatting toolbar"));
    connect(toolbarAction, &QAction::triggered,
            this, [this, toolbarPreferred] {
        setFormattingToolbarPreferredVisible(!toolbarPreferred);
    });

    menu->exec(event->globalPos());
    delete menu;
}

void MessageTextEditWidget::focusInEvent(QFocusEvent* event)
{
    InteractiveTextEdit::focusInEvent(event);
    setFormattingToolbarVisible(formattingToolbarPreferredVisible());
}

void MessageTextEditWidget::focusOutEvent(QFocusEvent* event)
{
    InteractiveTextEdit::focusOutEvent(event);
}

void MessageTextEditWidget::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool primaryOnly = hasPrimaryModifier(modifiers)
        && !modifiers.testFlag(Qt::ShiftModifier)
        && !modifiers.testFlag(Qt::AltModifier);
    if (primaryOnly && event->key() == Qt::Key_Up) {
        setFormattingToolbarPreferredVisible(true);
        event->accept();
        return;
    }
    if (primaryOnly && event->key() == Qt::Key_Down) {
        setFormattingToolbarPreferredVisible(false);
        event->accept();
        return;
    }

    if (!completionPopupVisible()) {
        if (event->matches(QKeySequence::Bold)) {
            toggleBold();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Italic)) {
            toggleItalic();
            event->accept();
            return;
        }
        if (hasPrimaryModifier(modifiers)
            && event->key() == Qt::Key_K
            && !modifiers.testFlag(Qt::AltModifier)) {
            insertLink();
            event->accept();
            return;
        }
        if (hasPrimaryModifier(modifiers)
            && modifiers.testFlag(Qt::ShiftModifier)
            && event->key() == Qt::Key_X) {
            toggleStrikeOut();
            event->accept();
            return;
        }

        switch (event->key()) {
        case Qt::Key_Up:
            emit upArrowPressed();
            break;
        case Qt::Key_Escape:
            emit escapePressed();
            break;
        default:
            break;
        }
    }

    InteractiveTextEdit::keyPressEvent(event);
}

void MessageTextEditWidget::resizeEvent(QResizeEvent* event)
{
    QTextEdit::resizeEvent(event);
    updateHeightToContents();
}

void MessageTextEditWidget::updateHeightToContents()
{
    if (!document() || !document()->documentLayout()) {
        return;
    }

    const QMargins margins = contentsMargins();
    const int chromeHeight = margins.top() + margins.bottom() + 2 * frameWidth();
    const int documentMargins =
        static_cast<int>(std::ceil(document()->documentMargin() * 2.0));
    const int lineHeight = fontMetrics().lineSpacing();
    const int oneLineHeight = lineHeight + documentMargins + chromeHeight;
    const int laidOutHeight = static_cast<int>(std::ceil(
        document()->documentLayout()->documentSize().height())) + chromeHeight;
    const int explicitLineHeight = std::max(1, document()->blockCount()) * lineHeight
        + documentMargins + chromeHeight;
    const int wantedHeight = std::clamp(
        std::max({oneLineHeight, laidOutHeight, explicitLineHeight}),
        oneLineHeight, ComposerMaximumHeight);

    if (height() != wantedHeight) {
        setFixedHeight(wantedHeight);
        updateGeometry();
    }
}

bool MessageTextEditWidget::hasNonEmptyText()
{
    return document()->characterCount() > 1;
}

} // namespace Mattermost
