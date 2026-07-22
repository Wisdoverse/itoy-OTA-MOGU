#ifndef POWER_CONTROL_H
#define POWER_CONTROL_H

/**
 * 电源控制模块（软开关 / 软关机）
 *
 * 硬件（依据 2026-07-08 网表）：
 *   POWER_ON    = GPIO39 → Q3 栅极
 *                 输出高 = 保持供电（开机锁定）；输出低 = 释放锁定 → 断电
 *   POWER_JUDGE = GPIO42 ← ON 网络
 *                 路径：按键 SW1 → SW → R30 → D3 → ON → D1 → GPIO42，R15 下拉
 *                 作为输入检测按键：按下为高电平，松开为低电平
 *
 * 行为：
 *   开机：上电后立即把 POWER_ON(GPIO39) 拉高，锁定供电。
 *   关机：监测到 POWER_JUDGE(GPIO42) 被持续按下 ≥2 秒
 *         → POWER_ON 与 POWER_JUDGE 全部拉低 → 断电。
 *
 * 说明：POWER_JUDGE 需要作为输入检测按键，故开机时【不】把它驱动为高，
 *       否则按键按下/松开都无法被识别。断电时才将其切为输出低，主动把
 *       ON 网络拉低，协助 PMIC 彻底断电。
 *       若你的原理图里 POWER_JUDGE 在开机时也必须驱动为高，请告知，
 *       需要改成「周期性在输入/输出间切换」的方案。
 */

// 初始化电源控制：锁定供电并启动按键监测任务。
// 必须在 app_main 最早期调用，赶在用户松开开机键之前完成锁定。
void PowerControlInit(void);

#endif  // POWER_CONTROL_H
