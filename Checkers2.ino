#include <Arduino.h>        // Подключение базовой библиотеки Arduino (пины, delay, digitalRead и т.д.)
#include <LovyanGFX.hpp>    // Библиотека для работы с дисплеем (быстрая графика, оптимизированная под ESP32)

// === Тип и константы должны идти до функций ===
// Важно: enum объявляется до использования в функциях

enum Piece { 
    EMPTY=0,   // Пустая клетка
    WHITE=1,   // Белая обычная шашка
    BLACK=2,   // Черная обычная шашка
    WHITE_K=3, // Белая дамка (King)
    BLACK_K=4  // Черная дамка
};

// Определение цветов в формате RGB565 (16-битный цвет)

#define COL_WHITE      0xFFFF  // Белый цвет (максимум по всем каналам)
#define COL_GREEN      0x03E0  // Зеленый
#define COL_LIGHTGREEN 0x9FD3  // Светло-зеленый (для подсветки)
#define COL_RED        0xF800  // Красный
#define COL_BLUE       0x001F  // Синий
#define COL_YELLOW     0xFFE0  // Желтый
#define COL_BLACK      0x0000  // Черный (нулевой цвет)

// ПИНЫ ДЖОЙСТИКА
#define J_X_PIN  32  // Аналоговый вход X (горизонталь)
#define J_Y_PIN  33  // Аналоговый вход Y (вертикаль)
#define J_SW_PIN 25  // Кнопка (нажатие стика)

// Максимальная глубина поиска для ИИ (чем больше — тем умнее, но медленнее)
#define AI_MAX_DEPTH 6

// Структура одного хода
struct Move { 
    int8_t fx, fy;  // from x, y — координаты откуда (начало хода)
    int8_t tx, ty;  // to x, y — координаты куда (конец хода)
    bool capture;   // флаг: был ли захват (съели фигуру)
};

bool gameOver = false;        // Флаг того, что игра завершена
String gameOverText = "";     // Текст для отображения (WIN или GAME OVER)
uint16_t gameOverCol = 0;    // Цвет фона для баннера (зеленый или красный)

// Массив для хранения возможных ходов
Move moves[64];   // максимум 64 хода (с запасом)
int mCnt = 0;     // текущее количество найденных ходов

// Флаг необходимости перерисовки экрана
bool needRedraw = true;

// Яркие цвета для курсора и выделения
#define COL_CURSOR_BRIGHT 0xF7BE  // цвет курсора
#define COL_SELECT_BRIGHT 0xFFE0  // цвет выбранной клетки

// Структура "битовой доски"
// Это оптимизированное представление: вместо массива используется битовая маска
// Вся доска укладывается в несколько чисел (очень быстро для ИИ)
struct Bitboard {
    uint32_t w;  // Белые фигуры (каждый бит = одна клетка)
    uint32_t b;  // Черные фигуры
    uint32_t k;  // Дамки (Kings) — отдельная маска (пересекается с w/b)
};
// Важно: обычная фигура определяется как (есть в w или b, но нет в k)
// дамка = есть в k + принадлежит w или b


class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
public:
  LGFX(void) {
    auto cfg = _bus_instance.config();
    cfg.freq_write = 27000000;
    cfg.freq_read = 16000000;
    cfg.spi_mode = 0;
    cfg.pin_sclk = 18;
    cfg.pin_mosi = 23;
    cfg.pin_miso = -1;
    cfg.pin_dc = 27;
    cfg.spi_host = VSPI_HOST;
    cfg.dma_channel = 1;
    _bus_instance.config(cfg);
    _panel_instance.setBus(&_bus_instance);

    auto pcfg = _panel_instance.config();
    pcfg.pin_cs = 5;
    pcfg.pin_rst = 14;
    pcfg.panel_width = 320;
    pcfg.panel_height = 320;
    pcfg.offset_x = 0;
    pcfg.offset_y = 0;
    pcfg.offset_rotation = 0;
    pcfg.dummy_read_pixel = 8;
    pcfg.dummy_read_bits = 1;
    pcfg.readable = false;
    pcfg.invert = false;
    pcfg.rgb_order = true;
    pcfg.dlen_16bit = false;
    pcfg.bus_shared = true;
    _panel_instance.config(pcfg);

    auto lcfg = _light_instance.config();
    lcfg.pin_bl = 21;
    lcfg.invert = false;
    lcfg.freq = 1200;
    lcfg.pwm_channel = 7;
    _light_instance.config(lcfg);
    _panel_instance.setLight(&_light_instance);

    setPanel(&_panel_instance);
  }
};

LGFX tft;

enum GamePhase { PHASE_PLAYER, PHASE_AI };
GamePhase phase = PHASE_PLAYER;

const int TILE = 30;
int board[8][8];     
int curX = 0, curY = 0;
int selX = -1, selY = -1; 
bool highlightMoves[8][8];

void clearHighlights() {
  for(int y=0;y<8;y++) for(int x=0;x<8;x++) highlightMoves[y][x] = false;
}


Piece myBoard[8][8];

volatile bool isWhiteTurn = true;     
bool aiMoveReady = false;     
struct AIMove { int8_t fx, fy, tx, ty; } bestAIMove;
bool chainActive = false;     
int8_t chainX = -1, chainY = -1;

int curAIdepth = 0;
bool aiAbort = false;
uint32_t aiNodes = 0;


// Переменные для управления анимацией
bool isAnimating = false; // Флаг: идет ли сейчас процесс анимации хода
float animX, animY;       // Текущие экранные координаты (в пикселях) для анимации
Piece animPiece;          // Тип фигуры, которую мы сейчас «двигаем»


// Функция преобразования координат (x, y) → индекс бита (0..31)
// Используются только темные клетки (как в реальных шашках)
int8_t getIdx(int8_t x, int8_t y) {
  // Проверка: если клетка светлая — возвращаем -1 (не используется)
  if ((x + y) % 2 == 0) return -1; // Белые клетки не используем
  // Формула перевода координат в индекс:
  // y*4 — каждая строка даёт 4 игровых клетки
  // (x>>1) — делим x на 2 (только темные клетки учитываются)
  return (y * 4) + (x / 2);
}

