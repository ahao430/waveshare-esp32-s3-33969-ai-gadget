/* battery_axp2101 — AXP2101 PMU 电池/充电状态读取。
 *
 * 寄存器语义（2026-11-14 定版，三源交叉验证 + 真机反推一致）：
 *   M5Unified AXP2101_Class.cpp / Zephyr charger_axp2101.c /
 *   OpenNextion xiaozhi axp2101.cc
 *   - 0x00 STATUS1: bit5=VBUS 在位, bit3=电池在位
 *   - 0x01 STATUS2: bit[6:5] 电流方向（1=充电 2=放电 0=待机）;
 *                   bit[2:0] 充电阶段（0-3=涓流/预充/恒流/恒压,
 *                   4=充电完成, 5=未充电）
 *   - 0x34/0x35: 电池电压 14bit（(H&0x3F)<<8|L，单位 mV）
 *   - 0xA4: 电量百分比 0-100
 * 真机首验：reg00=0x38（VBUS+电池在位）、reg01=0x32（恒流充电中）、
 * reg03=0x4A（ID）。
 */
#include "battery.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

#define AXP2101_I2C_ADDR 0x34

static i2c_master_bus_handle_t g_bus;
static bool g_present;
/* 常驻设备句柄：此前每次读写都 add/rm device——传输超时（等待 -1）后
 * 立即 rm 会报 "Wrong I2C status, cannot delete device" 且句柄泄漏
 * （PTT 期间与 codec 抢总线时必现，2026-11-17 串口日志实证），长期
 * 运行累积。init 时挂一次复用；传输出错后 rm 重挂自愈。 */
static i2c_master_dev_handle_t g_dev;

