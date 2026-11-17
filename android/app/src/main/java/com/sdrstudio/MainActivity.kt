package com.sdrstudio

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : AppCompatActivity() {

    private lateinit var spectrumView: SpectrumView
    private lateinit var statusView: TextView
    private lateinit var modeButton: Button
    private lateinit var gainButton: Button
    private lateinit var ppmButton: Button
    private lateinit var squelchButton: Button
    private lateinit var wavButton: Button
    private lateinit var iqButton: Button
    private lateinit var replayButton: Button
    private lateinit var offsetDownButton: Button
    private lateinit var offsetUpButton: Button
    private lateinit var offsetCentreButton: Button
    private lateinit var memoryAddButton: Button
    private lateinit var memoryListButton: Button
    private lateinit var scanButton: Button

    private lateinit var usbManager: UsbManager
    private var usbConnection: UsbDeviceConnection? = null
    private var sourceLabel = "aucune source"

    // L'audio, le focus audio et la notification appartiennent a SdrService :
    // la reception survit ainsi au passage en arriere-plan.
    private var stopped = false
    private var lastNotified = ""
    private var lastNotifyAt = 0L

    private var mode = 0              // index dans SdrEngine.modeNames (WBFM au demarrage)
    private var volume = 0.7f
    private var centerHz = 101.5e6

    // Reglages radio conserves entre deux passages en arriere-plan.
    private var gains = FloatArray(0)
    private var gainIndex = -1        // -1 = gain automatique
    private var ppm = 0
    private val ppmSteps = intArrayOf(0, 1, 2, 5, 10, -1, -2, -5, -10)
    private var squelchOn = false
    // Cycle : off -> chaque seuil -> off. Conserve entre deux lancements.
    private var squelchDb = DEFAULT_SQUELCH_DB

    private var wavPath: String? = null
    private var iqPath: String? = null

    // Rejeu d'un enregistrement IQ : fichier courant (null = direct) et
    // position du canal dans la bande capturee.
    private var replayFile: File? = null
    private var offsetHz = 0.0

    // Scan : avance par pas fixes et s'arrete quand le squelch s'ouvre.
    private var scanning = false
    private var scanHits = 0      // lectures ouvertes consecutives
    private var scanSettle = 0    // trames d'attente apres un accord
    private var scanPhase = 0     // 0 = balayage, 1 = recentrage sur le max
    private var refineCentre = 0.0
    private var refineIndex = 0
    private var refineBestHz = 0.0
    private var refineBestDb = -300.0
    // Message temporaire affiche dans la ligne d'etat (4 s).
    private var flash = ""
    private var flashUntil = 0L

    private val handler = Handler(Looper.getMainLooper())

    /** Le service a arrete la reception (bouton Arreter de la notification). */
    private val stopReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != SdrService.ACTION_STOPPED) return
            stopped = true
            stopScan(false)
            wavPath = null
            iqPath = null
            replayFile = null
            offsetHz = 0.0
            usbConnection?.let { runCatching { it.close() } }
            usbConnection = null
            updateButtons()
        }
    }

    /** Reponse a la demande d'autorisation USB : cle acceptee ou generateur interne. */
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != RtlSdrUsb.ACTION_PERMISSION) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            val device = if (granted) RtlSdrUsb.deviceFrom(intent) else null
            startEngine(device)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        loadSquelch()
        requestNotificationPermission()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF0C1016.toInt())
            setPadding(16, 40, 16, 16)
        }

        spectrumView = SpectrumView(this).apply {
            sampleRateHz = 2.4e6
            // En direct on accorde ; en rejeu on deplace le canal dans la bande
            // deja enregistree, le spectre restant celui du fichier.
            onTune = { delta -> if (replayFile != null) setOffset(delta) else tune(centerHz + delta) }
        }
        root.addView(spectrumView, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        statusView = TextView(this).apply {
            setTextColor(0xFF9AC0D8.toInt())
            textSize = 12f
            gravity = Gravity.START
            // Apres un arret depuis la notification, la ligne d'etat relance.
            setOnClickListener {
                if (stopped) {
                    stopped = false
                    startLive()
                }
            }
        }
        root.addView(statusView)

        // Deux rangees de commandes : largeur partagee, texte reduit.
        fun row(): LinearLayout = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        }

        fun button(parent: LinearLayout, label: String, action: () -> Unit): Button {
            val b = Button(this)
            b.text = label
            b.isAllCaps = false
            b.textSize = 11f
            b.setPadding(2, 0, 2, 0)
            b.layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            b.setOnClickListener { action() }
            parent.addView(b)
            return b
        }

        val radioRow = row()
        button(radioRow, "−1 MHz") { tune(centerHz - 1e6) }
        button(radioRow, "+1 MHz") { tune(centerHz + 1e6) }
        modeButton = button(radioRow, SdrEngine.modeNames[mode]) { cycleMode() }
        button(radioRow, "Vol −") { setVolume(volume - 0.1f) }
        button(radioRow, "Vol +") { setVolume(volume + 0.1f) }
        root.addView(radioRow)

        val rxRow = row()
        gainButton = button(rxRow, "Gain auto") { cycleGain() }
        ppmButton = button(rxRow, "PPM 0") { cyclePpm() }
        squelchButton = button(rxRow, "SQL off") { cycleSquelch() }
        wavButton = button(rxRow, "REC WAV") { toggleWav() }
        iqButton = button(rxRow, "REC IQ") { toggleIq() }
        root.addView(rxRow)

        // Rejeu : choisir un .cu8 deja enregistre, puis se deplacer dans sa bande.
        val replayRow = row()
        replayButton = button(replayRow, "REJEU") { cycleReplay() }
        offsetDownButton = button(replayRow, "-25 kHz") { nudgeOffset(-25e3) }
        offsetCentreButton = button(replayRow, "centre") { setOffset(0.0) }
        offsetUpButton = button(replayRow, "+25 kHz") { nudgeOffset(25e3) }
        root.addView(replayRow)

        // Memories nommees et scan pilote par le squelch.
        val memoryRow = row()
        memoryAddButton = button(memoryRow, "MEM+") { addMemory() }
        memoryListButton = button(memoryRow, "MEM") { showMemories() }
        scanButton = button(memoryRow, "SCAN") { toggleScan() }
        root.addView(memoryRow)

        setContentView(root)

        ContextCompat.registerReceiver(
            this, usbReceiver, IntentFilter(RtlSdrUsb.ACTION_PERMISSION),
            ContextCompat.RECEIVER_NOT_EXPORTED)
        ContextCompat.registerReceiver(
            this, stopReceiver, IntentFilter(SdrService.ACTION_STOPPED),
            ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    override fun onStart() {
        super.onStart()
        if (SdrService.isRunning && !stopped) {
            // La reception n'a jamais ete coupee : il suffit de raccrocher l'UI.
            handler.removeCallbacks(statusRunnable)
            handler.post(statusRunnable)
            return
        }
        stopped = false
        startLive()
    }

    /** Direct : cle USB si elle est autorisee, sinon generateur interne. */
    private fun startLive() {
        val device = RtlSdrUsb.find(this)
        when {
            device == null -> startEngine(null)          // aucune cle branchee
            usbManager.hasPermission(device) -> startEngine(device)
            else -> usbManager.requestPermission(device, RtlSdrUsb.permissionIntent(this))
        }
    }

    override fun onStop() {
        super.onStop()
        // On ne coupe plus rien : SdrService garde l'audio, le focus et le
        // processus actifs, et les enregistrements continuent.
        handler.removeCallbacks(statusRunnable)
    }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { unregisterReceiver(usbReceiver) }
        runCatching { unregisterReceiver(stopReceiver) }
    }

    private fun startEngine(device: UsbDevice?) {
        SdrEngine.nativeStop() // idempotent : libere une eventuelle source precedente
        val started = if (device != null) startUsb(device) else SdrEngine.nativeStart()
        if (!started) {
            statusView.text = "Moteur natif indisponible"
            return
        }
        replayFile = null
        offsetHz = 0.0
        afterStart()
    }

    /** Rejoue un enregistrement IQ ; le fichier est relu en boucle. */
    private fun startReplay(file: File) {
        SdrEngine.nativeStop()
        if (!SdrEngine.nativeStartFile(file.absolutePath, SAMPLE_RATE)) {
            statusView.text = String.format("impossible de relire %s", file.name)
            replayFile = null
            updateButtons()
            return
        }
        replayFile = file
        setOffset(0.0)
        afterStart()
    }

    /** Reglages communs au direct et au rejeu, puis audio et rafraichissement. */
    private fun afterStart() {
        sourceLabel = File(SdrEngine.nativeSourceName()).name

        // Les paliers de gain dependent du tuner : on les relit a chaque demarrage.
        gains = SdrEngine.nativeGains()
        if (gainIndex >= gains.size) gainIndex = -1

        SdrEngine.nativeSetFrequency(centerHz)
        SdrEngine.nativeSetMode(mode)
        SdrEngine.nativeSetVolume(volume)
        applyGain()
        SdrEngine.nativeSetPpm(ppm)
        applySquelch()

        // Le service prend le relais : notification, focus audio, verrou d'eveil.
        SdrService.start(this, notificationLine())
        updateButtons()
        handler.removeCallbacks(statusRunnable)
        handler.post(statusRunnable)
    }

    /** Ligne courte affichee dans la notification. */
    private fun notificationLine(): String {
        val file = replayFile
        return if (file != null) String.format("%s · offset %+.0f kHz", SdrEngine.modeNames[mode], offsetHz / 1e3)
        else String.format("%.3f MHz · %s", centerHz / 1e6, SdrEngine.modeNames[mode])
    }

    /** Pousse la notification au plus une fois par seconde, et si elle change. */
    private fun pushNotification() {
        val now = System.currentTimeMillis()
        if (now - lastNotifyAt < 1000) return
        lastNotifyAt = now
        val line = notificationLine()
        if (line == lastNotified) return
        lastNotified = line
        SdrService.update(this, line)
    }

    /** Android 13+ : sans cette autorisation la notification reste invisible. */
    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < 33) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED) return
        ActivityCompat.requestPermissions(
            this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
    }

    /** Cycle entre les .cu8 enregistres, puis revient au direct. */
    private fun cycleReplay() {
        val dir = getExternalFilesDir(null) ?: filesDir
        val files = dir.listFiles { f -> f.isFile && f.name.endsWith(".cu8") }
            ?.sortedByDescending { it.lastModified() }
            .orEmpty()
        if (files.isEmpty()) {
            statusView.text = "aucun .cu8 enregistre"
            return
        }
        val current = replayFile
        if (current == null) {
            startReplay(files[0])
            return
        }
        val index = files.indexOfFirst { it.absolutePath == current.absolutePath }
        if (index + 1 >= files.size) startLive() else startReplay(files[index + 1])
    }

    /** Applique un decalage dans la bande ; la valeur rendue est deja bornee. */
    private fun setOffset(hz: Double) {
        SdrEngine.nativeSetOffsetHz(hz)
        offsetHz = SdrEngine.nativeOffsetHz()
    }

    private fun nudgeOffset(delta: Double) {
        if (replayFile == null) return
        setOffset(offsetHz + delta)
    }

    /**
     * Ouvre la cle et confie son descripteur a libusb. En cas d'echec (cle
     * occupee, autorisation refusee) on retombe sur le generateur interne.
     */
    private fun startUsb(device: UsbDevice): Boolean {
        val connection = usbManager.openDevice(device) ?: return SdrEngine.nativeStart()
        val fd = connection.fileDescriptor
        if (fd < 0 || !SdrEngine.nativeStartUsb(fd)) {
            runCatching { connection.close() }
            return SdrEngine.nativeStart()
        }
        usbConnection = connection
        return true
    }

    private fun tune(hz: Double) {
        // En rejeu il n'y a pas de frequence absolue : c'est setOffset() qui agit.
        if (replayFile != null) return
        centerHz = hz.coerceIn(24e6, 1766e6)
        SdrEngine.nativeSetFrequency(centerHz)
    }

    private fun cycleMode() {
        mode = (mode + 1) % SdrEngine.modeNames.size
        SdrEngine.nativeSetMode(mode)
        modeButton.text = SdrEngine.modeNames[mode]
    }

    private fun setVolume(value: Float) {
        volume = value.coerceIn(0f, 1f)
        SdrEngine.nativeSetVolume(volume)
    }

    // ------------------------------------------------------------------ gain

    /** Cycle gain automatique -> chaque palier du tuner -> automatique. */
    private fun cycleGain() {
        gainIndex = if (gainIndex + 1 >= gains.size) -1 else gainIndex + 1
        applyGain()
        updateButtons()
    }

    private fun applyGain() {
        val db = if (gainIndex < 0) -1.0 else gains[gainIndex].toDouble()
        SdrEngine.nativeSetGain(db)
    }

    // ------------------------------------------------------------------- ppm

    private fun cyclePpm() {
        val index = (ppmSteps.indexOf(ppm) + 1) % ppmSteps.size
        ppm = ppmSteps[if (index < 0) 0 else index]
        SdrEngine.nativeSetPpm(ppm)
        updateButtons()
    }

    // --------------------------------------------------------------- squelch

    /** off -> seuil courant -> seuil suivant -> ... -> off. */
    private fun cycleSquelch() {
        if (!squelchOn) {
            squelchOn = true
        } else {
            val index = squelchSteps.indexOf(squelchDb)
            if (index < 0 || index + 1 >= squelchSteps.size) squelchOn = false
            else squelchDb = squelchSteps[index + 1]
        }
        applySquelch()
        saveSquelch()
        updateButtons()
    }

    private fun applySquelch() {
        SdrEngine.nativeSetSquelch(squelchOn, squelchDb)
    }

    private fun loadSquelch() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        squelchOn = prefs.getBoolean(KEY_ON, false)
        val saved = prefs.getFloat(KEY_DB, DEFAULT_SQUELCH_DB)
        // Un seuil hors bareme (ancienne version) est ramene au pas le plus proche.
        squelchDb = squelchSteps.minByOrNull { kotlin.math.abs(it - saved) } ?: DEFAULT_SQUELCH_DB
    }

    private fun saveSquelch() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putBoolean(KEY_ON, squelchOn)
            .putFloat(KEY_DB, squelchDb)
            .apply()
    }

    // --------------------------------------------------------------- memories

    private data class Memory(val name: String, val hz: Double, val mode: Int)

    private fun loadMemories(): MutableList<Memory> {
        val raw = getSharedPreferences(PREFS, MODE_PRIVATE)
            .getString(KEY_MEMORIES, null) ?: return mutableListOf()
        val out = mutableListOf<Memory>()
        runCatching {
            val array = JSONArray(raw)
            for (i in 0 until array.length()) {
                val o = array.getJSONObject(i)
                out.add(Memory(o.optString("name"), o.optDouble("hz", 0.0), o.optInt("mode", 0)))
            }
        }
        return out
    }

    private fun saveMemories(list: List<Memory>) {
        val array = JSONArray()
        for (m in list) {
            array.put(JSONObject().put("name", m.name).put("hz", m.hz).put("mode", m.mode))
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_MEMORIES, array.toString())
            .apply()
    }

    private fun modeName(index: Int): String =
        SdrEngine.modeNames[index.coerceIn(0, SdrEngine.modeNames.size - 1)]

    /** Memorise la frequence courante sous un nom libre. */
    private fun addMemory() {
        if (replayFile != null) {
            flashMessage("mémoires indisponibles en rejeu")
            return
        }
        val input = EditText(this).apply {
            setText(String.format("%.3f %s", centerHz / 1e6, SdrEngine.modeNames[mode]))
            inputType = InputType.TYPE_CLASS_TEXT
            setSelection(text.length)
            setPadding(48, 24, 48, 0)
        }
        AlertDialog.Builder(this)
            .setTitle(String.format("Mémoriser %.3f MHz %s", centerHz / 1e6, SdrEngine.modeNames[mode]))
            .setView(input)
            .setPositiveButton("Enregistrer") { _, _ ->
                val name = input.text.toString().trim()
                    .ifEmpty { String.format("%.3f", centerHz / 1e6) }
                val list = loadMemories()
                list.removeAll { it.hz == centerHz && it.mode == mode }
                list.add(0, Memory(name, centerHz, mode))
                saveMemories(list)
                flashMessage(String.format("mémoire « %s » enregistrée", name))
            }
            .setNegativeButton("Annuler", null)
            .show()
    }

    /** Liste des memories : appui = rappel, appui long = suppression. */
    private fun showMemories() {
        val list = loadMemories()
        if (list.isEmpty()) {
            flashMessage("aucune mémoire enregistrée")
            return
        }
        val labels = list.map {
            String.format("%s — %.3f MHz %s", it.name, it.hz / 1e6, modeName(it.mode))
        }.toTypedArray()
        val dialog = AlertDialog.Builder(this)
            .setTitle("Mémoires (appui long : supprimer)")
            .setItems(labels) { _, which -> recall(list[which]) }
            .setNegativeButton("Fermer", null)
            .create()
        dialog.setOnShowListener {
            dialog.listView.setOnItemLongClickListener { _, _, which, _ ->
                confirmDeleteMemory(list[which])
                true
            }
        }
        dialog.show()
    }

    private fun recall(m: Memory) {
        stopScan(false)
        mode = m.mode.coerceIn(0, SdrEngine.modeNames.size - 1)
        modeButton.text = SdrEngine.modeNames[mode]
        SdrEngine.nativeSetMode(mode)
        centerHz = m.hz.coerceIn(24e6, 1766e6)
        // En rejeu il n'y a pas de frequence absolue : on repart en direct.
        if (replayFile != null) startLive() else SdrEngine.nativeSetFrequency(centerHz)
        flashMessage(String.format("mémoire « %s » rappelée", m.name))
    }

    private fun confirmDeleteMemory(m: Memory) {
        AlertDialog.Builder(this)
            .setTitle(m.name)
            .setMessage(String.format("Supprimer la mémoire %.3f MHz %s ?", m.hz / 1e6, modeName(m.mode)))
            .setPositiveButton("Supprimer") { _, _ ->
                val list = loadMemories()
                list.removeAll { it.name == m.name && it.hz == m.hz && it.mode == m.mode }
                saveMemories(list)
                flashMessage(String.format("mémoire « %s » supprimée", m.name))
            }
            .setNegativeButton("Annuler", null)
            .show()
    }

    // ------------------------------------------------------------------- scan

    private fun scanStepHz(): Double = if (mode == 0) 100e3 else 25e3

    /** Bande balayee : radiodiffusion FM pour le WBFM, toutes ondes sinon. */
    private fun scanBandHz(): Pair<Double, Double> =
        if (mode == 0) 87.5e6 to 108e6 else 24e6 to 1766e6

    private fun toggleScan() {
        if (scanning) {
            stopScan(false)
            return
        }
        if (replayFile != null) {
            flashMessage("scan indisponible en rejeu")
            return
        }
        // Le scan a besoin d'un critere : on active le squelch au seuil courant.
        if (!squelchOn) {
            squelchOn = true
            applySquelch()
            saveSquelch()
            updateButtons()
        }
        scanning = true
        scanHits = 0
        scanPhase = 0
        // On repart du canal suivant : sinon un scan lance sur une station
        // ouverte s'y arrete aussitot. Sur une porteuse deja ouverte, il faut
        // sauter plus loin que le canal (240 kHz), sinon le voisin a 100 kHz
        // la voit encore et le scan se reverrouille sur elle.
        val (lo, hi) = scanBandHz()
        val jump = if (SdrEngine.nativeSquelchOpen()) 200e3 else scanStepHz()
        val next = centerHz + jump
        tune(if (next > hi) lo else next)
        scanSettle = 1
        scanButton.text = "STOP SCAN"
        handler.removeCallbacks(scanRunnable)
        handler.post(scanRunnable)
    }

    private fun stopScan(found: Boolean) {
        if (!scanning) return
        scanning = false
        scanPhase = 0
        refineIndex = 0
        handler.removeCallbacks(scanRunnable)
        scanButton.text = "SCAN"
        if (found) flashMessage(String.format("scan arrêté : %.3f MHz", centerHz / 1e6))
    }

    /**
     * Le canal demodule fait 240 kHz : le squelch s'ouvre des 100 kHz a cote de
     * la porteuse. On mesure donc le centre et ses +/-100 kHz et on s'accorde
     * sur le maximum, sinon on s'arrete sur le flanc de la station.
     */
    private val scanRunnable = object : Runnable {
        override fun run() {
            if (!scanning) return
            if (scanSettle > 0) {
                // Le tuner vient de bouger : on laisse la puissance se stabiliser.
                scanSettle--
            } else if (scanPhase == 0) {
                if (SdrEngine.nativeSquelchOpen()) {
                    // Deux lectures ouvertes d'affilee = vraie porteuse.
                    if (++scanHits >= 2) {
                        scanPhase = 1
                        refineCentre = centerHz
                        refineIndex = 0
                        refineBestHz = centerHz
                        refineBestDb = -300.0
                    } else {
                        scanSettle = 1
                    }
                } else {
                    scanHits = 0
                    val (lo, hi) = scanBandHz()
                    val next = centerHz + scanStepHz()
                    tune(if (next > hi) lo else next)
                    scanSettle = 1
                }
            } else {
                val db = SdrEngine.nativeChannelDb()
                if (db > refineBestDb) {
                    refineBestDb = db
                    refineBestHz = centerHz
                }
                if (refineIndex < refineOffsets.size) {
                    tune(refineCentre + refineOffsets[refineIndex])
                    refineIndex++
                    scanSettle = 1
                } else {
                    tune(refineBestHz)
                    stopScan(true)
                    return
                }
            }
            handler.postDelayed(this, 300)
        }
    }

    private fun flashMessage(text: String) {
        flash = text
        flashUntil = System.currentTimeMillis() + 4000
    }

    // ------------------------------------------------------------- enregistrement

    private fun newRecordingPath(extension: String): String {
        val dir = getExternalFilesDir(null) ?: filesDir
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        return File(dir, "sdr_$stamp.$extension").absolutePath
    }

    private fun toggleWav() {
        if (SdrEngine.nativeIsRecordingWav()) {
            SdrEngine.nativeStopWav()
            wavPath = null
        } else {
            val path = newRecordingPath("wav")
            if (SdrEngine.nativeStartWav(path)) wavPath = path
            else statusView.text = String.format("impossible d'ecrire %s", path)
        }
        updateButtons()
    }

    private fun toggleIq() {
        if (SdrEngine.nativeIsRecordingIq()) {
            SdrEngine.nativeStopIq()
            iqPath = null
        } else {
            val path = newRecordingPath("cu8")
            if (SdrEngine.nativeStartIq(path)) iqPath = path
            else statusView.text = String.format("impossible d'ecrire %s", path)
        }
        updateButtons()
    }

    private fun updateButtons() {
        gainButton.text =
            if (gainIndex < 0 || gains.isEmpty()) "Gain auto"
            else String.format("Gain %.1f", gains[gainIndex])
        ppmButton.text = String.format("PPM %d", ppm)
        squelchButton.text = if (squelchOn) String.format("SQL %.0f", squelchDb) else "SQL off"
        wavButton.text = if (wavPath != null) "STOP WAV" else "REC WAV"
        iqButton.text = if (iqPath != null) "STOP IQ" else "REC IQ"
        val replaying = replayFile != null
        replayButton.text = if (replaying) "LIVE" else "REJEU"
        offsetDownButton.isEnabled = replaying
        offsetUpButton.isEnabled = replaying
        offsetCentreButton.isEnabled = replaying
    }

    private val statusRunnable = object : Runnable {
        override fun run() {
            if (stopped) {
                statusView.text = "réception arrêtée — toucher pour relancer"
                handler.postDelayed(this, 500)
                return
            }
            val db = SdrEngine.nativeChannelDb()
            val rec = buildString {
                if (wavPath != null) append("  |  REC WAV")
                if (iqPath != null) append("  |  REC IQ")
            }
            val file = replayFile
            val where = if (file != null) String.format("offset %+.1f kHz", offsetHz / 1e3)
            else String.format("%.3f MHz", centerHz / 1e6)
            val tag = if (file != null) "  |  REJEU ${file.name}" else ""
            if (flash.isNotEmpty() && System.currentTimeMillis() > flashUntil) flash = ""
            val extra = when {
                scanning -> String.format("  |  SCAN %.3f MHz", centerHz / 1e6)
                flash.isNotEmpty() -> "  |  $flash"
                else -> ""
            }
            statusView.text = String.format(
                "%s%s\n%s  |  %s  |  canal %.1f dBFS  |  vol %.0f%%%s%s",
                sourceLabel, tag, where, SdrEngine.modeNames[mode], db, volume * 100, rec, extra)
            pushNotification()
            spectrumView.invalidate()
            handler.postDelayed(this, 33)
        }
    }

    private companion object {
        const val PREFS = "sdrstudio"
        const val KEY_ON = "squelch_on"
        const val KEY_DB = "squelch_db"
        const val KEY_MEMORIES = "memories"
        const val DEFAULT_SQUELCH_DB = -70f
        const val REQUEST_NOTIFICATIONS = 1

        /** Paliers de seuil, du plus permissif au plus selectif. */
        val squelchSteps = listOf(-85f, -80f, -75f, -70f, -65f, -60f, -55f, -50f)

        // Sondes de recentrage : centre, puis les deux bords du canal.
        val refineOffsets = doubleArrayOf(-100e3, 0.0, 100e3)

        // Debit des captures faites par l'app (source RTL ou generateur) :
        // le .cu8 brut ne contient pas cette information.
        const val SAMPLE_RATE = 2.4e6
    }
}
