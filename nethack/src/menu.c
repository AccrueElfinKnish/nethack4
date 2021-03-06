/* vim:set cin ft=c sw=4 sts=4 ts=8 et ai cino=Ls\:0t0(0 : -*- mode:c;fill-column:80;tab-width:8;c-basic-offset:4;indent-tabs-mode:nil;c-file-style:"k&r" -*-*/
/* Last modified by Alex Smith, 2014-04-10 */
/* Copyright (c) Daniel Thaler, 2011 */
/* NetHack may be freely redistributed.  See license for details. */

#include "nhcurses.h"


static nh_bool do_item_actions(const struct nh_objitem *item);

static int
calc_colwidths(char *menustr, int *colwidth)
{
    char *start, *tab;
    int col = 0;

    start = menustr;
    while ((tab = strchr(start, '\t')) != NULL && col < MAXCOLS) {
        colwidth[col] = max(colwidth[col], tab - start);
        start = tab + 1;
        col++;
    }
    colwidth[col] = max(colwidth[col], strlen(start));
    return col;
}


static int
calc_menuwidth(int *colwidth, int *colpos, int maxcol)
{
    int i;

    for (i = 1; i <= maxcol; i++)
        colpos[i] = colpos[i - 1] + colwidth[i - 1] + 1;
    return colpos[maxcol] + colwidth[maxcol];
}


static void
layout_menu(struct gamewin *gw)
{
    struct win_menu *mdat = (struct win_menu *)gw->extra;
    int i, col, w, x1, x2, y1, y2;
    int colwidth[MAXCOLS];
    int scrheight;
    int scrwidth;
    int singlewidth = 0;

    x1 = (mdat->x1 > 0) ? mdat->x1 : 0;
    y1 = (mdat->y1 > 0) ? mdat->y1 : 0;
    x2 = (mdat->x2 > 0 && mdat->x2 <= COLS) ? mdat->x2 : COLS;
    y2 = (mdat->y2 > 0 && mdat->y2 <= LINES) ? mdat->y2 : LINES;

    scrheight = y2 - y1;
    scrwidth = x2 - x1;

    /* calc height */
    mdat->frameheight = 2;
    if (mdat->title && mdat->title[0]) {
        mdat->frameheight += 2; /* window title + space */
    }

    mdat->height = mdat->frameheight + min(mdat->icount, 52);
    if (mdat->height > scrheight)
        mdat->height = scrheight;
    mdat->innerheight = mdat->height - mdat->frameheight;

    /* calc width */
    mdat->maxcol = 0;
    for (i = 0; i < MAXCOLS; i++)
        colwidth[i] = 0;

    for (i = 0; i < mdat->icount; i++) {
        /* headings without tabs are not fitted into columns, but headers with
           tabs are presumably column titles */
        if (!strchr(mdat->items[i].caption, '\t')) {
            w = strlen(mdat->items[i].caption);
            if (mdat->items[i].role == MI_NORMAL && mdat->items[i].id)
                w += 4;
            singlewidth = max(singlewidth, w);
        } else {
            col = calc_colwidths(mdat->items[i].caption, colwidth);
            mdat->maxcol = max(mdat->maxcol, col);
        }
    }
    if (mdat->how != PICK_NONE)
        colwidth[0] += 4;       /* "a - " */

    mdat->innerwidth =
        max(calc_menuwidth(colwidth, mdat->colpos, mdat->maxcol), singlewidth);
    if (mdat->innerwidth > scrwidth - 4)        /* make sure there is space for 
                                                   window borders */
        mdat->innerwidth = scrwidth - 4;
    mdat->width = mdat->innerwidth + 4; /* border + space */
    mdat->colpos[mdat->maxcol + 1] = mdat->innerwidth + 1;

    if (mdat->title && mdat->width < strlen(mdat->title) + 4) {
        mdat->innerwidth = strlen(mdat->title);
        mdat->width = mdat->innerwidth + 4;
    }
}