// Конвертер: индекс бита (0-31) -> координаты экрана
void getPos(int8_t idx, int8_t &x, int8_t &y) {
    y = idx / 4;
    x = (idx % 4) * 2 + (1 - (y % 2));
}

int evaluateBitboard(Bitboard b) {
    // 1. ВЕСА ФИГУР: Увеличиваем значимость дамок.
    // Обычная шашка = 100, Дамка = 400. ИИ будет активнее стремиться к превращению и беречь дамки.
    int whiteMaterial = __builtin_popcount(b.w & ~b.k) * 100 + __builtin_popcount(b.w & b.k) * 400;
    int blackMaterial = __builtin_popcount(b.b & ~b.k) * 100 + __builtin_popcount(b.b & b.k) * 400;
    
    // Начальный счет (разница материала). Черные (ИИ) — положительный, Белые (игрок) — отрицательный.
    int score = blackMaterial - whiteMaterial; 

    // Маски для анализа структуры доски
    uint32_t edgeMask = 0x81818181;    // Клетки у самых краев доски (безопасные зоны)
    uint32_t centerMask = 0x00666600;  // Центральный квадрат 4х4 (контроль поля)

    // 2. ЦИКЛ ПО ВСЕМ КЛЕТКАМ: Анализируем положение каждой фигуры
    for (int i = 0; i < 32; i++) {
        uint32_t bit = (1UL << i); // Битовая маска текущей клетки
        int8_t x, y;
        getPos(i, x, y); // Перевод индекса бита в координаты (x, y)

        // --- АНАЛИЗ ЧЕРНЫХ ШАШЕК (ИИ) ---
        if (b.b & bit) { // Для черных (ИИ)
            if (!(b.k & bit)) score += (y * 8); // Стимул идти в дамки
            if (x == 0 || x == 7 || y == 0 || y == 7) score += 12; // Борта — это безопасно
            if (y == 0) score += 30; // Держать "золотой ряд" (защита базы)
        } else if (b.w & bit) { // Для белых (игрок)
            if (!(b.k & bit)) score -= ((7 - y) * 8); // Опасаться прохода игрока
            if (x == 0 || x == 7 || y == 0 || y == 7) score -= 12;
            if (y == 7) score -= 30;
        }

    }

    // 3. АНАЛИЗ УГРОЗ (Почему ИИ подставляется):
    // Генерируем ходы игрока, чтобы увидеть, может ли он что-то съесть после хода ИИ
    Move playerMoves[32];
    int pCount = generateMovesFromBitboard(b, playerMoves, true); // true = ходы белых
    
    for (int i = 0; i < pCount; i++) {
        if (playerMoves[i].capture) {
            // КРИТИЧЕСКИЙ ШТРАФ (-150): Если ход ведет к тому, что игрок может срубить фигуру ИИ.
            // Это заставит алгоритм Minimax выбирать другие, более безопасные ветки.
            score -= 150; 
            
            // Если игрок может срубить дамку — штрафуем ИИ еще сильнее (-300)
            int targetIdx = getIdx((playerMoves[i].fx + playerMoves[i].tx)/2, (playerMoves[i].fy + playerMoves[i].ty)/2);
            if (targetIdx != -1 && (b.k & (1UL << targetIdx))) score -= 300;
        }
    }
    
    return score; // Возвращаем комплексную оценку позиции
}


int evaluate() {
  int s = 0;
  for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++){
    Piece p = myBoard[y][x]; 
    if(p == EMPTY) continue;
    int v = 0;
    if(p == BLACK) v = 100 + (7-y) * 5;
    else if(p == BLACK_K) v = 180;
    else if(p == WHITE) v = -(100 + y * 5);
    else if(p == WHITE_K) v = -180;
    if(x >= 2 && x <= 5 && y >= 2 && y <= 5) v += (v > 0 ? 15 : -15);
    s += v;
  }
  return s;
}

bool isDark(int x, int y) {
  return (x + y) % 2 == 1; 
}

bool isOwn(Piece p, bool white) {
  return (white && (p==WHITE||p==WHITE_K)) || (!white && (p==BLACK||p==BLACK_K)); 
}

bool isEnemy(Piece p, bool white) {
  return (white && (p==BLACK||p==BLACK_K)) || (!white && (p==WHITE||p==WHITE_K)); 
}

bool canCapturePieceBoard(Piece board[8][8], int x, int y, bool whiteTurn) {
  Piece p = board[y][x];
  if(p == EMPTY) return false;

  int dirs[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}};

  for(int d = 0; d < 4; d++) {
    int mx = x + dirs[d][0];
    int my = y + dirs[d][1];
    int tx = x + dirs[d][0]*2;
    int ty = y + dirs[d][1]*2;

    if(tx < 0 || tx >= 8 || ty < 0 || ty >= 8) continue;

    if(isEnemy(board[my][mx], whiteTurn) && board[ty][tx] == EMPTY) {
      return true;
    }
  }
  return false;
}

