// Horizontal side-scrolling shmup for Feather RP2040 + 128x64 OLED FeatherWing (SH1107)
//
// Controls:
//   A     = move up
//   C     = move down
//   B     = fire (hold for auto-fire)
//   BOOT  = bomb (clears nearby threats; limited stock)
//   A/B/C = start / restart from title or game over
//
// World is lightly generative: sparse parallax stars + timed spawn tables
// that escalate with distance. Open playfield (no corridor walls).
//
// Libraries: Adafruit SH110x, Adafruit GFX, Adafruit NeoPixel, Adafruit BusIO
// Board: Adafruit Feather RP2040 (Earle Philhower core)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#define BUTTON_A 9
#define BUTTON_B 8
#define BUTTON_C 7

#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 16
#endif

static const int SCREEN_W = 128;
static const int SCREEN_H = 64;
static const int HUD_H = 8;
static const int PLAY_TOP = HUD_H;
static const int PLAY_H = SCREEN_H - HUD_H;

Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

enum GameState : uint8_t { TITLE, PLAYING, DEAD };
GameState state = TITLE;

enum EnemyType : uint8_t { E_DRONE = 0, E_SINE = 1, E_DIVER = 2, E_TANK = 3 };
enum PowerType : uint8_t { P_SPREAD = 0, P_RAPID = 1, P_SHIELD = 2, P_BOMB = 3 };

struct Bullet {
  float x, y;
  float vx, vy;
  bool active;
  bool friendly;
};

struct Enemy {
  float x, y;
  float vx, vy;
  float phase;
  uint8_t type;
  uint8_t hp;
  bool active;
  unsigned long nextShot;
};

struct Powerup {
  float x, y;
  uint8_t type;
  bool active;
};

struct Star {
  int16_t x;
  uint8_t y;
  uint8_t layer;  // 1..3 scroll speed multiplier
};

static const uint8_t MAX_PBUL = 14;
static const uint8_t MAX_EBUL = 10;
static const uint8_t MAX_ENEMY = 10;
static const uint8_t MAX_POWER = 4;
static const uint8_t MAX_STAR = 12;  // sparse — enemies need to read clearly

Bullet pBullets[MAX_PBUL];
Bullet eBullets[MAX_EBUL];
Enemy enemies[MAX_ENEMY];
Powerup powers[MAX_POWER];
Star stars[MAX_STAR];

float playerY = 36;
const float playerX = 18;
uint8_t lives = 5;
uint8_t bombs = 1;
uint16_t score = 0;
uint16_t highScore = 0;
uint32_t distance = 0;  // "meters" scrolled — drives progression

bool spreadShot = false;
bool shield = false;
unsigned long rapidUntil = 0;
unsigned long invulnUntil = 0;
unsigned long lastFire = 0;
unsigned long nextSpawn = 0;
unsigned long lastFrame = 0;
unsigned long deadAt = 0;
unsigned long lastPixel = 0;
uint16_t hue = 0;

float scroll = 0;  // continuous world X for hashing walls/stars

bool aDown = false, bDown = false, cDown = false, bootDown = true;
unsigned long inputLockUntil = 0;

