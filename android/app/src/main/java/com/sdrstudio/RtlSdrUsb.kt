package com.sdrstudio

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build

/**
 * Recherche et autorisation de la cle RTL-SDR (RTL2832U) via UsbManager.
 * L'ouverture reelle et le transfert des donnees se font en natif : ici on ne
 * fait que fournir un descripteur de fichier a libusb.
 */
object RtlSdrUsb {

    const val ACTION_PERMISSION = "com.sdrstudio.USB_PERMISSION"

    private const val VENDOR_REALTEK = 0x0BDA
    // RTL2832U / RTL2838 / RTL2840 selon le PCB.
    private val PRODUCT_IDS = intArrayOf(0x2832, 0x2838, 0x2840)

    fun isRtlSdr(device: UsbDevice): Boolean =
        device.vendorId == VENDOR_REALTEK && PRODUCT_IDS.contains(device.productId)

    /** Premiere cle RTL-SDR branchee, ou null. */
    fun find(context: Context): UsbDevice? {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return manager.deviceList.values.firstOrNull { isRtlSdr(it) }
    }

    /**
     * Intent de demande d'autorisation. FLAG_MUTABLE est necessaire : le systeme
     * y ajoute EXTRA_PERMISSION_GRANTED.
     */
    fun permissionIntent(context: Context): PendingIntent {
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val intent = Intent(ACTION_PERMISSION).setPackage(context.packageName)
        return PendingIntent.getBroadcast(context, 0, intent, flags)
    }

    /** UsbDevice transporte par le broadcast d'autorisation. */
    fun deviceFrom(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
}
