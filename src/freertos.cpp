#include "freertos.h"
#include "pins.h"
#include "vector_draw.h"
#include "DACoutput.h"
#include "FS.h"
#include "SD_MMC.h"
#include "web_server.h"
#include <Arduino.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <BleMouse.h>
#include <BLEDevice.h>

// 来自 main.cpp 的外部变量
extern volatile int32_t encoderValue;

// 任务句柄
static TaskHandle_t s_serialOutputTaskHandle = nullptr;
static TaskHandle_t s_guiTaskHandle = nullptr;
static TaskHandle_t s_joystickCheckTaskHandle = nullptr;

// --- 摇杆状态 ---
static bool is_joystick_connected = true;
static BleMouse* bleMouse = nullptr;

// --- 音乐播放器变量 ---
static std::vector<std::string> music_files;
static int music_file_count = 0;
static bool is_playing = false;
static File audioFile;
static uint16_t music_channels = 2; // 1: 单声道, 2: 立体声

// --- 视频播放器变量 ---
static std::vector<std::string> video_files;
static int video_file_count = 0;
static bool is_video_playing = false;
static File videoFile;
static uint16_t video_channels = 4; // 应该是 4


// --- 贪吃蛇游戏定义 ---
#define SNAKE_GRID_SIZE 20
#define SNAKE_MAX_LEN 100
typedef struct {
    int8_t x;
    int8_t y;
} Point;

static Point snake_body[SNAKE_MAX_LEN];
static int snake_len = 3;
static Point snake_food;
static int snake_dir = 3; // 0:上, 1:下, 2:左, 3:右
static int game_over = 0;
static uint32_t last_game_tick = 0;
static int game_score = 0;
static int8_t Game_Input_Dir = 3;

// --- 打砖块游戏定义 ---
#define BRK_PADDLE_W 400
#define BRK_PADDLE_H 50
#define BRK_BALL_R 30
#define BRK_ROWS 5
#define BRK_COLS 8
#define BRK_BRICK_W (2048 / BRK_COLS)
#define BRK_BRICK_H 125

typedef struct {
    int32_t x, y;
    int32_t vx, vy;
} BrkBall;

typedef struct {
    int32_t x;
} BrkPaddle;

static uint8_t brk_bricks[BRK_ROWS][BRK_COLS];
static BrkBall brk_ball;
static BrkPaddle brk_paddle;
static int brk_score = 0;
static int brk_lives = 3;
static int brk_game_over = 0;
static int brk_brick_count = 0;

// --- Flappy 游戏定义 ---
#define FLP_GRAVITY 2
#define FLP_JUMP_FORCE 35
#define FLP_SPEED 15
#define FLP_GAP_H 550
#define FLP_OBSTACLE_W 150
#define FLP_OBSTACLE_SPACING 800
#define FLP_PLAYER_X 500
#define FLP_PLAYER_R 40
#define FLP_MAX_OBSTACLES 3

typedef struct {
    int32_t y;
    int32_t vy;
} FlpPlayer;

typedef struct {
    int32_t x;
    int32_t gap_y; // 间隙中心
    int active;
    int passed;
} FlpObstacle;

static FlpPlayer flp_player;
static FlpObstacle flp_obstacles[FLP_MAX_OBSTACLES];
static int flp_score = 0;
static int flp_game_over = 0;

// --- 赛车游戏定义 ---
#define RACE_CAR_W 150
#define RACE_CAR_H 250
#define RACE_OBSTACLE_W 150
#define RACE_OBSTACLE_H 150
#define RACE_SPEED 20
#define RACE_MAX_OBSTACLES 5

typedef struct {
    int32_t x;
} RaceCar;

typedef struct {
    int32_t x, y;
    int active;
    int passed;
} RaceObstacle;

static RaceCar race_car;
static RaceObstacle race_obstacles[RACE_MAX_OBSTACLES];
static int race_score = 0;
static int race_game_over = 0;

// --- RunTiny 游戏定义 ---
#define RUN_GROUND_Y 250
#define RUN_PLAYER_X 400
#define RUN_PLAYER_W 100
#define RUN_PLAYER_H 150
#define RUN_JUMP_FORCE 30
#define RUN_GRAVITY 2
#define RUN_SPEED 20
#define RUN_OBSTACLE_W 75
#define RUN_OBSTACLE_H 100
#define RUN_MAX_OBSTACLES 3

typedef struct {
    int32_t y;
    int32_t vy;
    int jumping;
} RunPlayer;

typedef struct {
    int32_t x;
    int active;
    int passed;
} RunObstacle;

static RunPlayer run_player;
static RunObstacle run_obstacles[RUN_MAX_OBSTACLES];
static int run_score = 0;
static int run_game_over = 0;

// --- 坦克游戏变量 ---
#define TANK_MAX_BULLETS 5
#define TANK_SPEED 15.0f
#define TANK_TURN_SPEED 0.15f
#define TANK_BULLET_SPEED 30.0f

typedef struct {
    float x, y;
    float angle; // 弧度
} Tank;

typedef struct {
    float x, y;
    float vx, vy;
    bool active;
    int bounce_count;
} Bullet;

static Tank my_tank;
static Bullet tank_bullets[TANK_MAX_BULLETS];
static int tank_game_over = 0;
static unsigned long last_fire_time = 0;

// 触摸引脚
#define TOUCH_UP    4
#define TOUCH_DOWN  6
#define TOUCH_LEFT  5
#define TOUCH_RIGHT 7
#define TOUCH_THRESHOLD 35000 

// 任务函数
static void serialOutputTask(void* pvParameters);
static void guiTask(void* pvParameters);
static void joystickCheckTask(void* pvParameters);

void initTasks() {
  // 创建摇杆检查任务
  if (!s_joystickCheckTaskHandle) {
    xTaskCreatePinnedToCore(
      joystickCheckTask,
      "JoystickCheckTask",
      2048,
      nullptr,
      1,
      &s_joystickCheckTaskHandle,
      1 // 核心 1
    );
  }

  // 创建串口输出任务
  if (!s_serialOutputTaskHandle) {
    xTaskCreatePinnedToCore(
      serialOutputTask,
      "SerialOutputTask",
      2048,
      nullptr,
      1,
      &s_serialOutputTaskHandle,
      1 // 核心 1
    );
  }

  // 创建 GUI 任务
  if (!s_guiTaskHandle) {
    xTaskCreatePinnedToCore(
      guiTask,
      "GuiTask",
      4096, // GUI 需要更大的堆栈
      nullptr,
      1,
      &s_guiTaskHandle,
      1 // 核心 1
    );
  }

  // 在核心 0 上初始化 Web 服务器
  initWebServer();
}

#include "network_manager.h"

// --- GUI 逻辑 ---

enum UI_State {
    UI_MENU_MAIN,
    UI_MENU_GAMES,
    UI_MENU_MUSIC,
    UI_MUSIC_PLAYER,
    UI_MENU_VIDEO,
    UI_VIDEO_PLAYER,
    UI_MENU_ONLINE, // 新增在线模式
    UI_GAME_JOY,    // 新增游戏手柄模式
    UI_SNAKE,
    UI_BREAKOUT,
    UI_FLAPPY,
    UI_RACING,
    UI_RUNTINY,
    UI_TANK, // 新增坦克游戏
    UI_ABOUT
};

static const char* main_menu_items[] = {
    "Music",
    "Video",
    "Games",
    "Online", 
    "Game Joy", // 新增项目
    "Settings",
    "About"
};
static const int main_menu_count = 7;

static const char* games_menu_items[] = {
    "Snake",
    "Breakout",
    "Flappy",
    "Racing",
    "RunTiny",
    "Tank", // 新增游戏
    "Back"
};
static const int games_menu_count = 7;

// --- 贪吃蛇游戏逻辑 ---
void Init_Snake_Game(void) {
    snake_len = 3;
    snake_body[0].x = 10; snake_body[0].y = 10;
    snake_body[1].x = 10; snake_body[1].y = 9;
    snake_body[2].x = 10; snake_body[2].y = 8;
    
    snake_dir = 3; // 右
    Game_Input_Dir = 3;
    
    // 随机食物
    snake_food.x = rand() % SNAKE_GRID_SIZE;
    snake_food.y = rand() % SNAKE_GRID_SIZE;
    
    game_over = 0;
    game_score = 0;
    last_game_tick = millis();
}

void Update_Snake_Game(void) {
    if(game_over) return;
    
    // Update Direction from Input
    // Prevent 180 degree turns
    if(Game_Input_Dir == 0 && snake_dir != 1) snake_dir = 0;
    if(Game_Input_Dir == 1 && snake_dir != 0) snake_dir = 1;
    if(Game_Input_Dir == 2 && snake_dir != 3) snake_dir = 2;
    if(Game_Input_Dir == 3 && snake_dir != 2) snake_dir = 3;
    
    if(millis() - last_game_tick > 200) { // 200ms 速度
        last_game_tick = millis();
        
        // 移动身体
        for(int i=snake_len-1; i>0; i--) {
            snake_body[i] = snake_body[i-1];
        }
        
        // 移动头部
        if(snake_dir == 0) snake_body[0].y++; // 上
        if(snake_dir == 1) snake_body[0].y--; // 下
        if(snake_dir == 2) snake_body[0].x--; // 左
        if(snake_dir == 3) snake_body[0].x++; // 右
        
        // 检查墙壁碰撞
        if(snake_body[0].x < 0 || snake_body[0].x >= SNAKE_GRID_SIZE ||
           snake_body[0].y < 0 || snake_body[0].y >= SNAKE_GRID_SIZE) {
            game_over = 1;
        }
        
        // 检查自身碰撞
        for(int i=1; i<snake_len; i++) {
            if(snake_body[0].x == snake_body[i].x && snake_body[0].y == snake_body[i].y) {
                game_over = 1;
            }
        }
        
        // 检查食物
        if(snake_body[0].x == snake_food.x && snake_body[0].y == snake_food.y) {
            if(snake_len < SNAKE_MAX_LEN) {
                snake_len++;
                game_score += 10;
            }
            // 新食物
            snake_food.x = rand() % SNAKE_GRID_SIZE;
            snake_food.y = rand() % SNAKE_GRID_SIZE;
        }
    }
}

// --- 打砖块游戏逻辑 ---
void Init_Breakout_Game(void) {
    // 重置挡板
    brk_paddle.x = 1024 - (BRK_PADDLE_W / 2);

    // 重置球
    brk_ball.x = 1024;
    brk_ball.y = 750;
    brk_ball.vx = 15; 
    brk_ball.vy = 15;

    // 重置砖块
    brk_brick_count = 0;
    for(int r=0; r<BRK_ROWS; r++) {
        for(int c=0; c<BRK_COLS; c++) {
            brk_bricks[r][c] = 1;
            brk_brick_count++;
        }
    }

    brk_score = 0;
    brk_lives = 3;
    brk_game_over = 0;
}

