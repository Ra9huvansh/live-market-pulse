#include <ncurses.h>
#include <unistd.h>

int main() {
    initscr();           // start ncurses mode
    start_color();       // enable colors
    cbreak();            // don't wait for enter key
    noecho();            // don't show typed characters
    curs_set(0);         // hide cursor

    // Define color pairs: pair number, foreground, background
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED,   COLOR_BLACK);
    init_pair(3, COLOR_CYAN,  COLOR_BLACK);

    // Draw something at exact coordinates (row, col)
    attron(COLOR_PAIR(3));
    mvprintw(0, 0, "============= PULSE =============");
    attroff(COLOR_PAIR(3));

    attron(COLOR_PAIR(2));
    mvprintw(3, 2, "$69151.85  |  0.0002 BTC  <-- SELL");
    attroff(COLOR_PAIR(2));

    attron(COLOR_PAIR(1));
    mvprintw(5, 2, "$69151.39  |  2.5759 BTC  <-- BUY");
    attroff(COLOR_PAIR(1));

    refresh();           // push changes to screen
    sleep(3);            // wait 3 seconds
    endwin();            // restore terminal
    return 0;
}