void
draw_menu(struct gamewin *gw)
{
    struct win_menu *mdat = (struct win_menu *)gw->extra;
    struct nh_menuitem *item;
    char caption[BUFSZ];
    char *colstrs[MAXCOLS];
    int i, j, col, scrlheight, scrlpos, scrltop, attr;
    char *tab;

    wattron(gw->win, FRAME_ATTRS);
    box(gw->win, 0, 0);
    wattroff(gw->win, FRAME_ATTRS);
    if (mdat->title) {
        wattron(gw->win, A_UNDERLINE);
        mvwaddnstr(gw->win, 1, 2, mdat->title, mdat->width - 4);
        wattroff(gw->win, A_UNDERLINE);
    }

    werase(gw->win2);
    /* draw menu items */
    item = &mdat->items[mdat->offset];
    for (i = 0; i < mdat->innerheight; i++, item++) {
        strncpy(caption, item->caption, BUFSZ - 1);
        caption[BUFSZ - 1] = '\0';

        col = 0;
        colstrs[col] = caption;
        while ((tab = strchr(colstrs[col], '\t')) != NULL) {
            col++;
            *tab = '\0';
            colstrs[col] = tab + 1;
        }

        if (item->role == MI_HEADING)
            wattron(gw->win2, settings.menu_headings);

        wmove(gw->win2, i, 0);
        if (mdat->how != PICK_NONE && item->role == MI_NORMAL && item->accel)
            wprintw(gw->win2, "%c %c ", item->accel,
                    mdat->selected[mdat->offset + i] ? '+' : '-');

        if (col) {
            for (j = 0; j <= col; j++) {
                waddnstr(gw->win2, colstrs[j],
                         mdat->colpos[j + 1] - mdat->colpos[j] - 1);
                if (j < col)
                    wmove(gw->win2, i, mdat->colpos[j + 1]);
            }
        } else
            waddnstr(gw->win2, caption, mdat->innerwidth);

        if (item->role == MI_HEADING)
            wattroff(gw->win2, settings.menu_headings);
    }

    if (mdat->icount <= mdat->innerheight)
        return;
    /* draw scroll bar */
    scrltop = mdat->title ? 3 : 1;
    scrlheight = mdat->innerheight * mdat->innerheight / mdat->icount;
    scrlpos = mdat->offset * (mdat->innerheight - scrlheight) /
        (mdat->icount - mdat->innerheight);
    for (i = 0; i < mdat->innerheight; i++) {
        attr = A_NORMAL;
        if (i >= scrlpos && i < scrlpos + scrlheight)
            attr = A_REVERSE;
        wattron(gw->win, attr);
        mvwaddch(gw->win, i + scrltop, mdat->innerwidth + 2, ' ');
        wattroff(gw->win, attr);
    }
}


static void
resize_menu(struct gamewin *gw)
{
    struct win_menu *mdat = (struct win_menu *)gw->extra;
    int startx, starty;

    layout_menu(gw);
    /* if the window got longer and the last line was already visible,
       draw_menu would go past the end of mdat->items without this */
    if (mdat->offset > mdat->icount - mdat->innerheight)
        mdat->offset = mdat->icount - mdat->innerheight;

    delwin(gw->win2);
    wresize(gw->win, mdat->height, mdat->width);
    gw->win2 =
        derwin(gw->win, mdat->innerheight, mdat->innerwidth,
               mdat->frameheight - 1, 2);

    starty = (LINES - mdat->height) / 2;
    startx = (COLS - mdat->width) / 2;

    mvwin(gw->win, starty, startx);

    draw_menu(gw);
}


static void
assign_menu_accelerators(struct win_menu *mdat)
{
    int i;
    char accel = 'a';

    for (i = 0; i < mdat->icount; i++) {

        if (mdat->items[i].accel || mdat->items[i].role != MI_NORMAL ||
            mdat->items[i].id == 0)
            continue;

        mdat->items[i].accel = accel;

        if (accel == 'z')
            accel = 'A';
        else if (accel == 'Z')
            accel = 'a';
        else
            accel++;
    }
}


