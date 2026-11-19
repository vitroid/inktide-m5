// inktide — M5Stack Core port of the autonomous four-faction breaker.
// 16x16 territory, four balls. Own cells are open; everything else bounces
// and is claimed. A cut that strands enemy cells away from their ball awards
// that enclave to the cutter. Each faction keeps three awards in enemy land;
// only its own ball can break them (speed x2, or a split). Effects last 40s,
// and a second pickup while one is live restarts both clocks.
//
// Buttons: A start over, B volume (low / mid / high), C mute.
// Boots muted. The speaker stays off until C is pressed, so power-on is silent.
// Field is 224x224 (14px cells) on the left. Scores sit in the right margin.

#include <M5Unified.h>

#include <esp_system.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

constexpr int GRID_W = 16;
constexpr int GRID_H = 16;
constexpr int CELL = 14;
constexpr int FIELD_PX_W = GRID_W * CELL;
constexpr int FIELD_PX_H = GRID_H * CELL;
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int FIELD_X = 0;
constexpr int FIELD_Y = (SCREEN_H - FIELD_PX_H) / 2;
constexpr int HUD_X = FIELD_PX_W;

constexpr float FRAME_MS = 1000.f / 16.f;
constexpr float BALL_SPEED = 8.8f * 3.f * (CELL / 32.f) / 2;
constexpr float BALL_RADIUS = 4.f;
constexpr int MAX_BOUNCES = 32;
constexpr float TOI_EPS = 1e-5f;
constexpr float SEP = 1e-3f;
constexpr int AWARD_FRAMES = 16 * 40;
constexpr int AWARDS_PER_FACTION = 3;
constexpr int START_NEIGHBORHOOD = 1;
constexpr int FACTION_N = 4;
constexpr int MAX_BALLS = 128;
constexpr int MAX_AWARDS = 16;
constexpr int CELL_N = GRID_W * GRID_H;

constexpr int8_t HARD_ID = -1;
constexpr int8_t VOID_ID = -2;

constexpr uint8_t KIND_WALL = 0;
constexpr uint8_t KIND_CELL = 1;
constexpr uint8_t KIND_HARD = 2;
constexpr uint8_t KIND_AWARD = 3;
constexpr uint8_t AWARD_SPLIT = 0;
constexpr uint8_t AWARD_SPEED = 1;

constexpr int CH_WALL = 0;
constexpr int CH_CELL = 1;
constexpr int CH_AWARD = 2;
constexpr int CH_CAPTURE = 3;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t COL_HARD = rgb565(0x6f, 0x6f, 0x76);
constexpr uint16_t COL_HARD_IN = rgb565(0x9a, 0x9a, 0xa2);
constexpr uint16_t COL_VOID = rgb565(0x05, 0x05, 0x06);
constexpr uint16_t COL_BG = rgb565(0x0c, 0x0c, 0x0e);
constexpr uint16_t COL_TEXT = rgb565(0xd2, 0xce, 0xc6);
constexpr uint16_t COL_DIM = rgb565(0x8a, 0x86, 0x80);
constexpr uint16_t COL_RIM = rgb565(0xe8, 0xd4, 0x8a);

