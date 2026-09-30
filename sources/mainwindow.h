/**
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

#include <memory>
#include <QMainWindow>
#include <QStringList>
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QEvent;
class QLineEdit;
class QSplitter;
class QSystemTrayIcon;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Mattermost {

class AttentionList;
class ChatArea;
class Backend;
class BackendChannel;
class BackendPost;
class BackendTeam;
class ChannelQuickList;
class NotificationManager;
class PostCollectionView;

class MainWindow: public QMainWindow {
	Q_OBJECT
public:
	MainWindow (QWidget *parent, QSystemTrayIcon& trayIcon, Backend& backend);
	~MainWindow();
public:
	void initializationComplete ();
    void installRealtimeUiSync();
    void beginSemanticNavigation();
	void openChannelPost(const QString& channelId,
	                     const QString& postId = QString(),
	                     const QString& rootId = QString(),
	                     const QStringList& contextPostIds = QStringList(),
	                     bool reachedOldest = false,
	                     bool reachedNewest = false,
                         bool preserveIfOpen = false);

    // Virtual collection destinations (Saved/Drafts) are presented through the
    // navigation layer as ordinary Channel destinations. These accessors let
    // that layer (and the sidebar activation path) lazily obtain and reveal the
    // canonical singleton page for a sentinel destination id.
    PostCollectionView* collectionPageForDestination(const QString& destination);
    void revealCollectionPage(const QString& destination);
    bool isTabbableCollectionPage(QWidget* page) const;

	void changeEvent (QEvent* event) override;
	void closeEvent(QCloseEvent *event) override;
	void saveState ();
	void messageNotify (BackendChannel& channel, const BackendPost& post);
	void unreadMessagesNotify (const BackendChannel& channel);
	void setNotificationsCountVisualization (uint32_t notificationsCount);
	void moveEvent (QMoveEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;

private slots:
    void on_usericon_label_clicked();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
private:
	void createMenu ();
	void setupChannelTabs ();
	void refreshSidebarViews ();
	void refreshChannelUnreadFilter ();
	void applySidebarTextFilter(QTreeWidget* tree) const;
	void openDirectMessageSearch ();
    void openMessageSearch();
    void openSavedMessages(const QString& teamId = QString());
    void openDrafts(const QString& teamId = QString());
    void openDraft(const QString& channelId, const QString& rootId,
                   const QString& draftKey = QString());
    void showCollectionPage(PostCollectionView* page);
	void refreshMenuButtonIcon ();
    void refreshSearchButtonIcon();
	void refreshUnreadFilterIcon ();
private:
	std::unique_ptr<Ui::MainWindow>		ui;
	QSystemTrayIcon&					trayIcon;
	std::unique_ptr<NotificationManager>	notificationManager;
	ChooseEmojiDialogWrapper			chooseEmojiDialog;
	Backend&							backend;
	QSplitter*							sidebarSplitter = nullptr;
	QTabWidget*							channelTabs = nullptr;
	QWidget*							channelsPage = nullptr;
	QLineEdit*							sidebarFilterEdit = nullptr;
	QToolButton*						unreadFilterButton = nullptr;
	ChannelQuickList*					recentChannels = nullptr;
	AttentionList*						attentionList = nullptr;
    PostCollectionView*                 savedMessagesPage = nullptr;
    PostCollectionView*                 draftsPage = nullptr;
    PostCollectionView*                 searchMessagesPage = nullptr;
	QString								retainedUnreadFilterChannelId;
    quint64                             semanticNavigationGeneration = 0;
	bool								currentTeamRestoredFromSettings;
	QMenu*								mainMenu;
	bool								doDeinit;
};

} /* namespace Mattermost */
