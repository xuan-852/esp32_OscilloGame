#include "freertos.h"
#include "pins.h"
#include "vector_draw.h"
#include <Arduino.h>
#include <stdio.h>
#include "SD_MMC.h"
#include "audio_common.h"
#include "DACoutput.h" // For setDACFreq

// External variables from main.cpp
extern volatile int32_t encoderValue;

// Task Handles
static TaskHandle_t s_serialOutputTaskHandle = nullptr;
static TaskHandle_t s_guiTaskHandle = nullptr;

// --- Snake Game Defines ---
#define SNAKE_GRID_SIZE 20
#define SNAKE_MAX_LEN 100
typedef struct {
    int8_t x;
    int8_t y;
} Point;

static Point snake_body[SNAKE_MAX_LEN];
static int snake_len = 3;
static Point snake_food;
static int snake_dir = 3; // 0:UP, 1:DOWN, 2:LEFT, 3:RIGHT
static int game_over = 0;
static uint32_t last_game_tick = 0;
static int game_score = 0;
static int8_t Game_Input_Dir = 3;

// --- Breakout Game Defines ---
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

// --- Flappy Game Defines ---
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
    int32_t gap_y; // Center of gap
    int active;
    int passed;
} FlpObstacle;

static FlpPlayer flp_player;
static FlpObstacle flp_obstacles[FLP_MAX_OBSTACLES];
static int flp_score = 0;
static int flp_game_over = 0;

// --- Racing Game Defines ---
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

// --- RunTiny Game Defines ---
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

// Touch Pins
#define TOUCH_UP    4
#define TOUCH_DOWN  6
#define TOUCH_LEFT  5
#define TOUCH_RIGHT 7
#define TOUCH_THRESHOLD 35000 

// Task Functions
static void serialOutputTask(void* pvParameters);
static void guiTask(void* pvParameters);

void initTasks() {
  // Create Serial Output Task
  if (!s_serialOutputTaskHandle) {
    xTaskCreatePinnedToCore(
      serialOutputTask,
      "SerialOutputTask",
      2048,
      nullptr,
      1,
      &s_serialOutputTaskHandle,
      1 // Core 1
    );
  }

  // Create GUI Task
  if (!s_guiTaskHandle) {
    xTaskCreatePinnedToCore(
      guiTask,
      "GuiTask",
      4096, // Larger stack for GUI
      nullptr,
      1,
      &s_guiTaskHandle,
      1 // Core 1
    );
  }
}

// --- GUI Logic ---

enum UI_State {
    UI_MENU_MAIN,
    UI_MENU_GAMES,
    UI_MUSIC_PLAYER,
    UI_SNAKE,
    UI_BREAKOUT,
    UI_FLAPPY,
    UI_RACING,
    UI_RUNTINY,
    UI_ABOUT
};

static const char* main_menu_items[] = {
    "Music Player",
    "Games",
    "Settings",
    "About"
};
static const int main_menu_count = 4;

static const char* games_menu_items[] = {
    "Snake",
    "Breakout",
    "Flappy",
    "Racing",
    "RunTiny",
    "Back"
};
static const int games_menu_count = 6;

// --- Music Player Variables ---
#define MAX_MUSIC_FILES 20
#define MAX_FILENAME_LEN 64
static char music_files[MAX_MUSIC_FILES][MAX_FILENAME_LEN];
static int music_file_count = 0;
static int music_selected_index = 0;
static File music_file;
static bool music_is_playing = false;
static uint32_t music_sample_rate = 44100;
static uint16_t music_channels = 2;
static uint32_t music_data_start = 44;
static uint32_t music_data_size = 0;
static uint32_t music_bytes_read = 0;

void ScanMusicFiles() {
    music_file_count = 0;
    File root = SD_MMC.open("/music");
    if(!root || !root.isDirectory()) return;
    
    File file = root.openNextFile();
    while(file) {
        if(!file.isDirectory()) {
            const char* name = file.name();
            int len = strlen(name);
            if(len > 4 && strcasecmp(name + len - 4, ".wav") == 0) {
                strncpy(music_files[music_file_count], name, MAX_FILENAME_LEN - 1);
                music_files[music_file_count][MAX_FILENAME_LEN - 1] = 0;
                music_file_count++;
                if(music_file_count >= MAX_MUSIC_FILES) break;
            }
        }
        file = root.openNextFile();
    }
    root.close();
}

