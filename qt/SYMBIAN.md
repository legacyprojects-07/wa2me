# Building `openknot-qt` as a Symbian `.sis`

Short version: `qmake -spec symbian-gcce qt/openknot.pro && make release-gcce && make sis`.
Read the **blockers** section first — as of today the sources need GCC ≥ 4.7 and
the Symbian toolchain is GCC 4.6.3, so the first build will fail unless the
C++11 features listed below are removed first.

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

## 2. Build from the command line

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

## 3. Signing and installation

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

## 4. Runtime notes

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

## 5. Blockers you will hit (honest list)

1. **C++11 completeness.** The sources are written for C++11 and are clean on
   GCC ≥ 4.7. GCGE is **4.6.3**, which does not have:
   * non-static data member initialisers (`int device = -1;` in a struct) —
     ~190 sites across 16 headers,
   * the `override` keyword,
   * `std::to_string` / `std::stoi` / `std::stoull` (libstdc++ has them from 4.9),
   * `std::chrono::high_resolution_clock` (only used as a fallback RNG seed).

   So on the Symbian compiler the first `make` stops in the first header.
   Removing them is mechanical (explicit constructors instead of NSDMI,
   `override` dropped, `to_string`/`stoi`/`stoull` shims, `time(nullptr)` for
   the seed) and the host test suite stays the guard rail — but it has **not**
   been done yet.

2. **OpenSSL in the Qt build.** Nokia's *Qt 4.7.4 for Symbian* ships with SSL
   enabled; a self-built Qt without `-openssl` gives you a device that can run
   proxy mode but never finishes a direct-mode handshake. Check the log line
   from §4 before debugging anything else.

3. **Qt version on the device.** Belle has Qt 4.7.4 in ROM, Anna 4.7.3, plain
   Symbian^3 only 4.6.3 — on the last one you need Nokia Smart Installer (or
   bundle Qt) so the app gets 4.7.x at install time.

4. **Build system.** CMake is not part of SBSv2, which is why `openknot.pro`
   exists; it builds exactly the same source list as `qt/CMakeLists.txt`.
