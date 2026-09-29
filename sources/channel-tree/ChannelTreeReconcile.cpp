/**
 * @file ChannelTreeReconcile.cpp
 * @brief Incremental reconciliation of the server sidebar with the live tree.
 */

#include "ChannelTree.h"

#include <QPointer>
#include <QSet>
#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

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

QTreeWidgetItem* findChannelChild(QTreeWidgetItem* category, const QString& channelId)
{
    if (!category) {
        return nullptr;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* row = category->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel
            && row->data(0, SidebarItem::IdRole).toString() == channelId) {
            return row;
        }
    }
    return nullptr;
}

// The three virtual destinations (Personal / Saved / Drafts) are always-present
// leading direct children of each team item, ordered before every category.
// Reconcile must place and preserve exactly this many rows in this order.
constexpr int kLeadingDestinationCount = 3;

QTreeWidgetItem* findVirtualRow(QTreeWidgetItem* team, int destination)
{
    if (!team) {
        return nullptr;
    }
    for (int i = 0; i < team->childCount(); ++i) {
        QTreeWidgetItem* row = team->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::VirtualDestination
            && row->data(0, SidebarItem::DestinationRole).toInt() == destination) {
            return row;
        }
    }
    return nullptr;
}

void moveChild(QTreeWidgetItem& parent, QTreeWidgetItem* child, int index)
{
    if (!child || child->parent() != &parent) {
        return;
    }
    const int current = parent.indexOfChild(child);
    if (current < 0 || current == index) {
        return;
    }
    parent.takeChild(current);
    parent.insertChild(index, child);
}

} // namespace

void ChannelTree::verifySidebarTeam(const QString& teamId, quint64 mutation)
{
    if (!backendForSidebar
        || sidebarMutationGeneration.value(teamId) != mutation) {
        return;
    }

    BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId);
    if (!team) {
        return;
    }

    QPointer<ChannelTree> guard(this);
    SidebarService::instance(*backendForSidebar).retrieveCategories(
        *team,
        [guard, teamId, mutation](const SidebarTeamState& serverState) {
            if (!guard || !guard->backendForSidebar
                || guard->sidebarMutationGeneration.value(teamId) != mutation) {
                return;
            }
            auto& sidebar = SidebarService::instance(*guard->backendForSidebar);
            sidebar.applyLocalTeamState(teamId, serverState);
            if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
                guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, serverState);
            }
        },
        {},
        false);
}

void ChannelTree::flushDeferredSidebarReconciles()
{
    if (!backendForSidebar || sidebarDragActive) {
        return;
    }
    if (pendingSidebarReconcileTeams.isEmpty()
        && pendingSidebarRefreshTeams.isEmpty()
        && pendingStoredChannelOpens.isEmpty()) {
        return;
    }

    const QSet<QString> pendingReconciles = pendingSidebarReconcileTeams;
    const QSet<QString> pendingRefreshes = pendingSidebarRefreshTeams;
    const QSet<QString> pendingOpens = pendingStoredChannelOpens;
    pendingSidebarReconcileTeams.clear();
    pendingSidebarRefreshTeams.clear();
    pendingStoredChannelOpens.clear();

    auto& sidebar = SidebarService::instance(*backendForSidebar);
    for (const QString& teamId : pendingReconciles) {
        TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
        const SidebarTeamState* state = sidebar.teamState(teamId);
        if (!teamItem || !state) {
            continue;
        }
        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);
    }

    // A channel/team leave mutates the tree directly in the legacy path. When
    // it occurs during DnD, refresh authoritatively after the drag instead.
    for (const QString& teamId : pendingRefreshes) {
        refreshSidebarTeam(teamId);
    }

    // Programmatic materialization can also create rows directly. Resume it
    // only after the drag-owned tree structure has been released.
    for (const QString& channelId : pendingOpens) {
        openStoredChannel(channelId);
    }
}

void ChannelTree::destroySidebarRow(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == ChannelItemKind) {
        removeChannelToItem(item->data(0, ItemIdRole).toString(), item);
    }

    if (kind == ChannelItemKind || kind == VirtualDestinationItemKind) {
        ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
        if (chatArea) {
            if (chatAreaStackedWidget) {
                chatAreaStackedWidget->removeWidget(chatArea);
            }
            delete chatArea;
        }
    }
    delete item;
}

