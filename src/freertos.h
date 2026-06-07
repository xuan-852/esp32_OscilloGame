#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void initTasks();

// Web 控制接口
extern volatile int web_enc_delta;
extern volatile bool web_btn_pressed;
extern volatile int web_game_dir; // -1: 无, 0:上, 1:下, 2:左, 3:右


#ifdef __cplusplus
}
#endif