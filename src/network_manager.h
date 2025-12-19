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

    static NetState getState();
    static int getPeerCount();
    static const PeerInfo* getPeers();
};
