/**
 * @file SidebarItem.h
 * @brief Shared semantic contract for sidebar tree/list rows.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QtCore/Qt>
#include <QString>

namespace Mattermost {
namespace SidebarItem {

/**
 * Logical row kind shared by Channels, Following and Attention.
 *
 * The concrete widget is deliberately not part of this contract. Delegates and
 * alternate views should be able to reason about a row solely from model data.
 */
enum Kind {
    Unknown = 0,
    Team,
    Category,
    Channel,
    Thread,
    VirtualDestination,
};

/** Local destinations that are not ordinary server sidebar rows. */
enum Destination {
    NoDestination = 0,
    PersonalDestination,
    SavedDestination,
    DraftsDestination,
};

/**
 * Canonical string discriminator for the virtual collection destinations.
 *
 * Saved and Drafts share the ordinary Channel navigation kind; these ids (which
 * mirror the ItemIdRole values emitted by ChannelTree) are the discriminator
 * that tells them apart from each other and from an ordinary channel in the
 * tab/history model.
 */
inline const QString SavedDestinationId = QStringLiteral("virtual:saved");
inline const QString DraftsDestinationId = QStringLiteral("virtual:drafts");

/**
 * Shared model roles for sidebar rows.
 *
 * ChannelTypeRole stores BackendChannel::type as an integer. PresenceRole is
 * meaningful only for direct-message rows backed by a concrete user.
 */
enum Role {
    KindRole = Qt::UserRole + 1,
    IdRole,
    TeamIdRole,
    MutedRole,
    MentionedRole,
    PresenceRole,
    UnreadRole,
    LifetimeRole,
    ChannelTypeRole,
    ChannelIdRole,
    ThreadIdRole,
    DestinationRole,
    // Paint-only accessory action on category rows; does not consume a tree column.
    CategoryActionRole,
    CategoryActionHoveredRole,

    // Transient geometry roles used only while a ChannelTree drag is active.
    // Values are view-local: gap roles are pixels, collapse is [0, 1].
    DropGapBeforeRole,
    DropGapAfterRole,
    DragCollapseRole,
    // Visual source suppression is intentionally independent from collapse:
    // AnyKeep-style DnD hides the source immediately while its layout extent
    // is still animated out in lockstep with the destination gap. This role is
    // paint-only and must not affect sizeHint/layout geometry.
    DragSourceHiddenRole,
};

} // namespace SidebarItem
} // namespace Mattermost
