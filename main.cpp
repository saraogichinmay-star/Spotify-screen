#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <base64.h>

// ===================== SETTINGS =====================
const char* WIFI_SSID      = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";

const char* CLIENT_ID      = "YOUR_SPOTIFY_CLIENT_ID";
const char* CLIENT_SECRET  = "YOUR_SPOTIFY_CLIENT_SECRET";
const char* REFRESH_TOKEN  = "YOUR_SPOTIFY_REFRESH_TOKEN";

const int PIN_PLAY_PAUSE = 26;
const int PIN_NEXT       = 25;

// Replace with the numbers from TFT_eSPI's Touch_calibrate example:
uint16_t calData[5] = { 275, 3620, 264, 3532, 1 };

const unsigned long POLL_INTERVAL_MS = 4000;
// ====================================================

TFT_eSPI tft = TFT_eSPI();

const int BTN_Y = 170, BTN_H = 50;
const int PLAY_X = 20,  PLAY_W = 130;
const int NEXT_X = 170, NEXT_W = 130;
const int BAR_X = 20, BAR_Y = 125, BAR_W = 280, BAR_H = 10;

String accessToken = "";
unsigned long tokenExpiresAt = 0;

String title = "Nothing playing";
String artist = "";
long durationMs = 0;
long progressMs = 0;
bool isPlaying = false;
bool hasTrack = false;
unsigned long progressStampMs = 0;

unsigned long nextPollAt = 0;
unsigned long lastBarUpdate = 0;
unsigned long lastTouchAt = 0;
unsigned long lastBtnAt = 0;

// ---------- Drawing ----------
String shorten(String s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 3) + "...";
}

void drawButton(int x, int w, const char* label, uint16_t fill) {
  tft.fillRoundRect(x, BTN_Y, w, BTN_H, 8, fill);
  tft.drawRoundRect(x, BTN_Y, w, BTN_H, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fill);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, BTN_Y + BTN_H / 2, 4);
}

void drawButtons() {
  drawButton(PLAY_X, PLAY_W, isPlaying ? "Pause" : "Play", TFT_DARKGREEN);
  drawButton(NEXT_X, NEXT_W, "Next", TFT_NAVY);
}