uint32_t urand(uint32_t n) {
  if (n == 0) return 0;
  return rp2040.hwrand32() % n;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float difficulty() {
  return clampf(distance / 2500.0f, 0.0f, 1.0f);
}

// Bullet pressure: soft while you grab early powerups, then climbs harder.
float fireIntensity() {
  float d = distance / 1800.0f;
  if (d < 0.3f) return d * 0.55f;  // gentle opening
  return clampf(0.165f + (d - 0.3f) * 1.25f, 0.0f, 1.0f);
}

unsigned long fireCooldown() {
  if (millis() < rapidUntil) return 90;
  return 160;
}

void setNeo(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void clearEntities() {
  for (uint8_t i = 0; i < MAX_PBUL; i++) pBullets[i].active = false;
  for (uint8_t i = 0; i < MAX_EBUL; i++) eBullets[i].active = false;
  for (uint8_t i = 0; i < MAX_ENEMY; i++) enemies[i].active = false;
  for (uint8_t i = 0; i < MAX_POWER; i++) powers[i].active = false;
}

void initStars() {
  for (uint8_t i = 0; i < MAX_STAR; i++) {
    stars[i].x = (int16_t)(rp2040.hwrand32() % SCREEN_W);
    stars[i].y = (uint8_t)(PLAY_TOP + rp2040.hwrand32() % PLAY_H);
    stars[i].layer = (uint8_t)(1 + rp2040.hwrand32() % 3);
  }
}

bool spawnBullet(Bullet *arr, uint8_t n, float x, float y, float vx, float vy,
                 bool friendly) {
  for (uint8_t i = 0; i < n; i++) {
    if (arr[i].active) continue;
    arr[i] = {x, y, vx, vy, true, friendly};
    return true;
  }
  return false;
}

bool spawnEnemy(uint8_t type, float y) {
  for (uint8_t i = 0; i < MAX_ENEMY; i++) {
    if (enemies[i].active) continue;
    Enemy &e = enemies[i];
    e.active = true;
    e.type = type;
    e.x = SCREEN_W + 4;
    e.y = y;
    e.phase = (float)(rp2040.hwrand32() % 628) / 100.0f;
    // First shot: a beat to react, not a long free pass.
    e.nextShot = millis() + 900 + urand(700);
    float spd = 0.9f + difficulty() * 1.1f;
    switch (type) {
      case E_DRONE:
        e.vx = -spd;
        e.vy = 0;
        e.hp = 1;
        break;
      case E_SINE:
        e.vx = -spd * 0.85f;
        e.vy = 0;
        e.hp = 1;
        break;
      case E_DIVER:
        e.vx = -spd * 1.1f;
        e.vy = 0;
        e.hp = 1;
        break;
      case E_TANK:
        e.vx = -spd * 0.55f;
        e.vy = 0;
        e.hp = 2 + (difficulty() > 0.6f ? 1 : 0);
        break;
    }
    return true;
  }
  return false;
}

bool spawnPower(uint8_t type, float x, float y) {
  for (uint8_t i = 0; i < MAX_POWER; i++) {
    if (powers[i].active) continue;
    powers[i] = {x, y, type, true};
    return true;
  }
  return false;
}

void resetGame() {
  clearEntities();
  playerY = PLAY_TOP + PLAY_H / 2;
  lives = 5;
  bombs = 1;
  score = 0;
  distance = 0;
  scroll = 0;
  spreadShot = false;
  shield = false;
  rapidUntil = 0;
  invulnUntil = millis() + 1200;
  lastFire = 0;
  nextSpawn = millis() + 700;
  deadAt = 0;
  state = PLAYING;
  lastFrame = millis();
  initStars();
}

void killPlayer() {
  if (millis() < invulnUntil) return;
  if (shield) {
    shield = false;
    invulnUntil = millis() + 900;
    setNeo(40, 40, 120);
    return;
  }
  if (lives > 0) lives--;
  spreadShot = false;
  rapidUntil = 0;
  if (lives == 0) {
    state = DEAD;
    deadAt = millis();
    if (score > highScore) highScore = score;
    setNeo(80, 0, 0);
  } else {
    invulnUntil = millis() + 1500;
    playerY = PLAY_TOP + PLAY_H / 2;
    clearEntities();
    bombs = 1;
    setNeo(80, 20, 0);
  }
}

void firePlayer() {
  unsigned long now = millis();
  if (now - lastFire < fireCooldown()) return;
  lastFire = now;
  float y = playerY;
  spawnBullet(pBullets, MAX_PBUL, playerX + 5, y, 3.2f, 0, true);
  if (spreadShot) {
    spawnBullet(pBullets, MAX_PBUL, playerX + 4, y, 2.8f, -1.1f, true);
    spawnBullet(pBullets, MAX_PBUL, playerX + 4, y, 2.8f, 1.1f, true);
  }
}

void doBomb() {
  if (bombs == 0) return;
  bombs--;
  for (uint8_t i = 0; i < MAX_ENEMY; i++) {
    if (!enemies[i].active) continue;
    if (enemies[i].x < SCREEN_W) {
      score += 15;
      // chance to drop
      if (urand(100) < 35) {
        spawnPower(urand(4), enemies[i].x, enemies[i].y);
      }
      enemies[i].active = false;
    }
  }
  for (uint8_t i = 0; i < MAX_EBUL; i++) eBullets[i].active = false;
  invulnUntil = millis() + 500;
  setNeo(255, 255, 255);
}

void applyPower(uint8_t type) {
  switch (type) {
    case P_SPREAD:
      spreadShot = true;
      setNeo(0, 180, 40);
      break;
    case P_RAPID:
      rapidUntil = millis() + 6000;
      setNeo(180, 180, 0);
      break;
    case P_SHIELD:
      shield = true;
      setNeo(40, 80, 220);
      break;
    case P_BOMB:
      if (bombs < 3) bombs++;
      setNeo(220, 80, 0);
      break;
  }
  score += 25;
}

void trySpawn() {
  unsigned long now = millis();
  if (now < nextSpawn) return;

  float d = difficulty();
  unsigned long gap = (unsigned long)(900 - d * 550);
  if (gap < 280) gap = 280;
  nextSpawn = now + gap + urand(120);

  // Pick enemy mix by stage
  uint8_t roll = urand(100);
  uint8_t type = E_DRONE;
  if (distance > 200 && roll < 35) type = E_SINE;
  if (distance > 500 && roll < 25) type = E_DIVER;
  if (distance > 900 && roll < 18) type = E_TANK;

  int y0 = PLAY_TOP + 6;
  int y1 = SCREEN_H - 6;
  float y = (float)(y0 + (int)urand((uint32_t)(y1 - y0 + 1)));

  // Occasional pairs later on
  spawnEnemy(type, y);
  if (d > 0.45f && urand(100) < 30) {
    float y2 = clampf(y + (urand(2) ? 10 : -10), (float)y0, (float)y1);
    spawnEnemy(E_DRONE, y2);
  }
}

void updateStars(float dx) {
  for (uint8_t i = 0; i < MAX_STAR; i++) {
    stars[i].x -= (int16_t)(dx * stars[i].layer);
    if (stars[i].x < 0) {
      stars[i].x += SCREEN_W;
      stars[i].y = (uint8_t)(PLAY_TOP + rp2040.hwrand32() % PLAY_H);
    }
  }
}

bool hitBox(float x, float y, float hx, float hy, float r) {
  float dx = x - hx;
  float dy = y - hy;
  return dx * dx + dy * dy <= r * r;
}

void updatePlay(float dt) {
  // Hold-to-move
  bool a = digitalRead(BUTTON_A) == LOW;
  bool c = digitalRead(BUTTON_C) == LOW;
  float speed = 70.0f * dt;
  if (a) playerY -= speed;
  if (c) playerY += speed;

  playerY = clampf(playerY, (float)(PLAY_TOP + 4), (float)(SCREEN_H - 4));

  // Scroll world
  float scrollSpeed = 28.0f + difficulty() * 36.0f;
  float dx = scrollSpeed * dt;
  scroll += dx;
  distance += (uint32_t)(dx * 2.0f);
  updateStars(dx);

  if (digitalRead(BUTTON_B) == LOW) firePlayer();

  // Player bullets
  for (uint8_t i = 0; i < MAX_PBUL; i++) {
    if (!pBullets[i].active) continue;
    pBullets[i].x += pBullets[i].vx;
    pBullets[i].y += pBullets[i].vy;
    if (pBullets[i].x > SCREEN_W + 2 || pBullets[i].y < PLAY_TOP ||
        pBullets[i].y > SCREEN_H) {
      pBullets[i].active = false;
    }
  }

  // Enemies
  unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_ENEMY; i++) {
    if (!enemies[i].active) continue;
    Enemy &e = enemies[i];
    e.phase += dt * 4.0f;
    if (e.type == E_SINE) {
      e.y += sinf(e.phase) * 0.9f;
    } else if (e.type == E_DIVER) {
      float aim = playerY - e.y;
      e.vy = clampf(aim * 0.08f, -1.2f, 1.2f);
      e.y += e.vy;
    }
    e.x += e.vx;
    e.y = clampf(e.y, (float)(PLAY_TOP + 4), (float)(SCREEN_H - 4));

    if (e.x < -8) {
      e.active = false;
      continue;
    }

    // Enemy shots — light early, denser after you've had time for powerups.
    bool canShoot = true;
    if (e.type == E_DRONE || e.type == E_SINE) {
      canShoot = distance > 280;  // brief mute, then they join in
    }
    if (canShoot && now >= e.nextShot && e.x < SCREEN_W - 4) {
      float fi = fireIntensity();
      float interval = 1350.0f - fi * 800.0f;  // ~1350ms → ~550ms
      if (interval < 520.0f) interval = 520.0f;
      e.nextShot = now + (unsigned long)interval + urand(280);
      // Fire chance rises with intensity (~50% early → ~90% late)
      uint32_t fireChance = 50 + (uint32_t)(fi * 40.0f);
      if (urand(100) < fireChance) {
        float bvx = -1.45f - fi * 0.75f;
        float bvy = 0;
        if (e.type == E_DIVER || e.type == E_TANK) {
          bvy = clampf((playerY - e.y) * (0.035f + fi * 0.03f), -0.75f, 0.75f);
        }
        spawnBullet(eBullets, MAX_EBUL, e.x - 2, e.y, bvx, bvy, false);
      }
    }

    // Collide player
    if (hitBox(e.x, e.y, playerX, playerY, 5.5f)) {
      e.active = false;
      killPlayer();
    }
  }

  // Enemy bullets
  for (uint8_t i = 0; i < MAX_EBUL; i++) {
    if (!eBullets[i].active) continue;
    eBullets[i].x += eBullets[i].vx;
    eBullets[i].y += eBullets[i].vy;
    if (eBullets[i].x < -2 || eBullets[i].y < PLAY_TOP ||
        eBullets[i].y > SCREEN_H) {
      eBullets[i].active = false;
      continue;
    }
    if (hitBox(eBullets[i].x, eBullets[i].y, playerX, playerY, 4.0f)) {
      eBullets[i].active = false;
      killPlayer();
    }
  }

  // Player bullets vs enemies
  for (uint8_t i = 0; i < MAX_PBUL; i++) {
    if (!pBullets[i].active) continue;
    for (uint8_t j = 0; j < MAX_ENEMY; j++) {
      if (!enemies[j].active) continue;
      if (!hitBox(pBullets[i].x, pBullets[i].y, enemies[j].x, enemies[j].y,
                  5.0f))
        continue;
      pBullets[i].active = false;
      if (enemies[j].hp > 0) enemies[j].hp--;
      if (enemies[j].hp == 0) {
        score += (enemies[j].type == E_TANK) ? 40 : 10;
        if (urand(100) < 22) {
          spawnPower(urand(4), enemies[j].x, enemies[j].y);
        }
        enemies[j].active = false;
      }
      break;
    }
  }

  // Powerups
  for (uint8_t i = 0; i < MAX_POWER; i++) {
    if (!powers[i].active) continue;
    powers[i].x -= dx * 0.6f + 0.4f;
    if (powers[i].x < -6) {
      powers[i].active = false;
      continue;
    }
    if (hitBox(powers[i].x, powers[i].y, playerX, playerY, 6.0f)) {
      applyPower(powers[i].type);
      powers[i].active = false;
    }
  }

  trySpawn();
}

void handleInput() {
  bool a = digitalRead(BUTTON_A) == LOW;
  bool b = digitalRead(BUTTON_B) == LOW;
  bool c = digitalRead(BUTTON_C) == LOW;
  bool boot = BOOTSEL;

  if (millis() >= inputLockUntil) {
    if (state == TITLE || state == DEAD) {
      if ((a && !aDown) || (b && !bDown) || (c && !cDown)) {
        if (state == DEAD && millis() - deadAt < 400) {
          // ignore immediate restart
        } else {
          resetGame();
        }
      }
    } else if (state == PLAYING) {
      if (boot && !bootDown) doBomb();
    }
  }

  aDown = a;
  bDown = b;
  cDown = c;
  bootDown = boot;
}

void drawShip(int16_t x, int16_t y) {
  // Nose-right triangle
  display.fillTriangle(x - 3, y - 3, x - 3, y + 3, x + 5, y, SH110X_WHITE);
  if (shield) {
    display.drawCircle(x, y, 6, SH110X_WHITE);
  }
}

void drawEnemy(const Enemy &e) {
  int16_t x = (int16_t)e.x;
  int16_t y = (int16_t)e.y;
  // Chunkier silhouettes so they read against the sparse starfield.
  switch (e.type) {
    case E_DRONE:
      display.fillRect(x - 3, y - 3, 8, 6, SH110X_WHITE);
      display.drawPixel(x - 3, y, SH110X_BLACK);
      break;
    case E_SINE:
      display.fillTriangle(x + 4, y, x - 4, y - 4, x - 4, y + 4, SH110X_WHITE);
      break;
    case E_DIVER:
      display.fillCircle(x, y, 4, SH110X_WHITE);
      display.drawPixel(x - 1, y, SH110X_BLACK);
      display.drawPixel(x + 1, y, SH110X_BLACK);
      break;
    case E_TANK:
      display.fillRect(x - 4, y - 4, 10, 8, SH110X_WHITE);
      display.fillRect(x + 2, y - 1, 3, 2, SH110X_BLACK);
      break;
  }
}

void drawPower(const Powerup &p) {
  char ch = '?';
  switch (p.type) {
    case P_SPREAD: ch = 'S'; break;
    case P_RAPID: ch = 'R'; break;
    case P_SHIELD: ch = 'H'; break;
    case P_BOMB: ch = 'B'; break;
  }
  display.drawRect((int16_t)p.x - 4, (int16_t)p.y - 4, 9, 9, SH110X_WHITE);
  display.setCursor((int16_t)p.x - 3, (int16_t)p.y - 3);
  display.write(ch);
}

void drawHud() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(F("S:"));
  display.print(score);
  display.setCursor(52, 0);
  display.print(F("L:"));
  display.print(lives);
  display.setCursor(78, 0);
  display.print(F("B:"));
  display.print(bombs);
  if (spreadShot) {
    display.setCursor(100, 0);
    display.print(F("S"));
  }
  if (millis() < rapidUntil) {
    display.setCursor(108, 0);
    display.print(F("R"));
  }
  if (shield) {
    display.setCursor(116, 0);
    display.print(F("H"));
  }
  display.drawFastHLine(0, HUD_H - 1, SCREEN_W, SH110X_WHITE);
}

