# Drakengard 3 Recompiled -- playing it

A native PC port of Drakengard 3 (PlayStation 3), made by recompiling the
game's own code. It does not include the game: you need your own copy, and
the setup takes the game's files from it.

This project is not affiliated with or endorsed by Square Enix, Access Games
or Sony. Drakengard 3 and its assets belong to their owners.


## What you need

1. A computer:

   - Windows 10 or 11, 64-bit, a GPU with Direct3D 12, and a CPU with AVX2
     (Intel Haswell / AMD Zen or newer).
   - Or a Mac with Apple silicon (M1 or newer) on macOS 13 Ventura or newer.
   - Or, experimentally, Linux on x86-64 with a CPU with AVX2, a Vulkan 1.3
     driver (Mesa's RADV or ANV, or NVIDIA's), SDL2 and glibc 2.38 or newer
     -- SteamOS on a Steam Deck has all of it.

   About 16 GB of free disk space for the game, plus 10 GB more if you
   install the Japanese voice pack and about 3 GB for the Spanish
   translation. The install folder should be on a
   drive that takes links between folders (NTFS or APFS, not exFAT).

2. Your copy of Drakengard 3, US release, as one of:

   - the disc (BLUS31197) dumped to a folder (the one that contains
     PS3_GAME),
   - a DECRYPTED disc image (.iso), or
   - the PSN release's game package (NPUB31251): the .pkg file a
     PlayStation 3 downloads for it, UP0082-NPUB31251_00-DOD3MAINGAME0000
     (15.6 GB). No licence files are needed.

   The easiest way is PS3 Disc Dumper (https://github.com/13xforever/ps3-disc-dumper,
   free, for Windows, macOS and Linux) with a compatible Blu-ray drive: it
   reads your disc and writes a decrypted folder, which the setup takes as
   it is. A modded PS3 (multiMAN, webMAN MOD) can also copy the disc to a
   decrypted folder or image. An image ripped with an ordinary disc tool is
   still encrypted and will be refused, and so are other regions (EU, JP),
   the European PSN release among them.

3. The game's 1.01 update, as the package file a PlayStation 3 downloads:
   UP0082-BLUS31197_00-DOD3PATCH0000000-A0101-V0101-PE.pkg (172 MB). The
   port runs the updated game, so the update is required. The setup checks
   it is the genuine 1.01 package. This is the disc release's update, and
   the PSN release takes it too; the PSN release's own update is not
   needed.

4. Optionally, the DLC packages you own (the .pkg files your PlayStation 3
   downloads for them, UP0082-NPUB31251_00-...): the Japanese Voice Pack,
   the six prologue chapters, the garbs and weapons, the music packs. No
   licence files are needed.

You do not need a PlayStation 3 emulator, firmware, or any other tool: the
setup unpacks the packages and prepares the game's executable itself.


## Installing

Windows:

1. Unzip the release anywhere you like (not inside Program Files).
2. Run dod3.exe. Windows may show a SmartScreen warning the first time,
   because the program is not signed: choose "More info", then "Run anyway".
3. The setup opens. Add your files -- the disc image or folder (or the PSN
   game package), the update package, and any DLC packages -- with Add
   Files / Add Folder; each one is
   recognised by itself, in any order. Then Install. The game's files are
   copied next to dod3.exe into the "game" folder (a few minutes).
4. Play. The graphics shaders come compiled, in the "cache" folder, and
   the game prepares them for your graphics card in the background at
   start-up; a scene they miss may stutter the first time only. From then
   on dod3.exe opens the launcher (below).

macOS:

1. Unzip the release and move "Drakengard 3 Recompiled.app" to
   Applications (or anywhere you like).
2. The first time, macOS refuses to open it, because the app is not
   notarised by Apple. On macOS 15 and later: try to open it once, then go
   to System Settings > Privacy & Security and choose "Open Anyway". On
   macOS 13 and 14: Control-click the app, choose Open, then Open again.
   After that it opens normally.
3. The setup opens, as on Windows. The game's files go into
   ~/Library/Application Support/Drakengard 3 Recompiled.

Linux (experimental):

1. Unpack the release anywhere you can write to (not under /usr or /opt).
2. Run dod3 in that folder. The setup opens, as on Windows, and the game's
   files go into the game folder beside dod3. Add Files and Add Folder use
   the desktop's own picker (kdialog or zenity); with neither,
   `./dod3 --install <disc or game .pkg> <update .pkg> [DLC .pkg ...]` in
   a terminal installs the same way. If the folder cannot be written to,
   everything goes into ~/.local/share/dod3-recomp instead.