void Update_Breakout_Game(int16_t encoder_delta) {
    if(brk_game_over) return;

    // 更新挡板
    brk_paddle.x += encoder_delta * 50; // 灵敏度
    if(brk_paddle.x < 0) brk_paddle.x = 0;
    if(brk_paddle.x > 2048 - BRK_PADDLE_W) brk_paddle.x = 2048 - BRK_PADDLE_W;

    // 更新球
    brk_ball.x += brk_ball.vx;
    brk_ball.y += brk_ball.vy;

    // 墙壁碰撞 (左/右)
    if(brk_ball.x <= 0) {
        brk_ball.x = 0;
        brk_ball.vx = -brk_ball.vx;
    }
    if(brk_ball.x >= 2048) {
        brk_ball.x = 2048;
        brk_ball.vx = -brk_ball.vx;
    }
    
    // 顶部墙壁
    if(brk_ball.y >= 2048) {
        brk_ball.y = 2048;
        brk_ball.vy = -brk_ball.vy;
    }

    // 挡板碰撞 (底部)
    int paddle_y = 100;
    int paddle_top = paddle_y + BRK_PADDLE_H;
    
    // 仅在向下移动时检查碰撞
    if(brk_ball.vy < 0) {
        if(brk_ball.y <= paddle_top + BRK_BALL_R && brk_ball.y >= 25) {
            // 检查 X 范围
            if(brk_ball.x >= brk_paddle.x - BRK_BALL_R && brk_ball.x <= brk_paddle.x + BRK_PADDLE_W + BRK_BALL_R) {
                // 击中
                brk_ball.vy = abs(brk_ball.vy); // 强制向上
                
                // 防穿透：将球推到表面
                brk_ball.y = paddle_top + BRK_BALL_R + 1;
                
                // 添加一些挡板速度影响 (可选)
                // brk_ball.vx += encoder_delta * 2;
            }
        }
    }

    // 底部墙壁 (死亡)
    if(brk_ball.y < 0) {
        brk_lives--;
        if(brk_lives <= 0) {
            brk_game_over = 1; // 输
        } else {
            // 重置球
            brk_ball.x = 1024;
            brk_ball.y = 750;
            brk_ball.vx = 15;
            brk_ball.vy = 15;
        }
    }

    // 砖块碰撞
    int brick_start_y = 1500;
    
    // 优化：仅在球位于砖块区域时检查
    if(brk_ball.y >= brick_start_y && brk_ball.y <= brick_start_y + (BRK_ROWS * BRK_BRICK_H)) {
        int r = (brk_ball.y - brick_start_y) / BRK_BRICK_H;
        int c = brk_ball.x / BRK_BRICK_W;
        
        if(r >= 0 && r < BRK_ROWS && c >= 0 && c < BRK_COLS) {
            if(brk_bricks[r][c]) {
                brk_bricks[r][c] = 0;
                brk_score += 10;
                brk_brick_count--;
                
                // 简单反弹 (反转 Y) - 可以改进
                brk_ball.vy = -brk_ball.vy;
                
                if(brk_brick_count <= 0) {
                    brk_game_over = 2; // 赢
                }
            }
        }
    }
}

// --- Flappy 游戏逻辑 ---
void Init_Flappy_Game(void) {
    flp_player.y = 1024;
    flp_player.vy = 0;
    
    flp_score = 0;
    flp_game_over = 0;
    
    // 初始化障碍物
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].active = 1;
        flp_obstacles[i].x = 2048 + 250 + (i * FLP_OBSTACLE_SPACING);
        flp_obstacles[i].gap_y = 500 + (rand() % 1048); // 500 到 1548
        flp_obstacles[i].passed = 0;
    }
}

void Update_Flappy_Game(int jump_requested) {
    if(flp_game_over) return;
    
    // 物理
    if(jump_requested) {
        flp_player.vy = FLP_JUMP_FORCE;
    }
    
    flp_player.y += flp_player.vy;
    flp_player.vy -= FLP_GRAVITY;
    
    // 限制速度
    if(flp_player.vy < -25) flp_player.vy = -25;
    if(flp_player.vy > 25) flp_player.vy = 25;
    
    // 地板/天花板碰撞
    if(flp_player.y < FLP_PLAYER_R) {
        flp_player.y = FLP_PLAYER_R;
        flp_game_over = 1;
    }
    if(flp_player.y > 2048 - FLP_PLAYER_R) {
        flp_player.y = 2048 - FLP_PLAYER_R;
        flp_player.vy = 0;
    }
    
    // 障碍物
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].x -= FLP_SPEED;
        
        // 回收
        if(flp_obstacles[i].x < -FLP_OBSTACLE_W) {
            // 找到最大 x 以放置在其后
            int max_x = 0;
            for(int j=0; j<FLP_MAX_OBSTACLES; j++) {
                if(flp_obstacles[j].x > max_x) max_x = flp_obstacles[j].x;
            }
            flp_obstacles[i].x = max_x + FLP_OBSTACLE_SPACING;
            flp_obstacles[i].gap_y = 500 + (rand() % 1048);
            flp_obstacles[i].passed = 0;
        }
        
        // 碰撞
        int ox = flp_obstacles[i].x;
        int ow = FLP_OBSTACLE_W;
        int gap_top = flp_obstacles[i].gap_y + FLP_GAP_H/2;
        int gap_bot = flp_obstacles[i].gap_y - FLP_GAP_H/2;
        
        // 水平检查
        if(FLP_PLAYER_X + FLP_PLAYER_R > ox && FLP_PLAYER_X - FLP_PLAYER_R < ox + ow) {
            // 垂直检查 (击中顶部或底部)
            if(flp_player.y + FLP_PLAYER_R > gap_top || flp_player.y - FLP_PLAYER_R < gap_bot) {
                flp_game_over = 1;
            }
        }
        
        // 得分
        if(!flp_obstacles[i].passed && flp_obstacles[i].x + ow < FLP_PLAYER_X - FLP_PLAYER_R) {
            flp_score++;
            flp_obstacles[i].passed = 1;
        }
    }
}

// --- 赛车游戏逻辑 ---
void Init_Racing_Game(void) {
    race_car.x = 1024 - (RACE_CAR_W / 2);
    race_score = 0;
    race_game_over = 0;
    
    // 初始化障碍物
    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].active = 1;
        race_obstacles[i].x = rand() % (2048 - RACE_OBSTACLE_W);
        race_obstacles[i].y = 2048 + (i * 750); // 间距
        race_obstacles[i].passed = 0;
    }
}

void Update_Racing_Game(int16_t encoder_delta) {
    if(race_game_over) return;
    
    // 移动赛车
    race_car.x += encoder_delta * 75; // 灵敏度
    if(race_car.x < 0) race_car.x = 0;
    if(race_car.x > 2048 - RACE_CAR_W) race_car.x = 2048 - RACE_CAR_W;
    
    // 移动障碍物
    int current_speed = RACE_SPEED + (race_score * 0.25);
    if(current_speed > 75) current_speed = 75; // 限制速度

    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].y -= current_speed;
        
        // 回收
        if(race_obstacles[i].y < -RACE_OBSTACLE_H) {
            // 找到最大 y
            int max_y = 0;
            for(int j=0; j<RACE_MAX_OBSTACLES; j++) {
                if(race_obstacles[j].y > max_y) max_y = race_obstacles[j].y;
            }
            race_obstacles[i].y = max_y + 750; // 间距
            race_obstacles[i].x = rand() % (2048 - RACE_OBSTACLE_W);
            race_obstacles[i].passed = 0;
        }
        
        // 碰撞
        // 简单 AABB
        if(race_car.x < race_obstacles[i].x + RACE_OBSTACLE_W &&
           race_car.x + RACE_CAR_W > race_obstacles[i].x &&
           100 < race_obstacles[i].y + RACE_OBSTACLE_H &&
           100 + RACE_CAR_H > race_obstacles[i].y) {
            race_game_over = 1;
        }
        
        // 得分
        if(!race_obstacles[i].passed && race_obstacles[i].y + RACE_OBSTACLE_H < 100) {
            race_score++;
            race_obstacles[i].passed = 1;
        }
    }
}

// --- RunTiny 游戏逻辑 ---
void Init_RunTiny_Game(void) {
    run_player.y = RUN_GROUND_Y;
    run_player.vy = 0;
    run_player.jumping = 0;
    
    run_score = 0;
    run_game_over = 0;
    
    // 初始化障碍物
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].active = 1;
        run_obstacles[i].x = 2048 + 500 + (i * 1000) + (rand() % 400); // 带抖动的间距
        run_obstacles[i].passed = 0;
    }
}

void Update_RunTiny_Game(int jump_requested) {
    if(run_game_over) return;
    
    // 物理
    if(jump_requested && !run_player.jumping) {
        run_player.vy = RUN_JUMP_FORCE;
        run_player.jumping = 1;
    }
    
    run_player.y += run_player.vy;
    
    if(run_player.y > RUN_GROUND_Y) {
        run_player.vy -= RUN_GRAVITY;
    } else {
        run_player.y = RUN_GROUND_Y;
        run_player.vy = 0;
        run_player.jumping = 0;
    }
    
    // 移动障碍物
    int current_speed = RUN_SPEED + (run_score * 0.1);
    if(current_speed > 50) current_speed = 50;
    
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].x -= current_speed;
        
        // 回收
        if(run_obstacles[i].x < -RUN_OBSTACLE_W) {
            // 找到最大 x
            int max_x = 0;
            for(int j=0; j<RUN_MAX_OBSTACLES; j++) {
                if(run_obstacles[j].x > max_x) max_x = run_obstacles[j].x;
            }
            // 更随机的间距: 600 到 1350
            run_obstacles[i].x = max_x + 600 + (rand() % 750);
            run_obstacles[i].passed = 0;
        }
        
        // 碰撞
        if(run_obstacles[i].x < RUN_PLAYER_X + RUN_PLAYER_W &&
           run_obstacles[i].x + RUN_OBSTACLE_W > RUN_PLAYER_X &&
           run_player.y < RUN_GROUND_Y + RUN_OBSTACLE_H) { // 简单高度检查
            run_game_over = 1;
        }
        
        // 得分
        if(!run_obstacles[i].passed && run_obstacles[i].x + RUN_OBSTACLE_W < RUN_PLAYER_X) {
            run_score++;
            run_obstacles[i].passed = 1;
        }
    }
}

// --- 坦克游戏逻辑 ---