bool OpenMusicFile(const char* filename) {
    char path[128];
    snprintf(path, sizeof(path), "/music/%s", filename);
    
    if(music_file) music_file.close();
    music_file = SD_MMC.open(path);
    if(!music_file) return false;
    
    // Parse WAV Header
    uint8_t header[44];
    if(music_file.read(header, 44) != 44) return false;
    
    // Check "RIFF" and "WAVE"
    if(memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;
    
    music_channels = header[22];
    music_sample_rate = *(uint32_t*)(header + 24);
    music_data_size = *(uint32_t*)(header + 40);
    music_data_start = 44; // Simplified, assuming standard header
    
    Serial.printf("WAV: %d Hz, %d Ch, Size: %d\n", music_sample_rate, music_channels, music_data_size);

    music_bytes_read = 0;
    
    // Reset Buffer
    bufferHead = 0;
    bufferTail = 0;
    
    return true;
}

// --- Snake Game Logic ---
void Init_Snake_Game(void) {
    snake_len = 3;
    snake_body[0].x = 10; snake_body[0].y = 10;
    snake_body[1].x = 10; snake_body[1].y = 9;
    snake_body[2].x = 10; snake_body[2].y = 8;
    
    snake_dir = 3; // RIGHT
    Game_Input_Dir = 3;
    
    // Random Food
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
    
    if(millis() - last_game_tick > 200) { // 200ms speed
        last_game_tick = millis();
        
        // Move Body
        for(int i=snake_len-1; i>0; i--) {
            snake_body[i] = snake_body[i-1];
        }
        
        // Move Head
        if(snake_dir == 0) snake_body[0].y++; // UP
        if(snake_dir == 1) snake_body[0].y--; // DOWN
        if(snake_dir == 2) snake_body[0].x--; // LEFT
        if(snake_dir == 3) snake_body[0].x++; // RIGHT
        
        // Check Wall Collision
        if(snake_body[0].x < 0 || snake_body[0].x >= SNAKE_GRID_SIZE ||
           snake_body[0].y < 0 || snake_body[0].y >= SNAKE_GRID_SIZE) {
            game_over = 1;
        }
        
        // Check Self Collision
        for(int i=1; i<snake_len; i++) {
            if(snake_body[0].x == snake_body[i].x && snake_body[0].y == snake_body[i].y) {
                game_over = 1;
            }
        }
        
        // Check Food
        if(snake_body[0].x == snake_food.x && snake_body[0].y == snake_food.y) {
            if(snake_len < SNAKE_MAX_LEN) {
                snake_len++;
                game_score += 10;
            }
            // New Food
            snake_food.x = rand() % SNAKE_GRID_SIZE;
            snake_food.y = rand() % SNAKE_GRID_SIZE;
        }
    }
}

// --- Breakout Game Logic ---
void Init_Breakout_Game(void) {
    // Reset Paddle
    brk_paddle.x = 1024 - (BRK_PADDLE_W / 2);

    // Reset Ball
    brk_ball.x = 1024;
    brk_ball.y = 750;
    brk_ball.vx = 15; 
    brk_ball.vy = 15;

    // Reset Bricks
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

    // Update Paddle
    brk_paddle.x += encoder_delta * 50; // Sensitivity
    if(brk_paddle.x < 0) brk_paddle.x = 0;
    if(brk_paddle.x > 2048 - BRK_PADDLE_W) brk_paddle.x = 2048 - BRK_PADDLE_W;

    // Update Ball
    brk_ball.x += brk_ball.vx;
    brk_ball.y += brk_ball.vy;

    // Wall Collisions (Left/Right)
    if(brk_ball.x <= 0) {
        brk_ball.x = 0;
        brk_ball.vx = -brk_ball.vx;
    }
    if(brk_ball.x >= 2048) {
        brk_ball.x = 2048;
        brk_ball.vx = -brk_ball.vx;
    }
    
    // Top Wall
    if(brk_ball.y >= 2048) {
        brk_ball.y = 2048;
        brk_ball.vy = -brk_ball.vy;
    }

    // Paddle Collision (Bottom)
    int paddle_y = 100;
    int paddle_top = paddle_y + BRK_PADDLE_H;
    
    // Only check collision if moving DOWN
    if(brk_ball.vy < 0) {
        if(brk_ball.y <= paddle_top + BRK_BALL_R && brk_ball.y >= 25) {
            // Check X range
            if(brk_ball.x >= brk_paddle.x - BRK_BALL_R && brk_ball.x <= brk_paddle.x + BRK_PADDLE_W + BRK_BALL_R) {
                // Hit
                brk_ball.vy = abs(brk_ball.vy); // Force Up
                
                // Anti-Tunneling: Push ball to surface
                brk_ball.y = paddle_top + BRK_BALL_R + 1;
                
                // Add some paddle velocity influence (optional)
                // brk_ball.vx += encoder_delta * 2;
            }
        }
    }

    // Bottom Wall (Death)
    if(brk_ball.y < 0) {
        brk_lives--;
        if(brk_lives <= 0) {
            brk_game_over = 1; // Lose
        } else {
            // Reset Ball
            brk_ball.x = 1024;
            brk_ball.y = 750;
            brk_ball.vx = 15;
            brk_ball.vy = 15;
        }
    }

    // Brick Collision
    int brick_start_y = 1500;
    
    // Optimization: Only check if ball is in brick area
    if(brk_ball.y >= brick_start_y && brk_ball.y <= brick_start_y + (BRK_ROWS * BRK_BRICK_H)) {
        int r = (brk_ball.y - brick_start_y) / BRK_BRICK_H;
        int c = brk_ball.x / BRK_BRICK_W;
        
        if(r >= 0 && r < BRK_ROWS && c >= 0 && c < BRK_COLS) {
            if(brk_bricks[r][c]) {
                brk_bricks[r][c] = 0;
                brk_score += 10;
                brk_brick_count--;
                
                // Simple bounce (reverse Y) - could be improved
                brk_ball.vy = -brk_ball.vy;
                
                if(brk_brick_count <= 0) {
                    brk_game_over = 2; // Win
                }
            }
        }
    }
}

// --- Flappy Game Logic ---
void Init_Flappy_Game(void) {
    flp_player.y = 1024;
    flp_player.vy = 0;
    
    flp_score = 0;
    flp_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].active = 1;
        flp_obstacles[i].x = 2048 + 250 + (i * FLP_OBSTACLE_SPACING);
        flp_obstacles[i].gap_y = 500 + (rand() % 1048); // 500 to 1548
        flp_obstacles[i].passed = 0;
    }
}