void drawTitle() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(28, 10);
  display.print(F("VOID RUNNER"));
  display.setCursor(10, 28);
  display.print(F("A/C move  B fire"));
  display.setCursor(22, 40);
  display.print(F("BOOT bomb"));
  display.setCursor(16, 54);
  display.print(F("press A/B/C"));
  if (highScore) {
    display.setCursor(70, 54);
    display.print(F("Hi"));
    display.print(highScore);
  }
  display.display();
}

void drawDead() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(34, 16);
  display.print(F("GAME OVER"));
  display.setCursor(28, 32);
  display.print(F("Score "));
  display.print(score);
  display.setCursor(28, 44);
  display.print(F("Dist "));
  display.print(distance);
  display.setCursor(16, 56);
  display.print(F("A/B/C retry"));
  display.display();
}

void drawPlay() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  for (uint8_t i = 0; i < MAX_STAR; i++) {
    display.drawPixel(stars[i].x, stars[i].y, SH110X_WHITE);
  }

  for (uint8_t i = 0; i < MAX_PBUL; i++) {
    if (!pBullets[i].active) continue;
    display.fillRect((int16_t)pBullets[i].x, (int16_t)pBullets[i].y, 3, 1,
                     SH110X_WHITE);
  }
  for (uint8_t i = 0; i < MAX_EBUL; i++) {
    if (!eBullets[i].active) continue;
    display.fillRect((int16_t)eBullets[i].x, (int16_t)eBullets[i].y, 2, 2,
                     SH110X_WHITE);
  }
  for (uint8_t i = 0; i < MAX_ENEMY; i++) {
    if (enemies[i].active) drawEnemy(enemies[i]);
  }
  for (uint8_t i = 0; i < MAX_POWER; i++) {
    if (powers[i].active) drawPower(powers[i]);
  }

  bool blink = (millis() < invulnUntil) && ((millis() / 80) & 1);
  if (!blink) drawShip((int16_t)playerX, (int16_t)playerY);

  drawHud();
  display.display();
}