constexpr int ORTHO[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

struct Faction {
  const char* label;
  float hue, sat, light;
  uint16_t territory;
  uint16_t barLit;
  uint16_t barBright;
  uint16_t ballFill;
  uint16_t ballOutline;
};

struct Ball {
  float x, y, vx, vy, radius;
  int8_t home;
  bool extra;
};

struct Award {
  int8_t col, row, faction;
  uint8_t kind;
};

struct Buff {
  int speedLeft;
  int splitLeft;
  float speedMult;
};

struct Hit {
  float t, nx, ny;
  int16_t col, row;
  uint8_t kind;
  bool ok;
};

struct BallSpec {
  int8_t home;
  float startX, startY, angle;
};

constexpr BallSpec BALL_SPECS[FACTION_N] = {
    {0, 0.50f, 0.22f, 1.15f},
    {1, 0.78f, 0.50f, 3.14159265f + 0.32f},
    {2, 0.50f, 0.78f, -1.22f},
    {3, 0.22f, 0.50f, 0.28f},
};

Faction factions[FACTION_N];
int8_t owner[GRID_H][GRID_W];
bool dirty[GRID_H][GRID_W];
bool pending[GRID_H][GRID_W];
Award awards[MAX_AWARDS];
int awardN = 0;
Ball balls[MAX_BALLS];
int ballN = 0;
Ball drawn[MAX_BALLS];
int drawnN = 0;
Ball newcomers[MAX_BALLS];
int newN = 0;
Buff buffs[FACTION_N];

struct Capture {
  bool active;
  bool outlineOnly;
  int8_t capturer;
  int8_t victim;
  int filled;
  int count;
  int8_t cols[CELL_N];
  int8_t rows[CELL_N];
} capture;

constexpr uint8_t VOLUME_STEPS[] = {50, 120, 220};
constexpr int VOLUME_STEP_N = 3;
constexpr char VOLUME_LABELS[VOLUME_STEP_N][8] = {"B low  ", "B mid  ", "B high "};

int volumeStep = 1;
bool muted = true;
bool speakerOk = false;
bool speakerStarted = false;
uint32_t bootAt = 0;
float accrued = 0.f;
uint32_t lastMs = 0;

struct ToneEv {
  uint32_t when;
  uint16_t freq;
  uint16_t dur;
  uint8_t ch;
  uint8_t vol;
};

constexpr int TONE_Q = 32;
ToneEv toneQ[TONE_Q];
int toneN = 0;

struct Burst {
  uint32_t at;
  int wall;
  int cell;
} burst = {0, 0, 0};

constexpr uint8_t SQUARE_WAV[16] = {
    255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0,
};

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float hueChannel(float p, float q, float t) {
  if (t < 0.f) t += 1.f;
  if (t > 1.f) t -= 1.f;
  if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
  if (t < 0.5f) return q;
  if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
  return p;
}

uint16_t hsl565(float h, float s, float l) {
  s *= 0.01f;
  l *= 0.01f;
  float r, g, b;
  if (s <= 0.f) {
    r = g = b = l;
  } else {
    float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
    float p = 2.f * l - q;
    float hk = h / 360.f;
    r = hueChannel(p, q, hk + 1.f / 3.f);
    g = hueChannel(p, q, hk);
    b = hueChannel(p, q, hk - 1.f / 3.f);
  }
  auto byte = [](float c) {
    int v = (int)(c * 255.f + 0.5f);
    return (uint8_t)clampi(v, 0, 255);
  };
  return rgb565(byte(r), byte(g), byte(b));
}

void initFactions() {
  const char* labels[FACTION_N] = {"ORG", "GRN", "BLU", "MAG"};
  const float hsl[FACTION_N][3] = {
      {41.f, 82.f, 40.f},
      {108.f, 50.f, 38.f},
      {258.f, 52.f, 30.f},
      {327.f, 48.f, 46.f},
  };
  for (int i = 0; i < FACTION_N; i++) {
    Faction& f = factions[i];
    f.label = labels[i];
    f.hue = hsl[i][0];
    f.sat = hsl[i][1];
    f.light = hsl[i][2];
    f.territory = hsl565(f.hue, f.sat, f.light);
    f.barLit = hsl565(f.hue, clampf(f.sat + 4.f, 0.f, 90.f),
                      clampf(f.light + 12.f, 0.f, 78.f));
    f.barBright = hsl565(f.hue, clampf(f.sat + 8.f, 0.f, 92.f),
                         clampf(f.light + 24.f, 0.f, 86.f));
    f.ballFill = hsl565(f.hue, f.sat, clampf(f.light + 22.f, 0.f, 72.f));
    f.ballOutline =
        hsl565(f.hue, clampf(f.sat - 18.f, 28.f, 100.f),
               clampf(f.light + 48.f, 0.f, 88.f));
  }
}

void markDirty(int col, int row) {
  if (col < 0 || row < 0 || col >= GRID_W || row >= GRID_H) return;
  dirty[row][col] = true;
}

void markBall(float x, float y, float radius) {
  int c0 = (int)floorf((x - radius) / CELL);
  int c1 = (int)floorf((x + radius) / CELL);
  int r0 = (int)floorf((y - radius) / CELL);
  int r1 = (int)floorf((y + radius) / CELL);
  for (int row = r0; row <= r1; row++) {
    for (int col = c0; col <= c1; col++) markDirty(col, row);
  }
}

void queueTone(uint32_t when, int freq, int dur, int ch, int vol) {
  if (!speakerOk || muted || toneN >= TONE_Q) return;
  if (vol < 1) vol = 1;
  if (vol > 255) vol = 255;
  if (freq < 20) freq = 20;
  ToneEv& ev = toneQ[toneN++];
  ev.when = when;
  ev.freq = (uint16_t)freq;
  ev.dur = (uint16_t)dur;
  ev.ch = (uint8_t)ch;
  ev.vol = (uint8_t)vol;
}

void drainTones(uint32_t now) {
  if (!speakerOk || muted) return;
  int w = 0;
  for (int i = 0; i < toneN; i++) {
    if (toneQ[i].when <= now) {
      M5.Speaker.setChannelVolume(toneQ[i].ch, toneQ[i].vol);
      M5.Speaker.tone(toneQ[i].freq, toneQ[i].dur, toneQ[i].ch, true, SQUARE_WAV,
                      sizeof(SQUARE_WAV));
    } else {
      toneQ[w++] = toneQ[i];
    }
  }
  toneN = w;
}

float sfxScale(int kind, int cap) {
  uint32_t now = millis();
  if (now - burst.at > 45) {
    burst.wall = 0;
    burst.cell = 0;
  }
  burst.at = now;
  int& n = kind == 0 ? burst.wall : burst.cell;
  n += 1;
  if (n > cap) return 0.f;
  return 1.f / sqrtf((float)n);
}

int mixVol(float base, float scale) {
  int v = (int)(255.f * base * scale);
  if (v < 1) return 1;
  if (v > 255) return 255;
  return v;
}

void playWallHit() {
  float scale = sfxScale(0, 8);
  if (scale <= 0.f) return;
  int freq = 1680 + (int)random(240);
  queueTone(millis(), freq, 16, CH_WALL, mixVol(0.85f, scale));
}

void playCellBreak() {
  float scale = sfxScale(1, 6);
  if (scale <= 0.f) return;
  uint32_t now = millis();
  int vol = mixVol(0.9f, scale);
  int freq = 280 + (int)random(30);
  queueTone(now, freq, 40, CH_CELL, vol);
  queueTone(now + 40, 170, 50, CH_CELL, vol);
}

void playAward() {
  const int notes[3] = {523, 587, 659};
  uint32_t t = millis() + 1;
  for (int i = 0; i < 3; i++) {
    queueTone(t + (uint32_t)i * 62, notes[i], 57, CH_AWARD, 200);
  }
}

void playCaptureTick(int index, int total) {
  float u = total <= 1 ? 1.f : (float)index / (float)(total - 1);
  float freq = 262.f * powf(2093.f / 262.f, u);
  queueTone(millis(), (int)freq, 22, CH_CAPTURE, 180);
}

void clearBuffs() {
  for (int i = 0; i < FACTION_N; i++) {
    buffs[i].speedLeft = 0;
    buffs[i].splitLeft = 0;
    buffs[i].speedMult = 1.f;
  }
}

float currentSpeed(const Ball& ball) {
  const Buff& buff = buffs[ball.home];
  if (buff.speedLeft <= 0) return BALL_SPEED;
  return BALL_SPEED * buff.speedMult;
}

void setSpeed(Ball& ball, float speed) {
  float mag = hypotf(ball.vx, ball.vy);
  if (mag == 0.f) return;
  float scale = speed / mag;
  ball.vx *= scale;
  ball.vy *= scale;
}

void setSpeed(Ball& ball) { setSpeed(ball, currentSpeed(ball)); }

const Award* awardAt(int col, int row) {
  for (int i = 0; i < awardN; i++) {
    if (awards[i].col == col && awards[i].row == row) return &awards[i];
  }
  return nullptr;
}

void removeAwardAt(int col, int row) {
  for (int i = 0; i < awardN; i++) {
    if (awards[i].col == col && awards[i].row == row) {
      awards[i] = awards[awardN - 1];
      awardN -= 1;
      return;
    }
  }
}

bool pickDeepEnemyCell(int factionId, int& outCol, int& outRow) {
  static int16_t dist[GRID_H][GRID_W];
  static int16_t qx[CELL_N];
  static int16_t qy[CELL_N];
  static int8_t deepC[CELL_N];
  static int8_t deepR[CELL_N];
  static int8_t fallC[CELL_N];
  static int8_t fallR[CELL_N];

  bool taken[GRID_H][GRID_W];
  memset(taken, 0, sizeof(taken));
  for (int i = 0; i < awardN; i++) taken[awards[i].row][awards[i].col] = true;

  int qn = 0;
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      dist[row][col] = -1;
      if (owner[row][col] != factionId) continue;
      dist[row][col] = 0;
      qx[qn] = (int16_t)col;
      qy[qn] = (int16_t)row;
      qn += 1;
    }
  }
  if (qn == 0) return false;

  int head = 0;
  while (head < qn) {
    int col = qx[head];
    int row = qy[head];
    head += 1;
    for (int k = 0; k < 4; k++) {
      int nc = col + ORTHO[k][0];
      int nr = row + ORTHO[k][1];
      if (nc < 0 || nr < 0 || nc >= GRID_W || nr >= GRID_H) continue;
      if (dist[nr][nc] != -1) continue;
      dist[nr][nc] = (int16_t)(dist[row][col] + 1);
      qx[qn] = (int16_t)nc;
      qy[qn] = (int16_t)nr;
      qn += 1;
    }
  }

  int deepN = 0;
  int fallN = 0;
  for (int row = 1; row < GRID_H - 1; row++) {
    for (int col = 1; col < GRID_W - 1; col++) {
      if (taken[row][col]) continue;
      int8_t id = owner[row][col];
      if (id == HARD_ID || id == factionId) continue;
      fallC[fallN] = (int8_t)col;
      fallR[fallN] = (int8_t)row;
      fallN += 1;
      if (dist[row][col] >= 2) {
        deepC[deepN] = (int8_t)col;
        deepR[deepN] = (int8_t)row;
        deepN += 1;
      }
    }
  }
  int poolN = deepN > 0 ? deepN : fallN;
  if (poolN == 0) return false;
  int pick = (int)random((long)poolN);
  if (deepN > 0) {
    outCol = deepC[pick];
    outRow = deepR[pick];
  } else {
    outCol = fallC[pick];
    outRow = fallR[pick];
  }
  return true;
}

