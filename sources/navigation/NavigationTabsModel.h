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
    struct Entry {
        QString channelId;
        QString rootId;
        QString postId;
        QString title;
        bool pinned = false;

        bool isValid() const { return !channelId.isEmpty(); }
        bool sameDestination(const Entry& other) const
        {
            return channelId == other.channelId && rootId == other.rootId;
        }
    };

    int count() const { return entries_.size(); }
    bool isEmpty() const { return entries_.isEmpty(); }
    bool shouldShowTabBar() const { return entries_.size() > 1; }

    const Entry* at(int index) const
    {
        return index >= 0 && index < entries_.size() ? &entries_.at(index) : nullptr;
    }

    // Index reached by moving `delta` positions from `currentIndex`, wrapping.
    // Returns -1 when empty; 0 when there is a single entry. An out-of-range
    // `currentIndex` seeds the move from just before the first entry (forward)
    // or from the first entry (backward) so cycling never restarts from 0.
    int cycledIndex(int currentIndex, int delta) const
    {
        const int n = entries_.size();
        if (n <= 0) {
            return -1;
        }
        if (n == 1) {
            return 0;
        }

        int base = currentIndex;
        if (base < 0 || base >= n) {
            base = delta > 0 ? -1 : 0;
        }
        return ((base + delta) % n + n) % n;
    }

    int findDestination(const QString& channelId, const QString& rootId) const
    {
        for (int i = 0; i < entries_.size(); ++i) {
            if (entries_.at(i).channelId == channelId
                && entries_.at(i).rootId == rootId) {
                return i;
            }
        }
        return -1;
    }

    int findReusableChannelTab(int preferredIndex = -1) const
    {
        if (const Entry* preferred = at(preferredIndex);
            preferred && preferred->rootId.isEmpty() && !preferred->pinned) {
            return preferredIndex;
        }

        for (int i = 0; i < entries_.size(); ++i) {
            if (i == preferredIndex) {
                continue;
            }
            const Entry& entry = entries_.at(i);
            if (entry.rootId.isEmpty() && !entry.pinned) {
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

        const int existing = findDestination(entry.channelId, entry.rootId);
        if (existing >= 0) {
            Entry merged = entries_.at(existing);
            if (!entry.postId.isEmpty()) {
                merged.postId = entry.postId;
            }
            if (!entry.title.isEmpty()) {
                merged.title = entry.title;
            }
            merged.pinned = merged.pinned || entry.pinned;
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

        const Entry previous = entries_.at(index);
        if (previous.pinned && !previous.sameDestination(entry)) {
            return false;
        }

        const int existing = findDestination(entry.channelId, entry.rootId);
        if (existing >= 0 && existing != index) {
            return false;
        }

        if (previous.sameDestination(entry)) {
            entry.pinned = previous.pinned || entry.pinned;
        }
        entries_[index] = std::move(entry);
        return true;
    }

    bool setPinned(int index, bool pinned)
    {
        if (index < 0 || index >= entries_.size()) {
            return false;
        }
        entries_[index].pinned = pinned;
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
