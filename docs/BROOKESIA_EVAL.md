# esp-brookesia 复用评估（2026-11-16 复评）

> 结论先行：**技术可用，工程不划算**。维持 ADR-008 的自研固件基线，
> 不引入 esp-brookesia 框架；借鉴它的三个机制（App 状态栏指示器、
> manifest 生命周期标志、定时器清理契约）。决策见 ADR-012。

## 1. 这次核实的事实

用户确认「出厂固件 = Launcher + 多应用，底座是 ESP-Brookesia」。复核
仓库与真机备份后，事实如下：

| 事实 | 证据 |
|------|------|
| 出厂固件确实基于 Brookesia | 16MB 备份中 `brookesia` 字符串 148 处；出厂分区表 `factory` app **8MB** @0x200000 |
| 出厂 App 镜像实际大小 | 6 个段合计 ≈ **5.52MB**（最后一个非 0xFF 字节 0x563A2F ≈ 5.39MB） |
| 出厂 App 清单（官方说明） | DrawPanel / squareline / Calculator / AIChats / SpecAnalyzer / Gravity Ball / VideoPlayer / MusicPlayer / Settings（FACTORY_REVIEW §9） |
| 出厂 App 源码是否公开 | **否**。仓库只提供恢复 BIN；`firmware/` 无可构建源码 |
| `03_esp-brookesia` 示例内容 | `brookesia_core`（含 Phone/Speaker 系统）+ **1 个 Squareline demo**；出厂那 9 个 App 的实现不在仓库 |
| 示例与我们的工具链兼容性 | **可编译**：IDF v6.0.3 + BSP 2.0.1 + LVGL 9.4，1937/1937 编译单元通过（2026-11-14 与 11-16 两次复跑） |

## 2. 尺寸账（关键否决项）

同一台 Mac、同一套 IDF v6.0.3 工具链实测：

| 项目 | Flash 镜像 | 说明 |
|------|-----------|------|
| `03_esp-brookesia` 示例 | **2,545,744 B（2.43MB）**，4MB 分区余 39% | 只有框架 + Phone 壳 + 1 个 demo |
| 其中 `brookesia_core` | **630,960 B** | 代码 129,793 + rodata 501,087（Phone 样式/资源） |
| 其中 `libstdc++` | **203,043 B** | C++ 运行时（另有 `.eh_frame` 34KB） |
| 其中 demo app | 267,892 B | 一个 Squareline 界面 |
| 其中 LVGL | 1,042,218 B | 示例开了 8–44px Montserrat 全家桶 |
| **我们的固件** | **3,979,232 B**，4MB 分区余 **216,096 B（5%）** | 9 个 App + 全部字体/图标/语音/配网/TF |
| 其中 `libmain.a`（我们的 UI+数据层+素材） | 2,264,912 B | rodata 2,096,326（字体/图标/素材）+ 代码 82KB |
| 出厂固件 | 5.39MB，**8MB** app 分区 | 框架 + 9 个 App + 完整素材 |

**结论**：换成 Brookesia，先要额外吃进约 **0.8–1.1MB**（core +
libstdc++ + Phone 资源），而我们只剩 216KB。要么砍字体/图标，要么
重划分区（放弃 OTA 双槽可给 app 8MB），两者都是产品级取舍，不是
「拿来就用」。

## 3. 为什么工程上不划算

1. **只给壳，不给应用**：复用框架 ≠ 得到出厂体验。出厂 9 个 App 的
   源码不在仓库，仍要自己实现；我们现在的 9 个 App 反而已经能用。
2. **迁移面覆盖整个 UI 层**：9 个 App 要从 C `app_def_t` 改成 C++
   `systems::phone::App` 子类（run/back/pause/resume）；Launcher、
   状态栏、手势、主题全部换成 Brookesia 的 Phone 系统；我们的
   `data_source`/Bridge/语音/TF 要包成服务或全局单例。
3. **圆屏与定稿 UI**：Phone 面向矩形大屏（官方 demo 1024×600），
   四宫格/状态栏/导航栏样式要重做；我们为这块板写死的性能规避
   （内部 RAM 10 行双缓冲、图标错峰挂载、禁拖动、局部失效、LVGL
   心跳看门狗）全部要重新验证，回归风险高。
4. **C++ 运行时成本**：libstdc++ + 异常展开表（`.eh_frame` 34KB）
   + `std::unordered_map`/`list`/`shared_ptr` 对内部 RAM 和碎片化的
   影响要实测；我们踩过的 16KB 栈 ENOMEM、PSRAM 栈 InstrFetch 等坑
   会叠加新的不确定性。
5. **产品定位不匹配**：Brookesia 的价值在应用商店、动态运行时
   （ELF/JS/Lua/WASM）、JSON UI、第三方 App 分发。我们是固定 9 App
   的个人 AI 终端，静态 `app_def_t` + 手写 LVGL 更直接、更可控。

**适用条件**：如果哪天产品要「第三方 App 上架/动态安装/JSON UI」，
再评估 upstream v0.8（要求 IDF 6.0–6.2，正好覆盖我们）。届时是
新分支/新固件工程，不是在本仓库里渐进替换。

## 4. 值得借鉴的机制（按优先级）

### P1：每 App 状态栏指示器（Brookesia `status_icon`）——已落地

Brookesia 的 App 配置里有 `status_icon` + `status_icon_area_index`：
App 退到后台后，状态栏仍保留一个小图标/状态。我们目前状态栏只有
Wi-Fi/电池，番茄钟后台计时在别的页面完全不可见。

