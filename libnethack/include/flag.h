/* vim:set cin ft=c sw=4 sts=4 ts=8 et ai cino=Ls\:0t0(0 : -*- mode:c;fill-column:80;tab-width:8;c-basic-offset:4;indent-tabs-mode:nil;c-file-style:"k&r" -*-*/
/* Last modified by Sean Hunt, 2013-12-22 */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/* NetHack may be freely redistributed.  See license for details. */

#ifndef FLAG_H
# define FLAG_H

# include "nethack_types.h"
# include "global.h"
# include "coord.h"
# include "objclass.h"
# include "prop.h"

/* Information that is reset every time the user has a chance to give commands
   (from default_turnstate).  This is never saved in a binary save; if it needs
   to be reconstructed, the reconstruction is done via replaying user actions
   from the last command (when it had a known value.) */
struct turnstate {

    /* Multi-turn commands */
    int (*occupation) (void);    /* what the player is currently doing */
    char occupation_txt[BUFSZ];  /* the reason the player is occupied */
    int multi;                   /* negative = helpless, positive = repeating */
    int (*afternmv) (void);      /* what to do after helplessness ends */
    char multi_txt[BUFSZ];       /* the reason the player is helpless */
    int saved_cmd;               /* command being repeated by positive multi */
    struct nh_cmd_arg saved_arg; /* argument for saved_cmd */

    /* Objects that might potentially be destroyed or otherwise changed during
       a turn. */
    struct obj *tracked[ttos_last_slot + 1];
};

extern struct turnstate turnstate;

# ifndef IN_DECL_C
#  define occupation turnstate.occupation
#  define multi      turnstate.multi
#  define afternmv   turnstate.afternmv
# endif

/*
 * Persistent flags that are saved and restored with the game.
 *
 */

struct flag {
    struct nh_autopickup_rules *ap_rules;
    boolean autodig;    /* MRKR: Automatically dig */
    boolean autodigdown;        /* autodigging works downwadrds */
    boolean autoquiver; /* Automatically fill quiver */
    boolean beginner;
    boolean bypasses;   /* bypass flag is set on at least one fobj */
    boolean confirm;    /* confirm before hitting tame monsters */
    boolean debug;      /* in debugging mode */
# define wizard  flags.debug
    boolean explore;    /* in exploration mode */
# define discover flags.explore
    boolean forcefight;
    boolean friday13;   /* it's Friday the 13th */
    boolean legacy;     /* print game entry "story" */
    boolean lit_corridor;       /* show a dark corr as lit if it is in sight */
    boolean made_amulet;
    boolean mon_generation;     /* debug: control monster generaion */
    boolean mon_moving; /* monsters' turn to move */
    boolean mon_polycontrol;    /* debug: control monster polymorphs */
    boolean move;       /* normally 1, unless an action during your turn did
                           NOT use up the move */
    boolean mv;
    boolean nopick;     /* do not pickup objects (as when running) */
    boolean null;       /* OK to send nulls to the terminal */
    boolean pickup;     /* whether you pickup or move and look */
    boolean pickup_thrown;      /* auto-pickup items you threw */
    boolean prayconfirm;        /* confirm before praying */
    boolean pushweapon; /* When wielding, push old weapon into second slot */
    boolean rest_on_space;      /* space means rest */
    boolean safe_dog;   /* give complete protection to the dog */
    boolean showrace;   /* show hero glyph by race rather than by role */
    boolean show_uncursed;      /* always show uncursed items as such */
    boolean silent;     /* whether the bell rings or not */
    boolean sortpack;   /* sorted inventory */
    boolean soundok;    /* ok to tell about sounds heard */
    boolean sparkle;    /* show "resisting" special FX (Scott Bigham) */
    boolean tombstone;  /* print tombstone */
    boolean travel_interrupt;   /* Interrupt travel if there is a hostile *
                                   monster in sight. */
    boolean verbose;    /* max battle info */

    unsigned ident;     /* social security number for each monster */
    unsigned moonphase;
# define NEW_MOON       0
# define FULL_MOON      4
    unsigned no_of_wizards;     /* 0, 1 or 2 (wizard and his shadow) */
    boolean travel;     /* find way automatically to u.tx,u.ty */
    unsigned run;       /* 0: h (etc), 1: H (etc), 2: fh (etc) 3: FH, 4: ff+,
                           5: ff-, 6: FF+, 7: FF- 8: travel */
    int runmode;        /* update screen display during run moves */
    int warnlevel;
    int djinni_count, ghost_count;      /* potion effect tuning */
    int pickup_burden;  /* maximum burden before prompt */
    int recently_broken_otyp;   /* object that broke recently */
    char inv_order[MAXOCLASSES];
# define DISCLOSE_PROMPT_DEFAULT_YES    'y'
# define DISCLOSE_PROMPT_DEFAULT_NO     'n'
# define DISCLOSE_YES_WITHOUT_PROMPT    '+'
# define DISCLOSE_NO_WITHOUT_PROMPT     '-'
    char end_disclose;  /* disclose various info upon exit */
    char menu_style;    /* User interface style setting */

    /* birth option flags */
    boolean elbereth_enabled;   /* should the E-word repel monsters? */
    boolean rogue_enabled;      /* create a rogue level */
    boolean seduce_enabled;     /* succubus sduction */
    boolean bones_enabled;      /* allow loading bones levels */
    boolean permablind; /* stay permanently blind */
    boolean permahallu; /* stay permanently hallucinating */
};


/*
 * Flags that are set each time the game is started.
 * These are not saved with the game.
 *
 */
struct instance_flags {
    boolean vision_inited;      /* true if vision is ready */
    boolean travel1;    /* first travel step */
    coord travelcc;     /* coordinates for travel_cache */
    boolean next_msg_nonblocking;       /* suppress a --More-- after this
                                           message */

    /* Items which belong in flags, but are here to allow save compatibility */
    boolean disable_log;        /* don't append anything to the logfile */
    boolean botl;       /* redo status line */
    boolean autoexplore;        /* currently autoexploring */
};

extern struct flag flags;
extern struct instance_flags iflags;

/* runmode options */
# define RUN_TPORT      0       /* don't update display until movement stops */
# define RUN_LEAP       1       /* update display every 7 steps */
# define RUN_STEP       2       /* update display every single step */
# define RUN_CRAWL      3       /* walk w/ extra delay after each update */

#endif /* FLAG_H */