// --- 坦克游戏地图 ---
#define TANK_MAP_SIZE 6
typedef struct {
    int x, y, w, h;
    int type; // 0: 墙 (反弹), 1: 水 (坦克阻挡, 子弹穿过)
} TankMapObject;

static TankMapObject tank_map[TANK_MAP_SIZE] = {
    {500, 500, 100, 400, 0},   // 墙 1
    {1400, 1200, 100, 400, 0}, // 墙 2
    {800, 1000, 400, 100, 0},  // 墙 3
    {200, 1500, 300, 300, 1},  // 水 1
    {1500, 200, 300, 300, 1},  // 水 2
    {900, 400, 200, 200, 1}    // 水 3
};

bool Check_Tank_Collision(float x, float y) {
    // 检查地图对象
    for(int i=0; i<TANK_MAP_SIZE; i++) {
        // 简单 AABB vs 点 (坦克中心)
        if(x >= tank_map[i].x && x <= tank_map[i].x + tank_map[i].w &&
           y >= tank_map[i].y && y <= tank_map[i].y + tank_map[i].h) {
            return true; // 碰撞
        }
    }
    return false;
}

// 如果反弹则返回 true
bool Check_Bullet_Collision(float &x, float &y, float &vx, float &vy, int &bounce_count) {
    bool bounced = false;
    
    // 屏幕边界
    if(x < 0) { x = 0; vx = -vx; bounced = true; }
    if(x > 2047) { x = 2047; vx = -vx; bounced = true; }
    if(y < 0) { y = 0; vy = -vy; bounced = true; }
    if(y > 2047) { y = 2047; vy = -vy; bounced = true; }
    
    // 地图墙壁 (类型 0)
    for(int i=0; i<TANK_MAP_SIZE; i++) {
        if(tank_map[i].type == 0) { // 墙
            if(x >= tank_map[i].x && x <= tank_map[i].x + tank_map[i].w &&
               y >= tank_map[i].y && y <= tank_map[i].y + tank_map[i].h) {
                
                // 确定碰撞侧以正确反射
                // 上一个位置是 (x-vx, y-vy)
                float prev_x = x - vx;
                float prev_y = y - vy;
                
                // 检查是否在 X 轴外
                bool outside_x = (prev_x < tank_map[i].x || prev_x > tank_map[i].x + tank_map[i].w);
                // 检查是否在 Y 轴外
                bool outside_y = (prev_y < tank_map[i].y || prev_y > tank_map[i].y + tank_map[i].h);
                
                if(outside_x) vx = -vx;
                if(outside_y) vy = -vy;
                
                // 稍微推出以防止粘连
                x += vx; 
                y += vy;
                
                bounced = true;
            }
        }
    }
    
    if(bounced) {
        bounce_count++;
    }
    
    return bounced;
}

void Generate_Random_Map(uint32_t seed) {
    srand(seed);
    for(int i=0; i<TANK_MAP_SIZE; i++) {
        // 随机化类型 (0 或 1)
        tank_map[i].type = rand() % 2;
        
        // 随机化位置 (远离极端角落以允许生成)
        // 屏幕 0-2047.
        // 墙/水大小 ~100-400.
        tank_map[i].w = 100 + (rand() % 300);
        tank_map[i].h = 100 + (rand() % 300);
        
        // 确保它们稍微居中或分散
        tank_map[i].x = 200 + (rand() % 1600);
        tank_map[i].y = 200 + (rand() % 1600);
    }
}

void Init_Tank_Game(uint32_t seed, bool is_initiator) {
    // 检查连接
    if(Network_Manager::getState() != NET_CONNECTED && Network_Manager::getState() != NET_IN_GAME) {
        tank_game_over = 2; // 未连接
        return;
    }

    // 如果我们是发起者 (尚未在游戏中), 发送开始游戏
    if(is_initiator) {
        if(seed == 0) seed = millis();
        Network_Manager::startGame(1, seed); 
    }
    
    Generate_Random_Map(seed);

    // 生成点
    if(is_initiator) {
        my_tank.x = 200;
        my_tank.y = 200;
        my_tank.angle = -1.5707f; // 朝上
    } else {
        my_tank.x = 1800;
        my_tank.y = 1800;
        my_tank.angle = 1.5707f; // 朝下
    }
    
    // 检查生成有效性 (微移)
    int attempts = 0;
    while(Check_Tank_Collision(my_tank.x, my_tank.y) && attempts < 100) {
        my_tank.x += (rand() % 200) - 100;
        my_tank.y += (rand() % 200) - 100;
        
        // 保持在边界内
        if(my_tank.x < 50) my_tank.x = 50;
        if(my_tank.x > 1997) my_tank.x = 1997;
        if(my_tank.y < 50) my_tank.y = 50;
        if(my_tank.y > 1997) my_tank.y = 1997;
        
        attempts++;
    }

    for(int i=0; i<TANK_MAX_BULLETS; i++) {
        tank_bullets[i].active = false;
    }
    tank_game_over = 0;
    Network_Manager::clearRemoteGameEnded();
}

void Update_Tank_Game(void) {
    if(tank_game_over) return;

    // 检查远程退出
    uint8_t reason;
    if(Network_Manager::isRemoteGameEnded(&reason)) {
        if(reason == 1) {
             tank_game_over = 4; // 你赢了 (对手死亡)
        } else {
             tank_game_over = 3; // 对手离开
        }
        return;
    }

    // 输入
    // 左摇杆 Y (JOY1_Y) - 前进/后退
    // 右摇杆 X (JOY2_X) - 旋转
    // 按钮 A (JOY_A) - 开火
    
    int joy1_y = analogRead(JOY1_Y); // 0-4095
    int joy2_x = analogRead(JOY2_X); // 0-4095
    int btn_a = !digitalRead(JOY_A);  // 松开 = 1 (HIGH), 按下 = 0 (LOW)
    
    // 死区和映射
    float speed = 0;
    if(abs(joy1_y - 2048) > 300) {
        // 假设向上 (低值) -> 前进
        // 2048 - val: 如果 val < 2048 (向上) 则为正
        speed = (2048 - joy1_y) / 2048.0f * TANK_SPEED;
    }
    
    float turn = 0;
    if(abs(joy2_x - 2048) > 300) {
        // 假设向右 (高值) -> 顺时针
        turn = (joy2_x - 2048) / 2048.0f * TANK_TURN_SPEED;
    }
    
    // 更新坦克
    my_tank.angle += turn;
    float next_x = my_tank.x + speed * cos(my_tank.angle);
    float next_y = my_tank.y + speed * sin(my_tank.angle);
    
    // 检查碰撞 (地图和边界)
    if(next_x >= 50 && next_x <= 1997 && next_y >= 50 && next_y <= 1997 && !Check_Tank_Collision(next_x, next_y)) {
        my_tank.x = next_x;
        my_tank.y = next_y;
    }
    
    static int bullet_frames[TANK_MAX_BULLETS];

    // 开火 (按下 = LOW)
    if(btn_a == LOW && millis() - last_fire_time > 250) { 
        last_fire_time = millis();
        for(int i=0; i<TANK_MAX_BULLETS; i++) {
            if(!tank_bullets[i].active) {
                tank_bullets[i].active = true;
                // 在炮管尖端生成 (约 100 单位)
                tank_bullets[i].x = my_tank.x + 100 * cos(my_tank.angle); 
                tank_bullets[i].y = my_tank.y + 100 * sin(my_tank.angle);
                tank_bullets[i].vx = TANK_BULLET_SPEED * cos(my_tank.angle);
                tank_bullets[i].vy = TANK_BULLET_SPEED * sin(my_tank.angle);
                tank_bullets[i].bounce_count = 0;
                bullet_frames[i] = 0;
                break;
            }
        }
    }
    
    // 更新子弹
    for(int i=0; i<TANK_MAX_BULLETS; i++) {
        if(tank_bullets[i].active) {
            tank_bullets[i].x += tank_bullets[i].vx;
            tank_bullets[i].y += tank_bullets[i].vy;
            
            // 检查碰撞 (反弹)
            Check_Bullet_Collision(tank_bullets[i].x, tank_bullets[i].y, tank_bullets[i].vx, tank_bullets[i].vy, tank_bullets[i].bounce_count);
            
            if(tank_bullets[i].bounce_count > 5) {
                tank_bullets[i].active = false;
            }

            bullet_frames[i]++;
            if(bullet_frames[i] > 300) { // 12 秒寿命
                tank_bullets[i].active = false;
            }
            
            // 检查自身击中 (自杀)
            float dx = tank_bullets[i].x - my_tank.x;
            float dy = tank_bullets[i].y - my_tank.y;
            if(dx*dx + dy*dy < 60*60) { // 坦克半径约 60 (从 30 增加)
                tank_game_over = 1; // 我死了
            }
        }
    }
    
    // 检查远程子弹击中我
    TankData remoteData;
    if(Network_Manager::getRemoteGameData(&remoteData)) {
        for(int i=0; i<remoteData.bullet_count; i++) {
             float dx = remoteData.bullets[i].x - my_tank.x;
             float dy = remoteData.bullets[i].y - my_tank.y;
             if(dx*dx + dy*dy < 60*60) { // 坦克半径约 60 (从 30 增加)
                 tank_game_over = 1; // 我死了
             }
        }
    }
    
    if(tank_game_over == 1) {
        Network_Manager::endGame(1); // 通知对等方我输了 (原因 1 = 死亡)
    }

    // 网络同步 (约 100Hz, 每帧调用一次, 主循环为 25Hz, 但我们想要更快?)
    // 主循环延迟为 40ms (25Hz). 我们应该每帧发送.
    TankData data;
    data.x = my_tank.x;
    data.y = my_tank.y;
    data.angle = my_tank.angle;
    data.bullet_count = 0;
    for(int i=0; i<TANK_MAX_BULLETS; i++) {
        if(tank_bullets[i].active) {
            if(data.bullet_count < 5) {
                data.bullets[data.bullet_count].x = tank_bullets[i].x;
                data.bullets[data.bullet_count].y = tank_bullets[i].y;
                data.bullet_count++;
            }
        }
    }
    Network_Manager::sendGameData(data);
}

// --- 音乐逻辑 ---
void Scan_Music_Files() {
    music_files.clear();
    File root = SD_MMC.open("/music");
    if(!root || !root.isDirectory()){
        Serial.println("Failed to open /music directory");
        return;
    }
    File file = root.openNextFile();
    while(file){
        if(!file.isDirectory()){
            String fname = file.name();
            if(fname.endsWith(".wav") || fname.endsWith(".WAV")){
                music_files.push_back(fname.c_str());
            }
        }
        file = root.openNextFile();
    }
    music_file_count = music_files.size();
}