void placeAward(int factionId) {
  if (awardN >= MAX_AWARDS) return;
  int col, row;
  if (!pickDeepEnemyCell(factionId, col, row)) return;
  Award& award = awards[awardN++];
  award.col = (int8_t)col;
  award.row = (int8_t)row;
  award.faction = (int8_t)factionId;
  award.kind = random(2) == 0 ? AWARD_SPLIT : AWARD_SPEED;
  markDirty(col, row);
}

int factionAwardCount(int factionId) {
  int n = 0;
  for (int i = 0; i < awardN; i++) {
    if (awards[i].faction == factionId) n += 1;
  }
  return n;
}

bool factionEffectsIdle(int factionId) {
  const Buff& buff = buffs[factionId];
  return buff.speedLeft <= 0 && buff.splitLeft <= 0;
}

void refillAwards() {
  for (int faction = 0; faction < FACTION_N; faction++) {
    if (!factionEffectsIdle(faction)) continue;
    while (factionAwardCount(faction) < AWARDS_PER_FACTION) {
      int before = awardN;
      placeAward(faction);
      if (awardN == before) break;
    }
  }
}

void seedStartNeighborhoods() {
  for (int i = 0; i < FACTION_N; i++) {
    const BallSpec& spec = BALL_SPECS[i];
    int col = clampi((int)floorf(spec.startX * GRID_W), 0, GRID_W - 1);
    int row = clampi((int)floorf(spec.startY * GRID_H), 0, GRID_H - 1);
    for (int dr = -START_NEIGHBORHOOD; dr <= START_NEIGHBORHOOD; dr++) {
      for (int dc = -START_NEIGHBORHOOD; dc <= START_NEIGHBORHOOD; dc++) {
        int c = col + dc;
        int r = row + dr;
        if (c < 0 || r < 0 || c >= GRID_W || r >= GRID_H) continue;
        owner[r][c] = spec.home;
      }
    }
  }
}

void createField() {
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) owner[row][col] = VOID_ID;
  }
  seedStartNeighborhoods();
  awardN = 0;
  refillAwards();
}

void spawnBalls() {
  ballN = 0;
  const float fieldW = (float)FIELD_PX_W;
  const float fieldH = (float)FIELD_PX_H;
  for (int i = 0; i < FACTION_N; i++) {
    const BallSpec& spec = BALL_SPECS[i];
    Ball& ball = balls[ballN++];
    ball.x = fieldW * spec.startX;
    ball.y = fieldH * spec.startY;
    ball.vx = BALL_SPEED * cosf(spec.angle);
    ball.vy = BALL_SPEED * sinf(spec.angle);
    ball.radius = BALL_RADIUS;
    ball.home = spec.home;
    ball.extra = false;
  }
}

bool betterHit(const Hit& best, float t, float nx, float ny) {
  if (!best.ok) return true;
  if (t < best.t - 1e-5f) return true;
  if (t > best.t + 1e-5f) return false;
  bool oldCorner = fabsf(best.nx) > 1e-4f && fabsf(best.ny) > 1e-4f;
  bool newCorner = fabsf(nx) > 1e-4f && fabsf(ny) > 1e-4f;
  return newCorner && !oldCorner;
}

void takeHit(Hit& best, float t, float nx, float ny, int col, int row, uint8_t kind,
             float remaining, float vx, float vy) {
  if (t < -TOI_EPS || t > remaining + TOI_EPS) return;
  float clamped = clampf(t, 0.f, remaining);
  if (vx * nx + vy * ny >= -1e-6f) return;
  if (!betterHit(best, clamped, nx, ny)) return;
  best.ok = true;
  best.t = clamped;
  best.nx = nx;
  best.ny = ny;
  best.col = (int16_t)col;
  best.row = (int16_t)row;
  best.kind = kind;
}

