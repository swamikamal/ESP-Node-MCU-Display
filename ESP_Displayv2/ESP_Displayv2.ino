/*
  ESP_Display (RAW RGB565 Edition) - Single .ino

  Changes from original:
  - Landscape rotation (160x128): setRotation(1)
  - FRAME_WIDTH/HEIGHT swapped to 160x128
  - Animation: single seek per frame, sequential line reads (no per-line seek)
  - No yield() inside frame render loop (eliminates tearing)
  - Watchdog fed after each complete frame
  - Removed partial-frame state (currentY) — frames render atomically
  - Web UI updated to reflect 160x128 resolution

  RAW .rgb format:
    - Resolution: 160x128 (landscape)
    - Pixel format: RGB565 little-endian
    - Frames concatenated: frame_size = 40960 bytes (160*128*2)
    - File size = frame_size * N (N >= 1)
*/

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <EEPROM.h>

// ====================== Configuration ======================
#define AP_SSID "ESP_Display"
#define AP_PASS "12345678"

// Paths
#define IMAGE_PATH "/u.jpg"
#define RGB_PATH   "/u.rgb"
#define TEXT_PATH  "/text.txt"

// Display — Landscape 160x128
#define FRAME_WIDTH   160
#define FRAME_HEIGHT  128
#define BYTES_PER_PIX 2
#define FRAME_SIZE    (FRAME_WIDTH * FRAME_HEIGHT * BYTES_PER_PIX)  // 40960 bytes

// Upload limits
#define MAX_JPG_SIZE   15000     // ~15 KB
#define MAX_RGB_SIZE   800000    // ~800 KB

// Text limits
#define MAX_TEXT_LENGTH 200

// RAW animation playback
#define DEFAULT_FRAME_DELAY_MS 150

// =================== Globals & State =======================
TFT_eSPI tft = TFT_eSPI();
AsyncWebServer server(80);
DNSServer dnsServer;

// Display state: 0=placeholder, 1=transitioning, 2=raw rgb, 3=jpg, 4=text
uint8_t displayState = 0;
char fileType = 0; // 0=none, 'r'=raw rgb, 'j'=jpg, 't'=text

// Text display variables
String   displayText = "";
uint16_t textColor   = TFT_WHITE;
uint16_t bgColor     = TFT_BLACK;
uint8_t  textSize    = 1;

// EEPROM addresses
#define EEPROM_FILE_TYPE   0
#define EEPROM_TEXT_COLOR  1   // 2 bytes
#define EEPROM_BG_COLOR    3   // 2 bytes
#define EEPROM_TEXT_SIZE   5   // 1 byte
#define EEPROM_FRAME_DELAY 6   // 2 bytes

// RAW RGB playback state
File      rgbFile;
bool      rgbOpen      = false;
size_t    totalFrames  = 0;
size_t    currentFrame = 0;
uint32_t  nextFrameDue = 0;
uint16_t  frameDelayMs = DEFAULT_FRAME_DELAY_MS;

// Line buffer — one scanline at a time (~320 bytes for 160px landscape)
uint16_t  lineBuffer[FRAME_WIDTH];

// Watchdog
#include <Ticker.h>
Ticker wdt;
void IRAM_ATTR reset() { ESP.restart(); }
void feed() { ESP.wdtFeed(); wdt.detach(); wdt.attach(8, reset); }