bool Play_Music(const char* filename) {
    String path = "/music/" + String(filename);
    audioFile = SD_MMC.open(path.c_str());
    if(!audioFile) {
        Serial.println("Failed to open audio file");
        return false;
    }
    
    // 读取 WAV 头
    uint8_t header[44];
    if(audioFile.read(header, 44) != 44) {
        Serial.println("Failed to read WAV header");
        audioFile.close();
        return false;
    }
    
    // 检查 RIFF
    if(header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
        Serial.println("Not a RIFF file");
        audioFile.close();
        return false;
    }
    
    // 获取采样率 (偏移 24, 4 字节)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    Serial.printf("Sample Rate: %d\n", sampleRate);
    
    // 获取通道数 (偏移 22, 2 字节)
    uint16_t channels = header[22] | (header[23] << 8);
    Serial.printf("Channels: %d\n", channels);
    music_channels = channels;
    
    // 获取每样本位数 (偏移 34, 2 字节)
    uint16_t bitsPerSample = header[34] | (header[35] << 8);
    Serial.printf("Bits: %d\n", bitsPerSample);
    
    if(bitsPerSample != 16) {
        Serial.println("Only 16-bit WAV supported");
        audioFile.close();
        return false;
    }
    
    Serial.printf("File Size: %d\n", audioFile.size());
    
    is_playing = true;
    Set_Player_Mode(1); // 1=音频
    setDACFreq(sampleRate);
    return true;
}

void Stop_Music() {
    is_playing = false;
    if(audioFile) audioFile.close();
    Set_Player_Mode(0); // 0=矢量
}

// --- 视频逻辑 ---
void Scan_Video_Files() {
    video_files.clear();
    File root = SD_MMC.open("/video");
    if(!root || !root.isDirectory()){
        Serial.println("Failed to open /video directory");
        return;
    }
    File file = root.openNextFile();
    while(file){
        if(!file.isDirectory()){
            String fname = file.name();
            if(fname.endsWith(".wav") || fname.endsWith(".WAV")){
                video_files.push_back(fname.c_str());
            }
        }
        file = root.openNextFile();
    }
    video_file_count = video_files.size();
}

bool Play_Video(const char* filename) {
    String path = "/video/" + String(filename);
    videoFile = SD_MMC.open(path.c_str());
    if(!videoFile) {
        Serial.println("Failed to open video file");
        return false;
    }
    
    // 读取 WAV 头
    uint8_t header[44];
    if(videoFile.read(header, 44) != 44) {
        Serial.println("Failed to read WAV header");
        videoFile.close();
        return false;
    }
    
    // 检查 RIFF
    if(header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
        Serial.println("Not a RIFF file");
        videoFile.close();
        return false;
    }
    
    // 获取采样率 (偏移 24, 4 字节)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    Serial.printf("Sample Rate: %d\n", sampleRate);
    
    // 获取通道数 (偏移 22, 2 字节)
    uint16_t channels = header[22] | (header[23] << 8);
    Serial.printf("Channels: %d\n", channels);
    video_channels = channels;
    
    // 获取每样本位数 (偏移 34, 2 字节)
    uint16_t bitsPerSample = header[34] | (header[35] << 8);
    Serial.printf("Bits: %d\n", bitsPerSample);
    
    if(bitsPerSample != 16) {
        Serial.println("Only 16-bit WAV supported");
        videoFile.close();
        return false;
    }
    
    if(channels != 4) {
        Serial.println("Only 4-channel WAV supported for Video");
        videoFile.close();
        return false;
    }
    
    Serial.printf("File Size: %d\n", videoFile.size());
    
    is_video_playing = true;
    Set_Player_Mode(2); // 2=视频
    setDACFreq(sampleRate);
    return true;
}

void Stop_Video() {
    is_video_playing = false;
    if(videoFile) videoFile.close();
    Set_Player_Mode(0); // 0=矢量
}