esp_err_t battery_init(void *i2c_bus)
{
    g_bus = (i2c_master_bus_handle_t)i2c_bus;
    if (g_bus == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = i2c_master_probe(g_bus, AXP2101_I2C_ADDR, 50);
    g_present = (err == ESP_OK);
    if (g_present) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        /* 挂失败不阻塞 init：read_regs/write_reg 的 g_dev==NULL 分支兜底 */
        (void)i2c_master_bus_add_device(g_bus, &dev, &g_dev);
    }
    ESP_LOGI(TAG, "AXP2101 @0x%02X probe: %s", AXP2101_I2C_ADDR,
             g_present ? "ACK" : esp_err_to_name(err));
    return g_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* 传输出错后句柄状态不可靠：rm 重挂，避免永久泄漏 */
static void dev_recover(void)
{
    if (g_dev) {
        (void)i2c_master_bus_rm_device(g_dev);
        g_dev = NULL;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    (void)i2c_master_bus_add_device(g_bus, &dev, &g_dev);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *val, size_t len)
{
    if (!g_dev) { /* init 时 probe 失败的兜底：按需挂（旧语义） */
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(g_bus, &dev, &g_dev) != ESP_OK) {
            g_dev = NULL;
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t err = i2c_master_transmit_receive(g_dev, &reg, 1, val, len, -1);
    if (err != ESP_OK) dev_recover();
    return err;
}

/* 电压→可用电量（保守 LiPo 曲线，放电方向）。AXP2101 无库仑计，
 * 0xA4 本身也是电压估算——此表用于放电态的显示上限钳制。 */
static int volt_pct(int mv)
{
    static const struct { int mv; int pct; } t[] = {
        {4150, 100}, {4050, 90}, {3950, 78}, {3850, 62}, {3750, 45},
        {3650, 30}, {3550, 20}, {3450, 12}, {3350, 6}, {3250, 2},
    };
    if (mv >= t[0].mv) return 100;
    for (int i = 1; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        if (mv >= t[i].mv) {
            int span = t[i - 1].mv - t[i].mv;
            int f = span ? (t[i - 1].mv - mv) * 100 / span : 0;
            return t[i].pct + (t[i - 1].pct - t[i].pct) * f / 100;
        }
    }
    return 0;
}

esp_err_t battery_get_status(battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!g_present || g_bus == NULL) return ESP_ERR_NOT_SUPPORTED;

    uint8_t s0 = 0, s1 = 0, vb[2] = { 0 }, pct = 0;
    if (read_regs(0x00, &s0, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0x01, &s1, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0x34, vb, 2) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0xA4, &pct, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;

    status->battery_present = (s0 & 0x08) != 0;
    status->usb_connected = (s0 & 0x20) != 0;

    int dir = (s1 >> 5) & 0x3; /* 1=充电 2=放电 */
    int phase = s1 & 0x7;      /* 0-3 充电中，4 完成，5 未充 */
    status->charging = (dir == 1) || (phase <= 3);

    status->battery_voltage_mv = ((vb[0] & 0x3F) << 8) | vb[1];
    status->battery_percent = pct;

    /* 显示电量钳制（2026-11-20）：0xA4 是 PMU 电压估算电量，负载重时
     * 偏高（准开路电压），电池耗尽边沿出现过"显示 80%+ 欠压硬断电"。
     * 放电态（无 USB 不充电）用电压曲线 +150mV 负载压降补偿做上限：
     * 宁可偏低，不让用户在高位百分比下被断电打个措手不及；充电态
     * 维持 0xA4 原值。 */
    if (status->battery_present && !status->usb_connected && !status->charging) {
        int cap = volt_pct(status->battery_voltage_mv + 150);
        if (status->battery_percent > cap) status->battery_percent = cap;
    }

    ESP_LOGD(TAG, "s0=%02X s1=%02X vb=%dmV pct=%d%% usb=%d bat=%d chg=%d",
             s0, s1, status->battery_voltage_mv, pct,
             status->usb_connected, status->battery_present, status->charging);
    return ESP_OK;
}

/* ---- PWR 键（2026-11-16，用户反馈"长按电源键关机有点问题"）----
 * 之前固件从未配置 PWR 键，行为完全取决于 PMU 出厂默认——有的芯片
 * 16s 关机位出厂开着、长按关机使能与时长也不确定，不可预测。
 * 寄存器语义照抄本板（waveshare_s3_216）实测过的 Muse SDK muse_pmu.c：
 *   0x10 COMMON_CFG：bit2=PWRON 16s 关机（防御性关闭，datasheet 默认关
 *                    但有芯片出厂开）；bit0=软关机命令（自清）；bit1=复位
 *                    命令——读改写时绝不能把 1 写进 [1:0]
 *   0x22 PWROFF_EN：bit1=长按过关断阈值即硬关
 *   0x27 IRQ_LEVEL：bit[3:2]=长按至关机时间（11=10s，最长的兜底）
 *   0x41 INTEN2 / 0x49 INTSTS2：bit0=释放沿 bit1=按下沿 bit2=长按
 *                    bit3=短按；状态位写 1 清零（IRQ 引脚未接也可轮询） */
#define REG_COMMON_CFG 0x10
#define REG_PWROFF_EN  0x22
#define REG_IRQ_LEVEL  0x27
#define REG_INTEN2     0x41
#define REG_INTSTS2    0x49
#define PKEY_ALL       0x0Fu

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    if (!g_dev) { /* 同 read_regs 的按需挂兜底 */
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(g_bus, &dev, &g_dev) != ESP_OK) {
            g_dev = NULL;
            return ESP_ERR_INVALID_STATE;
        }
    }
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(g_dev, buf, sizeof(buf), -1);
    if (err != ESP_OK) dev_recover();
    return err;
}

static esp_err_t update_reg(uint8_t reg, uint8_t mask, uint8_t set)
{
    uint8_t v = 0;
    esp_err_t err = read_regs(reg, &v, 1);
    if (err != ESP_OK) return err;
    return write_reg(reg, (uint8_t)((v & (uint8_t)~mask) | set));
}

esp_err_t battery_keys_init(void)
{
    if (!g_present || g_bus == NULL) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err;
    /* 长按至关机 = 10s（最长时间，固件楔死时的硬兜底；正常关机走
     * battery_power_off 的软件关机，比它早得多） */
    if ((err = update_reg(REG_IRQ_LEVEL, 0x0C, 0x0C)) != ESP_OK) return err;
    /* 长按过关断阈值即硬关（使能；配合上一条 = 长按 10s 硬关兜底） */
    if ((err = update_reg(REG_PWROFF_EN, 0x02, 0x02)) != ESP_OK) return err;
    /* 防御：关掉"PWRON 16s 关机"（有的芯片出厂开；bit1:0 是命令位，只清 bit2） */
    if ((err = update_reg(REG_COMMON_CFG, 0x04, 0x00)) != ESP_OK) return err;
    /* 键事件状态位使能 + 清历史挂起 */
    if ((err = update_reg(REG_INTEN2, PKEY_ALL, PKEY_ALL)) != ESP_OK) return err;
    if ((err = write_reg(REG_INTSTS2, PKEY_ALL)) != ESP_OK) return err;

    ESP_LOGI(TAG, "PWR key armed: 10s hard-off fallback, events on");
    return ESP_OK;
}

unsigned battery_poll_key(void)
{
    if (!g_present || g_bus == NULL) return 0;
    uint8_t sts = 0;
    if (read_regs(REG_INTSTS2, &sts, 1) != ESP_OK) return 0;
    sts &= PKEY_ALL;
    if (!sts) return 0;
    (void)write_reg(REG_INTSTS2, sts); /* 写 1 清零 */

    unsigned ev = 0;
    if (sts & 0x02) ev |= BATT_KEY_PRESS;
    if (sts & 0x01) ev |= BATT_KEY_RELEASE;
    if (sts & 0x08) ev |= BATT_KEY_CLICK;
    if (sts & 0x04) ev |= BATT_KEY_LONG;
    return ev;
}

esp_err_t battery_power_off(void)
{
    if (!g_present || g_bus == NULL) return ESP_ERR_NOT_SUPPORTED;
    /* 只写 bit0 软关机；bit1 是复位命令，绝不能带 1（muse_pmu 同款防御） */
    return update_reg(REG_COMMON_CFG, 0x03, 0x01);
}
