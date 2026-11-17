# ---- ADR-001：与模拟器共享 UI 源码（Phase 6 UI 移植）----
# 模拟器侧新增 .c 文件时，此处与 simulator/lvgl-pc/CMakeLists.txt 同步追加。
set(SIM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../simulator/lvgl-pc/src)

set(UI_SRC_LIST
    # UI（Launcher / 状态栏 / 各 App 页）
    ${SIM_SRC_DIR}/ui/app_manager.c
    ${SIM_SRC_DIR}/ui/companion.c
    ${SIM_SRC_DIR}/ui/ui_app_pages.c
    ${SIM_SRC_DIR}/ui/ui_conversation.c
    ${SIM_SRC_DIR}/ui/ui_gesture.c
    ${SIM_SRC_DIR}/ui/ui_keys.c
    ${SIM_SRC_DIR}/ui/ui_launcher.c
    ${SIM_SRC_DIR}/ui/ui_worker.c
    ${SIM_SRC_DIR}/ui/icons/icon_embed.c
    ${SIM_SRC_DIR}/ui/ui_settings.c
    ${SIM_SRC_DIR}/ui/ui_files.c
    ${SIM_SRC_DIR}/ui/ui_pomodoro.c
    ${SIM_SRC_DIR}/ui/ui_wifi_panel.c
    ${SIM_SRC_DIR}/ui/ui_statusbar.c
    ${SIM_SRC_DIR}/ui/ui_theme.c
    ${SIM_SRC_DIR}/ui/ui_weather.c
    ${SIM_SRC_DIR}/ui/ui_sound.c
    # 中文字体子集（GB2312-L1，gen_cn_font.sh 生成；flash 预算见 PLAN §14）
    ${SIM_SRC_DIR}/ui/fonts/font_cn_12.c
    ${SIM_SRC_DIR}/ui/fonts/font_cn_14.c
    ${SIM_SRC_DIR}/ui/fonts/font_cn_16.c
    ${SIM_SRC_DIR}/ui/fonts/font_cn_20.c
    ${SIM_SRC_DIR}/ui/fonts/font_cn_24.c
    ${SIM_SRC_DIR}/ui/fonts/font_digits_84.c
    ${SIM_SRC_DIR}/ui/fonts/font_digits_64.c
    ${SIM_SRC_DIR}/ui/fonts/font_digits_56.c
    ${SIM_SRC_DIR}/ui/icons/pomo_mascot.c
    ${SIM_SRC_DIR}/ui/fonts/font_pomo_20.c
    # 数据源（mock 起步；Wi-Fi 就绪后 ds_init 切 bridge）
    ${SIM_SRC_DIR}/data/data_source.c
    ${SIM_SRC_DIR}/data/data_source_mock.c
    ${SIM_SRC_DIR}/data/data_source_bridge.c
    ${SIM_SRC_DIR}/data/data_source_wifi.c
    ${SIM_SRC_DIR}/data/data_source_files.c
    ${SIM_SRC_DIR}/data/data_source_devstats.c
    ${SIM_SRC_DIR}/data/http_client.c
    ${SIM_SRC_DIR}/data/mock_data.c
    # 语音会话：voice_session 状态机 + mock（模拟器双端兼容）+ esp32 真机 ops
    ${SIM_SRC_DIR}/voice/voice_session.c
    ${SIM_SRC_DIR}/voice/audio_mock.c
    ${SIM_SRC_DIR}/voice/audio_esp32.c
)

idf_component_register(SRCS "main.c" ${UI_SRC_LIST}
    INCLUDE_DIRS "." "${SIM_SRC_DIR}" "${SIM_SRC_DIR}/ui" "${SIM_SRC_DIR}/ui/fonts"
    "${SIM_SRC_DIR}/ui/icons" "${SIM_SRC_DIR}/data" "${SIM_SRC_DIR}/voice"
    PRIV_REQUIRES
        cjson                                   # data_source_bridge.c 解析 Bridge 响应
        waveshare__esp32_s3_touch_amoled_2_16   # BSP（显示/触摸/音频/I2C）
        espressif__esp_codec_dev                # audio_esp32：ES8311/ES7210 读写
        freertos                                # audio_esp32：采集/播放任务
        power_manager                           # AXP2101 电池/充电
        wifi_prov                               # 运行期配网（NVS+扫描+试连）
        esp_wifi esp_event esp_netif nvs_flash  # Wi-Fi/NVS
        esp_timer                               # esp_timer_get_time
        esp_driver_gpio esp_driver_i2c          # KEY3 ISR / I2C 扫描探针
        esp_driver_tsens                        # devstats：芯片温度传感器
        app_update spi_flash fatfs              # devstats：OTA 描述/分区/FATFS 用量
        pthread                                 # esp_pthread_set_cfg：线程栈放 PSRAM
        lwip)                                   # esp_sntp 对时

# 临时排障：连续翻页压测（GADGET_STRESS_ANIM=0 带动画复现花屏）
target_compile_definitions(${COMPONENT_LIB} PRIVATE GADGET_STRESS_FLIP GADGET_STRESS_ANIM=0)
