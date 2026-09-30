#pragma once

#include <utility>

#include <QString>
#include <QVector>

namespace Mattermost {

/**
 * Presentation-only semantic state for the central navigation tabs.
 *
 * Channel tabs deliberately do not own ChatArea instances. The normal channel
 * surface remains authoritative and a tab stores only the semantic destination
 * plus a viewport bookmark. Thread tabs may temporarily own the presentation
 * parent of an existing thread ChatArea, but still use the same destination
 * identity here.
 */
class NavigationTabsModel final
{
public:
    // The semantic class of a tab destination. An ordinary channel/DM/GM and a
    // virtual collection destination (Saved/Drafts) share the Channel kind; the
    // Entry::destination field tells those apart. A separate kind exists so a
    // destination's class participates in identity and title semantics.
    enum class Kind {
        Channel,
        Thread,
    };

    struct Entry {
        QString channelId;
        QString rootId;
        QString postId;
        QString title;

        // Empty for an ordinary channel; "virtual:saved" / "virtual:drafts"
        // for a Saved/Drafts collection destination.
        QString destination;

        Kind kind = Kind::Channel;

        bool isValid() const
        {
            return !channelId.isEmpty() || !destination.isEmpty();
        }
        bool sameDestination(const Entry& other) const
        {
            return kind == other.kind
                && destination == other.destination
                && channelId == other.channelId
                && rootId == other.rootId;
        }
    };

    int count() const { return entries_.size(); }
    bool isEmpty() const { return entries_.isEmpty(); }
    bool shouldShowTabBar() const { return entries_.size() > 1; }

    const Entry* at(int index) const
    {
        return index >= 0 && index < entries_.size() ? &entries_.at(index) : nullptr;
    }

    int findDestination(const QString& channelId,
                        const QString& rootId,
                        const QString& destination,
                        Kind kind) const
    {
        for (int i = 0; i < entries_.size(); ++i) {
            const Entry& entry = entries_.at(i);
            if (entry.kind == kind
                && entry.destination == destination
                && entry.channelId == channelId
                && entry.rootId == rootId) {
                return i;
            }
        }
        return -1;
    }

    int append(Entry entry)
    {
        if (!entry.isValid()) {
            return -1;
        }

        const int existing = findDestination(entry.channelId, entry.rootId,
                                             entry.destination, entry.kind);
        if (existing >= 0) {
            Entry merged = entries_.at(existing);
            if (!entry.postId.isEmpty()) {
                merged.postId = entry.postId;
            }
            if (!entry.title.isEmpty()) {
                merged.title = entry.title;
            }
            entries_[existing] = std::move(merged);
            return existing;
        }

        entries_.push_back(std::move(entry));
        return entries_.size() - 1;
    }

    bool replace(int index, Entry entry)
    {
        if (index < 0 || index >= entries_.size() || !entry.isValid()) {
            return false;
        }

        const int existing = findDestination(entry.channelId, entry.rootId,
                                             entry.destination, entry.kind);
        if (existing >= 0 && existing != index) {
            return false;
        }

        entries_[index] = std::move(entry);
        return true;
    }

    bool remove(int index)
    {
        if (index < 0 || index >= entries_.size()) {
            return false;
        }
        entries_.removeAt(index);
        return true;
    }

    void move(int from, int to)
    {
        if (from < 0 || from >= entries_.size()
            || to < 0 || to >= entries_.size() || from == to) {
            return;
        }
        entries_.move(from, to);
    }

private:
    QVector<Entry> entries_;
};

} // namespace Mattermost
