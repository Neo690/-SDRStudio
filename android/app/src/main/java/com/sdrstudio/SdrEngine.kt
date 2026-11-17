package com.sdrstudio

/**
 * Acces au moteur natif C++ (meme coeur DSP que la version Windows).
 * L'ordre de [modeNames] suit exactement sdr::Mode (dsp.h) : c'est l'index
 * passe a nativeSetMode.
 */
object SdrEngine {

    init {
        System.loadLibrary("sdrstudio")
    }

    val modeNames = arrayOf("WBFM", "NBFM", "AM", "USB", "LSB", "CW")

    /** Demarre sur la cle RTL-SDR si un descripteur attend, sinon generateur interne. */
    external fun nativeStart(): Boolean

    /** Demarre sur la source materielle ; [fd] vient de UsbDeviceConnection. */
    external fun nativeStartUsb(fd: Int): Boolean

    /** Rejoue un enregistrement IQ (.cu8) ; [rate] est le debit de la capture. */
    external fun nativeStartFile(path: String, rate: Double): Boolean

    external fun nativeStop()

    /** Decalage du canal a l'interieur de la bande capturee (rejeu). */
    external fun nativeSetOffsetHz(hz: Double)

    /** Decalage effectivement applique (borne par le coeur DSP). */
    external fun nativeOffsetHz(): Double

    /** |offset| maximal qui garde le canal entier dans la bande. */
    external fun nativeMaxOffsetHz(): Double

    /** Nom de la source effectivement ouverte ("RTL-SDR R820T", "Generateur interne"...). */
    external fun nativeSourceName(): String

    external fun nativeGetSpectrum(out: FloatArray): Int
    external fun nativeReadAudio(out: FloatArray): Int
    external fun nativeSetMode(mode: Int)
    external fun nativeSetVolume(volume: Float)
    external fun nativeSetFrequency(hz: Double)

    /** Gain en dB ; une valeur negative selectionne le gain automatique. */
    external fun nativeSetGain(db: Double)

    /** Paliers de gain acceptes par le tuner (vide si la source n'en expose pas). */
    external fun nativeGains(): FloatArray

    external fun nativeSetPpm(ppm: Int)
    external fun nativeSetSquelch(on: Boolean, db: Float)

    /** Le canal depasse-t-il le seuil de squelch (critere d'arret du scan) ? */
    external fun nativeSquelchOpen(): Boolean

    external fun nativeStartWav(path: String): Boolean
    external fun nativeStopWav()
    external fun nativeIsRecordingWav(): Boolean

    external fun nativeStartIq(path: String): Boolean
    external fun nativeStopIq()
    external fun nativeIsRecordingIq(): Boolean

    external fun nativeChannelDb(): Double
}