bool canCapturePiece(int x, int y, int &mx, int &my) {
  Piece p = myBoard[y][x]; // Получаем тип фигуры в текущей клетке
  if(p == EMPTY) return false; // Если клетка пуста, бить нечем
  bool isKing = (p == WHITE_K || p == BLACK_K); // Проверяем, является ли фигура дамкой
  int dirs[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}}; // Массив направлений (все 4 диагонали)
  
  for(int d = 0; d < 4; d++) { // Цикл по всем направлениям
    if (isKing) { // ЛОГИКА ДЛЯ ДАЛЬНОБОЙНОЙ ДАМКИ
      int foundEnemyX = -1, foundEnemyY = -1; // Координаты потенциально сбиваемой фигуры
      for (int dist = 1; dist < 8; dist++) { // Просматриваем луч в направлении d
        int tx = x + dirs[d][0] * dist; // Координата X текущей точки луча
        int ty = y + dirs[d][1] * dist; // Координата Y текущей точки луча
        if (tx < 0 || tx >= 8 || ty < 0 || ty >= 8) break; // Выход за границы доски — стоп луч
        
        Piece target = myBoard[ty][tx]; // Смотрим, что находится в клетке луча
        if (target == EMPTY) { // Если клетка пустая
          if (foundEnemyX != -1) { // Если мы уже пролетели через врага
            mx = foundEnemyX; my = foundEnemyY; // Записываем координаты жертвы
            return true; // Нашли возможность боя: "своя дамка -> пусто -> враг -> пусто"
          }
        } else { // Если клетка не пуста
          if (isEnemy(target, isWhiteTurn)) { // Если это враг
            if (foundEnemyX != -1) break; // Если это второй враг на пути — бить нельзя, стоп луч
            foundEnemyX = tx; foundEnemyY = ty; // Запоминаем первого встречного врага
          } else break; // Если это своя фигура — путь заблокирован, стоп луч
        }
      }
    } else { // СТАНДАРТНАЯ ЛОГИКА ДЛЯ ОБЫЧНОЙ ШАШКИ
      int tx = x + dirs[d][0]*2, ty = y + dirs[d][1]*2; // Клетка приземления (через одну)
      int mmx = x + dirs[d][0], mmy = y + dirs[d][1]; // Клетка с потенциальной жертвой (соседняя)
      if(tx >= 0 && tx < 8 && ty >= 0 && ty < 8) { // Проверка границ доски
        if(isEnemy(myBoard[mmy][mmx], isWhiteTurn) && myBoard[ty][tx] == EMPTY) { // Если сосед — враг, а за ним пусто
          mx = mmx; my = mmy; // Записываем координаты жертвы
          return true; // Можно бить
        }
      }
    }
  }
  return false; // Ни в одном направлении бить нельзя
}

bool checkMandatoryCaptures() {
  int mx, my;
  for(int y = 0; y < 8; y++) for(int x = 0; x < 8; x++)
    if(isOwn(myBoard[y][x], isWhiteTurn) && canCapturePiece(x,y,mx,my)) return true;
  return false;
}

int generateMovesForBoard(Piece board[8][8], Move* moveList, bool whiteTurn, bool chain, int8_t cx, int8_t cy) {
  int count = 0; // Счетчик найденных ходов
  int dirs[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}}; // Векторы направлений диагоналей
  bool mustCap = false; // Флаг обязательного взятия

  // ПЕРВЫЙ ПРОХОД: Поиск обязательных взятий
  for(int y=0; y<8; y++) {
    for(int x=0; x<8; x++) {
      if(!isOwn(board[y][x], whiteTurn)) continue; // Пропускаем чужие фигуры
      if(chain && (x != cx || y != cy)) continue; // В серии прыжков ходит только одна фигура
      
      Piece p = board[y][x]; // Текущая фигура
      bool isKing = (p == WHITE_K || p == BLACK_K); // Флаг дамки

      for(int d=0; d<4; d++) { // Проверяем 4 диагонали
        if (isKing) { // Бой для дальнобойной дамки
          int ex = -1, ey = -1; // Координаты врага
          for (int dist = 1; dist < 8; dist++) { // Скользим по диагонали
            int mx = x + dirs[d][0] * dist, my = y + dirs[d][1] * dist; // Координаты проверки
            if (mx < 0 || mx >= 8 || my < 0 || my >= 8) break; // Граница доски
            if (board[my][mx] == EMPTY) { // Если клетка пуста
              if (ex != -1) { // И мы уже перепрыгнули врага
                moveList[count++] = {(int8_t)x, (int8_t)y, (int8_t)mx, (int8_t)my, true}; // Добавляем ход (приземлиться можно в любую пустую за врагом)
                mustCap = true; // Устанавливаем статус обязательного боя
              }
            } else { // Если встретили фигуру
              if (isEnemy(board[my][mx], whiteTurn)) { // Если это враг
                if (ex != -1) break; // Второй враг подряд — путь закрыт
                ex = mx; ey = my; // Запоминаем первого врага
              } else break; // Своя фигура — путь закрыт
            }
          }
        } else { // Бой для обычной шашки (на 2 клетки)
          int mx = x + dirs[d][0], my = y + dirs[d][1]; // Сосед
          int tx = x + dirs[d][0]*2, ty = y + dirs[d][1]*2; // За соседом
          if(tx>=0 && tx<8 && ty>=0 && ty<8 && board[ty][tx] == EMPTY && isEnemy(board[my][mx], whiteTurn)) {
            moveList[count++] = {(int8_t)x, (int8_t)y, (int8_t)tx, (int8_t)ty, true};
            mustCap = true;
          }
        }
      }
    }
  }

  if(mustCap) return count; // Если есть взятия, обычные ходы по правилам не рассматриваем

  // ВТОРОЙ ПРОХОД: Обычные ходы (только если нет обязательных взятий)
  if(chain) return 0; // В серии прыжков обычные ходы запрещены

  for(int y=0; y<8; y++) {
    for(int x=0; x<8; x++) {
      if(!isOwn(board[y][x], whiteTurn)) continue;
      Piece p = board[y][x];
      bool isKing = (p == WHITE_K || p == BLACK_K);

      for(int d=0; d<4; d++) {
        if (isKing) { // Ход дальнобойной дамки
          for (int dist = 1; dist < 8; dist++) { // Скользим по диагонали пока свободно
            int tx = x + dirs[d][0] * dist, ty = y + dirs[d][1] * dist;
            if (tx >= 0 && tx < 8 && ty >= 0 && ty < 8 && board[ty][tx] == EMPTY) {
              moveList[count++] = {(int8_t)x, (int8_t)y, (int8_t)tx, (int8_t)ty, false};
            } else break; // Встретили препятствие — это направление закончено
          }
        } else { // Ход обычной шашки (1 клетка, только вперед)
          if(p == WHITE && dirs[d][1] > 0) continue; // Белые вверх
          if(p == BLACK && dirs[d][1] < 0) continue; // Черные вниз
          int tx = x + dirs[d][0], ty = y + dirs[d][1];
          if(tx>=0 && tx<8 && ty>=0 && ty<8 && board[ty][tx] == EMPTY) {
            moveList[count++] = {(int8_t)x, (int8_t)y, (int8_t)tx, (int8_t)ty, false};
          }
        }
      }
    }
  }
  return count; // Возвращаем общее число найденных ходов
}

