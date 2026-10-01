#include <QtTest>

#include "navigation/NavigationTabsModel.h"
#include "navigation/NavigationUiController.h"

using Mattermost::NavigationTabsModel;
using Mattermost::NavigationUiController;

class NavigationTabsModelTest : public QObject
{
    Q_OBJECT

private slots:
    void singleDestinationKeepsTabBarHidden()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry entry;
        entry.channelId = QStringLiteral("channel-a");
        entry.title = QStringLiteral("A");

        QCOMPARE(model.append(entry), 0);
        QCOMPARE(model.count(), 1);
        QVERIFY(!model.shouldShowTabBar());
    }

    void secondDestinationShowsTabBar()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry first;
        first.channelId = QStringLiteral("channel-a");
        NavigationTabsModel::Entry second;
        second.channelId = QStringLiteral("channel-b");

        model.append(first);
        model.append(second);

        QCOMPARE(model.count(), 2);
        QVERIFY(model.shouldShowTabBar());
    }

    void repeatedChannelDestinationIsIdempotent()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry channel;
        channel.channelId = QStringLiteral("channel-a");
        channel.postId = QStringLiteral("post-1");

        QCOMPARE(model.append(channel), 0);

        channel.postId = QStringLiteral("post-2");
        QCOMPARE(model.append(channel), 0);
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.at(0)->postId, QStringLiteral("post-2"));
    }

    void replaceCannotCreateDuplicateDestination()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry first;
        first.channelId = QStringLiteral("channel-a");
        NavigationTabsModel::Entry second;
        second.channelId = QStringLiteral("channel-b");

        model.append(first);
        model.append(second);

        second.channelId = QStringLiteral("channel-a");
        QVERIFY(!model.replace(1, second));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.at(1)->channelId, QStringLiteral("channel-b"));
    }

    void pinnedDestinationCannotBeReplaced()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry first;
        first.channelId = QStringLiteral("channel-a");
        first.title = QStringLiteral("A");
        QCOMPARE(model.append(first), 0);
        QVERIFY(model.setPinned(0, true));

        NavigationTabsModel::Entry replacement;
        replacement.channelId = QStringLiteral("channel-b");
        replacement.title = QStringLiteral("B");

        QVERIFY(!model.replace(0, replacement));
        QCOMPARE(model.at(0)->channelId, QStringLiteral("channel-a"));
        QVERIFY(model.at(0)->pinned);
    }

    void pinnedDestinationStillAcceptsBookmarkUpdates()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry entry;
        entry.channelId = QStringLiteral("channel-a");
        entry.postId = QStringLiteral("post-1");
        QCOMPARE(model.append(entry), 0);
        QVERIFY(model.setPinned(0, true));

        entry.postId = QStringLiteral("post-2");
        entry.title = QStringLiteral("Updated title");
        QVERIFY(model.replace(0, entry));
        QCOMPARE(model.at(0)->postId, QStringLiteral("post-2"));
        QCOMPARE(model.at(0)->title, QStringLiteral("Updated title"));
        QVERIFY(model.at(0)->pinned);
    }

    void reusableChannelTabSkipsPinnedDestinations()
    {
        NavigationTabsModel model;
        for (const QString& id : {
                 QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            NavigationTabsModel::Entry entry;
            entry.channelId = id;
            model.append(entry);
        }
        QVERIFY(model.setPinned(0, true));
        QVERIFY(model.setPinned(1, true));

        QCOMPARE(model.findReusableChannelTab(0), 2);
        QCOMPARE(model.findReusableChannelTab(2), 2);

        QVERIFY(model.setPinned(2, true));
        QCOMPARE(model.findReusableChannelTab(0), -1);
    }

    void threadDestinationIsDeduplicatedAndBookmarkUpdated()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry thread;
        thread.channelId = QStringLiteral("channel-a");
        thread.rootId = QStringLiteral("root-a");
        thread.postId = QStringLiteral("reply-1");

        QCOMPARE(model.append(thread), 0);

        thread.postId = QStringLiteral("reply-2");
        QCOMPARE(model.append(thread), 0);
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.at(0)->postId, QStringLiteral("reply-2"));
    }

    void cycledIndexHandlesEmptyAndSingleTab()
    {
        NavigationTabsModel empty;
        QCOMPARE(empty.cycledIndex(0, 1), -1);
        QCOMPARE(empty.cycledIndex(0, -1), -1);

        NavigationTabsModel single;
        NavigationTabsModel::Entry entry;
        entry.channelId = QStringLiteral("channel-a");
        single.append(entry);
        QCOMPARE(single.cycledIndex(0, 1), 0);
        QCOMPARE(single.cycledIndex(0, -1), 0);
    }

    void cycledIndexWrapsAround()
    {
        NavigationTabsModel model;
        for (const QString& id : {
                 QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            NavigationTabsModel::Entry entry;
            entry.channelId = id;
            model.append(entry);
        }

        QCOMPARE(model.cycledIndex(0, 1), 1);
        QCOMPARE(model.cycledIndex(1, 1), 2);
        QCOMPARE(model.cycledIndex(2, 1), 0);
        QCOMPARE(model.cycledIndex(2, -1), 1);
        QCOMPARE(model.cycledIndex(1, -1), 0);
        QCOMPARE(model.cycledIndex(0, -1), 2);
    }

    void cycledIndexSeedsOutOfRangeCurrentIndex()
    {
        NavigationTabsModel model;
        for (const QString& id : {
                 QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            NavigationTabsModel::Entry entry;
            entry.channelId = id;
            model.append(entry);
        }

        QCOMPARE(model.cycledIndex(-1, 1), 0);
        QCOMPARE(model.cycledIndex(-1, -1), 2);
        QCOMPARE(model.cycledIndex(99, 1), 0);
        QCOMPARE(model.cycledIndex(99, -1), 2);
    }

    void movingTabsKeepsSemanticTargetsAligned()
    {
        NavigationTabsModel model;
        for (const QString& id : {
                 QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            NavigationTabsModel::Entry entry;
            entry.channelId = id;
            model.append(entry);
        }

        model.move(0, 2);

        QCOMPARE(model.at(0)->channelId, QStringLiteral("b"));
        QCOMPARE(model.at(1)->channelId, QStringLiteral("c"));
        QCOMPARE(model.at(2)->channelId, QStringLiteral("a"));
    }

    void collectionHistoryLocationsUseSemanticIdentity()
    {
        using Kind = NavigationUiController::Location::Kind;

        NavigationUiController::Location search;
        search.kind = Kind::Search;
        QVERIFY(search.isValid());
        QVERIFY(!search.isChat());

        NavigationUiController::Location sameSearch = search;
        QVERIFY(search.sameDestination(sameSearch));

        NavigationUiController::Location saved;
        saved.kind = Kind::Saved;
        QVERIFY(saved.isValid());
        QVERIFY(!search.sameDestination(saved));

        NavigationUiController::Location chat;
        QVERIFY(!chat.isValid());
        chat.channelId = QStringLiteral("channel-a");
        chat.rootId = QStringLiteral("root-a");
        QVERIFY(chat.isValid());
        QVERIFY(chat.isChat());

        NavigationUiController::Location sameChat = chat;
        sameChat.postId = QStringLiteral("reply-a");
        QVERIFY(chat.sameDestination(sameChat));
        QVERIFY(!chat.sameDestination(search));
    }

    void chatHistoryLocationKeepsLegacyConstructionShape()
    {
        NavigationUiController::Location chat{
            QStringLiteral("channel-a"),
            QStringLiteral("root-a"),
            QStringLiteral("post-a")};

        QVERIFY(chat.isChat());
        QVERIFY(chat.isValid());
        QCOMPARE(chat.channelId, QStringLiteral("channel-a"));
        QCOMPARE(chat.rootId, QStringLiteral("root-a"));
        QCOMPARE(chat.postId, QStringLiteral("post-a"));
    }
};

QTEST_APPLESS_MAIN(NavigationTabsModelTest)

#include "NavigationTabsModelTest.moc"