static int
find_accel(int accel, struct win_menu *mdat)
{
    int i, upper;

    /* 
     * scan visible entries first: long list of (eg the options menu)
     * might re-use accelerators, so that each is only unique among the visible
     * menu items
     */
    upper = min(mdat->icount, mdat->offset + mdat->innerheight);
    for (i = mdat->offset; i < upper; i++)
        if (mdat->items[i].accel == accel)
            return i;

    /* it's not a regular accelerator, maybe there is a group accel? */
    for (i = mdat->offset; i < upper; i++)
        if (mdat->items[i].group_accel == accel)
            return i;
    /* 
     * extra effort: if the list is too long for one page search for the accel
     * among those entries, too and scroll the changed item into view.
     */
    if (mdat->icount > mdat->innerheight)
        for (i = 0; i < mdat->icount; i++)
            if (mdat->items[i].accel == accel) {
                if (i > mdat->offset)
                    mdat->offset = i - mdat->innerheight + 1;
                else
                    mdat->offset = i;
                return i;
            }

    return -1;
}


static void
menu_search_callback(const char *sbuf, void *mdat_void)
{
    struct win_menu *mdat = mdat_void;
    int i;

    for (i = 0; i < mdat->icount; i++)
        if (strstr(mdat->items[i].caption, sbuf))
            break;
    if (i < mdat->icount) {
        int end = max(mdat->icount - mdat->innerheight, 0);
        
        mdat->offset = min(i, end);
    }
}

static void
objmenu_search_callback(const char *sbuf, void *mdat_void)
{
    struct win_objmenu *mdat = mdat_void;
    int i;

    for (i = 0; i < mdat->icount; i++)
        if (strstr(mdat->items[i].caption, sbuf))
            break;
    if (i < mdat->icount) {
        int end = max(mdat->icount - mdat->innerheight, 0);
        
        mdat->offset = min(i, end);
    }
}


void
curses_display_menu_core(struct nh_menulist *ml, const char *title, int how,
                         void *callbackarg,
                         void (*callback)(const int *, int, void *),
                         int x1, int y1, int x2, int y2, nh_bool bottom,
                         nh_bool(*changefn) (struct win_menu *, int))
{
    struct gamewin *gw;
    struct win_menu *mdat;
    int i, key, idx, rv, startx, starty, prevcurs, prev_offset;
    nh_bool done, cancelled;
    char selected[ml->icount ? ml->icount : 1];
    int results[ml->icount ? ml->icount : 1];

    /* Make a stack-allocated copy of the menulist, in case we end up longjmping
       out of here before we get a chance to deallocate it. */
    struct nh_menuitem item_copy[ml->icount ? ml->icount : 1];
    if (ml->icount)
        memcpy(item_copy, ml->items, sizeof item_copy);

    memset(selected, 0, sizeof selected);

    prevcurs = curs_set(0);

    gw = alloc_gamewin(sizeof (struct win_menu));
    gw->draw = draw_menu;
    gw->resize = resize_menu;
    mdat = (struct win_menu *)gw->extra;
    mdat->items = item_copy;
    mdat->icount = ml->icount;
    mdat->title = title;
    mdat->how = how;
    mdat->selected = selected;
    mdat->x1 = x1;
    mdat->y1 = y1;
    mdat->x2 = x2;
    mdat->y2 = y2;

    dealloc_menulist(ml);

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 < 0)
        x2 = COLS;
    if (y2 < 0)
        y2 = LINES;


    assign_menu_accelerators(mdat);
    layout_menu(gw);

    starty = (y2 - y1 - mdat->height) / 2 + y1;
    startx = (x2 - x1 - mdat->width) / 2 + x1;

    gw->win = newwin(mdat->height, mdat->width, starty, startx);
    keypad(gw->win, TRUE);
    wattron(gw->win, FRAME_ATTRS);
    box(gw->win, 0, 0);
    wattroff(gw->win, FRAME_ATTRS);
    gw->win2 =
        derwin(gw->win, mdat->innerheight, mdat->innerwidth,
               mdat->frameheight - 1, 2);
    leaveok(gw->win, TRUE);
    leaveok(gw->win2, TRUE);
    done = FALSE;
    cancelled = FALSE;

    if (bottom && mdat->icount - mdat->innerheight > 0)
        mdat->offset = mdat->icount - mdat->innerheight;
    while (!done && !cancelled) {
        draw_menu(gw);

        key = nh_wgetch(gw->win);

        switch (key) {
            /* one line up */
        case KEY_UP:
            if (mdat->offset > 0)
                mdat->offset--;
            break;

            /* one line down */
        case KEY_DOWN:
            if (mdat->offset < mdat->icount - mdat->innerheight)
                mdat->offset++;
            break;

            /* page up */
        case KEY_PPAGE:
        case '<':
            mdat->offset -= mdat->innerheight;
            if (mdat->offset < 0)
                mdat->offset = 0;
            break;

            /* page down */
        case KEY_NPAGE:
        case '>':
        case ' ':
            prev_offset = mdat->offset;
            mdat->offset += mdat->innerheight;
            if (mdat->offset > mdat->icount - mdat->innerheight)
                mdat->offset = mdat->icount - mdat->innerheight;
            if (key == ' ' && mdat->offset == prev_offset)
                done = TRUE;
            break;

            /* go to the top */
        case KEY_HOME:
        case '^':
            mdat->offset = 0;
            break;

            /* go to the end */
        case KEY_END:
        case '|':
            mdat->offset = max(mdat->icount - mdat->innerheight, 0);
            break;

            /* cancel */
        case KEY_ESCAPE:
            cancelled = TRUE;
            break;

            /* confirm */
        case KEY_ENTER:
        case '\r':
            done = TRUE;
            break;

            /* select all */
        case '.':
            if (mdat->how == PICK_ANY)
                for (i = 0; i < mdat->icount; i++)
                    if (mdat->items[i].role == MI_NORMAL)
                        mdat->selected[i] = TRUE;
            break;

            /* select none */
        case '-':
            for (i = 0; i < mdat->icount; i++)
                mdat->selected[i] = FALSE;
            break;

            /* search for a menu item */
        case ':':
            curses_getline("Search:", mdat, menu_search_callback);
            break;

            /* try to find an item for this key and, if one is found, select it 
             */
        default:
            if (mdat->how == PICK_LETTER) {
                if (key >= 'a' && key <= 'z') {
                    results[0] = key - 'a' + 1;
                    break;
                } else if (key >= 'A' && key <= 'Z') {
                    results[0] = key - 'A' + 27;
                    break;
                }
            }
            idx = find_accel(key, mdat);

            if (idx != -1 &&    /* valid accelerator */
                (!changefn || changefn(mdat, idx))) {
                mdat->selected[idx] = !mdat->selected[idx];

                if (mdat->how == PICK_ONE)
                    done = TRUE;
            }
            break;
        }
    }

    if (cancelled)
        rv = -1;
    else {
        rv = 0;
        for (i = 0; i < mdat->icount; i++) {
            if (mdat->selected[i]) {
                results[rv] = mdat->items[i].id;
                rv++;
            }
        }
    }

    delete_gamewin(gw);
    redraw_game_windows();
    curs_set(prevcurs);

    callback(results, rv, callbackarg);
}


