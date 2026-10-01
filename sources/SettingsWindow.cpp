/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include "SettingsWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFontComboBox>
#include <QFontInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Settings.h"
#include "backend/emoji/EmojiDefs.h"
#include "options/MLOptions.h"
#include "ui_SettingsWindow.h"
#include "ui/MonospaceFont.h"

namespace Mattermost {
namespace {

constexpr int MinChatFontPointSize = 8;
constexpr int MaxChatFontPointSize = 30;

QFont fontFromString(const QString& serialized, const QFont& fallback)
{
    QFont result;
    return !serialized.isEmpty() && result.fromString(serialized)
        ? result : fallback;
}

int chatFontPointSize(const QFont& font)
{
    qreal pointSize = font.pointSizeF();
    if (pointSize <= 0.0) {
        pointSize = QFontInfo(font).pointSizeF();
    }
    if (pointSize <= 0.0) {
        pointSize = 10.0;
    }
    return qBound(MinChatFontPointSize, qRound(pointSize), MaxChatFontPointSize);
}

QSpinBox* makeSpinBox(QWidget* parent,
                      int minimum,
                      int maximum,
                      int value,
                      const QString& suffix = QString())
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    spin->setAccelerated(true);
    return spin;
}

QLabel* makeDescription(QWidget* parent, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

} // namespace

SettingsWindow::SettingsWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SettingsWindow)
{
    ui->setupUi(this);
    ui->imageMaxWidthValue->setValidator(new QIntValidator (10, 1000, this));
    ui->imageMaxHeightValue->setValidator(new QIntValidator (10, 1000, this));

    QString defaultDownloadDir (QStandardPaths::writableLocation (QStandardPaths::DownloadLocation));

    auto* options = MLOptions::instance();
    ui->downloadLocationValue->setText(
        options->optionObject<QString>(DOWNLOAD_LOCATION, defaultDownloadDir)
            ->value().toString());
    ui->askLocationCheckBox->setChecked(
        options->optionObject<bool>(DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)
            ->value().toBool());
    ui->imageMaxWidthValue->setText(QString::number(
        options->optionObject<int>(
            DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
            ->value().toInt()));
    ui->imageMaxHeightValue->setText(QString::number(
        options->optionObject<int>(
            DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
            ->value().toInt()));

    // The old form mixed attachment settings with one unlabeled cache-size
    // field. Keep the generated UI stable for now, but move all cache policy to
    // an explicit tab and hide that legacy row.
    ui->label_3->hide();
    ui->label_4->hide();
    ui->cacheSizeMBValue->hide();

    ui->verticalLayout->removeWidget(ui->downloads);
    ui->verticalLayout->removeItem(ui->verticalSpacer);
    delete ui->verticalSpacer;
    ui->verticalSpacer = nullptr;

    auto* tabs = new QTabWidget(this);
    ui->downloads->setParent(tabs);
    tabs->addTab(ui->downloads, tr("Attachments"));

    auto* composerPage = new QWidget(tabs);
    auto* composerLayout = new QVBoxLayout(composerPage);
    composerLayout->setContentsMargins(12, 12, 12, 12);
    composerLayout->setSpacing(8);

    sendWithCtrlEnter = new QCheckBox(
        tr("Send messages with Ctrl+Enter instead of Enter"), composerPage);
    sendWithCtrlEnter->setChecked(
        options->optionObject<bool>(COMPOSER_SEND_WITH_CTRL_ENTER,
                                    COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT)
            ->value().toBool());
    composerLayout->addWidget(sendWithCtrlEnter);
    composerLayout->addWidget(makeDescription(
        composerPage,
        tr("When enabled, Enter inserts a new line and Ctrl+Enter sends the message. "
           "Shift+Enter always inserts a new line.")));
    composerLayout->addStretch(1);
    tabs->addTab(composerPage, tr("Composer"));

    auto* sidebarPage = new QWidget(tabs);
    auto* sidebarLayout = new QVBoxLayout(sidebarPage);
    sidebarLayout->setContentsMargins(12, 12, 12, 12);
    sidebarLayout->setSpacing(8);

    auto* unreadModeGroup = new QGroupBox(tr("Unread mode"), sidebarPage);
    auto* unreadModeLayout = new QVBoxLayout(unreadModeGroup);
    unreadModeLayout->setSpacing(6);

    unreadModeChannelsOnly = new QCheckBox(tr("Channels only"), unreadModeGroup);
    unreadModeChannelsOnly->setChecked(
        options->optionObject<bool>(UNREAD_MODE_CHANNELS_ONLY,
                                    UNREAD_MODE_CHANNELS_ONLY_DEFAULT)
            ->value().toBool());
    unreadModeLayout->addWidget(unreadModeChannelsOnly);
    unreadModeLayout->addWidget(makeDescription(
        unreadModeGroup,
        tr("Keep Following available while Unread mode filters the Channels tab.")));

    unreadModeIgnoreWhileFiltering = new QCheckBox(
        tr("Ignore while filtering"), unreadModeGroup);
    unreadModeIgnoreWhileFiltering->setChecked(
        options->optionObject<bool>(UNREAD_MODE_IGNORE_WHILE_FILTERING,
                                    UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT)
            ->value().toBool());
    unreadModeLayout->addWidget(unreadModeIgnoreWhileFiltering);
    unreadModeLayout->addWidget(makeDescription(
        unreadModeGroup,
        tr("While text is entered in the channel filter, show matching read channels too. "
           "Clearing the text restores Unread mode.")));

    sidebarLayout->addWidget(unreadModeGroup);
    sidebarLayout->addStretch(1);
    tabs->addTab(sidebarPage, tr("Sidebar"));

    auto* appearancePage = new QWidget(tabs);
    auto* appearanceLayout = new QVBoxLayout(appearancePage);
    appearanceLayout->setContentsMargins(12, 12, 12, 12);
    appearanceLayout->setSpacing(8);

    auto* chatFontGroup = new QGroupBox(tr("Chat"), appearancePage);
    auto* chatFontForm = new QFormLayout(chatFontGroup);

    auto* chatFontOption = options->optionObject<QString>(
        CHAT_FONT, font().toString());
    originalChatFont = chatFontOption->value().toString();
    QFont chatFont = fontFromString(originalChatFont, font());

    auto* chatFontFamily = new QFontComboBox(chatFontGroup);
    chatFontFamily->setObjectName(QStringLiteral("chatFontFamily"));
    chatFontFamily->setCurrentFont(chatFont);
    chatFontForm->addRow(tr("Font:"), chatFontFamily);

    auto* chatTextSizeEditor = new QWidget(chatFontGroup);
    auto* chatTextSizeLayout = new QHBoxLayout(chatTextSizeEditor);
    chatTextSizeLayout->setContentsMargins(0, 0, 0, 0);
    chatTextSizeLayout->setSpacing(8);

    auto* chatTextSizeSlider = new QSlider(Qt::Horizontal, chatTextSizeEditor);
    chatTextSizeSlider->setObjectName(QStringLiteral("chatTextSizeSlider"));
    chatTextSizeSlider->setRange(MinChatFontPointSize,
                                 MaxChatFontPointSize);
    chatTextSizeSlider->setSingleStep(1);
    chatTextSizeSlider->setPageStep(2);
    chatTextSizeSlider->setMinimumWidth(140);
    chatTextSizeSlider->setValue(chatFontPointSize(chatFont));

    auto* chatTextSizeSpin = new QSpinBox(chatTextSizeEditor);
    chatTextSizeSpin->setObjectName(QStringLiteral("chatTextSizeSpin"));
    chatTextSizeSpin->setRange(MinChatFontPointSize,
                               MaxChatFontPointSize);
    chatTextSizeSpin->setSuffix(tr(" pt"));
    chatTextSizeSpin->setValue(chatTextSizeSlider->value());

    chatTextSizeLayout->addWidget(chatTextSizeSlider, 1);
    chatTextSizeLayout->addWidget(chatTextSizeSpin);
    chatFontForm->addRow(tr("Size:"), chatTextSizeEditor);

    appearanceLayout->addWidget(chatFontGroup);

    auto* codeFontGroup = new QGroupBox(tr("Code"), appearancePage);
    auto* codeFontForm = new QFormLayout(codeFontGroup);

    auto* codeFontOption = options->optionObject<QString>(
        CHAT_MONOSPACE_FONT, QString());
    originalMonospaceFont = codeFontOption->value().toString();

    // The monospace size is independent of the message font, so its default
    // presentation follows the resolved monospace font (system fixed family at
    // the current chat size when the option is empty).
    QFont codeFont = MonospaceFont::resolved(chatFont);

    auto* codeFontFamily = new QFontComboBox(codeFontGroup);
    codeFontFamily->setObjectName(QStringLiteral("codeFontFamily"));
    codeFontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    codeFontFamily->setCurrentFont(codeFont);
    codeFontForm->addRow(tr("Font:"), codeFontFamily);

    auto* codeTextSizeEditor = new QWidget(codeFontGroup);
    auto* codeTextSizeLayout = new QHBoxLayout(codeTextSizeEditor);
    codeTextSizeLayout->setContentsMargins(0, 0, 0, 0);
    codeTextSizeLayout->setSpacing(8);

    auto* codeTextSizeSlider = new QSlider(Qt::Horizontal, codeTextSizeEditor);
    codeTextSizeSlider->setObjectName(QStringLiteral("codeTextSizeSlider"));
    codeTextSizeSlider->setRange(MinChatFontPointSize,
                                 MaxChatFontPointSize);
    codeTextSizeSlider->setSingleStep(1);
    codeTextSizeSlider->setPageStep(2);
    codeTextSizeSlider->setMinimumWidth(140);
    codeTextSizeSlider->setValue(chatFontPointSize(codeFont));

    auto* codeTextSizeSpin = new QSpinBox(codeTextSizeEditor);
    codeTextSizeSpin->setObjectName(QStringLiteral("codeTextSizeSpin"));
    codeTextSizeSpin->setRange(MinChatFontPointSize,
                               MaxChatFontPointSize);
    codeTextSizeSpin->setSuffix(tr(" pt"));
    codeTextSizeSpin->setValue(codeTextSizeSlider->value());

    codeTextSizeLayout->addWidget(codeTextSizeSlider, 1);
    codeTextSizeLayout->addWidget(codeTextSizeSpin);
    codeFontForm->addRow(tr("Size:"), codeTextSizeEditor);

    appearanceLayout->addWidget(codeFontGroup);
    appearanceLayout->addWidget(makeDescription(
        appearancePage,
        tr("Font changes are previewed immediately in materialized chat messages. "
           "Cancel restores the previous font.")));
    auto* emojiGroup = new QGroupBox(tr("Emoji"), appearancePage);
    auto* emojiForm = new QFormLayout(emojiGroup);
    emojiDefaultSkinTone_ = new QComboBox(emojiGroup);
    for (int tone = 0; tone < EmojiSkinTone::COUNT; ++tone) {
        emojiDefaultSkinTone_->addItem(
            EmojiSkinTone::descriptionString[tone], tone);
    }
    const int storedSkinTone = qBound(
        0,
        options->optionObject<int>(
            EMOJI_DEFAULT_SKIN_TONE,
            EMOJI_DEFAULT_SKIN_TONE_DEFAULT)
            ->value().toInt(),
        static_cast<int>(EmojiSkinTone::COUNT) - 1);
    emojiDefaultSkinTone_->setCurrentIndex(storedSkinTone);
    emojiDefaultSkinTone_->setToolTip(
        tr("Used by default for emoji that support skin tones. "
           "Hold an emoji in the picker to choose a different tone once."));
    emojiForm->addRow(tr("Default skin tone:"), emojiDefaultSkinTone_);
    appearanceLayout->addWidget(emojiGroup);

    appearanceLayout->addStretch(1);
    tabs->addTab(appearancePage, tr("Appearance"));

    const auto updateChatFontOption =
        [this, chatFontFamily, chatTextSizeSlider, chatFontOption] {
        QFont updated = fontFromString(chatFontOption->value().toString(), font());
        updated.setFamily(chatFontFamily->currentFont().family());
        updated.setPointSizeF(chatTextSizeSlider->value());
        chatFontOption->setValue(updated.toString());
    };

    connect(chatTextSizeSlider, &QSlider::valueChanged, this,
            [chatTextSizeSpin, updateChatFontOption](int value) {
        if (chatTextSizeSpin->value() != value) {
            chatTextSizeSpin->setValue(value);
        }
        updateChatFontOption();
    });
    connect(chatTextSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [chatTextSizeSlider](int value) {
        if (chatTextSizeSlider->value() != value) {
            chatTextSizeSlider->setValue(value);
        }
    });
    connect(chatFontFamily, &QFontComboBox::currentFontChanged, this,
            [updateChatFontOption](const QFont&) {
        updateChatFontOption();
    });

    const auto updateCodeFontOption =
        [this, codeFontFamily, codeTextSizeSlider, codeFontOption] {
        QFont updated = MonospaceFont::resolved(font());
        QString stored = codeFontOption->value().toString();
        QFont parsed;
        if (!stored.isEmpty() && parsed.fromString(stored)) {
            updated = parsed;
        }
        updated.setFamily(codeFontFamily->currentFont().family());
        updated.setPointSizeF(codeTextSizeSlider->value());
        codeFontOption->setValue(updated.toString());
    };

    connect(codeTextSizeSlider, &QSlider::valueChanged, this,
            [codeTextSizeSpin, updateCodeFontOption](int value) {
        if (codeTextSizeSpin->value() != value) {
            codeTextSizeSpin->setValue(value);
        }
        updateCodeFontOption();
    });
    connect(codeTextSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [codeTextSizeSlider](int value) {
        if (codeTextSizeSlider->value() != value) {
            codeTextSizeSlider->setValue(value);
        }
    });
    connect(codeFontFamily, &QFontComboBox::currentFontChanged, this,
            [updateCodeFontOption](const QFont&) {
        updateCodeFontOption();
    });

    auto* cacheScroll = new QScrollArea(tabs);
    cacheScroll->setWidgetResizable(true);
    cacheScroll->setFrameShape(QFrame::NoFrame);

    auto* cachePage = new QWidget(cacheScroll);
    auto* cacheLayout = new QVBoxLayout(cachePage);
    cacheLayout->setContentsMargins(12, 12, 12, 12);
    cacheLayout->setSpacing(12);

    auto* admissionDescription = makeDescription(
        cachePage,
        tr("Post caches are admitted by channel recency, not by message activity. "
           "A busy channel that you do not open therefore cannot keep itself hot."));
    cacheLayout->addWidget(admissionDescription);

    auto* attachmentGroup = new QGroupBox(tr("Attachment files"), cachePage);
    auto* attachmentForm = new QFormLayout(attachmentGroup);
    attachmentCacheSizeMB = makeSpinBox(
        attachmentGroup, 16, 102400,
        options->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)->value().toInt(), tr(" MB"));
    attachmentForm->addRow(tr("Maximum disk cache:"), attachmentCacheSizeMB);
    cacheLayout->addWidget(attachmentGroup);

    auto* diskGroup = new QGroupBox(tr("Post cache on disk"), cachePage);
    auto* diskForm = new QFormLayout(diskGroup);
    diskChannelIdleHours = makeSpinBox(
        diskGroup, 1, 720,
        options->optionObject<int>(POST_CACHE_DISK_CHANNEL_IDLE_HOURS, POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT)->value().toInt(),
        tr(" h"));
    diskMaxMB = makeSpinBox(
        diskGroup, 64, 102400,
        options->optionObject<int>(POST_CACHE_DISK_MAX_MB, POST_CACHE_DISK_MAX_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    diskMaxPosts = makeSpinBox(
        diskGroup, 100, 1000000,
        options->optionObject<int>(POST_CACHE_DISK_MAX_POSTS, POST_CACHE_DISK_MAX_POSTS_DEFAULT)->value().toInt());
    diskMaxThreadReplies = makeSpinBox(
        diskGroup, 10, 100000,
        options->optionObject<int>(POST_CACHE_DISK_MAX_THREAD_REPLIES, POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT)->value().toInt());
    diskMaintenanceMinutes = makeSpinBox(
        diskGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_DISK_MAINTENANCE_MINUTES, POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));

    diskForm->addRow(tr("Keep channels opened within:"), diskChannelIdleHours);
    diskForm->addRow(tr("Maximum compressed payload:"), diskMaxMB);
    diskForm->addRow(tr("Maximum posts:"), diskMaxPosts);
    diskForm->addRow(tr("Maximum replies per thread:"), diskMaxThreadReplies);
    diskForm->addRow(tr("Maintenance interval:"), diskMaintenanceMinutes);
    cacheLayout->addWidget(diskGroup);

    auto* memoryGroup = new QGroupBox(tr("Post cache in memory"), cachePage);
    auto* memoryForm = new QFormLayout(memoryGroup);
    memoryChannelIdleMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES, POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));
    memoryHardMB = makeSpinBox(
        memoryGroup, 64, 32768,
        options->optionObject<int>(POST_CACHE_MEMORY_HARD_MB, POST_CACHE_MEMORY_HARD_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    memoryTargetMB = makeSpinBox(
        memoryGroup, 32, memoryHardMB->value(),
        options->optionObject<int>(POST_CACHE_MEMORY_TARGET_MB, POST_CACHE_MEMORY_TARGET_MB_DEFAULT)->value().toInt(),
        tr(" MB"));
    memoryPostTtlMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        options->optionObject<int>(POST_CACHE_MEMORY_POST_TTL_MINUTES, POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT)->value().toInt(),
        tr(" min"));
    memorySweepSeconds = makeSpinBox(
        memoryGroup, 5, 3600,
        options->optionObject<int>(POST_CACHE_MEMORY_SWEEP_SECONDS, POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT)->value().toInt(),
        tr(" s"));

    connect(memoryHardMB, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int hardLimit) {
        memoryTargetMB->setMaximum(hardLimit);
        if (memoryTargetMB->value() > hardLimit) {
            memoryTargetMB->setValue(hardLimit);
        }
    });

    memoryForm->addRow(tr("Keep channels opened within:"), memoryChannelIdleMinutes);
    memoryForm->addRow(tr("Hard accounted limit:"), memoryHardMB);
    memoryForm->addRow(tr("Trim target after pressure:"), memoryTargetMB);
    memoryForm->addRow(tr("Cold post idle TTL:"), memoryPostTtlMinutes);
    memoryForm->addRow(tr("Sweep interval:"), memorySweepSeconds);
    cacheLayout->addWidget(memoryGroup);

    cacheLayout->addWidget(makeDescription(
        cachePage,
        tr("SQLite page size, WAL mode and bounded vacuum details are implementation "
           "invariants rather than cache-policy knobs and remain internal.")));
    cacheLayout->addStretch(1);

    cacheScroll->setWidget(cachePage);
    tabs->addTab(cacheScroll, tr("Cache"));
    ui->verticalLayout->insertWidget(0, tabs, 1);

    connect (ui->downloadLocationButton, &QPushButton::clicked, [this] {
        QDir defaultDir (ui->downloadLocationValue->text());

        if (!defaultDir.exists()) {
            defaultDir = QDir::home();
        }
        ui->downloadLocationValue->setText (QFileDialog::getExistingDirectory (this, "Select destination directory", defaultDir.absolutePath()));
    });
}

