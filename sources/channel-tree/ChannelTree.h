/**
 * @file ChannelTree.h
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

#pragma once

#include <QMap>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTreeWidget>
#include <QVector>

#include "ChannelTreeItem.h"
#include "SidebarItem.h"

class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMouseEvent;
class QPainter;
class QStackedWidget;
class QTreeWidgetItem;
class QVariantAnimation;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendTeam;
class ChatArea;
class TeamItem;
class ChannelItem;
struct SidebarTeamState;

class ChannelTree: public QTreeWidget {
	Q_OBJECT
public:
    using ItemKind = SidebarItem::Kind;
    using ItemRole = SidebarItem::Role;

    // Compatibility names for the existing ChannelTree implementation and
    // callers. SidebarItem is the canonical contract shared by all sidebar
    // views; new code should use it directly.
    static constexpr ItemKind UnknownItem = SidebarItem::Unknown;
    static constexpr ItemKind TeamItemKind = SidebarItem::Team;
    static constexpr ItemKind CategoryItemKind = SidebarItem::Category;
    static constexpr ItemKind ChannelItemKind = SidebarItem::Channel;
    static constexpr ItemKind VirtualDestinationItemKind = SidebarItem::VirtualDestination;

    static constexpr ItemRole ItemKindRole = SidebarItem::KindRole;
    static constexpr ItemRole ItemIdRole = SidebarItem::IdRole;
    static constexpr ItemRole ItemTeamIdRole = SidebarItem::TeamIdRole;
    static constexpr ItemRole ItemMutedRole = SidebarItem::MutedRole;
    static constexpr ItemRole ItemMentionedRole = SidebarItem::MentionedRole;
    static constexpr ItemRole ItemStatusRole = SidebarItem::PresenceRole;
    static constexpr ItemRole ItemUnreadRole = SidebarItem::UnreadRole;
    static constexpr ItemRole ItemLifetimeRole = SidebarItem::LifetimeRole;
    static constexpr ItemRole ItemChannelTypeRole = SidebarItem::ChannelTypeRole;
    static constexpr ItemRole ItemChannelIdRole = SidebarItem::ChannelIdRole;
    static constexpr ItemRole ItemDestinationRole = SidebarItem::DestinationRole;

	ChannelTree (QWidget* parent = nullptr);
	virtual ~ChannelTree ();
public:
	bool isChannelActive (const BackendChannel& channel);
	Backend* backendInstance() const { return backendForSidebar; }

    // ChannelItemDelegate requests this lazily when a group-DM row becomes
    // visible. Keeping the request on the tree side avoids backend/network work
    // inside paint() and keeps the delegate independently testable.
    Q_INVOKABLE void ensureGroupChannelDisplayName(const QString& channelId);

	void addTeam (Backend& backend, BackendTeam& team);
	void populateSidebars(Backend& backend);

	// Kept for source compatibility; the server-backed sidebar no longer uses
	// separate global DM/GM lists.
	void addDirectChannelsList (Backend& backend);
	void addGroupChannelsList (Backend& backend);

	void setChatAreaStackedWidget (QStackedWidget* chatAreaStackedWidget);
	ChatArea* getCurrentPage ();

	void openChannel (QString channelID);
	// Programmatic navigation may target a valid channel that is currently
	// outside the visible DM/GM sidebar limit. Materialize its server-category
	// row first, then activate it through the same path as an ordinary click.
	void openStoredChannel(QString channelID);
    // A realtime direct/group channel can reach Storage before the server-backed
    // sidebar category is refreshed. Admit it into every loaded Direct Messages
    // category without opening the conversation or issuing a category reload.
    void admitStoredConversation(BackendChannel& channel);
	void addChannelToItem (QString channelID, QTreeWidgetItem* item);
	void removeChannelToItem (QString channelID, QTreeWidgetItem* item = nullptr);

	// Recent and Attention are alternate views over the same channel objects.
	// Route their context-menu requests back through the real ChannelItem so all
	// actions and handlers stay identical to the Channels tab.
	void showChannelContextMenu(const QString& channelID, const QPoint& globalPos)
	{
		auto it = channelToItemMap.constFind(channelID);
		if (it == channelToItemMap.cend()) {
			return;
		}
		for (QTreeWidgetItem* item : it.value()) {
			if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
				continue;
			}
			static_cast<ChannelTreeItem*>(item)->showContextMenu(globalPos);
			return;
		}
	}

	bool canRemoveChannelFromCategory(const ChannelItem* item) const;
	void removeChannelFromCategory(ChannelItem* item);
	QVector<QPair<QString, QString>> customCategoryTargets(const ChannelItem* item) const;
	void moveChannelToCategory(ChannelItem* item, const QString& categoryId);
	void createGroupAndMoveChannel(ChannelItem* item);

signals:
    void virtualDestinationRequested(int destination, const QString& teamId);
    /** Completion of a stored-channel open that required asynchronous admission/join. */
    void storedChannelOpenFinished(const QString& channelId, bool opened);