void
curses_menu_callback(const int *results, int nresults, void *arg)
{
    if (nresults > 0)
        memcpy(arg, results, nresults * sizeof *results);
    else
        *(int *)arg = CURSES_MENU_CANCELLED;
}


void
curses_display_menu(struct nh_menulist *ml, const char *title,
                    int how, int placement_hint, void *callbackarg,
                    void (*callback)(const int *, int, void *))
{
    int x1 = 0, y1 = 0, x2 = -1, y2 = -1;

    if (placement_hint == PLHINT_INVENTORY ||
        placement_hint == PLHINT_CONTAINER) {
        if (ui_flags.draw_sidebar)
            x1 = COLS - getmaxx(sidebar);
        else
            x1 = COLS - 64;
    } else if (placement_hint == PLHINT_ONELINER && game_is_running) {
        x1 = 0;
        if (ui_flags.draw_sidebar)
            x2 = getmaxx(sidebar);
        y1 = 0;
        y2 = getmaxy(msgwin);
    }

    curses_display_menu_core(ml, title, how, callbackarg, callback,
                             x1, y1, x2, y2, FALSE, NULL);
}

/******************************************************************************
 * object menu functions follow
 * they are _very_ similar to the functions for regular menus, but not
 * identical. I can't shake the feeling that it should be possible to share lots
 * of code, but I can't quite see how to get there...
 */

