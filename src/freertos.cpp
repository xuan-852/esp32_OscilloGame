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

// External variables from main.cpp
extern volatile int32_t encoderValue;

// Task Handles
static TaskHandle_t s_serialOutputTaskHandle = nullptr;
static TaskHandle_t s_guiTaskHandle = nullptr;

// --- Music Player Variables ---
static std::vector<std::string> music_files;
static int music_file_count = 0;
static bool is_playing = false;
static File audioFile;
static uint16_t music_channels = 2; // 1: Mono, 2: Stereo

// --- Video Player Variables ---
static std::vector<std::string> video_files;
static int video_file_count = 0;
static bool is_video_playing = false;
static File videoFile;
static uint16_t video_channels = 4; // Should be 4


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

// --- Tank Game Variables ---
#define TANK_MAX_BULLETS 5
#define TANK_SPEED 15.0f
#define TANK_TURN_SPEED 0.15f
#define TANK_BULLET_SPEED 30.0f

typedef struct {
    float x, y;
    float angle; // Radians
} Tank;

typedef struct {
    float x, y;
    float vx, vy;
    bool active;
} Bullet;

static Tank my_tank;
static Bullet tank_bullets[TANK_MAX_BULLETS];
static int tank_game_over = 0;
static unsigned long last_fire_time = 0;

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

  // Initialize Web Server on Core 0
  initWebServer();
}

#include "network_manager.h"

// --- GUI Logic ---

enum UI_State {
    UI_MENU_MAIN,
    UI_MENU_GAMES,
    UI_MENU_MUSIC,
    UI_MUSIC_PLAYER,
    UI_MENU_VIDEO,
    UI_VIDEO_PLAYER,
    UI_MENU_ONLINE, // New Online Mode
    UI_SNAKE,
    UI_BREAKOUT,
    UI_FLAPPY,
    UI_RACING,
    UI_RUNTINY,
    UI_TANK, // New Tank Game
    UI_ABOUT
};

static const char* main_menu_items[] = {
    "Music",
    "Video",
    "Games",
    "Online", // New Item
    "Settings",
    "About"
};
static const int main_menu_count = 6;

static const char* games_menu_items[] = {
    "Snake",
    "Breakout",
    "Flappy",
    "Racing",
    "RunTiny",
    "Tank", // New Game
    "Back"
};
static const int games_menu_count = 7;

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

// --- Tank Game Logic ---
void Init_Tank_Game(void) {
    // Check Connection
    if(Network_Manager::getState() != NET_CONNECTED && Network_Manager::getState() != NET_IN_GAME) {
        tank_game_over = 2; // Not Connected
        return;
    }

    // If we are the initiator (not already in game), send Start Game
    if(Network_Manager::getState() == NET_CONNECTED) {
        Network_Manager::startGame(1); // 1 = Tank
    }

    my_tank.x = 1024;
    my_tank.y = 1024;
    my_tank.angle = -1.5707f; // Face Up (-PI/2)
    
    for(int i=0; i<TANK_MAX_BULLETS; i++) {
        tank_bullets[i].active = false;
    }
    tank_game_over = 0;
    Network_Manager::clearRemoteGameEnded();
}