// =================== Helpers & Utilities ===================
uint16_t htmlToRGB565(String hexColor) {
  if (hexColor.startsWith("#")) hexColor = hexColor.substring(1);
  if (hexColor.length() != 6) return TFT_WHITE;
  long rgb = strtol(hexColor.c_str(), NULL, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

String rgb565ToHex6(uint16_t c) {
  uint8_t r5 = (c >> 11) & 0x1F;
  uint8_t g6 = (c >> 5)  & 0x3F;
  uint8_t b5 =  c        & 0x1F;
  uint8_t r = (r5 * 527 + 23) >> 6;
  uint8_t g = (g6 * 259 + 33) >> 6;
  uint8_t b = (b5 * 527 + 23) >> 6;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", r, g, b);
  return String(buf);
}

void saveSettings() {
  EEPROM.write(EEPROM_FILE_TYPE, fileType);
  EEPROM.write(EEPROM_TEXT_COLOR,     textColor & 0xFF);
  EEPROM.write(EEPROM_TEXT_COLOR + 1, (textColor >> 8) & 0xFF);
  EEPROM.write(EEPROM_BG_COLOR,       bgColor & 0xFF);
  EEPROM.write(EEPROM_BG_COLOR + 1,   (bgColor >> 8) & 0xFF);
  EEPROM.write(EEPROM_TEXT_SIZE,      textSize);
  EEPROM.write(EEPROM_FRAME_DELAY,    frameDelayMs & 0xFF);
  EEPROM.write(EEPROM_FRAME_DELAY+1,  (frameDelayMs >> 8) & 0xFF);
  EEPROM.commit();
}

void loadSettings() {
  fileType = EEPROM.read(EEPROM_FILE_TYPE);
  if (fileType != 'r' && fileType != 'j' && fileType != 't') fileType = 0;

  textColor = EEPROM.read(EEPROM_TEXT_COLOR) | (EEPROM.read(EEPROM_TEXT_COLOR + 1) << 8);
  bgColor   = EEPROM.read(EEPROM_BG_COLOR)   | (EEPROM.read(EEPROM_BG_COLOR + 1)   << 8);
  textSize  = EEPROM.read(EEPROM_TEXT_SIZE);
  uint16_t fd = EEPROM.read(EEPROM_FRAME_DELAY) | (EEPROM.read(EEPROM_FRAME_DELAY + 1) << 8);

  if (textColor == 0) textColor = TFT_WHITE;
  if (textSize == 0 || textSize > 4) textSize = 1;
  if (fd != 0xFFFF && fd >= 20 && fd <= 2000) frameDelayMs = fd;
}

bool loadTextFromFile() {
  if (!LittleFS.exists(TEXT_PATH)) return false;
  File file = LittleFS.open(TEXT_PATH, "r");
  if (!file) return false;
  displayText = file.readString();
  file.close();
  displayText.trim();
  return true;
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y < tft.height()) tft.pushImage(x, y, w, h, bitmap);
  return true;
}

bool drawJPG(const char* fname) {
  uint16_t w, h;
  if (TJpgDec.getFsJpgSize(&w, &h, fname, LittleFS)) return false;
  int16_t x = (tft.width()  - w) / 2;
  int16_t y = (tft.height() - h) / 2;
  return !TJpgDec.drawFsJpg(x, y, fname, LittleFS);
}

void showText() {
  tft.fillScreen(bgColor);
  if (displayText.length() == 0) {
    tft.setTextColor(TFT_RED, bgColor);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString("No text to display", tft.width()/2, tft.height()/2);
    return;
  }
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(textSize);
  tft.setTextWrap(true, true);

  int16_t lineHeight = 8 * textSize;
  int16_t currentY   = 10;
  String text = displayText;

  while (text.length() > 0 && currentY < tft.height() - lineHeight) {
    int maxChars = (tft.width() - 10) / (6 * textSize);
    String line;
    if ((int)text.length() <= maxChars) {
      line = text; text = "";
    } else {
      int spacePos = text.lastIndexOf(' ', maxChars);
      if (spacePos > 0 && spacePos < maxChars) {
        line = text.substring(0, spacePos);
        text = text.substring(spacePos + 1);
      } else {
        line = text.substring(0, maxChars);
        text = text.substring(maxChars);
      }
    }
    tft.setCursor(5, currentY);
    tft.print(line);
    currentY += lineHeight;
  }
}

void showPlaceholder(const char* msg = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);

  int cx = tft.width()  / 2;
  int cy = tft.height() / 2;

  tft.drawString("ESP Display",           cx, cy - 30);
  tft.drawString("Upload: JPG/RAW/Text",  cx, cy - 15);
  tft.drawString(WiFi.softAPIP().toString(), cx, cy);
  tft.drawString("160x128 Landscape",     cx, cy + 15);

  if (msg && *msg) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString(msg, cx, cy + 30);
  }

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Free: " + String(ESP.getFreeHeap()), cx, cy + 45);
}

// ====================== RAW RGB Playback ===================
void cleanupRGB() {
  if (rgbOpen) {
    rgbFile.close();
    rgbOpen = false;
  }
  totalFrames  = 0;
  currentFrame = 0;
  nextFrameDue = 0;
}

