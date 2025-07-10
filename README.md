# NetVoid – Offline ESP32 Chatroom Works On Flipper Zero

NetVoid is a fully offline communication system built for ESP32 and Flipper Zero. It turns your ESP32 into a Wi-Fi access point that hosts a local chatroom or peer-to-peer chat—no internet required. Perfect for secure, private communication in any environment.

---

## 🔧 Requirements

- ESP32 board  
- Flipper Zero  
- Evil Portal app by [bigbrodude6119](https://github.com/bigbrodude6119/flipper-zero-evil-portal)  
- NetVoid firmware (`NetVoid.iso` or `NetVoid.bin`)  
- HTML chat page (`Websocket.html` or `P2P.html`)

---

## 💬 Chat Modes

### 1. **Chatroom Mode (Recommended)**

- **Supports many users**
- **No pairing or codes required**
- Devices connect to the Wi-Fi and can immediately chat with others
- Ideal for group communication

### 2. **P2P Mode**

- **One-on-one encrypted chats**
- Requires exchanging pairing codes
- Less ideal if users are fully offline or unable to share codes

---

## 🚀 Setup Instructions

1. **Flash the ESP32**

   Flash your ESP32 with either `NetVoid.iso` or `NetVoid.bin`. This prepares the board to host the chat system.
> **Note:** `NetVoid.bin` and `NetVoid.iso` are the same firmware — choose either format based on your preferred flashing method. It's purely for your convenience.
2. **Prepare the Flipper Zero**

   - Make sure you have the Evil Portal app installed already,
if you don't you can get it from here: [EvilPortal by bigbrodude6119](https://github.com/bigbrodude6119/flipper-zero-evil-portal).  
   - Copy the provided `Websocket.html` or `P2P.html` files to your Flipper Zero SD card under the portal directory (`SD Card/apps_data/evil_portal/html/`).

   ![Example Evil Portal Setup](example-image1.png)

3. **Launch the Portal**

   - Use the Evil Portal app on the Flipper Zero as normal
   - Select the html file of your choice (`Websocket.html` or `P2P.html`)

   ![Launching Evil Portal](example-image2.png)

4. **Join the Chat**

   - Any device connecting to the `NetVoid` AP will be redirected to the chat page if you are not redirected, simply go to the ip address your flipper says, e.g. `AP ip: 999.888.777.666`
   - Users can begin chatting immediately
   - The Flipper Zero displays all chat messages in real-time

   ![Chat in Action](example-image3.png)

---

## 🌐 How It Works

- ESP32 hosts a captive portal using WebSocket for real-time messaging.
- Devices connect via Wi-Fi and are served the `chat.html` from the Flipper Zero.
- All communication remains local and offline.
- The Flipper Zero displays chat history between all connected users.