void Update_Flappy_Game(int jump_requested) {
    if(flp_game_over) return;
    
    // Physics
    if(jump_requested) {
        flp_player.vy = FLP_JUMP_FORCE;
    }
    
    flp_player.y += flp_player.vy;
    flp_player.vy -= FLP_GRAVITY;
    
    // Cap Velocity
    if(flp_player.vy < -25) flp_player.vy = -25;
    if(flp_player.vy > 25) flp_player.vy = 25;
    
    // Floor/Ceiling Collision
    if(flp_player.y < FLP_PLAYER_R) {
        flp_player.y = FLP_PLAYER_R;
        flp_game_over = 1;
    }
    if(flp_player.y > 2048 - FLP_PLAYER_R) {
        flp_player.y = 2048 - FLP_PLAYER_R;
        flp_player.vy = 0;
    }
    
    // Obstacles
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].x -= FLP_SPEED;
        
        // Recycle
        if(flp_obstacles[i].x < -FLP_OBSTACLE_W) {
            // Find max x to place after
            int max_x = 0;
            for(int j=0; j<FLP_MAX_OBSTACLES; j++) {
                if(flp_obstacles[j].x > max_x) max_x = flp_obstacles[j].x;
            }
            flp_obstacles[i].x = max_x + FLP_OBSTACLE_SPACING;
            flp_obstacles[i].gap_y = 500 + (rand() % 1048);
            flp_obstacles[i].passed = 0;
        }
        
        // Collision
        int ox = flp_obstacles[i].x;
        int ow = FLP_OBSTACLE_W;
        int gap_top = flp_obstacles[i].gap_y + FLP_GAP_H/2;
        int gap_bot = flp_obstacles[i].gap_y - FLP_GAP_H/2;
        
        // Horizontal Check
        if(FLP_PLAYER_X + FLP_PLAYER_R > ox && FLP_PLAYER_X - FLP_PLAYER_R < ox + ow) {
            // Vertical Check (Hit Top or Hit Bottom)
            if(flp_player.y + FLP_PLAYER_R > gap_top || flp_player.y - FLP_PLAYER_R < gap_bot) {
                flp_game_over = 1;
            }
        }
        
        // Score
        if(!flp_obstacles[i].passed && flp_obstacles[i].x + ow < FLP_PLAYER_X - FLP_PLAYER_R) {
            flp_score++;
            flp_obstacles[i].passed = 1;
        }
    }
}

