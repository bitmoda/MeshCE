#ifndef HALO_MAC_MENU_H
#define HALO_MAC_MENU_H
#include <stddef.h>
void host_menu_initialize_application(void);
int host_menu_prepare(const char *support, const char *fallback, char *data, size_t capacity);
void host_menu_begin_game(void);
/* a throwaway local packet, before the game window, so the local-network
prompt is answered before a match connects */
void host_menu_allow_local_network(void);
void host_menu_finish_game(int exit_code);
void host_menu_window_changed(void);
void host_menu_style_window(void *window);
int host_menu_set_fullscreen(int enabled);
/* SDL operations stay on its real main thread. */
int host_sdl_is_fullscreen(void);
int host_sdl_set_fullscreen(int enabled);
void host_sdl_release_mouse(void);
void host_sdl_show_game(void);
void host_sdl_background_game(void);
int host_sdl_game_is_background(void);
void host_sdl_request_quit(void);
void host_menu_note_background(int hidden);
#endif
