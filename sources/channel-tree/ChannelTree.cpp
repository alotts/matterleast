/**
 * @file ChannelTree.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 18, 2021
 *
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

#include "ChannelTree.h"

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QPointer>
#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarChannelMovePolicy.h"
#include "channel-tree/channel-item/DirectChannelItem.h"
#include "channel-tree/team-item/GroupTeamItem.h"
#include "log.h"

namespace Mattermost {

namespace {

QString categoryDisplayName(const SidebarCategory& category)
{
    if (!category.displayName.isEmpty()) {
        return category.displayName;
    }
    if (category.type == QStringLiteral("favorites")) {
        return QStringLiteral("Favorites");
    }
    if (category.type == QStringLiteral("channels")) {
        return QStringLiteral("Channels");
    }
    if (category.type == QStringLiteral("direct_messages")) {
        return QStringLiteral("Direct Messages");
    }
    return QStringLiteral("Category");
}

QPoint dropEventPosition(const QDropEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

QRect dragContentRect(const QTreeWidgetItem* item, QRect rect)
{
    if (!item) {
        return rect;
    }
    rect.adjust(0, qMax(0, item->data(0, SidebarItem::DropGapBeforeRole).toInt()),
                0, -qMax(0, item->data(0, SidebarItem::DropGapAfterRole).toInt()));
    return rect;
}

bool channelDropChangesPosition(QTreeWidgetItem* source,
                                QTreeWidgetItem* targetCategory,
                                const QString& targetChannelId,
                                bool afterTarget)
{
    if (!source || !targetCategory) {
        return false;
    }
    if (source->parent() != targetCategory) {
        return true;
    }

    QStringList channelIds;
    for (int i = 0; i < targetCategory->childCount(); ++i) {
        QTreeWidgetItem* row = targetCategory->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel) {
            channelIds.push_back(row->data(0, SidebarItem::IdRole).toString());
        }
    }
    return reorderSidebarChannel(channelIds,
                                 source->data(0, SidebarItem::IdRole).toString(),
                                 targetChannelId, afterTarget);
}

bool categoryDropChangesPosition(QTreeWidgetItem* source,
                                 QTreeWidgetItem* targetCategory,
                                 bool afterTarget)
{
    if (!source || !targetCategory || source->parent() != targetCategory->parent()) {
        return false;
    }

    QTreeWidgetItem* team = source->parent();
    QStringList categoryIds;
    for (int i = 0; team && i < team->childCount(); ++i) {
        QTreeWidgetItem* row = team->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Category) {
            categoryIds.push_back(row->data(0, SidebarItem::IdRole).toString());
        }
    }
    return reorderSidebarCategory(categoryIds,
                                  source->data(0, SidebarItem::IdRole).toString(),
                                  targetCategory->data(0, SidebarItem::IdRole).toString(),
                                  afterTarget);
}

class VirtualDestinationItem final : public ChannelItem
{
public:
    using ChannelItem::ChannelItem;

    void showContextMenu(const QPoint&) override {}
};

} // namespace

ChannelTree::ChannelTree (QWidget* parent)
:QTreeWidget (parent)
,chatAreaStackedWidget(nullptr)
,backendForSidebar(nullptr)
,renderingSidebar(false)
{
	connect (this, &QTreeWidget::customContextMenuRequested, this, &ChannelTree::showContextMenu);

	connect (this, &QTreeWidget::currentItemChanged, this,
             [this] (QTreeWidgetItem* item, QTreeWidgetItem*) {
        activateChannelItem(item);
	});

    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, false);
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, true);
        }
    });

    setItemDelegate(new ChannelItemDelegate(this));
    setMouseTracking(true);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
}

ChannelTree::~ChannelTree () = default;

bool ChannelTree::isChannelActive (const BackendChannel& channel)
{
	return getCurrentPage () && &getCurrentPage()->getChannel() == &channel;
}

void ChannelTree::addTeam (Backend& backend, BackendTeam& team)
{
    backendForSidebar = &backend;

	TeamItem* teamList = new GroupTeamItem (*this, backend, team.display_name, team.id);
    teamList->setData(0, ItemKindRole, TeamItemKind);
    teamList->setData(0, ItemIdRole, team.id);
    teamList->setData(0, ItemTeamIdRole, team.id);
    teamList->setFlags(teamList->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));

	addTopLevelItem (teamList);
    teamToItemMap.insert(team.id, teamList);
	header()->setSectionResizeMode(0, QHeaderView::Stretch);

	connect (&team, &BackendTeam::onNewChannel, this, [this, &backend, &team] (BackendChannel&) {
        refreshTeamSidebar(backend, team);
	});

	connect (&team, &BackendTeam::onLeave, this, [this, &team, teamList] {
        if (sidebarDragActive) {
            pendingSidebarRefreshTeams.insert(team.id);
            return;
        }
        clearTeamSidebar(*teamList);
        teamToItemMap.remove(team.id);

		int index = indexOfTopLevelItem (teamList);
		if (index == -1) {
			return;
		}

		QTreeWidgetItem* item = takeTopLevelItem (index);
		delete item;
	});

    // Populate only channels at startup. Team members are loaded on demand by
    // the dialogs that actually need them; downloading every member here is a
    // large and unrelated startup request burst on big Mattermost instances.
	backend.retrieveOwnChannelMembershipsForTeam (team, [] (BackendChannel&) {});
}

void ChannelTree::populateSidebars(Backend& backend)
{
    backendForSidebar = &backend;
    auto& sidebar = SidebarService::instance(backend);
    connect(&sidebar, &SidebarService::channelMutedChanged,
            this, &ChannelTree::setChannelMutedVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelActivityChanged,
            this, &ChannelTree::refreshChannelUnreadVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelActivityReset,
            this, &ChannelTree::refreshAllChannelActivityVisuals,
            Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelMentionedChanged,
            this, &ChannelTree::setChannelMentionedVisual, Qt::UniqueConnection);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        if (BackendTeam* team = backend.getStorage().getTeamById(it.key())) {
            refreshTeamSidebar(backend, *team);
        }
    }
}

void ChannelTree::refreshTeamSidebar(Backend& backend, BackendTeam& team)
{
    const QString teamId = team.id;
    SidebarService::instance(backend).retrieveCategories(team,
        [this, &backend, teamId](const SidebarTeamState& state) {
            BackendTeam* currentTeam = backend.getStorage().getTeamById(teamId);
            TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
            if (!currentTeam || !teamItem) {
                return;
            }
            renderTeamSidebar(backend, *teamItem, state);
        });
}

void ChannelTree::renderTeamSidebar(Backend& backend, TeamItem& teamItem,
                                    const SidebarTeamState& state)
{
    reconcileTeamSidebar(backend, teamItem, state);
}

void ChannelTree::clearTeamSidebar(TeamItem& teamItem)
{
    while (teamItem.childCount() > 0) {
        QTreeWidgetItem* category = teamItem.takeChild(0);
        while (category->childCount() > 0) {
            QTreeWidgetItem* channelItem = category->takeChild(0);
            const QString channelId = channelItem->data(0, ItemIdRole).toString();
            removeChannelToItem(channelId, channelItem);

            ChatArea* chatArea = channelItem->data(0, Qt::UserRole).value<ChatArea*>();
            if (chatArea) {
                if (chatAreaStackedWidget) {
                    chatAreaStackedWidget->removeWidget(chatArea);
                }
                delete chatArea;
            }
            delete channelItem;
        }
        delete category;
    }
}

QTreeWidgetItem* ChannelTree::createCategoryItem(TeamItem& teamItem, const QString& categoryId,
                                                 const QString& displayName, bool collapsed)
{
    auto* item = new QTreeWidgetItem(&teamItem, QStringList() << displayName);
    item->setData(0, ItemKindRole, CategoryItemKind);
    item->setData(0, ItemIdRole, categoryId);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled)
                   & ~Qt::ItemIsEditable);

    QFont font = item->font(0);
    font.setBold(true);
    font.setPixelSize(12);
    item->setFont(0, font);
    item->setSizeHint(0, QSize(0, 22));
    item->setExpanded(!collapsed);
    return item;
}

ChannelItem* ChannelTree::createPersonalItem(Backend& backend, TeamItem& teamItem)
{
    auto* item = new DirectChannelItem(backend, nullptr);
    teamItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:personal"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::PersonalDestination);
    item->setData(0, ItemChannelTypeRole, BackendChannel::directChannel);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Personal"));

    if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(
            backend.getLoginUser().id)) {
        item->setData(0, ItemChannelIdRole, channel->id);
    }

    const BackendUser& self = backend.getLoginUser();
    item->setStatus(self.status);
    if (!self.avatar.isNull()) {
        item->setIcon(QIcon(self.avatar));
    }

    if (!personalUserConnected) {
        personalUserConnected = true;
        if (self.avatar.isNull()) {
            backend.retrieveUserAvatar(self.id);
        }
        connect(&self, &BackendUser::onAvatarChanged, this,
                [this] { refreshPersonalItems(); });
        connect(&self, &BackendUser::onStatusChanged, this,
                [this] { refreshPersonalItems(); });
    }
    return item;
}

ChannelItem* ChannelTree::createSavedItem(Backend& backend, TeamItem& teamItem)
{
    auto* item = new VirtualDestinationItem(backend, nullptr);
    teamItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:saved"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::SavedDestination);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Saved"));
    item->setIcon(QIcon(QStringLiteral(":/icons/bookmark")));
    return item;
}

ChannelItem* ChannelTree::createDraftsItem(Backend& backend, TeamItem& teamItem)
{
    auto* item = new VirtualDestinationItem(backend, nullptr);
    teamItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:drafts"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::DraftsDestination);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Drafts"));
    item->setIcon(QIcon(QStringLiteral(":/icons/edit")));
    return item;
}

QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId) const
{
    TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
    if (!teamItem) {
        return nullptr;
    }

    for (int rowIndex = 0; rowIndex < teamItem->childCount(); ++rowIndex) {
        QTreeWidgetItem* row = teamItem->child(rowIndex);
        if (row
            && row->data(0, ItemKindRole).toInt() == VirtualDestinationItemKind
            && row->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {
            return row;
        }
    }
    return nullptr;
}

void ChannelTree::refreshPersonalItems()
{
    if (!backendForSidebar) {
        return;
    }

    const BackendUser& self = backendForSidebar->getLoginUser();
    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(self.id);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        QTreeWidgetItem* row = personalItemForTeam(it.key());
        if (!row) {
            continue;
        }
        auto* personalItem = static_cast<ChannelItem*>(row);
        personalItem->setStatus(self.status);
        personalItem->setIcon(self.avatar.isNull() ? QIcon() : QIcon(self.avatar));
        row->setData(0, ItemChannelIdRole, channel ? channel->id : QString());
    }
}

ChannelItem* ChannelTree::createChannelItem(Backend& backend, TeamItem& teamItem,
                                            QTreeWidgetItem& categoryItem, BackendChannel& channel)
{
    ChannelItem* item = nullptr;
    if (channel.type == BackendChannel::directChannel || channel.type == BackendChannel::groupChannel) {
        item = new DirectChannelItem(backend, nullptr);
    } else {
        item = teamItem.createChannelItem(backend, nullptr);
    }

    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, ChannelItemKind);
    item->setData(0, ItemIdRole, channel.id);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    item->setLabel(channel.display_name);

    if (channel.type == BackendChannel::groupChannel) {
        item->setIcon(ChannelIcons::groupConversation());
    } else if (channel.type != BackendChannel::directChannel) {
        item->setIcon(ChannelIcons::channel());
    }

    if (channel.type == BackendChannel::directChannel) {
        BackendUser* user = backend.getStorage().getUserById(channel.name);
        if (user) {
            item->setStatus(user->status);
            if (!user->avatar.isNull()) {
                item->setIcon(QIcon(user->avatar));
            }

            if (!connectedSidebarUsers.contains(user->id)) {
                connectedSidebarUsers.insert(user->id);

                // Avatars are low-priority requests and are now limited to the
                // actually visible DM set rather than the entire user directory.
                if (user->avatar.isNull()) {
                    backend.retrieveUserAvatar(user->id);
                }

                connect(user, &BackendUser::onStatusChanged, this, [this, user] {
                    if (!backendForSidebar) {
                        return;
                    }
                    BackendChannel* directChannel = backendForSidebar->getStorage().getDirectChannelByUserId(user->id);
                    if (!directChannel) {
                        return;
                    }
                    const auto items = channelToItemMap.value(directChannel->id);
                    for (QTreeWidgetItem* row : items) {
                        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
                            static_cast<ChannelItem*>(row)->setStatus(user->status);
                        }
                    }
                });

                connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
                    if (!backendForSidebar) {
                        return;
                    }
                    BackendChannel* directChannel = backendForSidebar->getStorage().getDirectChannelByUserId(user->id);
                    if (!directChannel) {
                        return;
                    }
                    const auto items = channelToItemMap.value(directChannel->id);
                    for (QTreeWidgetItem* row : items) {
                        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
                            static_cast<ChannelItem*>(row)->setIcon(QIcon(user->avatar));
                        }
                    }
                });
            }
        }
    }

    // A channel may appear in multiple sidebar categories and category refreshes
    // routinely destroy/recreate the QTreeWidgetItem objects. Never capture a
    // row pointer in the long-lived BackendChannel::onLeave connection. The
    // member slot resolves the currently alive rows from channelToItemMap.
    QObject::connect(&channel, &BackendChannel::onLeave,
                     this, &ChannelTree::handleChannelLeave,
                     Qt::UniqueConnection);
    QObject::connect(&channel, &BackendChannel::onUpdated,
                     this, &ChannelTree::handleChannelUpdated,
                     Qt::UniqueConnection);

    addChannelToItem(channel.id, item);
    auto& sidebar = SidebarService::instance(backend);
    item->setMuted(sidebar.isChannelMuted(channel));
    item->setUnread(sidebar.isChannelUnread(channel));
    item->setMentioned(sidebar.hasUnreadMention(channel.id));
    return item;
}

void ChannelTree::handleChannelLeave()
{
    auto* channel = qobject_cast<BackendChannel*>(sender());
    if (!channel) {
        return;
    }

    const QString channelId = channel->id;
    const QList<QTreeWidgetItem*> items = channelToItemMap.value(channelId);
    if (sidebarDragActive) {
        for (QTreeWidgetItem* item : items) {
            QTreeWidgetItem* category = item ? item->parent() : nullptr;
            if (!category) {
                continue;
            }
            const QString teamId = category->data(0, ItemTeamIdRole).toString();
            if (!teamId.isEmpty()) {
                pendingSidebarRefreshTeams.insert(teamId);
            }
        }
        return;
    }

    for (QTreeWidgetItem* item : items) {
        if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
            continue;
        }

        removeChannelToItem(channelId, item);
        ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
        if (chatArea && chatAreaStackedWidget) {
            chatAreaStackedWidget->removeWidget(chatArea);
        }
        if (QTreeWidgetItem* parent = item->parent()) {
            parent->removeChild(item);
        }
        delete chatArea;
        delete item;
    }

    channelToItemMap.remove(channelId);
}

ChatArea* ChannelTree::ensureChatArea(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar || !chatAreaStackedWidget) {
        return nullptr;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind != ChannelItemKind && kind != VirtualDestinationItemKind) {
        return nullptr;
    }

    if (ChatArea* existing = item->data(0, Qt::UserRole).value<ChatArea*>()) {
        return existing;
    }

    QString channelId = kind == ChannelItemKind
        ? item->data(0, ItemIdRole).toString()
        : item->data(0, ItemChannelIdRole).toString();
    if (channelId.isEmpty() && kind == VirtualDestinationItemKind
        && item->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {
        if (BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
                backendForSidebar->getLoginUser().id)) {
            channelId = personal->id;
            item->setData(0, ItemChannelIdRole, channelId);
        }
    }
    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!channel) {
        LOG_DEBUG("Cannot create chat area: channel not found: " << channelId);
        return nullptr;
    }

    auto* channelItem = static_cast<ChannelItem*>(item);
    auto* chatArea = new ChatArea(*backendForSidebar, *channel, channelItem,
                                  chatAreaStackedWidget, false);
    chatAreaStackedWidget->addWidget(chatArea);
    item->setData(0, Qt::UserRole, QVariant::fromValue(chatArea));
    return chatArea;
}

void ChannelTree::activateChannelItem(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == VirtualDestinationItemKind) {
        activateVirtualDestination(item);
        return;
    }
    if (kind != ChannelItemKind) {
        return;
    }

    if (backendForSidebar) {
        BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
            backendForSidebar->getLoginUser().id);
        if (personal && item->data(0, ItemIdRole).toString() == personal->id) {
            if (QTreeWidgetItem* alias = personalItemForTeam(
                    item->data(0, ItemTeamIdRole).toString())) {
                setCurrentItem(alias);
                return;
            }
        }
    }

    ChatArea* newPage = ensureChatArea(item);
    if (!newPage) {
        return;
    }

    if (newPage == getCurrentPage()) {
        if (!newPage->isVisible()) {
            chatAreaStackedWidget->setCurrentWidget(newPage);
        }

        // QStackedWidget automatically makes its first added widget current.
        // With lazy ChatArea creation that means the first selected channel can
        // already be getCurrentPage() even though onActivate()/init() have never
        // run. Activation is idempotent, so always establish the backend current
        // channel and initialize the page on an explicit channel activation.
        newPage->onActivate();
        qDebug() << "Item Activated: " << newPage->channel.display_name;
        return;
    }

    if (ChatArea* currentPage = getCurrentPage()) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(newPage);
    newPage->onActivate();

    qDebug() << "Item Activated: " << newPage->channel.display_name;
}

void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar
        || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind) {
        return;
    }

    const int destination = item->data(0, ItemDestinationRole).toInt();
    if (destination == SidebarItem::SavedDestination
        || destination == SidebarItem::DraftsDestination) {
        emit virtualDestinationRequested(destination,
                                         item->data(0, ItemTeamIdRole).toString());
        return;
    }
    if (destination != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(
        backendForSidebar->getLoginUser().id);
    if (!channel) {
        const QString teamId = item->data(0, ItemTeamIdRole).toString();
        QPointer<ChannelTree> guard(this);
        backendForSidebar->createDirectChannel(backendForSidebar->getLoginUser(),
            [guard, teamId](BackendChannel& created) {
                if (!guard) {
                    return;
                }
                QTreeWidgetItem* currentPersonal = guard->personalItemForTeam(teamId);
                if (!currentPersonal) {
                    return;
                }
                currentPersonal->setData(0, ItemChannelIdRole, created.id);
                guard->refreshPersonalItems();
                // Do not steal focus if the user navigated elsewhere while the
                // first-ever self-DM was being created.
                if (guard->currentItem() == currentPersonal) {
                    guard->activateVirtualDestination(currentPersonal);
                }
            });
        return;
    }

    item->setData(0, ItemChannelIdRole, channel->id);
    ChatArea* newPage = ensureChatArea(item);
    if (!newPage) {
        return;
    }

    if (newPage == getCurrentPage()) {
        if (!newPage->isVisible()) {
            chatAreaStackedWidget->setCurrentWidget(newPage);
        }
        newPage->onActivate();
        return;
    }

    if (ChatArea* currentPage = getCurrentPage()) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(newPage);
    newPage->onActivate();
}

void ChannelTree::addGroupChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::addDirectChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::setChatAreaStackedWidget (QStackedWidget* stackedWidget)
{
	chatAreaStackedWidget = stackedWidget;
}

void ChannelTree::openChannel (QString channelID)
{
	auto it = channelToItemMap.find (channelID);
    QTreeWidgetItem* item = nullptr;

    if (it != channelToItemMap.end() && !it.value().isEmpty()) {
        item = it.value().front();
    } else if (backendForSidebar) {
        BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
            backendForSidebar->getLoginUser().id);
        if (personal && personal->id == channelID) {
            // Programmatic navigation (permalinks, Saved/Search results, etc.)
            // must resolve the self-DM even when its ordinary server row is
            // absent or suppressed in Favorites. Prefer the currently selected
            // team's Personal row, otherwise use the first available one.
            QTreeWidgetItem* current = currentItem();
            QTreeWidgetItem* teamItem = current;
            while (teamItem && teamItem->parent()) {
                teamItem = teamItem->parent();
            }
            if (teamItem && teamItem->data(0, ItemKindRole).toInt() == TeamItemKind) {
                item = personalItemForTeam(teamItem->data(0, ItemTeamIdRole).toString());
            }
            if (!item) {
                for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
                    item = personalItemForTeam(teamIt.key());
                    if (item) {
                        break;
                    }
                }
            }
        }
    }

    if (!item) {
        qDebug() << "openChannel " << channelID << ": channel not found";
        return;
    }

    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

void ChannelTree::addChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto& items = channelToItemMap[channelID];
	if (!items.contains(item)) {
		items.push_back(item);
	}
}

void ChannelTree::removeChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto it = channelToItemMap.find(channelID);
	if (it == channelToItemMap.end()) {
		return;
	}

	if (item) {
		it.value().removeAll(item);
	} else {
		it.value().clear();
	}

	if (it.value().isEmpty()) {
		channelToItemMap.erase(it);
	}
}

void ChannelTree::showContextMenu (const QPoint& pos)
{
	QTreeWidgetItem* item = itemAt(pos);
    if (!item || item->data(0, ItemKindRole).toInt() == CategoryItemKind) {
		return;
	}

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind != TeamItemKind && kind != ChannelItemKind) {
        return;
    }

    auto* pointedItem = static_cast<ChannelTreeItem*>(item);
	pointedItem->showContextMenu (mapToGlobal(pos) + QPoint (25, 15));
}

ChatArea* ChannelTree::getCurrentPage ()
{
    if (!chatAreaStackedWidget) {
        return nullptr;
    }
	return qobject_cast<ChatArea*> (chatAreaStackedWidget->currentWidget());
}

void ChannelTree::setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed)
{
    if (!backendForSidebar || !item || item->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    if (!category || category->collapsed == collapsed) {
        return;
    }

    category->collapsed = collapsed;
    SidebarService::instance(*backendForSidebar).updateCategory(*category);
}

void ChannelTree::setChannelMutedVisual(const QString& channelId, bool muted)
{
    const auto items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMuted(muted);
        }
    }
}

void ChannelTree::refreshChannelUnreadVisual(const QString& channelId)
{
    if (!backendForSidebar) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    setChannelUnreadVisual(
        channelId, SidebarService::instance(*backendForSidebar).isChannelUnread(*channel));
}

void ChannelTree::refreshAllChannelActivityVisuals()
{
    if (!backendForSidebar) {
        return;
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);
    const QStringList channelIds = channelToItemMap.keys();
    for (const QString& channelId : channelIds) {
        BackendChannel* channel =
            backendForSidebar->getStorage().getChannelById(channelId);
        if (!channel) {
            continue;
        }

        setChannelUnreadVisual(
            channelId, sidebar.isChannelUnread(*channel));
        setChannelMentionedVisual(
            channelId, sidebar.hasUnreadMention(channelId));
    }
}

void ChannelTree::setChannelUnreadVisual(const QString& channelId, bool unread)
{
    const auto items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setUnread(unread);
        }
    }
}

void ChannelTree::setChannelMentionedVisual(const QString& channelId, bool mentioned)
{
    const auto items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMentioned(mentioned);
        }
    }
}

bool ChannelTree::canRemoveChannelFromCategory(const ChannelItem* item) const
{
    if (!backendForSidebar || !item || !item->parent()) {
        return false;
    }

    QTreeWidgetItem* categoryItem = item->parent();
    if (categoryItem->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return false;
    }

    const QString teamId = categoryItem->data(0, ItemTeamIdRole).toString();
    const QString categoryId = categoryItem->data(0, ItemIdRole).toString();
    const SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    return category && (category->type == QStringLiteral("custom")
                        || category->type == QStringLiteral("favorites"));
}

void ChannelTree::removeChannelFromCategory(ChannelItem* item)
{
    if (!canRemoveChannelFromCategory(item) || !backendForSidebar) {
        return;
    }

    QTreeWidgetItem* sourceCategoryItem = item->parent();
    QTreeWidgetItem* teamItem = sourceCategoryItem ? sourceCategoryItem->parent() : nullptr;
    const QString channelId = item->data(0, ItemIdRole).toString();
    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!teamItem || !channel) {
        return;
    }

    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    SidebarCategory* sourceCategory = state
        ? state->category(sourceCategoryItem->data(0, ItemIdRole).toString()) : nullptr;
    if (!sourceCategory) {
        return;
    }

    const bool direct = channel->type == BackendChannel::directChannel
        || channel->type == BackendChannel::groupChannel;
    SidebarCategory* targetCategory = state->categoryByType(
        direct ? QStringLiteral("direct_messages") : QStringLiteral("channels"));
    if (!targetCategory || targetCategory->id == sourceCategory->id) {
        return;
    }

    sourceCategory->channelIds.removeAll(channelId);
    targetCategory->channelIds.removeAll(channelId);
    targetCategory->channelIds.push_back(channelId);

    QVector<SidebarCategory> updates {*sourceCategory, *targetCategory};
    SidebarService::instance(*backendForSidebar).updateCategories(teamId, updates,
        [this, teamId](const SidebarTeamState&) {
            if (!backendForSidebar) {
                return;
            }
            BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId);
            if (team) {
                refreshTeamSidebar(*backendForSidebar, *team);
            }
        });
}

void ChannelTree::refreshSidebarTeam(const QString& teamId)
{
    if (!backendForSidebar) {
        return;
    }
    if (BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId)) {
        refreshTeamSidebar(*backendForSidebar, *team);
    }
}

QVector<QPair<QString, QString>> ChannelTree::customCategoryTargets(const ChannelItem* item) const
{
    QVector<QPair<QString, QString>> result;
    if (!backendForSidebar || !item || !item->parent()) {
        return result;
    }

    const QTreeWidgetItem* sourceCategoryItem = item->parent();
    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    const QString sourceCategoryId = sourceCategoryItem->data(0, ItemIdRole).toString();
    const SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    if (!state) {
        return result;
    }

    for (const QString& categoryId : state->order) {
        const SidebarCategory* category = state->category(categoryId);
        if (!category || !category->isCustom() || category->id == sourceCategoryId) {
            continue;
        }
        result.push_back(qMakePair(category->id, categoryDisplayName(*category)));
    }
    return result;
}

void ChannelTree::createGroupAndMoveChannel(ChannelItem* item)
{
    if (!backendForSidebar || !item || !item->parent()) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString channelId = item->data(0, ItemIdRole).toString();
    const QString sourceCategoryId = item->parent()->data(0, ItemIdRole).toString();
    if (teamId.isEmpty() || channelId.isEmpty() || sourceCategoryId.isEmpty()) {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Create group"), tr("Group name:"), QLineEdit::Normal,
        QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    QPointer<ChannelTree> guard(this);
    SidebarService::instance(*backendForSidebar).createCategory(
        teamId, name,
        [guard, teamId, sourceCategoryId, channelId](const SidebarCategory& created) {
            if (!guard || !guard->backendForSidebar || created.id.isEmpty()) {
                return;
            }

            auto& sidebar = SidebarService::instance(*guard->backendForSidebar);
            SidebarTeamState* state = sidebar.teamState(teamId);
            const SidebarCategory* source = state ? state->category(sourceCategoryId) : nullptr;
            const SidebarCategory* target = state ? state->category(created.id) : nullptr;
            if (!target) {
                return;
            }

            QVector<SidebarCategory> updates;
            if (source && source->id != target->id) {
                SidebarCategory sourceUpdate = *source;
                sourceUpdate.channelIds.removeAll(channelId);
                updates.push_back(std::move(sourceUpdate));
            }

            SidebarCategory targetUpdate = *target;
            targetUpdate.channelIds.removeAll(channelId);
            targetUpdate.channelIds.push_back(channelId);
            targetUpdate.sorting = QStringLiteral("manual");
            updates.push_back(std::move(targetUpdate));

            sidebar.updateCategories(teamId, updates,
                [guard, teamId](const SidebarTeamState&) {
                    if (guard) {
                        guard->refreshSidebarTeam(teamId);
                    }
                });
        });
}

void ChannelTree::moveChannelToCategory(ChannelItem* item, const QString& categoryId)
{
    moveChannel(item, categoryId, {}, false, false);
}

void ChannelTree::moveChannel(ChannelItem* item,
                              const QString& targetCategoryId,
                              const QString& targetChannelId,
                              bool afterTarget,
                              bool explicitPosition)
{
    if (!backendForSidebar || !item || !item->parent() || targetCategoryId.isEmpty()) {
        return;
    }

    QTreeWidgetItem* sourceCategoryItem = item->parent();
    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    auto& sidebar = SidebarService::instance(*backendForSidebar);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state) {
        return;
    }

    const QString sourceCategoryId = sourceCategoryItem->data(0, ItemIdRole).toString();
    const QString channelId = item->data(0, ItemIdRole).toString();
    const SidebarCategory* sourceCategory = state->category(sourceCategoryId);
    const SidebarCategory* targetCategory = state->category(targetCategoryId);
    if (!sourceCategory || !targetCategory || targetCategory->teamId != teamId
        || channelId.isEmpty()) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    SidebarCategory* sourceUpdate = optimistic.category(sourceCategoryId);
    SidebarCategory* targetUpdate = optimistic.category(targetCategoryId);
    if (!sourceUpdate || !targetUpdate) {
        return;
    }

    bool changed = false;
    const bool sameCategory = sourceCategoryId == targetCategoryId;
    if (sameCategory) {
        changed = reorderSidebarChannel(sourceUpdate->channelIds, channelId,
                                        targetChannelId, afterTarget);
        if (changed) {
            sourceUpdate->sorting = QStringLiteral("manual");
        }
    } else {
        changed = moveSidebarChannel(sourceUpdate->channelIds, targetUpdate->channelIds,
                                     channelId, targetChannelId, afterTarget);
        if (changed && explicitPosition) {
            targetUpdate->sorting = QStringLiteral("manual");
        }
    }
    if (!changed) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamId];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamId, nullptr)) {
        if (const SidebarTeamState* local = sidebar.teamState(teamId)) {
            reconcileTeamSidebar(*backendForSidebar, *teamItem, *local);
        }
    }

    QPointer<ChannelTree> guard(this);
    auto verify = [guard, teamId, mutation] {
        if (!guard || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        guard->verifySidebarTeam(teamId, mutation);
    };
    auto rollback = [guard, teamId, mutation, before] {
        if (!guard || !guard->backendForSidebar
            || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        auto& currentSidebar = SidebarService::instance(*guard->backendForSidebar);
        currentSidebar.applyLocalTeamState(teamId, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
            guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, before);
        }
        // The rollback is immediate; the GET is only an authoritative check and
        // is reconciled incrementally when it arrives.
        guard->verifySidebarTeam(teamId, mutation);
    };

    if (sameCategory) {
        const SidebarCategory payload = *optimistic.category(sourceCategoryId);
        sidebar.updateCategory(
            payload,
            [verify](const SidebarCategory&) { verify(); },
            rollback,
            false);
    } else {
        const QVector<SidebarCategory> payload {
            *optimistic.category(sourceCategoryId),
            *optimistic.category(targetCategoryId),
        };
        sidebar.updateCategories(
            teamId, payload,
            [verify](const SidebarTeamState&) { verify(); },
            rollback,
            false);
    }
}

bool ChannelTree::resolveChannelDropTarget(QTreeWidgetItem* source,
                                           const QPoint& pos,
                                           QTreeWidgetItem*& targetCategoryItem,
                                           QString& targetChannelId,
                                           bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    targetChannelId.clear();
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return false;
    }

    QTreeWidgetItem* target = itemAt(pos);
    if (!target || target == source) {
        return false;
    }

    const int targetKind = target->data(0, ItemKindRole).toInt();
    if (targetKind == ChannelItemKind) {
        targetCategoryItem = target->parent();
        targetChannelId = target->data(0, ItemIdRole).toString();
        const QRect rect = dragContentRect(target, visualItemRect(target));
        afterTarget = rect.isValid() && pos.y() >= rect.center().y();
    } else if (targetKind == CategoryItemKind) {
        targetCategoryItem = target;
    } else if (targetKind == VirtualDestinationItemKind) {
        // Personal/Saved/Drafts are presentation-only leading rows directly under
        // the team item; they have no position in any category.channel_ids.
        // Treat dropping on them as invalid rather than inventing an ordinal.
        return false;
    } else {
        return false;
    }

    QTreeWidgetItem* sourceCategoryItem = source->parent();
    QTreeWidgetItem* sourceTeamItem = sourceCategoryItem ? sourceCategoryItem->parent() : nullptr;
    return targetCategoryItem
        && targetCategoryItem->data(0, ItemKindRole).toInt() == CategoryItemKind
        && sourceTeamItem
        && targetCategoryItem->parent() == sourceTeamItem;
}

void ChannelTree::moveCategory(QTreeWidgetItem* item,
                               const QString& targetCategoryId,
                               bool afterTarget)
{
    if (!backendForSidebar || !item || targetCategoryId.isEmpty()
        || item->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    auto& sidebar = SidebarService::instance(*backendForSidebar);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state || categoryId.isEmpty() || !state->category(categoryId)
        || !state->category(targetCategoryId)) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    if (!reorderSidebarCategory(optimistic.order, categoryId,
                                targetCategoryId, afterTarget)) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamId];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamId, nullptr)) {
        reconcileTeamSidebar(*backendForSidebar, *teamItem, optimistic);
    }

    QPointer<ChannelTree> guard(this);
    auto rollback = [guard, teamId, mutation, before] {
        if (!guard || !guard->backendForSidebar
            || guard->sidebarMutationGeneration.value(teamId) != mutation) {
            return;
        }
        auto& currentSidebar = SidebarService::instance(*guard->backendForSidebar);
        currentSidebar.applyLocalTeamState(teamId, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
            guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, before);
        }
        guard->verifySidebarTeam(teamId, mutation);
    };

    sidebar.updateCategoryOrder(
        teamId, optimistic.order,
        [guard, teamId, mutation] {
            if (guard && guard->sidebarMutationGeneration.value(teamId) == mutation) {
                guard->verifySidebarTeam(teamId, mutation);
            }
        },
        rollback,
        false);
}

bool ChannelTree::resolveCategoryDropTarget(QTreeWidgetItem* source,
                                            const QPoint& pos,
                                            QTreeWidgetItem*& targetCategoryItem,
                                            bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != CategoryItemKind
        || !source->parent() || categoryDragBoundaries.isEmpty()) {
        return false;
    }

    // Match AnyKeep's GenericReorderController: compare the dragged block's
    // leading edge against insertion boundaries computed after removing the
    // source.  Nearest-boundary selection means a neighbour moves once the
    // leading edge has crossed half of that neighbour's extent.  Moving up and
    // down therefore naturally use the two different half-overlap thresholds.
    const int probe = draggedBlockStartLogicalY + (pos.y() - dragStartPointerY);
    const CategoryDragBoundary* boundary = nearestCategoryDragBoundary(probe);
    if (!boundary || !boundary->targetCategory.isValid()) {
        return false;
    }

    targetCategoryItem = itemFromIndex(boundary->targetCategory);
    afterTarget = boundary->afterTarget;
    return targetCategoryItem
        && targetCategoryItem->data(0, ItemKindRole).toInt() == CategoryItemKind
        && targetCategoryItem->parent() == source->parent();
}

void ChannelTree::dragMoveEvent(QDragMoveEvent* event)
{
    // ChannelTree owns drag target resolution and structural displacement.
    // Calling QTreeWidget::dragMoveEvent() here would also update Qt's
    // InternalMove state from the already modified row geometry, so even a
    // horizontal-only pointer move could perturb the logical drop target.
    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            restoreSourceDropGap(true);
            event->ignore();
            return;
        }
        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {
            ensureDragSourceVisuals(source);
            restoreSourceDropGap(true);
            event->setDropAction(Qt::MoveAction);
            event->accept();
            return;
        }
        bool gapAfter = afterTarget;
        QTreeWidgetItem* anchor = categoryDropGapAnchor(
            targetCategoryItem, afterTarget, gapAfter);
        updateDragVisuals(source, anchor, gapAfter);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        restoreSourceDropGap(true);
        event->ignore();
        return;
    }

    if (!channelDropChangesPosition(source, targetCategoryItem,
                                    targetChannelId, afterTarget)) {
        ensureDragSourceVisuals(source);
        restoreSourceDropGap(true);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    bool gapAfter = afterTarget;
    QTreeWidgetItem* anchor = channelDropGapAnchor(
        source, targetCategoryItem, targetChannelId, afterTarget, gapAfter);
    updateDragVisuals(source, anchor, gapAfter);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void ChannelTree::dropEvent(QDropEvent* event)
{
    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }
    const QPoint pos = dropEventPosition(event);

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            resetDragVisuals(false);
            event->ignore();
            return;
        }

        const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {
            resetDragVisuals(false);
            event->setDropAction(Qt::MoveAction);
            event->accept();
            return;
        }
        resetDragVisuals(false);
        moveCategory(source, targetCategoryId, afterTarget);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        resetDragVisuals(false);
        event->ignore();
        return;
    }

    auto* channelItem = static_cast<ChannelItem*>(source);
    const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
    if (!channelDropChangesPosition(source, targetCategoryItem,
                                    targetChannelId, afterTarget)) {
        resetDragVisuals(false);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    resetDragVisuals(false);
    moveChannel(channelItem, targetCategoryId, targetChannelId,
                afterTarget, !targetChannelId.isEmpty());
    event->setDropAction(Qt::MoveAction);
    event->accept();
}


} // namespace Mattermost
