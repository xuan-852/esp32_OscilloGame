#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void initTasks();

// AI chat 时暂停/恢复非必要任务
void suspendNonessentialTasks();
void suspendNonessentialTasksKeepGUI();  // 暂停但不挂起 GUI 任务（保持渲染）
void resumeNonessentialTasks();

// DMA 外设释放/恢复（用于 SSL 连接前释放 BLE/SD_MMC 占用的 DMA 连续内存）
void deinitHardwareDMA();
void reinitHardwareDMA();

// Web 控制接口
extern volatile int web_enc_delta;
extern volatile bool web_btn_pressed;
extern volatile int web_game_dir; // -1: 无, 0:上, 1:下, 2:左, 3:右


#ifdef __cplusplus
}
#endif