// Функция проверяет, может ли конкретная шашка ходить в текущей ситуации
bool canPieceActuallyMove(int x, int y) {
    Move tempMoves[32]; // Временный массив для проверки
    // Генерируем ходы для всей доски с учетом правил (обязательный бой и серия прыжков)
    int count = generateMovesForBoard(myBoard, tempMoves, isWhiteTurn, chainActive, chainX, chainY);
    for (int i = 0; i < count; i++) {
        if (tempMoves[i].fx == x && tempMoves[i].fy == y) return true; // Если у этой шашки есть ход
    }
    return false;
}

// Поиск ближайшей валидной клетки в заданном направлении (dx, dy)
void moveToNearestActive(int dx, int dy) {
    float bestScore = -1e9; // Чем выше оценка, тем точнее попадание в направление
    int targetX = -1, targetY = -1;

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            if (x == curX && y == curY) continue; // Пропускаем текущую клетку

            bool isValidTarget = false;
            if (selX == -1) {
                // Если ничего не выбрано — целью может быть только шашка, имеющая ходы
                if (isOwn(myBoard[y][x], isWhiteTurn) && canPieceActuallyMove(x, y)) isValidTarget = true;
            } else {
                // Если шашка выбрана — целью может быть сама шашка (отмена) или подсвеченный ход
                if ((x == selX && y == selY) || highlightMoves[y][x]) isValidTarget = true;
            }

            if (isValidTarget) {
                // Вектор от курсора к проверяемой клетке
                float vx = x - curX;
                float vy = y - curY;
                float dist = sqrt(vx * vx + vy * vy);
                
                // Нормализуем вектор (скалярное произведение)
                // Оцениваем, насколько направление на клетку совпадает с наклоном джойстика
                float dotProduct = (vx * dx + vy * dy) / dist;

                // Формула оценки: приоритет направлению, штраф за расстояние
                float score = dotProduct * 10.0 - dist; 

                if (dotProduct > 0.5 && score > bestScore) { // 0.5 — это угол около 60 градусов
                    bestScore = score;
                    targetX = x;
                    targetY = y;
                }
            }
        }
    }

    if (targetX != -1) {
        curX = targetX;
        curY = targetY;
        needRedraw = true;
    }
}

void applyMove(int8_t fx, int8_t fy, int8_t tx, int8_t ty) {
  Piece p = myBoard[fy][fx]; // Запоминаем, какая фигура ходит
  
  // === БЛОК АНИМАЦИИ (Начало) ===
  isAnimating = true;       // Включаем флаг анимации для drawGame
  animPiece = p;            // Запоминаем фигуру для отрисовки в полете
  myBoard[fy][fx] = EMPTY;  // Временно удаляем фигуру с доски, чтобы она не "двоилась"
  
  int steps = 10;           // Количество кадров анимации (чем больше, тем медленнее)
  for (int i = 0; i <= steps; i++) {
    // Вычисляем промежуточные координаты от старта (fx, fy) до финиша (tx, ty) в пикселях
    animX = (fx * TILE) + ( (tx - fx) * TILE * i / steps ); 
    animY = (fy * TILE) + ( (ty - fy) * TILE * i / steps );
    
    drawGame();             // Вызываем перерисовку экрана (она отрисует animPiece в точках animX, animY)
    delay(10);           // Можно раскомментировать, если анимация слишком быстрая
  }
  isAnimating = false;      // Выключаем режим анимации
  // === БЛОК АНИМАЦИИ (Конец) ===

  bool wasCapture = false; // Флаг: был ли совершен захват

  // Ищем, была ли сбита фигура на пути (для дальнобойных прыжков)
  int dx = (tx > fx) ? 1 : -1; // Направление по X
  int dy = (ty > fy) ? 1 : -1; // Направление по Y
  int currX = fx + dx, currY = fy + dy; // Начинаем с соседней клетки
  
  while (currX != tx && currY != ty) { // Проходим циклом от точки старта до точки финиша
    if (myBoard[currY][currX] != EMPTY) { // Если по пути встретили не пустую клетку
      myBoard[currY][currX] = EMPTY; // Удаляем (сбиваем) найденную фигуру
      wasCapture = true; // Фиксируем факт захвата
      break; // В шашках за один прыжок можно сбить только одну фигуру
    }
    currX += dx; currY += dy; // Переходим к следующей клетке вектора
  }

  myBoard[ty][tx] = p; // Переносим ходившую фигуру в целевую точку (теперь официально)
  // myBoard[fy][fx] = EMPTY; // Эту строку можно удалить или закомментировать, так как мы очистили клетку в начале анимации

  if (wasCapture) {
      // Проверка на "дамку" сразу, чтобы дамка могла бить дальше если нужно
      if (ty == 7 && p == BLACK) { myBoard[ty][tx] = BLACK_K; p = BLACK_K; }
      if (ty == 0 && p == WHITE) { myBoard[ty][tx] = WHITE_K; p = WHITE_K; }

      int mx, my;
      // Важно: проверяем возможность захвата именно для текущего игрока
      if (canCapturePiece(tx, ty, mx, my)) {
          chainActive = true;
          chainX = tx;
          chainY = ty;
          needRedraw = true;
          return; // Выходим БЕЗ смены фазы
      }
  }

  // Если захвата не было или цепочка прервана
  chainActive = false;
  chainX = -1;
  chainY = -1;

  // Превращение в дамку в конце обычного хода
  if (ty == 7 && p == BLACK) myBoard[ty][tx] = BLACK_K;
  if (ty == 0 && p == WHITE) myBoard[ty][tx] = WHITE_K;

  // Смена игрока
  phase = (phase == PHASE_PLAYER) ? PHASE_AI : PHASE_PLAYER;
  isWhiteTurn = (phase == PHASE_PLAYER);
  needRedraw = true;

  float minDist = 999.0; // Переменная для хранения минимальной найденной дистанции
  int8_t targetX = curX, targetY = curY; // Временные координаты для новой позиции
  bool found = false; // Флаг, нашли ли мы вообще хоть одну свою фигуру

  for (int y = 0; y < 8; y++) { // Проходим по всем клеткам доски
      for (int x = 0; x < 8; x++) { 
          if (isOwn(myBoard[y][x], isWhiteTurn)) { // Если в клетке фигура текущего игрока
              float d = pow(x - curX, 2) + pow(y - curY, 2); 
              if (d < minDist) { // Если эта фигура ближе всех найденных ранее
                  minDist = d; // Обновляем рекорд минимальной дистанции
                  targetX = x; // Запоминаем её координату X
                  targetY = y; // Запоминаем её координату Y
                  found = true; // Поднимаем флаг успеха
              }
          }
      }
  }

  if (found) { // Если на доске осталась хотя бы одна своя фигура
      curX = targetX; // Прыгаем курсором на ближайшую
      curY = targetY; // Обновляем Y
  }    
}
int readJoy(int pin){
  int v=analogRead(pin); v=constrain(v,0,4095);
  return (abs(v-2048)<300)?4:map(v,0,4095,0,7);
}