protected:
	void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
	void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
	void refreshCurrentChannelReadState(QTreeWidgetItem* item);
	void showContextMenu (const QPoint& pos);
	void handleChannelLeave();
    void handleChannelUpdated();
	void refreshTeamSidebar(Backend& backend, BackendTeam& team);
	void renderTeamSidebar(Backend& backend, TeamItem& teamItem,
	                       const SidebarTeamState& state);
    void reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,
                              const SidebarTeamState& state);
    void destroySidebarRow(QTreeWidgetItem* item);
	void clearTeamSidebar(TeamItem& teamItem);
	QTreeWidgetItem* createCategoryItem(TeamItem& teamItem, const QString& categoryId,
	                                    const QString& displayName, bool collapsed);
	ChannelItem* createChannelItem(Backend& backend, TeamItem& teamItem,
	                               QTreeWidgetItem& categoryItem, BackendChannel& channel);
    ChannelItem* createPersonalItem(Backend& backend, TeamItem& teamItem);
    ChannelItem* createSavedItem(Backend& backend, TeamItem& teamItem);
    ChannelItem* createDraftsItem(Backend& backend, TeamItem& teamItem);
    QTreeWidgetItem* personalItemForTeam(const QString& teamId) const;
    void refreshPersonalItems();
	ChatArea* ensureChatArea(QTreeWidgetItem* item);
	void activateChannelItem(QTreeWidgetItem* item);
    void activateVirtualDestination(QTreeWidgetItem* item);
	void setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed);
	void setChannelMutedVisual(const QString& channelId, bool muted);
    void refreshChannelUnreadVisual(const QString& channelId);
    void refreshAllChannelActivityVisuals();
	void setChannelUnreadVisual(const QString& channelId, bool unread);
	void setChannelMentionedVisual(const QString& channelId, bool mentioned);
	bool resolveChannelDropTarget(QTreeWidgetItem* source, const QPoint& pos,
	                              QTreeWidgetItem*& targetCategoryItem,
	                              QString& targetChannelId, bool& afterTarget) const;
    struct CategoryDragBoundary {
        int position = 0;
        QPersistentModelIndex targetCategory;
        bool afterTarget = false;
    };

    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,
                                   QTreeWidgetItem*& targetCategoryItem,
                                   bool& afterTarget) const;
    void prepareCategoryDragBoundaries(QTreeWidgetItem* source);
    const CategoryDragBoundary* nearestCategoryDragBoundary(int probe) const;
    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
    void restoreSourceDropGap(bool animate);
    void clearDropGap(bool animate);
    void resetDragVisuals(bool animate);
    void ensureDragSourceVisuals(QTreeWidgetItem* source);
    QTreeWidgetItem* sourceDropGapAnchor(QTreeWidgetItem* source, bool& gapAfter) const;
    void animateSourceCollapse(qreal target);
    void animateDropGap(const QPersistentModelIndex& target, bool after, int extent);
    QTreeWidgetItem* channelDropGapAnchor(QTreeWidgetItem* source,
                                            QTreeWidgetItem* targetCategoryItem,
                                            const QString& targetChannelId,
                                            bool afterTarget, bool& gapAfter) const;
    QTreeWidgetItem* categoryDropGapAnchor(QTreeWidgetItem* targetCategoryItem,
                                             bool afterTarget, bool& gapAfter) const;
	void moveChannel(ChannelItem* item, const QString& targetCategoryId,
	                 const QString& targetChannelId, bool afterTarget,
	                 bool explicitPosition);
    void moveCategory(QTreeWidgetItem* item, const QString& targetCategoryId,
                      bool afterTarget);
	void refreshSidebarTeam(const QString& teamId);
    void verifySidebarTeam(const QString& teamId, quint64 mutation);
    void flushDeferredSidebarReconciles();
    void refreshPaletteDependentIcons();

	QStackedWidget*						chatAreaStackedWidget;
	QMap<QString, QList<QTreeWidgetItem*>>	channelToItemMap;
	QMap<QString, TeamItem*>			teamToItemMap;
	QSet<QString>						connectedSidebarUsers;
    QSet<QString>                        pendingChannelAdmissions;
	Backend*							backendForSidebar;
	bool							renderingSidebar;
    bool                                personalUserConnected = false;
    // True for the complete nested QDrag::exec() lifetime. Structural sidebar
    // mutations from network/realtime callbacks are deferred while this is set.
    bool                                sidebarDragActive = false;
    QPersistentModelIndex               categoryActionHoverIndex;
    bool                                categoryActionHovered = false;
    QVariantAnimation*                  sourceCollapseAnimation = nullptr;
    QVariantAnimation*                  dropGapAnimation = nullptr;
    QVector<QPersistentModelIndex>      dragSourceIndexes;
    QVector<QPersistentModelIndex>      dragGapIndexes;
    QPersistentModelIndex               currentDragGapIndex;
    QPersistentModelIndex               sourceDragGapIndex;
    bool                                currentDragGapAfter = false;
    bool                                sourceDragGapAfter = false;
    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    int                                 draggedBlockHotSpotY = 0;
    int                                 dragStartPointerY = 0;
    int                                 draggedBlockStartLogicalY = 0;
    QVector<CategoryDragBoundary>       categoryDragBoundaries;
    QSet<QString>                       pendingSidebarReconcileTeams;
    QSet<QString>                       pendingSidebarRefreshTeams;
    QSet<QString>                       pendingStoredChannelOpens;
    QMap<QString, quint64>              sidebarMutationGeneration;
};

} /* namespace Mattermost */
