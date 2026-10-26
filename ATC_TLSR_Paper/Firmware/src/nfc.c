#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "drivers/8258/flash.h"
#include "i2c.h"
#include "drivers/8258/i2c.h"
#include "nfc.h"
#include "main.h"
#include "time.h"

// ---------------------------------------------------------------------------
// FM11NC08I 只读探测 + RF 事件抓取（阶段 4）
//
// 协议结论（真机实证，详见 docs/hardware_notes.md）：
//  - I2C 8bit 写地址 0xAE（7bit 0x57）；CSN 低电平 = 接触侧电源使能
//  - 记忆访问帧 = START + 0xAE(W) + 2 字节大端地址 + [重启 + 0xAF(R)]
//  - 寄存器窗 0xFFF1-0xFFFD：FifoClear/FifoWordCnt/NfcStatus/TxEn/Cfg/Rats/
//    MainIrq/FifoIrq/AuxIrq/MainIrqMask/FifoIrqMask/AuxIrqMask/(rFFFD)
//    —— 与二代同族 solokeys fm11nt082c NC 模式表逐位同源（v3 基线快照印证）
//  - 0xFFF0 = FIFO 数据口（读会弹出，仅在 cnt>0 时读）
//  - 上游"复位序列"{0x03,0xB5,0xA0} = 向配置区 0x3B5（TB）写 0xA0（本标签出厂值），
//    幂等重写、非复位——本固件仅在解卡恢复流程第 3 级使用它做实验
//
// 真机根因（v4 轨迹环实锤）：MCU 首次进 deep retention（开机 ~19s）时数字引脚配置
// 丢失，I2C pad 在事务中途死亡 → FM11 从机卡在半字节状态（全 0xFF + IRQ 常低），
// CSN 脉冲救不活；卡死总线上 SDK 无界 busy-wait 会把 main_loop 冻结 40s+。
// v5 对策：
//  1) 每次轮询"挂载-卸载"总线：轮询外 pad 回 GPIO 输入 + CSN 断电——休眠瞬间
//     总线上绝无事务，从机无从被卡（挂载态的芯片对 pad 毛刺也免疫）
//  2) I2C 原语全部有界等待 + 超时重建外设（杜绝 main_loop 冻结）
//  3) 卡死恢复三级流水：GPIO 手动 9 时钟解卡 → CSN 脉冲 → 复位序列写，逐级验证
//
// flash 结果区（OTA 暂存区 0x20000-0x40000 之后的空闲扇区）：
//   0x40000 一次探测结果（v1 布局 0x628B，cmd4 读回）
//   0x41000-0x44FFF 抓取日志：页 0 = 元数据（魔数 + 逐字节别名检查），此后每页一条：
//     快照 [0-3]时间戳 [4]标志(bit0窗 bit2心跳 bit3IRQ沿 bit4FIFO非空 bit5卡死
//       bit6=CSN脉冲自愈 bit7=复位序列自愈) [5]序号 [6]IRQ电平 [7]卡死计数
//       [8-20]寄存器0xFFF1-0xFFFD [21]FIFO计数 [22-53]FIFO内容 [54]retention唤醒数
//       [55-58]时钟原始tick
//     轨迹转储 [4-7]='TRC1' + 24 条 [tick4 irq1 mainirq1 fifocnt1 status1]
// RAM 复用 epd_temp（OBD 虚拟屏缓冲，抓取前查 epd_update_state）。
// ---------------------------------------------------------------------------

#define NFC_RESULT_ADDR 0x40000
#define NFC_I2C_ID_7BIT 0x57
#define NFC_I2C_ID_WR   0xAE
// v18 结果区扩展：0x650 起 = EEPROM 写解锁策略矩阵（'NFM1'）
//   [0x654]=成功策略 id（0=全失败） [0x655]=NDEF 改写标志
//   [0x656..0x65D]=NDEF 块写后读回 0x010 前 8B
//   0x660 起 8 条 x 10B：[id][AuxIrq][读回 0x100 前 8B]
#define NFC_MATRIX_OFF 0x650
#define NFC_RESULT_LEN 0x6C0

// v14 干净版开关：EEPROM/寄存器写全部禁用（复位序列与低位地址写都危险）
#define NFC_EXPERIMENT 0
// 抓取器开关。v14-v41 实验期常开（配合 app.c 强制锁屏停刷：EPD 刷新大电流会把
// 芯片打到掉电楔死）；v42 收档改 0 —— 日常版恢复时钟分钟刷新 + 低功耗（suspend
// 正常，接触侧 CSN 常高断电待机），碰卡检测由芯片 RF 侧硬件自治（防冲突+RATS，
// v40 空闲期同姿态实证）。冷启动探测、BLE 控制台、T4T 应答器代码均保留不受影响。
#define NFC_LOGGER 0
// v27：实验一 —— maotek 最小驱动旁路（docs/nfc_reference_diff.md §6）。
// 同硬件参考 maotek/hanshow_stellar 的 nfc.c 整移植：I²C 全 2 字节编址（EEPROM 与
// 寄存器窗统一地址空间）、零掩码、零配置写，IRQ 电平入口 + MAIN_IRQ 读清 + 子寄存器
// 补读。一次实验裁决"自扰(P4) vs 出厂门控(假设B)"。MCU 加固（retention 引脚重建、
// 有界 I²C、楔死恢复、EPD hold）与 BLE 控制台全部保留；旧 v24/v25 路径 #else 备用。
#define NFC_MAOTEK_MODE 1
// v22：接触侧常上电（CSN 永久低）。v26 收档改回 0：转发路径已被证明受出厂人格
// 门控（见 hardware_notes），常上电不再有收益，回到 v16 的轮换省电模式。
#define NFC_PERMANENT_ATTACH 0

#define NFC_LOG_ADDR    0x41000
#define NFC_LOG_SECTORS 4
#define NFC_LOG_PAGES   (NFC_LOG_SECTORS * 16) // 64 页，页 0 元数据

#define NFC_I2C_TMO     20000 // 有界等待自旋上限（~几 ms），超时视为总线卡死

// flash 结果区布局（@0x40000，共 0x628 = 1576 字节）：
//   0x000 'NFCP' 魔数 / 0x004 版本(1) / 0x005 状态 / 0x006 I2C 分频 /
//   0x007 位样 bit0=扫描前 IRQ_N bit1=CSN 拉低后 IRQ_N（高=1）
//   0x008 ACK 位图·CSN 低（7bit 0x00-0x7F） / 0x018 ACK 位图·CSN 高
//   0x028 EEPROM 0x0000-0x03FF，2 字节地址式（1024B，含 0x390-0x3C0 配置区）
//   0x428 0x0000-0x00FF，1 字节地址式（256B，寻址假设对照）
//   0x528 0xFF00-0xFFFF，2 字节地址式（256B）

RAM uint8_t nfc_probe_done = 0;
RAM uint8_t nfc_probe_ack = 0;

RAM uint8_t nfc_log_armed = 0;      // 抓取器已布防
RAM uint8_t nfc_log_next_page = 0;  // 下一快照页
RAM uint8_t nfc_log_wraps = 0;      // 环形回卷代数（v18，记 meta[5]）
RAM uint8_t nfc_hb_retn_mark = 0;   // 心跳基准（retention 唤醒数快照，v18）
RAM uint8_t nfc_div_wraps = 0;      // nfc_log_div 回卷计数（v19：×4 节流心跳）
RAM uint8_t nfc_log_seq = 0;
RAM uint16_t nfc_log_div = 0;       // 轮询分频（会话期约每 128 圈，空闲约每 1024 圈）
RAM uint8_t nfc_last_irq = 0xFF;
RAM uint8_t nfc_regs[13];           // 0xFFF1-0xFFFD 寄存器镜像
RAM uint8_t nfc_last_regs[13];      // 上次快照的寄存器镜像（变化检测）
RAM uint8_t nfc_last_allff = 0;     // 芯片当前处于"接触侧卡死"态
RAM uint8_t nfc_wedge_cnt = 0;      // 累计卡死次数（记入快照 [7]）
RAM uint8_t nfc_recover_cnt = 0;    // 累计自愈成功次数
RAM uint8_t nfc_retn_wakes = 0;     // deep retention 唤醒计数（app.c 喂）
RAM uint8_t nfc_tr_flushed_idx = 0; // 上次轨迹转储到的位置（防重复转储）
RAM uint8_t nfc_session = 0;        // RF 会话进行中（总线保持挂载、快速采样）
RAM uint8_t nfc_sess_quiet = 0;     // 会话尾连续安静采样数
RAM uint16_t nfc_sess_samples = 0;  // 会话期采样数（硬顶防挂死）
RAM uint8_t nfc_sess_ff = 0;        // 会话期连续全 FF 采样数（真死判定，触发单次引脚重建）
RAM uint8_t nfc_attached = 0;       // 总线当前挂载态（空闲期每次轮询挂载/卸载，会话期 steady）
RAM uint8_t nfc_tx_disabled = 0;    // v40：TX 全链路禁用开关（§11.5 对照：判 TxEn 空帧微扰）
RAM uint8_t nfc_mute_hold = 0;      // v41：静默实验——CSN 常低 + 完全零 I²C 流量（判芯片在
                                    // 接触侧上电且静默时是否自应答 T4T；0.4 抠电池实验的
                                    // 混淆变量 CSN 悬空由此排除）
#if NFC_MAOTEK_MODE
// v28 消抖（自扰对策）：IRQ 低持续时长跟踪——接收中途的接触侧读会经仲裁复位非接
// 收（v27 实测：RxStart 亮而 RxDone 永不出现），IRQ 低后等帧收完再碰总线。
RAM uint8_t nfc_low_tracking = 0;
RAM uint32_t nfc_low_since = 0;
RAM uint32_t nfc_last_serve = 0; // v33c：最近服务时刻（IRQ 高且静默 >1s 才释放总线）
RAM uint32_t nfc_attach_time = 0; // v33d：本次挂载时刻（>5s 强制释放破 IRQ 钉低死锁）
#define NFC_DEBOUNCE_TICKS (8u * 16000u) // 8ms（TLSR8359 系统定时器固定 16MHz）
#define NFC_IDLE_RELEASE_TICKS (1000u * 16000u) // v33c：空闲 1s 释放接触侧
#define NFC_ATTACH_CAP_TICKS (5000u * 16000u) // v33d：单次挂载 5s 上限
// v30 盲应答状态机（TX 通路裁决）：RX 转发已被门控（v29 定案），但芯片确实收到了
// I-block（RxDone 亮），且 TX（FIFO 写 + TxEn）实证可达手机（v30：手机连发 23 帧
// 消费应答，App 稳定连卡）。NFC 读 NDEF 的帧序列是确定性的，可盲回：
// 0=SELECT NDEF 应用 1=SELECT CC(E103) 2=READ CC 3=SELECT 文件(E104)
// 4=READ NLEN 5=READ 数据 ≥6=兜底回完整 NDEF 文件（偏移不敏感）。
// v31 抗跑飞：帧间隔 >150ms = 重传（重发上一应答，不改 PCB）；>1.5s 无帧或
// Active 事件 = 状态机重置；日志节流防回卷（状态变化/会话首 4 帧/每 32 帧）。
RAM uint8_t nfc_blind_state = 0;
RAM uint8_t nfc_blind_pcb = 0x02; // I-block PCB 交替（首块 0x02）
RAM uint32_t nfc_blind_last_tx = 0; // 上一应答时刻（重传判定）
RAM uint16_t nfc_blind_frames = 0;  // 会话内帧计数（日志节流）
RAM uint8_t nfc_blind_last_state = 0xFF;
// v36 盲态机时间线（BSM）：RX 门控下看不见帧内容，64 页环形日志在 TagInfo 整场
// 扫描（数秒）里回卷 8-14 次，NDEF 探测的关键帧全被冲掉（v35 实证：14 页全在
// 同一秒内，状态机轨迹不可见）。改为把每次服务压成 3B 事件——(state,pcb,rlen)，
// 外加重激活标记 0xFE / >1.5s 间隔标记 0xFD / 重传标记 0xFC——缓存在 epd_temp
// 0x200 区，满 42 事件整页落到 0x46000 独立区（8 扇区 32 页，共 1344 事件容量，
// 足够容纳整场扫描的完整时间线）。会话收尾（间隔重置）时强制落页防丢尾。
#define NFC_BSM_ADDR 0x46000
#define NFC_BSM_SECTORS 8
#define NFC_BSM_PAGES (NFC_BSM_SECTORS * 16)
#define NFC_BSM_MAX 42 // 每页事件数（3B/事件，8B 页头）
#define NFC_BSM_EV_OFF 0x200  // epd_temp 内事件缓冲基址（42*3=126B）
#define NFC_BSM_PG_OFF 0x300  // epd_temp 内落页组装区（避开 capture 的 [0..255]）
RAM uint8_t nfc_bsm_n = 0;
RAM uint8_t nfc_bsm_page = 0;
RAM uint16_t nfc_bsm_total = 0;
#endif