void handleInput() {
  if(phase != PHASE_PLAYER && !gameOver) return;

  static uint32_t lastMove = 0;
  static int lastX = 4, lastY = 4;

  int x = analogRead(J_X_PIN);
  int y = analogRead(J_Y_PIN);

  int dx = 0, dy = 0;

  // НОРМАЛЬНАЯ МЕРТВАЯ ЗОНА
  if(x < 1200) dx = 1;
  else if(x > 2800) dx = -1;

  if(y < 1200) dy = 1;
  else if(y > 2800) dy = -1;

  if (!gameOver) {
    if((dx != 0 || dy != 0) && millis() - lastMove > 200) { 
      // Просто вызываем умный поиск в направлении dx, dy
      moveToNearestActive(dx, dy); 
      lastMove = millis(); // Сброс таймера
    }
  }

  if(!digitalRead(J_SW_PIN)) {
    delay(60);
    while(!digitalRead(J_SW_PIN)) delay(10);
    if (gameOver) {          // КРИТИЧЕСКОЕ ИЗМЕНЕНИЕ: если игра окончена
        resetGame();         // Вызываем перезагрузку
        return;              // Выходим из функции, чтобы не сработала логика хода
    }  
    Piece sel = myBoard[curY][curX];

    if(selX == curX && selY == curY) { 
      selX = -1; 
      selY = -1; 
      clearHighlights(); 
      needRedraw = true; 
      return; 
    }

    if(selX != -1 && selY != -1) {
      int fx = selX, fy = selY, tx = curX, ty = curY;
      
      // ИСПОЛЬЗУЕМ НОВУЮ ФУНКЦИЮ:
      // Передаем нашу реальную доску myBoard, массив moves и параметры хода игрока
      mCnt = generateMovesForBoard(myBoard, moves, true, chainActive, chainX, chainY);
      
      for(int i = 0; i < mCnt; i++) {
        if(moves[i].fx == fx && moves[i].fy == fy && moves[i].tx == tx && moves[i].ty == ty) {
          applyMove(fx, fy, tx, ty); 
          selX = -1; 
          selY = -1; 
          clearHighlights(); 
          needRedraw = true; return;
        }
      }
    }

    if(isOwn(sel, true)) { 
      selX = curX; selY = curY; 
      clearHighlights(); 
      
      // И ЗДЕСЬ ТОЖЕ ОБНОВЛЯЕМ ВЫЗОВ:
      mCnt = generateMovesForBoard(myBoard, moves, true, chainActive, chainX, chainY);
      
      for(int i = 0; i < mCnt; i++) {
        if(moves[i].fx == selX && moves[i].fy == selY) {
          highlightMoves[moves[i].ty][moves[i].tx] = true;
        }
      }
      needRedraw = true;
    }
  }
}