void drawStatus(const char* msg) {
  tft.fillRect(0, 0, 320, 20, TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(msg, 4, 2, 2);
}

void drawTrackInfo() {
  tft.fillRect(0, 30, 320, 85, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(shorten(title, 24), 20, 40, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(shorten(artist, 32), 20, 80, 2);
}

String fmtTime(long ms) {
  long s = ms / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
  return String(buf);
}

void drawProgress() {
  long shown = progressMs;
  if (isPlaying) shown += (long)(millis() - progressStampMs);
  if (durationMs > 0 && shown > durationMs) shown = durationMs;
  if (shown < 0) shown = 0;

  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, TFT_WHITE);
  tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, TFT_BLACK);
  if (durationMs > 0) {
    int fillW = (int)((long long)(BAR_W - 2) * shown / durationMs);
    tft.fillRect(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2, TFT_GREEN);
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(BAR_X, BAR_Y + 14, BAR_W, 18, TFT_BLACK);
  tft.drawString(fmtTime(shown), BAR_X, BAR_Y + 14, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(fmtTime(durationMs), BAR_X + BAR_W, BAR_Y + 14, 2);
}

void drawAll() {
  tft.fillScreen(TFT_BLACK);
  drawTrackInfo();
  drawProgress();
  drawButtons();
}

// ---------- WiFi ----------
void connectWiFi() {
  drawStatus("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) drawStatus("WiFi connected");
  else drawStatus("WiFi failed - retrying");
}

// ---------- Spotify ----------
bool refreshAccessToken() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;

  String auth = base64::encode(String(CLIENT_ID) + ":" + String(CLIENT_SECRET));
  http.addHeader("Authorization", "Basic " + auth);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST("grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN));
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err && doc["access_token"].is<const char*>()) {
      accessToken = doc["access_token"].as<String>();
      long expiresIn = doc["expires_in"] | 3600;
      tokenExpiresAt = millis() + (unsigned long)(expiresIn - 60) * 1000UL;
      ok = true;
    }
  } else {
    Serial.printf("Token refresh failed, HTTP %d\n", code);
  }
  http.end();
  return ok;
}

bool ensureToken() {
  if (accessToken.length() == 0 || (long)(millis() - tokenExpiresAt) >= 0) {
    return refreshAccessToken();
  }
  return true;
}

int spotifyRequest(const char* method, const char* url, String* body = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (!ensureToken()) return -1;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", "Bearer " + accessToken);

  int code;
  if (strcmp(method, "GET") == 0) code = http.GET();
  else code = http.sendRequest(method, (uint8_t*)nullptr, 0);

  if (code == 200 && body) *body = http.getString();
  http.end();

  if (code == 401) accessToken = "";
  return code;
}

void pollNowPlaying() {
  String body;
  int code = spotifyRequest("GET", "https://api.spotify.com/v1/me/player/currently-playing", &body);

  if (code == 200) {
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["artists"][0]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (!err && doc["item"].is<JsonObject>()) {
      String newTitle  = doc["item"]["name"].as<String>();
      String newArtist = doc["item"]["artists"][0]["name"].as<String>();
      bool changedTrack = (newTitle != title) || (newArtist != artist) || !hasTrack;

      title = newTitle;
      artist = newArtist;
      durationMs = doc["item"]["duration_ms"] | 0;
      progressMs = doc["progress_ms"] | 0;
      progressStampMs = millis();
      bool wasPlaying = isPlaying;
      isPlaying = doc["is_playing"] | false;
      hasTrack = true;

      if (changedTrack) { drawTrackInfo(); drawProgress(); }
      if (wasPlaying != isPlaying) drawButtons();
      drawStatus("");
    }
  } else if (code == 204) {
    if (hasTrack || title != "Nothing playing") {
      title = "Nothing playing";
      artist = "Start Spotify on a device";
      durationMs = 0; progressMs = 0; isPlaying = false; hasTrack = false;
      drawTrackInfo(); drawProgress(); drawButtons();
    }
    drawStatus("");
  } else if (code == 401) {
    drawStatus("Token expired - refreshing");
  } else if (code == 429) {
    drawStatus("Rate limited - slowing down");
    nextPollAt = millis() + 15000;
    return;
  } else {
    char msg[40];
    snprintf(msg, sizeof(msg), "Spotify error %d", code);
    drawStatus(msg);
  }
  nextPollAt = millis() + POLL_INTERVAL_MS;
}

// ---------- Actions ----------
void actionPlayPause() {
  int code;
  if (isPlaying) {
    progressMs += (long)(millis() - progressStampMs);
    progressStampMs = millis();
    code = spotifyRequest("PUT", "https://api.spotify.com/v1/me/player/pause");
    if (code == 204 || code == 200) isPlaying = false;
  } else {
    code = spotifyRequest("PUT", "https://api.spotify.com/v1/me/player/play");
    if (code == 204 || code == 200) { isPlaying = true; progressStampMs = millis(); }
  }
  if (code != 204 && code != 200) {
    char msg[40];
    snprintf(msg, sizeof(msg), "Play/pause failed (%d)", code);
    drawStatus(msg);
  }
  drawButtons();
  nextPollAt = millis() + 1000;
}

void actionNext() {
  int code = spotifyRequest("POST", "https://api.spotify.com/v1/me/player/next");
  if (code != 204 && code != 200) {
    char msg[40];
    snprintf(msg, sizeof(msg), "Next failed (%d)", code);
    drawStatus(msg);
  }
  nextPollAt = millis() + 1200;
}

// ---------- Input ----------
bool inRect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}

void handleTouch() {
  uint16_t x, y;
  if (!tft.getTouch(&x, &y)) return;
  if (millis() - lastTouchAt < 400) return;
  lastTouchAt = millis();

  if (inRect(x, y, PLAY_X, BTN_Y, PLAY_W, BTN_H)) actionPlayPause();
  else if (inRect(x, y, NEXT_X, BTN_Y, NEXT_W, BTN_H)) actionNext();
}

void handleButtons() {
  if (millis() - lastBtnAt < 300) return;
  if (digitalRead(PIN_PLAY_PAUSE) == LOW) { lastBtnAt = millis(); actionPlayPause(); }
  else if (digitalRead(PIN_NEXT) == LOW)  { lastBtnAt = millis(); actionNext(); }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_PLAY_PAUSE, INPUT_PULLUP);
  pinMode(PIN_NEXT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.setTouch(calData);
  drawAll();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    drawStatus("Getting Spotify token...");
    if (!refreshAccessToken()) drawStatus("Token failed - check keys");
  }
  nextPollAt = millis();
}

void loop() {
  handleTouch();
  handleButtons();

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      connectWiFi();
    }
  }

  if ((long)(millis() - nextPollAt) >= 0) {
    pollNowPlaying();
  }

  if (millis() - lastBarUpdate > 500) {
    lastBarUpdate = millis();
    if (hasTrack) drawProgress();
  }
}
