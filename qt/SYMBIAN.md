# Building `openknot-qt` as a Symbian `.sis`

Qt Creator: open **`qt/openknot.pro`**, pick the *Symbian Device* kit,
**Build ▸ Deploy**. Command line: `qmake -spec symbian-gcce openknot.pro &&
make release-gcce && make sis`.

The sources build with the Symbian GCGE (**GCC 4.6.3**) — see §6 — and only need
the `qt/` folder; §2.1 shows the layout and where each file belongs.

## 1. What you need

| Piece | Version / notes |
|---|---|
| Host | Windows (the Symbian toolchain is Windows-only); Linux/macOS only via Qt's remote compiler |
| SDK | **Qt SDK 1.2** (`Nokia_Qt_SDK_Win_offline_v1_2_1`) or Qt SDK 1.1.2 if you need the older targets |
| Target | *Qt 4.7.4 for Symbian* (Symbian Belle), *Qt 4.7.3 for Symbian Anna* |
| Compiler | GCGE `arm-none-symbianelf-g++` **4.6.3** (part of the SDK), SBSv2 |
| Device | Symbian Anna/Belle or Symbian^3 (the latter needs Smart Installer to get Qt 4.7.4) |

The app only needs `QtCore`, `QtGui` (widgets live in QtGui in Qt 4) and
`QtNetwork` — no Qt Quick, no QtSql, no QtMobility.

## 2. Working in Qt Creator (recommended)

Qt Creator only needs **the `qt/` folder** — the Node backend, the docs and
`qt/tests/` are not part of the device build. Copy the whole checkout if you
like, but open `qt/openknot.pro` and nothing else.

### 2.1 Where to put the files

Use a **short path without spaces** on the Windows host, e.g.
`C:\dev\openknot\qt`. Symbian's SBSv2 build system breaks on spaces in
paths and on very long paths, and Qt Creator's shadow build directory appends
another 40+ characters to whatever you choose.

```
C:\dev\openknot\
└── qt\                        <- open THIS in Qt Creator (qt\openknot.pro)
    ├── openknot.pro            the only project file; paths in it are relative
    ├── SYMBIAN.md              this guide
    ├── README.md
    ├── CMakeLists.txt          desktop builds only - ignore for Symbian
    ├── src\
    │   ├── main.cpp
    │   ├── models.h
    │   ├── api_client.h/.cpp   proxy mode (REST client)
    │   ├── mainwindow.h/.cpp   the UI
    │   ├── qt4compat\          QJson*/WebSocket/SignalHook for Qt 4.7
    │   └── direct\             the WhatsApp protocol (Qt-free core + glue)
    └── tests\                  host-only tests - NOT in the .pro, never built
                                for the device
```

Rules that matter:

* **Do not flatten** `src/`, `src/direct/` and `src/qt4compat/` into one
  folder. `INCLUDEPATH += src src/direct src/qt4compat` and includes such as
  `#include "direct/whatsapp_direct.h"` are relative to that layout.
* **Do not add `tests/` to the project.** Those are plain host programs
  (`g++` + Node scripts), not Qt targets; adding them to `SOURCES` makes the
  Symbian build fail. They are not listed in `openknot.pro` for that reason.
* Keep `openknot.pro` next to `src/`; every path inside it is relative, so the
  folder can be moved or copied freely as a unit.
