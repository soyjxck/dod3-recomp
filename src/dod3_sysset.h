/* The engine's graphics switches (src/dod3_sysset.cpp): the rows of the
 * Advanced Graphics page, and the poll that applies them at start-up. */
#pragma once
#include <utility>
#include <vector>

struct Dod3RowSpec {
    const char* key;      /* the dod3.ini key */
    const char* label;
    const char* desc;     /* about 60 characters at most */
    std::vector<std::pair<const char*, const char*>> choices;   /* value, text; NULL value = key unset */
    int def;
    bool live;            /* applied at once; otherwise at the next start */
    void (*apply)(const char* value);
};

/* The Advanced Graphics page's rows. */
void dod3_sysset_rows(std::vector<Dod3RowSpec>& rows);

/* From the vblank loop, often: once the title has read its INI (and before
 * the engine object makes its render targets from it), the dod3.ini keys
 * of the rows are written over it; also the DOD3_SYSSET* testing hooks. */
void dod3_sysset_poll();
