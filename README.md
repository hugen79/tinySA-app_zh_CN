# tinySA-App 2

PC software for the **tinySA** spectrum analysers (tinySA basic, tinySA Ultra, tinySA Ultra+),
rewritten in C++20 / Qt 6. It replaces the C++Builder version of tinySA-App; the device is
recognised from its `info` reply, so the controls always match
the connected model.

## Features
- Live spectrum over USB with the firmware's `scanraw`, on the exact frequency grid of the unit;
  with tinySA Ultra firmware v1.4-314 or newer the tinySA's screen is switched off while
  scanning, which gives up to 50 % more sweeps per second (Settings > Device; touch the tinySA
  to get its screen back)
- Start/stop/center/span, points (51–30000), RBW, attenuation, LNA (Ultra), LOW/HIGH input (basic),
  or take over the range set on the tinySA itself
- Max/min hold, averaging over 2–64 sweeps (with a sweep counter and a restart button),
  normalisation, three trace memories (kept between runs),
  log (dBm) or linear (W) scale, spline and point display
- Graph interaction: hover readout, drag the frequency or level scale (ends or middle), mouse wheel
  zoom, PgUp/PgDn, Esc stops, right-click menu
- Markers: double click to add, drag, delta markers, marker list; max/min peak markers with
  sub-point (least-squares parabolic) frequency interpolation
- Signal generator: an Analyzer | Generator switch turns the tinySA into a generator, with a
  front panel in place of the graph:
  - Large frequency and level displays: wheel or arrow keys over a digit tune it, or type a value
    such as `433.92M`.
  - An RF on/off button (off whenever the generator is switched on; Esc switches it off).
  - CW, sweep (frequency and/or level), AM and FM (tinySA basic: AM, NFM, WFM, external), and
    burst on the tinySA Ultra with firmware v1.4-305 or newer: the output on for an ON time of
    every period, continuously or a number of bursts per Start burst.
  - The output path: LOW/HIGH port on the basic, accurate or cleanest on the Ultra.
  - External gain.
  The app follows a tinySA that is already generating when it connects, and leaves it
  generating when it closes.
- Save CSV (also the tinySA's MHz format for wireless managers such as Wireless Workbench),
  record every sweep, save or copy the graph as an image
- Screen capture of the tinySA and live remote control (click on the picture to touch the screen);
  uses DiSlord's compressed RLE8 capture when the firmware supports it; inverted colours for printing
- Console with command history and four programmable macro buttons, battery voltage plot
- Firmware update over USB (DFU) with whichever Windows driver the bootloader uses
  (WinUSB, libusb-win32, libusbK or ST's DfuSe/STTub30); only the pages the firmware needs are
  erased, so the tinySA keeps its stored configuration
- Light or dark application colours, adjustable graph colours

## Running
Unzip `tinySA-App-<version>.zip` (e.g. `tinySA-App-2.0.0-39.zip`) anywhere and start `tinySA-App.exe`;
nothing needs installing. `changelog.txt` lists what changed in each version.
Settings are kept in `tinySA-App.settings.ini` next to the exe (or in `%APPDATA%\tinySA-App`
when that folder is read-only). `tinySA-App.exe --sim` adds simulated tinySA units to the port list.

Use tinySA firmware v1.4-266 or newer. Older firmware can lose a command sent over USB and stop
answering until it is restarted; the app then shows "Old firmware" next to the device, with a link
to the firmware update.

## 简体中文界面 (Simplified Chinese UI)

This fork adds a Simplified Chinese (zh_CN) interface:
本 fork 新增了简体中文（zh_CN）界面：

- 278 UI strings are translated (menus, toolbar, control panel, generator, console, battery,
  screen, settings and firmware dialogs, graph axis labels, connection states).
  已翻译 278 条界面字符串（菜单、工具栏、控制面板、发生器、控制台、电池、屏幕、设置与固件对话框、图表坐标轴标签、连接状态）。
- The language is selected with the command-line option `--lang zh_CN` (or `--lang en` for
  English). Without `--lang`, a Chinese system locale is detected automatically.
  界面语言通过命令行选项 `--lang zh_CN` 选择（`--lang en` 为英文）。不指定 `--lang` 时，若系统为中文区域会自动检测。

```
tinySA-App.exe --lang zh_CN
```

- The translation source is `translations/tinySA-app_zh_CN.ts`; it is compiled to
  `translations/tinySA-app_zh_CN.qm` and embedded into the executable through
  `translations/translations.qrc`, so no external language file is needed at run time.
  To change the wording, edit the `.ts` and run `lrelease` (or `build.bat`).
  翻译源文件为 `translations/tinySA-app_zh_CN.ts`；它被编译为 `translations/tinySA-app_zh_CN.qm`，
  并通过 `translations/translations.qrc` 嵌入可执行文件，因此运行时无需外部语言文件。
  如需修改文案，编辑 `.ts` 后运行 `lrelease`（或 `build.bat`）。

## Building
Requirements: Visual Studio 2026 (Community is fine) with *Desktop development with C++*, and
Qt 6.10 for `msvc2022_64` including *Qt Serial Port*, installed under `C:\Qt`.

```
build.bat          configure + build (build\), copy the Qt runtime, run the unit tests
build.bat dist     the same, plus a ready-to-run dist\ folder and tinySA-App-<version>.zip (replaces an older one)
```

In Visual Studio use *File > Open > Folder* on this directory (CMake project).

This fork also builds with **MinGW-w64 GCC 13** and Qt 6.10 `win64_mingw` (no Visual Studio
needed). A few changes were required for GCC / Qt 6.10 compatibility — see the commit
"Add Simplified Chinese localization and MinGW build fixes":

- `QList::append({...})` braced-init ambiguity → explicit element types (`append(T{...})`).
- `res/tinySA-App.rc` version string fixed for `windres`.
- `AUTORCC ON` so the translation resource is embedded.

```
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<Qt>/6.10.3/mingw_64
cmake --build build --parallel
```

### Editing the screens
Every window is a Qt Designer form in `src/ui/*.ui` (main window, generator page, console,
battery, screen, settings, firmware). Open them with Qt Designer (`C:\Qt\6.10.3\msvc2022_64\bin\designer.exe`),
or from Visual Studio with the Qt VS Tools extension. The graph (`SpectrumView`) and the screen
view are *promoted* widgets: you place and size them in Designer, their drawing lives in code.
Give new widgets an objectName; after moving widgets between containers, delete `build\` once.

## Tests
- `build\tst_core.exe`: unit tests for the protocol parser, device model, sweep settings, traces,
  CSV, markers, RLE8 capture and DFU (against a simulated bootloader); run by `build.bat`.
- GUI scripts: `build\tinySA-App.exe --sim --ini test\out\t.ini --script test\smoke.txt --out test\out`
  (exit code = number of failures). `test\step*.txt` and `test\gen_*.txt` cover each feature,
  `test\hw_*.txt` need a real tinySA (`--port COMx`); `test\hw_gen_*.txt` need two, the RF port of
  one cabled to the input of the other.

`MAP.md` describes the code layout, the tinySA protocol details and the test conventions.

## License
GPL-3.0-or-later. Uses Qt 6 (LGPL-3) and QCustomPlot 2.1.1 (GPL). Based on tinySA-App by
Erik Kaashoek, itself derived from NanoVNA-App by OneOfEleven. RLE8 screen capture format by
DiSlord (NanoVNA-D).
