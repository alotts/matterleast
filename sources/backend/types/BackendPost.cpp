/**
 * @file BackendPost.cpp
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
 * the Free Software Foundation; either version 3 of the License, or
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

#include "BackendPost.h"

#include <QJsonArray>
#include "BackendPoll.h"
#include "backend/Storage.h"
#include "log.h"

namespace Mattermost {

BackendPost::BackendPost (const QJsonObject& jsonObject, const Storage& storage)
:rootPost (nullptr)
,isDeleted (false)
,currentUserMentioned(jsonObject.value(QStringLiteral("_mmqt_current_user_mentioned")).toBool())
,has_thread(false)
{
	id = jsonObject.value("id").toString();
	create_at = jsonObject.value("create_at").toVariant().toULongLong();
	update_at = jsonObject.value("update_at").toVariant().toULongLong();
	edit_at = jsonObject.value("edit_at").toVariant().toULongLong();
	delete_at = jsonObject.value("delete_at").toVariant().toULongLong();
	is_pinned = jsonObject.value("is_pinned").toBool();
	user_id = jsonObject.value("user_id").toString();
	sender_name = jsonObject.value(QStringLiteral("_mmqt_sender_name")).toString();
	author = storage.getUserById (user_id);
	channel_id = jsonObject.value("channel_id").toString();
	root_id = jsonObject.value("root_id").toString();
	parent_id = jsonObject.value("parent_id").toString();
	original_id = jsonObject.value("original_id").toString();
	message = jsonObject.value("message").toString();
	type = jsonObject.value("type").toString();
	props = jsonObject.value("props");
	hashtags = jsonObject.value("hashtags").toString();
	pending_post_id = jsonObject.value("pending_post_id").toString();
	// `hidden` is a timeline-topology flag: replies belong to their thread and
	// must not be treated as main-channel rows. System events are ordinary root
	// rows returned by Mattermost's channel history and must stay in the same
	// coordinate system as /posts paging and permalink contexts.
	hidden = !root_id.isEmpty();

	// Mattermost adds these transient fields to root posts. They are exactly the
	// metadata used by the web client to render the thread footer without first
	// downloading the complete thread.
	reply_count = jsonObject.value("reply_count").toVariant().toLongLong();
	last_reply_at = jsonObject.value("last_reply_at").toVariant().toULongLong();
	for (const QJsonValue& participantValue : jsonObject.value("participants").toArray()) {
		const QString participantId = participantValue.isObject()
			? participantValue.toObject().value(QStringLiteral("id")).toString()
			: participantValue.toString();
		if (!participantId.isEmpty() && !threadParticipantUserIds.contains(participantId)) {
			threadParticipantUserIds.push_back(participantId);
		}
	}
	has_thread = reply_count > 0;

	QJsonObject metadata = jsonObject.value("metadata").toObject();
	embeds = metadata.value(QStringLiteral("embeds")).toArray();

	for (const auto &fileElement: metadata.value("files").toArray()) {
		files.emplace_back (fileElement.toObject());
	}

	for (const auto& reactionElement : metadata.value("reactions").toArray()) {
        const QJsonObject reaction = reactionElement.toObject();
        addReaction(reaction.value("user_id").toString(),
                    reaction.value("emoji_name").toString());
	}

	/**
	 * Matterpoll stores its definition in the same props.attachments field used
	 * by ordinary Slack-compatible message attachments. The old heuristic treated
	 * any attachment containing actions or fields as a poll, which misclassified
	 * rich bot posts (Jira/Jenkins/GitLab are common examples). A real Matterpoll
	 * post carries its explicit poll_id, so use that as the discriminator.
	 */
	const QString pollId = props.toObject().value(QStringLiteral("poll_id")).toString();
	if (pollId.isEmpty()) {
		return;
	}

	QJsonValue attachments (props.toObject().value("attachments"));
	if (attachments.isArray() && !attachments.toArray().isEmpty()) {
		auto pollObject = attachments.toArray()[0].toObject();

		/**
		 * If there is no actions array and no fields array, this is not a poll
		 */
		if (pollObject.value("actions").toArray().isEmpty() && pollObject.value("fields").toArray().isEmpty()) {
			return;
		}

		poll = std::make_unique<BackendPoll> (pollId, pollObject);
	}
}