* Build **out of source** (Qt Creator's default: *shadow build* ticked). The
  generated `.pkg`/`.sis` land in that shadow directory, not in `qt/`.

### 2.2 Kit setup

1. *Tools ▸ Options ▸ Build & Run ▸ Qt Versions* — **Add…** the Symbian Qt
   4.7.4 `qmake.exe` from the Qt SDK (`…\QtSDK\Symbian\SDK\4.7.4\…`).
2. *Kits* — the SDK normally pre-creates **Symbian Device**; check it uses the
   GCGE 4.6.3 compiler, *Qt 4.7.4 for Symbian*, and the **CODA** device
   connection (PC Suite mode, USB).
3. *Projects ▸ Build* — set the build configuration to **Release**, keep
   *Shadow build directory* ticked, and confirm the C++ flag shows
   `-std=c++0x` (from the `.pro`).
4. *Projects ▸ Run* — arguments, for example:
   * direct mode: `--direct --creds E:/openknot/creds.json`
   * proxy mode: `--url http://192.168.1.10:3000 --key YOUR_API_KEY`
5. **Build ▸ Deploy** (or right-click the project ▸ *Deploy*) builds the
   `sis` and installs it. The same step exists as a *Deploy configuration* in
   the Projects pane.

### 2.3 Where the app writes at runtime (on the phone)

| What | Default | Recommended (pass as an argument) |
|---|---|---|
| device credentials (`creds.json`) | `~/.openknot-qt/creds.json` → `C:/Data/…` | `--creds E:/openknot/creds.json` (mass memory, survives a hard reset of C:) |
| downloaded media (`File ▸ Save media`) | you pick the path each time | `E:/openknot/media/` |
| files you send | any readable path | watch out: `C:/Data` needs `ReadUserData`, which the package has |

The package already requests `NetworkServices ReadUserData WriteUserData`, so
`E:/openknot/` works without a developer certificate.

## 3. Build from the command line

From the Qt SDK's *Symbian Qt 4.7.4* command prompt (it sets `EPOCROOT`,
`SBS_HOME`, `QTDIR`):

```bash
cd qt
qmake -spec symbian-gcce openknot.pro
make release-gcce          # compiles arm-none-symbianelf-g++
make sis                   # -> openknot-qt_template.pkg + unsigned .sis
make signedsis             # -> openknot-qt.sis signed with a self-signed cert
```

From Qt Creator: open `qt/openknot.pro`, pick the *Symbian Device* target,
then **Build ▸ Create SIS Package** (same thing, wrapped in the IDE).

Shadow builds put the artifacts under
`build-openknot-qt-Symbian_Device-…/`; the interesting files are
`openknot-qt_template.pkg` (the generated package definition) and
`openknot-qt.sis`.

## 4. Signing and installation

`make signedsis` signs with `selfsignedcert.cer` / `selfsignedkey.pem`, which
qmake generates with `makekeys` if you do not provide your own:

```bash
qmake -spec symbian-gcce openknot.pro \
      SELF_SIGN_KEY=mykey.pem SELF_SIGN_CERT=mycert.cer
make signedsis
```

The capabilities in `openknot.pro` are the user-grantable ones, so no
developer certificate from Symbian Signed is needed:

```
NetworkServices ReadUserData WriteUserData
```

On the phone:

* Settings ▸ Application manager ▸ Installation ▸ **Software installation = All**,
  **Online certificate check = Off**;
* set the clock correctly (a wrong date makes self-signed certs look expired);
* copy the `.sis` over (USB mass storage / Bluetooth) and tap it.

If you only want proxy mode you still install the same package — the mode is a
runtime switch (`--direct` or *Connection ▸ Direct WhatsApp Web mode*).

## 5. Runtime notes

* Credentials live next to the app data; on Symbian default to the mass memory:
  `openknot-qt --direct --creds E:/openknot/creds.json`.
* Direct mode logs `TLS: …` or a loud warning when the Qt build has no OpenSSL
  (`QSslSocket::supportsSsl()` is false). Without TLS, `wss://` cannot work.
* If the phone's root store is too old for WhatsApp's certificate chain,
  `OPENKNOT_INSECURE_TLS=1` makes the transport skip verification (testing
  only).
* Nothing has been run on a Symbian device yet: RAM use, font sizes and the
  soft-key/QMenuBar layout are unverified. The window is plain Qt widgets and
  `QSplitter`-based, so it should scale, but expect cosmetic work.

## 6. Blockers you will hit (honest list)