// --- Racing Game Logic ---
void Init_Racing_Game(void) {
    race_car.x = 1024 - (RACE_CAR_W / 2);
    race_score = 0;
    race_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].active = 1;
        race_obstacles[i].x = rand() % (2048 - RACE_OBSTACLE_W);
        race_obstacles[i].y = 2048 + (i * 750); // Spacing
        race_obstacles[i].passed = 0;
    }
}

void Update_Racing_Game(int16_t encoder_delta) {
    if(race_game_over) return;
    
    // Move Car
    race_car.x += encoder_delta * 75; // Sensitivity
    if(race_car.x < 0) race_car.x = 0;
    if(race_car.x > 2048 - RACE_CAR_W) race_car.x = 2048 - RACE_CAR_W;
    
    // Move Obstacles
    int current_speed = RACE_SPEED + (race_score * 0.25);
    if(current_speed > 75) current_speed = 75; // Cap speed

    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].y -= current_speed;
        
        // Recycle
        if(race_obstacles[i].y < -RACE_OBSTACLE_H) {
            // Find max y
            int max_y = 0;
            for(int j=0; j<RACE_MAX_OBSTACLES; j++) {
                if(race_obstacles[j].y > max_y) max_y = race_obstacles[j].y;
            }
            race_obstacles[i].y = max_y + 750; // Spacing
            race_obstacles[i].x = rand() % (2048 - RACE_OBSTACLE_W);
            race_obstacles[i].passed = 0;
        }
        
        // Collision
        // Simple AABB
        if(race_car.x < race_obstacles[i].x + RACE_OBSTACLE_W &&
           race_car.x + RACE_CAR_W > race_obstacles[i].x &&
           100 < race_obstacles[i].y + RACE_OBSTACLE_H &&
           100 + RACE_CAR_H > race_obstacles[i].y) {
            race_game_over = 1;
        }
        
        // Score
        if(!race_obstacles[i].passed && race_obstacles[i].y + RACE_OBSTACLE_H < 100) {
            race_score++;
            race_obstacles[i].passed = 1;
        }
    }
}

// --- RunTiny Game Logic ---
void Init_RunTiny_Game(void) {
    run_player.y = RUN_GROUND_Y;
    run_player.vy = 0;
    run_player.jumping = 0;
    
    run_score = 0;
    run_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].active = 1;
        run_obstacles[i].x = 2048 + 500 + (i * 1000) + (rand() % 400); // Spacing with jitter
        run_obstacles[i].passed = 0;
    }
}

void Update_RunTiny_Game(int jump_requested) {
    if(run_game_over) return;
    
    // Physics
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
    
    // Move Obstacles
    int current_speed = RUN_SPEED + (run_score * 0.1);
    if(current_speed > 50) current_speed = 50;
    
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].x -= current_speed;
        
        // Recycle
        if(run_obstacles[i].x < -RUN_OBSTACLE_W) {
            // Find max x
            int max_x = 0;
            for(int j=0; j<RUN_MAX_OBSTACLES; j++) {
                if(run_obstacles[j].x > max_x) max_x = run_obstacles[j].x;
            }
            // More random spacing: 600 to 1350
            run_obstacles[i].x = max_x + 600 + (rand() % 750);
            run_obstacles[i].passed = 0;
        }
        
        // Collision
        if(run_obstacles[i].x < RUN_PLAYER_X + RUN_PLAYER_W &&
           run_obstacles[i].x + RUN_OBSTACLE_W > RUN_PLAYER_X &&
           run_player.y < RUN_GROUND_Y + RUN_OBSTACLE_H) { // Simple height check
            run_game_over = 1;
        }
        
        // Score
        if(!run_obstacles[i].passed && run_obstacles[i].x + RUN_OBSTACLE_W < RUN_PLAYER_X) {
            run_score++;
            run_obstacles[i].passed = 1;
        }
    }
}