bool initRGB() {
  cleanupRGB();

  if (!LittleFS.exists(RGB_PATH)) {
    Serial.println("RAW file not found");
    return false;
  }
  rgbFile = LittleFS.open(RGB_PATH, "r");
  if (!rgbFile) {
    Serial.println("RAW open failed");
    return false;
  }
  size_t sz = rgbFile.size();
  if (sz < FRAME_SIZE || (sz % FRAME_SIZE) != 0) {
    Serial.printf("Invalid RAW size: %u (frame size=%u)\n", (unsigned)sz, (unsigned)FRAME_SIZE);
    rgbFile.close();
    return false;
  }

  totalFrames  = sz / FRAME_SIZE;
  currentFrame = 0;
  nextFrameDue = 0;
  rgbOpen      = true;

  tft.fillScreen(TFT_BLACK);

  Serial.printf("RAW initialized: %u bytes, %u frames, heap=%u\n",
                (unsigned)sz, (unsigned)totalFrames, ESP.getFreeHeap());
  return true;
}

/*
  handleRGB — Atomic per-frame render.
  - Seeks ONCE to frame start, then reads all lines sequentially.
  - No yield() inside the render loop = no tearing.
  - Watchdog fed after each complete frame.
*/
void handleRGB() {
  if (!rgbOpen) {
    if (!initRGB()) {
      showPlaceholder("RAW error");
      displayState = 0;
      return;
    }
    displayState = 2;
    return;
  }

  // Honour frame delay
  if (millis() < nextFrameDue) return;

  // Seek once to the start of the current frame
  size_t frameBase = (size_t)currentFrame * FRAME_SIZE;
  if (!rgbFile.seek(frameBase, SeekSet)) {
    Serial.println("Seek failed in RAW");
    cleanupRGB();
    showPlaceholder("RAW seek error");
    displayState = 0;
    return;
  }

  // Read and push all scanlines sequentially — no mid-frame yields
  for (uint16_t y = 0; y < FRAME_HEIGHT; y++) {
    size_t need = FRAME_WIDTH * BYTES_PER_PIX;
    size_t got  = rgbFile.read((uint8_t*)lineBuffer, need);
    if (got != need) {
      Serial.printf("Short read at line %u: got=%u need=%u\n", y, (unsigned)got, (unsigned)need);
      cleanupRGB();
      showPlaceholder("RAW read error");
      displayState = 0;
      return;
    }
    tft.pushImage(0, y, FRAME_WIDTH, 1, (uint16_t*)lineBuffer);
  }

  // Advance to next frame and schedule delay
  currentFrame = (currentFrame + 1) % totalFrames;
  nextFrameDue = millis() + frameDelayMs;

  // Feed watchdog after each complete frame (safe — frame render is well under 8s)
  feed();
}