void drawGame() {
  tft.startWrite();
  for(int y=0;y<8;y++) for(int x=0;x<8;x++){
    uint16_t tileCol=((x+y)%2==0)?COL_WHITE:COL_GREEN;
    tft.fillRect(x*TILE,y*TILE,TILE,TILE,tileCol);
    if(x==curX && y==curY){
      tft.drawRect(x*TILE,y*TILE,TILE,TILE,COL_CURSOR_BRIGHT);
      tft.drawRect(x*TILE+1,y*TILE+1,TILE-2,TILE-2,COL_BLACK);
      tft.drawRect(x*TILE+2,y*TILE+2,TILE-4,TILE-4,COL_CURSOR_BRIGHT);
    }
    if(x==selX && y==selY) tft.fillRect(x*TILE+2,y*TILE+2,TILE-4,TILE-4,COL_SELECT_BRIGHT);
    if(myBoard[y][x]!=EMPTY){
      uint16_t pCol=(myBoard[y][x]==WHITE||myBoard[y][x]==WHITE_K)?COL_BLUE:COL_RED;
      int cx=x*TILE+TILE/2,cy=y*TILE+TILE/2;
      tft.fillCircle(cx,cy,TILE/2-3,pCol);
      tft.drawCircle(cx,cy,TILE/2-3,COL_BLACK);
      tft.fillCircle(cx-4,cy-4,3,COL_WHITE);

      // ОТЛИЧИЕ ДЛЯ ДАМКИ
      if(myBoard[y][x] == WHITE_K || myBoard[y][x] == BLACK_K) {
        tft.drawCircle(cx, cy, TILE / 2 - 6, COL_YELLOW); // Золотое кольцо внутри
        tft.fillCircle(cx, cy, 3, COL_YELLOW);            // И точка в центре
      }
    }
    if(highlightMoves[y][x]) tft.drawRect(x*TILE+4,y*TILE+4,TILE-8,TILE-8,COL_YELLOW);
  }
  // --- НОВЫЙ БЛОК: Отрисовка анимированной фигуры ---
  if (isAnimating) { // Если включен режим анимации
    uint16_t pCol = (animPiece == WHITE || animPiece == WHITE_K) ? COL_BLUE : COL_RED; // Выбираем цвет: синий или красный
    int cx = animX + TILE / 2; // Вычисляем центр фигуры по X (координата + половина клетки)
    int cy = animY + TILE / 2; // Вычисляем центр фигуры по Y (координата + половина клетки)
    tft.fillCircle(cx,cy,TILE/2-3,pCol);
    tft.drawCircle(cx,cy,TILE/2-3,COL_BLACK);
    tft.fillCircle(cx-4,cy-4,3,COL_WHITE);
    if (animPiece == WHITE_K || animPiece == BLACK_K) { // Если это дамка
        tft.drawCircle(cx, cy, TILE / 2 - 6, COL_YELLOW); // Золотое кольцо внутри
        tft.fillCircle(cx, cy, 3, COL_YELLOW);            // И точка в центре
    }
  }
  //tft.fillRect(0,240,320,20,COL_BLACK);
  //tft.setTextColor(COL_YELLOW,COL_BLACK); 
  //tft.setTextSize(2);
  //tft.setCursor(5,242); 
  //tft.print((phase==PHASE_PLAYER)?"WHITE":"AI");
  if (gameOver) { // Если игра завершена, рисуем баннер
      int bW = 220; // Ширина баннера в пикселях
      int bH = 80;  // Высота баннера в пикселях
      int bX = (240 - bW) / 2; // Центрируем по горизонтали (исходя из поля 240x240)
      int bY = (240 - bH) / 2; // Центрируем по вертикали

      // Рисуем прямоугольник баннера (рамка и фон)
      tft.fillRect(bX, bY, bW, bH, gameOverCol);      // Заливка цветом (красный/зеленый)
      tft.drawRect(bX, bY, bW, bH, COL_WHITE);       // Белая рамка
      tft.drawRect(bX+2, bY+2, bW-4, bH-4, COL_BLACK); // Внутренняя черная кайма для стиля

      // Настройка текста
      tft.setTextColor(COL_WHITE);                   // Белый цвет букв
      tft.setTextSize(3);                            // Крупный размер шрифта
      
      // Вычисляем положение текста внутри баннера для центровки
      int textX = bX + (bW - (gameOverText.length() * 18)) / 2; 
      int textY = bY + 20;                           
      
      tft.setCursor(textX, textY);                   // Устанавливаем "курсор" печати
      tft.print(gameOverText);                       // Печатаем WIN или GAME OVER
  }

  tft.endWrite();
}

void setup() {
  pinMode(J_SW_PIN,INPUT_PULLUP);
  analogReadResolution(12);
  Serial.begin(115200);
  tft.init();
  tft.setRotation(5);
  tft.setBrightness(255);
  tft.fillScreen(COL_BLACK);
  for(int y=0;y<8;y++) for(int x=0;x<8;x++){
    myBoard[y][x]=EMPTY;
    if((x+y)%2==1){
      if(y<3) myBoard[y][x]=BLACK;
      if(y>4) myBoard[y][x]=WHITE;
    }
  }
}

inline bool canCaptureBit(Bitboard b, int x, int y, bool whiteTurn) {
    int dx[4] = {-1,1,-1,1};
    int dy[4] = {-1,-1,1,1};

    for(int d=0; d<4; d++) {
        int mx = x + dx[d];
        int my = y + dy[d];
        int tx = x + dx[d]*2;
        int ty = y + dy[d]*2;

        if(tx < 0 || tx >= 8 || ty < 0 || ty >= 8) continue;

        int mIdx = getIdx(mx,my);
        int tIdx = getIdx(tx,ty);

        if(mIdx == -1 || tIdx == -1) continue;

        if(whiteTurn) {
            if((b.b & (1UL<<mIdx)) && !(b.w & (1UL<<tIdx)) && !(b.b & (1UL<<tIdx)))
                return true;
        } else {
            if((b.w & (1UL<<mIdx)) && !(b.w & (1UL<<tIdx)) && !(b.b & (1UL<<tIdx)))
                return true;
        }
    }
    return false;
}

