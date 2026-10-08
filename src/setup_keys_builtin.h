/*
 * The keys for the game's executable, built into the setup so the player is
 * asked only for the disc and the packages (src/setup_keys.h has what each
 * one is). One value per line, as hex; an empty value means it is not built
 * in, and the setup asks the player instead (the Keys page, or
 * dod3 --install --keys <file>). Kept apart so they can be taken out by
 * emptying this file.
 *
 * A 1.01 build needs the four npdrm_* / klic_* values, a 1.00 build the two
 * app_* values; all for key revision 0x1C. The executable they make is
 * checked by its hash, so a wrong digit here fails the install, never the
 * game.
 */
#pragma once

static const struct { const char* id; const char* hex; } k_builtin_keys[] = {
    {"npdrm_erk", "8103ea9db790578219c4cedf0592b43064a7d98b601b6c7bc45108c4047aa80f"},
    {"npdrm_riv", "246f4b8328be6a2d394ede20479247c5"},
    {"klic_free", "72f990788f9cff745725f08e4c128387"},
    {"klic_key", "f2fbca7a75b04edc1390638ccdfdd1ee"},
    {"app_erk", "cff025375ba0079226be01f4a31f346d79f62cfb643ca910e16cf60bd9092752"},
    {"app_riv", "fd40664e2ebba01bf359b0dcdf543da4"},
};