static void
layout_objmenu(struct gamewin *gw)
{
    struct win_objmenu *mdat = (struct win_objmenu *)gw->extra;
    char weightstr[16];
    int i, maxwidth, itemwidth;
    int scrheight = LINES;
    int scrwidth = COLS;

    /* calc height */
    mdat->frameheight = 2;
    if (mdat->title && mdat->title[0]) {
        mdat->frameheight += 2; /* window title + space */
    }

    mdat->height = mdat->frameheight + min(mdat->icount, 52);
    if (mdat->height > scrheight)
        mdat->height = scrheight;
    mdat->innerheight = mdat->height - mdat->frameheight;

    /* calc width */
    maxwidth = 0;
    for (i = 0; i < mdat->icount; i++) {
        itemwidth = strlen(mdat->items[i].caption);

        /* add extra space for an object symbol */
        if (mdat->items[i].role == MI_NORMAL)
            itemwidth += 2;

        /* if the weight is known, leave space to show it */
        if (settings.invweight && mdat->items[i].weight != -1) {
            sprintf(weightstr, " {%d}", mdat->items[i].weight);
            itemwidth += strlen(weightstr);
        }
        if ((mdat->items[i].role == MI_NORMAL && mdat->items[i].accel) ||
            (mdat->items[i].role == MI_HEADING && mdat->items[i].group_accel))
            itemwidth += 4;     /* "a - " or " ')'" */
        maxwidth = max(maxwidth, itemwidth);
    }

    mdat->innerwidth = maxwidth;
    if (mdat->innerwidth > scrwidth - 4)        /* make sure there is space for 
                                                   window borders */
        mdat->innerwidth = scrwidth - 4;
    mdat->width = mdat->innerwidth + 4; /* border + space */

    if (mdat->title && mdat->width < strlen(mdat->title) + 4) {
        mdat->innerwidth = strlen(mdat->title);
        mdat->width = mdat->innerwidth + 4;
    }
}


void
draw_objlist(WINDOW * win, struct nh_objlist *objlist, int *selected, int how)
{
    int i, maxitem, txtattr, width, pos;
    char buf[BUFSZ];

    width = getmaxx(win);
    werase(win);

    /* draw menu items */
    maxitem = min(getmaxy(win), objlist->icount);
    for (i = 0; i < maxitem; i++) {
        struct nh_objitem *const olii = objlist->items + i;

        wmove(win, i, 0);
        pos = 0;
        wattrset(win, 0);
        txtattr = A_NORMAL;

        if (olii->role != MI_NORMAL) {
            if (olii->role == MI_HEADING)
                txtattr = settings.menu_headings;
            wattron(win, txtattr);
            pos += snprintf(buf + pos, BUFSZ - pos, "%s", olii->caption);
            if (olii->group_accel)
                snprintf(buf + pos, BUFSZ - pos, " '%c'", olii->group_accel);
            waddnstr(win, buf, width);
            wattroff(win, txtattr);
            continue;
        }

        if (how != PICK_NONE && selected && olii->accel)
            switch (selected[i]) {
            case -1:
                pos += snprintf(buf + pos, BUFSZ - pos, "%c + ", olii->accel);
                break;
            case 0:
                pos += snprintf(buf + pos, BUFSZ - pos, "%c - ", olii->accel);
                break;
            default:
                pos += snprintf(buf + pos, BUFSZ - pos, "%c # ", olii->accel);
                break;
        } else if (olii->accel)
            pos += snprintf(buf + pos, BUFSZ - pos, "%c - ", olii->accel);

        if (olii->worn)
            txtattr |= A_BOLD;
        switch (olii->buc) {
        case B_CURSED:
            txtattr |= COLOR_PAIR(2);
            break;
        case B_BLESSED:
            txtattr |= COLOR_PAIR(7);
            break;
        default:
            break;
        }
        wattron(win, txtattr);
        pos += snprintf(buf + pos, BUFSZ - pos, "%s", olii->caption);
        if (settings.invweight && olii->weight != -1) {
            pos += snprintf(buf + pos, BUFSZ - pos, " {%d}", olii->weight);
        }
        waddnstr(win, buf, width - 1);
        wattroff(win, txtattr);
    }
    wnoutrefresh(win);
}