// Функция применения хода к битовой доске (без влияния на экран)
void applyMoveToBitboard(Bitboard &b, Move m) {
    int8_t fIdx = getIdx(m.fx, m.fy); // Индекс клетки "откуда"
    int8_t tIdx = getIdx(m.tx, m.ty); // Индекс клетки "куда"
    uint32_t fMask = (1UL << fIdx); // Маска начальной позиции
    uint32_t tMask = (1UL << tIdx); // Маска конечной позиции

    if (b.b & fMask) { // Если ходит черная фигура
        b.b &= ~fMask; // Снимаем с начальной позиции
        b.b |= tMask;  // Ставим на новую позицию
        if (b.k & fMask) { b.k &= ~fMask; b.k |= tMask; } // Переносим статус дамки
        if (m.ty == 7) b.k |= tMask; // Превращаем в дамку при достижении края
    } else { // Если ходит белая фигура
        b.w &= ~fMask; // Снимаем белую шашку
        b.w |= tMask;  // Ставим в новую клетку
        if (b.k & fMask) { b.k &= ~fMask; b.k |= tMask; } // Переносим статус дамки
        if (m.ty == 0) b.k |= tMask; // Превращаем в дамку
    }

    if (m.capture) {
        int dx = (m.tx > m.fx) ? 1 : -1; // Направление по X
        int dy = (m.ty > m.fy) ? 1 : -1; // Направление по Y
        int cx = m.fx + dx;
        int cy = m.fy + dy;
        
        while (cx != m.tx) { // Идем от старта до финиша
            int8_t cIdx = getIdx(cx, cy);
            if (cIdx != -1) {
                uint32_t cMask = ~(1UL << cIdx);
                b.w &= cMask; // Удаляем белую, если попалась
                b.b &= cMask; // Удаляем черную, если попалась
                b.k &= cMask; // Снимаем статус дамки
            }
            cx += dx; cy += dy;
        }
    }
}
// Рекурсивный алгоритм поиска лучшего хода с альфа-бета отсечением
int minimax(Bitboard b, int depth, bool maximizing, int alpha, int beta) {
  // Если достигли максимальной глубины, возвращаем оценку текущей позиции
  if (depth <= 0) return evaluateBitboard(b);
  // Массив для хранения списка возможных ходов на текущем уровне рекурсии
  Move localMoves[32];
  // Генерируем ходы: если maximizing=true, то ходит ИИ (Черные), иначе игрок (Белые)
  int count = generateMovesFromBitboard(b, localMoves, !maximizing); 

  // Если ходов нет, значит текущий игрок проиграл (возвращаем экстремально низкое/высокое значение)
  if (count == 0) return maximizing ? -10000 : 10000;
  struct PrioritizedMove { 
    Move m; 
    int p; 
  } 
  pMoves[count];

  for (int i = 0; i < count; i++) {
      pMoves[i].m = localMoves[i];
      pMoves[i].p = getMovePriority(b, localMoves[i]); // Считаем приоритет один раз
  }

  // Простая сортировка ходов по приоритету (взятия вперед)
  for (int i = 0; i < count - 1; i++) {
      for (int j = 0; j < count - i - 1; j++) {
          if (pMoves[j].p < pMoves[j+1].p) {
              PrioritizedMove temp = pMoves[j];
              pMoves[j] = pMoves[j+1];
              pMoves[j+1] = temp;
          }
      }
  }

  if (maximizing) {// Ветка максимизации (ход ИИ)
    // Инициализируем лучшую оценку как "минус бесконечность"
    int maxEval = -20000;
    for (int i = 0; i < count; i++) { // Перебираем все возможные ходы
      Bitboard next = b; // Создаем копию доски для симуляции хода
      applyMoveToBitboard(next, pMoves[i].m); // Используем отсортированный ход
      // Рекурсивно вызываем минимакс для хода противника
      int eval = minimax(next, depth - 1, false, alpha, beta);
      // Обновляем лучший результат
      maxEval = max(maxEval, eval); 
      // Обновляем "альфа" (лучшее, что может гарантировать себе максимизирующий игрок)
      alpha = max(alpha, eval); 
      // Если ветка уже хуже, чем гарантированный результат игрока, отсекаем её
      if (beta <= alpha) break;
      }
      return maxEval; // Возвращаем лучшую найденную оценку
  } else { // Ветка минимизации (ход игрока)
      // Инициализируем лучшую оценку как "плюс бесконечность"
      int minEval = 20000; 
      for (int i = 0; i < count; i++) { // Перебираем ходы игрока
          Bitboard next = b; // Копируем доску
          applyMoveToBitboard(next, pMoves[i].m); // Используем отсортированный ход
          // Рекурсивно вызываем минимакс для хода ИИ
          int eval = minimax(next, depth - 1, true, alpha, beta); 
          // Обновляем худший результат (с точки зрения ИИ)
          minEval = min(minEval, eval); 
          // Обновляем "бета" (лучшее, что может гарантировать себе минимизирующий игрок)
          beta = min(beta, eval); 
          // Отсечение по альфе
          if (beta <= alpha) break; 
      }
      return minEval; // Возвращаем худшую для ИИ (лучшую для игрока) оценку
  }
}

// Структура для хранения результата анализа конкретного хода
struct EvalResult {
    Move move;      // Сам ход
    int score;      // Оценка, полученная из Minimax
};

// Функция для перевода координаты в шахматную нотацию (например, 0,7 -> a1)
String toNotation(int8_t x, int8_t y) {
    char col = 'a' + x;     // Столбец от 'a' до 'h'
    char row = '8' - y;     // Строка от '8' до '1'
    return String(col) + String(row);
}

// Функция для быстрой оценки приоритета хода (без рекурсии)
int getMovePriority(Bitboard &b, Move &m) {
    int priority = 0; // Начальный приоритет
    if (m.capture) priority += 1000; // Взятие — самое важное
    
    int8_t fIdx = getIdx(m.fx, m.fy); // Индекс откуда ходим
    if (b.k & (1UL << fIdx)) priority += 500; // Ходы дамками проверяем раньше простых шашек
    
    return priority; // Возвращаем вес для сортировки
}

