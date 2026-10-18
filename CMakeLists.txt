cmake_minimum_required(VERSION 3.22)

set(BIN silent)
project(${BIN} VERSION 0.1.0 LANGUAGES CXX)

option(SILENT_I18N_TRACE_IDS "Emit i18n ID pragma trace markers" OFF)
option(SILENT_BUILD_HOST_SMOKE "Build the plugin host smoke test" OFF)
option(SILENT_BUILD_DEVICE_E2E "Build the device GUI end to end probe" OFF)
option(SILENT_MOCK_UI "Run the UI on mock account data instead of the real FFI controller" OFF)

# Build silent Rust crate before main project
add_custom_target(
    RunBeforeBuild ALL
    COMMAND bash build.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Import Rust static library
add_library(silent_rust STATIC IMPORTED)
set_target_properties(silent_rust PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/libsilent.a
)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core Svg)

# qontrol framework (prefer vendored lib/qontrol, otherwise fetch pinned commit)
if(EXISTS "${CMAKE_SOURCE_DIR}/lib/qontrol/CMakeLists.txt")
    add_subdirectory(lib/qontrol)
else()
    include(FetchContent)
    FetchContent_Declare(
        qontrol_src
        GIT_REPOSITORY https://github.com/pythcoiner/qontrol.git
        GIT_TAG dfc65d1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(qontrol_src)
endif()
add_subdirectory(src/interfaces)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# Shared application sources (everything except the executable entry point).
# Reused by the optional host smoke target so it links the real Host/PluginRegistry.
set(SILENT_APP_SOURCES
    src/QtAbiSurface.cpp
    src/host/Host.h
    src/host/Host.cpp
    src/host/PluginRegistry.h
    src/host/PluginRegistry.cpp
    src/interfaces/host.h
    src/interfaces/module.h
    src/interfaces/account.h
    src/interfaces/feed.h
    src/interfaces/keys.h
    src/interfaces/signer.h
    src/interfaces/theme.h
    src/interfaces/instance.h
    src/theme/Theme.h
    src/theme/Theme.cpp
    src/theme/Palette.h
    src/theme/Palette.cpp
    src/catalog/format.h
    src/catalog/format.cpp
    src/catalog/Button.h
    src/catalog/Button.cpp
    src/catalog/CopyButton.h
    src/catalog/CopyButton.cpp
    src/catalog/Variants.h
    src/catalog/Variants.cpp
    src/catalog/display/Label.h
    src/catalog/display/Label.cpp
    src/catalog/inputs/Input.h
    src/catalog/inputs/Input.cpp
    src/catalog/display/Display.h
    src/catalog/display/Display.cpp
    src/AppController.h
    src/AppController.cpp
    src/i18n/I18nManager.h
    src/i18n/I18nManager.cpp
    src/i18n/Tr.h
    src/MainWindow.h
    src/MainWindow.cpp
    src/modules/sp/AccountController.h
    src/modules/sp/AccountController.cpp
    src/modules/sp/AccountWidget.h
    src/modules/sp/AccountWidget.cpp
    src/modules/hwi/HwiPlugin.h
    src/modules/hwi/HwiPlugin.cpp
    src/modules/hwi/HwiModule.h
    src/modules/hwi/HwiModule.cpp
    src/modules/hwi/HwiServiceInstance.h
    src/modules/hwi/HwiServiceInstance.cpp
    src/modules/hwi/HwiDeviceInstance.h
    src/modules/hwi/HwiDeviceInstance.cpp
    src/modules/hwi/HwiSigner.h
    src/modules/hwi/HwiSigner.cpp
    src/modules/keys/KeysModule.h
    src/modules/keys/KeysModule.cpp
    src/modules/keys/KeysTab.h
    src/modules/keys/KeysTab.cpp
    src/modules/keys/BwkQrEngine.h
    src/modules/keys/BwkQrEngine.cpp
    src/modules/keys/DeviceName.h
    src/modules/keys/DeviceName.cpp
    src/modules/keys/KeysView.h
    src/modules/keys/KeysView.cpp
    src/modules/keys/flow/FlowCopy.h
    src/modules/keys/flow/FlowCopy.cpp
    src/modules/keys/flow/CameraSettings.h
    src/modules/keys/flow/CameraSettings.cpp
    src/modules/keys/flow/QrSettings.h
    src/modules/keys/flow/QrSettings.cpp
    src/modules/keys/flow/CameraStep.h
    src/modules/keys/flow/CameraStep.cpp
    src/modules/keys/flow/SigningFlow.h
    src/modules/keys/flow/SigningFlow.cpp
    src/modules/keys/flow/signingflow/LiveDevice.h
    src/modules/keys/flow/signingflow/LiveDevice.cpp
    src/modules/keys/flow/signingflow/VerifyResultView.h
    src/modules/keys/flow/signingflow/VerifyResultView.cpp
    src/modules/keys/flow/signingflow/UsbRouteFlow.h
    src/modules/keys/flow/signingflow/UsbRouteFlow.cpp
    src/modules/keys/flow/signingflow/HotRouteFlow.h
    src/modules/keys/flow/signingflow/HotRouteFlow.cpp
    src/modules/keys/flow/signingflow/SdRouteFlow.h
    src/modules/keys/flow/signingflow/SdRouteFlow.cpp
    src/modules/keys/flow/signingflow/QrRouteFlow.h
    src/modules/keys/flow/signingflow/QrRouteFlow.cpp
    src/modules/keys/signers/RouteSigner.h
    src/modules/keys/signers/RouteSigner.cpp
    src/modules/keys/signers/OfflineSigner.h
    src/modules/keys/signers/OfflineSigner.cpp
    src/modules/keys/modals/AddKey.h
    src/modules/keys/modals/AddKey.cpp
    src/modules/keys/modals/addkey/KeyChecks.h
    src/modules/keys/modals/addkey/KeyChecks.cpp
    src/modules/keys/modals/addkey/KeyFields.h
    src/modules/keys/modals/addkey/KeyFields.cpp
    src/modules/keys/modals/addkey/KeyReview.h
    src/modules/keys/modals/addkey/KeyReview.cpp
    src/modules/keys/modals/addkey/QrFlow.h
    src/modules/keys/modals/addkey/QrFlow.cpp
    src/modules/keys/modals/addkey/FileFlow.h
    src/modules/keys/modals/addkey/FileFlow.cpp
    src/modules/keys/modals/addkey/PasteFlow.h
    src/modules/keys/modals/addkey/PasteFlow.cpp
    src/modules/keys/modals/addkey/HotKeyFlow.h
    src/modules/keys/modals/addkey/HotKeyFlow.cpp
    src/modules/keys/modals/RegisterDevice.h
    src/modules/keys/modals/RegisterDevice.cpp
    src/modules/keys/modals/KeyDetail.h
    src/modules/keys/modals/KeyDetail.cpp
    src/modules/keys/modals/keydetail/KeyRecord.h
    src/modules/keys/modals/keydetail/KeyRecord.cpp
    src/modules/keys/modals/keydetail/RemoveFlow.h
    src/modules/keys/modals/keydetail/RemoveFlow.cpp
    src/modules/keys/modals/FetchXpub.h
    src/modules/keys/modals/FetchXpub.cpp
    src/modules/keys/modals/Unseal.h
    src/modules/keys/modals/Unseal.cpp
    src/modules/keys/modals/RevealMnemonic.h
    src/modules/keys/modals/RevealMnemonic.cpp
    src/modules/sp/SpPlugin.h
    src/modules/sp/SpPlugin.cpp
    src/modules/sp/SpModule.h
    src/modules/sp/SpModule.cpp
    src/modules/sp/SpInstance.h
    src/modules/sp/SpInstance.cpp
    src/modules/sp/SpAccount.h
    src/modules/sp/SpAccount.cpp
    src/modules/sp/StatusBar.h
    src/modules/sp/StatusBar.cpp
    src/modules/musig/MusigPlugin.h
    src/modules/musig/MusigPlugin.cpp
    src/modules/musig/MusigModule.h
    src/modules/musig/MusigModule.cpp
    src/modules/musig/MusigInstance.h
    src/modules/musig/MusigInstance.cpp
    src/modules/musig/MusigAccountAdapter.h
    src/modules/musig/MusigAccountAdapter.cpp
    src/modules/musig/MusigAccountController.h
    src/modules/musig/MusigAccountController.cpp
    src/modules/musig/MusigAccountWidget.h
    src/modules/musig/MusigAccountWidget.cpp
    src/modules/musig/MusigStatusBar.h
    src/modules/musig/MusigStatusBar.cpp
    src/modules/musig/views/Send.h
    src/modules/musig/views/Send.cpp
    src/modules/musig/views/Settings.h
    src/modules/musig/views/Settings.cpp
    src/modules/musig/creator/MusigCreatorModule.h
    src/modules/musig/creator/MusigCreatorModule.cpp
    src/modules/musig/creator/CreatePolicy.h
    src/modules/musig/creator/CreatePolicy.cpp
    src/modules/musig/creator/PolicyEditor.h
    src/modules/musig/creator/PolicyEditor.cpp
    src/modules/musig/creator/PathEditor.h
    src/modules/musig/creator/PathEditor.cpp
    src/modules/musig/creator/ReviewPolicy.h
    src/modules/musig/creator/ReviewPolicy.cpp
    src/views/MenuTab.h
    src/views/MenuTab.cpp
    src/views/modals/AppSettings.h
    src/views/modals/AppSettings.cpp
    src/views/modals/appsettings/DisableFlow.h
    src/views/modals/appsettings/DisableFlow.cpp
    src/views/modals/appsettings/SectionFlow.h
    src/views/modals/appsettings/SectionFlow.cpp
    src/modules/sp/modals/CreateAccount.h
    src/modules/sp/modals/CreateAccount.cpp
    src/modules/sp/modals/ScanAddress.h
    src/modules/sp/modals/ScanAddress.cpp
    src/views/utils.h
    src/views/utils.cpp
    src/views/DashboardLayout.h
    src/views/DashboardLayout.cpp
    src/account/AccountBackend.h
    src/account/views/Coins.h
    src/account/views/Coins.cpp
    src/account/views/History.h
    src/account/views/History.cpp
    src/account/views/Receive.h
    src/account/views/Receive.cpp
    src/modules/sp/views/Send.h
    src/modules/sp/views/Send.cpp
    src/modules/sp/views/Settings.h
    src/modules/sp/views/Settings.cpp
    src/views/modals/ConfirmDelete.h
    src/views/modals/ConfirmDelete.cpp
    src/views/modals/AccountType.cpp
    src/theme/Icon.h
    src/theme/Icon.cpp
    src/catalog/containers/Tab.h
    src/catalog/containers/Tab.cpp
    src/catalog/inputs/Checkbox.h
    src/catalog/inputs/Checkbox.cpp
    src/catalog/inputs/Toggle.h
    src/catalog/inputs/Toggle.cpp
    src/catalog/inputs/ComboBox.h
    src/catalog/inputs/ComboBox.cpp
    src/catalog/inputs/TextEdit.h
    src/catalog/inputs/TextEdit.cpp
    src/catalog/containers/Separator.h
    src/catalog/containers/Separator.cpp
    src/catalog/containers/ScrollArea.h
    src/catalog/containers/ScrollArea.cpp
    src/catalog/containers/ModalActions.h
    src/catalog/containers/ModalActions.cpp
    src/catalog/containers/Section.h
    src/catalog/containers/Section.cpp
    src/catalog/containers/SectionHeader.h
    src/catalog/containers/SectionHeader.cpp
    src/catalog/containers/Modal.h
    src/catalog/containers/Modal.cpp
    src/catalog/containers/ViewSlots.h
    src/catalog/containers/ViewSlots.cpp
    src/catalog/containers/Card.h
    src/catalog/containers/Card.cpp
    src/catalog/display/Badge.h
    src/catalog/display/Badge.cpp
    src/catalog/display/CopyField.h
    src/catalog/display/CopyField.cpp
    src/catalog/display/RouteIcons.h
    src/catalog/display/RouteIcons.cpp
    src/catalog/display/ShortValue.h
    src/catalog/display/ShortValue.cpp
    src/catalog/display/WordGrid.h
    src/catalog/display/WordGrid.cpp
    src/catalog/form/ValidationMark.h
    src/catalog/form/ValidationMark.cpp
    src/catalog/form/ValidatedInput.h
    src/catalog/form/ValidatedInput.cpp
    src/catalog/Table.h
    src/catalog/Table.cpp
    src/catalog/containers/FoldSection.h
    src/catalog/containers/FoldSection.cpp
    src/catalog/containers/Tabs.h
    src/catalog/containers/Tabs.cpp
    src/catalog/form/Field.h
    src/catalog/form/Field.cpp
    src/catalog/form/FieldGroup.h
    src/catalog/form/FieldGroup.cpp
    src/catalog/form/FieldNote.h
    src/catalog/form/FieldNote.cpp
    src/catalog/form/EndpointRow.h
    src/catalog/form/EndpointRow.cpp
    src/catalog/form/WordInputGrid.h
    src/catalog/form/WordInputGrid.cpp
    src/catalog/BalanceHeader.h
    src/catalog/BalanceHeader.cpp
    src/catalog/panels/receive/AddressRow.h
    src/catalog/panels/receive/AddressRow.cpp
    src/catalog/panels/receive/LabelRow.h
    src/catalog/panels/receive/LabelRow.cpp
    src/catalog/panels/receive/ChunkedAddress.h
    src/catalog/panels/receive/ChunkedAddress.cpp
    src/catalog/panels/receive/IndexPill.h
    src/catalog/panels/receive/IndexPill.cpp
    src/views/modals/AddressHistoryModal.h
    src/views/modals/AddressHistoryModal.cpp
    src/catalog/panels/receive/QrCode.h
    src/catalog/panels/receive/QrCode.cpp
    src/catalog/SelectRow.h
    src/catalog/SelectRow.cpp
    src/catalog/inputs/Stepper.h
    src/catalog/inputs/Stepper.cpp
    src/qr/QrTypes.h
    src/qr/QrTypes.cpp
    src/qr/QrEngine.h
    src/qr/QrEngine.cpp
    src/qr/QrDisplayEngine.h
    src/qr/QrDisplayEngine.cpp
    src/qr/QrDisplay.h
    src/qr/QrDisplay.cpp
    src/qr/QrCamera.h
    src/qr/QrCamera.cpp
    src/qr/QrCameraEngine.h
    src/qr/QrCameraEngine.cpp
    src/qr/QrCameraDisplay.h
    src/qr/QrCameraDisplay.cpp
    src/qr/bbqr/BbqrDisplayEngine.h
    src/qr/bbqr/BbqrDisplayEngine.cpp
    src/qr/bbqr/BbqrCameraEngine.h
    src/qr/bbqr/BbqrCameraEngine.cpp
    src/catalog/feedback/MessageModal.h
    src/catalog/feedback/MessageModal.cpp
    src/catalog/feedback/ModalStatus.h
    src/catalog/feedback/ModalStatus.cpp
    src/catalog/feedback/EmptyState.h
    src/catalog/feedback/EmptyState.cpp
    src/catalog/feedback/Notice.h
    src/catalog/feedback/Notice.cpp
    src/catalog/feedback/ScanProgress.h
    src/catalog/feedback/ScanProgress.cpp
    src/catalog/feedback/Spinner.h
    src/catalog/feedback/Spinner.cpp
    src/catalog/StatusBarItem.h
    src/catalog/StatusBarItem.cpp
    src/catalog/feedback/Tooltip.h
    src/catalog/feedback/Tooltip.cpp
    src/catalog/panels/send/TxFlow.h
    src/catalog/panels/send/TxFlow.cpp
    src/catalog/panels/home/HistoryTable.h
    src/catalog/panels/home/HistoryTable.cpp
)

# The mock controller is dev-only UI scaffolding. It is excluded from the build
# unless SILENT_MOCK_UI is set, which also flips AccountWidget to instantiate it.
if(SILENT_MOCK_UI)
    list(APPEND SILENT_APP_SOURCES
        src/modules/sp/MockAccountController.h
        src/modules/sp/MockAccountController.cpp
    )
endif()

set(SOURCES
    src/main.cpp
    ${SILENT_APP_SOURCES}
)

# Discover all translator-editable catalogs.
# Any new `i18n/silent_<locale>.lang` file is embedded automatically.
file(GLOB TRANSLATION_LANG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/i18n/silent_*.lang)

qt_add_executable(${BIN}
    ${SOURCES}
)

if(SILENT_I18N_TRACE_IDS)
    target_compile_definitions(${BIN} PRIVATE I18N_TRACE_IDS=1)
endif()

if(SILENT_MOCK_UI)
    target_compile_definitions(${BIN} PRIVATE SILENT_MOCK_UI=1)
endif()

foreach(LANG_FILE ${TRANSLATION_LANG_FILES})
    get_filename_component(LANG_BASENAME ${LANG_FILE} NAME)
    set_source_files_properties(${LANG_FILE} PROPERTIES QT_RESOURCE_ALIAS ${LANG_BASENAME})
endforeach()

# Embed `.lang` files directly in the executable for single-binary distribution.
qt_add_resources(${BIN} embedded_i18n_lang
    PREFIX "/i18n"
    FILES ${TRANSLATION_LANG_FILES}
)

# Ensure Rust library is built before executable
add_dependencies(${BIN} RunBeforeBuild)

add_dependencies(${BIN}
    qontrol
)

target_include_directories(${BIN}
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/modules/sp>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include>
)

target_link_libraries(${BIN}
    PRIVATE
    silent_rust
    ssl
    crypto
    pthread
    dl
    udev
)

if(UNIX AND NOT APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_link_options(${BIN} PRIVATE
        "-Wl,--export-dynamic"
    )

    set(SILENT_QT_AND_QONTROL_LIBS
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Svg
        qontrol
    )

    set(SILENT_ELF_LINK_LIBS)
    foreach(SILENT_LIB ${SILENT_QT_AND_QONTROL_LIBS})
        get_target_property(SILENT_LIB_TYPE ${SILENT_LIB} TYPE)
        if(SILENT_LIB_TYPE STREQUAL "STATIC_LIBRARY")
            list(APPEND SILENT_ELF_LINK_LIBS "$<LINK_LIBRARY:WHOLE_ARCHIVE,${SILENT_LIB}>")
        else()
            list(APPEND SILENT_ELF_LINK_LIBS "${SILENT_LIB}")
        endif()
    endforeach()

    target_link_libraries(${BIN} PRIVATE ${SILENT_ELF_LINK_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows host-symbol export for plugin DLLs. Plugins link only against the
    # host import library, so a single static Qt lives in the host and plugins
    # resolve the host ABI from it. qontrol and the QtCore/Gui/Widgets/Svg modules
    # are whole-archived so their full symbol set is present in the image and can
    # be re-exported: a plugin may use any of those modules, not just the subset
    # the host itself calls. A curated export table (scripts/gen_host_exports.sh)
    # keeps the PE export count under the 64K ordinal cap that plain
    # --export-all-symbols overflows.
    target_link_libraries(${BIN}
        PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,Qt6::Core>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,Qt6::Gui>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,Qt6::Widgets>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,Qt6::Svg>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,qontrol>"
    )

    if(NOT CMAKE_NM)
        find_program(CMAKE_NM NAMES x86_64-w64-mingw32-nm nm)
    endif()

    set(SILENT_HOST_DEF "${CMAKE_CURRENT_BINARY_DIR}/silent_host_exports.def")
    add_custom_command(TARGET ${BIN} PRE_LINK
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/gen_host_exports.sh"
                "${CMAKE_NM}"
                "$<TARGET_FILE:qontrol>"
                "$<TARGET_FILE_DIR:Qt6::Core>"
                "${SILENT_HOST_DEF}"
                "$<TARGET_OBJECTS:${BIN}>"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Generating curated Windows host export table (plugin ABI)"
    )

    target_link_options(${BIN} PRIVATE
        "-Wl,--exclude-all-symbols"
        "${SILENT_HOST_DEF}"
    )

    # Build the import library that plugin DLLs link against. We generate it from
    # the same .def with dlltool rather than the linker's --out-implib: the latter
    # is tied to ld's auto-export path, which --exclude-all-symbols disables, so it
    # produces nothing here.
    if(NOT CMAKE_DLLTOOL)
        find_program(CMAKE_DLLTOOL NAMES x86_64-w64-mingw32-dlltool dlltool)
    endif()
    set(SILENT_HOST_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/libsilent_host_exports.a")
    add_custom_command(TARGET ${BIN} POST_BUILD
        COMMAND "${CMAKE_DLLTOOL}"
                --input-def "${SILENT_HOST_DEF}"
                --dllname "$<TARGET_FILE_NAME:${BIN}>"
                --output-lib "${SILENT_HOST_IMPLIB}"
        BYPRODUCTS "${SILENT_HOST_IMPLIB}"
        VERBATIM
        COMMENT "Generating host import library for plugin DLLs"
    )
else()
    target_link_libraries(${BIN}
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Svg
        qontrol
    )
endif()

set_target_properties(${BIN} PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Opt-in plugin host/registry smoke test. OFF by default so it never affects the
# normal build. When ON it reuses the shared application sources so it links the
# real Host and PluginRegistry, and drives them headless with mock plugins.
if(SILENT_BUILD_HOST_SMOKE)
    set(SMOKE host_smoke)

    qt_add_executable(${SMOKE}
        tests/host/host_smoke.cpp
        tests/host/MockPlugin.h
        ${SILENT_APP_SOURCES}
    )

    add_dependencies(${SMOKE} RunBeforeBuild qontrol)

    # The catalogs, so TR() returns real text.
    qt_add_resources(${SMOKE} smoke_i18n_lang
        PREFIX "/i18n"
        FILES ${TRANSLATION_LANG_FILES}
    )

    target_include_directories(${SMOKE}
      PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/modules/sp>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/tests/host>
    )

    target_link_libraries(${SMOKE}
        PRIVATE
        silent_rust
        ssl
        crypto
        pthread
        dl
        udev
    )

    if(UNIX AND NOT APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        target_link_options(${SMOKE} PRIVATE
            "-Wl,--export-dynamic"
        )

        set(SMOKE_QT_AND_QONTROL_LIBS
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Svg
            qontrol
        )

        set(SMOKE_ELF_LINK_LIBS)
        foreach(SMOKE_LIB ${SMOKE_QT_AND_QONTROL_LIBS})
            get_target_property(SMOKE_LIB_TYPE ${SMOKE_LIB} TYPE)
            if(SMOKE_LIB_TYPE STREQUAL "STATIC_LIBRARY")
                list(APPEND SMOKE_ELF_LINK_LIBS "$<LINK_LIBRARY:WHOLE_ARCHIVE,${SMOKE_LIB}>")
            else()
                list(APPEND SMOKE_ELF_LINK_LIBS "${SMOKE_LIB}")
            endif()
        endforeach()

        target_link_libraries(${SMOKE} PRIVATE ${SMOKE_ELF_LINK_LIBS})
    else()
        target_link_libraries(${SMOKE}
            PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Svg
            qontrol
        )
    endif()
endif()

# Opt-in device GUI end to end probe (tests/e2e/devices/run.sh). OFF by default so it
# never affects the normal build. When ON it links the shared application sources, as
# host_smoke does, and drives the real app against Speculos and the Coldcard simulator.
if(SILENT_BUILD_DEVICE_E2E)
    set(DEVICE_E2E device_e2e)

    qt_add_executable(${DEVICE_E2E}
        tests/e2e/devices/probe/main.cpp
        ${SILENT_APP_SOURCES}
    )

    add_dependencies(${DEVICE_E2E} RunBeforeBuild qontrol)

    # The catalogs, so TR() returns real text.
    qt_add_resources(${DEVICE_E2E} device_e2e_i18n_lang
        PREFIX "/i18n"
        FILES ${TRANSLATION_LANG_FILES}
    )

    target_include_directories(${DEVICE_E2E}
      PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/modules/sp>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include>
    )

    target_link_libraries(${DEVICE_E2E}
        PRIVATE
        silent_rust
        ssl
        crypto
        pthread
        dl
        udev
    )

    if(UNIX AND NOT APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        target_link_options(${DEVICE_E2E} PRIVATE
            "-Wl,--export-dynamic"
        )

        set(DEVICE_E2E_ELF_LINK_LIBS)
        foreach(DEVICE_E2E_LIB Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg qontrol)
            get_target_property(DEVICE_E2E_LIB_TYPE ${DEVICE_E2E_LIB} TYPE)
            if(DEVICE_E2E_LIB_TYPE STREQUAL "STATIC_LIBRARY")
                list(APPEND DEVICE_E2E_ELF_LINK_LIBS "$<LINK_LIBRARY:WHOLE_ARCHIVE,${DEVICE_E2E_LIB}>")
            else()
                list(APPEND DEVICE_E2E_ELF_LINK_LIBS "${DEVICE_E2E_LIB}")
            endif()
        endforeach()

        target_link_libraries(${DEVICE_E2E} PRIVATE ${DEVICE_E2E_ELF_LINK_LIBS})
    else()
        target_link_libraries(${DEVICE_E2E}
            PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Svg
            qontrol
        )
    endif()
endif()
