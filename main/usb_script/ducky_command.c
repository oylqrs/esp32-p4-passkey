#include "ducky_command.h"
#include <string.h>
#include <strings.h>
#include "keyboard_layout.h"

bool ducky_command_parse_keystroke(char *text, uint8_t *modifier, uint8_t *key)
{
    *modifier = 0; *key = 0;
    char *save = NULL;
    for (char *token = strtok_r(text, " \t", &save); token; token = strtok_r(NULL, " \t", &save)) {
        if (!strcasecmp(token, "CTRL") || !strcasecmp(token, "CONTROL")) *modifier |= 0x01;
        else if (!strcasecmp(token, "SHIFT")) *modifier |= 0x02;
        else if (!strcasecmp(token, "ALT")) *modifier |= 0x04;
        else if (!strcasecmp(token, "GUI") || !strcasecmp(token, "WINDOWS") || !strcasecmp(token, "COMMAND")) *modifier |= 0x08;
        else if (*key != 0 || !keyboard_layout_named_key(token, key)) return false;
    }
    return *modifier != 0 || *key != 0;
}