static void guiTask(void* pvParameters) {
    // Calibration
    int touch_base_up = 0;
    int touch_base_down = 0;
    int touch_base_left = 0;
    int touch_base_right = 0;
    
    // Take samples
    for(int i=0; i<20; i++) {
        //touch_base_up += touchRead(TOUCH_UP);
        //touch_base_down += touchRead(TOUCH_DOWN);
        //touch_base_left += touchRead(TOUCH_LEFT);
        //touch_base_right += touchRead(TOUCH_RIGHT);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    touch_base_up /= 20;
    touch_base_down /= 20;
    touch_base_left /= 20;
    touch_base_right /= 20;
    
    // Define a delta
    const int TOUCH_DELTA = 30000; 

    UI_State ui_state = UI_MENU_MAIN;
    int menu_index = 0;
    int last_menu_index = -1;
    int32_t last_encoder = 0;
    
    // Debounce for button
    int last_btn_state = HIGH;
    unsigned long last_btn_time = 0;
    
    // Music Player state
    bool music_menu_active = true;

    // Flappy state
    bool last_touch_up = false;

    for (;;) {
        // 1. Handle Input
        int32_t current_encoder = encoderValue / 4;
        int16_t enc_delta = current_encoder - last_encoder;
        last_encoder = current_encoder;
        
        int btn_state = digitalRead(EN_S);
        bool btn_pressed = false;
        
        if (btn_state == LOW && last_btn_state == HIGH && (millis() - last_btn_time > 50)) {
            btn_pressed = true;
            last_btn_time = millis();
        }
        last_btn_state = btn_state;

        // Touch Input for Snake
        if (ui_state == UI_SNAKE) {
            if (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA) Game_Input_Dir = 0;
            if (touchRead(TOUCH_DOWN) > touch_base_down + TOUCH_DELTA) Game_Input_Dir = 1;
            if (touchRead(TOUCH_LEFT) > touch_base_left + TOUCH_DELTA) Game_Input_Dir = 2;
            if (touchRead(TOUCH_RIGHT) > touch_base_right + TOUCH_DELTA) Game_Input_Dir = 3;
        }

        // 2. State Machine
        bool rebuild = false;

        if (ui_state == UI_MENU_MAIN) {
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = main_menu_count - 1;
                if (menu_index >= main_menu_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
            if (btn_pressed) {
                if (menu_index == 0) {
                    ui_state = UI_MUSIC_PLAYER;
                    ScanMusicFiles();
                    music_menu_active = true;
                    music_selected_index = 0;
                    last_menu_index = -1;
                } else if (menu_index == 1) {
                    ui_state = UI_MENU_GAMES;
                    menu_index = 0; // Reset for submenu
                    last_menu_index = -1;
                } else if (menu_index == 3) {
                    ui_state = UI_ABOUT;
                }
                rebuild = true;
            }
            
            // Render Main Menu
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047); // Full Screen Border
                
                int start_y = 1600; 
                int spacing = 400;
                int scale = 40; 
                
                for (int i = 0; i < main_menu_count; i++) {
                    int y = start_y - (i * spacing);
                    if (i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                    }
                    DRAW_AddString(main_menu_items[i], 0, 250, y, scale, scale);
                }
                last_menu_index = menu_index;
            }
            
        } else if (ui_state == UI_MENU_GAMES) {
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = games_menu_count - 1;
                if (menu_index >= games_menu_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
            if (btn_pressed) {
                if (menu_index == 0) {
                    ui_state = UI_SNAKE;
                    Init_Snake_Game();
                } else if (menu_index == 1) {
                    ui_state = UI_BREAKOUT;
                    Init_Breakout_Game();
                } else if (menu_index == 2) {
                    ui_state = UI_FLAPPY;
                    Init_Flappy_Game();
                } else if (menu_index == 3) {
                    ui_state = UI_RACING;
                    Init_Racing_Game();
                } else if (menu_index == 4) {
                    ui_state = UI_RUNTINY;
                    Init_RunTiny_Game();
                } else if (menu_index == 5) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 1; // Return to "Games" selection
                    last_menu_index = -1;
                }
                rebuild = true;
            }
            
            // Render Games Menu
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047); 
                
                int start_y = 1600; 
                int spacing = 400;
                int scale = 40; 
                
                for (int i = 0; i < games_menu_count; i++) {
                    int y = start_y - (i * spacing);
                    if (i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                    }
                    DRAW_AddString(games_menu_items[i], 0, 250, y, scale, scale);
                }
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_MUSIC_PLAYER) {
            // Volume Control (Encoder)
            if (enc_delta != 0) {
                if (music_menu_active) {
                    music_selected_index += enc_delta;
                    if (music_selected_index < 0) music_selected_index = music_file_count - 1;
                    if (music_selected_index >= music_file_count) music_selected_index = 0;
                    rebuild = true;
                } else {
                    // Adjust Volume
                    int new_vol = audioVolume + enc_delta * 5;
                    if (new_vol < 0) new_vol = 0;
                    if (new_vol > 100) new_vol = 100;
                    audioVolume = new_vol;
                    rebuild = true; // Redraw volume
                }
            }
            
            // Button
            if (btn_pressed) {
                if (music_menu_active) {
                    // Select File
                    if (music_file_count > 0) {
                        if (OpenMusicFile(music_files[music_selected_index])) {
                            music_menu_active = false;
                            music_is_playing = true;
                            
                            // Pre-fill buffer
                            int prefill_count = 0;
                            uint8_t chunkBuf[512];
                            while (!audioBufferFull() && music_file.available() && prefill_count < AUDIO_BUFFER_SIZE - 100) {
                                int bytesToRead = 512;
                                int bytesRead = music_file.read(chunkBuf, bytesToRead);
                                if (bytesRead > 0) {
                                    for (int i = 0; i < bytesRead; i += (music_channels == 2 ? 4 : 2)) {
                                        if (audioBufferFull()) break;
                                        
                                        int16_t left, right;
                                        if (music_channels == 2) {
                                            if (i + 3 < bytesRead) {
                                                left = (int16_t)(chunkBuf[i] | (chunkBuf[i+1] << 8));
                                                right = (int16_t)(chunkBuf[i+2] | (chunkBuf[i+3] << 8));
                                            } else break;
                                        } else {
                                            if (i + 1 < bytesRead) {
                                                left = (int16_t)(chunkBuf[i] | (chunkBuf[i+1] << 8));
                                                right = left;
                                            } else break;
                                        }
                                        uint16_t dac_l = ((int32_t)left * audioVolume / 100) + 32768;
                                        uint16_t dac_r = ((int32_t)right * audioVolume / 100) + 32768;
                                        uint32_t packed = dac_l | (dac_r << 16);
                                        audioBuffer[bufferHead] = packed;
                                        bufferHead = (bufferHead + 1) % AUDIO_BUFFER_SIZE;
                                        prefill_count++;
                                    }
                                } else {
                                    break;
                                }
                            }
                            
                            audioPlaying = true;
                            // Set DAC Freq to match audio
                            setDACFreq(music_sample_rate); 
                        }
                    } else {
                        // No files, exit
                        ui_state = UI_MENU_MAIN;
                        rebuild = true;
                    }
                } else {
                    // Stop / Back
                    music_is_playing = false;
                    audioPlaying = false;
                    if(music_file) music_file.close();
                    music_menu_active = true;
                    rebuild = true;
                    // Restore default DAC freq
                    setDACFreq(30000);
                }
            }
            
            // Playback Logic
            if (music_is_playing && music_file) {
                // Fill Buffer - Read in chunks for efficiency
                int bytes_processed = 0;
                uint8_t chunkBuf[512];
                
                while (!audioBufferFull() && music_file.available() && bytes_processed < 4096) {
                    int bytesToRead = 512;
                    int bytesRead = music_file.read(chunkBuf, bytesToRead);
                    
                    if (bytesRead > 0) {
                        bytes_processed += bytesRead;
                        for (int i = 0; i < bytesRead; i += (music_channels == 2 ? 4 : 2)) {
                             if (audioBufferFull()) break;
                             
                             int16_t left, right;
                             if (music_channels == 2) {
                                 if (i + 3 < bytesRead) {
                                     left = (int16_t)(chunkBuf[i] | (chunkBuf[i+1] << 8));
                                     right = (int16_t)(chunkBuf[i+2] | (chunkBuf[i+3] << 8));
                                 } else break;
                             } else {
                                 if (i + 1 < bytesRead) {
                                     left = (int16_t)(chunkBuf[i] | (chunkBuf[i+1] << 8));
                                     right = left;
                                 } else break;
                             }
                             
                             uint16_t dac_l = ((int32_t)left * audioVolume / 100) + 32768;
                             uint16_t dac_r = ((int32_t)right * audioVolume / 100) + 32768;
                             uint32_t packed = dac_l | (dac_r << 16);
                             audioBuffer[bufferHead] = packed;
                             bufferHead = (bufferHead + 1) % AUDIO_BUFFER_SIZE;
                        }
                    } else {
                        // End of file
                        music_is_playing = false;
                        audioPlaying = false;
                        music_menu_active = true;
                        rebuild = true;
                        setDACFreq(30000); // Restore default
                        break;
                    }
                }
            }
            
            // Render
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                if (music_menu_active) {
                    DRAW_AddString("SELECT MUSIC", 0, 500, 1800, 20, 20);
                    if (music_file_count == 0) {
                        DRAW_AddString("NO FILES", 0, 600, 1000, 20, 20);
                    } else {
                        int start_y = 1500;
                        for (int i = 0; i < 5; i++) { // Show 5 files
                            int idx = (music_selected_index - 2 + i + music_file_count) % music_file_count;
                            int y = start_y - (i * 250);
                            if (i == 2) { // Center item
                                DRAW_AddString(">", 0, 50, y, 25, 25);
                                DRAW_AddString(music_files[idx], 0, 200, y, 25, 25);
                            } else {
                                DRAW_AddString(music_files[idx], 0, 200, y, 15, 15);
                            }
                        }
                    }
                } else {
                    // Playing Screen
                    DRAW_AddString("PLAYING", 0, 600, 1800, 20, 20);
                    DRAW_AddString(music_files[music_selected_index], 0, 100, 1400, 20, 20);
                    
                    char vol_str[32];
                    sprintf(vol_str, "VOL: %d%%", audioVolume);
                    DRAW_AddString(vol_str, 0, 600, 800, 25, 25);
                    
                    DRAW_AddString("[BTN] STOP", 0, 600, 400, 15, 15);
                }
                last_menu_index = 0;
            }
            
        } else if (ui_state == UI_SNAKE) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
            }
            
            Update_Snake_Game();
            
            // Always redraw game
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Snake
            int cell_size = 2048 / SNAKE_GRID_SIZE;
            for(int i=0; i<snake_len; i++) {
                int x = snake_body[i].x * cell_size + (cell_size/2);
                int y = snake_body[i].y * cell_size + (cell_size/2);
                int half_size = (cell_size / 2) - 5;
                DRAW_AddRect(x - half_size, y - half_size, 2*half_size, 2*half_size);
            }
            
            // Draw Food
            int fx = snake_food.x * cell_size + (cell_size/2);
            int fy = snake_food.y * cell_size + (cell_size/2);
            int f_half = (cell_size / 2) - 10;
            DRAW_AddLine(fx - f_half, fy - f_half, fx + f_half, fy + f_half);
            DRAW_AddLine(fx - f_half, fy + f_half, fx + f_half, fy - f_half);
            
            if(game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", game_score);
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            }

        } else if (ui_state == UI_BREAKOUT) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
            }
            
            Update_Breakout_Game(enc_delta);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Paddle
            int py = 100;
            DRAW_AddRect(brk_paddle.x, py, BRK_PADDLE_W, BRK_PADDLE_H);
            
            // Draw Ball
            DRAW_AddCircle(brk_ball.x, brk_ball.y, BRK_BALL_R);
            
            // Draw Bricks
            int brick_start_y = 1500;
            for(int r=0; r<BRK_ROWS; r++) {
                for(int c=0; c<BRK_COLS; c++) {
                    if(brk_bricks[r][c]) {
                        int bx = c * BRK_BRICK_W;
                        int by = brick_start_y + (r * BRK_BRICK_H);
                        int bw = BRK_BRICK_W;
                        int bh = BRK_BRICK_H;
                        
                        // Simple Brick Drawing (Rect)
                        DRAW_AddRect(bx + 2, by + 2, bw - 4, bh - 4);
                    }
                }
            }
            
            // Draw Score/Lives
            if(brk_game_over) {
                if(brk_game_over == 2) {
                    DRAW_AddString("YOU WIN", 0, 600, 1100, 20, 20);
                } else {
                    DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                }
                char score_str[32];
                sprintf(score_str, "SCORE: %d", brk_score);
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            } else {
                char lives_str[16];
                sprintf(lives_str, "L:%d", brk_lives);
                DRAW_AddString(lives_str, 0, 50, 50, 10, 10);
            }

        } else if (ui_state == UI_FLAPPY) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
            }
            
            // Input
            bool current_touch_up = (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA);
            int jump = 0;
            if (current_touch_up && !last_touch_up) {
                jump = 1;
            }
            last_touch_up = current_touch_up;
            
            Update_Flappy_Game(jump);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Player
            DRAW_AddCircle(FLP_PLAYER_X, flp_player.y, FLP_PLAYER_R);
            // Antennas
            DRAW_AddLine(FLP_PLAYER_X - 20, flp_player.y + 30, FLP_PLAYER_X - 40, flp_player.y + 75);
            DRAW_AddLine(FLP_PLAYER_X + 20, flp_player.y + 30, FLP_PLAYER_X + 40, flp_player.y + 75);
            
            // Draw Obstacles
            for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
                if(flp_obstacles[i].active) {
                    int x = flp_obstacles[i].x;
                    int gap_y = flp_obstacles[i].gap_y;
                    int w = FLP_OBSTACLE_W;
                    int h_gap = FLP_GAP_H / 2;
                    
                    // Top Obstacle
                    int top_y = gap_y + h_gap;
                    if(top_y < 2048) {
                        DRAW_AddRect(x, top_y, w, 2048 - top_y);
                    }
                    
                    // Bottom Obstacle
                    int bot_y = gap_y - h_gap;
                    if(bot_y > 0) {
                        DRAW_AddRect(x, 0, w, bot_y);
                    }
                }
            }
            
            // Score
            if(flp_game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", flp_score);
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            } else {
                char score_str[32];
                sprintf(score_str, "SCORE: %d", flp_score);
                DRAW_AddString(score_str, 0, 50, 1900, 10, 10);
            }

        } else if (ui_state == UI_RACING) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
            }
            
            Update_Racing_Game(enc_delta);
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Car
            DRAW_AddRect(race_car.x, 100, RACE_CAR_W, RACE_CAR_H);
            // Add some detail to car (wheels)
            DRAW_AddRect(race_car.x - 25, 125, 25, 75);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 125, 25, 75);
            DRAW_AddRect(race_car.x - 25, 250, 25, 75);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 250, 25, 75);
            
            // Draw Obstacles
            for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
                if(race_obstacles[i].active) {
                    DRAW_AddRect(race_obstacles[i].x, race_obstacles[i].y, RACE_OBSTACLE_W, RACE_OBSTACLE_H);
                    // Draw X inside obstacle
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y + RACE_OBSTACLE_H);
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y + RACE_OBSTACLE_H, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y);
                }
            }
            
            if(race_game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", race_score);
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", race_score);
                DRAW_AddString(score_str, 0, 50, 1900, 10, 10);
            }

        } else if (ui_state == UI_RUNTINY) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
            }
            
            // Input
            bool current_touch_up = (touchRead(TOUCH_UP) > touch_base_up + TOUCH_DELTA);
            int jump = 0;
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
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", run_score);
                DRAW_AddString(score_str, 0, 50, 1900, 10, 10);
            }

        } else if (ui_state == UI_ABOUT) {
             if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1;
            }
            
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(50, 50, 1948, 1948);
                DRAW_AddString("ABOUT", 0, 780, 1800, 17, 17);
                DRAW_AddString("ESP32 VECTOR", 0, 610, 1300, 12, 12);
                DRAW_AddString("DISPLAY SYSTEM", 0, 540, 1050, 12, 12);
                DRAW_AddString("V1.0", 0, 885, 800, 12, 12);
                last_menu_index = 0; // Mark as drawn
            }
        }

        // Update animations and Render
        if (!(ui_state == UI_MUSIC_PLAYER && music_is_playing)) {
            DRAW_Update();
            DRAW_Render();
        }

        if (ui_state == UI_MUSIC_PLAYER && music_is_playing) {
             vTaskDelay(pdMS_TO_TICKS(10)); // Faster update for audio filling
        } else {
             vTaskDelay(pdMS_TO_TICKS(40)); // 25Hz update rate
        }
    }
}

static void serialOutputTask(void* pvParameters) {
  unsigned long lastOutputTime = 0;
  const unsigned long outputInterval = 200; // 200ms interval
  
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