// 楔死前历史轨迹环：每 512 圈采一次（时钟原始 tick + IRQ 电平 + MAIN_IRQ/FIFO计数/状态）
#define NFC_TRACE_N 24
RAM uint32_t nfc_tr_tick[NFC_TRACE_N];
RAM uint8_t nfc_tr_irq[NFC_TRACE_N];
RAM uint8_t nfc_tr_mirq[NFC_TRACE_N];
RAM uint8_t nfc_tr_cnt[NFC_TRACE_N];
RAM uint8_t nfc_tr_st[NFC_TRACE_N];
RAM uint8_t nfc_tr_idx = 0;

extern uint8_t epd_temp[];        // epd_buffer_size = 5000B，借用安全
extern uint8_t epd_update_state;  // 1 = EPD 刷新进行中（epd_temp 被占用）

// 有界 I²C 原语与应答器的前向声明（定义顺序：原语在前、应答器在后）
static int nfc_i2c_read_mem(uint16_t mem_addr, uint8_t *dst, int len);
static int nfc_i2c_write_mem2(uint16_t mem_addr, const uint8_t *data, int len);
static int nfc_i2c_cmd_read(uint8_t cmd, uint8_t *dst, int len);
static int nfc_i2c_cmd_write(uint8_t cmd, const uint8_t *data, int len);
static void nfc_t4t_respond(uint8_t cnt);

// v18：T4T 应答器整体退役。p14 日志实锤：本芯片出厂配置为 tag 自应答（手机 SELECT
// 由芯片内部从 EEPROM 应答，接触侧 FIFO 永远拿不到 I-block，MainIrq 只留面包屑）。
// MCU 通道应答需先改 EEPROM 仲裁/模式配置——路径已通（见写解锁矩阵），拿到写权限后
// 再按复旦微 demo 的帧格式复活（FIFO 帧含 PCB 首字节、应答回显 PCB：status_ok[0]=rx[0]）。
//
// v20 修正定性 + 应答器复活：v19 无 suspend 采样 7 场会话实锤——芯片既不自应答也不
// 转发（RxDone 亮、FifoIrq=空、FIFO 恒 0），出厂仲裁 FFE2=00 把接触侧锁在数据通路外。
// USER_CFG 镜像的 EEPROM 真身 = 尾块（0x3FA=c2↔FFE1、0x3FB=03↔FFE0，推 FFE2↔0x3F9），
// 经 BLE 控制台写 0x3F9 + 软复位翻转仲裁后，I-block 进 FIFO，本应答器即刻生效。
// 帧格式（复旦微 demo FM11T4T 同款）：rx=[PCB][CLA][INS][P1][P2][LC][data][CRC2]，
// 应答=[PCB 回显][payload][90 00]，SELECT 按 DF 名(P1=04)/文件 ID(P1=00,E103/E104)。

#define T4T_RX_OFF 256 // epd_temp 会话期交换记录区（[0]=cnt [1..32]=rx [33]=resp_len [34..75]=resp）
static const uint8_t t4t_cc[15] = {
    0x00, 0x0F, 0x20, 0x00, 0x3B, 0x00, 0x34, 0x04, 0x06, 0xE1, 0x04, 0x03, 0x84, 0x00, 0x00};
    // v32：MLe/MLc 从 0x00FF 改 0x003B/0x0034（maotek 同硬件实证值；Hi3861 注释：
    // 0xFF 会触发部分手机分帧/链式，0x3B/0x34 不会）
static const uint8_t t4t_ndef_rom[20] = {
    0x00, 0x12, 0xD1, 0x01, 0x0E, 0x54, 0x02, 0x65, 0x6E,
    'n', 'o', 'w', 'a', '1', '5', '4', '-', 'N', 'F', 'C'};
RAM uint8_t t4t_ndef[20]; // UPDATE BINARY 的 RAM 副本（UPDATE 不落 EEPROM）
RAM uint8_t t4t_file = 0; // 0=未选 1=CC(E103) 2=NDEF 文件(E104)
RAM uint8_t nfc_t4t_loglen = 0;
RAM uint8_t nfc_cnt_cmd = 0xFF; // 最近一次命令态 FifoCnt（快照 p[133]，v21）

#if NFC_MAOTEK_MODE
// —— v27 maotek 移植：发送与应答（I²C 全 2 字节编址）——
static void nfc_maotek_send(const uint8_t *resp, uint8_t rlen)
{
    if (nfc_tx_disabled)
        return; // v40：盲态机/BSM 照跑，仅不发（对照手机侧行为是否随之变）
    if (rlen)
        nfc_i2c_write_mem2(0xFFF0, resp, rlen); // FIFO PUSH（maotek fm11_send 同款）
    uint8_t txen = 0x55;
    nfc_i2c_write_mem2(0xFFF4, &txen, 1); // TxEn 触发非接触口回发
    for (int i = 0; i < 40; i++)          // 等 FIFO 排空（solokeys 082C：TX 水位 8）
    {
        uint8_t c = 0;
        if (nfc_i2c_read_mem(0xFFF2, &c, 1) == 0 && (uint8_t)(c & 0x3F) < 8)
            break;
        sleep_us(200);
    }
}

