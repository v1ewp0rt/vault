#include <ncurses.h>

int main() {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);

    printw("PRESS KEY (Q TO EXIT)\n");
    refresh();

    int ch;
    while ((ch=getch())!='q') {
        printw("KEY: %d\n", ch);
        refresh();
    }

    endwin();
    return 0;
}