3. Steam Deck: do the setup in Desktop Mode, then in Steam, Add a Game >
   Add a Non-Steam Game, and pick dod3, to play it in Game Mode with the
   Deck's own controls. Started outside Steam on a Deck (Desktop Mode, from
   the file manager or a terminal), the game reads the Deck's controls as
   the pad only and leaves the keyboard and mouse alone: Steam's desktop
   layout turns the Deck's buttons into keys there, and every press would
   arrive twice (one of them as Esc, which opens the pause menu). With a
   keyboard of your own plugged in, start it with DOD3_KEYBOARD=1.

## The launcher

Once the game is installed, it opens on a launcher, over the game's own key
art (from your copy of the game):

- Play (Enter, or the pad's confirm button, starts it straight away)
- Mods: the Spanish translation and the DLC Undub, what of them is
  installed, a switch to turn one off and on again, Remove, and Add Files /
  Add Folder for more (see Mods below)
- DLC: which of the 20 packs are installed, and Add DLC Packages
- Settings: the display, graphics, controls (the keyboard's keys too),
  system settings and cheats, saved to dod3.ini as you change them
- Trophies: the game's trophies, what you have earned and when (see
  Trophies below)
- Game Files, at the bottom: the setup, to install the disc or the update
  again
- Update to ..., at the bottom (Windows), once a newer release is out: its
  notes, and Install Update (see Your saves, and updating)

It works with the mouse, the keyboard and a controller (Esc or the pad's
back button closes a page). To start the game without it, set Settings >
System > Launcher at Start to Off; holding Shift (Option on a Mac) while
starting the game opens it anyway (on Linux, start it with DOD3_LAUNCHER=1
in the environment; in Steam, as the launch options
`DOD3_LAUNCHER=1 %command%`).

To run the setup again -- to replace a file -- use Game Files in the
launcher, or run dod3.exe --setup from a command prompt on Windows
(./dod3 --setup on Linux).
Everything already installed stays.

If the setup cannot open its window, it can run from a command prompt:

    dod3.exe --install <disc .iso or folder> <update .pkg> [DLC .pkg ...]

and `dod3.exe --check <file>...` says what the setup makes of each file
without installing anything.


## Settings

In the launcher: Settings, in five tabs (Display, Graphics, Controls,
System, Cheats), sets the settings below and saves them to dod3.ini as you change
them; the game uses them from its next start.

In the game: Settings > Graphics Settings (on the title menu, next to
Audio Settings) sets the resolution, display mode, frame rate, v-sync,
anti-aliasing (FXAA, or MSAA 2x/4x/8x), texture filtering and field of
view. Settings > System Settings sets whether the logos and opening movie
are skipped, what the game does while its window is in the background
(keep running, mute, or pause), which controller's buttons the game shows,
the mouse's sensitivity, the renderer and the blood on the screen.
Settings > Advanced Graphics sets the aspect ratio and the engine's own
shadow quality, motion blur, post-processing, depth of field, draw distance
and tree detail. Settings > Picture Settings sets the post-processing
quality and HDR (Windows and macOS). Confirm with X to save them; everything but the renderer, Skip
Intro, Shadow Quality and Post-Processing Quality changes at once (a second
of black while the resolution or display mode switches; Tree Detail in the
next area the game loads), those four the next time the game starts.

The first start adds the pages to the game's menu, which takes a second or
two. The game's own files are not changed.

Or edit dod3.ini (next to dod3.exe; on a Mac, in the Application Support
folder above). It explains each setting:

| Setting | What it does |
|---|---|
| RSX_SCALE | Internal resolution as a multiple of 1280x720: 1.5 = 1080p, 2 = 1440p, 3 = 4K. Above the window's size it is averaged down: supersampling |
| RSX_DISPLAY | windowed, borderless (recommended for full screen) or fullscreen |
| RSX_WINDOW | Window size in windowed mode, e.g. 1920x1080 |
| RSX_VSYNC | 1 to sync to the display; 0 lets the frame rate exceed its refresh |
| DOD3_FPS | Frame-rate cap: 60 is tested and recommended, 120 works; adaptive holds the highest rate from 120 down to 30 that the scene keeps up with; unset keeps the original 30 |
| DOD3_FOV | Field of view: 5 to 30 degrees added to the gameplay camera's 65; cutscenes keep their framing. Unset is the original |
| DOD3_ASPECT | Aspect ratio, your screen's shape: 16:10, 21:9, 32:9, a ratio such as 2.39, or auto (the window's). Wider shows more at the sides in play and cutscenes, 16:10 more above and below; the HUD keeps its 16:9 layout in the middle, and the title, menus and movies are 16:9 in the middle of the screen. A screen of another shape shows it with bars (a 16:9 screen set to 21:9 shows what a 21:9 one would). A wider view is 1280 game pixels wide, so raise RSX_SCALE to keep it sharp. Unset is 16:9 |
| RSX_ANISO | Texture filtering: 1 (original) to 16 (16x anisotropic, the default) |
| RSX_AA | fxaa, or msaa2 / msaa4 / msaa8 (MSAA costs a lot at 4K); unset is off |
| DOD3_SHADOWS | high or ultra: sharper, more defined shadows (ultra costs a few percent); unset is the original. From the next start |
| DOD3_MOTION_BLUR | 0 turns motion blur off |
| DOD3_POSTFX | 0 turns off depth of field, bloom and the colour grading |
| DOD3_DOF | 0 turns off only the depth of field, the blur of what is near and far; the bloom and the colour grading stay |
| RSX_HDR | Windows (Direct3D 12 or Vulkan): 1 shows the game in HDR on a display that has HDR on in Windows (Settings > Display > Use HDR): the picture at the brightness below, with its shadows as an SDR display shows them (Windows' own conversion of an SDR game washes them out), and the light the game clips at white -- the sky, fire, glints on blades -- brighter than white, up to the display's peak. The HUD and menus stay at the brightness below. The bright parts come from the next start when HDR is switched on in the game. macOS (Metal): 1 shows it in HDR on a display with EDR headroom (a MacBook Pro's XDR display; an external HDR display with HDR on), white at the system's SDR white; the extra brightness follows the brightness setting, and macOS takes it back while the window is covered. Unset is SDR |
| RSX_HDR_HIGHLIGHTS | 0 keeps HDR's picture within white: no brighter highlights |
| RSX_HDR_HUE | Windows: how much of its own colour the brightest light (fire, lamps, the sun) keeps in HDR, 0 to 1. 1 keeps all of it; 0 whitens it as the SDR picture does. Unset is 0.5 |
| RSX_HDR_PEAK | Windows: the brightest the display shows, in nits, which the highlights roll off toward. Unset is what the display reports |
| RSX_HDR_WHITE | The brightness of white in HDR, in nits (100 to 400). Windows: unset is 203 nits, where HDR games put it; `windows` follows Windows' SDR content brightness slider. On a Mac, unset is the system's SDR white, which a setting counts as 100 nits |
| DOD3_POSTFX_QUALITY | high: above 1280x720, the depth of field and bloom are drawn smoothly, and what is out of focus stays soft instead of showing sharp detail through the blur (the port's own version of the game's post-processing shader). The same picture as Original at 1280x720. Unset is the original. From the next start |
| DOD3_SCREEN_BLOOD | Blood over the screen in combat: border leaves out the splashes and keeps the blood around the edges at low health, a warning; 0 leaves out both. Unset is the original |
| DOD3_DRAW_DISTANCE | 2, 3, 5 or 10: props, bushes and small trees are drawn from that many times as far away; unset is the original |
| DOD3_TREE_LOD | 2, 3, 5 or 10: the large trees are drawn, and stay their full models, that many times as far away; unset is the original. From the next area loaded |
| DOD3_SKIP_INTRO | 1 (default) skips the logos and the opening movie at start-up; 0 keeps them |
| DOD3_LAUNCHER | 0 starts the game straight away, without the launcher; holding Shift (Option on a Mac) while it starts opens the launcher anyway. Unset opens it at every start |
| DOD3_UPDATE_CHECK | Windows: 0 stops the launcher asking GitHub for a newer release when it opens. In the launcher: Settings > System > Check for Updates. Unset asks |
| DOD3_UNFOCUSED | While the window is in the background: unset keeps running, mute silences the audio, pause stops the game until you come back |
| DOD3_SONG_AUTOPLAY | 1 plays the song of branch D's final mission for you: every note is pressed on its time (the RPCS3 patch "Auto Play Song of The D Branch Final Mission", by bucanero). In the launcher: Settings > Cheats > Final Song Auto-Play. Unset is off |
| DOD3_TROPHY_POPUPS | 0 leaves out the pop-up at the top right of the game as you earn a trophy (Windows, Direct3D 12 for now). The trophies are kept either way, in gamedata/trophies, and listed on the launcher's Trophies page. In the launcher: Settings > System > Trophy Pop-ups. Unset shows them |
| DOD3_CHEAT_HEALTH | 1 keeps your health full: Zero's, or the dragon's in the sky. Enemies are hurt as usual. In the launcher: Settings > Cheats > Infinite Health. From the next start; unset is off |
| DOD3_CHEAT_INTONER | 1 fills the Intoner gauge at the first blood and keeps it full, in Intoner mode too. Settings > Cheats > Full Intoner Gauge |
| DOD3_CHEAT_ITEMS | 1: items are not used up. Settings > Cheats > Infinite Items |
| DOD3_CHEAT_TIME | 1: a mission's time limit does not count down. Settings > Cheats > Frozen Time Limits |
| DOD3_PROMPTS | The buttons the game shows: unset follows the controller you hold; or ps3, ps4, ps5, xbox, switch, keyboard |
| DOD3_MOUSE_SENS | How far the mouse turns the camera; 1 is the default. DOD3_MOUSE = 0 turns the mouse off |
| KEY_CROSS, KEY_MOVE_UP, ... | Windows: the keys for each pad button and stick direction (the file lists them all) |
| DOD3_CAMERA_SPEED | original gives the camera stick the game's own speed, which falls as the frame rate rises; unset keeps it at the console's |
| RSX_BACKEND | Windows: vulkan for the Vulkan renderer (experimental); unset is Direct3D 12 |


## HDR

On a display that can show HDR, the game can use it: Settings > Picture
Settings > HDR in the game, or `RSX_HDR = 1` in dod3.ini.

- Windows: turn HDR on for the display in Windows first (Settings > Display
  > Use HDR). Direct3D 12 and Vulkan both do it.
- macOS: a display with EDR headroom, such as a MacBook Pro's XDR display,
  a Pro Display XDR, or an external HDR display with HDR on.
- Linux: not yet.

The game draws its scene with more light than an SDR screen shows and cuts
it off at white. In HDR that light is kept: fire, lamps, embers and glints
on blades go on above white, up to your display's peak, rolled off smoothly
instead of cut, with the game's own colour grading. Everything up to about
three quarters of white is the SDR picture itself -- the same colours and
the same dark shadows -- at the brightness HDR Brightness sets for white.
The HUD, menus, movies and pop-ups stay at that white.

- HDR Brightness: on Windows the default is 203 nits, where HDR games put
  white; As Windows takes Windows' SDR content brightness instead (on a Mac,
  Auto is the system's normal white). A lower white leaves the highlights
  more room above it.
- HDR Peak Brightness (Windows): Auto is what the display reports; set it
  lower if the brightest parts lose detail, as on a display that reports
  more than it shows.
- HDR Highlight Colour (Windows): how much of its own colour the brightest
  light keeps. Balanced is the default; Full keeps all of it (deep orange
  fire), As SDR whitens it as the SDR picture does.
- Switching HDR on in the game changes the picture at once; the brighter
  highlights need HDR on when the game starts, so they come at the next
  start. `RSX_HDR_HIGHLIGHTS = 0` keeps the picture within white.
- On a Mac the room above white follows the brightness setting, and macOS
  takes it back while the game's window is covered.


## Trophies

The game's 72 trophies are earned as on the console, read from your copy's
own trophy list, and kept in gamedata/trophies (NPWR04682.json) with your
saves: back that folder up with them. Trophies earned with earlier versions
of the port carry over. Nothing goes to PSN or anywhere else.

- In the game: a pop-up at the top right as you earn one, with its icon,
  name and grade (a platinum follows the trophy that completes it). Windows
  with Direct3D 12 for now; Settings > System > Trophy Pop-ups in the
  launcher turns them off.
- In the launcher: Trophies lists them all with the date you earned each, by
  grade, filtered by earned or not; hidden trophies keep their secret until
  earned (Show Hidden reveals them).


## Controls

Xbox, PlayStation (DualShock 4, DualSense) and Switch Pro controllers,
wired or over Bluetooth, plugged in before or during the game; the first
one connected is player 1. The buttons follow their positions, so the
bottom face button is Cross whatever the pad prints on it.

The game shows the buttons of the controller you hold: A and B on an Xbox
pad, B and A on a Switch one, the DualShock 4's or the DualSense's own, the
keys when you play on the keyboard. It changes when you plug another pad in
or pick up the other of pad and keyboard;
Settings > System Settings > Button Prompts fixes it to one (PlayStation 3
is the game's original art). A few places still name the PlayStation 3
pad whatever you choose: "Press the START Button" on the title screen,
"Press START to Skip" in cutscenes, the Circle on the dragon's icon, and
the picture on the controller settings page.

The camera stick turns the camera at the speed it has on the console
whatever the frame rate. The game itself slows it as the frame rate rises
(a quarter of the speed at 120 frames a second); DOD3_CAMERA_SPEED =
original in dod3.ini gives that back. The game's own Controller Settings
(camera sensitivity, inverted axes, the chase camera) work as before.

### Keyboard and mouse

The mouse turns the camera and the keys are the pad's buttons, with or
without a controller plugged in: use either at any moment, and the prompts
on screen follow the one you touched last.

| Key | Button | In play |
|---|---|---|
| W A S D | Left stick | Move |
| Mouse | Right stick | Camera |
| Left mouse button | Square | Attack |
| Right mouse button | Triangle | Strong attack |
| Space | Cross | Jump; confirm in menus |
| E, Backspace | Circle | Guard; back in menus |
| Shift | R1 | Evade |
| Q, middle mouse button | L1 | Lock on |
| Mouse wheel | Right stick left and right | Switch target |
| R, F | L2, R2 | Weapon menus |
| V | L3 and R3 together | Intoner Mode (C and X press one each) |
| Arrow keys, 1 2 3 4 | D-pad | Menus; items |
| Esc, Enter | Start | Pause |
| Tab | Select | The select menu |

Every key can be changed in dod3.ini, which lists them, and the prompts
show the keys you chose. Settings > System Settings > Mouse Sensitivity
sets how far the mouse turns the camera; the game's own camera settings
apply to the mouse as they do to the stick. Escape pauses, so it no longer
closes the game: Alt+F4 does, or Cmd+Q on a Mac.

On the dragon in the sky, the mouse moves the aiming cursor (the red ring
of the lock-on) and the cursor stays where you leave it; moving the mouse
past the edge of the screen turns the view. Hold the left mouse button and
sweep the cursor over enemies to lock on to them, then let go to fire at
all of them. W A S D move the cursor too, as the stick does on a pad.

While the game is in front the cursor is hidden and the mouse is the
game's; switch to another application (Alt+Tab, Cmd+Tab on a Mac) and it
is yours again. On a Mac the keys are places on the keyboard, the same on
every layout; Alt is the Option key, and CapsLock cannot be used. On Linux
too the keys are places on the keyboard, whatever the layout.


## Japanese voices

With the Japanese Voice Pack installed: on the title menu, Change
Outfits/Music > Voice > Language: Japanese, then start or continue a game.
Voices, lip-sync and the cutscene movies switch to Japanese.

The official pack has no Japanese for the six DLC chapters (the prologues),
and the 1.01 update switches the voices to English while one is played.
With the DLC Undub installed (see Mods below), the DLC chapters follow the
Language setting too: Japanese voices, lip-sync and movies there as well,
and English when English is chosen.


## Mods

The launcher installs two fan-made mods by name, on its Mods page. Unzip
or unpack what you downloaded first, then add the files with Add Files or
Add Folder. Your own game files stay as they are: a mod's files go to the
game/mods folder (and the voice pack's folder, for the DLC Undub).

- The Spanish translation by Traducciones Vacuno (TraduSquare, v1.1, for
  the US release BLUS31197): from its downloads, the
  Drakengard_ES_BLUS31197_V2.pkg and Drakengard_ES_BLUS31197DATA_V3.pkg
  packages and MOVIES.zip (the US game's download), and if you have the
  DLC, Drakengard_DLC_ES_BLUS31197_NPUB31251_V2.pkg and, with the Japanese
  Voice Pack, Drakengard_ES_DLC_Voices_BLUS31197_NPUB31251_V2.pkg. Add
  them as they are, the movies' .zip too; do not copy anything into the
  game's folders by hand. The port's own Settings pages are there with it,
  in Spanish too (Ajustes gráficos, Ajustes del sistema, Gráficos
  avanzados); switched off, the game and the pages are in English again.

- The DLC Undub by Canzah and Slow (Japanese voices for the DLC chapters):
  unpack its .7z and add the folder that holds GAME and ISO with Add
  Folder. It needs the Japanese Voice Pack. It follows Change Outfits/Music
  > Voice > Language, as the pack does; the 1.01 update's executable needs
  no swapping here.

With both, the DLC chapters' text is the translation's in Japanese too,
where the translation has its own (the undub's is English text timed for
the Japanese voices).

Other mods made as homebrew packages for this release, laid out as the
console installs them (in the update's folder, the game-data folder, or
the DLC's), are taken too and listed by their file names.

To play without a mod for a while -- the game in English again, say --
turn the switch on its card in the launcher's Mods page to Off: its files
are set aside and the update's own put back, at once, and the game starts
without it. On puts it back, again at once. The DLC Undub has no switch:
Change Outfits/Music > Voice turns it on and off inside the game.

To take a mod out: Remove on its card in the launcher's Mods page. The
update's own files that a mod replaced are kept aside while it is
installed and put back. (A mod installed by an older version of the setup
has no such list: removing it asks for the update package, which puts the
update's files back, and takes out all the mods; it has no switch either.)

A mod copied into the update's folder by hand
(game/disc/game/BLES00000/USRDIR/PATCH next to dod3.exe) is played as it
is. If it replaces the game's script package (SQEX03GAME.XXX), the port's
additions to the Settings menu are not there, because they are made to
that package: the Graphics Settings, System Settings, Advanced Graphics and
Picture Settings pages, and what works through them (the wider field of view, the Aspect
Ratio setting's wider view, skipping the intro). dod3.ini still sets the
resolution, the display mode, the frame rate and the anti-aliasing.
Running the setup again with the update package puts the update's own
files back.


## Your saves, and updating

Saves, trophies and the game's own data are in the "gamedata" folder next
to dod3.exe (on a Mac, in the Application Support folder; on Linux, next to
dod3). Back that folder up to keep your progress.

On Windows the launcher looks for a newer release on GitHub each time it
opens (Settings > System > Check for Updates turns that off). When there is
one, "Update to ..." shows at the bottom: it opens the release's notes, and
Install Update downloads it, checks it against the SHA-256 GitHub lists for
it, puts its files in place of the old ones and starts the new version. Your
game, gamedata and dod3.ini are left as they are; if a file cannot be
replaced (another program has it open), every file is put back as it was.

To update by hand, unzip the new release into a new folder and move these
into it from the old one: game (the installed game), gamedata (saves) and
dod3.ini (your settings). Leave cache behind: a Windows release brings its
own compiled shaders, newer than the old folder's. Unzipping it over
the old folder works too, but the release's dod3.ini (the default
settings) then replaces yours: keep a copy and put it back. To uninstall,
delete the folder.


## If something goes wrong

- Each run writes dod3.log next to dod3.exe (the run before is kept as
  dod3.prev.log; on a Mac both are in the Application Support folder; on
  Linux next to dod3, or printed to the terminal when started from one).
  Please include it when you report a problem, with your GPU and CPU.
- The setup refuses a file it cannot use and says why: an encrypted disc
  image, another region or version, a package for another game, a damaged
  package (its checksum fails). `dod3.exe --check <file>` shows the same.
- "The game's executable could not be loaded": run dod3.exe --setup and add
  the update package again.


## Known issues

- Tested on few machines so far (Windows: i9-13900K with an RTX 4090;
  macOS: M1 Pro; Linux: a Steam Deck, in Desktop Mode). Reports from other
  hardware are welcome.
- Trophy pop-ups show on Windows with Direct3D 12 only so far; on Vulkan,
  macOS and Linux the trophies are earned and listed in the launcher without
  the pop-up.
- In HDR the brighter highlights come from the next start after HDR is
  switched on in the game.
- At an aspect ratio other than 16:9 (21:9, 32:9, 16:10) the HUD keeps its
  16:9 layout in the middle of the screen, and some of it can still be out
  of place: the pause screen's frosted background stays 16:9, and not every
  menu and effect has been checked at those shapes.
