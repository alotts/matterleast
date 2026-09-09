add_executable(long-list-widget-test
    LongListWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/widgets/LongListWidget.cpp"
)

target_include_directories(long-list-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(long-list-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME long-list-widget-test COMMAND long-list-widget-test)
set_tests_properties(long-list-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(long-list-widget-removal-test
    LongListWidgetRemovalTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/widgets/LongListWidget.cpp"
)

target_include_directories(long-list-widget-removal-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(long-list-widget-removal-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME long-list-widget-removal-test COMMAND long-list-widget-removal-test)
set_tests_properties(long-list-widget-removal-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(post-source-request-gate-test
    PostSourceRequestGateTest.cpp
)

target_include_directories(post-source-request-gate-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-source-request-gate-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-source-request-gate-test COMMAND post-source-request-gate-test)

add_executable(message-formatter-test
    MessageFormatterTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageFormatter.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiInfo.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiMap.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiRegistryNotifier.cpp"
)

target_include_directories(message-formatter-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-formatter-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME message-formatter-test COMMAND message-formatter-test)
set_tests_properties(message-formatter-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(message-content-widget-test
    MessageContentWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageContentWidget.cpp"
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageFormatter.cpp"
    "${CMAKE_SOURCE_DIR}/sources/options/MLOptions.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiInfo.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiMap.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiRegistryNotifier.cpp"
)

target_include_directories(message-content-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-content-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
        qsourcehighlite
)

add_test(NAME message-content-widget-test COMMAND message-content-widget-test)
set_tests_properties(message-content-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(post-attachment-list-widget-test
    PostAttachmentListWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/attachments/PostAttachmentListWidget.cpp"
)

target_include_directories(post-attachment-list-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-attachment-list-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME post-attachment-list-widget-test COMMAND post-attachment-list-widget-test)
set_tests_properties(post-attachment-list-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(channel-activity-tracker-test
    ChannelActivityTrackerTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/ChannelActivityTracker.cpp"
)

target_include_directories(channel-activity-tracker-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(channel-activity-tracker-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME channel-activity-tracker-test COMMAND channel-activity-tracker-test)
set_tests_properties(channel-activity-tracker-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(backend-user-test
    BackendUserTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendUser.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendUserProps.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendNotifyPreps.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendTimeZone.cpp"
)

target_include_directories(backend-user-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(backend-user-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME backend-user-test COMMAND backend-user-test)
set_tests_properties(backend-user-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(backend-poll-test
    BackendPollTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendPoll.cpp"
)

target_include_directories(backend-poll-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(backend-poll-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME backend-poll-test COMMAND backend-poll-test)

add_executable(sidebar-item-delegate-test
    SidebarItemDelegateTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/channel-tree/ChannelItemDelegate.cpp"
    "${CMAKE_SOURCE_DIR}/sources/ui/IconUtils.cpp"
)

target_include_directories(sidebar-item-delegate-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(sidebar-item-delegate-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME sidebar-item-delegate-test COMMAND sidebar-item-delegate-test)
set_tests_properties(sidebar-item-delegate-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(post-window-selection-test
    PostWindowSelectionTest.cpp
)

target_include_directories(post-window-selection-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-window-selection-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-window-selection-test COMMAND post-window-selection-test)

add_executable(post-navigation-window-test
    PostNavigationWindowTest.cpp
)

target_include_directories(post-navigation-window-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-navigation-window-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-navigation-window-test COMMAND post-navigation-window-test)


add_executable(navigation-request-gate-test NavigationRequestGateTest.cpp)
target_include_directories(navigation-request-gate-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(navigation-request-gate-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME navigation-request-gate-test COMMAND navigation-request-gate-test)

add_executable(message-text-edit-widget-test
    MessageTextEditWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/outgoing-post/MessageTextEditWidget.cpp"
    "${CMAKE_SOURCE_DIR}/sources/options/MLOptions.cpp"
    "${CMAKE_SOURCE_DIR}/sources/widgets/InteractiveTextEdit.cpp"
)

target_include_directories(message-text-edit-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-text-edit-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME message-text-edit-widget-test COMMAND message-text-edit-widget-test)
set_tests_properties(message-text-edit-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(ml-options-test
    MLOptionsTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/options/MLOptions.cpp"
)

target_include_directories(ml-options-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(ml-options-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME ml-options-test COMMAND ml-options-test)

add_executable(quoted-reply-format-test
    QuotedReplyFormatTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/QuotedReplyFormat.cpp"
)

target_include_directories(quoted-reply-format-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(quoted-reply-format-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME quoted-reply-format-test COMMAND quoted-reply-format-test)

add_executable(post-cache-store-test
    PostCacheStoreTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/PostCacheStore.cpp"
)

target_include_directories(post-cache-store-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-cache-store-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Sql
)

add_test(NAME post-cache-store-test COMMAND post-cache-store-test)

add_executable(post-cache-service-test
    PostCacheServiceTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/PostCacheService.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/PostCacheStore.cpp"
    "${CMAKE_SOURCE_DIR}/sources/options/MLOptions.cpp"
)

target_include_directories(post-cache-service-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-cache-service-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Sql
)

add_test(NAME post-cache-service-test COMMAND post-cache-service-test)

add_executable(channel-gap-reconciliation-test
    ChannelGapReconciliationTest.cpp
)

target_include_directories(channel-gap-reconciliation-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(channel-gap-reconciliation-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME channel-gap-reconciliation-test COMMAND channel-gap-reconciliation-test)

add_executable(post-page-cursor-metadata-test
    PostPageCursorMetadataTest.cpp
)

target_include_directories(post-page-cursor-metadata-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-page-cursor-metadata-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-page-cursor-metadata-test COMMAND post-page-cursor-metadata-test)

add_executable(thread-navigation-placement-test
    ThreadNavigationPlacementTest.cpp
)

target_include_directories(thread-navigation-placement-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(thread-navigation-placement-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME thread-navigation-placement-test COMMAND thread-navigation-placement-test)

add_executable(thread-pane-layout-test
    ThreadPaneLayoutTest.cpp
)

target_include_directories(thread-pane-layout-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(thread-pane-layout-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME thread-pane-layout-test COMMAND thread-pane-layout-test)
set_tests_properties(thread-pane-layout-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(emoji-dialog-support-test
    EmojiDialogSupportTest.cpp
)

target_include_directories(emoji-dialog-support-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(emoji-dialog-support-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME emoji-dialog-support-test COMMAND emoji-dialog-support-test)
set_tests_properties(emoji-dialog-support-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(TARGET matterleast-core)
    add_executable(thread-post-source-integration-test ThreadPostSourceIntegrationTest.cpp)
    target_link_libraries(thread-post-source-integration-test PRIVATE matterleast-core Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME thread-post-source-integration-test COMMAND thread-post-source-integration-test)
    set_tests_properties(thread-post-source-integration-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()


add_executable(public-channel-paging-test PublicChannelPagingTest.cpp)
target_include_directories(public-channel-paging-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(public-channel-paging-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME public-channel-paging-test COMMAND public-channel-paging-test)

add_executable(avatar-image-test AvatarImageTest.cpp)
target_include_directories(avatar-image-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(avatar-image-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
add_test(NAME avatar-image-test COMMAND avatar-image-test)
set_tests_properties(avatar-image-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(theme-icon-widgets-test
    ThemeIconWidgetsTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/ui/ThemeIconWidgets.cpp"
    "${CMAKE_SOURCE_DIR}/sources/ui/IconUtils.cpp"
    "${CMAKE_SOURCE_DIR}/sources/ui/EmojiPresentation.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiInfo.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiMap.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiRegistryNotifier.cpp"
    "${CMAKE_SOURCE_DIR}/resource.qrc"
)
target_include_directories(theme-icon-widgets-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(theme-icon-widgets-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
add_test(NAME theme-icon-widgets-test COMMAND theme-icon-widgets-test)
set_tests_properties(theme-icon-widgets-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(presence-avatar-label-test
    PresenceAvatarLabelTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/ui/PresenceAvatarLabel.cpp"
    "${CMAKE_SOURCE_DIR}/sources/ui/ClickableLabel.cpp"
)
target_include_directories(presence-avatar-label-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(presence-avatar-label-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
add_test(NAME presence-avatar-label-test COMMAND presence-avatar-label-test)
set_tests_properties(presence-avatar-label-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(sidebar-channel-move-policy-test SidebarChannelMovePolicyTest.cpp)
target_include_directories(sidebar-channel-move-policy-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(sidebar-channel-move-policy-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME sidebar-channel-move-policy-test COMMAND sidebar-channel-move-policy-test)


add_executable(post-selection-policy-test PostSelectionPolicyTest.cpp)
target_include_directories(post-selection-policy-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(post-selection-policy-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME post-selection-policy-test COMMAND post-selection-policy-test)


add_executable(manual-unread-visibility-gate-test ManualUnreadVisibilityGateTest.cpp)
target_include_directories(manual-unread-visibility-gate-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(manual-unread-visibility-gate-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME manual-unread-visibility-gate-test COMMAND manual-unread-visibility-gate-test)

add_executable(post-unread-request-test PostUnreadRequestTest.cpp)
target_include_directories(post-unread-request-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(post-unread-request-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME post-unread-request-test COMMAND post-unread-request-test)

if(TARGET matterleast-core)
    add_executable(post-reaction-interaction-test PostReactionInteractionTest.cpp)
    target_link_libraries(post-reaction-interaction-test PRIVATE matterleast-core Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME post-reaction-interaction-test COMMAND post-reaction-interaction-test)
    set_tests_properties(post-reaction-interaction-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET matterleast-core)
    add_executable(manual-unread-read-state-test ManualUnreadReadStateTest.cpp)
    target_link_libraries(manual-unread-read-state-test PRIVATE matterleast-core Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME manual-unread-read-state-test COMMAND manual-unread-read-state-test)
    set_tests_properties(manual-unread-read-state-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET matterleast-core)
    add_executable(channel-header-text-label-test ChannelHeaderTextLabelTest.cpp)
    target_link_libraries(channel-header-text-label-test
        PRIVATE
            matterleast-core
            Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME channel-header-text-label-test COMMAND channel-header-text-label-test)
    set_tests_properties(channel-header-text-label-test
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
