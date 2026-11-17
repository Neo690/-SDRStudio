# SDR Studio — Android

Application Kotlin + JNI qui réutilise **le même cœur DSP C++** que la version Windows
(`../core/`) : canaliseur, démodulateurs WBFM/NBFM/AM/USB/LSB/CW, spectre FFT 4096 points,
AGC, squelch. Le thread natif lit la source par blocs de 8192 échantillons à 2,4 MS/s ;
Kotlin ne fait que consommer le spectre et l'audio.

Deux sources de signal, choisies automatiquement au démarrage :

- **clé RTL-SDR** branchée en USB (backend `librtlsdr` + `libusb` compilé avec le NDK) ;
- **générateur interne** (5 stations FM synthétiques) si aucune clé n'est présente ou si
  l'autorisation USB est refusée — l'application reste utilisable sans matériel.

---

## État vérifié sur cette machine

| Élément | État | Preuve |
|---|---|---|
| JDK 17 / SDK 34 / NDK 26.1 / CMake 3.22.1 / Gradle 8.7 | ✅ installés | `%LOCALAPPDATA%\android-toolchain` et `%LOCALAPPDATA%\Android\Sdk` |
| Code Kotlin | ✅ compilé | `:app:compileDebugKotlin` sans erreur |
| Cœur DSP + JNI + backend USB (arm64/armv7) | ✅ compilé et lié | `libusb` + `librtlsdr` compilés par le NDK, `BUILD SUCCESSFUL` |
| Symboles dans la `.so` | ✅ vérifiés | `rtlsdr_open_sys_device` (patch), `libusb_wrap_sys_device`, 10 fonctions JNI |
| APK debug | ✅ produit | 4 482 462 o, `.so` 669 776 o (arm64) / 466 876 o (armv7) |
| Exécution sur téléphone | ✅ testée | Realme RMX3563, Android 14 : repli sur le générateur interne, pas de crash, DSP à 178 % CPU |
| Gain / PPM / squelch / enregistrement | ✅ testés à la main | boutons cliqués en `adb`, `STOP WAV` + fichier `.wav` valide (RIFF, 48 kHz, 16 bits, en-tête recalculé) et `.cu8` de 49 Mo produits puis supprimés |
| **Réception réelle avec la clé** | ❌ **non testée** | aucune clé branchée en OTG sur le téléphone au moment du test |

Autrement dit : le chemin matériel est écrit et se lie correctement, mais **il n'a jamais
fait couler un seul échantillon**. C'est la limite honnête de ce qui est vérifiable ici.

---

## Backend USB (clé RTL-SDR)

### Côté Kotlin

`RtlSdrUsb.kt` :

1. parcourt `UsbManager.deviceList` à la recherche de VID `0x0BDA` et PID `0x2832`,
   `0x2838` ou `0x2840` ;
2. si l'autorisation manque, appelle `UsbManager.requestPermission()` avec un
   `PendingIntent` `FLAG_MUTABLE` (le système y ajoute `EXTRA_PERMISSION_GRANTED`) ;
3. `MainActivity` écoute ce broadcast (`ContextCompat.RECEIVER_NOT_EXPORTED`) puis ouvre la
   clé avec `openDevice()` et transmet `UsbDeviceConnection.getFileDescriptor()` au natif.

`MainActivity` ne revendique **pas** l'interface en Java : c'est libusb qui fait
`claim_interface`, sur le même descripteur. Le `UsbDeviceConnection` est conservé tant que
le moteur tourne, car le descripteur appartient à libusb jusqu'à `rtlsdr_close()`.

Le manifeste déclare `USB_DEVICE_ATTACHED` avec `res/xml/usb_device_filter.xml`
(VID 3034), donc Android propose SDR Studio au branchement de la clé.

### Côté natif

`rtl_android.cpp` implémente `sdr::IqSource` :

- `rtlsdr_open_sys_device(fd)` (patch local, voir plus bas) →
  `libusb_wrap_sys_device()` avec `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` (indispensable sans
  root : on ne scanne pas `/dev/bus/usb`) ;
- réglages : 2,4 MS/s, gain automatique du tuner, correction de fréquence ;
- `rtlsdr_read_async()` tourne dans son propre thread et convertit le `cu8` en `cfloat`
  dans un anneau SPSC de 65 536 échantillons (~27 ms) ; `read()` ne fait que dépiler, donc
  le thread DSP du JNI n'attend jamais USB ;
