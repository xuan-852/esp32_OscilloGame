#include "network_manager.h"
#include <WiFi.h>
#include <esp_now.h>

// 网络状态变量
static NetState current_state = NET_IDLE;                    // 当前网络状态
static std::vector<PeerInfo> discovered_peers;              // 发现的设备列表
static uint8_t my_mac[6];                                   // 本机MAC地址
static char my_name[16];                                    // 本机名称

// 连接详情
static uint8_t connected_peer_mac[6];                       // 已连接设备的MAC地址
static uint8_t pending_peer_mac[6];                         // 待处理连接设备的MAC地址
static unsigned long last_broadcast_time = 0;               // 上次广播时间
static const unsigned long BROADCAST_INTERVAL = 500;        // 广播间隔: 500ms
static const unsigned long PEER_TIMEOUT = 3000;             // 设备超时时间: 3s

// 游戏状态
static volatile bool game_request_pending = false;          // 游戏请求待处理标志
static volatile uint8_t game_request_id = 0;                // 游戏请求ID
static volatile uint32_t game_request_seed = 0;             // 游戏随机种子
static TankData remote_tank_data;                           // 远程坦克数据
static volatile bool remote_game_ended = false;             // 远程游戏结束标志
static volatile uint8_t remote_game_end_reason = 0;         // 远程游戏结束原因

// ESP-NOW数据发送回调函数
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // 处理发送状态（如果需要）
}

// ESP-NOW数据接收回调函数
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    if (len != sizeof(NetMessage)) return;
    
    NetMessage *msg = (NetMessage *)incomingData;
    
    // 1. 发现消息
    if (msg->type == MSG_DISCOVERY) {
        if (current_state == NET_DISCOVERING) {
            // 检查是否已存在
            bool found = false;
            for (auto &p : discovered_peers) {
                if (memcmp(p.mac, msg->src_mac, 6) == 0) {
                    p.last_seen = millis();
                    found = true;
                    break;
                }
            }
            if (!found) {
                PeerInfo p;
                memcpy(p.mac, msg->src_mac, 6);
                p.name = String(msg->name);
                p.last_seen = millis();
                discovered_peers.push_back(p);
            }
        }
    }
    // 2. 配对请求
    else if (msg->type == MSG_PAIR_REQUEST) {
        if (current_state == NET_DISCOVERING || current_state == NET_IDLE) {
            // 自动接受（简化演示）
            // 在实际应用中，会显示提示
            
            // 发送接受消息
            if (!esp_now_is_peer_exist(msg->src_mac)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, msg->src_mac, 6);
                peerInfo.channel = 0;  
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_AP; // 使用AP接口
                esp_now_add_peer(&peerInfo);
            }
            
            NetMessage reply;
            reply.type = MSG_PAIR_ACCEPT;
            memcpy(reply.src_mac, my_mac, 6);
            strncpy(reply.name, my_name, 16);
            esp_now_send(msg->src_mac, (uint8_t *) &reply, sizeof(reply));
            
            memcpy(connected_peer_mac, msg->src_mac, 6);
            current_state = NET_CONNECTED;
        }
    }
    // 3. 配对接受
    else if (msg->type == MSG_PAIR_ACCEPT) {
        if (current_state == NET_PAIRING) {
             memcpy(connected_peer_mac, msg->src_mac, 6);
             current_state = NET_CONNECTED;
        }
    }
    // 4. 断开连接
    else if (msg->type == MSG_DISCONNECT) {
        if (current_state == NET_CONNECTED || current_state == NET_IN_GAME) {
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                current_state = NET_IDLE;
                esp_now_del_peer(connected_peer_mac);
                remote_game_ended = true; // 强制退出游戏
            }
        }
    }
    // 5. 数据消息（测试）
    else if (msg->type == MSG_DATA) {
        if (current_state == NET_CONNECTED) {
            // 验证是否来自已连接设备
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                Serial.printf("Received DATA from Peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    msg->src_mac[0], msg->src_mac[1], msg->src_mac[2], 
                    msg->src_mac[3], msg->src_mac[4], msg->src_mac[5]);
            }
        }
    }
    // 6. 游戏开始
    else if (msg->type == MSG_START_GAME) {
        if (current_state == NET_CONNECTED || current_state == NET_IN_GAME) {
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                game_request_id = msg->payload.start_req.game_id;
                game_request_seed = msg->payload.start_req.seed;
                game_request_pending = true;
                current_state = NET_IN_GAME;
            }
        }
    }
    // 7. 游戏数据
    else if (msg->type == MSG_GAME_DATA) {
        if (current_state == NET_IN_GAME) {
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                remote_tank_data = msg->payload.tank_data;
            }
        }
    }
    // 8. 游戏结束
    else if (msg->type == MSG_END_GAME) {
        if (current_state == NET_IN_GAME) {
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                remote_game_ended = true;
                remote_game_end_reason = msg->payload.end_req.reason;
                current_state = NET_CONNECTED;
            }
        }
    }
}

// 初始化网络管理器
void Network_Manager::init() {
    // 设置为AP_STA模式，允许同时运行Web服务器（AP）和ESP-NOW
    WiFi.mode(WIFI_AP_STA);
    
    WiFi.macAddress(my_mac);
    sprintf(my_name, "Player_%02X:%02X", my_mac[4], my_mac[5]);
    
    if (esp_now_init() != ESP_OK) {
        return;
    }
    
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);
}

// 禁用网络管理器
void Network_Manager::disable() {
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
}

// 启用网络管理器
void Network_Manager::enable() {
    init();
    // 重新启用AP（逻辑与web_server.cpp重复）
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char ssid[32];
    sprintf(ssid, "ESP32_Game_%02X:%02X", mac[4], mac[5]);
    WiFi.softAP(ssid, "12345678");
}