static void guiTask(void* pvParameters) {
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(500));

    // 校准
    int touch_base_up = 0;
    int touch_base_down = 0;
    int touch_base_left = 0;
    int touch_base_right = 0;
    
    // 采样
    for(int i=0; i<20; i++) {
        touch_base_up += touchRead(TOUCH_UP);
        touch_base_down += touchRead(TOUCH_DOWN);
        touch_base_left += touchRead(TOUCH_LEFT);
        touch_base_right += touchRead(TOUCH_RIGHT);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    touch_base_up /= 20;
    touch_base_down /= 20;
    touch_base_left /= 20;
    touch_base_right /= 20;
    
    Serial.printf("Calibrated Touch Base: U=%d D=%d L=%d R=%d\n", 
        touch_base_up, touch_base_down, touch_base_left, touch_base_right);
    
    // 定义增量
    // 空闲 ~30000, 按下 > 70000.
    // 如果校准失败 (0), 35000 是安全的 (30000 < 35000).
    // 如果校准成功 (30000), 阈值 65000. 按下 (70000+) > 65000.
    // 用户反馈触摸变化量只有10000以上，所以降低阈值
    const int TOUCH_DELTA = 5000; 

    // 初始化网络
    Network_Manager::init();

    UI_State ui_state = UI_MENU_MAIN;
    int menu_index = 0;
    int last_menu_index = -1;
    int32_t last_encoder = 0;
    
    // 按钮去抖动
    int last_btn_state = HIGH;
    unsigned long last_btn_time = 0;
    
    // 示波器状态
    int32_t osc_last_val = -999999;

    // 音乐状态
    int music_scroll = 0;
    const int visible_lines = 5;
    
    // 主菜单状态
    int main_menu_scroll = 0;
    const int main_menu_visible_lines = 5;

    // Flappy 状态
    bool last_touch_up = false;

    for (;;) {
        // 1. 处理输入
        int32_t current_encoder = encoderValue / 4;
        int16_t enc_delta = current_encoder - last_encoder;
        last_encoder = current_encoder;
        
        // Web 输入注入
        if (web_enc_delta != 0) {
            enc_delta += web_enc_delta;
            web_enc_delta = 0;
        }
        
        int btn_state = digitalRead(EN_S);
        bool btn_pressed = false;
        
        // 在释放时触发 (上升沿) 以防止保持问题
        if (btn_state == HIGH && last_btn_state == LOW && (millis() - last_btn_time > 50)) {
            btn_pressed = true;
        }
        
        if (web_btn_pressed) {
            btn_pressed = true;
            web_btn_pressed = false;
        }

        // 记录按钮变低的时间
        if (btn_state == LOW && last_btn_state == HIGH) {
            last_btn_time = millis();
        }
        last_btn_state = btn_state;

        // 贪吃蛇触摸输入
        if (ui_state == UI_SNAKE) {
            if (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA) Game_Input_Dir = 0;
            if (touchRead(TOUCH_DOWN) > touch_base_down + TOUCH_DELTA) Game_Input_Dir = 1;
            if (touchRead(TOUCH_LEFT) > touch_base_left + TOUCH_DELTA) Game_Input_Dir = 2;
            if (touchRead(TOUCH_RIGHT) > touch_base_right + TOUCH_DELTA) Game_Input_Dir = 3;
            
            if (web_game_dir != -1) {
                Game_Input_Dir = web_game_dir;
                web_game_dir = -1;
            }
        }

        // 2. 状态机
        bool rebuild = false;

        // 全局游戏请求检查 (用于非阻塞状态)
        uint8_t reqGameId;
        uint32_t reqSeed;
        if(Network_Manager::hasGameRequest(&reqGameId, &reqSeed)) {
            Network_Manager::clearGameRequest();
            if(reqGameId == 1) { // 坦克大战
                ui_state = UI_TANK;
                Init_Tank_Game(reqSeed, false); // 响应者
                rebuild = true;
            }
        }

        if (ui_state == UI_MENU_MAIN) {
            // 导航
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = main_menu_count - 1;
                if (menu_index >= main_menu_count) menu_index = 0;
                rebuild = true;
            }
            
            // 选择
            if (btn_pressed) {
                if (menu_index == 0) {
                    ui_state = UI_MENU_MUSIC;
                    Scan_Music_Files();
                    menu_index = 0;
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 1) {
                    ui_state = UI_MENU_VIDEO;
                    Scan_Video_Files();
                    menu_index = 0;
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 2) {
                    ui_state = UI_MENU_GAMES;
                    menu_index = 0; // 重置子菜单
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 3) {
                    ui_state = UI_MENU_ONLINE;
                    Network_Manager::startDiscovery();
                    menu_index = 0;
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 4) {
                    ui_state = UI_GAME_JOY;
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 6) {
                    ui_state = UI_ABOUT;
                    last_menu_index = -1; // 确保重绘
                    continue;
                }
                rebuild = true;
            }
            
            // 渲染主菜单
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047); // 全屏边框
                
                int start_y = 1600; 
                int spacing = 400;
                int scale = 40; 
                
                // 计算滚动
                if(menu_index < main_menu_scroll) main_menu_scroll = menu_index;
                if(menu_index >= main_menu_scroll + main_menu_visible_lines) main_menu_scroll = menu_index - main_menu_visible_lines + 1;
                
                String status = "MENU: MAIN\n";
                
                for (int i = 0; i < main_menu_visible_lines; i++) {
                    int item_idx = main_menu_scroll + i;
                    int y = start_y - (i * spacing);
                    
                    if(item_idx < main_menu_count) {
                        if (item_idx == menu_index) {
                            DRAW_AddString(">", 0, 50, y, scale, scale);
                            status += "> " + String(main_menu_items[item_idx]) + "\n";
                        } else {
                            status += "  " + String(main_menu_items[item_idx]) + "\n";
                        }
                        DRAW_AddString(main_menu_items[item_idx], 0, 250, y, scale, scale);
                    }
                }
                updateWebUIStatus(status);
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_GAME_JOY) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1;
                if(bleMouse) {
                    bleMouse->end();
                    delete bleMouse;
                    bleMouse = nullptr;
                }
                Network_Manager::enable(); // 重新启用 WiFi
                continue;
            }

            // 检查连接
            if (!is_joystick_connected) {
                 if (rebuild || last_menu_index == -1) {
                    DRAW_Clear();
                    DRAW_AddString("GAME JOY", 0, 600, 1800, 30, 30);
                    DRAW_AddString("JOYSTICK", 0, 400, 1100, 30, 30);
                    DRAW_AddString("DISCONNECTED", 0, 200, 900, 30, 30);
                    DRAW_AddString("PRESS BTN TO EXIT", 0, 300, 500, 20, 20);
                    updateWebUIStatus("GAME JOY\nJOYSTICK DISCONNECTED");
                    last_menu_index = 0;
                 }
                 // 不运行鼠标逻辑
            } else {
                if (last_menu_index == -1) {
                    Network_Manager::disable(); // 禁用 WiFi
                    vTaskDelay(pdMS_TO_TICKS(200)); 
                    
                    // 启动 BLE
                    if(bleMouse == nullptr) {
                        bleMouse = new BleMouse("ESP32 Game Joy", "Espressif", 100);
                        bleMouse->begin();
                        // 将功率提升至最大 (P9 = +9dBm)
                        BLEDevice::setPower(ESP_PWR_LVL_P9); 
                    }
                }
                
                if(!bleMouse) continue;

                // 读取摇杆
                int jx = analogRead(JOY2_X);
                int jy = analogRead(JOY2_Y);
                
                // 映射到鼠标
                // 假设 0-4095. 中心 ~2048.
                // 死区 +/- 200
                int dx = 0;
                int dy = 0;
                
                // 反转 X 轴计算
                if (abs(jx - 2048) > 200) {
                    dx = (2048 - jx) / 40; // 反转 X
                }
                if (abs(jy - 2048) > 200) {
                    dy = (jy - 2048) / 40; 
                }
                
                // 按钮逻辑
                // A = 左键单击
                static bool last_click_left = false;
                bool click_left = (digitalRead(JOY_A) == LOW);
                if(click_left != last_click_left) {
                    if(click_left) bleMouse->press(MOUSE_LEFT);
                    else bleMouse->release(MOUSE_LEFT);
                    last_click_left = click_left;
                }

                // B = 右键单击
                static bool last_click_right = false;
                bool click_right = (digitalRead(JOY_B) == LOW);
                if(click_right != last_click_right) {
                    if(click_right) bleMouse->press(MOUSE_RIGHT);
                    else bleMouse->release(MOUSE_RIGHT);
                    last_click_right = click_right;
                }

                if (bleMouse->isConnected()) {
                    if(dx != 0 || dy != 0) {
                        bleMouse->move(dx, dy); // 固定方向 (移除负号)
                    }
                }
                
                // 检查连接状态更改以重绘
                static bool last_ble_connected = false;
                bool current_ble_connected = bleMouse->isConnected();
                if(current_ble_connected != last_ble_connected) {
                    rebuild = true;
                    last_ble_connected = current_ble_connected;
                }
                
                // 渲染
                if (rebuild || last_menu_index == -1) {
                    DRAW_Clear();
                    DRAW_AddString("GAME JOY", 0, 600, 1800, 30, 30);
                    if(current_ble_connected) {
                        DRAW_AddString("BLE CONNECTED", 0, 400, 1500, 20, 20);
                    } else {
                        DRAW_AddString("WAITING BLE...", 0, 400, 1500, 20, 20);
                    }
                    
                    // 可视化摇杆
                    DRAW_AddCircle(1024, 1024, 500); // 基座
                    int vis_x = 1024 - (dx * 20);
                    int vis_y = 1024 - (dy * 20); 
                    
                    DRAW_AddCircle(vis_x, vis_y, 50); // 摇杆头
                    
                    if(click_left) DRAW_AddCircle(vis_x, vis_y, 30); // 左键反馈
                    if(click_right) DRAW_AddCircle(vis_x, vis_y, 40); // 右键反馈
                    
                    updateWebUIStatus("GAME JOY\n" + String(current_ble_connected ? "Connected" : "Waiting..."));
                    last_menu_index = 0;
                }
            }
            
        } else if (ui_state == UI_MENU_GAMES) {
            // 导航
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = games_menu_count - 1;
                if (menu_index >= games_menu_count) menu_index = 0;
                rebuild = true;
            }
            
            // 选择
            if (btn_pressed) {
                if (menu_index == 0) {
                    ui_state = UI_SNAKE;
                    Init_Snake_Game();
                    continue;
                } else if (menu_index == 1) {
                    ui_state = UI_BREAKOUT;
                    Init_Breakout_Game();
                    continue;
                } else if (menu_index == 2) {
                    ui_state = UI_FLAPPY;
                    Init_Flappy_Game();
                    continue;
                } else if (menu_index == 3) {
                    ui_state = UI_RACING;
                    Init_Racing_Game();
                    continue;
                } else if (menu_index == 4) {
                    ui_state = UI_RUNTINY;
                    Init_RunTiny_Game();
                    continue;
                } else if (menu_index == 5) {
                    ui_state = UI_TANK;
                    Init_Tank_Game(0, true); // 发起者, Seed=0 (自动)
                    continue;
                } else if (menu_index == 6) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 1; // 返回 "Games" 选项
                    last_menu_index = -1;
                    continue;
                }
                rebuild = true;
            }
            
            // 渲染游戏菜单
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047); 
                
                int start_y = 1800; 
                int spacing = 250;
                int scale = 40; 
                
                String status = "MENU: GAMES\n";

                for (int i = 0; i < games_menu_count; i++) {
                    int y = start_y - (i * spacing);
                    if (i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                        status += "> " + String(games_menu_items[i]) + "\n";
                    } else {
                        status += "  " + String(games_menu_items[i]) + "\n";
                    }
                    DRAW_AddString(games_menu_items[i], 0, 250, y, scale, scale);
                }
                updateWebUIStatus(status);
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_MENU_MUSIC) {
            // 导航
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = music_file_count; // +1 用于返回
                if (menu_index > music_file_count) menu_index = 0;
                rebuild = true;
            }
            
            // 选择
            if (btn_pressed) {
                if (menu_index == music_file_count) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 0;
                    last_menu_index = -1;
                    continue;
                } else {
                    if(Play_Music(music_files[menu_index].c_str())) {
                        ui_state = UI_MUSIC_PLAYER;
                        last_menu_index = -1;
                        continue;
                    } else {
                        // 播放失败，停留在菜单
                        // 也许显示错误？
                    }
                }
                rebuild = true;
            }
            
            // 渲染音乐菜单
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                int start_y = 1600;
                int spacing = 300;
                int scale = 30;
                
                // 计算滚动
                if(menu_index < music_scroll) music_scroll = menu_index;
                if(menu_index >= music_scroll + visible_lines) music_scroll = menu_index - visible_lines + 1;
                
                String status = "MENU: MUSIC\n";

                for(int i=0; i<visible_lines; i++) {
                    int item_idx = music_scroll + i;
                    int y = start_y - (i * spacing);
                    
                    if(item_idx <= music_file_count) {
                        if(item_idx == menu_index) {
                            DRAW_AddString(">", 0, 50, y, scale, scale);
                            if(item_idx == music_file_count) status += "> Back\n";
                            else status += "> " + String(music_files[item_idx].c_str()) + "\n";
                        } else {
                            if(item_idx == music_file_count) status += "  Back\n";
                            else status += "  " + String(music_files[item_idx].c_str()) + "\n";
                        }
                        
                        if(item_idx == music_file_count) {
                            DRAW_AddString("Back", 0, 250, y, scale, scale);
                        } else {
                            DRAW_AddString(music_files[item_idx].c_str(), 0, 250, y, scale, scale);
                        }
                    }
                }
                updateWebUIStatus(status);
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_MUSIC_PLAYER) {
            // 独占音频循环 - 阻塞模式
            // 这确保了缓冲区填充的最大 CPU 时间，并防止 UI 饥饿
            
            // 清屏一次
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                // 绘制简单的 "Playing" 指示器
                DRAW_AddString("PLAYING...", 0, 500, 1000, 20, 20);
                DRAW_Update(); 
                updateWebUIStatus("MUSIC PLAYER\nPlaying: " + String(music_files[menu_index].c_str()));
                last_menu_index = 0;
                // 移除了 "等待释放" 循环，因为我们现在在释放时进入
            }
            
            // 安全检查：如果未播放，则返回
            if (!is_playing) {
                ui_state = UI_MENU_MUSIC;
                rebuild = true;
            }

            // 进入阻塞循环
            Serial.println("Entering Music Loop");
            
            // 重置去抖动计时器
            unsigned long low_start = 0;
            
            // 在 PSRAM 中分配 64KB 临时缓冲区用于读取
            // 64KB = 65536 字节
            uint8_t *read_buf = (uint8_t*)ps_malloc(65536);
            if(read_buf == NULL) {
                Serial.println("Failed to allocate 64KB read buffer!");
                is_playing = false;
            }

            // 音量控制初始化
            static int volume = 1; // 0-256. 默认 1/256
            int32_t last_enc = encoderValue;
            
            while (is_playing && audioFile) {
                // Check for Game Request
                uint8_t reqGameId;
                uint32_t reqSeed;
                if(Network_Manager::hasGameRequest(&reqGameId, &reqSeed)) {
                    Network_Manager::clearGameRequest();
                    if(reqGameId == 1) { // Tank
                        Stop_Music();
                        ui_state = UI_TANK;
                        Init_Tank_Game(reqSeed, false);
                        rebuild = true;
                        break;
                    }
                }

                // Update Volume
                int32_t curr_enc = encoderValue;
                int delta = (curr_enc - last_enc) / 4; // Sensitivity: 4 encoder counts (1 detent) = 1 volume step
                if (delta != 0) {
                    volume += delta;
                    if (volume < 0) volume = 0;
                    if (volume > 256) volume = 256;
                    last_enc = curr_enc;
                }
                // Web Volume
                if (web_enc_delta != 0) {
                    volume += web_enc_delta;
                    if (volume < 0) volume = 0;
                    if (volume > 256) volume = 256;
                    web_enc_delta = 0;
                }

                // 1. Fill Buffer A if free
                if (Is_Buf_A_Free() && audioFile.available()) {
                     int bytesToRead = 65536;
                     if(audioFile.available() < bytesToRead) bytesToRead = audioFile.available();
                     
                     int bytesRead = audioFile.read(read_buf, bytesToRead);
                     if(bytesRead > 0) {
                         uint16_t* bufL = Get_Buf_A_L();
                         uint16_t* bufR = Get_Buf_A_R();
                         int count = 0;
                         
                         int sample_idx = 0;
                         while(sample_idx < bytesRead) {
                            int16_t l, r;
                            if(music_channels == 1) {
                                // Mono: 2 bytes
                                if(sample_idx + 1 < bytesRead) {
                                    int16_t val = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                    l = val; r = val;
                                    sample_idx += 2;
                                } else break;
                            } else {
                                // Stereo: 4 bytes
                                if(sample_idx + 3 < bytesRead) {
                                    l = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                    r = (int16_t)(read_buf[sample_idx+2] | (read_buf[sample_idx+3] << 8));
                                    sample_idx += 4;
                                } else break;
                            }
                            // Apply Volume
                            l = (int16_t)((l * volume) >> 8);
                            r = (int16_t)((r * volume) >> 8);

                            bufL[count] = (uint16_t)(l + 32768);
                            bufR[count] = (uint16_t)(r + 32768);
                            count++;
                         }
                         Mark_Buf_A_Ready(count);
                     }
                }

                // 2. Fill Buffer B if free
                if (Is_Buf_B_Free() && audioFile.available()) {
                     int bytesToRead = 65536;
                     if(audioFile.available() < bytesToRead) bytesToRead = audioFile.available();
                     
                     int bytesRead = audioFile.read(read_buf, bytesToRead);
                     if(bytesRead > 0) {
                         uint16_t* bufL = Get_Buf_B_L();
                         uint16_t* bufR = Get_Buf_B_R();
                         int count = 0;
                         
                         int sample_idx = 0;
                         while(sample_idx < bytesRead) {
                            int16_t l, r;
                            if(music_channels == 1) {
                                // Mono: 2 bytes
                                if(sample_idx + 1 < bytesRead) {
                                    int16_t val = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                    l = val; r = val;
                                    sample_idx += 2;
                                } else break;
                            } else {
                                // Stereo: 4 bytes
                                if(sample_idx + 3 < bytesRead) {
                                    l = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                    r = (int16_t)(read_buf[sample_idx+2] | (read_buf[sample_idx+3] << 8));
                                    sample_idx += 4;
                                } else break;
                            }
                            // Apply Volume
                            l = (int16_t)((l * volume) >> 8);
                            r = (int16_t)((r * volume) >> 8);

                            bufL[count] = (uint16_t)(l + 32768);
                            bufR[count] = (uint16_t)(r + 32768);
                            count++;
                         }
                         Mark_Buf_B_Ready(count);
                     }
                }
                
                // 3. Check Exit Button (Polling)
                // Check Web Exit
                if (web_btn_pressed) {
                    web_btn_pressed = false;
                    Stop_Music();
                    ui_state = UI_MENU_MUSIC;
                    rebuild = true;
                    last_menu_index = -1;
                    break;
                }

                // Require Stable LOW
                if (digitalRead(EN_S) == LOW) {
                    if (low_start == 0) low_start = millis();
                    if (millis() - low_start > 50) { // Stable for 50ms
                        Serial.println("Button Exit (Stable)");
                        Stop_Music();
                        ui_state = UI_MENU_MUSIC;
                        rebuild = true;
                        last_menu_index = -1;
                        
                        // Wait for release again
                        while(digitalRead(EN_S) == LOW) vTaskDelay(10);
                        break; 
                    }
                } else {
                    low_start = 0; // Reset if HIGH detected
                }
                
                // 4. Check EOF
                if(!audioFile.available() && Is_Buf_A_Free() && Is_Buf_B_Free()) {
                    // Only exit if file is done AND both buffers are empty (played out)
                    Serial.println("EOF Exit");
                    Stop_Music();
                    ui_state = UI_MENU_MUSIC;
                    rebuild = true;
                    break;
                }
                
                // 5. Yield slightly
                vTaskDelay(1); 
            }
            
            if(read_buf) free(read_buf);
            Serial.println("Exited Music Loop");
            
        } else if (ui_state == UI_MENU_VIDEO) {
            // 导航
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = video_file_count; // +1 用于返回
                if (menu_index > video_file_count) menu_index = 0;
                rebuild = true;
            }
            
            // 选择
            if (btn_pressed) {
                if (menu_index == video_file_count) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 1;
                    last_menu_index = -1;
                    continue;
                } else {
                    if(Play_Video(video_files[menu_index].c_str())) {
                        ui_state = UI_VIDEO_PLAYER;
                        last_menu_index = -1;
                        continue;
                    } else {
                        // 播放失败
                    }
                }
                rebuild = true;
            }
            
            // 渲染视频菜单
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                int start_y = 1600;
                int spacing = 300;
                int scale = 30;
                
                // 计算滚动
                if(menu_index < music_scroll) music_scroll = menu_index;
                if(menu_index >= music_scroll + visible_lines) music_scroll = menu_index - visible_lines + 1;
                
                String status = "MENU: VIDEO\n";

                for(int i=0; i<visible_lines; i++) {
                    int item_idx = music_scroll + i;
                    int y = start_y - (i * spacing);
                    
                    if(item_idx <= video_file_count) {
                        if(item_idx == menu_index) {
                            DRAW_AddString(">", 0, 50, y, scale, scale);
                            if(item_idx == video_file_count) status += "> Back\n";
                            else status += "> " + String(video_files[item_idx].c_str()) + "\n";
                        } else {
                            if(item_idx == video_file_count) status += "  Back\n";
                            else status += "  " + String(video_files[item_idx].c_str()) + "\n";
                        }
                        
                        if(item_idx == video_file_count) {
                            DRAW_AddString("Back", 0, 250, y, scale, scale);
                        } else {
                            DRAW_AddString(video_files[item_idx].c_str(), 0, 250, y, scale, scale);
                        }
                    }
                }
                updateWebUIStatus(status);
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_VIDEO_PLAYER) {
            // 独占视频循环
            
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddString("PLAYING VIDEO...", 0, 500, 1000, 20, 20);
                DRAW_Update(); 
                updateWebUIStatus("VIDEO PLAYER\nPlaying: " + String(video_files[menu_index].c_str()));
                last_menu_index = 0;
            }
            
            if (!is_video_playing) {
                ui_state = UI_MENU_VIDEO;
                rebuild = true;
            }

            Serial.println("Entering Video Loop");
            unsigned long low_start = 0;
            
            uint8_t *read_buf = (uint8_t*)ps_malloc(65536);
            if(read_buf == NULL) {
                Serial.println("Failed to allocate 64KB read buffer!");
                is_video_playing = false;
            }

            // 音量控制初始化 (仅用于音频通道)
            static int volume = 1; 
            int32_t last_enc = encoderValue;
            
            while (is_video_playing && videoFile) {
                // Check for Game Request
                uint8_t reqGameId;
                uint32_t reqSeed;
                if(Network_Manager::hasGameRequest(&reqGameId, &reqSeed)) {
                    Network_Manager::clearGameRequest();
                    if(reqGameId == 1) { // Tank
                        Stop_Video();
                        ui_state = UI_TANK;
                        Init_Tank_Game(reqSeed, false);
                        rebuild = true;
                        break;
                    }
                }

                // Update Volume
                int32_t curr_enc = encoderValue;
                int delta = (curr_enc - last_enc) / 4; 
                if (delta != 0) {
                    volume += delta;
                    if (volume < 0) volume = 0;
                    if (volume > 256) volume = 256;
                    last_enc = curr_enc;
                }
                // Web Volume
                if (web_enc_delta != 0) {
                    volume += web_enc_delta;
                    if (volume < 0) volume = 0;
                    if (volume > 256) volume = 256;
                    web_enc_delta = 0;
                }

                // 1. Fill Buffer A if free
                if (Is_Buf_A_Free() && videoFile.available()) {
                     int bytesToRead = 65536;
                     if(videoFile.available() < bytesToRead) bytesToRead = videoFile.available();
                     
                     int bytesRead = videoFile.read(read_buf, bytesToRead);
                     if(bytesRead > 0) {
                         uint16_t* bufL = Get_Buf_A_L();
                         uint16_t* bufR = Get_Buf_A_R();
                         uint16_t* bufX = Get_Buf_A_X();
                         uint16_t* bufY = Get_Buf_A_Y();
                         int count = 0;
                         
                         int sample_idx = 0;
                         while(sample_idx < bytesRead) {
                            // 4 Channels, 16-bit = 8 bytes per frame
                            if(sample_idx + 7 < bytesRead) {
                                int16_t ch1 = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                int16_t ch2 = (int16_t)(read_buf[sample_idx+2] | (read_buf[sample_idx+3] << 8));
                                int16_t ch3 = (int16_t)(read_buf[sample_idx+4] | (read_buf[sample_idx+5] << 8));
                                int16_t ch4 = (int16_t)(read_buf[sample_idx+6] | (read_buf[sample_idx+7] << 8));
                                sample_idx += 8;
                                
                                // Apply Volume to Audio (Ch1/Ch2)
                                ch1 = (int16_t)((ch1 * volume) >> 8);
                                ch2 = (int16_t)((ch2 * volume) >> 8);
                                
                                // Map to Buffers
                                // Ch1 -> DAC 2 (L)
                                // Ch2 -> DAC 3 (R)
                                // Ch3 -> DAC 0 (X)
                                // Ch4 -> DAC 1 (Y)
                                bufL[count] = (uint16_t)(ch1 + 32768);
                                bufR[count] = (uint16_t)(ch2 + 32768);
                                bufX[count] = (uint16_t)(ch3 + 32768);
                                bufY[count] = (uint16_t)(ch4 + 32768);
                                count++;
                            } else break;
                         }
                         Mark_Buf_A_Ready(count);
                     }
                }

                // 2. Fill Buffer B if free
                if (Is_Buf_B_Free() && videoFile.available()) {
                     int bytesToRead = 65536;
                     if(videoFile.available() < bytesToRead) bytesToRead = videoFile.available();
                     
                     int bytesRead = videoFile.read(read_buf, bytesToRead);
                     if(bytesRead > 0) {
                         uint16_t* bufL = Get_Buf_B_L();
                         uint16_t* bufR = Get_Buf_B_R();
                         uint16_t* bufX = Get_Buf_B_X();
                         uint16_t* bufY = Get_Buf_B_Y();
                         int count = 0;
                         
                         int sample_idx = 0;
                         while(sample_idx < bytesRead) {
                            if(sample_idx + 7 < bytesRead) {
                                int16_t ch1 = (int16_t)(read_buf[sample_idx] | (read_buf[sample_idx+1] << 8));
                                int16_t ch2 = (int16_t)(read_buf[sample_idx+2] | (read_buf[sample_idx+3] << 8));
                                int16_t ch3 = (int16_t)(read_buf[sample_idx+4] | (read_buf[sample_idx+5] << 8));
                                int16_t ch4 = (int16_t)(read_buf[sample_idx+6] | (read_buf[sample_idx+7] << 8));
                                sample_idx += 8;
                                
                                ch1 = (int16_t)((ch1 * volume) >> 8);
                                ch2 = (int16_t)((ch2 * volume) >> 8);
                                
                                bufL[count] = (uint16_t)(ch1 + 32768);
                                bufR[count] = (uint16_t)(ch2 + 32768);
                                bufX[count] = (uint16_t)(ch3 + 32768);
                                bufY[count] = (uint16_t)(ch4 + 32768);
                                count++;
                            } else break;
                         }
                         Mark_Buf_B_Ready(count);
                     }
                }
                
                // 3. Check Exit Button
                // Check Web Exit
                if (web_btn_pressed) {
                    web_btn_pressed = false;
                    Stop_Video();
                    ui_state = UI_MENU_VIDEO;
                    rebuild = true;
                    last_menu_index = -1;
                    break;
                }

                if (digitalRead(EN_S) == LOW) {
                    if (low_start == 0) low_start = millis();
                    if (millis() - low_start > 50) { 
                        Serial.println("Button Exit (Stable)");
                        Stop_Video();
                        ui_state = UI_MENU_VIDEO;
                        rebuild = true;
                        last_menu_index = -1;
                        while(digitalRead(EN_S) == LOW) vTaskDelay(10);
                        break; 
                    }
                } else {
                    low_start = 0; 
                }
                
                // 4. Check EOF
                if(!videoFile.available() && Is_Buf_A_Free() && Is_Buf_B_Free()) {
                    Serial.println("EOF Exit");
                    Stop_Video();
                    ui_state = UI_MENU_VIDEO;
                    rebuild = true;
                    break;
                }
                
                vTaskDelay(1); 
            }
            
            if(read_buf) free(read_buf);
            Serial.println("Exited Video Loop");
            
        } else if (ui_state == UI_MENU_ONLINE) {
            // 更新网络
            Network_Manager::update();
            
            // 获取对等方
            int peer_count = Network_Manager::getPeerCount();
            const PeerInfo* peers = Network_Manager::getPeers();
            
            // 导航
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = peer_count; // +1 用于返回
                if (menu_index > peer_count) menu_index = 0;
                rebuild = true;
            }
            
            // 选择
            if (btn_pressed) {
                if (menu_index == peer_count) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 3; // 返回 "Online" 选项
                    last_menu_index = -1;
                    continue;
                } else {
                    // 与选定的对等方配对
                    if(peer_count > 0 && menu_index < peer_count) {
                        Network_Manager::pair(peers[menu_index].mac);
                    }
                }
                rebuild = true;
            }
            
            // 每 500ms 强制重绘以更新状态文本
            static unsigned long last_redraw = 0;
            if (millis() - last_redraw > 500) {
                rebuild = true;
                last_redraw = millis();
            }

            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                DRAW_AddString("ONLINE MODE", 0, 600, 1900, 30, 30);
                
                // 显示状态
                NetState state = Network_Manager::getState();
                const char* status_str = "IDLE";
                if(state == NET_DISCOVERING) status_str = "SCANNING...";
                else if(state == NET_PAIRING) status_str = "PAIRING...";
                else if(state == NET_CONNECTED) status_str = "CONNECTED";
                
                DRAW_AddString(status_str, 0, 50, 1800, 20, 20);
                
                int start_y = 1600;
                int spacing = 200;
                int scale = 25;
                
                String status = "MENU: ONLINE\nStatus: " + String(status_str) + "\n";

                // 列出对等方
                static char peer_strings[20][32];
                for(int i=0; i<peer_count && i<20; i++) {
                    int y = start_y - (i * spacing);
                    
                    sprintf(peer_strings[i], "PEER %02X:%02X", peers[i].mac[4], peers[i].mac[5]);

                    if(i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                        status += "> " + String(peer_strings[i]) + "\n";
                    } else {
                        status += "  " + String(peer_strings[i]) + "\n";
                    }
                    
                    DRAW_AddString(peer_strings[i], 0, 200, y, scale, scale);
                }
                
                // 返回选项
                int back_y = start_y - (peer_count * spacing);
                if(menu_index == peer_count) {
                    DRAW_AddString(">", 0, 50, back_y, scale, scale);
                    status += "> Back\n";
                } else {
                    status += "  Back\n";
                }
                DRAW_AddString("BACK", 0, 200, back_y, scale, scale);
                
                updateWebUIStatus(status);
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_SNAKE) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            Update_Snake_Game();
            
            // 始终重绘游戏
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // 绘制蛇
            int cell_size = 2048 / SNAKE_GRID_SIZE;
            for(int i=0; i<snake_len; i++) {
                int x = snake_body[i].x * cell_size + (cell_size/2);
                int y = snake_body[i].y * cell_size + (cell_size/2);
                int half_size = (cell_size / 2) - 5;
                DRAW_AddRect(x - half_size, y - half_size, 2*half_size, 2*half_size);
            }
            
            // 绘制食物
            int fx = snake_food.x * cell_size + (cell_size/2);
            int fy = snake_food.y * cell_size + (cell_size/2);
            int f_half = (cell_size / 2) - 10;
            DRAW_AddLine(fx - f_half, fy - f_half, fx + f_half, fy + f_half);
            DRAW_AddLine(fx - f_half, fy + f_half, fx + f_half, fy - f_half);
            
            if(game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", game_score);
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
                updateWebUIStatus("GAME: SNAKE\nGAME OVER\nScore: " + String(game_score));
            } else {
                updateWebUIStatus("GAME: SNAKE\nScore: " + String(game_score));
            }

        } else if (ui_state == UI_BREAKOUT) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            Update_Breakout_Game(enc_delta);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // 绘制挡板
            int py = 100;
            DRAW_AddRect(brk_paddle.x, py, BRK_PADDLE_W, BRK_PADDLE_H);
            
            // 绘制球
            DRAW_AddCircle(brk_ball.x, brk_ball.y, BRK_BALL_R);
            
            // 绘制砖块
            int brick_start_y = 1500;
            for(int r=0; r<BRK_ROWS; r++) {
                for(int c=0; c<BRK_COLS; c++) {
                    if(brk_bricks[r][c]) {
                        int bx = c * BRK_BRICK_W;
                        int by = brick_start_y + (r * BRK_BRICK_H);
                        int bw = BRK_BRICK_W;
                        int bh = BRK_BRICK_H;
                        
                        // 简单的砖块绘制 (矩形)
                        DRAW_AddRect(bx + 2, by + 2, bw - 4, bh - 4);
                    }
                }
            }
            
            // 绘制分数/生命
            if(brk_game_over) {
                if(brk_game_over == 2) {
                    DRAW_AddString("YOU WIN", 0, 600, 1100, 20, 20);
                    updateWebUIStatus("GAME: BREAKOUT\nYOU WIN\nScore: " + String(brk_score));
                } else {
                    DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                    updateWebUIStatus("GAME: BREAKOUT\nGAME OVER\nScore: " + String(brk_score));
                }
                char score_str[32];
                sprintf(score_str, "SCORE: %d", brk_score);
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            } else {
                char lives_str[16];
                sprintf(lives_str, "L:%d", brk_lives);
                DRAW_AddString(lives_str, 0, 50, 50, 30, 30);
                updateWebUIStatus("GAME: BREAKOUT\nScore: " + String(brk_score) + "\nLives: " + String(brk_lives));
            }

        } else if (ui_state == UI_FLAPPY) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            // 输入
            bool current_touch_up = (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA);
            int jump = 0;
            
            // Web 输入
            if (web_game_dir == 0) {
                jump = 1;
                web_game_dir = -1;
            }

            if (current_touch_up && !last_touch_up) {
                jump = 1;
            }
            last_touch_up = current_touch_up;
            
            Update_Flappy_Game(jump);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // 绘制玩家
            DRAW_AddCircle(FLP_PLAYER_X, flp_player.y, FLP_PLAYER_R);
            // 天线
            DRAW_AddLine(FLP_PLAYER_X - 20, flp_player.y + 30, FLP_PLAYER_X - 40, flp_player.y + 75);
            DRAW_AddLine(FLP_PLAYER_X + 20, flp_player.y + 30, FLP_PLAYER_X + 40, flp_player.y + 75);
            
            // 绘制障碍物
            for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
                if(flp_obstacles[i].active) {
                    int x = flp_obstacles[i].x;
                    int gap_y = flp_obstacles[i].gap_y;
                    int w = FLP_OBSTACLE_W;
                    int h_gap = FLP_GAP_H / 2;
                    
                    // 顶部障碍物
                    int top_y = gap_y + h_gap;
                    if(top_y < 2048) {
                        DRAW_AddRect(x, top_y, w, 2048 - top_y);
                    }
                    
                    // 底部障碍物
                    int bot_y = gap_y - h_gap;
                    if(bot_y > 0) {
                        DRAW_AddRect(x, 0, w, bot_y);
                    }
                }
            }
            
            // 分数
            if(flp_game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", flp_score);
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            } else {
                char score_str[32];
                sprintf(score_str, "SCORE: %d", flp_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
            }

        } else if (ui_state == UI_RACING) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            Update_Racing_Game(enc_delta);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // 绘制赛车
            DRAW_AddRect(race_car.x, 100, RACE_CAR_W, RACE_CAR_H);
            // 添加一些细节 (车轮)
            DRAW_AddRect(race_car.x - 25, 125, 25, 75);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 125, 25, 75);
            DRAW_AddRect(race_car.x - 25, 250, 25, 75);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 250, 25, 75);
            
            // 绘制障碍物
            for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
                if(race_obstacles[i].active) {
                    DRAW_AddRect(race_obstacles[i].x, race_obstacles[i].y, RACE_OBSTACLE_W, RACE_OBSTACLE_H);
                    // 在障碍物内绘制 X
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y + RACE_OBSTACLE_H);
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y + RACE_OBSTACLE_H, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y);
                }
            }
            
            if(race_game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", race_score);
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
                updateWebUIStatus("GAME: RACING\nGAME OVER\nScore: " + String(race_score));
            } else {
                char score_str[16];
                sprintf(score_str, "%d", race_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
                updateWebUIStatus("GAME: RACING\nScore: " + String(race_score));
            }

        } else if (ui_state == UI_RUNTINY) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            // Input
            bool current_touch_up = (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA);
            int jump = 0;
            
            // Web Input
            if (web_game_dir == 0) {
                jump = 1;
                web_game_dir = -1;
            }

            if (current_touch_up && !last_touch_up) {
                jump = 1;
            }
            last_touch_up = current_touch_up;
            
            Update_RunTiny_Game(jump);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Ground
            DRAW_AddLine(0, RUN_GROUND_Y, 2048, RUN_GROUND_Y);
            
            // Draw Player
            DRAW_AddRect(RUN_PLAYER_X, run_player.y, RUN_PLAYER_W, RUN_PLAYER_H);
            // Legs animation
            if(run_player.y == RUN_GROUND_Y) { // Only animate when running
                static int leg_state = 0;
                if(millis() % 200 < 100) leg_state = 0; else leg_state = 1;
                
                if(leg_state) {
                    DRAW_AddLine(RUN_PLAYER_X + 25, run_player.y, RUN_PLAYER_X + 25, run_player.y - 25);
                    DRAW_AddLine(RUN_PLAYER_X + 75, run_player.y, RUN_PLAYER_X + 75, run_player.y - 25);
                } else {
                    DRAW_AddLine(RUN_PLAYER_X + 10, run_player.y, RUN_PLAYER_X + 40, run_player.y - 25);
                    DRAW_AddLine(RUN_PLAYER_X + 90, run_player.y, RUN_PLAYER_X + 60, run_player.y - 25);
                }
            }
            
            // Draw Obstacles
            for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
                if(run_obstacles[i].active) {
                    DRAW_AddRect(run_obstacles[i].x, RUN_GROUND_Y, RUN_OBSTACLE_W, RUN_OBSTACLE_H);
                }
            }
            
            if(run_game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", run_score);
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
                updateWebUIStatus("GAME: RUNTINY\nGAME OVER\nScore: " + String(run_score));
            } else {
                char score_str[16];
                sprintf(score_str, "%d", run_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
                updateWebUIStatus("GAME: RUNTINY\nScore: " + String(run_score));
            }

        } else if (ui_state == UI_TANK) {
            // 退出
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                Network_Manager::endGame(0); // 原因 0 = 退出
                continue;
            }
            
            Update_Tank_Game();
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // 绘制地图
            for(int i=0; i<TANK_MAP_SIZE; i++) {
                if(tank_map[i].type == 0) { // 墙
                    DRAW_AddRect(tank_map[i].x, tank_map[i].y, tank_map[i].w, tank_map[i].h);
                    // 墙的交叉阴影
                    DRAW_AddLine(tank_map[i].x, tank_map[i].y, tank_map[i].x + tank_map[i].w, tank_map[i].y + tank_map[i].h);
                    DRAW_AddLine(tank_map[i].x, tank_map[i].y + tank_map[i].h, tank_map[i].x + tank_map[i].w, tank_map[i].y);
                } else { // 水
                    DRAW_AddRect(tank_map[i].x, tank_map[i].y, tank_map[i].w, tank_map[i].h);
                    // 水的波浪
                    int y_mid = tank_map[i].y + tank_map[i].h/2;
                    DRAW_AddLine(tank_map[i].x, y_mid, tank_map[i].x + tank_map[i].w, y_mid);
                }
            }
            
            // 绘制坦克
            // 车身 (旋转矩形)
            float cos_a = cos(my_tank.angle);
            float sin_a = sin(my_tank.angle);
            
            // 坦克尺寸
            float w = 60;
            float h = 80;
            
            // 相对于中心的 4 个角
            float x1 = -w/2, y1 = -h/2;
            float x2 = w/2, y2 = -h/2;
            float x3 = w/2, y3 = h/2;
            float x4 = -w/2, y4 = h/2;
            
            // 旋转和平移
            auto rotX = [&](float x, float y) { return my_tank.x + x*cos_a - y*sin_a; };
            auto rotY = [&](float x, float y) { return my_tank.y + x*sin_a + y*cos_a; };
            
            float rx1 = rotX(x1, y1), ry1 = rotY(x1, y1);
            float rx2 = rotX(x2, y2), ry2 = rotY(x2, y2);
            float rx3 = rotX(x3, y3), ry3 = rotY(x3, y3);
            float rx4 = rotX(x4, y4), ry4 = rotY(x4, y4);
            
            DRAW_AddLine(rx1, ry1, rx2, ry2);
            DRAW_AddLine(rx2, ry2, rx3, ry3);
            DRAW_AddLine(rx3, ry3, rx4, ry4);
            DRAW_AddLine(rx4, ry4, rx1, ry1);
            
            // 炮塔 (盒子)
            float tw = 30, th = 30;
            float tx1 = -tw/2, ty1 = -th/2;
            float tx2 = tw/2, ty2 = -th/2;
            float tx3 = tw/2, ty3 = th/2;
            float tx4 = -tw/2, ty4 = th/2;
            
            DRAW_AddLine(rotX(tx1, ty1), rotY(tx1, ty1), rotX(tx2, ty2), rotY(tx2, ty2));
            DRAW_AddLine(rotX(tx2, ty2), rotY(tx2, ty2), rotX(tx3, ty3), rotY(tx3, ty3));
            DRAW_AddLine(rotX(tx3, ty3), rotY(tx3, ty3), rotX(tx4, ty4), rotY(tx4, ty4));
            DRAW_AddLine(rotX(tx4, ty4), rotY(tx4, ty4), rotX(tx1, ty1), rotY(tx1, ty1));
            
            // 炮管 (线)
            DRAW_AddLine(my_tank.x, my_tank.y, my_tank.x + 100*cos_a, my_tank.y + 100*sin_a);
            
            // 绘制子弹
            for(int i=0; i<TANK_MAX_BULLETS; i++) {
                if(tank_bullets[i].active) {
                    DRAW_AddCircle(tank_bullets[i].x, tank_bullets[i].y, 5);
                }
            }

            // 绘制远程坦克
            TankData remoteData;
            if(Network_Manager::getRemoteGameData(&remoteData)) {
                float r_cos = cos(remoteData.angle);
                float r_sin = sin(remoteData.angle);
                
                // 远程坦克车身
                auto rRotX = [&](float x, float y) { return remoteData.x + x*r_cos - y*r_sin; };
                auto rRotY = [&](float x, float y) { return remoteData.y + x*r_sin + y*r_cos; };
                
                float rx1 = rRotX(x1, y1), ry1 = rRotY(x1, y1);
                float rx2 = rRotX(x2, y2), ry2 = rRotY(x2, y2);
                float rx3 = rRotX(x3, y3), ry3 = rRotY(x3, y3);
                float rx4 = rRotX(x4, y4), ry4 = rRotY(x4, y4);
                
                DRAW_AddLine(rx1, ry1, rx2, ry2);
                DRAW_AddLine(rx2, ry2, rx3, ry3);
                DRAW_AddLine(rx3, ry3, rx4, ry4);
                DRAW_AddLine(rx4, ry4, rx1, ry1);
                
                // 远程炮塔
                DRAW_AddLine(rRotX(tx1, ty1), rRotY(tx1, ty1), rRotX(tx2, ty2), rRotY(tx2, ty2));
                DRAW_AddLine(rRotX(tx2, ty2), rRotY(tx2, ty2), rRotX(tx3, ty3), rRotY(tx3, ty3));
                DRAW_AddLine(rRotX(tx3, ty3), rRotY(tx3, ty3), rRotX(tx4, ty4), rRotY(tx4, ty4));
                DRAW_AddLine(rRotX(tx4, ty4), rRotY(tx4, ty4), rRotX(tx1, ty1), rRotY(tx1, ty1));
                
                // 远程炮管
                DRAW_AddLine(remoteData.x, remoteData.y, remoteData.x + 100*r_cos, remoteData.y + 100*r_sin);
                
                // 远程子弹
                for(int i=0; i<remoteData.bullet_count; i++) {
                    DRAW_AddCircle(remoteData.bullets[i].x, remoteData.bullets[i].y, 5);
                }
            }
            
            String status = "GAME: TANK\n";

            if(tank_game_over) {
                if(tank_game_over == 1) {
                    DRAW_AddString("YOU LOSE", 0, 600, 1100, 20, 20);
                    status += "YOU LOSE\n";
                } else if(tank_game_over == 2) {
                    DRAW_AddString("NOT CONNECTED", 0, 400, 1100, 20, 20);
                    status += "NOT CONNECTED\n";
                } else if(tank_game_over == 3) {
                    DRAW_AddString("OPPONENT LEFT", 0, 400, 1100, 20, 20);
                    status += "OPPONENT LEFT\n";
                } else if(tank_game_over == 4) {
                    DRAW_AddString("YOU WIN", 0, 600, 1100, 20, 20);
                    status += "YOU WIN\n";
                }
                
                // 2 秒后自动退出
                static unsigned long exit_timer = 0;
                if(exit_timer == 0) exit_timer = millis();
                if(millis() - exit_timer > 2000) {
                    ui_state = UI_MENU_GAMES;
                    rebuild = true;
                    last_menu_index = -1;
                    exit_timer = 0;
                    Network_Manager::endGame(0);
                }
            } else {
                status += "Playing...\n";
            }

            if(!is_joystick_connected) {
                DRAW_AddString("JOYSTICK", 0, 400, 900, 20, 20);
                DRAW_AddString("DISCONNECTED", 0, 400, 700, 20, 20);
                status += "JOYSTICK DISCONNECTED\n";
            }
            
            updateWebUIStatus(status);

        } else if (ui_state == UI_ABOUT) {
             if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1;
                continue;
            }
            
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(50, 50, 1948, 1948);
                DRAW_AddString("ABOUT", 0, 780, 1800, 17, 17);
                DRAW_AddString("ESP32 VECTOR", 0, 610, 1300, 30, 30);
                DRAW_AddString("DISPLAY SYSTEM", 0, 540, 1050, 30, 30);
                DRAW_AddString("V1.0", 0, 885, 800, 30, 30);
                updateWebUIStatus("ABOUT\nESP32 VECTOR\nDISPLAY SYSTEM\nV1.0");
                last_menu_index = 0; // 标记为已绘制
            }
        }

        // 更新动画并渲染
        DRAW_Update();
        DRAW_Render();

        vTaskDelay(pdMS_TO_TICKS(40)); // 25Hz 更新率
    }
}

