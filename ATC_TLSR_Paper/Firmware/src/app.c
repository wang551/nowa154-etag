#include <stdint.h>
#include "tl_common.h"
#include "app.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "battery.h"
#include "ble.h"
#include "flash.h"
#include "nfc.h"
#include "ota.h"
#include "epd.h"
#include "time.h"
#include "bart_tif.h"

RAM uint8_t battery_level;
RAM uint16_t battery_mv;
RAM int16_t temperature;

RAM uint8_t hour_refresh = 100;
RAM uint8_t minute_refresh = 100;

// Called when the BLE image hold is released: invalidate minute_refresh so the
// next main_loop pass redraws the clock immediately (visible confirmation).
void epd_resume_clock(void)
{
    minute_refresh = 100;
}

// Settings
extern settings_struct settings;

_attribute_ram_code_ void user_init_normal(void)
{                            // this will get executed one time after power up
    random_generator_init(); // must
    init_time();
    init_ble();
    init_flash();
    init_nfc();

    // v42 收档：NFC 实验期曾在此强制 epd_hold_image=1 停刷（EPD 分钟刷新的大电流
    // 会把 NFC 芯片打到掉电楔死，v12 日志实锤）。抓取器已随战役收档默认关闭
    // （nfc.c NFC_LOGGER=0），时钟恢复正常分钟刷新；实验请直接刷 bin/ 的
    // nfcexp/v40/v41 历史固件（自带锁屏 + 常驻抓取）。

    // epd_display_tiff((uint8_t *)bart_tif, sizeof(bart_tif));
    // epd_display(3334533);
}

_attribute_ram_code_ void user_init_deepRetn(void)
{ // after sleep this will get executed
    nfc_note_retention_wake();
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3p01dBm);
    blc_ll_recoverDeepRetention();
}

_attribute_ram_code_ void main_loop(void)
{
    blt_sdk_main_loop();
    handler_time();
    nfc_logger_poll(); // NFC RF 事件抓取（变化/IRQ 沿/心跳 → flash 0x41000 区）

    if (time_reached_period(Timer_CH_1, 30))
    {
        battery_mv = get_battery_mv();
        battery_level = get_battery_level(battery_mv);
        temperature = get_temperature_c();
        set_adv_data(EPD_read_temp() * 10, battery_level, battery_mv);
        ble_send_battery(battery_level);
        ble_send_temp(EPD_read_temp() * 10);
    }

    uint8_t current_minute = (get_time() / 60) % 60;
    if (current_minute != minute_refresh && !epd_hold_image) // hold: keep BLE-uploaded image instead of the clock
    {
        minute_refresh = current_minute;
        uint8_t current_hour = ((get_time() / 60) / 60) % 24;
        if (current_hour != hour_refresh)
        {
            hour_refresh = current_hour;
            epd_display(get_time(), battery_mv, temperature, 1);
        }
        else
        {
            epd_display(get_time(), battery_mv, temperature, 0);
        }
    }

    if (time_reached_period(Timer_CH_0, 10))
    {
        if (ble_get_connected())
            set_led_color(3);
        else
            set_led_color(2);
        WaitMs(1);
        set_led_color(0);
    }

    if (epd_state_handler()) // if epd_update is ongoing enable gpio wakeup to put the display to sleep as fast as possible
    {
        cpu_set_gpio_wakeup(EPD_BUSY, 1, 1);
        bls_pm_setWakeupSource(PM_WAKEUP_PAD);
        bls_pm_setSuspendMask(SUSPEND_DISABLE);
    }
    else if (nfc_session_active()) // v19：RF 会话期禁 suspend——nfc.c 以 8 圈粒度嗅 FIFO，
    {                             // suspend（广播间隔 1s）会把 <200ms 的整场 T4T 交换睡过去
        bls_pm_setSuspendMask(SUSPEND_DISABLE);
    }
    else
    {
        blt_pm_proc(); // 内部每次重设 suspend 掩码，会话结束后自动恢复正常低功耗
    }
}
