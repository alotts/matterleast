#include <algorithm>

#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/types/BackendPost.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/post/ReactionChipStyle.h"
#include "chat-area/post/reactions/PostReaction.h"
#include "chat-area/post/reactions/PostReactionList.h"

using namespace Mattermost;

class PostReactionInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void reactionCountTracksInheritedChatFont()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Alice")});

        QFont chatFont = list.font();
        chatFont.setPointSize(19);
        list.setFont(chatFont);
        QApplication::processEvents();

        auto* chip = list.findChild<PostReaction*>();
        QVERIFY(chip);
        auto* count = chip->findChild<QLabel*>(QStringLiteral("count"));
        QVERIFY(count);
        const QFont expected = ReactionChipStyle::countFont(chatFont);
        QCOMPARE(count->font().pointSizeF(), expected.pointSizeF());

        auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(emoji);
        const int expectedExtent = ReactionChipStyle::iconExtent(chatFont);
        const int expectedBoxExtent = ReactionChipStyle::iconBoxExtent(chatFont);
        QCOMPARE(emoji->size(), QSize(expectedBoxExtent, expectedBoxExtent));
        QCOMPARE(emoji->font().pointSizeF(), chatFont.pointSizeF());
        QVERIFY(expectedBoxExtent > expectedExtent);
        QCOMPARE(chip->minimumHeight(), ReactionChipStyle::chipHeight(chatFont));
        QVERIFY(list.minimumHeight() >= ReactionChipStyle::chipHeight(chatFont));
        QVERIFY2(list.minimumHeight() > 27,
                 "Reaction list must not retain the legacy 27px height cap");
    }

    void reactionEventsAreIdempotentAndUseUserIds()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        QJsonObject user {
            {QStringLiteral("id"), userId},
            {QStringLiteral("username"), QStringLiteral("alice")},
            {QStringLiteral("first_name"), QStringLiteral("Alice")},
        };
        BackendUser* loginUser = storage.addUser(user, true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        QJsonArray reactionArray {
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), QStringLiteral("eyes")},
            },
        };
        QJsonObject metadata {
            {QStringLiteral("reactions"), reactionArray},
        };
        QJsonObject postJson {
            {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
            {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("create_at"), 1},
            {QStringLiteral("metadata"), metadata},
        };

        BackendPost post(postJson, storage);
        auto reaction = post.reactions.find(QStringLiteral("eyes"));
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        // A replayed reaction_added event is a repeated fact, not a toggle.
        post.addReaction(userId, QStringLiteral("eyes"));
        reaction = post.reactions.find(QStringLiteral("eyes"));
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        post.removeReaction(userId, QStringLiteral("eyes"));
        QVERIFY(post.reactions.find(QStringLiteral("eyes"))
                == post.reactions.end());
    }

    void serverEmojiNameRemainsReactionIdentity()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("zzzzzzzzzzzzzzzzzzzzzzzzzz");
        BackendUser* loginUser = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("alias-tester")},
            },
            true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        // The built-in registry intentionally aliases "thumbsup" to the same
        // presentation as "+1". That makes this a deterministic regression for
        // reaction identity being replaced by EmojiInfo's canonical name.
        const QString wireName = QStringLiteral("thumbsup");
        const EmojiID aliasId = EmojiInfo::findByName(wireName);
        QVERIFY(aliasId);
        const Emoji presentation = EmojiInfo::getEmoji(aliasId);
        QCOMPARE(presentation.name, QStringLiteral("+1"));
        QVERIFY(presentation.name != wireName);

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("yyyyyyyyyyyyyyyyyyyyyyyyyy")},
                {QStringLiteral("channel_id"), QStringLiteral("xxxxxxxxxxxxxxxxxxxxxxxxxx")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("alias")},
                {QStringLiteral("metadata"),
                 QJsonObject {
                     {QStringLiteral("reactions"),
                      QJsonArray {
                          QJsonObject {
                              {QStringLiteral("user_id"), userId},
                              {QStringLiteral("emoji_name"), wireName},
                          },
                      }},
                 }},
            },
            storage);

        auto reaction = post.reactions.find(wireName);
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, wireName));
        QVERIFY(!post.hasReaction(userId, presentation.name));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        auto* chip = widget.findChild<PostReaction*>();
        QVERIFY(chip);
        QVERIFY2(chip->toolTip().startsWith(wireName + QStringLiteral("  ")),
                 qPrintable(chip->toolTip()));

        auto* emojiLabel = chip->findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(emojiLabel);
        QVERIFY(emojiLabel->text().contains(QString::fromUtf8("👍")));
    }

    void customReactionSurvivesUntilEmojiRegistration()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("dddddddddddddddddddddddddd");
        BackendUser* loginUser = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("customtester")},
                {QStringLiteral("first_name"), QStringLiteral("Custom")},
            },
            true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        const QString customName =
            QStringLiteral("matterleast_pending_reaction_regression");
        QVERIFY(!EmojiInfo::findByName(customName));

        QJsonArray reactionArray {
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), QStringLiteral("eyes")},
            },
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), customName},
            },
        };
        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeee")},
                {QStringLiteral("channel_id"), QStringLiteral("ffffffffffffffffffffffffff")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
                {QStringLiteral("metadata"),
                 QJsonObject {{QStringLiteral("reactions"), reactionArray}}},
            },
            storage);

        QCOMPARE(static_cast<int>(post.reactions.size()), 2);
        auto customReaction = post.reactions.find(customName);
        QVERIFY(customReaction != post.reactions.end());
        QCOMPARE(customReaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, customName));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        PostWidget secondWidget(backend, post, nullptr, nullptr, nullptr);
        auto* list = widget.findChild<PostReactionList*>();
        auto* secondList = secondWidget.findChild<PostReactionList*>();
        QVERIFY2(list,
                 "An unresolved custom reaction must still have a visible placeholder chip");
        QVERIFY(secondList);
        QCOMPARE(list->findChildren<PostReaction*>().size(), 2);
        QCOMPARE(secondList->findChildren<PostReaction*>().size(), 2);

        const auto customChipText = [&customName](PostReactionList* reactionList) {
            const auto chips = reactionList->findChildren<PostReaction*>();
            for (PostReaction* chip : chips) {
                auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
                if (emoji && emoji->text().contains(customName)) {
                    return emoji->text();
                }
            }
            return QString();
        };
        const QString unresolvedText =
            QStringLiteral(":") + customName + QLatin1Char(':');
        QCOMPARE(customChipText(list), unresolvedText);
        QCOMPARE(customChipText(secondList), unresolvedText);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("custom.png"));
        QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QVERIFY(image.save(imagePath));

        // Registration can happen later through either the eager /emoji load or
        // CustomEmojiService. The existing post/widget must adopt it in place.
        EmojiInfo::addCustomEmoji(customName, imagePath);

        const EmojiID customId = EmojiInfo::findByName(customName);
        QVERIFY(customId);
        customReaction = post.reactions.find(customName);
        QVERIFY(customReaction != post.reactions.end());
        QCOMPARE(customReaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, customName));

        QApplication::processEvents();
        list = widget.findChild<PostReactionList*>();
        secondList = secondWidget.findChild<PostReactionList*>();
        QVERIFY(list);
        QVERIFY(secondList);
        QCOMPARE(list->findChildren<PostReaction*>().size(), 2);
        QCOMPARE(secondList->findChildren<PostReaction*>().size(), 2);
        const auto hasImageChip = [](PostReactionList* reactionList) {
            const auto chips = reactionList->findChildren<PostReaction*>();
            for (PostReaction* chip : chips) {
                auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
                if (emoji && emoji->text().contains(QStringLiteral("<img"))) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(hasImageChip(list));
        QVERIFY(hasImageChip(secondList));
    }

    void customReactionCanBeRemovedBeforeRegistration()
    {
        Storage storage;
        const QString userId = QStringLiteral("gggggggggggggggggggggggggg");
        const QString customName =
            QStringLiteral("matterleast_pending_reaction_remove_test");
        QVERIFY(!EmojiInfo::findByName(customName));

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("hhhhhhhhhhhhhhhhhhhhhhhhhh")},
                {QStringLiteral("channel_id"), QStringLiteral("iiiiiiiiiiiiiiiiiiiiiiiiii")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
            },
            storage);

        post.addReaction(userId, customName);
        QVERIFY(post.hasReaction(userId, customName));
        QVERIFY(post.reactions.find(customName) != post.reactions.end());

        post.removeReaction(userId, customName);
        QVERIFY(!post.hasReaction(userId, customName));
        QVERIFY(post.reactions.find(customName) == post.reactions.end());
    }

    void liveReactionUpdateCommitsPostGeometry()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        BackendUser* user = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("alice")},
                {QStringLiteral("first_name"), QStringLiteral("Alice")},
            },
            true);
        QVERIFY(user);
        user->isLoginUser = true;

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
            },
            storage);

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(480, widget.sizeHint().height());
        widget.show();
        QApplication::processEvents();
        const int before = widget.sizeHint().height();

        QSignalSpy dimensions(&widget, &PostWidget::dimensionsChanged);
        post.addReaction(userId, QStringLiteral("eyes"));
        widget.updateReactions();

        QTRY_VERIFY_WITH_TIMEOUT(dimensions.count() > 0, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(widget.sizeHint().height() > before, 1000);

        // PostWidget publishes size hints; its LongList owner owns the physical
        // top-level row rect. Apply the owner's geometry commit before checking
        // that the reaction child is contained.
        widget.resize(widget.width(), widget.sizeHint().height());
        QApplication::processEvents();

        auto* list = widget.findChild<PostReactionList*>();
        QVERIFY(list);
        QVERIFY(list->height() >= list->minimumSizeHint().height());
        QVERIFY(widget.rect().contains(
            list->mapTo(&widget, list->rect().bottomRight())));
    }

    void secondChipIsOneClickableHitTarget()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("thumbsup"), QString::fromUtf8("👍"),
                         BackendPostReaction {QStringLiteral("Alice")});
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Bob")});
        list.resize(220, 32);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        QApplication::processEvents();

        auto chips = list.findChildren<PostReaction*>();
        QCOMPARE(chips.size(), 2);
        std::sort(chips.begin(), chips.end(), [](PostReaction* lhs, PostReaction* rhs) {
            return lhs->mapToGlobal(QPoint()).x() < rhs->mapToGlobal(QPoint()).x();
        });
        PostReaction* second = chips.at(1);
        const QPoint globalCenter = second->mapToGlobal(second->rect().center());
        QWidget* hit = QApplication::widgetAt(globalCenter);
        QCOMPARE(hit, static_cast<QWidget*>(second));

        QSignalSpy spy(&list, &PostReactionList::reactionClicked);
        QTest::mouseClick(hit, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("eyes"));
    }
};

QTEST_MAIN(PostReactionInteractionTest)
#include "PostReactionInteractionTest.moc"