BackendPost::~BackendPost () = default;

bool BackendPost::isOwnPost () const
{
	// Deleted posts are no longer actionable as "our post". In particular, a
	// post_deleted websocket event is rendered as the same tombstone an official
	// remote client shows, rather than being mistaken for a locally initiated
	// delete/remove action merely because its original author is the login user.
	if (isDeleted || !author) {
		return false;
	}

	return author->isLoginUser;
}

/**
 * Get the author name to be displayed in chat windows, dialogs, etc.
 * This differs from author name, in cases like polls - where the post author is a bot
 * @return
 */
QString BackendPost::getDisplayAuthorName () const
{
	if (poll) {
		return poll->authorName + " (" + getAuthorName() + ")";
	}

	return getAuthorName();
}

void BackendPost::addReaction(QString userId, QString emojiName)
{
    if (userId.isEmpty() || emojiName.isEmpty()) {
        return;
    }

    // WebSocket events may be replayed after reconnect. reaction_added is an
    // idempotent fact, never a toggle. Keep the exact Mattermost emoji_name as
    // model identity; EmojiInfo resolution belongs to presentation only.
    auto& users = reactions[emojiName];
    if (!users.contains(userId)) {
        users.push_back(userId);
    }
}

void BackendPost::removeReaction(QString userId, QString emojiName)
{
    if (userId.isEmpty() || emojiName.isEmpty()) {
        return;
    }

    auto reaction = reactions.find(emojiName);
    if (reaction == reactions.end()) {
        return;
    }

    auto& users = reaction->second;
    users.erase(std::remove(users.begin(), users.end(), userId), users.end());
    if (users.isEmpty()) {
        reactions.erase(reaction);
    }
}

bool BackendPost::hasReaction(const QString& userId, const QString& emojiName) const
{
    if (userId.isEmpty() || emojiName.isEmpty()) {
        return false;
    }

    const auto reaction = reactions.find(emojiName);
    return reaction != reactions.end() && reaction->second.contains(userId);
}

/**
 * Get the author name. This is the name of the user, set as a post author
 * @return
 */
QString BackendPost::getAuthorName () const
{
	if (author) {
		return author->getDisplayName ();
	}

	if (!sender_name.isEmpty()) {
		return sender_name;
	}

	return user_id;
}

QDateTime BackendPost::getCreationTime () const
{
	return QDateTime::fromMSecsSinceEpoch (create_at);
}

bool BackendPost::refreshFromJson (const QJsonObject& jsonObject, const Storage& storage)
{
	// A few fields are local annotations rather than Mattermost post fields. A
	// raw REST/cache snapshot normally does not contain them, so absence must not
	// erase information already derived by the client.
	QJsonObject normalized = jsonObject;
	if (!normalized.contains(QStringLiteral("_mmqt_sender_name")) && !sender_name.isEmpty()) {
		normalized.insert(QStringLiteral("_mmqt_sender_name"), sender_name);
	}
	if (!normalized.contains(QStringLiteral("_mmqt_current_user_mentioned"))) {
		normalized.insert(QStringLiteral("_mmqt_current_user_mentioned"), currentUserMentioned);
	}

	BackendPost refreshed(normalized, storage);
	if (!refreshed.root_id.isEmpty()) {
		refreshed.hidden = true;
	}
	return updatePostEdits(refreshed);
}

