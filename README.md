# SDR Studio

Récepteur SDR natif pour **Windows** (C++20, Win32 + GDI + WASAPI) et **Android**
(même cœur DSP, interface Kotlin/JNI), piloté par une clé **RTL-SDR** (RTL2832U + R820T/R820T2).

Tout le traitement du signal est fait en C++ sur le CPU : pas de Python, pas d'Electron,
pas de dépendance lourde. L'exécutable Windows pèse ~800 Ko et n'utilise que
`librtlsdr.dll` + `libusb-1.0.dll` fournies dans `vendor/`.

---

## 1. Fonctionnalités

| Domaine | Détail |
|---|---|
| Spectre | FFT 4096 points, fenêtre de Hann, moyenne glissante, plage −110…−15 dBFS |
| Waterfall | défilement vertical, palette thermique, une ligne par trame FFT |
| Démodulations | WBFM, NBFM, AM, USB, LSB, CW |
| Audio | 48 kHz, WASAPI événementiel (10 ms demandés), AGC, volume, mute par gate |
| Accord | clic/drag sur le spectre, flèches, bandes préréglées, molette = volume |
| Réglages | gain (auto ou manuel par pas du tuner), correction ppm, squelch |
| Enregistrement | audio WAV 16 bits, IQ brut `.cu8` (rejouable avec `--file`) |
| Sans matériel | générateur interne de stations FM (mode simulation) |
| Auto-test | démodulations vérifiées + benchmark de débit en console |

Chaîne de réception : `IQ 2,4 MS/s` → suppression DC → mélangeur NCO →
décimation ÷5 (480 kS/s) → décimation ÷2 (240 kS/s) → démodulation →
filtre + décimation ÷5 → **audio 48 kS/s**.

## 2. Compilation (Windows)

Prérequis : MinGW-w64 / GCC récent.

```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT
build.bat
```

`build.bat` produit :

- `build\SDRStudio.exe` — l'application
- `build\sdr_selftest.exe` — le banc d'essai DSP

Compilation manuelle (si le dossier `bin` de MinGW est dans le PATH) :

```bat
g++ -std=c++20 -O3 -march=native -ffast-math -static -I core -I win ^
  -o build\SDRStudio.exe core\dsp.cpp core\source.cpp core\recorder.cpp ^
  core\selftest.cpp win\rtl_win.cpp win\audio_wasapi.cpp win\main.cpp ^
  -lole32 -luuid -lgdi32 -luser32
```

## 3. Pilote de la clé (obligatoire, une seule fois)

Une clé RTL-SDR fraîche n'a pas de pilote exploitable sous Windows : Windows la voit
en erreur et `librtlsdr` ne trouve aucun périphérique. Il faut lier **WinUSB** :

1. Brancher la clé.
2. Lancer `install-driver.bat` (ou `vendor\tools\zadig-2.9.exe` en administrateur).
3. Dans Zadig : sélectionner **RTL2832U**, pilote **WinUSB** (déjà présélectionné par
   `zadig.ini`), puis **Install Driver** / **Replace Driver**.
4. Vérifier :

```bat
build\SDRStudio.exe --probe
```

Attendu : nom de la clé, débit 2 400 000 éch/s, puissance en dBFS, pic spectral.
Si le message est « aucune cle RTL-SDR detectee », le pilote WinUSB n'est pas lié.