void ChannelTree::reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,
                                       const SidebarTeamState& state)
{
    // A live QDrag owns the tree structure until it finishes. Reconciliation
    // during a drag would unhide/reparent the real source item underneath the
    // drag pixmap, producing a visible duplicate and invalidating gap anchors.
    // SidebarService still receives network state normally; coalesce structural
    // updates per team and apply only the newest state once the drag finishes.
    if (sidebarDragActive) {
        pendingSidebarReconcileTeams.insert(teamItem.teamId);
        return;
    }

    resetDragVisuals(false);
    renderingSidebar = true;

    auto& sidebar = SidebarService::instance(backend);
    BackendChannel* personalChannel = backend.getStorage().getDirectChannelByUserId(
        backend.getLoginUser().id);

    QMap<QString, QStringList> desiredChannels;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category) {
            continue;
        }
        QStringList ids = sidebar.visibleChannelIds(*category);
        if (category->type == QStringLiteral("favorites") && personalChannel) {
            ids.removeAll(personalChannel->id);
        }
        desiredChannels.insert(categoryId, std::move(ids));
    }

    QMap<QString, QTreeWidgetItem*> categories;
    for (int i = 0; i < teamItem.childCount(); ++i) {
        QTreeWidgetItem* category = teamItem.child(i);
        if (!category || category->data(0, ItemKindRole).toInt() != CategoryItemKind) {
            continue;
        }
        categories.insert(category->data(0, ItemIdRole).toString(), category);
    }

    // Rows which no longer belong to their current category are reusable.  Keep
    // them alive so a server-side move preserves the ChannelItem and ChatArea.
    QMap<QString, QList<QTreeWidgetItem*>> movableChannels;
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        QSet<QString> desired;
        const QStringList desiredIds = desiredChannels.value(it.key());
        for (const QString& id : desiredIds) {
            desired.insert(id);
        }
        QTreeWidgetItem* category = it.value();
        for (int row = 0; category && row < category->childCount(); ++row) {
            QTreeWidgetItem* child = category->child(row);
            if (!child || child->data(0, ItemKindRole).toInt() != ChannelItemKind) {
                continue;
            }
            const QString channelId = child->data(0, ItemIdRole).toString();
            if (!desired.contains(channelId)) {
                movableChannels[channelId].push_back(child);
            }
        }
    }

    // Personal / Saved / Drafts are always-present leading rows directly under
    // the team item, ordered before every category and outside any category
    // group. Ensure each exists and sits at the fixed leading index so the
    // categories below always mount after them.
    const struct {
        int destination;
        ChannelItem* (ChannelTree::*create)(Backend&, TeamItem&);
    } leadingDestinations[] = {
        {SidebarItem::PersonalDestination, &ChannelTree::createPersonalItem},
        {SidebarItem::SavedDestination, &ChannelTree::createSavedItem},
        {SidebarItem::DraftsDestination, &ChannelTree::createDraftsItem},
    };
    for (int desiredIndex = 0; desiredIndex < kLeadingDestinationCount; ++desiredIndex) {
        const auto& spec = leadingDestinations[desiredIndex];
        QTreeWidgetItem* row = findVirtualRow(&teamItem, spec.destination);
        if (!row) {
            row = (this->*spec.create)(backend, teamItem);
        }
        moveChild(teamItem, row, desiredIndex);
    }

    struct ActiveCategory {
        QTreeWidgetItem* item = nullptr;
        const SidebarCategory* category = nullptr;
        int desiredRows = 0;
    };
    QVector<ActiveCategory> active;

    int categoryIndex = 0;
    QSet<QString> seenCategories;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category || seenCategories.contains(categoryId)) {
            continue;
        }
        seenCategories.insert(categoryId);

        QTreeWidgetItem* categoryItem = categories.value(categoryId, nullptr);
        if (!categoryItem) {
            categoryItem = createCategoryItem(
                teamItem, category->id, categoryDisplayName(*category), category->collapsed);
            categories.insert(categoryId, categoryItem);
        }

        const int wantedIndex = kLeadingDestinationCount + categoryIndex;
        const int currentIndex = teamItem.indexOfChild(categoryItem);
        if (currentIndex >= 0 && currentIndex != wantedIndex) {
            teamItem.takeChild(currentIndex);
            teamItem.insertChild(wantedIndex, categoryItem);
        }
        categoryItem->setText(0, categoryDisplayName(*category));
        categoryItem->setData(0, ItemTeamIdRole, teamItem.teamId);
        categoryItem->setExpanded(!category->collapsed);

        active.push_back(ActiveCategory {categoryItem, category, 0});
        ++categoryIndex;
    }

    // Place every desired row first. Surplus rows are only destroyed after all
    // categories had a chance to claim them.
    for (ActiveCategory& entry : active) {
        QTreeWidgetItem* categoryItem = entry.item;
        int rowIndex = 0;

        for (const QString& channelId : desiredChannels.value(entry.category->id)) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
            if (!channel) {
                continue;
            }

            QTreeWidgetItem* row = findChannelChild(categoryItem, channelId);
            if (!row) {
                auto& candidates = movableChannels[channelId];
                while (!candidates.isEmpty() && !row) {
                    QTreeWidgetItem* candidate = candidates.takeFirst();
                    if (!candidate || !candidate->parent()) {
                        continue;
                    }
                    QTreeWidgetItem* oldParent = candidate->parent();
                    const int oldIndex = oldParent->indexOfChild(candidate);
                    if (oldIndex >= 0) {
                        oldParent->takeChild(oldIndex);
                        categoryItem->insertChild(rowIndex, candidate);
                        row = candidate;
                    }
                }
            }
            if (!row) {
                row = createChannelItem(backend, teamItem, *categoryItem, *channel);
            }
            moveChild(*categoryItem, row, rowIndex++);
        }
        entry.desiredRows = rowIndex;
    }

    for (const ActiveCategory& entry : active) {
        while (entry.item->childCount() > entry.desiredRows) {
            QTreeWidgetItem* extra = entry.item->takeChild(entry.desiredRows);
            destroySidebarRow(extra);
        }
    }

    // Desired categories were placed in exact order after the leading destination
    // rows; anything left no longer exists in the authoritative state and must
    // be destroyed. Never touch the leading Personal / Saved / Drafts rows.
    const int activeCategoryCount = static_cast<int>(active.size());
    const int totalLeadingRows = kLeadingDestinationCount + activeCategoryCount;
    while (teamItem.childCount() > totalLeadingRows) {
        QTreeWidgetItem* stale = teamItem.takeChild(totalLeadingRows);
        if (stale && stale->data(0, ItemKindRole).toInt() == VirtualDestinationItemKind) {
            destroySidebarRow(stale);
            continue;
        }
        while (stale && stale->childCount() > 0) {
            destroySidebarRow(stale->takeChild(0));
        }
        delete stale;
    }

    teamItem.setExpanded(true);
    renderingSidebar = false;
}

} // namespace Mattermost
