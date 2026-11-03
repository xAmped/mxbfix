/*
 * The in-game page: a small tab that opens one page with the version, update status, crashes prevented and the state
 * of each fix. The tab appears when the mouse moves. Online the game can't pause, so this works while riding too; the page closes when the
 * throttle opens or the mouse is left alone. [ui] enabled=0 turns it off; the fixes don't depend on it.
 */
#pragma once

int  ui_draw_init(int *num_sprites, char **sprite_names, int *num_fonts, char **font_names);
void ui_draw(int state, int *num_quads, void **quads, int *num_strings, void **strings);
int  ui_reports_allowed(void);    /* the player went through the setup and left crash reports on */