// FIFO 突发 POP → 去尾 2B RF CRC → T4T 状态机解析 → 回发。
// 解析逻辑与 nfc_t4t_respond 逐分支相同，仅运输层从 1 字节通道换成 2 字节编址。
static void nfc_maotek_respond(uint8_t cnt)
{
    uint8_t *rec = &epd_temp[T4T_RX_OFF];
    uint8_t *rx = &rec[1];
    if (cnt > 32)
        cnt = 32; // 突发 POP 记录上限（>32 残留下轮再弹；实验关心的 I-block 均 <32B）
    nfc_i2c_read_mem(0xFFF0, rx, cnt);
    rec[0] = cnt;
    uint8_t *resp = &rec[34];
    uint8_t rlen = 0;

    if (cnt > 2)
    {
        uint8_t plen = cnt - 2;                  // 尾 2 字节 = RF CRC（R1/R4/R5 一致）
        uint8_t pcb = rx[0];                     // I-block PCB（02/03 交替），应答回显
        uint8_t ins = (plen >= 3) ? rx[2] : 0x00; // rx=[PCB][CLA][INS][P1][P2][LC][data]
        uint8_t p1 = (plen >= 4) ? rx[3] : 0x00;
        resp[0] = pcb;
        rlen = 1;
        if (ins == 0xA4)
        { // SELECT
            uint8_t lc = (plen >= 6) ? rx[5] : 0;
            if (p1 == 0x04)
            { // by DF name（NDEF 应用 D2760000850101，FCI 空即 9000）
                t4t_file = 0;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else if (p1 == 0x00 && lc == 2 && rx[6] == 0xE1 && rx[7] == 0x03)
            {
                t4t_file = 1;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else if (p1 == 0x00 && lc == 2 && rx[6] == 0xE1 && rx[7] == 0x04)
            {
                t4t_file = 2;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else
            {
                t4t_file = 0;
                resp[rlen++] = 0x6A;
                resp[rlen++] = 0x82;
            }
        }
        else if (ins == 0xB0 && t4t_file)
        { // READ BINARY（P1P2=偏移，LC=Le）
            uint16_t off = ((uint16_t)((plen >= 4) ? rx[3] : 0) << 8) | ((plen >= 5) ? rx[4] : 0);
            uint8_t le = (plen >= 6) ? rx[5] : 0;
            const uint8_t *f = (t4t_file == 1) ? t4t_cc : t4t_ndef;
            uint16_t flen = (t4t_file == 1) ? sizeof(t4t_cc) : sizeof(t4t_ndef);
            if (off < flen)
            {
                if (le == 0 || le > flen - off)
                    le = flen - off;
                memcpy(&resp[rlen], &f[off], le);
                rlen += le;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else
            {
                resp[rlen++] = 0x6A;
                resp[rlen++] = 0x82;
            }
        }
        else if (ins == 0xD6 && t4t_file == 2)
        { // UPDATE BINARY（RAM 副本，不落 EEPROM）
            uint16_t off = ((uint16_t)((plen >= 4) ? rx[3] : 0) << 8) | ((plen >= 5) ? rx[4] : 0);
            uint8_t lc = (plen >= 6) ? rx[5] : 0;
            if (off < sizeof(t4t_ndef) && off + lc <= sizeof(t4t_ndef) && plen >= 6 + lc)
                memcpy(&t4t_ndef[off], &rx[6], lc);
            resp[rlen++] = 0x90;
            resp[rlen++] = 0x00;
        }
        else
        {
            resp[rlen++] = 0x6D;
            resp[rlen++] = 0x00;
        }
    }

    nfc_maotek_send(resp, rlen);
    rec[33] = rlen;
    nfc_t4t_loglen = (34 + rlen) > 76 ? 76 : (uint8_t)(34 + rlen);
}
#endif // NFC_MAOTEK_MODE

#if !NFC_MAOTEK_MODE
// FIFO 弹出→解析→应答（会话期 cnt>2 时调用）。总线已挂载态。
static void nfc_t4t_respond(uint8_t cnt)
{
    uint8_t *rec = &epd_temp[T4T_RX_OFF];
    uint8_t *rx = &rec[1];
    if (cnt > 32)
        cnt = 32;
    nfc_i2c_cmd_read(0x00, rx, cnt); // FIFO 弹出：reg0 + 单事务自增（1 字节通道，v25）
    rec[0] = cnt;
    uint8_t *resp = &rec[34];
    uint8_t rlen = 0;

    if (cnt > 2)
    {
        uint8_t plen = cnt - 2;
        uint8_t pcb = rx[0];                      // I-block PCB（02/03 交替），应答回显
        uint8_t ins = (plen >= 3) ? rx[2] : 0x00; // rx=[PCB][CLA][INS][P1][P2][LC][data]
        uint8_t p1 = (plen >= 4) ? rx[3] : 0x00;
        resp[0] = pcb;
        rlen = 1;
        if (ins == 0xA4)
        { // SELECT
            uint8_t lc = (plen >= 6) ? rx[5] : 0;
            if (p1 == 0x04)
            { // by DF name（NDEF 应用 D2760000850101，FCI 空即 9000）
                t4t_file = 0;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else if (p1 == 0x00 && lc == 2 && rx[6] == 0xE1 && rx[7] == 0x03)
            {
                t4t_file = 1;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else if (p1 == 0x00 && lc == 2 && rx[6] == 0xE1 && rx[7] == 0x04)
            {
                t4t_file = 2;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else
            {
                t4t_file = 0;
                resp[rlen++] = 0x6A;
                resp[rlen++] = 0x82;
            }
        }
        else if (ins == 0xB0 && t4t_file)
        { // READ BINARY（P1P2=偏移，LC=Le）
            uint16_t off = ((uint16_t)((plen >= 4) ? rx[3] : 0) << 8) | ((plen >= 5) ? rx[4] : 0);
            uint8_t le = (plen >= 6) ? rx[5] : 0;
            const uint8_t *f = (t4t_file == 1) ? t4t_cc : t4t_ndef;
            uint16_t flen = (t4t_file == 1) ? sizeof(t4t_cc) : sizeof(t4t_ndef);
            if (off < flen)
            {
                if (le == 0 || le > flen - off)
                    le = flen - off;
                memcpy(&resp[rlen], &f[off], le);
                rlen += le;
                resp[rlen++] = 0x90;
                resp[rlen++] = 0x00;
            }
            else
            {
                resp[rlen++] = 0x6A;
                resp[rlen++] = 0x82;
            }
        }
        else if (ins == 0xD6 && t4t_file == 2)
        { // UPDATE BINARY（RAM 副本，不落 EEPROM）
            uint16_t off = ((uint16_t)((plen >= 4) ? rx[3] : 0) << 8) | ((plen >= 5) ? rx[4] : 0);
            uint8_t lc = (plen >= 6) ? rx[5] : 0;
            if (off < sizeof(t4t_ndef) && off + lc <= sizeof(t4t_ndef) && plen >= 6 + lc)
                memcpy(&t4t_ndef[off], &rx[6], lc);
            resp[rlen++] = 0x90;
            resp[rlen++] = 0x00;
        }
        else
        {
            resp[rlen++] = 0x6D;
            resp[rlen++] = 0x00;
        }
    }

    nfc_i2c_cmd_write(0x00, resp, rlen); // FIFO 写：reg0 自增连发（1 字节通道，v25）
    uint8_t txen = 0x55;
    nfc_i2c_cmd_write(4, &txen, 1); // reg4=TxEn 触发非接触口发送
    for (int i = 0; i < 40; i++)
    {
        uint8_t c = 0;
        nfc_i2c_read_mem(0xFFF2, &c, 1);
        if (c < 8)
            break;
        sleep_us(200);
    }
    rec[33] = rlen;
    nfc_t4t_loglen = (34 + rlen) > 76 ? 76 : (uint8_t)(34 + rlen);
}
#endif // !NFC_MAOTEK_MODE

// 全地址 ACK 扫描（启动探测用，retention 生效前总线健康，可用 SDK 无界原语）
static void nfc_scan_bus(uint8_t *bitmap16)
{
    memset(bitmap16, 0, 16);
    for (uint8_t a = 0; a < 0x7F; a++)
    {
        if (test_i2c_device(a))
            bitmap16[a >> 3] |= 0x80 >> (a & 7);
    }
}

// AddrLen: 2 = 2 字节大端地址式（真机实证成立），1 = 1 字节地址式（对照）
static void nfc_dump(uint16_t mem_addr, uint8_t addr_len, uint8_t *dst, int len)
{
    while (len > 0)
    {
        i2c_set_id(NFC_I2C_ID_WR);
        i2c_read_series(mem_addr, addr_len, dst, 32);
        dst += 32;
        mem_addr += 32;
        len -= 32;
    }
}

// ---------------------------------------------------------------------------
// 有界 I2C 原语（SDK 系列函数是无界 busy-wait，卡死总线上会冻结 main_loop）
// ---------------------------------------------------------------------------

static int nfc_i2c_wait(void) // 0=完成，-1=超时（总线被从机拖死）
{
    uint32_t tmo = NFC_I2C_TMO;
    while (reg_i2c_status & FLD_I2C_CMD_BUSY)
    {
        if (--tmo == 0)
            return -1;
    }
    return 0;
}

static void nfc_i2c_reinit(void) // 重建外设与引脚（超时后必做）
{
    i2c_gpio_set(I2C_GPIO_GROUP_C0C1);
    i2c_master_init(NFC_I2C_ID_WR, (uint8_t)(CLOCK_SYS_CLOCK_HZ / (4 * 400000)));
}

// 2 字节大端地址读 N 字节；0=ok，-1=超时（已重建外设）
static int nfc_i2c_read_mem(uint16_t mem_addr, uint8_t *dst, int len)
{
    if (len <= 0)
        return 0;
    i2c_set_id(NFC_I2C_ID_WR);
    reg_i2c_adr = (uint8_t)(mem_addr >> 8);
    reg_i2c_do = (uint8_t)mem_addr;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_ADDR | FLD_I2C_CMD_DO | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    reg_i2c_id |= FLD_I2C_WRITE_READ_BIT;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    int i = 0;
    for (; i < len - 1; i++)
    {
        reg_i2c_ctrl = FLD_I2C_CMD_DI | FLD_I2C_CMD_READ_ID;
        if (nfc_i2c_wait())
            goto wedged;
        dst[i] = reg_i2c_di;
    }
    reg_i2c_ctrl = FLD_I2C_CMD_DI | FLD_I2C_CMD_READ_ID | FLD_I2C_CMD_ACK;
    if (nfc_i2c_wait())
        goto wedged;
    dst[i] = reg_i2c_di;
    reg_i2c_ctrl = FLD_I2C_CMD_STOP;
    if (nfc_i2c_wait())
        goto wedged;
    return 0;
wedged:
    nfc_i2c_reinit();
    return -1;
}

// 2 字节大端地址写 N 字节（复位序列 = write_mem2(0x03B5, {0xA0}, 1)，线上等价原序列）；0=ok
static int nfc_i2c_write_mem2(uint16_t mem_addr, const uint8_t *data, int len){
    i2c_set_id(NFC_I2C_ID_WR);
    reg_i2c_adr = (uint8_t)(mem_addr >> 8);
    reg_i2c_do = (uint8_t)mem_addr;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_ADDR | FLD_I2C_CMD_DO | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    for (int i = 0; i < len; i++)
    {
        reg_i2c_di = data[i];
        reg_i2c_ctrl = FLD_I2C_CMD_DI;
        if (nfc_i2c_wait())
            goto wedged;
    }
    reg_i2c_ctrl = FLD_I2C_CMD_STOP;
    if (nfc_i2c_wait())
        goto wedged;
    return 0;
wedged:
    nfc_i2c_reinit();
    return -1;
}

// 命令态访问（v21）：1 字节"地址"在 gen-1 I²C 上实际走 SPI 命令译码——毒理取证实锤
// （{0x03,…} = 写寄存器 3 命令 + 数据自增），与 solokeys SPI 版命令格式逐位同源：
// 寄存器读 = 0x20|reg、FIFO 读 = 0xA0、FIFO 写 = 0x80+数据。内存映射 0xFFF0+ 窗可能是
// 有限镜像（FifoCnt 恒 0 的疑点之一），双路并行采样以辨真身。
static int nfc_i2c_cmd_read(uint8_t cmd, uint8_t *dst, int len)
{
    if (len <= 0)
        return 0;
    i2c_set_id(NFC_I2C_ID_WR);
    reg_i2c_adr = cmd;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_ADDR | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    reg_i2c_id |= FLD_I2C_WRITE_READ_BIT;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    int i = 0;
    for (; i < len - 1; i++)
    {
        reg_i2c_ctrl = FLD_I2C_CMD_DI | FLD_I2C_CMD_READ_ID;
        if (nfc_i2c_wait())
            goto wedged;
        dst[i] = reg_i2c_di;
    }
    reg_i2c_ctrl = FLD_I2C_CMD_DI | FLD_I2C_CMD_READ_ID | FLD_I2C_CMD_ACK;
    if (nfc_i2c_wait())
        goto wedged;
    dst[i] = reg_i2c_di;
    reg_i2c_ctrl = FLD_I2C_CMD_STOP;
    if (nfc_i2c_wait())
        goto wedged;
    return 0;
wedged:
    nfc_i2c_reinit();
    return -1;
}

// 命令 + 单帧连发数据（FIFO 写 [0x80, resp...]，solokeys WriteFifo 同构）
static int nfc_i2c_cmd_write(uint8_t cmd, const uint8_t *data, int len)
{
    i2c_set_id(NFC_I2C_ID_WR);
    reg_i2c_adr = cmd;
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_ADDR | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    for (int i = 0; i < len; i++)
    {
        reg_i2c_di = data[i];
        reg_i2c_ctrl = FLD_I2C_CMD_DI;
        if (nfc_i2c_wait())
            goto wedged;
    }
    reg_i2c_ctrl = FLD_I2C_CMD_STOP;
    if (nfc_i2c_wait())
        goto wedged;
    return 0;
wedged:
    nfc_i2c_reinit();
    return -1;
}

// ---------------------------------------------------------------------------
// 总线挂载/卸载纪律（轮询外总线上绝无事务，retention 切换无从卡死从机）
// ---------------------------------------------------------------------------

// 引脚重建（不含 settle）：retention 唤醒会抹掉数字引脚配置，任何时刻访问前都要重跑
static void nfc_pins_rearm(void)
{
    gpio_set_func(NFC_CS, AS_GPIO);
    gpio_set_output_en(NFC_CS, 1);
    gpio_set_input_en(NFC_CS, 0);
    gpio_setup_up_down_resistor(NFC_CS, PM_PIN_PULLUP_10K);
    gpio_write(NFC_CS, 0); // 接触侧上电
    gpio_set_func(NFC_IRQ, AS_GPIO);
    gpio_set_output_en(NFC_IRQ, 0);
    gpio_set_input_en(NFC_IRQ, 1);
    gpio_setup_up_down_resistor(NFC_IRQ, PM_PIN_PULLUP_10K);
    i2c_gpio_set(I2C_GPIO_GROUP_C0C1);
    i2c_master_init(NFC_I2C_ID_WR, (uint8_t)(CLOCK_SYS_CLOCK_HZ / (4 * 400000)));
}

static void nfc_bus_attach(void)
{
    nfc_pins_rearm();
#if NFC_PERMANENT_ATTACH
    sleep_ms(2);
#endif
    sleep_ms(2); // CSN 上电建立（demo 要求 250us，保守 2ms）
    nfc_attached = 1;
    nfc_attach_time = clock_time(); // v33d：挂载时长基准（防 IRQ 钉低死锁）
}

static void nfc_bus_detach(void)
{
    nfc_attached = 0;
#if NFC_PERMANENT_ATTACH
    nfc_pins_rearm(); // 常上电模式：不落闸，仅把 retention 抹掉的引脚配置补回来
#else
    gpio_write(NFC_CS, 1); // 接触侧断电
    gpio_set_func(NFC_SDA, AS_GPIO);
    gpio_set_func(NFC_SCL, AS_GPIO);
    gpio_set_output_en(NFC_SDA, 0);
    gpio_set_input_en(NFC_SDA, 1);
    gpio_set_output_en(NFC_SCL, 0);
    gpio_set_input_en(NFC_SCL, 1);
    gpio_setup_up_down_resistor(NFC_SDA, PM_PIN_PULLUP_10K);
    gpio_setup_up_down_resistor(NFC_SCL, PM_PIN_PULLUP_10K);
#endif
}

// GPIO 手动 9 时钟解卡：从机若扣着半字节等时钟，可被放出（经典 I2C unwedge）
static void nfc_i2c_unstick(void)
{
    gpio_set_func(NFC_SDA, AS_GPIO);
    gpio_set_func(NFC_SCL, AS_GPIO);
    gpio_set_output_en(NFC_SDA, 1);
    gpio_set_input_en(NFC_SDA, 0);
    gpio_set_output_en(NFC_SCL, 1);
    gpio_set_input_en(NFC_SCL, 0);
    gpio_write(NFC_SDA, 1);
    for (int i = 0; i < 9; i++)
    {
        gpio_write(NFC_SCL, 0);
        sleep_us(5);
        gpio_write(NFC_SCL, 1);
        sleep_us(5);
    }
    // STOP：SCL 高时 SDA 低→高
    gpio_write(NFC_SDA, 0);
    sleep_us(5);
    gpio_write(NFC_SCL, 1);
    sleep_us(5);
    gpio_write(NFC_SDA, 1);
    sleep_us(5);
}

// ---------------------------------------------------------------------------
// 寄存器与日志
// ---------------------------------------------------------------------------

// 逐字节安全读 0xFFF1-0xFFFD（避开 0xFFF0 FIFO 口）；0=健康，1=卡死（全 0xFF 或超时
// 或全 0x00——后者是 SDA 卡低电平态，v22 实测：开机即全零 + IRQ 抖动 + 每页 171ms
// 的记录风暴，旧检测只认全 FF 完全漏掉）
static uint8_t nfc_read_regs(void)
{
    for (int i = 0; i < 13; i++)
    {
        if (nfc_i2c_read_mem(0xFFF1 + i, &nfc_regs[i], 1) != 0)
        {
            memset(nfc_regs, 0xFF, sizeof(nfc_regs));
            return 1;
        }
    }
    uint8_t nz = 0;
    uint8_t ff = 0;
    for (int i = 0; i < 13; i++)
    {
        if (nfc_regs[i] != 0x00)
            nz = 1;
        if (nfc_regs[i] == 0xFF)
            ff++;
    }
    if (!nz) // 全零：健康态至少 Status=04/NfcCfg=01 非零
        return 1;
    if (ff >= 12) // 全 FF
        return 1;
    return 0;
}

// IRQ 路由布防（v24.1，solokeys 配方逐字）：MainIrqMask(0xFFFA) ← **0xE5**
// （位=1 为屏蔽！0xE5 = 只放行 RxDone|TxDone|Fifo）。绝不放行 Active/RxStart/
// RF_PWON：接收中途轮询会经接触优先仲裁复位射频接收。掩码为活寄存器，场撤/CSN
// 电源周期后回出厂 0x42，故只在"空闲且 IRQ 高"时补布，布完同步 nfc_last_irq
// 吞掉写入本身引起的 IRQ 跳变（v24 教训：每次边沿后重布 → 自激振荡 3.7 页/秒）。
static void nfc_arm_irq_routing(void)
{
    // v25 根因修复：0xFFF0-0xFFFD 活寄存器窗在 2 字节寻址下只读（写一律 NAK，
    // v24.2 实测），寄存器写唯一有效路径 = 1 字节寄存器文件寻址（毒理事件实锤的
    // 通道，solokeys SPI 命令同构）：reg10=MainIrqMask reg11=FifoIrqMask。
    uint8_t m = 0xE5;                  // MainIrqMask：位=1 屏蔽；只放行 RxDone|TxDone|Fifo
    nfc_i2c_cmd_write(10, &m, 1);
    uint8_t f = 0xF5;                  // FifoIrqMask：屏 Empty/Overflow，放行 Full/WL
    nfc_i2c_cmd_write(11, &f, 1);      // （出厂 0x00 一个不屏 → FIFO-Empty 常真 → IRQ 常低）
    nfc_last_irq = gpio_read(NFC_IRQ) ? 1 : 0; // 吞掉掩码写入的 IRQ 伪沿
}

// v33c：场判定（控制台门槛）——RfStatus(0xFFF3) 高半字节 = NFC_STATE。v33e 收窄：
// 只把 ISO-DEP 会话态（8=RATS 前 /9/A=PPS 后 /C/D=认证态）当"场中"——环境射频
// 噪声会把状态机打进 READY/HALT（v32 日志 AuxIrq CRC 错误页为证），不能误判。
static uint8_t nfc_field_active(void)
{
    return 0; // v33f：门槛退役——RfStatus 状态机会因环境噪声/盲发 TX 驻留在 8/4 等
              // 合法非 IDLE 态，状态判定连续误报锁死控制台。改由操作者保证无场再写。
}

// 日志环形回卷（v18）：64 页写满即整区擦除重写 meta（旧数据放弃），wrap 代数记 meta[5]。
// v17 教训：写满即永久停写，一晚就把 armed 状态变瞎。
static void nfc_log_wrap(void)
{
    uint8_t *m = epd_temp;
    uint32_t t = get_time();
    for (int s = 0; s < NFC_LOG_SECTORS; s++)
        flash_erase_sector(NFC_LOG_ADDR + s * 0x1000);
    memset(m, 0, 256);
    m[0] = 'N';
    m[1] = 'F';
    m[2] = 'C';
    m[3] = 'L';
    m[4] = 8;
    m[5] = ++nfc_log_wraps;
    m[6] = (uint8_t)t;
    m[7] = (uint8_t)(t >> 8);
    flash_write_page(NFC_LOG_ADDR, 256, m);
    nfc_log_next_page = 1;
    nfc_log_seq = 0;
}

// 快照落 flash 一页；nfc_regs 须已就绪，FIFO 计数 >0 时顺带把 FIFO 读出来
static void nfc_log_capture(uint8_t flags)
{
    if (!nfc_log_armed)
        return;
    if (epd_update_state)
        return; // 屏幕刷新占用 epd_temp，跳过本轮（回卷一并推迟）
    if (nfc_log_next_page >= NFC_LOG_PAGES)
        nfc_log_wrap(); // v18：回卷续写而非停写

    uint8_t *p = epd_temp;
    uint32_t t = get_time();
    uint32_t tick = clock_time();
    memset(p, 0, 256);
    p[0] = (uint8_t)t;
    p[1] = (uint8_t)(t >> 8);
    p[2] = (uint8_t)(t >> 16);
    p[3] = (uint8_t)(t >> 24);
    p[4] = flags;
    p[5] = nfc_log_seq++;
    p[6] = gpio_read(NFC_IRQ) ? 1 : 0;
    p[7] = nfc_wedge_cnt;
    memcpy(&p[8], nfc_regs, 13);
    p[21] = nfc_regs[1]; // 0xFFF2 = FIFO 字计数
    p[54] = nfc_retn_wakes;
    p[55] = (uint8_t)tick;
    p[56] = (uint8_t)(tick >> 8);
    p[57] = (uint8_t)(tick >> 16);
    p[58] = (uint8_t)(tick >> 24);
#if !NFC_MAOTEK_MODE
    uint8_t cnt = nfc_regs[1];
    if (cnt && cnt != 0xFF)
    {
        if (cnt > 32)
            cnt = 32;
        nfc_i2c_cmd_read(0x00, &p[22], cnt); // FIFO 弹出（reg0 1 字节通道自增）
    }
#endif // v27：FIFO 已由应答器弹出；1 字节通道按 P2 实为 EEPROM 越界读，禁用
    // T4T 交换记录（会话路径）：p[59]=cnt p[60..91]=rx p[92]=resp_len p[93..132]=resp
    if (nfc_t4t_loglen)
    {
        memcpy(&p[59], &epd_temp[T4T_RX_OFF], nfc_t4t_loglen);
        nfc_t4t_loglen = 0;
    }
    p[133] = nfc_cnt_cmd; // 命令态 FifoCnt（v21 双路对比）
    flash_write_page(NFC_LOG_ADDR + nfc_log_next_page * 256, 256, p);
    nfc_log_next_page++;
#if !NFC_MAOTEK_MODE
    nfc_arm_irq_routing(); // 场撤后掩码会复位，快照后重布防
#endif                      // v27：零掩码（maotek 保真，单变量纪律）
}

#if NFC_MAOTEK_MODE
// v36 盲态机时间线：事件入 RAM 环，满页/会话尾落 flash 0x46000
static void nfc_bsm_flush(void)
{
    if (!nfc_bsm_n || !nfc_log_armed || epd_update_state)
        return;
    if (nfc_bsm_page >= NFC_BSM_PAGES)
    {
        for (int s = 0; s < NFC_BSM_SECTORS; s++)
            flash_erase_sector(NFC_BSM_ADDR + s * 0x1000);
        nfc_bsm_page = 0;
    }
    uint8_t *pg = &epd_temp[NFC_BSM_PG_OFF];
    memset(pg, 0, 256);
    pg[0] = 'B';
    pg[1] = 'S';
    pg[2] = nfc_bsm_n;
    pg[3] = nfc_bsm_page;
    pg[4] = (uint8_t)nfc_bsm_total;
    pg[5] = (uint8_t)(nfc_bsm_total >> 8);
    memcpy(&pg[8], &epd_temp[NFC_BSM_EV_OFF], nfc_bsm_n * 3);
    flash_write_page(NFC_BSM_ADDR + nfc_bsm_page * 256, 256, pg);
    nfc_bsm_page++;
    nfc_bsm_n = 0;
}

static void nfc_bsm_append(uint8_t a, uint8_t b, uint8_t c)
{
    if (!nfc_log_armed)
        return;
    uint8_t *ev = &epd_temp[NFC_BSM_EV_OFF];
    ev[(uint16_t)nfc_bsm_n * 3] = a;
    ev[(uint16_t)nfc_bsm_n * 3 + 1] = b;
    ev[(uint16_t)nfc_bsm_n * 3 + 2] = c;
    nfc_bsm_n++;
    nfc_bsm_total++;
    if (nfc_bsm_n >= NFC_BSM_MAX)
        nfc_bsm_flush();
}
#endif

// 轨迹环转储一页：'TRC1' + 24 条 [tick4 irq1 mainirq1 fifocnt1 status1 pad2]
static void nfc_log_trace_flush(void)
{
    if (!nfc_log_armed || epd_update_state)
        return;
    if (nfc_log_next_page >= NFC_LOG_PAGES)
        nfc_log_wrap();
    if (nfc_tr_idx == nfc_tr_flushed_idx)
        return; // 环无新采样，防重复转储刷屏
    nfc_tr_flushed_idx = nfc_tr_idx;
    uint8_t *p = epd_temp;
    memset(p, 0, 256);
    uint32_t t = get_time();
    p[0] = (uint8_t)t;
    p[1] = (uint8_t)(t >> 8);
    p[2] = (uint8_t)(t >> 16);
    p[3] = (uint8_t)(t >> 24);
    p[4] = 'T';
    p[5] = 'R';
    p[6] = 'C';
    p[7] = '1';
    for (int i = 0; i < NFC_TRACE_N; i++)
    {
        uint8_t *e = &p[8 + i * 10];
        int j = (nfc_tr_flushed_idx + i) % NFC_TRACE_N; // 从最老到最新
        e[0] = (uint8_t)nfc_tr_tick[j];
        e[1] = (uint8_t)(nfc_tr_tick[j] >> 8);
        e[2] = (uint8_t)(nfc_tr_tick[j] >> 16);
        e[3] = (uint8_t)(nfc_tr_tick[j] >> 24);
        e[4] = nfc_tr_irq[j];
        e[5] = nfc_tr_mirq[j];
        e[6] = nfc_tr_cnt[j];
        e[7] = nfc_tr_st[j];
    }
    flash_write_page(NFC_LOG_ADDR + nfc_log_next_page * 256, 256, p);
    nfc_log_next_page++;
}

// 轨迹环采样（3 次有界字节读）
static void nfc_trace_sample(void)
{
    int i = nfc_tr_idx;
    nfc_i2c_read_mem(0xFFF7, &nfc_tr_mirq[i], 1);
    nfc_i2c_read_mem(0xFFF2, &nfc_tr_cnt[i], 1);
    nfc_i2c_read_mem(0xFFF3, &nfc_tr_st[i], 1);
    nfc_tr_tick[i] = clock_time();
    nfc_tr_irq[i] = gpio_read(NFC_IRQ) ? 1 : 0;
    nfc_tr_idx = (nfc_tr_idx + 1) % NFC_TRACE_N;
}

// 卡死恢复两级流水（总线已挂载态调用）；返回 1 = 恢复健康。
// ⚠️ 绝不发 {0x03,0xB5,0xA0}"复位序列"——低位地址写走 1 字节寄存器译码，
// 它 = NfcStatus←0xB5 + NfcTxEn←0xA0（TX 锁存，射频变聋且跨软重启存活）。
static uint8_t nfc_wedge_recover(void)
{
    nfc_i2c_unstick(); // 1) 手动 9 时钟
    gpio_write(NFC_CS, 1);
    sleep_ms(10);
    gpio_write(NFC_CS, 0); // 2) CSN 脉冲
    sleep_ms(2);
    nfc_i2c_reinit();
    if (nfc_read_regs() == 0)
    {
        nfc_recover_cnt++;
        memcpy(nfc_last_regs, nfc_regs, sizeof(nfc_regs));
        nfc_log_capture(0x40 | 0x01); // bit6 = 解卡/CSN 脉冲后恢复
        return 1;
    }
    return 0;
}

void nfc_note_retention_wake(void) { nfc_retn_wakes++; }

// app.c 用：RF 会话进行中（此期间 main_loop 禁 suspend，采样不失联）
uint8_t nfc_session_active(void) { return nfc_session; }

// main_loop 调用。空闲：挂载-轮询-卸载纪律；会话（MainIrq≠0 / IRQ 低）：总线保持
// 挂载、加密采样 FIFO——CSN 翻转会掐死手机的 ISO-DEP 会话（v7 教训），会话期不撒手。
void nfc_logger_poll(void)
{
    if (!nfc_log_armed)
        return;
    if (nfc_mute_hold)
        return; // v41 静默模式：零总线活动（CSN 由 mutemode 拉低后保持，logger 全面让路）

#if NFC_MAOTEK_MODE
    // v27 maotek nfc_handler 移植：IRQ 高 = 纯 GPIO 等待（零总线活动）；IRQ 低 =
    // 服务一轮：MAIN_IRQ 读清 → RxDone 弹 FIFO 应答 → 子寄存器读清 → 快照 → 释放。
    // nfc_session 布防后恒 1（app.c 禁 suspend）。
    if (epd_update_state)
        return; // 屏忙：epd_temp 被占用（本实验固件 EPD 已 hold，常态不会走到）
    // v28 消抖：IRQ 高 = 纯 GPIO 等待并复位消抖基准；IRQ 低 = 起表，持续 8ms
    // （一帧最长 ~20ms@106kbps 的主力场景是 ~7-20B I-block，1-2ms 即收完）后才碰
    // 总线服务。v27 教训：立即服务 = 接收中途读寄存器 = 仲裁打死每个 I-block。
    if (gpio_read(NFC_IRQ))
    {
        nfc_low_tracking = 0;
        return; // v34：总线逐轮已释放，无需静默释放逻辑
    }
    if (!nfc_low_tracking)
    {
        nfc_low_tracking = 1;
        nfc_low_since = clock_time();
        return;
    }
    if ((uint32_t)(clock_time() - nfc_low_since) < NFC_DEBOUNCE_TICKS)
        return;
    nfc_low_tracking = 0; // 服务条件成立；服务后下一轮重新消抖

    if (!nfc_attached) // v34：回到逐轮挂载/释放（v32 实证形态；stead-attach 是回归嫌疑）
    {
        nfc_bus_attach();
    }
    uint8_t mi = 0, cnt = 0;
    nfc_i2c_read_mem(0xFFF7, &mi, 1); // MAIN_IRQ（读清）
    uint32_t now = clock_time();
    if (mi & 0x40)
    {
        nfc_blind_state = 0; // Active：新会话，盲状态机归零
        nfc_blind_pcb = 0x02;
        nfc_blind_frames = 0;
        nfc_blind_last_state = 0xFF;
        nfc_bsm_append(0xFE, mi, 0); // v36：重激活标记
    }
    // v40 探针：本机 TX 从不进 FIFO（v39 fifotest 终审）——TxDone 亮只能来自芯片自身
    // 发射（ROM 人格对厂商命令自应答的检测器）；Arbit 亮 = 接触/非接仲裁现场。
    if (mi & 0x08)
        nfc_bsm_append(0xF3, mi, 0);
    if (mi & 0x04)
        nfc_bsm_append(0xF4, mi, 0);
    if (nfc_blind_last_tx && now - nfc_blind_last_tx > (uint32_t)(1500 * 16000u))
    {
        nfc_blind_state = 0; // >1.5s 无帧 = 新一轮碰触，重置
        nfc_blind_pcb = 0x02;
        nfc_blind_frames = 0;
        nfc_blind_last_state = 0xFF;
        nfc_bsm_append(0xFD, 0, 0); // v36：会话间隔标记
        nfc_bsm_flush();            // 会话收尾强制落页防丢尾
    }
    if (mi & 0x10)                    // RxDone
    {
        nfc_i2c_read_mem(0xFFF2, &cnt, 1);
        cnt &= 0x3F;
        if (cnt > 2)
            nfc_maotek_respond(cnt); // 突发 POP + 应答（内含尾 2B CRC 剥离）
        else
        {
            // v31 盲应答：RxDone 而 FIFO 空（转发被门控）。间隔 >150ms = 重传，
            // 原样重发上一应答（rec[33]=len rec[34..]=resp 已留存）；否则按状态机前进。
            // v33e：先验 RfStatus 会话态（≥8）——环境噪声也会触发 RxDone（AuxIrq CRC
            // 错误页为证），非会话态的 RxDone 不盲发 TX（避免状态机被噪声搅乱）。
            uint8_t sst = 0;
            nfc_i2c_read_mem(0xFFF3, &sst, 1);
            if ((sst & 0xF0) < 0x80)
            {
                nfc_i2c_read_mem(0xFFF0, &epd_temp[0x180], 8); // 噪声残留弹出丢弃
                nfc_regs[2] = sst;                              // 快照留痕状态值
            }
            else
            {
            uint8_t *rec = &epd_temp[T4T_RX_OFF];
            // v34 探针已判决（TagInfo 会话日志）：单字节读与突发读返回同一逐事件
            // 常量字节（da/0d/95/1c/45/5c/8c/1d，非帧 CRC/PCB/我方应答 CRC），且
            // FifoCnt 恒 0 —— RX 门控在 FIFO 上游，接触侧无从取帧，真·FIFO 应答
            // 死路。v35 起热路径不再碰空 FIFO 口（读空口疑似 FIFO underflow 副作用，
            // v34 带探针最长连发 3 帧 vs v30 无探针 23 帧）。
            uint8_t retry = nfc_blind_last_tx &&
                            now - nfc_blind_last_tx > (uint32_t)(150 * 16000u);
            uint8_t *resp = &rec[34];
            uint8_t rlen;
            if (retry && rec[33] && rec[33] != 0xFF)
            {
                rlen = rec[33]; // 重传：原样重发（同 PCB 同内容）
            }
            else
            {
                uint8_t st = nfc_blind_state;
                rlen = 0;
                resp[rlen++] = nfc_blind_pcb;
                nfc_blind_pcb ^= 0x01;
                if (st == 0 || st == 1 || st == 3)
                {
                    if (st == 0)
                    { // v38：SELECT NDEF 应用带 FCI 模板（真 T4T 形态：DF 名回显）
                        static const uint8_t fci[] = {0x6F, 0x09, 0x84, 0x07,
                                                      0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};
                        memcpy(&resp[rlen], fci, sizeof(fci));
                        rlen += sizeof(fci);
                    }
                    resp[rlen++] = 0x90; // SELECT → 9000
                    resp[rlen++] = 0x00;
                }
                else if (st == 2)
                {
                    memcpy(&resp[rlen], t4t_cc, sizeof(t4t_cc));
                    rlen += sizeof(t4t_cc); // READ CC
                    resp[rlen++] = 0x90;
                    resp[rlen++] = 0x00;
                }
                else if (st == 4)
                {
                    resp[rlen++] = t4t_ndef[0]; // READ NLEN → 回真 NLEN（v35 修复：
                    resp[rlen++] = t4t_ndef[1]; // 旧硬编码 0x000F 与 st5 数据 0x0012 矛盾，
                    resp[rlen++] = 0x90;        // Android 按 15 截断解析 18 字节记录必败）
                    resp[rlen++] = 0x00;
                }
                else if (st == 5)
                {
                    memcpy(&resp[rlen], &t4t_ndef[2], sizeof(t4t_ndef) - 2); // READ 数据
                    rlen += sizeof(t4t_ndef) - 2;
                    resp[rlen++] = 0x90;
                    resp[rlen++] = 0x00;
                }
                else
                {
                    // v37：序列外帧一律 6D 00（INS 不支持）。v36 BSM 时间线实证：
                    // 完整走穿 st0-5 后手机还会连问 2-3 帧（疑似分段续读/复检），
                    // 旧"兜底回完整文件"对偏移续读是错数据、对非 READ 是错 SW，
                    // 手机侧当场断链——TagInfo 两次 "scan interrupt" 与 st6-8 的
                    // 乱答逐帧对应，NDEF 判定因此从未写完。
                    resp[rlen++] = 0x6D;
                    resp[rlen++] = 0x00;
                }
                nfc_blind_state++;
                rec[0] = 0xFD;
                rec[25] = st;
            }
            nfc_blind_last_tx = now;
            nfc_blind_frames++;
            nfc_maotek_send(resp, rlen); // TX（FIFO 写 + TxEn，全 2 字节编址）
            rec[33] = rlen;
            nfc_bsm_append(retry ? 0xFC : rec[25], resp[0], rlen); // v36：时间线
            if (nfc_blind_state != nfc_blind_last_state || nfc_blind_frames <= 4 ||
                (nfc_blind_frames & 0x1F) == 0)
            { // 日志节流：防每帧一页把会话史刷没（v30 教训：回卷吞掉关键早期页）
                rec[0] = 0xFD;
                nfc_t4t_loglen = (34 + rlen) > 76 ? 76 : (uint8_t)(34 + rlen);
                nfc_blind_last_state = nfc_blind_state;
            }
            } // v33e 会话态判定块结束
        }
        // cnt==0 且 FIFO 确实空：RxDone 属于 RATS/PPS 等硬件自答层——只留痕
    }
    if (mi & 0x02)
    {
        uint8_t x;
        nfc_i2c_read_mem(0xFFF8, &x, 1); // 子寄存器读清（R4 纪律：不读清 IRQ 不释放）
    }
    if (mi & 0x01)
    {
        uint8_t x;
        nfc_i2c_read_mem(0xFFF9, &x, 1);
    }
    uint8_t wedged = nfc_read_regs();
    if (!wedged)
    {
        nfc_regs[6] = mi; // 快照保留服务轮原始 MainIrq / FifoCnt
        nfc_regs[1] = cnt;
    }
    // 防日志风暴：只在有事（mi/cnt/变化）或 ~1.6s 心跳时落页；楔死态 16 轮一页。
    // v36：纯重激活面包屑（mi=0x60 无 RxDone）不占环形页——TagInfo 扫描里它约占
    // 半数页，砍掉后同一环形容量可回看的时间窗翻倍（重激活已有 BSM 0xFE 标记）。
    nfc_log_div++;
    uint8_t changed = memcmp(nfc_regs, nfc_last_regs, sizeof(nfc_regs)) != 0;
    uint8_t rats_only = (mi == 0x60 && cnt == 0 && !wedged);
    if ((!rats_only && (mi || cnt || changed)) || (nfc_log_div & 0xFFF) == 0 ||
        (wedged && (nfc_log_div & 0x0F) == 0))
        nfc_log_capture(0x01 | 0x10 | (wedged ? 0x20 : 0));
    memcpy(nfc_last_regs, nfc_regs, sizeof(nfc_regs));
    if (wedged)
        nfc_wedge_recover(); // unstick + CSN 脉冲 + 验证（成功自落 0x41 快照）
    nfc_bus_detach(); // v34：逐轮释放（v32 形态；stead-attach 撤销）
#else
    // v18 心跳：不再用上游 time_reached_period/Timer_CH_2（unix 时钟在 suspend 重态下
    // 会冻结，v17 实锤：最后一次心跳后再无快照）。改用 retention 唤醒数（suspend 态下
    // 每个广播间隔唤醒一次 ≈ 1Hz）+ nfc_log_div 回卷（常醒态兜底，×4 节流防刷屏）双通道。
    uint8_t heartbeat = (uint8_t)(nfc_retn_wakes - nfc_hb_retn_mark) >= 180;
    if (heartbeat)
        nfc_hb_retn_mark = nfc_retn_wakes;
    nfc_log_div++;
    if (nfc_log_div == 0)
    {
        if (++nfc_div_wraps >= 4)
        {
            nfc_div_wraps = 0;
            heartbeat = 1; // 4×65536 圈（会话期常醒时约几分钟一次）
        }
    }

    if (nfc_session)
    {
        if (epd_update_state)
            return; // 屏忙：不动总线也不采（FIFO 数据保留，稍后再弹）
        // v24 会话纪律（根因修复）：**只在 IRQ 拉低（=RxDone/TxDone/FIFO 事件）时碰总线**，
        // 其余时间纯 GPIO 等待——RF 接收中途的接触侧读会复位接收（仲裁），此前 6ms 轮询
        // 把手机的每个 SELECT 都打断了（v10-v23 "FIFO 恒空" 真因）
        uint8_t irq_now = gpio_read(NFC_IRQ) ? 1 : 0;
        if (!irq_now)
        {
            // IRQ 低：一帧已收完（或 FIFO 事件）——现在碰总线是安全的
            uint8_t mi = 0, cnt = 0, st = 0;
            nfc_i2c_read_mem(0xFFF7, &mi, 1);
            nfc_i2c_read_mem(0xFFF2, &cnt, 1);
            nfc_i2c_read_mem(0xFFF3, &st, 1);
            nfc_cnt_cmd = mi; // p[133] 复用记 MainIrq 供离线分析
            if (mi == 0xFF && cnt == 0xFF)
            {
                if (++nfc_sess_ff >= 10)
                {
                    nfc_pins_rearm(); // retention 引脚鬼才允许重建（日常 NAK 不算）
                    nfc_sess_ff = 0;
                }
                return;
            }
            nfc_sess_ff = 0;
            if (cnt && cnt != 0xFF)
            {
                if (cnt > 2)
                {
                    nfc_t4t_respond(cnt); // 应答器已弹 FIFO，置 0 防快照重复弹
                    nfc_read_regs();
                    nfc_regs[1] = 0;
                }
                else
                {
                    nfc_i2c_cmd_read(0x00, &epd_temp[T4T_RX_OFF], cnt);
                    nfc_read_regs();
                    nfc_regs[1] = cnt;
                }
                nfc_log_capture(0x01 | 0x10);
                nfc_sess_quiet = 0;
            }
            else
            {
                // RxDone 面包屑但 FIFO 空（RATS 等）：原样快照留痕
                nfc_read_regs();
                nfc_regs[6] = mi;
                nfc_log_capture(0x01 | 0x08);
            }
            nfc_sess_quiet = 0;
            nfc_sess_samples = 0;
            return; // 会话期绝不写寄存器（掩码在会话结束后的空闲健康读里补）
        }
        // IRQ 高：纯等待。连续安静 ~3s（场撤）才收兵（SUSPEND 关着，tick 走真时间）
        if (++nfc_sess_samples > 8000) // ~3s @0.4ms/圈
        {
            nfc_session = 0;
            nfc_read_regs();
            nfc_log_capture(0x01);
        }
        return;
    }

    if (epd_update_state)
        return;

    // v24 空闲纪律：每圈纯 GPIO 看 IRQ（零总线活动）；IRQ 低（RxDone 等）= 进会话；
    // IRQ 高时每 ~1.2s 一次健康读（楔死监护），读窗极小化——接收途中不碰总线
    uint8_t irq_now = gpio_read(NFC_IRQ) ? 1 : 0;
    uint8_t irq_edge = (irq_now != nfc_last_irq);
    nfc_last_irq = irq_now;

    if (!irq_now)
    { // RxDone/TxDone/FIFO 事件挂起 = 会话开始（场内首帧 RATS 完成即触发）。
      // 只置位返回——读总线/应答交给会话分支下一圈统一处理（此时 nfc_regs 是旧的）
        nfc_session = 1;
        nfc_sess_quiet = 0;
        nfc_sess_ff = 0;
        nfc_sess_samples = 0;
        return;
    }

    if (epd_update_state)
        return;

    uint8_t trace_due = (nfc_log_div & 0x1FF) == 0;
    uint8_t health_due = heartbeat || irq_edge || (nfc_log_div & 0x07) == 0;
    if (!health_due && !trace_due)
        return;

    nfc_bus_attach(); // 常上电：仅重建引脚（retention 每次唤醒都抹）
    if (trace_due)
        nfc_trace_sample();

    if (nfc_read_regs())
    { // 全 0xFF / 全 0x00 / 超时：接触侧休眠/卡死
        nfc_last_allff = 1;
        nfc_log_trace_flush(); // 先落楔死前历史轨迹
        if (irq_edge || heartbeat || nfc_wedge_cnt == 0)
            nfc_log_capture(0x20 | (irq_edge ? 0x08 : 0)); // bit5=卡死，bit3=伴随 IRQ 沿
        nfc_wedge_cnt++;
        if (nfc_wedge_recover())
            nfc_last_allff = 0;
        return; // 常上电：不解挂（wedge_recover 内的 CSN 脉冲已重上电）
    }
    nfc_last_allff = 0;

    // 掩码漂移补布（场撤/CSN 脉冲会把掩码打回出厂值）：只在 IRQ 高时做
    if (nfc_regs[9] != 0xE5 || nfc_regs[10] != 0xF5)
        nfc_arm_irq_routing();

    if (heartbeat)
    {
        nfc_log_capture(0x01 | 0x04);
        memcpy(nfc_last_regs, nfc_regs, sizeof(nfc_regs));
    }
#endif // !NFC_MAOTEK_MODE
}

// RF 解毒：NfcTxEn(0xFFF4)←0x00 清 TX 锁存（历史"复位序列"/低位写可能把 TX 使能
// 锁成 0xA0，芯片变聋；2 字节寄存器写已实证可用）。返回读回值。
static uint8_t nfc_rf_untaint(void)
{
    uint8_t zero = 0x00;
    nfc_i2c_cmd_write(4, &zero, 1); // reg4=TxEn，1 字节通道（v25：映射写 NAK）
    uint8_t rb = 0xFF;
    nfc_i2c_read_mem(0xFFF4, &rb, 1);
    return rb;
}

// ---------------------------------------------------------------------------
// EEPROM 写解锁策略矩阵（v18）
//
// 三源实证（详见 docs/hardware_notes.md）：
//  - solokeys fm11nc08（同芯片 SPI 版）：写 EEPROM 前须发独立帧 magic {0xCE,0x55}；
//    写后查 AuxIrq(0xFFF9)：bit6=写了禁区 bit7=没写进去；写块后 ~10ms 完成。
//  - 复旦微 I²C demo（NT082C 内核驱动）：无 magic，静默(0xFFE6←0x33)→16B 块写→
//    STOP 后 10ms→释放(0xCC)；寄存器窗 0xFFF0-0xFFFF 与本芯片完全同构
//    （0xFFFF=EE_WR_CTRL，本芯片读回 0x01）。
//  - 本机 v10 教训：0x3B0 单字节裸写被拒（无 10ms 等待、无块对齐、可能在保护区）。
// 8 种策略依次试草稿区 0x100（出厂全零、远离 NDEF/配置块），读回+AuxIrq 双验证；
// 任一成功即还原草稿区并改写 NDEF 文本（等长，手机通知直接可见）。
// ---------------------------------------------------------------------------

// 无地址相位原始写（[START][0xAE][bytes...][STOP]）——SPI magic 帧的 I²C 直译
static int nfc_i2c_write_raw(const uint8_t *bytes, int len)
{
    i2c_set_id(NFC_I2C_ID_WR);
    reg_i2c_ctrl = FLD_I2C_CMD_ID | FLD_I2C_CMD_START;
    if (nfc_i2c_wait())
        goto wedged;
    for (int i = 0; i < len; i++)
    {
        reg_i2c_di = bytes[i];
        reg_i2c_ctrl = FLD_I2C_CMD_DI;
        if (nfc_i2c_wait())
            goto wedged;
    }
    reg_i2c_ctrl = FLD_I2C_CMD_STOP;
    if (nfc_i2c_wait())
        goto wedged;
    return 0;
wedged:
    nfc_i2c_reinit();
    return -1;
}

static void nfc_reg_write(uint16_t addr, uint8_t v) { nfc_i2c_write_mem2(addr, &v, 1); }

// 按策略 id 做前置动作 → 16B 块写 → 10ms → 后置恢复
static void nfc_eeprom_try(uint8_t id, uint16_t addr, const uint8_t *w16)
{
    uint8_t m[2];
    switch (id)
    {
    case 1: break;                                              // 裸写（gen-2 demo 式）
    case 2: nfc_reg_write(0xFFE6, 0x33); break;                 // 静默后写
    case 3: nfc_reg_write(0xFFFF, 0x55); break;                 // EE_WR_CTRL←0x55
    case 4: nfc_reg_write(0xFFFF, 0xCE); nfc_reg_write(0xFFFF, 0x55); break;
    case 5: m[0] = 0xCE; m[1] = 0x55; nfc_i2c_write_raw(m, 2); break; // magic 原始帧
    case 6: m[0] = 0x55; nfc_i2c_write_mem2(0xCE55, m, 1); break;     // magic 当地址写
    case 7: nfc_reg_write(0xFFE6, 0x33); nfc_reg_write(0xFFFF, 0x55); break;
    case 8: m[0] = 0xCE; m[1] = 0x55; nfc_reg_write(0xFFE6, 0x33); nfc_i2c_write_raw(m, 2); break;
    }
    nfc_i2c_write_mem2(addr, w16, 16);
    sleep_ms(10); // STOP 后 EEPROM 块擦写周期（demo 注释：FM441 5ms / 保守 10ms）
    if (id == 2 || id == 7 || id == 8)
    {
        nfc_reg_write(0xFFE6, 0xCC); // 释放静默
        sleep_ms(1);
    }
    if (id == 3 || id == 4 || id == 7)
        nfc_reg_write(0xFFFF, 0x01); // EE_WR_CTRL 回出厂读值
}

// 返回成功策略 id（0=全失败）；结果记 res[NFC_MATRIX_OFF..]
static uint8_t nfc_eeprom_matrix(uint8_t *res)
{
    uint8_t *hdr = &res[NFC_MATRIX_OFF];
    hdr[0] = 'N';
    hdr[1] = 'F';
    hdr[2] = 'M';
    hdr[3] = '1';
    uint8_t winner = 0;
    for (uint8_t id = 1; id <= 8; id++)
    {
        uint8_t pat[16], rb[8], aux = 0;
        for (int i = 0; i < 16; i++)
            pat[i] = (uint8_t)(0x5A ^ (id << 4) ^ i);
        nfc_reg_write(0xFFF9, 0x00); // 清 AuxIrq（写 0 清除，solokeys 同法）
        nfc_eeprom_try(id, 0x0100, pat);
        nfc_i2c_read_mem(0x0100, rb, 8);
        nfc_i2c_read_mem(0xFFF9, &aux, 1);
        uint8_t *e = &hdr[0x10 + (id - 1) * 10];
        e[0] = id;
        e[1] = aux;
        memcpy(&e[2], rb, 8);
        if (memcmp(rb, pat, 8) == 0)
        {
            winner = id;
            break;
        }
    }
    hdr[4] = winner;
    if (winner)
    {
        // v26 收档：不再每次开机重写 NDEF 文本（EEPROM 磨损 + 已还原出厂原文）。
        // NDEF 内容今后一律经 BLE 控制台按需写（eewrite 0x010 ...）
        uint8_t zeros[16] = {0};
        nfc_eeprom_try(winner, 0x0100, zeros); // 草稿区还原全零
    }
    return winner;
}

// ---------------------------------------------------------------------------
// BLE 在线 EEPROM 控制台（v20，ota.c case 8/9/A 分发；免重刷迭代配置实验）
// 返回码：0=ok 1=地址越界 2=RF 会话期（拒用防搅局）3=总线卡死
// ---------------------------------------------------------------------------

uint8_t nfc_ee_console_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb)
{
    if (addr < 0x000C || (uint32_t)addr + len > 0x3FF || len == 0)
        return 1; // 只保 UID 区 0x000-0x00B；0x00C-0x00F = T2T CC 区（v26 人格移植用）
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    if (nfc_read_regs())
    {
        nfc_bus_detach();
        return 3;
    }
    uint16_t blk = addr & ~0x0F; // 覆盖到的 16B 块逐个读-改-写（S1 实证形态）
    while (1)
    {
        uint8_t buf[16];
        if (nfc_i2c_read_mem(blk, buf, 16) != 0)
        {
            nfc_bus_detach();
            return 3;
        }
        for (uint16_t i = addr; i < addr + len && i < blk + 16; i++)
            buf[i - blk] = data[i - addr];
        nfc_i2c_write_mem2(blk, buf, 16);
        sleep_ms(10); // STOP 后块擦写周期
        if (blk >= ((addr + len - 1) & ~0x0F))
            break;
        blk += 16;
    }
    nfc_i2c_read_mem(addr, rb, len);
    nfc_rf_untaint();
    nfc_bus_detach();
    return 0;
}

uint8_t nfc_ee_console_read(uint16_t addr, uint8_t *dst, uint8_t len)
{
    if (len == 0 || (uint32_t)addr + len > 0x400)
        return 1;
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    if (nfc_read_regs())
    {
        nfc_bus_detach();
        return 3;
    }
    nfc_i2c_read_mem(addr, dst, len);
    nfc_bus_detach();
    return 0;
}

// 软复位（重读 EEPROM 配置）后读回状态：rb[0..5]=FFE0/FFE1/FFE2/NfcStatus/TxEn/MainIrqMask
uint8_t nfc_ee_console_reset(uint8_t *rb)
{
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    uint8_t v = 0x55;
    nfc_i2c_write_mem2(0xFFE6, &v, 1); // RESET_SILENCE.soft_reset
    sleep_ms(5);
    nfc_rf_untaint();
    nfc_arm_irq_routing();
    for (int i = 0; i < 3; i++)
        nfc_i2c_read_mem(0xFFE0 + i, &rb[i], 1);
    nfc_i2c_read_mem(0xFFF3, &rb[3], 1);
    nfc_i2c_read_mem(0xFFF4, &rb[4], 1);
    nfc_i2c_read_mem(0xFFFA, &rb[5], 1);
    nfc_bus_detach();
    return 0;
}

// 交互诊断（v21，会话中允许——只读、不动挂载态）：
// out[0..8] = FifoCnt映射 / FifoCnt命令 / Status映射 / Status命令 / MainIrq / Rats / TxEn / AuxIrq / NfcCfg
uint8_t nfc_ee_console_diag(uint8_t *out)
{
    memset(out, 0xFF, 9);
    uint8_t held = nfc_session;
    if (!held)
        nfc_bus_attach();
    uint8_t dead = nfc_read_regs();
    out[0] = nfc_regs[1];
    out[2] = nfc_regs[2];
    out[4] = nfc_regs[6];
    out[5] = nfc_regs[5];
    out[6] = nfc_regs[3];
    nfc_i2c_read_mem(0xFFF9, &out[7], 1);
    nfc_i2c_read_mem(0xFFFD, &out[8], 1);
    nfc_i2c_cmd_read(0x02, &out[1], 1); // FifoCnt 1 字节索引式
    nfc_i2c_cmd_read(0x03, &out[3], 1); // RfStatus 1 字节索引式
    if (!held)
        nfc_bus_detach();
    return dead ? 3 : 0;
}

// 寄存器窗写（v21，实验用：0x77 回接收态 / RfBaud 等）
// v33b：写后延时 2ms 再读回（即读会拿到 FF——寄存器写后总线短暂不可读，
// masktest 实证掩码写其实成功）；门槛改 IRQ 判场。
uint8_t nfc_reg_console_write(uint16_t addr, uint8_t v, uint8_t *rb)
{
    *rb = 0xFF;
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    nfc_i2c_write_mem2(addr, &v, 1);
    sleep_ms(2);
    nfc_i2c_read_mem(addr, rb, 1);
    nfc_bus_detach();
    return 0;
}

// ---------------------------------------------------------------------------
// v33 RE 控制台（逆向实验梯，docs/nfc_reference_diff.md §9）
// 依据（NT082C 手册 4.3 双配置字机制 + 3.3.3.5 静默时序 + 复旦微 demo）：
//  - 实际配置字 @0x390 [CFG0,CFG1,CFG2,CHK=~(C0^C1^C2)]，CHK 合法时优先于
//    默认/尾块配置生效（本机 0x390 全零 = 校验恒不过 = 恒回退）
//  - 写配置的官方时序：0x33 静默 → 写 → 0xCC 放开 → 1ms → TxEn←0x77 → 0x55 软复位
//  - AUX_IRQ bit6=写被拒 bit7=写成功（判别器）
// ---------------------------------------------------------------------------

// 写 0x390 实际配置字。rb[0..3]=写后读回 [4..6]=FFE0/1/2 [7]=AuxIrq [8]=MIM
uint8_t nfc_cfg_console_write(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t mode, uint8_t *rb)
{
    if (nfc_field_active())
        return 2;
    uint8_t w[4] = {c0, c1, c2, (uint8_t)~(c0 ^ c1 ^ c2)};
    nfc_bus_attach();
    if (nfc_read_regs())
    {
        nfc_bus_detach();
        return 3;
    }
    if (mode & 1)
    {
        uint8_t v33 = 0x33;
        nfc_i2c_write_mem2(0xFFE6, &v33, 1); // 静默（RF 状态机复位，写配置不被打扰）
        sleep_ms(1);
    }
    nfc_i2c_write_mem2(0x0390, w, 4); // 4B 直写（复旦微 demo 同款：Protocol_4×4B@0x390）
    sleep_ms(10);                     // EEPROM 块擦写周期
    if (mode & 1)
    {
        uint8_t vcc = 0xCC;
        nfc_i2c_write_mem2(0xFFE6, &vcc, 1); // 放开静默
        sleep_ms(1);
        uint8_t rx = 0x77; // TxEn←0x77 切回接收态（demo 时序第 ⑨ 步）
        nfc_i2c_write_mem2(0xFFF4, &rx, 1);
    }
    uint8_t v55 = 0x55;
    nfc_i2c_write_mem2(0xFFE6, &v55, 1); // 软复位：重读 EEPROM 配置（mode 0/1 都做）
    sleep_ms(5);
    nfc_i2c_read_mem(0x0390, &rb[0], 4);
    nfc_i2c_read_mem(0xFFE0, &rb[4], 1);
    nfc_i2c_read_mem(0xFFE1, &rb[5], 1);
    nfc_i2c_read_mem(0xFFE2, &rb[6], 1);
    nfc_i2c_read_mem(0xFFF9, &rb[7], 1); // AuxIrq（bit6 写被拒 / bit7 写成功）
    nfc_i2c_read_mem(0xFFFA, &rb[8], 1);
    nfc_bus_detach();
    return 0;
}

// v33d：手动解卡（控制台 0xF，无门槛）——9 时钟解卡 + 彻底断电 20ms（清 RF 状态机
// 与锁存 IRQ 源，破"IRQ 钉低+保持挂载"死锁）+ 重上电验证。rb[0..5]=FFF1-FFF6。
uint8_t nfc_unstick_console(uint8_t *rb)
{
    nfc_i2c_unstick();
    gpio_write(NFC_CS, 1);
    sleep_ms(20);
    gpio_write(NFC_CS, 0);
    sleep_ms(2);
    nfc_i2c_reinit();
    uint8_t dead = nfc_read_regs();
    memcpy(rb, nfc_regs, 6);
    nfc_bus_detach();
    return dead ? 3 : 0;
}

// ---------------------------------------------------------------------------
// v34 RE 控制台第二梯队（逆向续篇）：
//  - eesnap：EEPROM 区间快照落 flash 0x45000（PC 经 cmd4 read 取回）——碰卡前后
//    全片 diff，裁决"邮箱假设"（仲裁=00 时 EEPROM 作双界面信箱，手册 3.2.12）
//  - dwrite：EEPROM 直写（非 RMW、不块对齐）——探测写保护颗粒度（今晚 0x3B8 的
//    拒绝走的是 16B 块 RMW，被同块 0x3BB 牵连；海思 demo 都是单字节直写）
//  - magwrite：solokeys SPI 版 EEPROM 写使能 magic {0xCE,0x55} 的 I²C 直译帧 + 直写
// ---------------------------------------------------------------------------

uint8_t nfc_ee_snapshot(uint16_t addr, uint16_t len)
{
    if (len == 0 || len > 1024 || (uint32_t)addr + len > 0x400)
        return 1;
    if (epd_update_state)
        return 4; // 屏忙占用 epd_temp
    uint8_t *buf = epd_temp;
    nfc_bus_attach();
    for (uint32_t o = 0; o < len; o += 16)
    {
        if (nfc_i2c_read_mem(addr + o, &buf[o], 16) != 0)
        {
            nfc_bus_detach();
            return 3;
        }
    }
    nfc_bus_detach();
    flash_erase_sector(0x45000);
    for (uint32_t o = 0; o < len; o += 256)
        flash_write_page(0x45000 + o, 256, &buf[o]); // 尾页可能带旧数据，PC 只取前 len 字节
    return 0;
}

// rb[0]=AuxIrq（bit6 写被拒 / bit7 写成功）rb[1..]=写后读回
uint8_t nfc_ee_direct_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb)
{
    if (len == 0 || len > 11)
        return 1;
    nfc_bus_attach();
    nfc_i2c_write_mem2(addr, data, len);
    sleep_ms(10);
    nfc_i2c_read_mem(0xFFF9, &rb[0], 1);
    nfc_i2c_read_mem(addr, &rb[1], len);
    nfc_bus_detach();
    return 0;
}

uint8_t nfc_ee_magic_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb)
{
    if (len == 0 || len > 11)
        return 1;
    uint8_t m[2] = {0xCE, 0x55};
    nfc_bus_attach();
    nfc_i2c_write_raw(m, 2); // solokeys start_write 的 SPI magic 直译（v18 矩阵只试过普通区）
    sleep_ms(1);
    nfc_i2c_write_mem2(addr, data, len);
    sleep_ms(10);
    nfc_i2c_read_mem(0xFFF9, &rb[0], 1);
    nfc_i2c_read_mem(addr, &rb[1], len);
    nfc_bus_detach();
    return 0;
}

// 掩码写同会话验证（裁决 ct_sleep 掉电回默认 vs 写即楔死）。
// rb[0]=写前 [1]=写后即读 [2]=+2ms [3]=+10ms（0xFFFA 值，全程不 CSN 翻转）[4]=AuxIrq
uint8_t nfc_mask_console_test(uint8_t *rb){
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    nfc_i2c_read_mem(0xFFFA, &rb[0], 1);
    uint8_t m = 0xE5;
    nfc_i2c_write_mem2(0xFFFA, &m, 1);
    nfc_i2c_read_mem(0xFFFA, &rb[1], 1);
    sleep_ms(2);
    nfc_i2c_read_mem(0xFFFA, &rb[2], 1);
    sleep_ms(10);
    nfc_i2c_read_mem(0xFFFA, &rb[3], 1);
    nfc_i2c_read_mem(0xFFF9, &rb[4], 1);
    nfc_bus_detach();
    return 0;
}

// v39：TX FIFO 落点自环（接触侧，无需碰卡）。三种写法各写 3B 图案 {5A A5 3C}
// （不发 TxEn），看 FifoCnt 是否 +3、单字节弹出是否回原图案。
// 动机：v35-v38 三轮应答内容大改（NLEN/6D00/FCI）手机侧行为逐事件零变化——
// 应答内容可能从未进 TX FIFO（EEPROM 事故已实证 0xFFF0 写的译码不稳定，
// 楔死态被当 EEPROM 写）。rb[0]=写前cnt [1]=写后cnt [2..7]=弹出6B
// [8]=终cnt（flush 后） [9]=AuxIrq [10]=命令通道cnt 对照
uint8_t nfc_fifo_console_test(uint8_t mode, uint8_t *rb)
{
    static const uint8_t pat[3] = {0x5A, 0xA5, 0x3C};
    nfc_bus_attach();
    memset(rb, 0xFF, 11);
    if (nfc_read_regs())
    {
        nfc_bus_detach();
        return 3;
    }
    rb[0] = nfc_regs[1]; // 0xFFF2 FifoCnt（内存窗）
    if (mode == 0)
        nfc_i2c_write_mem2(0xFFF0, pat, 3); // 内存窗写（v27+ 盲应答现行路径）
    else if (mode == 1)
        nfc_i2c_cmd_write(0x80, pat, 3); // solokeys SPI 直译：0x80+数据
    else
        nfc_i2c_cmd_write(0x00, pat, 3); // v25 旧应答器路径（reg0 自增）
    sleep_ms(1);
    nfc_i2c_read_mem(0xFFF2, &rb[1], 1);
    for (int i = 0; i < 6; i++)
        nfc_i2c_read_mem(0xFFF0, &rb[2 + i], 1); // 弹出留痕（空口回显锁存值）
    nfc_i2c_cmd_read(0x02, &rb[10], 1);          // 命令通道 FifoCnt 对照
    nfc_i2c_read_mem(0xFFF9, &rb[9], 1);
    uint8_t ff = 0xFF;
    nfc_i2c_write_mem2(0xFFF1, &ff, 1); // flush：别把图案留给下一场 RF
    sleep_ms(1);
    nfc_i2c_read_mem(0xFFF2, &rb[8], 1);
    nfc_bus_detach();
    return 0;
}

// ---------------------------------------------------------------------------
// v40 RE 控制台第三梯队（ota.c case 0x14/0x15）
//  - txdis：TX 全链路禁用开关（§11.5 预设对照实验，RAM 态重启即回 0=照常发）
//  - vendorseq：从 RAM 缓冲回放 8B 定长记录的接触侧 I²C 脚本——阶段 1（Handy Link
//    逆向）/阶段 4（原厂固件尸检）提取的厂商配方在 tag#1 复刻的执行器。
//    记录格式 [type][addr_hi][addr_lo][len][p1..p4]：
//      0=EEPROM 写（2 字节地址，写后 10ms） 5=寄存器窗快写（写后 2ms）
//      1=读 ≤4B（结果顺序拼进 notify）     2=延时 p1 ms
//      3=原始帧写（无地址相位，solokeys magic 直译） 4=CSN 断电周期（p1 ms 后回低）
//      6=FIFO flush（0xFFF1←0xFF）
//    红线依旧由硬件地址级保护兜底（尾块/UID/0x3BB），脚本端不重复设防；
//    0xFFFD 禁写靠操作者纪律（gen-1 红线，见 hardware_notes）。
// ---------------------------------------------------------------------------

uint8_t nfc_txdis_console_set(uint8_t en, uint8_t *rb)
{
    nfc_tx_disabled = en ? 1 : 0;
    rb[0] = nfc_tx_disabled;
    return 0;
}

// v41：静默模式（ota.c case 0x16）。置 1 = 重建引脚（CSN 拉低=接触侧上电）后
// logger_poll 全面静默——总线上零流量、CSN 不再翻转。测"芯片在接触侧上电且
// 静默时是否自应答 T4T NDEF"（0.4 抠电池实验只能证明 MCU 应答或 CSN 悬空不行，
// 无法区分本假设）。置 0 恢复正常轮询（CSN 回空闲断电态）。RAM 态，重启即清。
uint8_t nfc_mute_console_set(uint8_t en, uint8_t *rb)
{
    if (en)
    {
        nfc_mute_hold = 1;
        nfc_pins_rearm(); // CSN 输出 0（此后无人再碰引脚/总线；suspend 已被禁）
    }
    else
    {
        nfc_mute_hold = 0;
        nfc_bus_detach(); // CSN 回高（接触侧断电，正常空闲态）
    }
    rb[0] = nfc_mute_hold;
    return 0;
}

// rb[0]=执行条数 rb[1]=收尾健康检查（0=ok 1=卡死） rb[2..17]=read 拼包（≤16B）
uint8_t nfc_vendorseq_console_run(const uint8_t *script, uint8_t bytes, uint8_t *rb)
{
    memset(rb, 0, 18);
    if (bytes == 0 || (bytes & 7) || bytes > 240)
        return 1; // 非法脚本（须 8B 整数倍，≤30 条）
    if (nfc_field_active())
        return 2;
    nfc_bus_attach();
    if (nfc_read_regs())
    {
        nfc_bus_detach();
        return 3;
    }
    uint8_t n_op = bytes >> 3;
    uint8_t ridx = 0;
    for (uint8_t op = 0; op < n_op; op++)
    {
        const uint8_t *e = &script[op * 8];
        uint8_t type = e[0];
        uint16_t addr = (uint16_t)((e[1] << 8) | e[2]);
        uint8_t len = e[3] > 4 ? 4 : e[3];
        uint8_t tmp[4];
        if (type == 0)
        {
            nfc_i2c_write_mem2(addr, &e[4], len);
            sleep_ms(10); // EEPROM 块擦写周期
        }
        else if (type == 5)
        {
            nfc_i2c_write_mem2(addr, &e[4], len);
            sleep_ms(2);
        }
        else if (type == 1)
        {
            if (nfc_i2c_read_mem(addr, tmp, len) == 0 && ridx + len <= 16)
            {
                memcpy(&rb[2 + ridx], tmp, len);
                ridx += len;
            }
        }
        else if (type == 2)
        {
            sleep_ms(e[4]);
        }
        else if (type == 3)
        {
            nfc_i2c_write_raw(&e[4], len);
            sleep_ms(2);
        }
        else if (type == 4)
        {
            gpio_write(NFC_CS, 1);
            sleep_ms(e[4] ? e[4] : 20);
            gpio_write(NFC_CS, 0);
            sleep_ms(2);
            nfc_i2c_reinit(); // 断电后外设/从机相位重建
        }
        else if (type == 6)
        {
            uint8_t ff = 0xFF;
            nfc_i2c_write_mem2(0xFFF1, &ff, 1);
            sleep_ms(1);
        }
    }
    rb[0] = n_op;
    rb[1] = nfc_read_regs();
    nfc_rf_untaint(); // 厂商序列若写 TxEn，防 TX 锁存残留
    nfc_bus_detach();
    return 0;
}

#if NFC_MAOTEK_MODE
// v27 布防（maotek 移植）：确保 0x3B5=0xA0 → FIFO_FLUSH → 实验三 IRQ 归因序列留痕。
// 不写掩码、不写 0xFFE0 区、不复位 —— 与 maotek/hanshow_stellar 的 init_nfc 逐步对齐。
// meta[0x40..0x4E]：0x3B5 值/补写标志/补写读回、flush 后 FifoCnt、IRQ 归因序列、
// FFE0/1/2 与 MainIrqMask 基线。
static void nfc_logger_init(void)
{
    uint8_t *meta = epd_temp;
    nfc_bus_attach();
    memset(meta, 0, 256);
    meta[0] = 'N';
    meta[1] = 'F';
    meta[2] = 'C';
    meta[3] = 'L';
    meta[4] = 9; // 结构版本（v27：maotek 旁路 + IRQ 归因序列）

    // maotek init 第一步：0x3B5（TB/模式字节）必须 = 0xA0（汉朔出厂值；0x90 杀 RF）
    uint8_t v = 0xFF;
    uint8_t ok = (nfc_i2c_read_mem(0x03B5, &v, 1) == 0);
    meta[0x40] = v;
    if (ok && v != 0xA0) // 读失败（v=0xFF）不盲写——写本身幂等但不制造无谓写周期
    {
        uint8_t a0 = 0xA0;
        nfc_i2c_write_mem2(0x03B5, &a0, 1);
        sleep_ms(10); // EEPROM 写周期（官方 demo：必须 10ms）
        meta[0x41] = 1;
        nfc_i2c_read_mem(0x03B5, &meta[0x42], 1); // 补写后读回
    }
    // maotek init 第二步：FIFO_FLUSH（2 字节寄存器窗写——本机是否接受此写法的实证点）
    uint8_t ff = 0xFF;
    nfc_i2c_write_mem2(0xFFF1, &ff, 1);
    sleep_ms(1);
    nfc_i2c_read_mem(0xFFF2, &meta[0x43], 1); // flush 后 FifoCnt

    // 实验三：IRQ 常低归因（逐步读清，看哪一步释放 IRQ）
    uint8_t mi = 0;
    meta[0x44] = gpio_read(NFC_IRQ) ? 1 : 0;
    nfc_i2c_read_mem(0xFFF7, &mi, 1);
    meta[0x45] = mi;
    meta[0x46] = gpio_read(NFC_IRQ) ? 1 : 0;
    if (mi & 0x02)
        nfc_i2c_read_mem(0xFFF8, &meta[0x47], 1); // FifoIrq 读清
    if (mi & 0x01)
        nfc_i2c_read_mem(0xFFF9, &meta[0x48], 1); // AuxIrq 读清
    meta[0x49] = gpio_read(NFC_IRQ) ? 1 : 0;
    nfc_i2c_write_mem2(0xFFF1, &ff, 1); // 二次 flush 清残留
    meta[0x4A] = gpio_read(NFC_IRQ) ? 1 : 0;
    // 活配置基线（2026-10-25 BLE 控制台实测：FFE0=03 FFE1=c2 FFE2=00 MIM=42）
    nfc_i2c_read_mem(0xFFE0, &meta[0x4B], 1);
    nfc_i2c_read_mem(0xFFE1, &meta[0x4C], 1);
    nfc_i2c_read_mem(0xFFE2, &meta[0x4D], 1);
    nfc_i2c_read_mem(0xFFFA, &meta[0x4E], 1);
    meta[0x4F] = 8; // v28 标记：IRQ 消抖 8ms（v27=0）
    // v32 的 FIFO 回读自检已移除（v33c）：空 FIFO 过读疑似楔死接触侧 + 触发
    // FIFO-Empty 锁存（FIM=0x00 下 IRQ 被钉低），boot 后整机服务循环打转

    for (int s = 0; s < NFC_LOG_SECTORS; s++)
        flash_erase_sector(NFC_LOG_ADDR + s * 0x1000);
    flash_write_page(NFC_LOG_ADDR, 256, meta);
    // v36：BSM 时间线区清零开局
    for (int s = 0; s < NFC_BSM_SECTORS; s++)
        flash_erase_sector(NFC_BSM_ADDR + s * 0x1000);
    nfc_bsm_n = 0;
    nfc_bsm_page = 0;
    nfc_bsm_total = 0;
    nfc_read_regs();
    memcpy(nfc_last_regs, nfc_regs, sizeof(nfc_regs));
    nfc_last_irq = gpio_read(NFC_IRQ) ? 1 : 0;
    nfc_log_next_page = 1;
    nfc_log_seq = 0;
    memcpy(t4t_ndef, t4t_ndef_rom, sizeof(t4t_ndef));
    nfc_log_armed = 1;
    nfc_session = 1; // v27：布防即禁 suspend（v19：1s 广播的 suspend 会睡过整场 T4T 交换）
    nfc_bus_detach(); // maotek：空闲期接触侧断电
}
#else
// 0x41000 布防：页 0 = 元数据（魔数 + 逐字节别名检查），总线保持挂载
static void nfc_logger_init(void)
{
    uint8_t *meta = epd_temp;
    nfc_bus_attach();
    memset(meta, 0, 256);
    meta[0] = 'N';
    meta[1] = 'F';
    meta[2] = 'C';
    meta[3] = 'L';
    meta[4] = 8; // 结构版本（v18：环形回卷 + 唤醒数心跳 + 嗅探模式）
    // 配置生效序列（solokeys gen-2 同法）：软复位重读 EEPROM → 活 USER_CFG 写 → RF 激活
    {
        uint8_t v;
        v = 0x55;
        nfc_i2c_write_mem2(0xFFE6, &v, 1); // RESET_SILENCE.soft_reset
        sleep_ms(5);
        nfc_i2c_read_mem(0xFFE0, &v, 1);
        v |= 0x01; // OP_MODE_SELECT = 1（NC 通道模式）
        nfc_i2c_write_mem2(0xFFE0, &v, 1);
        nfc_i2c_read_mem(0xFFE1, &v, 1);
        v &= (uint8_t)~0x0C; // NFC_mode[3:2] = 00（ISO14443-4）
        nfc_i2c_write_mem2(0xFFE1, &v, 1);
        nfc_i2c_read_mem(0xFFE2, &v, 1);
        v = (v & (uint8_t)~0x30) | 0x10; // 仲裁 = 先到先得（00 会拒收 I-block！）
        nfc_i2c_write_mem2(0xFFE2, &v, 1);
        v = 0xCC;
        nfc_i2c_write_mem2(0xFFE6, &v, 1); // NFC_silence → active
        meta[0x30] = nfc_rf_untaint();     // 清 TX 锁存
        nfc_i2c_read_mem(0xFFE0, &meta[0x31], 1);
        nfc_i2c_read_mem(0xFFE1, &meta[0x32], 1);
        nfc_i2c_read_mem(0xFFE2, &meta[0x33], 1);
        nfc_i2c_read_mem(0xFFF3, &meta[0x34], 1);
        nfc_i2c_read_mem(0xFFF4, &meta[0x35], 1);
    }
    for (int i = 0; i < 32; i++)
    {
        nfc_i2c_read_mem(0xFFE0 + i, &meta[0x10 + i], 1); // 单字节读，验证窗内是否别名
    }

    for (int s = 0; s < NFC_LOG_SECTORS; s++)
        flash_erase_sector(NFC_LOG_ADDR + s * 0x1000);
    flash_write_page(NFC_LOG_ADDR, 256, meta);

    nfc_read_regs();
    memcpy(nfc_last_regs, nfc_regs, sizeof(nfc_regs));
    nfc_last_irq = gpio_read(NFC_IRQ) ? 1 : 0;
    nfc_log_next_page = 1;
    nfc_log_seq = 0;
    nfc_log_div = 0;
    nfc_hb_retn_mark = nfc_retn_wakes;
    memcpy(t4t_ndef, t4t_ndef_rom, sizeof(t4t_ndef)); // v20：应答器 NDEF RAM 副本
    nfc_tr_idx = 0;
    nfc_tr_flushed_idx = 0;
    nfc_log_armed = 1;
    nfc_arm_irq_routing(); // v18：布防即路由（RfPower 置 IRQ_N，空闲圈沿触发秒进会态）
    nfc_bus_detach();
    nfc_attached = 0; // v16：空闲期每次轮询挂载/卸载（防接触接口 60s 自动休眠）
}
#endif // !NFC_MAOTEK_MODE

// 配置实验（v10）：把 solokeys 在同族硅上验证可用的 Type 4 通道 ATS/NFC_CFG 块写入
// 0x3B0-0x3B6。出厂值 {05 72 01 57 f7 a0 02} 与其差异 = T0(72→78)/NFC_CFG(01→00)/
// TA(f7→91)/TB(a0→78)/TC(02→00)。出厂值已留档于本结果区 0x028 dump，可随时回滚。
// 写法 = 同代 NT081D 海思驱动实证的裸 I2C 写（2 字节地址 + 数据 + 10ms 写周期）。
// 结果记入 res[0x628..]：[0-6]写入值 [7]读回一致标志 [8-15]读回 0x3B0-0x3B7。
static void nfc_config_experiment(uint8_t *res)
{
    static const uint8_t cfg[7] = {0x05, 0x78, 0x00, 0x57, 0x91, 0x78, 0x00};
    uint8_t rb[8] = {0};
    memset(&res[0x628], 0, 0x18);
    memcpy(&res[0x628], cfg, 7);
    for (int i = 0; i < 7; i++)
        nfc_i2c_write_mem2(0x03B0 + i, &cfg[i], 1);
    sleep_ms(15);
    nfc_i2c_read_mem(0x03B0, rb, 8);
    memcpy(&res[0x630], rb, 8);
    res[0x62F] = (memcmp(rb, cfg, 7) == 0) ? 1 : 0;
}

void init_nfc(void)
{
    uint8_t *res = epd_temp;

    // CSN 与 IRQ_N 的引脚配置与上游一致（CSN 平时拉高 = 接触侧断电待机）
    gpio_write(NFC_CS, 1);
    gpio_set_func(NFC_CS, AS_GPIO);
    gpio_set_output_en(NFC_CS, 1);
    gpio_set_input_en(NFC_CS, 0);
    gpio_setup_up_down_resistor(NFC_CS, PM_PIN_PULLUP_10K);

    gpio_set_func(NFC_IRQ, AS_GPIO);
    gpio_set_output_en(NFC_IRQ, 0);
    gpio_set_input_en(NFC_IRQ, 1);
    gpio_setup_up_down_resistor(NFC_IRQ, PM_PIN_PULLUP_10K);

    memset(res, 0, NFC_RESULT_LEN);
    res[0] = 'N';
    res[1] = 'F';
    res[2] = 'C';
    res[3] = 'P';
    res[4] = 1; // 结构版本
    res[6] = (uint8_t)(CLOCK_SYS_CLOCK_HZ / (4 * 400000));
    res[7] = gpio_read(NFC_IRQ) ? 0x01 : 0x00;

    // 扫描 1：CSN 低（接触侧上电，主假设，HiSilicon/ATC 同态）
    gpio_write(NFC_CS, 0);
    sleep_ms(5);
    nfc_scan_bus(&res[0x08]);
    res[7] |= gpio_read(NFC_IRQ) ? 0x02 : 0x00;

    nfc_probe_done = 1;
    nfc_probe_ack = (res[0x08 + (NFC_I2C_ID_7BIT >> 3)] & (0x80 >> (NFC_I2C_ID_7BIT & 7))) ? 1 : 0;

    if (nfc_probe_ack)
    {
        // 扫描 2：CSN 高（对照：I2C 是否需要 CSN 使能）
        gpio_write(NFC_CS, 1);
        sleep_ms(5);
        nfc_scan_bus(&res[0x18]);
        gpio_write(NFC_CS, 0);
        sleep_ms(5);

        nfc_dump(0x0000, 2, &res[0x028], 0x400);
        nfc_dump(0x0000, 1, &res[0x428], 0x100);
        nfc_dump(0xFF00, 2, &res[0x528], 0x100);

#if NFC_EXPERIMENT
        nfc_config_experiment(res); // 出厂状态已 dump 留档，之后写 solokeys 验证配置
#endif
        // 1 字节地址式读 SPI 风格寄存器窗 0x00-0x0E（判别低位写译码假设的读路径）
        for (int i = 0; i < 15; i++)
            i2c_read_series(0x00 + i, 1, &res[0x640 + i], 1);

        // v18：EEPROM 写解锁策略矩阵（草稿区 0x100；成功则顺势把 NDEF 文本改成
        // "nowa154-NFC"，手机碰一下通知文本变化即端到端实证）
#if !NFC_MAOTEK_MODE // v27：maotek 模式零 EEPROM 写（S1 裸写可写已由控制台实证）
        nfc_eeprom_matrix(res);
#endif
    }

    // 结果落 flash：0x40000 空闲扇区，OTA 暂存区止于 0x40000 之前
    flash_erase_sector(NFC_RESULT_ADDR);
    for (uint32_t off = 0; off < NFC_RESULT_LEN; off += 256)
    {
        uint32_t chunk = NFC_RESULT_LEN - off;
        if (chunk > 256)
            chunk = 256;
        flash_write_page(NFC_RESULT_ADDR + off, chunk, &res[off]);
    }
#if NFC_LOGGER
        nfc_logger_init(); // v27：maotek 布防（0x3B5 校验 + FIFO_FLUSH + IRQ 归因留痕）
#else
        gpio_write(NFC_CS, 1); // 干净版：接触侧断电待机（与 v1 等价状态）
#endif
}