void Update_Tank_Game(void) {
    if(tank_game_over) return;

    // Check Remote Exit
    if(Network_Manager::isRemoteGameEnded()) {
        tank_game_over = 3; // Remote Disconnected
        return;
    }

    // Inputs
    // Left Stick Y (JOY1_Y) - Forward/Back
    // Right Stick X (JOY2_X) - Rotate
    // Button A (JOY_A) - Fire
    
    int joy1_y = analogRead(JOY1_Y); // 0-4095
    int joy2_x = analogRead(JOY2_X); // 0-4095
    int btn_a = !digitalRead(JOY_A);  // Released = 1 (HIGH), Pressed = 0 (LOW)
    
    // Deadzone & Mapping
    float speed = 0;
    if(abs(joy1_y - 2048) > 300) {
        // Assuming Up (Low val) -> Forward
        // 2048 - val: Positive if val < 2048 (Up)
        speed = (2048 - joy1_y) / 2048.0f * TANK_SPEED;
    }
    
    float turn = 0;
    if(abs(joy2_x - 2048) > 300) {
        // Assuming Right (High val) -> Clockwise
        turn = (joy2_x - 2048) / 2048.0f * TANK_TURN_SPEED;
    }
    
    // Update Tank
    my_tank.angle += turn;
    my_tank.x += speed * cos(my_tank.angle);
    my_tank.y += speed * sin(my_tank.angle);
    
    // Boundary
    if(my_tank.x < 50) my_tank.x = 50;
    if(my_tank.x > 1997) my_tank.x = 1997;
    if(my_tank.y < 50) my_tank.y = 50;
    if(my_tank.y > 1997) my_tank.y = 1997;
    
    // Fire (Pressed = LOW)
    if(btn_a == LOW && millis() - last_fire_time > 250) { 
        last_fire_time = millis();
        for(int i=0; i<TANK_MAX_BULLETS; i++) {
            if(!tank_bullets[i].active) {
                tank_bullets[i].active = true;
                // Spawn at tip of barrel (approx 40 units)
                tank_bullets[i].x = my_tank.x + 40 * cos(my_tank.angle); 
                tank_bullets[i].y = my_tank.y + 40 * sin(my_tank.angle);
                tank_bullets[i].vx = TANK_BULLET_SPEED * cos(my_tank.angle);
                tank_bullets[i].vy = TANK_BULLET_SPEED * sin(my_tank.angle);
                break;
            }
        }
    }
    
    // Update Bullets
    for(int i=0; i<TANK_MAX_BULLETS; i++) {
        if(tank_bullets[i].active) {
            tank_bullets[i].x += tank_bullets[i].vx;
            tank_bullets[i].y += tank_bullets[i].vy;
            
            if(tank_bullets[i].x < 0 || tank_bullets[i].x > 2047 ||
               tank_bullets[i].y < 0 || tank_bullets[i].y > 2047) {
                tank_bullets[i].active = false;
            }
        }
    }

    // Network Sync (100Hz approx, called every frame which is 25Hz in main loop, but we want faster?)
    // Main loop delay is 40ms (25Hz). We should send every frame.
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

// --- Music Logic ---
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
    
    // Read WAV Header
    uint8_t header[44];
    if(audioFile.read(header, 44) != 44) {
        Serial.println("Failed to read WAV header");
        audioFile.close();
        return false;
    }
    
    // Check RIFF
    if(header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
        Serial.println("Not a RIFF file");
        audioFile.close();
        return false;
    }
    
    // Get Sample Rate (Offset 24, 4 bytes)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    Serial.printf("Sample Rate: %d\n", sampleRate);
    
    // Get Channels (Offset 22, 2 bytes)
    uint16_t channels = header[22] | (header[23] << 8);
    Serial.printf("Channels: %d\n", channels);
    music_channels = channels;
    
    // Get Bits Per Sample (Offset 34, 2 bytes)
    uint16_t bitsPerSample = header[34] | (header[35] << 8);
    Serial.printf("Bits: %d\n", bitsPerSample);
    
    if(bitsPerSample != 16) {
        Serial.println("Only 16-bit WAV supported");
        audioFile.close();
        return false;
    }
    
    Serial.printf("File Size: %d\n", audioFile.size());
    
    is_playing = true;
    Set_Player_Mode(1); // 1=Audio
    setDACFreq(sampleRate);
    return true;
}

void Stop_Music() {
    is_playing = false;
    if(audioFile) audioFile.close();
    Set_Player_Mode(0); // 0=Vector
}