// Основная функция генерации и применения хода ИИ
void generateAI() {
    // Если сейчас не фаза ИИ или игра окончена, ничего не делаем
    if (phase != PHASE_AI || gameOver) return; 

    Bitboard current = {0, 0, 0}; // Создаем структуру битовой доски
    // Конвертируем текущий массив myBoard в битовое представление для расчетов
    for (int8_t y = 0; y < 8; y++) { 
        for (int8_t x = 0; x < 8; x++) {
            int8_t idx = getIdx(x, y); // Получаем индекс клетки
            if (idx == -1) continue; // Пропускаем белые клетки
            if (myBoard[y][x] == WHITE) current.w |= (1UL << idx); // Бит белой шашки
            if (myBoard[y][x] == BLACK) current.b |= (1UL << idx); // Бит черной шашки
            if (myBoard[y][x] == WHITE_K) { current.w |= (1UL << idx); current.k |= (1UL << idx); } // Белая дамка
            if (myBoard[y][x] == BLACK_K) { current.b |= (1UL << idx); current.k |= (1UL << idx); } // Черная дамка
        }
    }

    Move aiMoves[32]; // Массив для возможных ходов ИИ
    // Генерируем все легальные ходы для черных (ИИ)
    int count = generateMovesFromBitboard(current, aiMoves, false); 

    // Если ходов нет, ИИ проиграл
    if (count == 0) { 
        gameOver = true; 
        gameOverText = "WIN"; 
        gameOverCol = COL_BLUE;
        return; 
    } 
    // Создаем массив для хранения оценок всех возможных ходов
    EvalResult results[count];

    // 1. Расчет всех ходов через Minimax
    for (int i = 0; i < count; i++) {
        Bitboard simulation = current;
        applyMoveToBitboard(simulation, aiMoves[i]);
        // Получаем оценку хода
        int score = minimax(simulation, AI_MAX_DEPTH - 1, false, -30000, 30000);
        
        results[i].move = aiMoves[i];
        results[i].score = score;
    }

    // 2. Сортировка результатов по убыванию (Bubble Sort)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (results[j].score < results[j+1].score) { // Если следующий ход лучше текущего
                EvalResult temp = results[j];            // Меняем их местами
                results[j] = results[j+1];
                results[j+1] = temp;
            }
        }
    }
    
    // 3. Вывод расчетов на экран (в нижнюю часть под доской)
    tft.fillRect(0, 240, 320, 90, COL_BLACK); // Очищаем зону логов (ниже текста "AI")
    tft.setTextSize(1);                       // Мелкий шрифт для логов
    tft.setTextColor(COL_WHITE);
    
    // Выводим топ-4 рассчитанных хода (чтобы влезло на экран)
    for (int i = 0; i < min(count, 8); i++) {
        tft.setCursor(5, 240 + (i * 10)); // Смещение каждой строки на 10 пикселей вниз
        String log = "#" + String(i+1) + ": " + 
                     toNotation(results[i].move.fx, results[i].move.fy) + "->" + 
                     toNotation(results[i].move.tx, results[i].move.ty) + 
                     " " + String(results[i].score);
        tft.print(log);
    }
    for (int i = 0; i < count; i++) {
        String log = "#" + String(i+1) + ": " + 
                     toNotation(results[i].move.fx, results[i].move.fy) + "->" + 
                     toNotation(results[i].move.tx, results[i].move.ty) + 
                     " " + String(results[i].score);
        Serial.println(log); // Дублируем в монитор порта для удобства
    }

    // 4. Применяем самый лучший ход (он теперь под индексом 0 после сортировки)
    applyMove(results[0].move.fx, results[0].move.fy, results[0].move.tx, results[0].move.ty);    
}

int generateMovesFromBitboard(Bitboard b, Move* moveList, bool whiteTurn) {
    int count = 0;
    // Здесь мы просто временно переводим биты в массив, чтобы использовать твою 
    // готовую логику generateMovesForBoard без переписывания всего движка на чистые сдвиги
    Piece temp[8][8];
    for(int y=0; y<8; y++) for(int x=0; x<8; x++) temp[y][x] = EMPTY;
    
    for(int i=0; i<32; i++) {
        int8_t x, y; getPos(i, x, y);
        if (b.w & (1UL << i)) temp[y][x] = (b.k & (1UL << i)) ? WHITE_K : WHITE;
        if (b.b & (1UL << i)) temp[y][x] = (b.k & (1UL << i)) ? BLACK_K : BLACK;
    }
    
    return generateMovesForBoard(temp, moveList, whiteTurn, false, -1, -1);
}

void resetGame() {
    // 1. Очистка доски
    for(int y=0; y<8; y++) {
        for(int x=0; x<8; x++) {
            myBoard[y][x] = EMPTY; // Сначала очищаем всё
            if((x+y)%2 == 1) {     // Ставим шашки только на темные клетки
                if(y < 3) myBoard[y][x] = BLACK; // Шашки ИИ
                if(y > 4) myBoard[y][x] = WHITE; // Шашки игрока
            }
        }
    }

    // 2. Сброс игровых состояний
    phase = PHASE_PLAYER;   // Первым всегда ходит игрок
    isWhiteTurn = true;     // Белые ходят первыми
    gameOver = false;       // Снимаем блокировку игры
    gameOverText = "";      // Убираем текст
    selX = -1;              // Снимаем выделение
    selY = -1;
    chainActive = false;    // Сбрасываем серии прыжков
    clearHighlights();      // Убираем подсветку ходов

    // 3. Установка курсора на первую доступную фигуру
    for(int y = 0; y < 8; y++) {
        for(int x = 0; x < 8; x++) {
            if(isOwn(myBoard[y][x], true)) {
                curX = x; curY = y;
                goto found; // Быстрый выход из вложенного цикла
            }
        }
    }
    found: 
    needRedraw = true; // Запрос на полную перерисовку
}

void loop() {
  handleInput();
  if (!gameOver) {
    if (phase == PHASE_PLAYER) {
      Move pMoves[64];
      int count = generateMovesForBoard(myBoard, pMoves, true, chainActive, chainX, chainY);
      if (count == 0) { // Игроку некуда ходить — ПРОИГРЫШ
          gameOver = true;                   // Останавливаем игру
          gameOverText = "GAME OVER";        // Текст поражения
          gameOverCol = COL_RED;             // Красный фон
          needRedraw = true;                 // Запрос на отрисовку баннера
      }
    } 

    if (phase == PHASE_AI) {
      delay(500); // Пауза для естественности
      generateAI();
    }
  }

  if (needRedraw) {
    drawGame(); 
    needRedraw = false; 
  }
}