/* The engine's graphics switches (src/patches/sysset.cpp): the rows of the
 * Advanced Graphics page, the poll that applies them at start-up, and the
 * hook behind the tree detail row. */
#pragma once
#include <utility>
#include <vector>

/* The in-game Settings pages a row is on. */
enum { PAGE_GRAPHICS, PAGE_SYSTEM, PAGE_ADVANCED, PAGE_PICTURE };

struct Dod3RowSpec {
    const char* key;      /* the dod3.ini key */
    const char* label;
    const char* desc;     /* about 60 characters at most */
    std::vector<std::pair<const char*, const char*>> choices;   /* value, text; NULL value = key unset */
    int def = 0;          /* the default choice */
    bool live = false;    /* applied at once; otherwise at the next start */
    void (*apply)(const char* value) = nullptr;   /* a live row's own way of applying it */
    int page = PAGE_ADVANCED;
};

/* The Advanced Graphics page's rows. */
void dod3_sysset_rows(std::vector<Dod3RowSpec>& rows);

/* Every Settings page's rows, in order (src/ui/settings_menu.cpp): the
 * in-game pages' and the launcher's. Making the list changes nothing. */
void dod3_setting_rows(std::vector<Dod3RowSpec>& rows);

/* From the vblank loop, often: once the title has read its INI (and before
 * the engine object makes its render targets from it), the dod3.ini keys
 * of the rows are written over it; also the DOD3_SYSSET* testing hooks. */
void dod3_sysset_poll();

/* Once at boot, after the lifted function table is registered. */
void dod3_sysset_install();