bool circleHitsCell(const Ball& ball, int col, int row, float& nx, float& ny) {
  float left = col * (float)CELL;
  float top = row * (float)CELL;
  float right = left + CELL;
  float bottom = top + CELL;
  float closestX = clampf(ball.x, left, right);
  float closestY = clampf(ball.y, top, bottom);
  float dx = ball.x - closestX;
  float dy = ball.y - closestY;
  float dist2 = dx * dx + dy * dy;
  float r = ball.radius;
  if (dist2 > r * r) return false;
  if (dist2 < 1e-6f) {
    float toLeft = ball.x - left;
    float toRight = right - ball.x;
    float toTop = ball.y - top;
    float toBottom = bottom - ball.y;
    float shallow = toLeft;
    nx = -1.f;
    ny = 0.f;
    if (toRight < shallow) {
      shallow = toRight;
      nx = 1.f;
      ny = 0.f;
    }
    if (toTop < shallow) {
      shallow = toTop;
      nx = 0.f;
      ny = -1.f;
    }
    if (toBottom < shallow) {
      nx = 0.f;
      ny = 1.f;
    }
    return true;
  }
  float dist = sqrtf(dist2);
  nx = dx / dist;
  ny = dy / dist;
  return true;
}

bool rayCircleEnterT(float px, float py, float vx, float vy, float cx, float cy,
                     float radius, float remaining, float& outT) {
  float dx = px - cx;
  float dy = py - cy;
  float a = vx * vx + vy * vy;
  if (a < 1e-12f) return false;
  float b = 2.f * (dx * vx + dy * vy);
  float c = dx * dx + dy * dy - radius * radius;
  float disc = b * b - 4.f * a * c;
  if (disc < 0.f) return false;
  float root = sqrtf(disc);
  float tEnter = (-b - root) / (2.f * a);
  float tExit = (-b + root) / (2.f * a);
  if (tEnter >= 0.f && tEnter <= remaining) {
    outT = tEnter;
    return true;
  }
  if (c <= 0.f && tExit >= 0.f) {
    outT = 0.f;
    return true;
  }
  return false;
}

Hit cellTOI(const Ball& ball, int col, int row, float remaining) {
  float left = col * (float)CELL;
  float top = row * (float)CELL;
  float right = left + CELL;
  float bottom = top + CELL;
  float px = ball.x;
  float py = ball.y;
  float vx = ball.vx;
  float vy = ball.vy;
  float r = ball.radius;
  Hit best;
  best.ok = false;

  float nx, ny;
  if (circleHitsCell(ball, col, row, nx, ny)) {
    takeHit(best, 0.f, nx, ny, col, row, KIND_CELL, remaining, vx, vy);
  }

  if (fabsf(vy) > 1e-8f) {
    float tTop = (top - r - py) / vy;
    float xTop = px + tTop * vx;
    if (xTop >= left && xTop <= right) {
      takeHit(best, tTop, 0.f, -1.f, col, row, KIND_CELL, remaining, vx, vy);
    }
    float tBottom = (bottom + r - py) / vy;
    float xBottom = px + tBottom * vx;
    if (xBottom >= left && xBottom <= right) {
      takeHit(best, tBottom, 0.f, 1.f, col, row, KIND_CELL, remaining, vx, vy);
    }
  }
  if (fabsf(vx) > 1e-8f) {
    float tLeft = (left - r - px) / vx;
    float yLeft = py + tLeft * vy;
    if (yLeft >= top && yLeft <= bottom) {
      takeHit(best, tLeft, -1.f, 0.f, col, row, KIND_CELL, remaining, vx, vy);
    }
    float tRight = (right + r - px) / vx;
    float yRight = py + tRight * vy;
    if (yRight >= top && yRight <= bottom) {
      takeHit(best, tRight, 1.f, 0.f, col, row, KIND_CELL, remaining, vx, vy);
    }
  }

  const float corners[4][2] = {
      {left, top}, {right, top}, {left, bottom}, {right, bottom}};
  for (int i = 0; i < 4; i++) {
    float cx = corners[i][0];
    float cy = corners[i][1];
    float t;
    if (!rayCircleEnterT(px, py, vx, vy, cx, cy, r, remaining, t)) continue;
    float qx = px + t * vx;
    float qy = py + t * vy;
    bool outside = false;
    if (i == 0) outside = qx <= left && qy <= top;
    if (i == 1) outside = qx >= right && qy <= top;
    if (i == 2) outside = qx <= left && qy >= bottom;
    if (i == 3) outside = qx >= right && qy >= bottom;
    if (!outside) continue;
    float nnx = qx - cx;
    float nny = qy - cy;
    float mag = hypotf(nnx, nny);
    if (mag < 1e-6f) continue;
    takeHit(best, t, nnx / mag, nny / mag, col, row, KIND_CELL, remaining, vx, vy);
  }
  return best;
}

Hit wallTOI(const Ball& ball, float remaining) {
  Hit best;
  best.ok = false;
  float x = ball.x;
  float y = ball.y;
  float vx = ball.vx;
  float vy = ball.vy;
  float r = ball.radius;
  float fieldW = (float)FIELD_PX_W;
  float fieldH = (float)FIELD_PX_H;
  if (fabsf(vx) > 1e-8f) {
    takeHit(best, (r - x) / vx, 1.f, 0.f, 0, 0, KIND_WALL, remaining, vx, vy);
    takeHit(best, (fieldW - r - x) / vx, -1.f, 0.f, 0, 0, KIND_WALL, remaining, vx,
            vy);
  }
  if (fabsf(vy) > 1e-8f) {
    takeHit(best, (r - y) / vy, 0.f, 1.f, 0, 0, KIND_WALL, remaining, vx, vy);
    takeHit(best, (fieldH - r - y) / vy, 0.f, -1.f, 0, 0, KIND_WALL, remaining, vx,
            vy);
  }
  return best;
}