bool BackendPost::updatePostEdits (BackendPost& editedPost)
{
	// Identity/topology is immutable for a Mattermost post. Refusing a malformed
	// snapshot here is safer than moving an existing object to another timeline
	// while widgets and sources still hold its stable address/ID.
	if (editedPost.id.isEmpty() || editedPost.id != id
		|| editedPost.channel_id != channel_id
		|| editedPost.root_id != root_id
		|| editedPost.create_at != create_at) {
		LOG_DEBUG("Ignoring structurally inconsistent refresh for post " << id);
		return false;
	}

	const auto sameFiles = [](const std::list<BackendFile>& lhs,
	                          const std::list<BackendFile>& rhs) {
		if (lhs.size() != rhs.size()) {
			return false;
		}
		auto left = lhs.cbegin();
		auto right = rhs.cbegin();
		for (; left != lhs.cend(); ++left, ++right) {
			if (left->id != right->id || left->name != right->name
				|| left->mimeType != right->mimeType || left->size != right->size
				|| left->extension != right->extension) {
				return false;
			}
		}
		return true;
	};
    const auto sameReactions =
        [](const std::map<QString, BackendPostReaction>& lhs,
           const std::map<QString, BackendPostReaction>& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        auto left = lhs.cbegin();
        auto right = rhs.cbegin();
        for (; left != lhs.cend(); ++left, ++right) {
            if (left->first != right->first || left->second != right->second) {
                return false;
            }
        }
        return true;
    };

	const bool nextDeleted = editedPost.delete_at != 0 || editedPost.isDeleted;
	const bool nextHidden = editedPost.hidden || !root_id.isEmpty();
	const QString nextSenderName = editedPost.sender_name.isEmpty()
		? sender_name : editedPost.sender_name;
	const bool nextCurrentUserMentioned = currentUserMentioned
		|| editedPost.currentUserMentioned;
	const bool changed = update_at != editedPost.update_at
		|| edit_at != editedPost.edit_at
		|| delete_at != editedPost.delete_at
		|| is_pinned != editedPost.is_pinned
		|| user_id != editedPost.user_id
		|| sender_name != nextSenderName
		|| author != editedPost.author
		|| parent_id != editedPost.parent_id
		|| original_id != editedPost.original_id
		|| message != editedPost.message
		|| type != editedPost.type
		|| props != editedPost.props
		|| hashtags != editedPost.hashtags
		|| pending_post_id != editedPost.pending_post_id
		|| !sameFiles(files, editedPost.files)
		|| !sameReactions(reactions, editedPost.reactions)
		|| embeds != editedPost.embeds
		|| reply_count != editedPost.reply_count
		|| last_reply_at != editedPost.last_reply_at
		|| threadParticipantUserIds != editedPost.threadParticipantUserIds
		|| currentUserMentioned != nextCurrentUserMentioned
		|| has_thread != editedPost.has_thread
		|| isDeleted != nextDeleted
		|| hidden != nextHidden
		|| static_cast<bool>(poll) != static_cast<bool>(editedPost.poll);

	if (!changed) {
		return false;
	}

	// The poll metadata endpoint is asynchronous and PostPoll connects directly
	// to this QObject. Keep the model identity stable while refreshing the same
	// poll definition; transferring metadata into a replacement object leaves
	// in-flight callbacks and existing signal connections pointing at the old one.
	const bool samePoll = poll && editedPost.poll && poll->id == editedPost.poll->id;

	update_at = editedPost.update_at;
	edit_at = editedPost.edit_at;
	delete_at = editedPost.delete_at;
	is_pinned = editedPost.is_pinned;
	user_id = editedPost.user_id;
	sender_name = nextSenderName;
	author = editedPost.author;
	parent_id = editedPost.parent_id;
	original_id = editedPost.original_id;
	message = editedPost.message;
	type = editedPost.type;
	props = editedPost.props;
	hashtags = editedPost.hashtags;
	pending_post_id = editedPost.pending_post_id;
	files = std::move(editedPost.files);
	reactions = std::move(editedPost.reactions);
	embeds = std::move(editedPost.embeds);
	reply_count = editedPost.reply_count;
	last_reply_at = editedPost.last_reply_at;
	threadParticipantUserIds = std::move(editedPost.threadParticipantUserIds);
	if (samePoll) {
		poll->updateDefinition(*editedPost.poll);
	} else {
		poll = std::move(editedPost.poll);
	}
	currentUserMentioned = nextCurrentUserMentioned;
	has_thread = editedPost.has_thread;
	isDeleted = nextDeleted;
	hidden = nextHidden;
	return true;
}


} /* namespace Mattermost */