1. ~~C++11 completeness.~~ **Done.** GCGE is 4.6.3, which lacks non-static
   data member initialisers, `override`, `std::to_string`/`stoi`/`stoull` and
   `<chrono`. The sources now avoid all of them:
   * every struct/class initialises its members in a constructor instead of in
     the declaration (~190 sites across 16 headers),
   * `override` is gone,
   * `src/direct/compat.h` provides `compat::toString()` / `parseUnsigned()` /
     `hexNibble()` / `clockTicks()` to stand in for the C++11 string
     conversions and the `<chrono>` seed,
   * the qmake project builds with `-std=c++0x` (GCC 4.6 does not know
     `-std=c++11`).

   `qt/tests/gcc46_check.sh` is the gate: it greps the sources for anything
   GCC 4.6 rejects and compiles the protocol core with `-std=c++0x`.

2. **OpenSSL in the Qt build.** Nokia's *Qt 4.7.4 for Symbian* ships with SSL
   enabled; a self-built Qt without `-openssl` gives you a device that can run
   proxy mode but never finishes a direct-mode handshake. Check the log line
   from §5 before debugging anything else.

3. **Qt version on the device.** Belle has Qt 4.7.4 in ROM, Anna 4.7.3, plain
   Symbian^3 only 4.6.3 — on the last one you need Nokia Smart Installer (or
   bundle Qt) so the app gets 4.7.x at install time.

4. **Build system.** CMake is not part of SBSv2, which is why `openknot.pro`
   exists; it builds exactly the same source list as `qt/CMakeLists.txt`.

## 7. Troubleshooting

### `nullptr was not declared`, `ISO C++ forbids initialization of member`, `::swprintf has not been declared`

You are building the **Qt Simulator (desktop) target**, not Symbian. The build
log gives it away:

```
...\Symbilink-build-simulator-Simulator_Qt_for_MinGW_4_4__Qt_SDK__Debug\
g++ ... -std=gnu++0x ...        c:\QtSDK\mingw\...\mingw32\4.4.0\include
```

The SDK's simulator/desktop Qt 4.7.4 is compiled with **MinGW GCC 4.4**, a
C++98 compiler — no `nullptr`, lambdas, `auto`, range-`for` or `enum class`.
This client is C++11 and needs GCC ≥ 4.6, i.e. the **Symbian Device** kit
(GCGE 4.6.3).

Fix: in the **Projects** pane, switch the active target from *Qt Simulator* to
**Symbian Device** (or use the kit selector above the Run button). The shadow
build directory then becomes `...-build-symbian-Symbian_Device-...` and the
build runs `arm-none-symbianelf-g++`.

`openknot.pro` now refuses to configure for a compiler older than GCC 4.6 with
a single error, so a wrong kit is obvious immediately instead of producing a
screenful of template errors.

If there is **no** Symbian target in the kit list, the SDK was installed
without it: run `C:\QtSDK\MaintenanceTool.exe` (or the SDK installer) and add
**Qt 4.7.4 for Symbian** plus its toolchain (the installer also wants the
Symbian SDK / CODA for on-device deploy).

Note on the `::swprintf` error specifically: that one is a libstdc++ 4.4 quirk
with the *strict* `-std=c++0x` dialect. The project now uses `-std=gnu++0x`,
which keeps those C99 declarations visible — but GCC 4.4 still cannot compile
the client.

### I want to try the UI on the PC first

Don't use the Qt Simulator. Build the desktop version with Qt 5 (or 6):

```bash
cmake -S qt -B build/qt && cmake --build build/qt
```

or open `qt/CMakeLists.txt` in Qt Creator with a Qt 5 kit. Same sources, same
features; only the Qt-version-specific bits differ (the `qt4compat/` shims are
compiled out on Qt 5+).

### The package is called `openknot-qt.sis` but my project is "Symbilink"

`TARGET` in the `.pro` decides the binary/package name: set `TARGET =
Symbilink` (and keep `TARGET.UID3` unique to your app). Renaming the `.pro`
only changes the Qt Creator project name.