Hit findEarliestHit(const Ball& ball, float remaining) {
  Hit best = wallTOI(ball, remaining);
  float r = ball.radius;
  float x1 = ball.x + ball.vx * remaining;
  float y1 = ball.y + ball.vy * remaining;
  int minCol = (int)floorf((fminf(ball.x, x1) - r) / CELL);
  int maxCol = (int)floorf((fmaxf(ball.x, x1) + r) / CELL);
  int minRow = (int)floorf((fminf(ball.y, y1) - r) / CELL);
  int maxRow = (int)floorf((fmaxf(ball.y, y1) + r) / CELL);
  if (minCol < 0) minCol = 0;
  if (minRow < 0) minRow = 0;
  if (maxCol > GRID_W - 1) maxCol = GRID_W - 1;
  if (maxRow > GRID_H - 1) maxRow = GRID_H - 1;

  for (int row = minRow; row <= maxRow; row++) {
    for (int col = minCol; col <= maxCol; col++) {
      int8_t cellOwner = owner[row][col];
      const Award* awardHere = awardAt(col, row);
      if (!awardHere && cellOwner != HARD_ID && cellOwner == ball.home) continue;
      Hit hit = cellTOI(ball, col, row, remaining);
      if (!hit.ok) continue;
      if (awardHere) {
        hit.kind = awardHere->faction == ball.home ? KIND_AWARD : KIND_HARD;
      } else {
        hit.kind = cellOwner == HARD_ID ? KIND_HARD : KIND_CELL;
      }
      bool beatsWall = hit.kind == KIND_AWARD && best.ok && best.kind == KIND_WALL &&
                       hit.t <= best.t + 0.1f;
      if (betterHit(best, hit.t, hit.nx, hit.ny) || beatsWall) best = hit;
    }
  }
  return best;
}

struct Comp {
  int begin;
  int count;
};

Comp comps[CELL_N];
int compN = 0;
int16_t compCol[CELL_N];
int16_t compRow[CELL_N];

void componentsOf(int factionId) {
  static uint8_t seen[GRID_H][GRID_W];
  static int stackC[CELL_N];
  static int stackR[CELL_N];
  memset(seen, 0, sizeof(seen));
  compN = 0;
  int filled = 0;
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      if (seen[row][col] || owner[row][col] != factionId) continue;
      int begin = filled;
      int sp = 0;
      seen[row][col] = 1;
      stackC[sp] = col;
      stackR[sp] = row;
      sp += 1;
      while (sp > 0) {
        sp -= 1;
        int c = stackC[sp];
        int r = stackR[sp];
        compCol[filled] = (int16_t)c;
        compRow[filled] = (int16_t)r;
        filled += 1;
        for (int k = 0; k < 4; k++) {
          int nc = c + ORTHO[k][0];
          int nr = r + ORTHO[k][1];
          if (nc < 0 || nr < 0 || nc >= GRID_W || nr >= GRID_H) continue;
          if (seen[nr][nc] || owner[nr][nc] != factionId) continue;
          seen[nr][nc] = 1;
          stackC[sp] = nc;
          stackR[sp] = nr;
          sp += 1;
        }
      }
      comps[compN].begin = begin;
      comps[compN].count = filled - begin;
      compN += 1;
    }
  }
}

bool compContains(int index, int col, int row) {
  int begin = comps[index].begin;
  int end = begin + comps[index].count;
  for (int i = begin; i < end; i++) {
    if (compCol[i] == col && compRow[i] == row) return true;
  }
  return false;
}

int liveComponentIndex(const Ball& ball) {
  if (compN == 0) return -1;
  int col = clampi((int)floorf(ball.x / CELL), 0, GRID_W - 1);
  int row = clampi((int)floorf(ball.y / CELL), 0, GRID_H - 1);
  if (owner[row][col] == ball.home) {
    for (int i = 0; i < compN; i++) {
      if (compContains(i, col, row)) return i;
    }
  }
  int best = 0;
  float bestDist = 1e30f;
  for (int i = 0; i < compN; i++) {
    int begin = comps[i].begin;
    int end = begin + comps[i].count;
    for (int k = begin; k < end; k++) {
      float dx = ball.x - (compCol[k] + 0.5f) * CELL;
      float dy = ball.y - (compRow[k] + 0.5f) * CELL;
      float dist = dx * dx + dy * dy;
      if (dist < bestDist) {
        bestDist = dist;
        best = i;
      }
    }
  }
  return best;
}

int collectEnclaves(int victimId, int* outCol, int* outRow) {
  if (victimId == HARD_ID || victimId == VOID_ID) return 0;
  componentsOf(victimId);
  if (compN <= 1) return 0;
  uint8_t occupied[CELL_N];
  memset(occupied, 0, sizeof(occupied));
  for (int pass = 0; pass < 2; pass++) {
    int n = pass == 0 ? ballN : newN;
    const Ball* src = pass == 0 ? balls : newcomers;
    for (int i = 0; i < n; i++) {
      if (src[i].home != victimId) continue;
      int index = liveComponentIndex(src[i]);
      if (index >= 0) occupied[index] = 1;
    }
  }
  int n = 0;
  for (int i = 0; i < compN; i++) {
    if (occupied[i]) continue;
    int begin = comps[i].begin;
    int end = begin + comps[i].count;
    for (int k = begin; k < end; k++) {
      int col = compCol[k];
      int row = compRow[k];
      if (awardAt(col, row)) continue;
      outCol[n] = col;
      outRow[n] = row;
      n += 1;
    }
  }
  return n;
}

void dismissCapture() {
  if (!capture.active) return;
  for (int i = 0; i < capture.count; i++) {
    int col = capture.cols[i];
    int row = capture.rows[i];
    if (!pending[row][col]) continue;
    pending[row][col] = false;
    markDirty(col, row);
  }
  capture.active = false;
}