> Le pilote DVB-T officiel de Realtek ne convient pas : il faut WinUSB (c'est ce que fait Zadig).

## 4. Utilisation

```
SDRStudio.exe                 réception (clé si présente, sinon simulation)
SDRStudio.exe --synthetic     simulation seule
SDRStudio.exe --probe         test matériel en console (1 s de signal)
SDRStudio.exe --selftest      auto-test DSP + benchmark
SDRStudio.exe --file x.cu8    rejoue un enregistrement IQ [--rate 2400000] (en boucle)
```

| Touche | Action |
|---|---|
| clic / glisser sur le spectre | accorder la fréquence |
| ← / → | ±12,5 kHz (Maj : ±1 kHz) |
| ↑ / ↓ | ±500 kHz |
| Page↑ / Page↓ | ±2,5 MHz |
| M (Maj+M) | mode suivant (précédent) |
| G | cycle des gains (auto → pas du tuner) |
| S | squelch on/off |
| R | enregistrement audio WAV |
| I | enregistrement IQ `.cu8` |
| 1 / 2 / 3 / 4 | FM 98 MHz / aéronautique 127 / marine 156,8 / PMR446 |
| molette, + / − | volume |

**En rejeu (`--file`)**, les flèches, le clic et le glisser ne changent plus la fréquence :
un fichier IQ ne contient pas de fréquence absolue. Ils déplacent le canal à l'intérieur de
la bande enregistrée, avec le même décalage que le cœur DSP (`Receiver::setOffsetHz`).
L'en-tête affiche alors l'offset (`+125.0 kHz`) au lieu d'une fréquence, un repère pointillé
sur le spectre montre la position du canal, et `1`–`4` ramènent au centre du fichier.
Déplacement maximal ±(bande − canal)/2, soit ±1,08 MHz pour une capture à 2,4 MS/s. Le
fichier est relu en boucle : une capture de quelques secondes reste donc utilisable pour
écouter, se déplacer d'une station à l'autre et vérifier une démodulation.

Les enregistrements sont écrits dans le dossier courant (`rec_audio_*.wav`, `rec_iq_*.cu8`).

## 5. Performances mesurées (ce PC : i5-8265U, un seul cœur)

Sortie de `build\sdr_selftest.exe` :

```
[ OK ] resolution spectrale   pic au bin 317 (attendu 317)
[ OK ] WBFM 1 kHz             tone 1000.0 Hz (attendu 1000), rms 0.267
[ OK ] WBFM 1,5 kHz decale    tone 1500.0 Hz (attendu 1500), rms 0.267
[ OK ] NBFM 800 Hz            tone 800.0 Hz  (attendu 800),  rms 0.267
[ OK ] AM 1,2 kHz             tone 1200.0 Hz (attendu 1200), rms 0.267
[ OK ] calcul de decimation   canal 240000 Hz -> audio 48000 Hz (blocs de 8192)
[ OK ] invariance de phase    blocs 8192 vs 997 : ecart max 2.38e-07 sur 19200 ech.
[ OK ] debit chaine complete  68.4 MS/s (x28.5 temps reel a 2,4 MS/s), 80 trames FFT
```

Autrement dit : la chaîne complète (filtrage, démodulation, spectre 4096 points)
consomme moins de **4 % d'un cœur** à 2,4 MS/s. La marge absorbe les pics du système
et laisse le CPU au rendu de l'interface.

Deux tests verrouillent les calculs : le spectre ne calcule que 40 FFT/s (au lieu de
586, inutiles puisque l'interface affiche 30 images/s), et le résultat ne dépend pas du
découpage en blocs (écart 2,4e-07 entre des blocs de 8192 et de 997 échantillons,
simplement dû à l'arrondi flottant).

## 6. Architecture

```
core/                 cœur portable, aucune dépendance OS
  fft.h               FFT radix-2 + fenêtre de Hann
  dsp.h / dsp.cpp     canaliseur, démodulateurs, squelch, AGC, spectre
  source.h/.cpp       interface IqSource + générateur interne + lecture de fichiers IQ
  recorder.h/.cpp     WAV 16 bits + IQ .cu8
  selftest.cpp        tests d'exactitude + benchmark
  ring.h              file SPSC sans verrou (USB → DSP → audio)
win/
  main.cpp            interface Win32 (DIB, spectre, waterfall, clavier/souris)
  rtl_win.cpp         backend RTL-SDR (chargement dynamique de librtlsdr.dll)
  audio_wasapi.*      sortie audio WASAPI événementielle
android/              application Kotlin + JNI (même cœur C++)
vendor/               librtlsdr.dll, libusb-1.0.dll (builds hebdomadaires osmocom) + Zadig
```

Trois threads : lecture USB → (file SPSC) → DSP/spectre → (file SPSC) → WASAPI.
Aucun verrou sur le chemin des échantillons ; un mutex ne protège que les
changements de mode/volume.

Le backend matériel est isolé derrière `IqSource` : ajouter HackRF, PlutoSDR ou une
source distante (rtl_tcp) ne touche pas au DSP.

## 7. Limites connues (assumées)

- **Réception seule** : le RTL2832U n'a pas de chaîne d'émission, aucun logiciel ne peut en ajouter.
  Pour émettre il faut un autre matériel (PlutoSDR, HackRF, LimeSDR) — voir §8.
- **FM mono** : pas de décodeur stéréo (pilote 19 kHz) ni de RDS pour l'instant.
- **Débit fixe 2,4 MS/s** : bande visible de 2,4 MHz, pas de zoom ni de débit réduit.
- **Pas de décodeurs** (ADS-B, APRS, pagers, NOAA) : le logiciel est un récepteur généraliste.
- Android : voir `android/README.md` — l'app partage le cœur DSP mais la couche USB
  matérielle doit être compilée avec le NDK (non disponible sur cette machine).

## 8. Émission : ce qu'il faut savoir

Une clé RTL-SDR est **réception seule** (pas de PA, pas de DAC). Un logiciel
« émetteur-récepteur » suppose un second matériel capable d'émettre :

| Matériel | Bande | Duplex |
|---|---|---|
| ADALM-Pluto | 325 MHz – 3,8 GHz | semi-duplex |
| HackRF One | 1 MHz – 6 GHz | semi-duplex |
| LimeSDR Mini / USRP B210 | large | duplex simultané |

En France, l'émission est soumise à autorisation de l'ANFR (licence radioamateur
pour les bandes amateurs, appareils homologués uniquement en PMR446/CB).
Le brouillage est un délit. L'écoute, elle, est libre (hors correspondances privées
et secrets nationaux).

## 9. Ajouter l'émission plus tard

Le découpage le permet sans refonte : un `IqSink` symétrique de `IqSource`, une
chaîne de modulation (FM/AM/SSB/CW → IQ) et un panneau TX dans l'interface.
Le cœur DSP (filtres, NCO, spectre) est déjà partagé entre RX et TX.