void updateNeo() {
  unsigned long now = millis();
  if (now - lastPixel < 40) return;
  lastPixel = now;
  if (state == TITLE) {
    hue += 200;
    pixel.setPixelColor(0, pixel.gamma32(pixel.ColorHSV(hue, 255, 140)));
    pixel.show();
  } else if (state == PLAYING) {
    if (millis() < invulnUntil) setNeo(60, 20, 0);
    else if (shield) setNeo(20, 40, 100);
    else setNeo(0, 40, 10);
  }
}

void setup() {
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

#if defined(NEOPIXEL_POWER)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
#endif
  pixel.begin();
  pixel.setBrightness(45);
  setNeo(0, 0, 20);

  delay(250);
  if (!display.begin(0x3C, true)) {
    display.begin(0x3D, true);
  }
  display.setRotation(1);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.display();

  aDown = digitalRead(BUTTON_A) == LOW;
  bDown = digitalRead(BUTTON_B) == LOW;
  cDown = digitalRead(BUTTON_C) == LOW;
  bootDown = true;  // ignore BOOT edge at boot
  inputLockUntil = millis() + 800;

  initStars();
  lastFrame = millis();
}

void loop() {
  handleInput();
  updateNeo();

  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  lastFrame = now;

  if (state == TITLE) {
    drawTitle();
  } else if (state == DEAD) {
    drawDead();
  } else {
    updatePlay(dt);
    if (state == PLAYING) drawPlay();
    else drawDead();
  }
}
