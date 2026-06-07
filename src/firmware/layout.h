#pragma once

#include "device.h"

#define CHAR_W          6
#define CHAR_H          8
#define MARGIN_X        0
#define MARGIN_Y        0
#define STATUS_H        10

#if defined(BOARD_RETERMINAL)
#define MAX_TEXT_LEN    16384       // larger notepad on the 7.5" panel
#define SCREEN_W        800
#define SCREEN_H        480
#define TERM_ROWS       120         // terminal scrollback
#else
#define MAX_TEXT_LEN    4096
#define SCREEN_W        240
#define SCREEN_H        320
#define TERM_ROWS       100
#endif

#define COLS_PER_LINE   ((SCREEN_W - MARGIN_X * 2) / CHAR_W)
#define ROWS_PER_SCREEN ((SCREEN_H - MARGIN_Y - STATUS_H) / CHAR_H)
#define TERM_COLS       COLS_PER_LINE