- `setCenterHz`, `setSampleRate`, `setGainDb` (gain manuel au pas le plus proche),
  `setPpm` sont branchés sur l'API `rtlsdr_*` ;
- `setBiasTee` n'est pas exposé : `librtlsdr` 0.5.3 ne le fournit pas.

Le choix de source passe par `createSource()` : `nativeStartUsb(fd)` dépose le descripteur
puis démarre en mode `"rtl"`, `nativeStart()` démarre en mode `"auto"` (matériel si un
descripteur attend, sinon générateur interne). En cas d'échec d'ouverture, Kotlin retombe
proprement sur le générateur.

### Dépendances vendoriées

`app/src/main/cpp/third_party/` (859 Ko, détails et licences dans `third_party/VENDOR.md`) :

- **libusb 1.0.30** : compilé avec le `config.h` Android officiel de libusb (il n'y a pas
  de CMake en amont). Seuls le backend POSIX/Linux et les en-têtes qu'il inclut sont
  conservés : les backends Windows/macOS/Haiku/BSD/emscripten, les fichiers autotools et
  les `.def`/`.rc` ont été retirés ;
- **librtlsdr 0.5.3** (dernière version sans dépendance RPC) avec **un seul patch** :
  `rtlsdr_open_sys_device()`, qui saute l'énumération et branche
  `libusb_wrap_sys_device()` sur le descripteur fourni. Le chemin desktop de librtlsdr est
  inchangé.

---

## Recompiler

```bat
cd android
build-apk.bat
```

Équivalent manuel :

```bat
set "JAVA_HOME=%LOCALAPPDATA%\android-toolchain\jdk-17"
set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
"%LOCALAPPDATA%\android-toolchain\gradle-8.7\bin\gradle.bat" -p . assembleDebug
```

`local.properties` fixe `sdk.dir`. Sortie : `app/build/outputs/apk/debug/app-debug.apk`.
Installation : `adb install -r app\build\outputs\apk\debug\app-debug.apk`.

Un workflow GitHub Actions (`.github/workflows/android.yml`) fait le même build sur
`ubuntu-latest` avec le NDK, sans Android Studio.

Contrôle rapide du pont JNI hors NDK (sans le backend USB) :

```bash
g++ -std=c++20 -O3 -ffast-math -Wall -Wextra -fsyntax-only \
    -I android/tools/jni-stub -I core android/app/src/main/cpp/sdr_jni.cpp
```

---

## Structure

```
build-apk.bat                                 compilation en une commande
local.properties                              chemin du SDK pour Gradle
app/build.gradle.kts                          SDK 34, minSdk 24, NDK, CMake
app/src/main/AndroidManifest.xml              USB host + filtre VID de la clé
app/src/main/res/xml/usb_device_filter.xml    VID 3034 (Realtek)
app/src/main/cpp/CMakeLists.txt               libusb + librtlsdr + coeur + JNI
app/src/main/cpp/sdr_jni.cpp                  pont JNI, thread DSP, cycle de vie
app/src/main/cpp/rtl_android.cpp              IqSource sur librtlsdr/USB
app/src/main/cpp/rtl_android.h                passage du descripteur Kotlin -> natif
app/src/main/cpp/third_party/                 libusb 1.0.30, librtlsdr 0.5.3 (patché)
app/src/main/java/com/sdrstudio/
    SdrEngine.kt                              méthodes natives
    RtlSdrUsb.kt                              detection + autorisation USB
    SpectrumView.kt                           spectre + waterfall (Bitmap/Canvas)
    MainActivity.kt                           AudioTrack 48 kHz, cycle de vie, USB
tools/jni-stub/jni.h                          faux jni.h pour le contrôle hors NDK
```

Interface : spectre par colonne, waterfall à défilement (≈22 lignes/s), marqueur de
fréquence centrale, accord par appui sur le spectre, et une ligne de statut qui rappelle la
source réellement ouverte (`RTL-SDR R820T` ou `Generateur interne (demo)`), la fréquence,
le mode, le niveau du canal en dBFS, le volume et les enregistrements en cours.

Trois rangées de commandes :

