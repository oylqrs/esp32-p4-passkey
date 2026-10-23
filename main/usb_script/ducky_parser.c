#include "ducky_parser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

esp_err_t ducky_parser_parse_line(char *line, ducky_command_t *command)
{
    memset(command, 0, sizeof(*command));
    char *text = trim(line);
    if (!*text || (!strncasecmp(text, "REM", 3) && (text[3] == '\0' || isspace((unsigned char)text[3])))) return ESP_OK;
    char keystroke[DUCKY_TEXT_MAX_LEN];
    if (strlcpy(keystroke, text, sizeof(keystroke)) >= sizeof(keystroke)) return ESP_ERR_INVALID_SIZE;

    char *argument = text;
    while (*argument && !isspace((unsigned char)*argument)) argument++;
    if (*argument) { *argument++ = '\0'; while (isspace((unsigned char)*argument)) argument++; }

    if (!strcasecmp(text, "STRING") || !strcasecmp(text, "STRINGLN")) {
        command->type = !strcasecmp(text, "STRINGLN") ? DUCKY_COMMAND_STRINGLN : DUCKY_COMMAND_STRING;
        if (strlcpy(command->text, argument, sizeof(command->text)) >= sizeof(command->text)) return ESP_ERR_INVALID_SIZE;
        return ESP_OK;
    }
    if (!strcasecmp(text, "DELAY") || !strcasecmp(text, "DEFAULT_DELAY") || !strcasecmp(text, "DEFAULTDELAY")) {
        char *end; unsigned long delay = strtoul(argument, &end, 10);
        if (!*argument || *end || delay > UINT32_MAX) return ESP_ERR_INVALID_ARG;
        command->delay_ms = (uint32_t)delay;
        command->type = !strcasecmp(text, "DELAY") ? DUCKY_COMMAND_DELAY : DUCKY_COMMAND_DEFAULT_DELAY;
        return ESP_OK;
    }
    command->type = DUCKY_COMMAND_KEYSTROKE;
    return ducky_command_parse_keystroke(keystroke, &command->modifier, &command->key) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
