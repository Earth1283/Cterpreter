#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOMS 8
#define ITEMS 4
#define NOWHERE (-1)

enum { SEMICOLON, CAST, DANGLING, RETURN_ZERO };

struct Room {
    const char *name;
    const char *description;
    int exits[4];
};

static const char *const directions[] = {"north", "south", "east", "west"};
static const char *const item_names[] = {"semicolon", "cast", "pointer", "return"};
static const char *const item_descriptions[] = {
    "a warm, well-used semicolon",
    "a parenthesised cast, type unspecified",
    "a dangling pointer, still twitching",
    "the literal statement `return 0;`"
};

static const struct Room rooms[ROOMS] = {
    {"The Prompt", "A blinking cursor. Everything begins and ends here.", {1, NOWHERE, NOWHERE, NOWHERE}},
    {"The Lexer", "Tokens drift past like plankton. One of them looks detachable.", {NOWHERE, 0, 2, NOWHERE}},
    {"The Parser", "A forest of syntax trees. The canopy expects something.", {3, NOWHERE, NOWHERE, 1}},
    {"The Type Registry", "Shelves of interned types, refcounts ticking quietly.", {NOWHERE, 2, 4, NOWHERE}},
    {"The Heap", "Allocations sprawl in every direction. Something here was freed twice.", {5, NOWHERE, NOWHERE, 3}},
    {"The Stack", "Frames stacked to the ceiling. They wobble when you breathe.", {NOWHERE, 4, 6, NOWHERE}},
    {"Undefined Behaviour", "You cannot describe this room. Neither can the standard.", {7, NOWHERE, NOWHERE, 5}},
    {"main()", "The entry point. A door marked EXIT STATUS stands open.", {NOWHERE, 6, NOWHERE, NOWHERE}}
};

static int here = 0;
static int carried[ITEMS];
static int located[ITEMS] = {1, 3, 4, 7};
static int moves = 0;
static int playing = 1;

static int holding(int item) { return carried[item]; }

static void describe(void) {
    printf("\n== %s ==\n%s\n", rooms[here].name, rooms[here].description);
    for (int item = 0; item < ITEMS; item++)
        if (located[item] == here) printf("You can see %s.\n", item_descriptions[item]);
    printf("Exits:");
    int any = 0;
    for (int d = 0; d < 4; d++)
        if (rooms[here].exits[d] != NOWHERE) { printf(" %s", directions[d]); any = 1; }
    printf("%s\n", any ? "" : " none, which is worrying");
}

static int find_item(const char *word) {
    if (!word) return NOWHERE;
    for (int item = 0; item < ITEMS; item++)
        if (!strcmp(word, item_names[item])) return item;
    return NOWHERE;
}

static int do_look(const char *rest) { (void)rest; describe(); return 0; }

static int do_go(const char *rest) {
    int direction = NOWHERE;
    for (int d = 0; d < 4; d++)
        if (rest && (!strcmp(rest, directions[d]) || (strlen(rest) == 1 && rest[0] == directions[d][0])))
            direction = d;
    if (direction == NOWHERE) { printf("Go where?\n"); return 0; }

    int target = rooms[here].exits[direction];
    if (target == NOWHERE) { printf("There is no way %s.\n", directions[direction]); return 0; }
    if (here == 2 && !holding(SEMICOLON)) { printf("The parser refuses you: expected ';' before end of room.\n"); return 0; }
    if (target == 6 && !holding(CAST)) {
        printf("You step into Undefined Behaviour unprotected.\nThe interpreter shrugs. So does the standard.\n");
        playing = 0;
        return 1;
    }
    here = target;
    moves++;
    describe();
    return 0;
}

static int do_take(const char *rest) {
    int item = find_item(rest);
    if (item == NOWHERE || located[item] != here) { printf("There is no such thing here.\n"); return 0; }
    if (item == DANGLING) { printf("You grab the dangling pointer. It dereferences you back. Ouch.\n"); return 0; }
    located[item] = NOWHERE;
    carried[item] = 1;
    printf("Taken: %s.\n", item_descriptions[item]);
    return 0;
}

static int do_drop(const char *rest) {
    int item = find_item(rest);
    if (item == NOWHERE || !holding(item)) { printf("You are not carrying that.\n"); return 0; }
    carried[item] = 0;
    located[item] = here;
    printf("Dropped.\n");
    return 0;
}

static int do_inventory(const char *rest) {
    (void)rest;
    int carrying = 0;
    for (int item = 0; item < ITEMS; item++)
        if (carried[item]) { printf("  %s\n", item_descriptions[item]); carrying++; }
    if (!carrying) printf("You are carrying nothing but expectations.\n");
    return 0;
}

static int do_use(const char *rest) {
    int item = find_item(rest);
    if (item == NOWHERE || !holding(item)) { printf("You are not carrying that.\n"); return 0; }
    if (item == RETURN_ZERO && here == 7) {
        printf("\nYou execute `return 0;`.\nThe interpreter unwinds, the prompt returns, and you are free.\n");
        printf("Escaped in %d moves.\n", moves);
        playing = 0;
        return 1;
    }
    if (item == CAST) { printf("You brandish the cast. Reality narrows obligingly.\n"); return 0; }
    printf("Nothing happens. This is C; that is often the best outcome.\n");
    return 0;
}

static int do_help(const char *rest) {
    (void)rest;
    printf("Verbs: look, go <direction>, take <item>, drop <item>, inventory, use <item>, help, quit\n");
    printf("Directions may be abbreviated to n, s, e, w.\n");
    return 0;
}

static int do_quit(const char *rest) {
    (void)rest;
    printf("You return to the prompt the boring way.\n");
    playing = 0;
    return 1;
}

struct Verb {
    const char *word;
    int (*run)(const char *rest);
};

static const struct Verb verbs[] = {
    {"look", do_look}, {"l", do_look}, {"go", do_go},
    {"north", do_go}, {"south", do_go}, {"east", do_go}, {"west", do_go},
    {"n", do_go}, {"s", do_go}, {"e", do_go}, {"w", do_go},
    {"take", do_take}, {"get", do_take}, {"drop", do_drop},
    {"inventory", do_inventory}, {"i", do_inventory},
    {"use", do_use}, {"help", do_help}, {"quit", do_quit}, {"q", do_quit}
};

int main(void) {
    char line[128];
    printf("You wake up inside a C interpreter.\nThis has happened to better programs.\nType `help` if you need it.\n");
    describe();

    while (playing) {
        printf("\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\nStandard input ended. So does the adventure.\n"); break; }

        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        if (!length) continue;

        char *verb = strtok(line, " \t");
        char *rest = strtok(NULL, " \t");
        int handled = 0;
        for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++)
            if (!strcmp(verb, verbs[i].word)) {
                const char *argument = rest;
                if (verbs[i].run == do_go && !rest) argument = verb;
                verbs[i].run(argument);
                handled = 1;
                break;
            }
        if (!handled) printf("The parser does not recognise `%s`.\n", verb);
    }
    return 0;
}
