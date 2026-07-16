#pragma once
#include <Arduino.h>
#include <vector>
#include <string>

// Message Types
enum NetMsgType {
    MSG_DISCOVERY = 0,
    MSG_PAIR_REQUEST,
    MSG_PAIR_ACCEPT,
    MSG_PAIR_REJECT,
    MSG_DISCONNECT,
    MSG_DATA,
    MSG_START_GAME,
    MSG_GAME_DATA,
    MSG_END_GAME
};

// Game Data Structure
struct TankData {
    float x, y, angle;
    uint8_t bullet_count;
    struct {
        float x, y;
    } bullets[5];
};

// --- ESP-NOW 无线手柄数据 ---
extern const uint8_t GAMEPAD_SLAVE_MAC[6];
#define GAMEPAD_TIMEOUT_MS 500  // 手柄超时判定 (ms)

typedef struct __attribute__((packed)) {
    uint16_t joy1X;     // 左摇杆 X (0-4095)
    uint16_t joy1Y;     // 左摇杆 Y
    uint16_t joy2X;     // 右摇杆 X
    uint16_t joy2Y;     // 右摇杆 Y
    uint8_t  btnA;      // 按钮 A (0/1)
    uint8_t  btnB;      // 按钮 B (0/1)
    uint8_t  btn1SW;    // 摇杆1 按下
    uint8_t  btn2SW;    // 摇杆2 按下
    int16_t  encDelta;  // 编码器增量 (可选)
    uint8_t  dirPad;    // 方向键: 0=上 1=下 2=左 3=右 255=无
} GamepadData;

// Fixed size packet for simplicity
typedef struct {
    uint8_t type;
    uint8_t src_mac[6];
    char name[16]; // "Player_XXXX"
    union {
        uint8_t padding[8];
        TankData tank_data;
        struct {
            uint8_t game_id;
            uint32_t seed;
        } start_req;
        struct {
            uint8_t reason; // 0: Quit, 1: Died
        } end_req;
    } payload;
} NetMessage;

// Peer Info
struct PeerInfo {
    uint8_t mac[6];
    String name;
    unsigned long last_seen;
};

enum NetState {
    NET_IDLE,
    NET_DISCOVERING,
    NET_PAIRING,
    NET_CONNECTED,
    NET_IN_GAME
};

class Network_Manager {
public:
    static void init();
    static void disable();
    static void enable();
    static void update();
    static void startDiscovery();
    static void stopDiscovery();
    static void pair(const uint8_t* target_mac);
    static void disconnect();
    
    // Game Methods
    static void startGame(uint8_t gameId, uint32_t seed);
    static void sendGameData(const TankData& data);
    static void endGame(uint8_t reason);
    
    static bool hasGameRequest(uint8_t* gameIdOut, uint32_t* seedOut);
    static void clearGameRequest();
    static bool getRemoteGameData(TankData* dataOut);
    static bool isRemoteGameEnded(uint8_t* reasonOut);
    static void clearRemoteGameEnded();

    // --- 无线手柄 API ---
    static bool isGamepadConnected();           // 手柄是否在线
    static bool getGamepadData(GamepadData* out); // 获取最新手柄数据

    static NetState getState();
    static int getPeerCount();
    static const PeerInfo* getPeers();
};