// --- Video Logic ---
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
    
    // Read WAV Header
    uint8_t header[44];
    if(videoFile.read(header, 44) != 44) {
        Serial.println("Failed to read WAV header");
        videoFile.close();
        return false;
    }
    
    // Check RIFF
    if(header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
        Serial.println("Not a RIFF file");
        videoFile.close();
        return false;
    }
    
    // Get Sample Rate (Offset 24, 4 bytes)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    Serial.printf("Sample Rate: %d\n", sampleRate);
    
    // Get Channels (Offset 22, 2 bytes)
    uint16_t channels = header[22] | (header[23] << 8);
    Serial.printf("Channels: %d\n", channels);
    video_channels = channels;
    
    // Get Bits Per Sample (Offset 34, 2 bytes)
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
    Set_Player_Mode(2); // 2=Video
    setDACFreq(sampleRate);
    return true;
}

void Stop_Video() {
    is_video_playing = false;
    if(videoFile) videoFile.close();
    Set_Player_Mode(0); // 0=Vector
}

static void guiTask(void* pvParameters) {
    // Wait for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Calibration
    int touch_base_up = 0;
    int touch_base_down = 0;
    int touch_base_left = 0;
    int touch_base_right = 0;
    
    // Take samples
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
    
    // Define a delta
    // Idle ~30000, Pressed > 70000.
    // If calibration fails (0), 35000 is safe (30000 < 35000).
    // If calibration works (30000), Threshold 65000. Pressed (70000+) > 65000.
    const int TOUCH_DELTA = 35000; 

    // Initialize Network
    Network_Manager::init();

    UI_State ui_state = UI_MENU_MAIN;
    int menu_index = 0;
    int last_menu_index = -1;
    int32_t last_encoder = 0;
    
    // Debounce for button
    int last_btn_state = HIGH;
    unsigned long last_btn_time = 0;
    
    // Oscilloscope state
    int32_t osc_last_val = -999999;

    // Music state
    int music_scroll = 0;
    const int visible_lines = 5;

    // Flappy state
    bool last_touch_up = false;

    for (;;) {
        // 1. Handle Input
        int32_t current_encoder = encoderValue / 4;
        int16_t enc_delta = current_encoder - last_encoder;
        last_encoder = current_encoder;
        
        // Web Input Injection
        if (web_enc_delta != 0) {
            enc_delta += web_enc_delta;
            web_enc_delta = 0;
        }
        
        int btn_state = digitalRead(EN_S);
        bool btn_pressed = false;
        
        // Trigger on Release (Rising Edge) to prevent holding issues
        if (btn_state == HIGH && last_btn_state == LOW && (millis() - last_btn_time > 50)) {
            btn_pressed = true;
        }
        
        if (web_btn_pressed) {
            btn_pressed = true;
            web_btn_pressed = false;
        }

        // Record time when button goes LOW
        if (btn_state == LOW && last_btn_state == HIGH) {
            last_btn_time = millis();
        }
        last_btn_state = btn_state;

        // Touch Input for Snake
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

        // 2. State Machine
        bool rebuild = false;

        // Global Game Request Check (for non-blocking states)
        uint8_t reqGameId;
        if(Network_Manager::hasGameRequest(&reqGameId)) {
            Network_Manager::clearGameRequest();
            if(reqGameId == 1) { // Tank
                ui_state = UI_TANK;
                Init_Tank_Game();
                rebuild = true;
            }
        }

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
                    menu_index = 0; // Reset for submenu
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 3) {
                    ui_state = UI_MENU_ONLINE;
                    Network_Manager::startDiscovery();
                    menu_index = 0;
                    last_menu_index = -1;
                    continue;
                } else if (menu_index == 5) {
                    ui_state = UI_ABOUT;
                    last_menu_index = -1; // Ensure redraw
                    continue;
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
                    Init_Tank_Game();
                    continue;
                } else if (menu_index == 6) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 1; // Return to "Games" selection
                    last_menu_index = -1;
                    continue;
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

        } else if (ui_state == UI_MENU_MUSIC) {
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = music_file_count; // +1 for Back
                if (menu_index > music_file_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
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
                        // Failed to play, stay in menu
                        // Maybe show error?
                    }
                }
                rebuild = true;
            }
            
            // Render Music Menu
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                int start_y = 1600;
                int spacing = 300;
                int scale = 30;
                
                // Calculate Scroll
                if(menu_index < music_scroll) music_scroll = menu_index;
                if(menu_index >= music_scroll + visible_lines) music_scroll = menu_index - visible_lines + 1;
                
                for(int i=0; i<visible_lines; i++) {
                    int item_idx = music_scroll + i;
                    int y = start_y - (i * spacing);
                    
                    if(item_idx <= music_file_count) {
                        if(item_idx == menu_index) {
                            DRAW_AddString(">", 0, 50, y, scale, scale);
                        }
                        
                        if(item_idx == music_file_count) {
                            DRAW_AddString("Back", 0, 250, y, scale, scale);
                        } else {
                            DRAW_AddString(music_files[item_idx].c_str(), 0, 250, y, scale, scale);
                        }
                    }
                }
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_MUSIC_PLAYER) {
            // Exclusive Audio Loop - Blocking Mode
            // This ensures maximum CPU time for buffer filling and prevents UI starvation
            
            // Clear screen once
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                // Draw a simple "Playing" indicator
                DRAW_AddString("PLAYING...", 0, 500, 1000, 20, 20);
                DRAW_Update(); 
                last_menu_index = 0;
                // Removed "Wait for release" loop as we now enter on Release
            }
            
            // Safety check: If not playing, go back
            if (!is_playing) {
                ui_state = UI_MENU_MUSIC;
                rebuild = true;
            }

            // Enter blocking loop
            Serial.println("Entering Music Loop");
            
            // Reset debounce timer
            unsigned long low_start = 0;
            
            // Allocate a 64KB temp buffer in PSRAM for reading
            // 64KB = 65536 bytes
            uint8_t *read_buf = (uint8_t*)ps_malloc(65536);
            if(read_buf == NULL) {
                Serial.println("Failed to allocate 64KB read buffer!");
                is_playing = false;
            }

            // Volume Control Init
            static int volume = 1; // 0-256. Default 1/256
            int32_t last_enc = encoderValue;
            
            while (is_playing && audioFile) {
                // Check for Game Request
                uint8_t reqGameId;
                if(Network_Manager::hasGameRequest(&reqGameId)) {
                    Network_Manager::clearGameRequest();
                    if(reqGameId == 1) { // Tank
                        Stop_Music();
                        ui_state = UI_TANK;
                        Init_Tank_Game();
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
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = video_file_count; // +1 for Back
                if (menu_index > video_file_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
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
                        // Failed to play
                    }
                }
                rebuild = true;
            }
            
            // Render Video Menu
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                int start_y = 1600;
                int spacing = 300;
                int scale = 30;
                
                // Calculate Scroll
                if(menu_index < music_scroll) music_scroll = menu_index;
                if(menu_index >= music_scroll + visible_lines) music_scroll = menu_index - visible_lines + 1;
                
                for(int i=0; i<visible_lines; i++) {
                    int item_idx = music_scroll + i;
                    int y = start_y - (i * spacing);
                    
                    if(item_idx <= video_file_count) {
                        if(item_idx == menu_index) {
                            DRAW_AddString(">", 0, 50, y, scale, scale);
                        }
                        
                        if(item_idx == video_file_count) {
                            DRAW_AddString("Back", 0, 250, y, scale, scale);
                        } else {
                            DRAW_AddString(video_files[item_idx].c_str(), 0, 250, y, scale, scale);
                        }
                    }
                }
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_VIDEO_PLAYER) {
            // Exclusive Video Loop
            
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddString("PLAYING VIDEO...", 0, 500, 1000, 20, 20);
                DRAW_Update(); 
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

            // Volume Control Init (Only for Audio Channels)
            static int volume = 1; 
            int32_t last_enc = encoderValue;
            
            while (is_video_playing && videoFile) {
                // Check for Game Request
                uint8_t reqGameId;
                if(Network_Manager::hasGameRequest(&reqGameId)) {
                    Network_Manager::clearGameRequest();
                    if(reqGameId == 1) { // Tank
                        Stop_Video();
                        ui_state = UI_TANK;
                        Init_Tank_Game();
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
            // Update Network
            Network_Manager::update();
            
            // Get Peers
            int peer_count = Network_Manager::getPeerCount();
            const PeerInfo* peers = Network_Manager::getPeers();
            
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = peer_count; // +1 for Back
                if (menu_index > peer_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
            if (btn_pressed) {
                if (menu_index == peer_count) {
                    ui_state = UI_MENU_MAIN;
                    menu_index = 3; // Return to "Online" selection
                    last_menu_index = -1;
                    continue;
                } else {
                    // Pair with selected peer
                    if(peer_count > 0 && menu_index < peer_count) {
                        Network_Manager::pair(peers[menu_index].mac);
                    }
                }
                rebuild = true;
            }
            
            // Force redraw every 500ms to update status text
            static unsigned long last_redraw = 0;
            if (millis() - last_redraw > 500) {
                rebuild = true;
                last_redraw = millis();
            }

            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047);
                
                DRAW_AddString("ONLINE MODE", 0, 600, 1900, 30, 30);
                
                // Show Status
                NetState state = Network_Manager::getState();
                const char* status_str = "IDLE";
                if(state == NET_DISCOVERING) status_str = "SCANNING...";
                else if(state == NET_PAIRING) status_str = "PAIRING...";
                else if(state == NET_CONNECTED) status_str = "CONNECTED";
                
                DRAW_AddString(status_str, 0, 50, 1800, 20, 20);
                
                int start_y = 1600;
                int spacing = 200;
                int scale = 25;
                
                // List Peers
                for(int i=0; i<peer_count; i++) {
                    int y = start_y - (i * spacing);
                    
                    if(i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                    }
                    
                    char peer_str[32];
                    sprintf(peer_str, "PEER %02X:%02X", peers[i].mac[4], peers[i].mac[5]);
                    DRAW_AddString(peer_str, 0, 200, y, scale, scale);
                }
                
                // Back Option
                int back_y = start_y - (peer_count * spacing);
                if(menu_index == peer_count) {
                    DRAW_AddString(">", 0, 50, back_y, scale, scale);
                }
                DRAW_AddString("BACK", 0, 200, back_y, scale, scale);
                
                last_menu_index = menu_index;
            }

        } else if (ui_state == UI_SNAKE) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
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
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            }

        } else if (ui_state == UI_BREAKOUT) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
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
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            } else {
                char lives_str[16];
                sprintf(lives_str, "L:%d", brk_lives);
                DRAW_AddString(lives_str, 0, 50, 50, 30, 30);
            }

        } else if (ui_state == UI_FLAPPY) {
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
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            } else {
                char score_str[32];
                sprintf(score_str, "SCORE: %d", flp_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
            }

        } else if (ui_state == UI_RACING) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                continue;
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
                DRAW_AddString(score_str, 0, 300, 800, 30, 30);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", race_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
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
            } else {
                char score_str[16];
                sprintf(score_str, "%d", run_score);
                DRAW_AddString(score_str, 0, 50, 1900, 20, 20);
            }

        } else if (ui_state == UI_TANK) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_GAMES;
                rebuild = true;
                last_menu_index = -1;
                Network_Manager::endGame();
                continue;
            }
            
            Update_Tank_Game();
            
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Tank
            // Body (Rect rotated)
            float cos_a = cos(my_tank.angle);
            float sin_a = sin(my_tank.angle);
            
            // Tank Dimensions
            float w = 60;
            float h = 80;
            
            // 4 Corners relative to center
            float x1 = -w/2, y1 = -h/2;
            float x2 = w/2, y2 = -h/2;
            float x3 = w/2, y3 = h/2;
            float x4 = -w/2, y4 = h/2;
            
            // Rotate and Translate
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
            
            // Turret (Box)
            float tw = 30, th = 30;
            float tx1 = -tw/2, ty1 = -th/2;
            float tx2 = tw/2, ty2 = -th/2;
            float tx3 = tw/2, ty3 = th/2;
            float tx4 = -tw/2, ty4 = th/2;
            
            DRAW_AddLine(rotX(tx1, ty1), rotY(tx1, ty1), rotX(tx2, ty2), rotY(tx2, ty2));
            DRAW_AddLine(rotX(tx2, ty2), rotY(tx2, ty2), rotX(tx3, ty3), rotY(tx3, ty3));
            DRAW_AddLine(rotX(tx3, ty3), rotY(tx3, ty3), rotX(tx4, ty4), rotY(tx4, ty4));
            DRAW_AddLine(rotX(tx4, ty4), rotY(tx4, ty4), rotX(tx1, ty1), rotY(tx1, ty1));
            
            // Barrel (Line)
            DRAW_AddLine(my_tank.x, my_tank.y, my_tank.x + 60*cos_a, my_tank.y + 60*sin_a);
            
            // Draw Bullets
            for(int i=0; i<TANK_MAX_BULLETS; i++) {
                if(tank_bullets[i].active) {
                    DRAW_AddCircle(tank_bullets[i].x, tank_bullets[i].y, 5);
                }
            }

            // Draw Remote Tank
            TankData remoteData;
            if(Network_Manager::getRemoteGameData(&remoteData)) {
                float r_cos = cos(remoteData.angle);
                float r_sin = sin(remoteData.angle);
                
                // Remote Tank Body
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
                
                // Remote Turret
                DRAW_AddLine(rRotX(tx1, ty1), rRotY(tx1, ty1), rRotX(tx2, ty2), rRotY(tx2, ty2));
                DRAW_AddLine(rRotX(tx2, ty2), rRotY(tx2, ty2), rRotX(tx3, ty3), rRotY(tx3, ty3));
                DRAW_AddLine(rRotX(tx3, ty3), rRotY(tx3, ty3), rRotX(tx4, ty4), rRotY(tx4, ty4));
                DRAW_AddLine(rRotX(tx4, ty4), rRotY(tx4, ty4), rRotX(tx1, ty1), rRotY(tx1, ty1));
                
                // Remote Barrel
                DRAW_AddLine(remoteData.x, remoteData.y, remoteData.x + 60*r_cos, remoteData.y + 60*r_sin);
                
                // Remote Bullets
                for(int i=0; i<remoteData.bullet_count; i++) {
                    DRAW_AddCircle(remoteData.bullets[i].x, remoteData.bullets[i].y, 5);
                }
            }
            
            if(tank_game_over) {
                if(tank_game_over == 2) {
                    DRAW_AddString("NOT CONNECTED", 0, 400, 1100, 20, 20);
                } else if(tank_game_over == 3) {
                    DRAW_AddString("OPPONENT LEFT", 0, 400, 1100, 20, 20);
                }
                
                // Auto exit after 2 seconds
                static unsigned long exit_timer = 0;
                if(exit_timer == 0) exit_timer = millis();
                if(millis() - exit_timer > 2000) {
                    ui_state = UI_MENU_GAMES;
                    rebuild = true;
                    last_menu_index = -1;
                    exit_timer = 0;
                    Network_Manager::endGame();
                }
            }

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
                last_menu_index = 0; // Mark as drawn
            }
        }

        // Update animations and Render
        DRAW_Update();
        DRAW_Render();

        vTaskDelay(pdMS_TO_TICKS(40)); // 25Hz update rate
    }
}

static void serialOutputTask(void* pvParameters) {
  unsigned long lastOutputTime = 0;
  const unsigned long outputInterval = 200; // 200ms interval
  
  for (;;) {
    if (millis() - lastOutputTime >= outputInterval) {
      lastOutputTime = millis();
      /* Serial.printf("Touch(Cap): U=%d D=%d L=%d R=%d\n", 
        touchRead(TOUCH_UP), 
        touchRead(TOUCH_DOWN), 
        touchRead(TOUCH_LEFT), 
        touchRead(TOUCH_RIGHT)); */
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}