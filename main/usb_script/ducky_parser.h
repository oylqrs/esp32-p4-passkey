#pragma once
#include "ducky_command.h"
#include "esp_err.h"
esp_err_t ducky_parser_parse_line(char *line, ducky_command_t *command);