static void
draw_objmenu(struct gamewin *gw)
{
    struct win_objmenu *mdat = (struct win_objmenu *)gw->extra;
    int i, scrlheight, scrlpos, scrltop, attr;

    wattron(gw->win, FRAME_ATTRS);
    box(gw->win, 0, 0);
    wattroff(gw->win, FRAME_ATTRS);
    if (mdat->title) {
        wattron(gw->win, A_UNDERLINE);
        mvwaddnstr(gw->win, 1, 2, mdat->title, mdat->width - 4);
        wattroff(gw->win, A_UNDERLINE);
    }

    draw_objlist(gw->win2,
                 &(struct nh_objlist){.icount = mdat->icount - mdat->offset,
                                      .items  = mdat->items  + mdat->offset},
                 mdat->selected + mdat->offset, mdat->how);

    if (mdat->selcount > 0) {
        wmove(gw->win, getmaxy(gw->win) - 1, 1);
        wprintw(gw->win, "Count: %d", mdat->selcount);
    }

    if (mdat->icount <= mdat->innerheight)
        return;

    /* draw scroll bar */
    scrltop = mdat->title ? 3 : 1;
    scrlheight = mdat->innerheight * mdat->innerheight / mdat->icount;
    scrlpos = mdat->offset * (mdat->innerheight - scrlheight) /
        (mdat->icount - mdat->innerheight);
    for (i = 0; i < mdat->innerheight; i++) {
        attr = A_NORMAL;
        if (i >= scrlpos && i < scrlpos + scrlheight)
            attr = A_REVERSE;
        wattron(gw->win, attr);
        mvwaddch(gw->win, i + scrltop, mdat->innerwidth + 2, ' ');
        wattroff(gw->win, attr);
    }
}


static void
resize_objmenu(struct gamewin *gw)
{
    struct win_objmenu *mdat = (struct win_objmenu *)gw->extra;
    int startx, starty;

    layout_objmenu(gw);

    delwin(gw->win2);
    wresize(gw->win, mdat->height, mdat->width);
    gw->win2 =
        derwin(gw->win, mdat->innerheight, mdat->innerwidth,
               mdat->frameheight - 1, 2);

    starty = (LINES - mdat->height) / 2;
    startx = (COLS - mdat->width) / 2;

    mvwin(gw->win, starty, startx);

    draw_objmenu(gw);
}


static void
assign_objmenu_accelerators(struct win_objmenu *mdat)
{
    int i;
    char accel = 'a';

    for (i = 0; i < mdat->icount; i++) {

        if (mdat->items[i].accel || mdat->items[i].role != MI_NORMAL ||
            mdat->items[i].id == 0)
            continue;

        mdat->items[i].accel = accel;

        if (accel == 'z')
            accel = 'A';
        else if (accel == 'Z')
            accel = 'a';
        else
            accel++;
    }
}


static int
find_objaccel(int accel, struct win_objmenu *mdat)
{
    int i, upper;

    /* 
     * scan visible entries first: long list of items (eg the options menu)
     * might re-use accelerators, so that each is only unique among the visible
     * menu items
     */
    upper = min(mdat->icount, mdat->offset + mdat->innerheight);
    for (i = mdat->offset; i < upper; i++)
        if (mdat->items[i].accel == accel)
            return i;

    /* 
     * extra effort: if the list is too long for one page search for the accel
     * among those entries, too
     */
    if (mdat->icount > mdat->innerheight)
        for (i = 0; i < mdat->icount; i++)
            if (mdat->items[i].accel == accel)
                return i;

    return -1;
}


