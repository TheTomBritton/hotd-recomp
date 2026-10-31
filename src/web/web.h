#ifndef HOTD_WEB_H
#define HOTD_WEB_H
#include <stddef.h>
/* The game folder (fetched from beside the page, file by file as the game
 * reads them) and the folder for what the game writes (the browser's own
 * storage): 1 if there is a game. */
int  web_mount(const char *exe_name, char *game_dir, size_t gn, char *save_dir, size_t sn);
void web_set_status(const char *text);        /* a line for the page to show */
void web_pump(void);
#endif