- **radio** : ±1 MHz, mode (WBFM → NBFM → AM → USB → LSB → CW), volume ;
- **réception** : gain (cycle automatique puis chaque palier du tuner), correction PPM
  (0, ±1, ±2, ±5, ±10), squelch (un appui par état : `off` → seuils −85 à −50 dBFS par pas
  de 5 → `off` ; le seuil et l'état sont conservés d'un lancement à l'autre), `REC WAV`,
  `REC IQ` ;
- **réjeu** : `REJEU` (cycle entre les `.cu8` enregistrés, puis retour au direct),
  `-25 kHz`, `centre`, `+25 kHz` ; un appui sur le spectre place directement le canal.

Les enregistrements vont dans `Android/data/com.sdrstudio/files/` (aucune permission de
stockage requise) sous la forme `sdr_AAAAMMJJ_HHMMSS.wav` (48 kHz mono 16 bits) et
`sdr_AAAAMMJJ_HHMMSS.cu8` (IQ brut, rejouable ici par `REJEU` ou sur Windows par
`SDRStudio.exe --file`). Ils sont fermés
proprement dans `nativeStop()`, donc l'en-tête WAV reste valide même si l'application passe
en arrière-plan en pleine capture.

### Mémoires et scan

**MEM+** mémorise la fréquence courante sous un nom libre (le nom proposé est
`101.500 WBFM`, il suffit de le remplacer). **MEM** affiche la liste : un appui rappelle
l'accord (fréquence *et* mode), un appui long supprime après confirmation. Les mémoires
sont conservées en `SharedPreferences` (`memories`, JSON) et survivent au redémarrage ;
un même couple fréquence/mode n'est enregistré qu'une fois.

**SCAN** balaie la bande courante par pas de 100 kHz (radiodiffusion FM 87,5–108 MHz en
WBFM, 24–1766 MHz pour les autres modes) et s'arrête dès que le squelch s'ouvre. Le scan
demande un critère : si le squelch est désactivé il est activé au seuil courant en même
temps. Deux points de détail, appris sur la démo :

- le canal démodulé fait 240 kHz, donc le squelch s'ouvre déjà à 100 kHz d'une porteuse ;
après détection le scan mesure le centre et ses ±100 kHz et s'accorde sur le maximum, sinon
il s'arrêterait sur le flanc de la station ;
- lancé sur une station déjà ouverte, il saute d'abord la largeur du canal (200 kHz), sinon
le canal voisin la voit encore et le scan se reverrouille dessus.

Le scan est indisponible en rejeu (un fichier n'a pas de fréquence absolue) et s'arrête seul
si la réception est coupée depuis la notification.

### Arrière-plan (service de premier plan)

La réception n'est plus coupée quand l'application passe en arrière-plan ou que l'écran
s'éteint. `SdrService` tourne en service de premier plan (`foregroundServiceType="mediaPlayback"`)
et possède la lecture `AudioTrack`, le focus audio et un verrou d'éveil partiel : le thread
DSP et les enregistreurs WAV/IQ continuent donc de tourner. Une notification permanente
rappelle l'état (`101,500 MHz · WBFM`, ou `WBFM · offset +25 kHz` en réjeu) et propose un
bouton **Arreter** qui coupe la réception, libère le focus audio et retire la notification.
L'application affiche alors `réception arrêtée — toucher pour relancer` : un appui sur cette
ligne relance la réception (elle repart aussi d'elle-même au retour au premier plan).

Le focus perdu (appel entrant, autre lecteur) met l'audio en silence, ou en sourdine dans le
cas `CAN_DUCK`, sans interrompre ni la démodulation ni l'enregistrement. `nativeStop()` est
idempotent : l'activité et le service peuvent tous deux demander l'arrêt sans refermer deux
fois le descripteur USB.

---

## Ce qui reste à valider avec la clé branchée

1. `UsbManager` doit proposer SDR Studio au branchement (filtre VID) et la boîte de dialogue
   d'autorisation doit apparaître ;
2. `libusb_wrap_sys_device()` doit accepter le descripteur d'`UsbDeviceConnection` (c'est le
   cas dans l'exemple officiel `libusb/android/examples/unrooted_android.c`, mais non
   vérifié ici) ;
3. le débit réel : à 2,4 MS/s il faut que l'anneau de 65 536 échantillons absorbe les
   rafales USB, sinon `read()` renverra des blocs incomplets (le surplus est abandonné,
   jamais bloquant) ;
4. aucun accès root n'est demandé ; sur certains appareils le contrôleur USB OTG doit
   fournir assez de courant (clé RTL-SDR ≈ 300 mA).