SettingsWindow::~SettingsWindow()
{
    delete ui;
}

void SettingsWindow::reject()
{
    MLOptions::instance()
        ->optionObject<QString>(CHAT_FONT, font().toString())
        ->setValue(originalChatFont);
    MLOptions::instance()
        ->optionObject<QString>(CHAT_MONOSPACE_FONT, QString())
        ->setValue(originalMonospaceFont);
    QDialog::reject();
}

void SettingsWindow::applyNewSettings ()
{
    auto* options = MLOptions::instance();
    options->optionObject<QString>(
        DOWNLOAD_LOCATION,
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
        ->setValue(ui->downloadLocationValue->text());
    options->optionObject<bool>(DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)
        ->setValue(ui->askLocationCheckBox->isChecked());
    options->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
        ->setValue(ui->imageMaxWidthValue->text().toInt());
    options->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
        ->setValue(ui->imageMaxHeightValue->text().toInt());
    options->optionObject<bool>(
        COMPOSER_SEND_WITH_CTRL_ENTER,
        COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT)
        ->setValue(sendWithCtrlEnter->isChecked());
    options->optionObject<bool>(
        UNREAD_MODE_CHANNELS_ONLY,
        UNREAD_MODE_CHANNELS_ONLY_DEFAULT)
        ->setValue(unreadModeChannelsOnly->isChecked());
    options->optionObject<bool>(
        UNREAD_MODE_IGNORE_WHILE_FILTERING,
        UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT)
        ->setValue(unreadModeIgnoreWhileFiltering->isChecked());

    options->optionObject<int>(
        EMOJI_DEFAULT_SKIN_TONE,
        EMOJI_DEFAULT_SKIN_TONE_DEFAULT)
        ->setValue(emojiDefaultSkinTone_->currentData().toInt());

    options->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)
        ->setValue(attachmentCacheSizeMB->value());
    options->optionObject<int>(
        POST_CACHE_DISK_CHANNEL_IDLE_HOURS,
        POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT)
        ->setValue(diskChannelIdleHours->value());
    options->optionObject<int>(POST_CACHE_DISK_MAX_MB, POST_CACHE_DISK_MAX_MB_DEFAULT)
        ->setValue(diskMaxMB->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAX_POSTS, POST_CACHE_DISK_MAX_POSTS_DEFAULT)
        ->setValue(diskMaxPosts->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAX_THREAD_REPLIES,
        POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT)
        ->setValue(diskMaxThreadReplies->value());
    options->optionObject<int>(
        POST_CACHE_DISK_MAINTENANCE_MINUTES,
        POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT)
        ->setValue(diskMaintenanceMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES,
        POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT)
        ->setValue(memoryChannelIdleMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_HARD_MB, POST_CACHE_MEMORY_HARD_MB_DEFAULT)
        ->setValue(memoryHardMB->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_TARGET_MB, POST_CACHE_MEMORY_TARGET_MB_DEFAULT)
        ->setValue(memoryTargetMB->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_POST_TTL_MINUTES,
        POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT)
        ->setValue(memoryPostTtlMinutes->value());
    options->optionObject<int>(
        POST_CACHE_MEMORY_SWEEP_SECONDS,
        POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT)
        ->setValue(memorySweepSeconds->value());
}

} /* namespace Mattermost */
