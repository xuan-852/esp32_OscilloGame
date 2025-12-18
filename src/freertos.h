#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void initTasks();

// Web Control Interface
extern volatile int web_enc_delta;
extern volatile bool web_btn_pressed;
extern volatile int web_game_dir; // -1: None, 0:UP, 1:DOWN, 2:LEFT, 3:RIGHT


#ifdef __cplusplus
}
#endif