void
curses_display_objects(
    struct nh_objlist *objlist, const char *title, int how,
    int placement_hint, void *callbackarg,
    void (*callback)(const struct nh_objresult *, int, void *))
{
    struct gamewin *gw;
    struct win_objmenu *mdat;
    int i, key, idx, rv, startx, starty, prevcurs, prev_offset;
    nh_bool done, cancelled;
    nh_bool inventory_special = title && !!strstr(title, "Inventory") &&
        how == PICK_NONE;
    int selected[objlist->icount ? objlist->icount : 1];
    struct nh_objresult results[objlist->icount ? objlist->icount : 1];

    /* Make a stack-allocated copy of the objlist, in case we end up longjmping
       out of here before we get a chance to deallocate it. */
    struct nh_objitem item_copy[objlist->icount ? objlist->icount : 1];
    if (objlist->icount)
        memcpy(item_copy, objlist->items, sizeof item_copy);

    memset(selected, 0, sizeof selected);

    if (inventory_special)
        placement_hint = PLHINT_INVENTORY;

    prevcurs = curs_set(0);

    gw = alloc_gamewin(sizeof (struct win_objmenu));
    gw->draw = draw_objmenu;
    gw->resize = resize_objmenu;
    mdat = (struct win_objmenu *)gw->extra;
    mdat->items = item_copy;
    mdat->icount = objlist->icount;
    mdat->title = title;
    mdat->how = how;
    mdat->selcount = -1;
    mdat->selected = selected;

    dealloc_objmenulist(objlist);

    if (how != PICK_NONE)
        assign_objmenu_accelerators(mdat);
    layout_objmenu(gw);

    starty = (LINES - mdat->height) / 2;
    startx = (COLS - mdat->width) / 2;

    if (placement_hint == PLHINT_INVENTORY ||
        placement_hint == PLHINT_CONTAINER) {
        if (ui_flags.draw_sidebar)
            mdat->width = max(mdat->width, 2 + getmaxx(sidebar));
        starty = 0;
        startx = COLS - mdat->width;
    } else if (placement_hint == PLHINT_ONELINER && game_is_running) {
        starty = 0;
        startx = 0;
    }

    gw->win = newwin(mdat->height, mdat->width, starty, startx);
    keypad(gw->win, TRUE);
    wattron(gw->win, FRAME_ATTRS);
    box(gw->win, 0, 0);
    wattroff(gw->win, FRAME_ATTRS);
    gw->win2 =
        derwin(gw->win, mdat->innerheight, mdat->innerwidth,
               mdat->frameheight - 1, 2);
    leaveok(gw->win, TRUE);
    leaveok(gw->win2, TRUE);

    done = FALSE;
    cancelled = FALSE;
    while (!done && !cancelled) {
        draw_objmenu(gw);

        key = nh_wgetch(gw->win);

        switch (key) {
            /* one line up */
        case KEY_UP:
            if (mdat->offset > 0)
                mdat->offset--;
            break;

            /* one line down */
        case KEY_DOWN:
            if (mdat->offset < mdat->icount - mdat->innerheight)
                mdat->offset++;
            break;

            /* page up */
        case KEY_PPAGE:
        case '<':
            mdat->offset -= mdat->innerheight;
            if (mdat->offset < 0)
                mdat->offset = 0;
            break;

            /* page down */
        case KEY_NPAGE:
        case '>':
        case ' ':
            prev_offset = mdat->offset;
            mdat->offset += mdat->innerheight;
            if (mdat->offset > mdat->icount - mdat->innerheight)
                mdat->offset = mdat->icount - mdat->innerheight;
            if (key == ' ' && prev_offset == mdat->offset)
                done = TRUE;
            break;

            /* go to the top */
        case KEY_HOME:
        case '^':
            mdat->offset = 0;
            break;

            /* go to the end */
        case KEY_END:
        case '|':
            mdat->offset = max(mdat->icount - mdat->innerheight, 0);
            break;

            /* cancel */
        case KEY_ESCAPE:
            cancelled = TRUE;
            break;

            /* confirm */
        case KEY_ENTER:
        case '\r':
            done = TRUE;
            break;

            /* select all */
        case '.':
            if (mdat->how == PICK_ANY)
                for (i = 0; i < mdat->icount; i++)
                    if (mdat->items[i].oclass != -1)
                        mdat->selected[i] = -1;
            break;

            /* select none */
        case '-':
            for (i = 0; i < mdat->icount; i++)
                mdat->selected[i] = 0;
            break;

            /* invert all */
        case '@':
            if (mdat->how == PICK_ANY)
                for (i = 0; i < mdat->icount; i++)
                    if (mdat->items[i].oclass != -1)
                        mdat->selected[i] = mdat->selected[i] ? 0 : -1;
            break;

            /* select page */
        case ',':
            if (mdat->how != PICK_ANY)
                break;

            for (i = mdat->offset;
                 i < mdat->icount && i < mdat->offset + mdat->innerheight;
                 i++)
                if (mdat->items[i].oclass != -1)
                    mdat->selected[i] = -1;
            break;

            /* deselect page */
        case '\\':
            for (i = mdat->offset;
                 i < mdat->icount && i < mdat->offset + mdat->innerheight;
                 i++)
                if (mdat->items[i].oclass != -1)
                    mdat->selected[i] = 0;
            break;

            /* invert page */
        case '~':
            if (mdat->how != PICK_ANY)
                break;

            for (i = mdat->offset;
                 i < mdat->icount && i < mdat->offset + mdat->innerheight;
                 i++)
                if (mdat->items[i].oclass != -1)
                    mdat->selected[i] = mdat->selected[i] ? 0 : -1;
            break;

            /* search for a menu item */
        case ':':
            curses_getline("Search:", mdat, objmenu_search_callback);
            break;

            /* edit selection count */
        case KEY_BACKSPACE:
            mdat->selcount /= 10;
            if (mdat->selcount == 0)
                mdat->selcount = -1;    /* -1: select all */
            break;

        default:
            /* selection allows an item count */
            if (key >= '0' && key <= '9') {
                if (mdat->selcount == -1)
                    mdat->selcount = 0;
                mdat->selcount = mdat->selcount * 10 + (key - '0');
                if (mdat->selcount > 0xffff)
                    mdat->selcount /= 10;

                break;
            }

            /* try to find an item for this key and, if one is found, select it 
             */
            idx = find_objaccel(key, mdat);

            if (idx != -1) {    /* valid item accelerator */
                if (mdat->selected[idx])
                    mdat->selected[idx] = 0;
                else
                    mdat->selected[idx] = mdat->selcount;
                mdat->selcount = -1;

                if (mdat->how == PICK_ONE)
                    done = TRUE;

                /* inventory special case: show item actions menu */
                else if (inventory_special)
                    if (do_item_actions(&mdat->items[idx]))
                        done = TRUE;

            } else if (mdat->how == PICK_ANY) { /* maybe it's a group accel? */
                int grouphits = 0;

                for (i = 0; i < mdat->icount; i++) {
                    if (mdat->items[i].group_accel == key &&
                        mdat->items[i].oclass != -1) {
                        if (mdat->selected[i] == mdat->selcount)
                            mdat->selected[i] = 0;
                        else
                            mdat->selected[i] = mdat->selcount;
                        grouphits++;
                    }
                }

                if (grouphits)
                    mdat->selcount = -1;
            }
            break;
        }
    }

    if (cancelled)
        rv = -1;
    else {
        rv = 0;
        for (i = 0; i < mdat->icount; i++) {
            if (mdat->selected[i]) {
                results[rv].id = mdat->items[i].id;
                results[rv].count = mdat->selected[i];
                rv++;
            }
        }
    }

    delete_gamewin(gw);
    redraw_game_windows();
    curs_set(prevcurs);

    callback(results, rv, callbackarg);
}


static nh_bool
do_item_actions(const struct nh_objitem *item)
{
    int ccount = 0, i, selected[1];
    struct nh_cmd_desc *obj_cmd = nh_get_object_commands(&ccount, item->accel);
    char title[QBUFSZ];
    struct nh_menulist menu;
    struct nh_cmd_arg arg;

    if (!obj_cmd || !ccount)
        return FALSE;

    init_menulist(&menu);

    for (i = 0; i < ccount; i++)
        add_menu_item(&menu, i + 1, obj_cmd[i].desc, obj_cmd[i].defkey, FALSE);

    if (settings.invweight && item->weight != -1) {
        snprintf(title, QBUFSZ, "%c - %s {%d}", item->accel, item->caption,
                 item->weight);
    } else {
        snprintf(title, QBUFSZ, "%c - %s", item->accel, item->caption);
    }
    curses_display_menu(&menu, title, PICK_ONE, PLHINT_INVENTORY,
                        selected, curses_menu_callback);

    if (*selected == CURSES_MENU_CANCELLED)
        return FALSE;

    arg.argtype = CMD_ARG_OBJ;
    arg.invlet = item->accel;
    set_next_command(obj_cmd[selected[0] - 1].name, &arg);

    return TRUE;
}
