/* ui_wifi_panel — 运行期配网全屏面板（ADR-009）：
 *   ui_wifi_panel_open()        Wi-Fi 列表（扫描/选网/软键盘输密码/改密）
 *   ui_bridge_host_panel_open() Bridge 地址编辑（保存即 rebind 生效）
 * 交互范式照 bot picker（ui_conversation.c）：后台线程做阻塞网络操作，
 * UI 构建全部在 LVGL 线程（点击回调或 lv_async_call）。 */
#ifndef UI_WIFI_PANEL_H
#define UI_WIFI_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

void ui_wifi_panel_open(void);
void ui_bridge_host_panel_open(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIFI_PANEL_H */