**实现（2026-11-16）**：`ui_statusbar` 增加单 label 药丸
（`ui_statusbar_set_app_status(text, color)`），番茄钟在既有 1s 节流
`update_ui` 里同步：前台隐藏（页面大计时器已显示），后台运行显示
`🔔 24:31`（阶段亮色），暂停/停止隐藏。单 label 自带圆角背景 +
内边距，不额外建容器；分配失败静默跳过。

**实测成本**：
- Flash **+768 B**（3,978,208 → 3,978,976；P1 落地时余 215,328 B；
  含后续布局/字体/订阅 worker 修复后当前总余 221,328 B / 5%）；
- 内部 RAM：一个 `lv_label_t` + 短文本（<1KB），**无新任务/栈**；
- 设备启动水位（2026-11-16 复测）：post-ui internal 72,899 B，
  运行期 probe internal 26,571 B、largest_int 12,788 B（打开订阅 App
  后）——药丸 1KB 级分配在 largest block 内，不会失败；
- 番茄钟退出时页面 arc/labels/按钮先释放，药丸是"替换"而非叠加。

后续音乐播放、文件传输可复用同一槽；状态槽的入口位置（左/中/右）
目前固定顶部中央，多 App 同时需要时再扩展。

### P2：把后台规则写进 App manifest —— 已落地

ADR-011 的后台规则目前是文档表格，新增 App 靠人记。Brookesia 把
`enable_recycle_resource`、`use_status_bar`、`pause/resume` 做成
App 配置位。我们给 `app_def_t` 末尾加了 `flags`：

```c
#define APP_FLAG_BG_TIMER    (1u << 0)  /* 退出后定时器仍运行（番茄钟） */
#define APP_FLAG_STATUSBAR   (1u << 1)  /* 持有状态栏指示器 */
#define APP_FLAG_KEEP_STATE  (1u << 2)  /* 重进时恢复内部状态而非重建 */
```

番茄钟已标 `APP_FLAG_BG_TIMER | APP_FLAG_STATUSBAR`；`appmem` 日志
带上 `flags=0x3`，评审时一眼能看出后台语义。每 App 成本 4B，无行为
变化（`KEEP_STATE` 目前仅声明，未做强制）。

### P2：定时器清理契约 —— 已落地

Brookesia 在 `run()/pause()` 期间自动记录屏幕、timer、anim，退出时
统一清理。我们的 `on_exit` 是显式的，已经踩过「timer 不随屏幕释放」
（launcher 页点、番茄钟）。已加轻量包装：

```c
lv_timer_t *app_manager_timer_create(lv_timer_cb_t cb, uint32_t period, void *user);
void app_manager_timer_delete(lv_timer_t *t);
```

创建时登记到当前 App（`app_manager_open` 现在在 `build()` 之前就设置
`g_current_def`，保证 owner id 正确）；`app_manager` 切走 App 时统一
删除未释放的登记 timer。订阅页的倒计时/自动刷新两个 timer 已改用该
接口，`on_exit` 只断引用。

**验证**：模拟器从订阅页返回桌面输出
`[app] timer cleanup app=0 left=0`；固件同样打 `app` 警告日志。
**边界**：只登记"重复 timer"；一次性 timer 会被 LVGL 自动删除，登记表
会悬垂，因此不要登记；后台常驻 timer（番茄钟）也不登记。

### P3：其他可参考、暂不落地

- **可视区域助手** `ui_safe_rect()`：统一状态栏/圆屏安全区，替代各页
  手写坐标（新 App 收益，老页面不强改）。
- **服务层接口**：Brookesia v0.8 的 service manager（HTTP/Storage/
  WiFi/audio）可作为 ADR-011 暂缓的 Chat Service/Storage Manager 的
  接口参考，不引入实现。
- **Recents/任务列表/快照**：RAM 与圆屏 UX 不匹配；单前台 + 番茄钟
  后台已满足当前需求，暂不做。
- **App Store / 动态运行时 / JSON UI**：与产品定位不符，不做。

## 5. 如果一定要迁移：成本估算

- 9 个 App：每个 200–600 行 C → C++ 类，逐页重做并回归触摸/性能；
- Launcher/状态栏/手势/主题：换成 Phone 系统并适配 480×480 圆屏；
- 数据层：`data_source`/Bridge/语音/TF 包成服务或单例；
- 字体/图标：重做资源管线（Brookesia 的字体/图标体系不同）；
- 真机稳定性：显示管线、内部 RAM、语音回合、配网、TF 全部重验；
- 预估 **2–3 周**，且 flash 超预算概率高；期间现有功能冻结。

替代路线：如果只是想要「原厂味」的具体功能（任务列表、音乐后台），
直接在现有 C 框架里做——音乐后台任务 + 简单最近应用切换，约
1–2 天/项，风险远低于换框架。

## 6. 验证记录

- 2026-11-14：`03_esp-brookesia` 在本仓库 IDF v6.0.3 下完整编译通过
  （FACTORY_REVIEW §1）。
- 2026-11-16 复跑：
  - `idf.py set-target esp32s3 build` → 1937/1937，`esp-brookesia.bin`
    2,545,744 B，4MB 分区余 39%；
  - `idf.py size-components`：brookesia_core 630,960 B、libstdc++
    203,043 B、demo 267,892 B、LVGL 1,042,218 B；
  - 本仓库固件 `idf.py size`：3,979,232 B，余 216,096 B（5%）；
  - 出厂备份解析：分区表 @0x8000，factory app 8MB @0x200000，镜像
    5.39MB，`brookesia` 字符串 148 处。
- **未刷入示例固件**：避免覆盖现有设备固件。如需对照，可刷到 `ota_0`
  分区并切 otadata（需要额外确认，且会改变启动分区）。