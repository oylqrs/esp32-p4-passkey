#pragma once
#include <stdbool.h>
#include <stdint.h>
#define DUCKY_TEXT_MAX_LEN 512
typedef enum { DUCKY_COMMAND_EMPTY, DUCKY_COMMAND_STRING, DUCKY_COMMAND_STRINGLN, DUCKY_COMMAND_DELAY, DUCKY_COMMAND_DEFAULT_DELAY, DUCKY_COMMAND_KEYSTROKE } ducky_command_type_t;
typedef struct { ducky_command_type_t type; uint32_t delay_ms; uint8_t modifier; uint8_t key; char text[DUCKY_TEXT_MAX_LEN]; } ducky_command_t;
bool ducky_command_parse_keystroke(char *text, uint8_t *modifier, uint8_t *key);
