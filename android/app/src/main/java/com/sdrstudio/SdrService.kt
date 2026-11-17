package com.sdrstudio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import kotlin.concurrent.thread

/**
 * Service de premier plan : il garde la reception utilisable quand l'application
 * passe en arriere-plan ou que l'ecran s'eteint.
 *
 * Il est proprietaire de la lecture audio, du focus audio, du verrou d'eveil et
 * de la notification. Le moteur natif (thread DSP, enregistreurs WAV/IQ) vit dans
 * le meme processus : tant que le service tourne, la reception et les
 * enregistrements continuent.
 */
class SdrService : Service() {

    private var audioTrack: AudioTrack? = null
    private var audioThread: Thread? = null

    @Volatile private var playing = false
    // Focus perdu (appel, autre lecteur) : on continue a vider le tampon natif
    // mais on n'ecrit plus dans AudioTrack.
    @Volatile private var muted = false

    @Volatile private var focusRequest: AudioFocusRequest? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var statusLine = "reception en cours"

    private val audioManager by lazy { getSystemService(Context.AUDIO_SERVICE) as AudioManager }

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> {
                muted = false
                audioTrack?.let { runCatching { it.setVolume(1f); it.play() } }
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                // Radio : on baisse plutot que de couper pendant une notification.
                runCatching { audioTrack?.setVolume(0.2f) }
            }
            else -> muted = true // perte durable ou temporaire : silence
        }
    }

    override fun onCreate() {
        super.onCreate()
        alive = this
        createChannel()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopReception()
                return START_NOT_STICKY
            }
            ACTION_UPDATE -> {
                val line = intent.getStringExtra(EXTRA_TEXT)
                if (!line.isNullOrEmpty() && line != statusLine) {
                    statusLine = line
                    updateNotification()
                }
            }
            else -> {
                intent?.getStringExtra(EXTRA_TEXT)?.takeIf { it.isNotEmpty() }?.let { statusLine = it }
                startReception()
            }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopAudio()
        abandonFocus()
        releaseWakeLock()
        if (alive === this) {
            alive = null
            isRunning = false
            // Destruction externe (retrait de la tache, recyclage systeme) :
            // le moteur ne doit pas continuer sans audio ni notification.
            SdrEngine.nativeStop()
        }
        super.onDestroy()
    }

    // ------------------------------------------------------------- reception

    private fun startReception() {
        createChannel()
        startForeground(NOTE_ID, buildNotification())
        isRunning = true
        requestFocus()
        acquireWakeLock()
        startAudio()
    }

    private fun stopReception() {
        stopAudio()
        abandonFocus()
        releaseWakeLock()
        // Le moteur natif est dans ce processus : on l'arrete ici pour que le
        // bouton "Arreter" de la notification suffise, application fermee.
        SdrEngine.nativeStop()
        sendBroadcast(Intent(ACTION_STOPPED).setPackage(packageName))
        isRunning = false
        alive = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ----------------------------------------------------------------- audio

    private fun startAudio() {
        stopAudio()
        val minBytes = AudioTrack.getMinBufferSize(
            48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build())
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(48000)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build())
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(maxOf(minBytes * 2, 8192)) // ~100 ms de marge
            .build()
        track.play()
        audioTrack = track
        muted = false

        playing = true
        audioThread = thread(name = "sdr-audio") {
            val buffer = FloatArray(1024)
            while (playing) {
                val n = SdrEngine.nativeReadAudio(buffer)
                if (n <= 0) {
                    Thread.sleep(4)
                    continue
                }
                // Focus perdu : on vide quand meme la file pour ne pas prendre
                // de retard au retour du son.
                if (!muted) track.write(buffer, 0, n, AudioTrack.WRITE_BLOCKING)
            }
        }
    }

    private fun stopAudio() {
        playing = false
        audioThread?.join(500)
        audioThread = null
        audioTrack?.run {
            runCatching { stop() }
            release()
        }
        audioTrack = null
    }

    // -------------------------------------------------------- focus / verrou

    private fun requestFocus() {
        val granted = if (Build.VERSION.SDK_INT >= 26) {
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setOnAudioFocusChangeListener(focusListener)
                .build()
            focusRequest = request
            audioManager.requestAudioFocus(request)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
        }
        muted = granted != AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun abandonFocus() {
        if (Build.VERSION.SDK_INT >= 26) {
            focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
            focusRequest = null
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(focusListener)
        }
    }

    /** Ecran eteint : le DSP et les enregistrements doivent continuer a tourner. */
    private fun acquireWakeLock() {
        if (wakeLock != null) return
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "sdrstudio:reception").apply {
            setReferenceCounted(false)
            runCatching { acquire() }
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { runCatching { it.release() } }
        wakeLock = null
    }

    // ---------------------------------------------------------- notification

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (manager.getNotificationChannel(CHANNEL) != null) return
        val channel = NotificationChannel(CHANNEL, "Reception", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Reception SDR en arriere-plan"
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, SdrService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_sdr)
            .setContentTitle("SDR Studio")
            .setContentText(statusLine)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .addAction(0, "Arreter", stop)
            .build()
    }

    private fun updateNotification() {
        if (!isRunning) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        runCatching { manager.notify(NOTE_ID, buildNotification()) }
    }

    companion object {
        const val ACTION_START = "com.sdrstudio.action.START"
        const val ACTION_STOP = "com.sdrstudio.action.STOP"
        const val ACTION_UPDATE = "com.sdrstudio.action.UPDATE"
        const val ACTION_STOPPED = "com.sdrstudio.action.STOPPED"

        private const val EXTRA_TEXT = "text"
        private const val CHANNEL = "reception"
        private const val NOTE_ID = 1

        /** Vrai tant que le service tourne, donc que la reception continue. */
        @Volatile
        var isRunning = false
            private set

        // Instance courante : une destruction retardee ne doit pas arreter un
        // moteur relance entre-temps.
        @Volatile
        private var alive: SdrService? = null

        /** Demarre (ou met a jour) la reception au premier plan. */
        fun start(context: Context, text: String) {
            val intent = Intent(context, SdrService::class.java)
                .setAction(ACTION_START)
                .putExtra(EXTRA_TEXT, text)
            ContextCompat.startForegroundService(context, intent)
        }

        /** Rafraichit le texte de la notification (au plus une fois par seconde). */
        fun update(context: Context, text: String) {
            if (!isRunning) return
            runCatching {
                context.startService(
                    Intent(context, SdrService::class.java)
                        .setAction(ACTION_UPDATE)
                        .putExtra(EXTRA_TEXT, text))
            }
        }

        /** Arrete la reception : audio, focus, notification, moteur natif. */
        fun shutdown(context: Context) {
            if (!isRunning) {
                context.stopService(Intent(context, SdrService::class.java))
                return
            }
            runCatching {
                context.startService(
                    Intent(context, SdrService::class.java).setAction(ACTION_STOP))
            }.onFailure { context.stopService(Intent(context, SdrService::class.java)) }
        }
    }
}