// 更新网络管理器状态
void Network_Manager::update() {
    // 清理超时设备
    if (current_state == NET_DISCOVERING) {
        for (auto it = discovered_peers.begin(); it != discovered_peers.end(); ) {
            if (millis() - it->last_seen > PEER_TIMEOUT) {
                it = discovered_peers.erase(it);
            } else {
                ++it;
            }
        }
        
        // 广播存在
        if (millis() - last_broadcast_time > BROADCAST_INTERVAL) {
            last_broadcast_time = millis();
            
            NetMessage msg;
            msg.type = MSG_DISCOVERY;
            memcpy(msg.src_mac, my_mac, 6);
            strncpy(msg.name, my_name, 16);
            
            uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            
            if (!esp_now_is_peer_exist(broadcastAddress)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, broadcastAddress, 6);
                peerInfo.channel = 0;  
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_AP; // 在AP接口上广播
                esp_now_add_peer(&peerInfo);
            }
            
            esp_now_send(broadcastAddress, (uint8_t *) &msg, sizeof(msg));
        }
    }
    
    // 连接逻辑（测试心跳）
    if (current_state == NET_CONNECTED) {
        static unsigned long last_data_time = 0;
        if (millis() - last_data_time > 1000) {
            last_data_time = millis();
            
            NetMessage msg;
            msg.type = MSG_DATA;
            memcpy(msg.src_mac, my_mac, 6);
            strncpy(msg.name, my_name, 16);
            
            // 发送到已连接设备
            esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
             Serial.println("Sent MSG_DATA (Heartbeat)");
        }
    }
}

// 开始游戏
void Network_Manager::startGame(uint8_t gameId, uint32_t seed) {
    if (current_state != NET_CONNECTED && current_state != NET_IN_GAME) return;
    
    NetMessage msg;
    msg.type = MSG_START_GAME;
    memcpy(msg.src_mac, my_mac, 6);
    strncpy(msg.name, my_name, 16);
    msg.payload.start_req.game_id = gameId;
    msg.payload.start_req.seed = seed;
    
    esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
    current_state = NET_IN_GAME;
}

// 发送游戏数据
void Network_Manager::sendGameData(const TankData& data) {
    if (current_state != NET_IN_GAME) return;
    
    NetMessage msg;
    msg.type = MSG_GAME_DATA;
    memcpy(msg.src_mac, my_mac, 6);
    strncpy(msg.name, my_name, 16);
    msg.payload.tank_data = data;
    
    esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
}

// 结束游戏
void Network_Manager::endGame(uint8_t reason) {
    if (current_state != NET_IN_GAME) return;
    
    NetMessage msg;
    msg.type = MSG_END_GAME;
    memcpy(msg.src_mac, my_mac, 6);
    strncpy(msg.name, my_name, 16);
    msg.payload.end_req.reason = reason;
    
    esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
    current_state = NET_CONNECTED;
}

// 检查是否有游戏请求
bool Network_Manager::hasGameRequest(uint8_t* gameIdOut, uint32_t* seedOut) {
    if (game_request_pending) {
        *gameIdOut = game_request_id;
        *seedOut = game_request_seed;
        return true;
    }
    return false;
}

// 清除游戏请求
void Network_Manager::clearGameRequest() {
    game_request_pending = false;
}

// 获取远程游戏数据
bool Network_Manager::getRemoteGameData(TankData* dataOut) {
    *dataOut = remote_tank_data;
    return true;
}

// 检查远程游戏是否结束
bool Network_Manager::isRemoteGameEnded(uint8_t* reasonOut) {
    if(remote_game_ended) {
        *reasonOut = remote_game_end_reason;
        return true;
    }
    return false;
}

// 清除远程游戏结束标志
void Network_Manager::clearRemoteGameEnded() {
    remote_game_ended = false;
}

// 开始设备发现
void Network_Manager::startDiscovery() {
    if (current_state == NET_CONNECTED) return;
    current_state = NET_DISCOVERING;
    discovered_peers.clear();
}

// 停止设备发现
void Network_Manager::stopDiscovery() {
    if (current_state == NET_DISCOVERING) {
        current_state = NET_IDLE;
    }
}

// 配对设备
void Network_Manager::pair(const uint8_t* target_mac) {
    if (current_state == NET_CONNECTED) return;
    
    memcpy(pending_peer_mac, target_mac, 6);
    
    if (!esp_now_is_peer_exist(pending_peer_mac)) {
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, pending_peer_mac, 6);
        peerInfo.channel = 0;  
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_AP; // 使用AP接口
        esp_now_add_peer(&peerInfo);
    }
    
    NetMessage msg;
    msg.type = MSG_PAIR_REQUEST;
    memcpy(msg.src_mac, my_mac, 6);
    strncpy(msg.name, my_name, 16);
    
    esp_now_send(pending_peer_mac, (uint8_t *) &msg, sizeof(msg));
    
    current_state = NET_PAIRING;
}

// 断开连接
void Network_Manager::disconnect() {
    if (current_state == NET_CONNECTED) {
        NetMessage msg;
        msg.type = MSG_DISCONNECT;
        memcpy(msg.src_mac, my_mac, 6);
        strncpy(msg.name, my_name, 16);
        
        esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
        
        esp_now_del_peer(connected_peer_mac);
        current_state = NET_IDLE;
    }
}

// 获取当前网络状态
NetState Network_Manager::getState() {
    return current_state;
}

// 获取发现的设备数量
int Network_Manager::getPeerCount() {
    return discovered_peers.size();
}

// 获取发现的设备列表
const PeerInfo* Network_Manager::getPeers() {
    return discovered_peers.data();
}