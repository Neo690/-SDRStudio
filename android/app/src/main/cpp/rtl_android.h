// SDRStudio - passage du descripteur USB de Kotlin (UsbManager) au backend RTL.
#pragma once

namespace sdr {

// Depose le descripteur ouvert par UsbManager. Consomme une seule fois, par
// createRtlSource() (kind "rtl" ou "auto").
void setPendingUsbFd(int fd);

int pendingUsbFd(); // -1 si aucun descripteur en attente

} // namespace sdr