void orderFromSeed(const int* cellsC, const int* cellsR, int n, int seedCol, int seedRow,
                   int8_t* outC, int8_t* outR) {
  static uint8_t keys[GRID_H][GRID_W];
  static uint8_t seen[GRID_H][GRID_W];
  static int qC[CELL_N];
  static int qR[CELL_N];
  memset(keys, 0, sizeof(keys));
  memset(seen, 0, sizeof(seen));
  for (int i = 0; i < n; i++) keys[cellsR[i]][cellsC[i]] = 1;

  int qn = 0;
  for (int k = 0; k < 4; k++) {
    int c = seedCol + ORTHO[k][0];
    int r = seedRow + ORTHO[k][1];
    if (c < 0 || r < 0 || c >= GRID_W || r >= GRID_H) continue;
    if (!keys[r][c] || seen[r][c]) continue;
    seen[r][c] = 1;
    qC[qn] = c;
    qR[qn] = r;
    qn += 1;
  }
  if (qn == 0 && n > 0) {
    int bestI = 0;
    int bestD = 1 << 30;
    for (int i = 0; i < n; i++) {
      int dc = cellsC[i] - seedCol;
      int dr = cellsR[i] - seedRow;
      int d = dc * dc + dr * dr;
      if (d < bestD) {
        bestD = d;
        bestI = i;
      }
    }
    int c = cellsC[bestI];
    int r = cellsR[bestI];
    seen[r][c] = 1;
    qC[qn] = c;
    qR[qn] = r;
    qn += 1;
  }

  int ordered = 0;
  int head = 0;
  while (head < qn) {
    int c = qC[head];
    int r = qR[head];
    head += 1;
    outC[ordered] = (int8_t)c;
    outR[ordered] = (int8_t)r;
    ordered += 1;
    for (int k = 0; k < 4; k++) {
      int nc = c + ORTHO[k][0];
      int nr = r + ORTHO[k][1];
      if (nc < 0 || nr < 0 || nc >= GRID_W || nr >= GRID_H) continue;
      if (!keys[nr][nc] || seen[nr][nc]) continue;
      seen[nr][nc] = 1;
      qC[qn] = nc;
      qR[qn] = nr;
      qn += 1;
    }
  }
  for (int i = 0; i < n; i++) {
    int c = cellsC[i];
    int r = cellsR[i];
    if (seen[r][c]) continue;
    seen[r][c] = 1;
    outC[ordered] = (int8_t)c;
    outR[ordered] = (int8_t)r;
    ordered += 1;
  }
}

void startCaptureShow(const int* cellsC, const int* cellsR, int n, int capturer,
                      int victim, int seedCol, int seedRow) {
  if (n <= 0) return;
  dismissCapture();
  capture.active = true;
  capture.outlineOnly = true;
  capture.capturer = (int8_t)capturer;
  capture.victim = (int8_t)victim;
  capture.filled = 0;
  capture.count = n;
  orderFromSeed(cellsC, cellsR, n, seedCol, seedRow, capture.cols, capture.rows);
  for (int i = 0; i < n; i++) {
    pending[capture.rows[i]][capture.cols[i]] = true;
    markDirty(capture.cols[i], capture.rows[i]);
  }
}

void stepCaptureShow() {
  if (!capture.active) return;
  if (capture.outlineOnly) {
    capture.outlineOnly = false;
    return;
  }
  if (capture.filled >= capture.count) {
    capture.active = false;
    return;
  }
  int col = capture.cols[capture.filled];
  int row = capture.rows[capture.filled];
  pending[row][col] = false;
  playCaptureTick(capture.filled, capture.count);
  capture.filled += 1;
  markDirty(col, row);
  if (capture.filled >= capture.count) capture.active = false;
}

void spawnSplitFrom(const Ball& source) {
  if (ballN + newN + 2 > MAX_BALLS) return;
  float heading = atan2f(source.vy, source.vx);
  float speed = currentSpeed(source);
  const float offsets[2] = {-0.55f, 0.55f};
  for (int i = 0; i < 2; i++) {
    Ball& born = newcomers[newN++];
    born.x = source.x;
    born.y = source.y;
    born.vx = speed * cosf(heading + offsets[i]);
    born.vy = speed * sinf(heading + offsets[i]);
    born.radius = source.radius;
    born.home = source.home;
    born.extra = true;
  }
}

void applyAward(Ball& ball, uint8_t kind) {
  Buff& buff = buffs[ball.home];
  bool dual = buff.speedLeft > 0 || buff.splitLeft > 0;
  int duration = AWARD_FRAMES;
  if (kind == AWARD_SPEED) {
    buff.speedMult *= 2.f;
    buff.speedLeft = duration;
    if (dual && buff.splitLeft > 0) buff.splitLeft = duration;
    for (int i = 0; i < ballN; i++) {
      if (balls[i].home == ball.home) setSpeed(balls[i]);
    }
    for (int i = 0; i < newN; i++) {
      if (newcomers[i].home == ball.home) setSpeed(newcomers[i]);
    }
    return;
  }
  buff.splitLeft = duration;
  if (dual && buff.speedLeft > 0) buff.speedLeft = duration;
  int ballsNow = ballN;
  int newsNow = newN;
  for (int i = 0; i < ballsNow; i++) {
    if (balls[i].home == ball.home) spawnSplitFrom(balls[i]);
  }
  for (int i = 0; i < newsNow; i++) {
    if (newcomers[i].home == ball.home) spawnSplitFrom(newcomers[i]);
  }
}

void reflectVelocity(float& vx, float& vy, float nx, float ny) {
  float incoming = vx * nx + vy * ny;
  if (incoming >= 0.f) return;
  vx -= 2.f * incoming * nx;
  vy -= 2.f * incoming * ny;
}

void clampInside(Ball& ball) {
  float r = ball.radius;
  float fieldW = (float)FIELD_PX_W;
  float fieldH = (float)FIELD_PX_H;
  ball.x = clampf(ball.x, r, fieldW - r);
  ball.y = clampf(ball.y, r, fieldH - r);
}

