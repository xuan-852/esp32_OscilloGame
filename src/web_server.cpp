#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "freertos.h"

// Web服务器对象，端口80
WebServer server(80);
// 当前UI状态字符串
String current_ui_status = "{}";

// HTML页面内容
const char* html = u8R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { 
        font-family: Arial; 
        text-align: center; 
        margin:0px auto; 
        padding-top: 30px; 
        background-color: #222; 
        color: #fff; 
        position: relative;
        min-height: 100vh;
    }
    body::before {
        content: "";
        position: fixed;
        top: 0; left: 0; width: 100%; height: 100%;
        background-image: url('/background.jpg');
        background-size: cover;
        background-position: center;
        opacity: 0.5;
        z-index: -1;
    }
    .button { display: inline-block; padding: 20px 30px; font-size: 24px; cursor: pointer; text-align: center; text-decoration: none; outline: none; color: #fff; background-color: #4CAF50; border: none; border-radius: 15px; box-shadow: 0 9px #999; margin: 10px; -webkit-user-select: none; user-select: none; }
    .button:active { background-color: #3e8e41; box-shadow: 0 5px #666; transform: translateY(4px); }
    .nav-btn { background-color: #008CBA; }
    .enter-btn { background-color: #f44336; }
    .row { display: flex; justify-content: center; align-items: center; }
    #ui-status { margin-top: 20px; padding: 10px; border: 1px solid #555; background: rgba(51, 51, 51, 0.8); text-align: left; font-family: monospace; white-space: pre-wrap; min-height: 100px; }
  </style>
  <script>
    var t = 0;
    function c(path, e) {
      if (e.type === 'touchstart') {
        t = Date.now();
        e.preventDefault();
      } else if (Date.now() - t < 500) {
        return;
      }
      fetch(path);
    }
    
    setInterval(function() {
      fetch('/status').then(response => response.text()).then(data => {
        document.getElementById('ui-status').innerText = data;
      });
    }, 500);
  </script>
</head>
<body>
  <h1>ESP32游戏控制台</h1>
  
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/up', event)" ontouchstart="c('/up', event)">UP / PREV</button>
  </div>
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/left', event)" ontouchstart="c('/left', event)">LEFT / VOL-</button>
    <button class="button enter-btn" onmousedown="c('/enter', event)" ontouchstart="c('/enter', event)">ENTER</button>
    <button class="button nav-btn" onmousedown="c('/right', event)" ontouchstart="c('/right', event)">RIGHT / VOL+</button>
  </div>
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/down', event)" ontouchstart="c('/down', event)">DOWN / NEXT</button>
  </div>
  <p>当前UI状态: </p>
  <div id="ui-status">Loading UI Status...</div>

  <p>连接至WiFi: ESP32_Game_XX:XX/密码: 12345678</p>
  <p>南京理工大学 eeCommunity</p>
  <p><i>由 llgl0207 制作</i></p>
</body>
</html>
)rawliteral";

// 处理根路径请求
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", html);
}

// 处理状态查询请求
void handleStatus() {
  server.send(200, "text/plain; charset=utf-8", current_ui_status);
}

// 更新Web UI状态
void updateWebUIStatus(String status) {
    current_ui_status = status;
}

// 处理向上按钮
void handleUp() {
  web_enc_delta = -1; // 菜单向上/上一个
  web_game_dir = 0;   // 贪吃蛇向上
  server.send(200, "text/plain", "OK");
}

// 处理向下按钮
void handleDown() {
  web_enc_delta = 1;  // 菜单向下/下一个
  web_game_dir = 1;   // 贪吃蛇向下
  server.send(200, "text/plain", "OK");
}

// 处理向左按钮
void handleLeft() {
  web_enc_delta = -5; // 音量减小（更快）
  web_game_dir = 2;   // 贪吃蛇向左
  server.send(200, "text/plain", "OK");
}

// 处理向右按钮
void handleRight() {
  web_enc_delta = 5;  // 音量增加（更快）
  web_game_dir = 3;   // 贪吃蛇向右
  server.send(200, "text/plain", "OK");
}

// 处理确认按钮
void handleEnter() {
  web_btn_pressed = true;
  server.send(200, "text/plain", "OK");
}

// Web服务器任务函数
void webServerTask(void* pvParameters) {
  // 获取MAC地址并生成SSID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  sprintf(ssid, "ESP32_Game_%02X:%02X", mac[4], mac[5]);
  
  // 启动AP模式
  WiFi.softAP(ssid, "12345678");
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // 初始化LittleFS文件系统
  if(!LittleFS.begin(true)){
      Serial.println("An Error has occurred while mounting LittleFS");
  } else {
      Serial.println("LittleFS mounted successfully");
      // 列出文件用于调试
      File root = LittleFS.open("/");
      File file = root.openNextFile();
      while(file){
          Serial.print("FILE: ");
          Serial.println(file.name());
          file = root.openNextFile();
      }
  }

  // 注册URL处理函数
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/up", handleUp);
  server.on("/down", handleDown);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/enter", handleEnter);
  
  // 提供背景图片静态文件
  server.serveStatic("/background.jpg", LittleFS, "/background.jpg");

  // 启动服务器
  server.begin();
  Serial.println("HTTP server started");

  // 主循环：处理客户端请求
  while (1) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// 初始化Web服务器
void initWebServer() {
  xTaskCreatePinnedToCore(
    webServerTask,
    "WebServerTask",
    4096,
    NULL,
    1,
    NULL,
    0 // 核心0
  );
}