static void serialOutputTask(void* pvParameters) {
  unsigned long lastOutputTime = 0;
  const unsigned long outputInterval = 200; // 200ms 间隔
  //循环输出触摸值
  for (;;) {
    if (millis() - lastOutputTime >= outputInterval) {
      lastOutputTime = millis();
      Serial.printf("Touch(Cap): U=%d D=%d L=%d R=%d\n", 
        touchRead(TOUCH_UP), 
        touchRead(TOUCH_DOWN), 
        touchRead(TOUCH_LEFT), 
        touchRead(TOUCH_RIGHT));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void joystickCheckTask(void* pvParameters) {
    int consecutive_disconnects = 0;
    
    for(;;) {
        int j1x = analogRead(JOY1_X);
        int j1y = analogRead(JOY1_Y);
        int j2x = analogRead(JOY2_X);
        int j2y = analogRead(JOY2_Y);
        
        bool cond1 = (j1x >= 300 && j1x <= 700);
        bool cond2 = (j1y >= 1 && j1y <= 200);
        bool cond3 = (j2x >= 400 && j2x <= 1200);
        bool cond4 = (j2y >= 1 && j2y <= 350);
        
        if(cond1 && cond2 && cond3 && cond4) {
            consecutive_disconnects++;
        } else {
            consecutive_disconnects = 0;
            is_joystick_connected = true;
        }
        
        if(consecutive_disconnects >= 10) {
            is_joystick_connected = false;
            if(consecutive_disconnects > 20) consecutive_disconnects = 20; 
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}