void updateBall(Ball& ball) {
  static int seizedC[CELL_N];
  static int seizedR[CELL_N];
  float remaining = 1.f;
  for (int bounce = 0; bounce < MAX_BOUNCES && remaining > TOI_EPS; bounce++) {
    Hit hit = findEarliestHit(ball, remaining);
    if (!hit.ok) {
      ball.x += ball.vx * remaining;
      ball.y += ball.vy * remaining;
      remaining = 0.f;
      break;
    }
    ball.x += ball.vx * hit.t;
    ball.y += ball.vy * hit.t;
    remaining -= hit.t;

    if (hit.kind == KIND_AWARD) {
      const Award* found = awardAt(hit.col, hit.row);
      Award award{};
      bool have = found != nullptr;
      if (have) award = *found;
      owner[hit.row][hit.col] = ball.home;
      markDirty(hit.col, hit.row);
      playAward();
      if (have) {
        applyAward(ball, award.kind);
        removeAwardAt(award.col, award.row);
      }
    } else if (hit.kind == KIND_CELL) {
      int victim = owner[hit.row][hit.col];
      owner[hit.row][hit.col] = ball.home;
      markDirty(hit.col, hit.row);
      playCellBreak();
      int seizedN = collectEnclaves(victim, seizedC, seizedR);
      if (seizedN > 0) {
        for (int i = 0; i < seizedN; i++) {
          owner[seizedR[i]][seizedC[i]] = ball.home;
          markDirty(seizedC[i], seizedR[i]);
        }
        startCaptureShow(seizedC, seizedR, seizedN, ball.home, victim, hit.col, hit.row);
      }
    } else {
      playWallHit();
    }

    reflectVelocity(ball.vx, ball.vy, hit.nx, hit.ny);
    setSpeed(ball);
    ball.x += hit.nx * SEP;
    ball.y += hit.ny * SEP;
  }
  if (remaining > TOI_EPS) {
    ball.x += ball.vx * remaining;
    ball.y += ball.vy * remaining;
  }
  clampInside(ball);
}

void removeExtras(int faction) {
  int w = 0;
  for (int i = 0; i < ballN; i++) {
    if (balls[i].home == faction && balls[i].extra) continue;
    balls[w++] = balls[i];
  }
  ballN = w;
}

void stepAwards() {
  for (int faction = 0; faction < FACTION_N; faction++) {
    Buff& buff = buffs[faction];
    if (buff.speedLeft > 0) {
      buff.speedLeft -= 1;
      if (buff.speedLeft == 0) {
        buff.speedMult = 1.f;
        for (int i = 0; i < ballN; i++) {
          if (balls[i].home == faction) setSpeed(balls[i]);
        }
      }
    }
    if (buff.splitLeft > 0) {
      buff.splitLeft -= 1;
      if (buff.splitLeft == 0) removeExtras(faction);
    }
  }
  refillAwards();
}

void stepSim() {
  newN = 0;
  int n = ballN;
  for (int i = 0; i < n; i++) updateBall(balls[i]);
  for (int i = 0; i < newN && ballN < MAX_BALLS; i++) balls[ballN++] = newcomers[i];
  stepAwards();
  stepCaptureShow();
}

void drawAwardMark(int x, int y, const Award& award) {
  const Faction& faction = factions[award.faction];
  M5.Display.fillRect(x, y, CELL, CELL, faction.territory);
  M5.Display.drawRect(x + 1, y + 1, CELL - 2, CELL - 2, COL_RIM);
  uint16_t mark = faction.ballOutline;
  int cx = x + CELL / 2;
  int cy = y + CELL / 2;
  if (award.kind == AWARD_SPLIT) {
    M5.Display.fillRect(cx - 1, cy - 4, 2, 2, mark);
    M5.Display.fillRect(cx - 4, cy + 2, 2, 2, mark);
    M5.Display.fillRect(cx + 2, cy + 2, 2, 2, mark);
  } else {
    M5.Display.fillTriangle(cx - 4, cy - 3, cx + 1, cy, cx - 4, cy + 3, mark);
    M5.Display.fillTriangle(cx - 1, cy - 3, cx + 4, cy, cx - 1, cy + 3, mark);
  }
}

void drawCell(int col, int row) {
  int x = FIELD_X + col * CELL;
  int y = FIELD_Y + row * CELL;
  const Award* award = awardAt(col, row);
  if (award) {
    drawAwardMark(x, y, *award);
    return;
  }
  int8_t id = owner[row][col];
  if (id == HARD_ID) {
    M5.Display.fillRect(x, y, CELL, CELL, COL_HARD);
    M5.Display.fillRect(x + 2, y + 2, CELL - 4, CELL - 4, COL_HARD_IN);
    return;
  }
  bool pend = capture.active && pending[row][col];
  int8_t fillId = pend ? capture.victim : id;
  uint16_t color = COL_VOID;
  if (fillId >= 0 && fillId < FACTION_N) color = factions[fillId].territory;
  M5.Display.fillRect(x, y, CELL, CELL, color);
  if (pend && capture.capturer >= 0 && capture.capturer < FACTION_N) {
    M5.Display.drawRect(x, y, CELL, CELL, factions[capture.capturer].territory);
  }
}

void flushDirty() {
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      if (!dirty[row][col]) continue;
      drawCell(col, row);
      dirty[row][col] = false;
    }
  }
}

void drawBall(const Ball& ball) {
  int cx = FIELD_X + (int)lroundf(ball.x);
  int cy = FIELD_Y + (int)lroundf(ball.y);
  int rad = (int)lroundf(ball.radius);
  if (rad < 1) rad = 1;
  const Faction& faction = factions[ball.home];
  M5.Display.fillCircle(cx, cy, rad, faction.ballFill);
  M5.Display.drawCircle(cx, cy, rad, faction.ballOutline);
}

void countTerritory(int out[FACTION_N]) {
  for (int i = 0; i < FACTION_N; i++) out[i] = 0;
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      int8_t id = owner[row][col];
      if (id >= 0 && id < FACTION_N) out[id] += 1;
    }
  }
}

float awardRemainSeconds(int faction) {
  const Buff& buff = buffs[faction];
  int left = buff.speedLeft > buff.splitLeft ? buff.speedLeft : buff.splitLeft;
  if (left <= 0) return 0.f;
  return (float)left / 16.f;
}

