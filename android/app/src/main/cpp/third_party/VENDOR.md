# Dépendances vendoriées

Ces sources sont copiées dans le dépôt pour que le build Android soit
reproductible et hors ligne (aucun téléchargement à la configuration CMake).

## libusb 1.0.30 — `libusb/`

- Source : https://github.com/libusb/libusb (archive du tag `v1.0.30`), dossier
  `libusb/` + `android/config.h` officiel.
- Licence : LGPL-2.1 (`libusb/COPYING`).
- **Sources non modifiées, arborescence réduite.** libusb n'a pas de CMake en
  amont : `CMakeLists.txt` compile la liste de fichiers de `android/jni/libusb.mk`
  avec `HAVE_CONFIG_H=1` et `android/config.h` comme configuration.
- Ne restent que les fichiers nécessaires à ce build : les 6 `.c` compilés
  (`core`, `descriptor`, `hotplug`, `io`, `strerror`, `sync`) et le backend
  `os/` POSIX/Linux (`events_posix`, `linux_netlink`, `linux_usbfs`,
  `threads_posix`), les en-têtes qu'ils incluent (`libusb.h`, `libusbi.h`,
  `version*.h`, `os/*.h`), `android/config.h` et `COPYING`. Ont été supprimés
  les backends Windows, macOS, Haiku, NetBSD/OpenBSD/Solaris, emscripten et
  udev (non compilé : `USE_UDEV` est absent du `config.h` Android), ainsi que
  les fichiers autotools et `libusb-1.0.def`/`.rc` (Windows).
- Utilisé pour `libusb_wrap_sys_device()`, qui rattache un descripteur déjà
  ouvert par `UsbDeviceConnection.getFileDescriptor()`.

## librtlsdr 0.5.3 — `librtlsdr/`

- Source : https://github.com/librtlsdr/librtlsdr (archive du tag `v0.5.3`).
- Licence : GPL-2.0 (`librtlsdr/COPYING`).
- Seuls `src/librtlsdr.c`, `src/tuner_*.c` et `include/*.h` sont conservés ; les
  outils en ligne de commande (`rtl_test`, `rtl_fm`, …) ne sont pas compilés.
- La 0.5.3 est choisie parce qu'elle est la dernière version sans dépendance
  RPC/UDP (`rtlsdr_rpc.h`, `controlThread.c`) ajoutée ensuite.

### Patch local (le seul)

`librtlsdr` ne sait ouvrir une clé que par index après énumération de
`/dev/bus/usb` — impossible sans root sur Android. Ajouts :

1. `include/rtl-sdr.h` : déclaration de `rtlsdr_open_sys_device(dev, fd)`.
2. `src/librtlsdr.c` :
   - `static int rtlsdr_sys_fd` et `rtlsdr_open_sys_device()` qui pose
     `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` puis appelle `rtlsdr_open()` ;
   - dans `rtlsdr_open()`, une branche qui, si un descripteur est en attente,
     saute l'énumération, appelle `libusb_wrap_sys_device()` et saute au label
     `have_handle:` placé juste avant la configuration de l'interface.

Aucune autre ligne du fichier n'est modifiée : le chemin normal (desktop) est
inchangé.