// ====================== Web UI =============================
String htmlPage() {
  String html = F(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP Display</title>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#f0f0f0}"
    ".container{max-width:520px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,.1)}"
    "h2{margin:0 0 16px;color:#333}"
    ".status{background:#e8f5e8;padding:10px;border-radius:6px;margin:10px 0}"
    ".form-group{margin:15px 0;text-align:left}"
    "label{display:block;margin-bottom:6px;font-weight:bold}"
    "input,textarea,select{width:100%;padding:8px;border:1px solid #ddd;border-radius:6px;box-sizing:border-box}"
    "textarea{height:80px;resize:vertical}"
    ".color-group{display:flex;gap:10px}"
    ".color-group>div{flex:1}"
    "input[type=color]{height:40px;cursor:pointer}"
    ".size-group{display:flex;gap:10px;align-items:center}"
    "input[type=range]{flex:1}"
    ".size-display{min-width:30px;font-weight:bold}"
    "button{background:#007bff;color:#fff;padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:16px}"
    "button:hover{background:#0056b3}"
    ".reset-btn{background:#dc3545;margin-left:10px}"
    ".reset-btn:hover{background:#c82333}"
    ".file-info{font-size:12px;color:#666;margin-top:5px}"
    "</style></head><body><div class='container'>"
    "<h2>ESP Display Controller</h2>"
    "<div class='status'><b>Current Display:</b> ");

  if      (fileType == 'r') html += "RAW Animation (.rgb)";
  else if (fileType == 'j') html += "JPG Image";
  else if (fileType == 't') html += "Custom Text";
  else                      html += "None";

  html += F("</div>"
            "<div class='status'><b>Display Size:</b> 160x128 pixels (Landscape)</div>");

  // Upload form
  html += F(
    "<div class='form-group'>"
    "<label>Upload JPG Image:</label>"
    "<input type='file' id='jpgInput' accept='.jpg,.jpeg' onchange='previewJPG(this)'>"
    "<div class='file-info'>Any JPG — browser will auto-resize to fit 160x128 and compress under 15 KB before uploading.</div>"
    "</div>"
    "<canvas id='cvs' style='display:none'></canvas>"
    "<div id='jpgPreview' style='margin:8px 0;display:none'>"
    "<img id='previewImg' style='max-width:160px;max-height:128px;border:1px solid #ccc;display:block;margin-bottom:4px'>"
    "<div id='previewInfo' style='font-size:12px;color:#555'></div>"
    "</div>"
    "<button onclick='uploadJPG()' id='jpgUploadBtn' style='display:none'>Upload Image</button>"
    "<hr>"
    "<div class='form-group'>"
    "<label>Upload RAW Animation (.rgb):</label>"
    "<input type='file' id='rgbInput' accept='.rgb'>"
    "<div class='file-info'>RAW .rgb: <b>160x128</b>, RGB565 LE, must be multiple of 40960 bytes. Max ");
  html += String(MAX_RGB_SIZE / 1024);
  html += F(" KB.</div></div>"
    "<div class='form-group'>"
    "<label>Frame Delay (ms) for RAW animation:</label>"
    "<input type='number' id='frameDelayInput' min='20' max='2000' value='");
  html += String(frameDelayMs);
  html += F("'></div>"
    "<button onclick='uploadRGB()'>Upload RAW</button>"
    "<hr>"

    "<script>"
    "var processedBlob=null;"

    // Preview + auto-process JPG on file select
    "function previewJPG(input){"
    "  if(!input.files||!input.files[0])return;"
    "  var file=input.files[0];"
    "  var reader=new FileReader();"
    "  reader.onload=function(e){"
    "    var img=new Image();"
    "    img.onload=function(){"
    "      processedBlob=null;"
    "      document.getElementById('jpgUploadBtn').style.display='none';"
    "      document.getElementById('jpgPreview').style.display='block';"
    "      resizeAndCompress(img);"
    "    };"
    "    img.src=e.target.result;"
    "  };"
    "  reader.readAsDataURL(file);"
    "}"

    // Resize to fit 160x128 (letterbox), then compress iteratively under 15KB
    "function resizeAndCompress(img){"
    "  var TW=160,TH=128,MAX=14800;"
    "  var cvs=document.getElementById('cvs');"
    "  cvs.width=TW; cvs.height=TH;"
    "  var ctx=cvs.getContext('2d');"
    // Fill black background (letterbox)
    "  ctx.fillStyle='#000000';"
    "  ctx.fillRect(0,0,TW,TH);"
    // Scale keeping aspect ratio
    "  var scale=Math.min(TW/img.width,TH/img.height);"
    "  var sw=Math.round(img.width*scale);"
    "  var sh=Math.round(img.height*scale);"
    "  var ox=Math.round((TW-sw)/2);"
    "  var oy=Math.round((TH-sh)/2);"
    "  ctx.drawImage(img,ox,oy,sw,sh);"
    // Iteratively reduce quality until under MAX bytes
    "  var q=0.92;"
    "  function tryCompress(){"
    "    cvs.toBlob(function(blob){"
    "      if(!blob){alert('Canvas error');return;}"
    "      if(blob.size<=MAX||q<=0.20){"
    "        processedBlob=blob;"
    "        var pimg=document.getElementById('previewImg');"
    "        pimg.src=URL.createObjectURL(blob);"
    "        document.getElementById('previewInfo').textContent="
    "          'Size: '+Math.round(blob.size/1024*10)/10+' KB | '+TW+'x'+TH+' | Quality: '+Math.round(q*100)+'%';"
    "        document.getElementById('jpgUploadBtn').style.display='inline-block';"
    "      } else {"
    "        q=Math.max(q-0.08,0.20);"
    "        tryCompress();"
    "      }"
    "    },'image/jpeg',q);"
    "  }"
    "  tryCompress();"
    "}"

    // Upload processed JPG blob
    "function uploadJPG(){"
    "  if(!processedBlob){alert('No image processed yet.');return;}"
    "  var fd=new FormData();"
    "  fd.append('file',processedBlob,'u.jpg');"
    "  var btn=document.getElementById('jpgUploadBtn');"
    "  btn.disabled=true; btn.textContent='Uploading...';"
    "  fetch('/upload',{method:'POST',body:fd})"
    "  .then(function(r){return r.text();})"
    "  .then(function(){btn.disabled=false;btn.textContent='Upload Image';alert('Image uploaded!');location='/';})"
    "  .catch(function(e){btn.disabled=false;btn.textContent='Upload Image';alert('Upload failed: '+e);});"
    "}"

    // Upload RAW .rgb file directly (no processing needed)
    "function uploadRGB(){"
    "  var input=document.getElementById('rgbInput');"
    "  if(!input.files||!input.files[0]){alert('Select a .rgb file first.');return;}"
    "  var fd=new FormData();"
    "  fd.append('file',input.files[0]);"
    "  var delay=document.getElementById('frameDelayInput').value;"
    "  fd.append('frameDelay',delay);"
    "  fetch('/upload',{method:'POST',body:fd})"
    "  .then(function(r){return r.text();})"
    "  .then(function(){alert('RAW uploaded!');location='/';})"
    "  .catch(function(e){alert('Upload failed: '+e);});"
    "}"
    "</script>");

  // Text form
  html += F(
    "<form method='POST' action='/text'>"
    "<div class='form-group'>"
    "<label>Display Text:</label>"
    "<textarea name='text' maxlength='200'>");
  html += displayText;
  html += F("</textarea></div>"
    "<div class='color-group'>"
    "<div><label>Text Color:</label>"
    "<input type='color' name='textColor' value='#");
  html += rgb565ToHex6(textColor);
  html += F("'></div>"
    "<div><label>Background Color:</label>"
    "<input type='color' name='bgColor' value='#");
  html += rgb565ToHex6(bgColor);
  html += F("'></div></div>"
    "<div class='form-group'>"
    "<label>Text Size:</label>"
    "<div class='size-group'>"
    "<input type='range' name='textSize' min='1' max='4' value='");
  html += String(textSize);
  html += F("' oninput=\"document.getElementById('sizeValue').textContent=this.value\">"
    "<div class='size-display' id='sizeValue'>");
  html += String(textSize);
  html += F("</div></div></div>"
    "<button type='submit'>Display Text</button>"
    "</form><hr>"
    "<button onclick=\"location.href='/reset'\" class='reset-btn'>Reset Display</button>"
    "</div></body></html>");

  return html;
}

