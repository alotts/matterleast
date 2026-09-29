/**
 * @file BackendPost.h
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 4, 2021
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MatterLeast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MatterLeast. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVariant>
#include <list>
#include <map>
#include <memory>
#include "BackendUser.h"
#include "BackendFile.h"
#include "backend/emoji/EmojiDefs.h"

namespace Mattermost {

class BackendPoll;
class Storage;

using BackendPostReaction = QVector<QString>;

class BackendPost {
public:
	BackendPost (const QJsonObject& jsonObject, const Storage& storage);
	BackendPost (BackendPost&& other) = default;
	~BackendPost ();
public:

	/**
	 * Returns whether a post was posted by the logged-in user.
	 * Poll posts are never considered as own posts, because they appear as posted by Matterpoll bot
	 * @return bool
	 */
	bool isOwnPost () 		const;

	QString getDisplayAuthorName () const;
	QDateTime getCreationTime () const;
	/**
	 * Replace server-backed fields from an already parsed snapshot without
	 * changing this object's address or immutable timeline identity.
	 * @return true when observable post state changed.
	 */
	bool updatePostEdits (BackendPost& editedPost);
	/**
	 * Parse and apply one authoritative full post snapshot in place. Local
	 * annotations absent from raw REST/cache JSON are preserved.
	 * @return true when observable post state changed.
	 */
	bool refreshFromJson (const QJsonObject& jsonObject, const Storage& storage);
	void addReaction(QString userId, QString emojiName);
	void removeReaction(QString userId, QString emojiName);
    bool hasReaction(const QString& userId, const QString& emojiName) const;
private:
	QString getAuthorName () const;
public:
	QString						id;
	uint64_t					create_at;
	uint64_t					update_at;
	uint64_t					edit_at;
	uint64_t					delete_at;
	QString						user_id;
	QString						sender_name;
	QString						channel_id;
#if 1
	QString						root_id;
	QString						parent_id;
	QString						original_id;
#endif
	BackendPost*				rootPost;
	QString						message;
	QString						type;
	QJsonValue					props;
	QString						hashtags;
	QString						pending_post_id;
	std::list<BackendFile>		files;
    // Mattermost reaction identity is the wire-level emoji_name. EmojiInfo is
    // presentation-only: aliases and a mutable custom-emoji registry must never
    // replace the name used for add/remove/tooltip semantics.
    std::map<QString, BackendPostReaction> reactions;
	// Server-generated embed metadata (permalinks, OpenGraph, etc.). Keep this
	// opaque in the backend so UI consumers can understand only the embed types
	// they support without duplicating Mattermost's metadata model here.
	QJsonArray                      embeds;

	// Transient thread metadata supplied by Mattermost for root posts.
	int64_t						reply_count = 0;
	uint64_t					last_reply_at = 0;
	QStringList                     threadParticipantUserIds;

	std::unique_ptr<BackendPoll> poll;
	const BackendUser*			author;
	bool						isDeleted;
	bool 						hidden;
	bool						is_pinned;
	bool						currentUserMentioned;
	bool						has_thread;
};

} /* namespace Mattermost */
