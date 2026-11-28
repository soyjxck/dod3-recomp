# Translating the launcher and the setup

The launcher (Play, Mods, DLC, Settings, Trophies) and the setup that installs
the game can speak any language that is written with Latin, Cyrillic or Greek
letters. A translation is one text file, and you can try it with a release
you have downloaded: nothing has to be built.

What this covers: every sentence of those two windows, the messages of the
install (what a file was recognised as, what went wrong) included. What it
does not: the game itself (its text is in the game's own files, which
translation mods replace), the Settings pages inside the game, the release
notes the Update page shows, and the log.

## Starting a translation

1. Take [`lang/dod3.pot`](../lang/dod3.pot), the list of every string, and
   save a copy named after your language's code: `ru.po`, `de.po`, `pt-BR.po`.
   It is a gettext catalogue: [Poedit](https://poedit.net) opens it (File >
   New from POT/PO file), and so does any text editor, as UTF-8.
2. In the header at the top, set `Language:` to the code and `Plural-Forms:`
   to your language's rule. Poedit fills both in; by hand, the rules are in
   [gettext's manual](https://www.gnu.org/software/gettext/manual/html_node/Plural-forms.html).
   Russian's, for example:

   ```
   "Language: ru\n"
   "Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\n"
   ```

   With the source and Python at hand, `python tools/lang.py new ru` writes
   the file with both set.
3. Translate each `msgstr`. One left empty stays in English, so a
   translation can be tried long before it is finished.

## Trying it

Make a folder named `lang` beside `dod3.exe` (beside `dod3` on Linux; on a
Mac, in `~/Library/Application Support/Drakengard 3 Recompiled`) and put your
file in it. Start the game: the launcher opens in your language if it is the
system's. Otherwise choose it in Settings > System > Launcher Language, or
write `DOD3_UI_LANGUAGE = ru` in `dod3.ini`. The file is read again at each
start. The setup uses the same language: `dod3.exe --setup` opens it on an
installed game.

A file in that folder is used instead of the one a release was built with,
under the same name.

## The entries

```
#: src/ui/launcher.cpp
msgid "Needs {1}; {2} free."
msgstr "Нужно {1}; свободно {2}."
```

- `{1}`, `{2}` ... stand for something filled in when the sentence is shown
  (a size, a name, a count). Keep each one; put them where your language
  wants them, in any order.
- An entry with `msgid_plural` depends on a count and has one `msgstr[n]` per
  plural form of your language, in the order of its `Plural-Forms` rule:

  ```
  msgid "Checking {1} file"
  msgid_plural "Checking {1} files"
  msgstr[0] "Проверяется {1} файл"
  msgstr[1] "Проверяются {1} файла"
  msgstr[2] "Проверяется {1} файлов"
  ```

- `msgctxt` tells two uses of the same English apart: `Install` is a button,
  `Install` with the context `heading` is the name of a step and of a page.
- The entry with the context `language` and the text `English` is not to be
  translated as a word: give your language's own name there (`Русский`,
  `Deutsch`). It is what the Launcher Language list shows.
- `#:` says which source file a string is from, `#.` is a note for you.
- An entry marked `#, fuzzy` (Poedit's "needs work") is not used: it shows in
  English until the mark is removed.
- File and folder names, and names the player types or sees elsewhere, stay
  as they are: `dod3.ini`, `PS3_GAME`, `EBOOT.BIN`, `.pkg`, the package IDs.
  Where a sentence names a button or a page ("use Game Files", "Add Folder"),
  use the words you gave that button or page.
- Text that ends with `...` or a space keeps it.

Buttons and the Settings rows grow with their text. The menu on the left, the
tabs and the small labels at the right of a card have less room: look at each
page once in the window at its smallest size, 1280x720.

## Sending it

Open a pull request that adds `lang/<code>.po`, or attach the file to an
[issue](https://github.com/soyjxck/dod3-recomp/issues). It is built into the
next release, and the language is then chosen by itself on a system set to it.

When a later release adds or changes strings, the file in the repository is
brought up to date with them (the new ones empty, the changed ones too: they
show in English until translated); an update of the translation is welcome
the same way.

## Writing systems

The typeface, Inter, has Latin, Cyrillic and Greek. A translation in another
script (Chinese, Japanese, Korean, Arabic, Hebrew, Thai ...) needs a typeface
added for it, and the last three need text laid out in ways the windows do not
do yet: open an issue first.

## For changes to the source

A sentence the launcher or the setup shows goes through `src/lang.h`:
`tr("...")`, `trf("... {1} ...", value)` for one with parts filled in,
`trnf(n, "{1} file", "{1} files", n)` for one that depends on a count,
`trc("context", "...")` where the same English is two things, and
`TR_NOOP("...")` in a table whose text is passed to `tr()` where it is drawn.
A sentence is one string: it is not joined from translated pieces.

`python tools/lang.py update` writes `lang/dod3.pot` again from those calls
and brings every `lang/*.po` in line; `python tools/lang.py check` fails when
that has not been done, or when a translation has a placeholder its English
lacks. `python tools/lang.py pseudo <folder>/lang/qps.po` writes a made-up
translation of every string for tests: with `DOD3_UI_LANGUAGE=qps`, anything
still in plain English on a page was not marked, and `DOD3_UI_LANGUAGE_LOG=1`
lists those strings in the log.