void drawHud() {
  constexpr int tx = HUD_X + 6;
  M5.Display.setTextColor(COL_TEXT, COL_BG);
  M5.Display.setCursor(tx, 8);
  M5.Display.print("inktide");

  int tallies[FACTION_N];
  countTerritory(tallies);
  for (int i = 0; i < FACTION_N; i++) {
    int y = 28 + i * 22;
    M5.Display.fillRect(tx, y, 10, 10, factions[i].territory);
    float pct = (100.f * tallies[i]) / (float)CELL_N;
    char line[20];
    snprintf(line, sizeof(line), "%s %5.1f%%", factions[i].label, pct);
    M5.Display.setCursor(tx + 14, y + 1);
    M5.Display.print(line);
  }

  constexpr int barX = tx;
  constexpr int barY = 124;
  constexpr int barW = SCREEN_W - barX - 8;
  constexpr int barH = 12;
  M5.Display.fillRect(barX, barY, barW, barH, COL_BG);
  int splitX = barX;
  bool blinkOn = ((millis() / 200) % 2) == 0;
  for (int i = 0; i < FACTION_N; i++) {
    int width = (int)lroundf((barW * (float)tallies[i]) / (float)CELL_N);
    if (splitX + width > barX + barW) width = barX + barW - splitX;
    if (width < 0) width = 0;
    M5.Display.fillRect(splitX, barY, width, barH, factions[i].territory);
    float remain = awardRemainSeconds(i);
    if (remain > 0.f && width > 0) {
      int blinkW = (int)lroundf((barW * remain) / 100.f);
      if (blinkW > width) blinkW = width;
      uint16_t blink = blinkOn ? factions[i].barBright : factions[i].barLit;
      M5.Display.fillRect(splitX, barY, blinkW, barH, blink);
    }
    splitX += width;
  }
  M5.Display.drawRect(barX, barY, barW, barH, COL_TEXT);

  M5.Display.setTextColor(COL_DIM, COL_BG);
  M5.Display.setCursor(tx, 160);
  M5.Display.print("A start");
  M5.Display.setCursor(tx, 174);
  M5.Display.print(VOLUME_LABELS[volumeStep]);
  M5.Display.setCursor(tx, 188);
  M5.Display.print(muted ? "C muted" : "C mute ");
}

void render() {
  for (int i = 0; i < drawnN; i++) markBall(drawn[i].x, drawn[i].y, drawn[i].radius);
  for (int i = 0; i < ballN; i++) markBall(balls[i].x, balls[i].y, balls[i].radius);
  M5.Display.startWrite();
  M5.Display.setClipRect(FIELD_X, FIELD_Y, FIELD_PX_W, FIELD_PX_H);
  flushDirty();
  for (int i = 0; i < ballN; i++) drawBall(balls[i]);
  M5.Display.clearClipRect();
  drawHud();
  M5.Display.endWrite();
  drawnN = ballN;
  if (ballN > 0) memcpy(drawn, balls, sizeof(Ball) * (size_t)ballN);
}

void resetGame() {
  if (speakerOk) M5.Speaker.stop();
  toneN = 0;
  burst.at = 0;
  burst.wall = 0;
  burst.cell = 0;
  accrued = 0.f;
  lastMs = millis();
  clearBuffs();
  capture.active = false;
  memset(pending, 0, sizeof(pending));
  createField();
  memset(dirty, 0, sizeof(dirty));
  spawnBalls();
  M5.Display.startWrite();
  M5.Display.fillScreen(COL_BG);
  M5.Display.setClipRect(FIELD_X, FIELD_Y, FIELD_PX_W, FIELD_PX_H);
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) drawCell(col, row);
  }
  for (int i = 0; i < ballN; i++) drawBall(balls[i]);
  M5.Display.clearClipRect();
  drawHud();
  M5.Display.endWrite();
  drawnN = ballN;
  memcpy(drawn, balls, sizeof(Ball) * (size_t)ballN);
}

void applyVolume() {
  if (!speakerOk) return;
  M5.Speaker.setVolume(muted ? 0 : VOLUME_STEPS[volumeStep]);
}

void startSpeaker() {
  if (speakerStarted) return;
  auto spk = M5.Speaker.config();
  spk.i2s_port = I2S_NUM_0;
  spk.use_dac = true;
  spk.pin_data_out = GPIO_NUM_25;
  spk.magnification = 8;
  spk.sample_rate = 96000;
  pinMode(GPIO_NUM_25, OUTPUT);
  digitalWrite(GPIO_NUM_25, LOW);
  M5.Speaker.config(spk);
  speakerOk = M5.Speaker.begin();
  speakerStarted = speakerOk;
  if (!speakerOk) return;
  M5.Speaker.setAllChannelVolume(255);
  applyVolume();
}

void handleButtons() {
  if (millis() - bootAt < 400) return;
  if (M5.BtnA.wasPressed()) resetGame();
  if (M5.BtnB.wasPressed()) {
    volumeStep = (volumeStep + 1) % VOLUME_STEP_N;
    if (!muted) applyVolume();
    M5.Display.startWrite();
    drawHud();
    M5.Display.endWrite();
  }
  if (M5.BtnC.wasPressed()) {
    muted = !muted;
    toneN = 0;
    if (muted) {
      if (speakerOk) {
        M5.Speaker.stop();
        M5.Speaker.setVolume(0);
      }
    } else {
      startSpeaker();
      applyVolume();
    }
    M5.Display.startWrite();
    drawHud();
    M5.Display.endWrite();
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() + 1);
  }
  M5.Display.setColorDepth(16);
  M5.Display.setBrightness(180);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextWrap(false);

  randomSeed((uint32_t)micros() ^ esp_random());
  initFactions();
  bootAt = millis();
  Serial.println("inktide");
  resetGame();
}

void loop() {
  M5.update();
  handleButtons();
  uint32_t now = millis();
  uint32_t elapsed = now - lastMs;
  lastMs = now;
  accrued += (float)elapsed;
  if (accrued > FRAME_MS * 4.f) accrued = FRAME_MS;
  int steps = 0;
  while (accrued >= FRAME_MS && steps < 4) {
    stepSim();
    accrued -= FRAME_MS;
    steps += 1;
  }
  if (steps > 0) render();
  drainTones(now);
  delay(1);
}