// ====================== Setup & Loop =======================
void setup() {
  Serial.begin(115200);
  Serial.println("ESP Display Controller (Landscape) Starting...");

  // Display init — Landscape rotation
  tft.begin();
  tft.setRotation(1);   // 1 = Landscape: 160 wide x 128 tall on ST7735
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.drawString("Initializing...", tft.width()/2, tft.height()/2);
  Serial.printf("Display: %dx%d\n", tft.width(), tft.height());

  // Filesystem
  if (!LittleFS.begin()) {
    Serial.println("Formatting LittleFS...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("LittleFS failed!");
      while (1) delay(1000);
    }
  }

  // EEPROM
  EEPROM.begin(16);
  loadSettings();
  if (fileType == 't') loadTextFromFile();
  Serial.printf("Loaded: type=%c frameDelay=%u\n", fileType ? fileType : 'N', frameDelayMs);

  // JPG decoder
  TJpgDec.setCallback(tft_output);
  TJpgDec.setSwapBytes(true);

  // WiFi AP + DNS captive portal
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());

  // ---- Web server routes ----

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/html", htmlPage());
  });

  server.on("/text", HTTP_POST, [](AsyncWebServerRequest* r){
    String text     = "";
    String txtColor = "#FFFFFF";
    String bgCol    = "#000000";
    int    size     = 1;

    if (r->hasParam("text",      true)) text     = r->getParam("text",      true)->value();
    if (r->hasParam("textColor", true)) txtColor = r->getParam("textColor", true)->value();
    if (r->hasParam("bgColor",   true)) bgCol    = r->getParam("bgColor",   true)->value();
    if (r->hasParam("textSize",  true)) size     = r->getParam("textSize",  true)->value().toInt();

    text.trim();
    if (size < 1) size = 1;
    if (size > 4) size = 4;

    File textFile = LittleFS.open(TEXT_PATH, "w");
    if (textFile) { textFile.print(text); textFile.close(); }

    displayText = text;
    textColor   = htmlToRGB565(txtColor);
    bgColor     = htmlToRGB565(bgCol);
    textSize    = size;
    fileType    = 't';
    saveSettings();
    displayState = 1;

    r->send(200, "text/html", F("<script>alert('Text updated!');location='/';</script>"));
  });

  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){
      r->send(200, "text/html", F("<script>alert('Upload complete!');location='/';</script>"));
    },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      static File     uploadFile;
      static char     newType          = 0;
      static uint32_t totalSize        = 0;
      static uint16_t postedFrameDelay = 0;

      if (index == 0) {
        totalSize = 0;
        postedFrameDelay = 0;
        if (r->hasParam("frameDelay", true)) {
          int fd = r->getParam("frameDelay", true)->value().toInt();
          if (fd >= 20 && fd <= 2000) postedFrameDelay = (uint16_t)fd;
        }

        filename.toLowerCase();
        if (filename.endsWith(".rgb")) {
          newType = 'r';
          LittleFS.remove(RGB_PATH);
          uploadFile = LittleFS.open(RGB_PATH, "w");
        } else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) {
          newType = 'j';
          LittleFS.remove(IMAGE_PATH);
          uploadFile = LittleFS.open(IMAGE_PATH, "w");
        } else {
          Serial.println("Unsupported file type");
          newType = 0;
          return;
        }

        if (!uploadFile) {
          Serial.println("Upload file creation failed");
          return;
        }
        Serial.printf("Starting upload: %s\n", filename.c_str());
      }

      if (uploadFile && len) {
        totalSize += len;
        if ((newType == 'r' && totalSize > MAX_RGB_SIZE) ||
            (newType == 'j' && totalSize > MAX_JPG_SIZE)) {
          uploadFile.close();
          Serial.println("File too large — upload cancelled");
          return;
        }
        if (uploadFile.write(data, len) != len) {
          uploadFile.close();
          Serial.println("Write error during upload");
          return;
        }
      }

      if (final && uploadFile) {
        uploadFile.close();
        Serial.printf("Upload done: type=%c size=%u\n", newType, (unsigned)totalSize);

        // Validate RAW file
        if (newType == 'r') {
          File f  = LittleFS.open(RGB_PATH, "r");
          size_t sz = f ? f.size() : 0;
          if (!f || sz < FRAME_SIZE || (sz % FRAME_SIZE) != 0) {
            if (f) f.close();
            Serial.printf("Invalid RAW file size: %u (must be multiple of %u)\n",
                          (unsigned)sz, (unsigned)FRAME_SIZE);
            LittleFS.remove(RGB_PATH);
            return;
          }
          f.close();
          if (postedFrameDelay) frameDelayMs = postedFrameDelay;
        }

        fileType     = newType;
        saveSettings();
        displayState = 1;
        cleanupRGB();
      }
    }
  );

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest* r){
    fileType     = 0;
    displayText  = "";
    textColor    = TFT_WHITE;
    bgColor      = TFT_BLACK;
    textSize     = 1;
    frameDelayMs = DEFAULT_FRAME_DELAY_MS;
    saveSettings();
    cleanupRGB();
    displayState = 1;
    r->send(200, "text/html", F("<script>alert('Display reset!');location='/';</script>"));
  });

  server.onNotFound([](AsyncWebServerRequest* r){ r->redirect("/"); });
  server.begin();

  // Watchdog
  wdt.attach(8, reset);
  Serial.println("Watchdog started");

  // Initial display
  if (fileType) {
    displayState = 1;
  } else {
    showPlaceholder();
    displayState = 0;
  }
  Serial.println("Setup complete!");
}

void loop() {
  feed();
  dnsServer.processNextRequest();

  switch (displayState) {
    case 1: // Transitioning — load saved content
      if (fileType == 'r') {
        handleRGB();
      } else if (fileType == 'j') {
        tft.fillScreen(TFT_BLACK);
        if (drawJPG(IMAGE_PATH)) {
          displayState = 3;
          Serial.println("JPG displayed");
        } else {
          Serial.println("JPG display failed");
          showPlaceholder("JPG error");
          displayState = 0;
        }
      } else if (fileType == 't') {
        if (loadTextFromFile()) {
          showText();
          displayState = 4;
          Serial.println("Text displayed");
        } else {
          Serial.println("Text display failed");
          showPlaceholder("Text error");
          displayState = 0;
        }
      } else {
        showPlaceholder();
        displayState = 0;
      }
      break;

    case 2: // Showing RAW animation
      handleRGB();
      break;

    case 0: // Placeholder
    case 3: // Static JPG
    case 4: // Static Text
      break;
  }

  delay(3);
}
