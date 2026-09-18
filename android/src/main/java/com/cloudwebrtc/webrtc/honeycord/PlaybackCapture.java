package com.cloudwebrtc.webrtc.honeycord;

import android.annotation.TargetApi;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * System-Ton der Bildschirmfreigabe auf Android (HoneyCord).
 *
 * <p>Pendant zu {@code honeycord_wasapi_loopback} (Windows), zum SCK-Audio-Pfad
 * (macOS) und zu {@code honeycord_pipewire_loopback} (Linux). Auf Android gibt
 * es dafür {@code AudioPlaybackCapture} (ab Android 10 / API 29): Mit derselben
 * {@link MediaProjection}, die den Bildschirm aufnimmt, darf man auch den Ton
 * mitnehmen, den andere Apps abspielen.
 *
 * <p><b>Warum der Weg über den Aufnahme-Puffer und nicht über einen eigenen
 * Track:</b> Auf den Desktop-Plattformen erzeugt der Fork eine eigene
 * {@code RTCAudioSource(kCustom)} und hängt einen zweiten Track an den Stream.
 * Die WebRTC-Java-API kennt so etwas nicht — dort speist genau ein
 * {@code AudioDeviceModule} alle lokalen Audio-Tracks, und das nimmt das Mikrofon
 * auf. Der einzige offene Zugang ist {@code JavaAudioDeviceModule
 * .Builder.setAudioBufferCallback}: ein Haken, der den frisch aufgenommenen
 * PCM-Puffer sieht, bevor WebRTC ihn verarbeitet.
 *
 * <p>Diese Klasse füllt genau dort die aufgenommenen System-Samples ein. Läuft
 * keine Freigabe, rührt sie nichts an — das Mikrofon geht unverändert durch.
 *
 * <p><b>Was der Nutzer davon merkt:</b> Bei laufender Freigabe trägt der
 * Audio-Track den Spiel-/Videoton. Ist das Mikrofon zusätzlich an, wird
 * gemischt; ist es aus, kommt nur der System-Ton. Das ist der Fall, für den
 * dieser Code gebaut wurde (HoneyCord, 01.09.2026: „Ton von der
 * Bildschirmfreigabe, aber nicht vom Mikro").
 */
@TargetApi(Build.VERSION_CODES.Q)
public final class PlaybackCapture {
    private static final String TAG = "HoneycordPlaybackCapture";

    /** Es gibt genau eine Freigabe zur Zeit, also genau eine Aufnahme. */
    private static volatile PlaybackCapture aktiv;

    private final AtomicBoolean laeuft = new AtomicBoolean(false);
    private AudioRecord record;
    private Thread leser;

    /**
     * Ringpuffer zwischen Aufnahme- und WebRTC-Faden.
     *
     * <p>Die beiden laufen unabhängig: {@code AudioRecord} liefert in seinem
     * eigenen Takt, WebRTC holt alle 10 ms. Ohne Puffer dazwischen würde der
     * Callback blockieren — und der blockiert dann die Mikrofon-Aufnahme, also
     * die ganze Verbindung.
     *
     * <p>Groß genug für rund eine halbe Sekunde bei 48 kHz Stereo; läuft er
     * über, wird das Älteste verworfen. Verzögerter Ton ist schlimmer als
     * fehlender.
     */
    private final byte[] ring = new byte[48000 * 2 * 2 / 2];
    private int schreibPos = 0, lesePos = 0, gefuellt = 0;
    private final Object ringSchloss = new Object();

    private int rate, kanaele;

    // ── Pegel-Messung (#125, Diagnose 17.09.2026) ─────────────────────────
    //
    // Tims Meldung: der Freigabeton vom Handy übertönt alles. Bevor wir den
    // Pegel absenken, wird gemessen, WIE laut Mikrofon und System-Ton hier im
    // Haken ankommen — also VOR Echo-Unterdrückung und Aussteuerung von WebRTC.
    //
    // ★ Läuft ausschließlich, solange eine Aufnahme aktiv ist (`fuelleIntern`
    // wird nur dann gerufen). Ein Gespräch mit Kamera oder nur Mikrofon
    // berührt diesen Code nicht: dort endet `ScreenAudio.pufferHaken` schon in
    // der ersten Zeile. Nur der WebRTC-Aufnahmefaden schreibt hier, deshalb
    // ohne Sperre.
    private static final long BERICHT_NS = 5_000_000_000L;
    private long berichtStart = 0;
    private double quadMik = 0, quadSys = 0;
    private long werteMik = 0, werteSys = 0;
    private int spitzeMik = 0, spitzeSys = 0;
    private long bytesGewollt = 0, bytesGeliefert = 0;
    private int aufrufe = 0, teilweise = 0, leer = 0;

    private static double dbfs(double wert) {
        return wert <= 0 ? -120.0 : 20.0 * Math.log10(wert / 32768.0);
    }

    /** Pegel eines 16-Bit-PCM-Bereichs aufsummieren. */
    private void messe(ByteBuffer puffer, int start, int bytes, boolean mik) {
        double q = 0;
        int spitze = 0;
        int n = 0;
        for (int i = 0; i + 1 < bytes; i += 2) {
            int v = (short) ((puffer.get(start + i) & 0xFF) | (puffer.get(start + i + 1) << 8));
            q += (double) v * v;
            int b = Math.abs(v);
            if (b > spitze) spitze = b;
            n++;
        }
        if (mik) { quadMik += q; werteMik += n; spitzeMik = Math.max(spitzeMik, spitze); }
        else { quadSys += q; werteSys += n; spitzeSys = Math.max(spitzeSys, spitze); }
    }

    private void messe(byte[] daten, int bytes) {
        double q = 0;
        int spitze = 0;
        int n = 0;
        for (int i = 0; i + 1 < bytes; i += 2) {
            int v = (short) ((daten[i] & 0xFF) | (daten[i + 1] << 8));
            q += (double) v * v;
            int b = Math.abs(v);
            if (b > spitze) spitze = b;
            n++;
        }
        quadSys += q; werteSys += n; spitzeSys = Math.max(spitzeSys, spitze);
    }

    private float letzterPegel = 1;

    private void berichteWennFaellig(boolean mischen) {
        long jetzt = System.nanoTime();
        if (berichtStart == 0) { berichtStart = jetzt; return; }
        if (jetzt - berichtStart < BERICHT_NS) return;
        double rmsMik = werteMik > 0 ? Math.sqrt(quadMik / werteMik) : 0;
        double rmsSys = werteSys > 0 ? Math.sqrt(quadSys / werteSys) : 0;
        int fuellung;
        synchronized (ringSchloss) { fuellung = gefuellt; }
        int bytesJeMs = Math.max(1, rate * kanaele * 2 / 1000);
        Log.i(TAG, String.format(java.util.Locale.ROOT,
                "[schirmton-pegel] modus=%s pegel=%.2f rate=%d kanaele=%d | mik rms=%.1f spitze=%.1f dBFS"
                        + " | system rms=%.1f spitze=%.1f dBFS | geliefert=%d%% teilweise=%d leer=%d"
                        + " aufrufe=%d ring=%dms",
                mischen ? "mischen" : "ersetzen", letzterPegel, rate, kanaele,
                dbfs(rmsMik), dbfs(spitzeMik), dbfs(rmsSys), dbfs(spitzeSys),
                bytesGewollt > 0 ? (int) (100 * bytesGeliefert / bytesGewollt) : 0,
                teilweise, leer, aufrufe, fuellung / bytesJeMs));
        berichtStart = jetzt;
        quadMik = quadSys = 0;
        werteMik = werteSys = 0;
        spitzeMik = spitzeSys = 0;
        bytesGewollt = bytesGeliefert = 0;
        aufrufe = teilweise = leer = 0;
    }

    private PlaybackCapture() {}

    /**
     * Aufnahme starten. Ruft man sie zweimal, gewinnt die neue.
     *
     * @param projection dieselbe Projection, die den Bildschirm aufnimmt
     * @param rate       Abtastrate, die WebRTC verlangt (aus dem Puffer-Haken)
     * @param kanaele    1 oder 2, ebenfalls aus dem Haken
     */
    public static synchronized boolean start(MediaProjection projection, int rate, int kanaele) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Log.w(TAG, "AudioPlaybackCapture braucht Android 10 - hier " + Build.VERSION.SDK_INT);
            return false;
        }
        if (projection == null) {
            Log.w(TAG, "keine MediaProjection - ohne sie gibt es keinen System-Ton");
            return false;
        }
        stop();
        PlaybackCapture p = new PlaybackCapture();
        if (!p.starteIntern(projection, rate, kanaele)) return false;
        aktiv = p;
        return true;
    }

    public static synchronized void stop() {
        PlaybackCapture p = aktiv;
        aktiv = null;
        if (p != null) p.stoppeIntern();
    }

    public static boolean istAktiv() {
        PlaybackCapture p = aktiv;
        return p != null && p.laeuft.get();
    }

    /**
     * Den PCM-Puffer aus dem {@code AudioBufferCallback} füllen.
     *
     * <p>Gibt {@code true} zurück, wenn etwas eingesetzt wurde. Ist keine
     * Aufnahme aktiv oder liegt gerade nichts an, bleibt der Puffer unberührt
     * und das Mikrofon geht durch.
     *
     * @param mischen {@code true} mischt zum vorhandenen Mikrofonsignal,
     *                {@code false} ersetzt es
     */
    public static boolean fuelle(ByteBuffer puffer, int bytes, boolean mischen, float pegel) {
        PlaybackCapture p = aktiv;
        return p != null && p.fuelleIntern(puffer, bytes, mischen, pegel);
    }

    /** Was die Aufnahme gerade liefert — für den Fall, dass WebRTC umschaltet. */
    public static boolean passt(int rate, int kanaele) {
        PlaybackCapture p = aktiv;
        return p != null && p.rate == rate && p.kanaele == kanaele;
    }

    private boolean starteIntern(MediaProjection projection, int rate, int kanaele) {
        this.rate = rate;
        this.kanaele = kanaele;
        try {
            // ★ Nur MEDIA und GAME mitnehmen. Sprachanrufe und
            // Benachrichtigungstöne wären ein Datenschutz-Problem, und Android
            // gibt sie ohnehin nicht heraus. Ein Spiel läuft unter GAME,
            // YouTube unter MEDIA — das ist genau, was gemeint ist.
            AudioPlaybackCaptureConfiguration konfig =
                    new AudioPlaybackCaptureConfiguration.Builder(projection)
                            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                            .addMatchingUsage(AudioAttributes.USAGE_GAME)
                            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                            .build();
            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(kanaele >= 2 ? AudioFormat.CHANNEL_IN_STEREO
                                                 : AudioFormat.CHANNEL_IN_MONO)
                    .build();
            int min = AudioRecord.getMinBufferSize(
                    rate,
                    kanaele >= 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0) min = rate * kanaele * 2 / 10;   // 100 ms als Rückfall
            record = new AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(min * 2)
                    .setAudioPlaybackCaptureConfig(konfig)
                    .build();
            if (record.getState() != AudioRecord.STATE_INITIALIZED) {
                Log.w(TAG, "AudioRecord nicht bereit (state=" + record.getState() + ")");
                record.release();
                record = null;
                return false;
            }
            record.startRecording();
            laeuft.set(true);
            final int haeppchen = Math.max(960, min / 2);
            leser = new Thread(() -> {
                byte[] tmp = new byte[haeppchen];
                while (laeuft.get()) {
                    AudioRecord r = record;
                    if (r == null) break;
                    int n = r.read(tmp, 0, tmp.length);
                    if (n > 0) schreibeInRing(tmp, n);
                    else if (n < 0) {
                        Log.w(TAG, "AudioRecord.read = " + n + " - Aufnahme endet");
                        break;
                    }
                }
            }, "honeycord-playback-capture");
            leser.setDaemon(true);
            leser.start();
            Log.i(TAG, "System-Ton laeuft: " + rate + " Hz, " + kanaele + " Kanal/Kanaele");
            return true;
        } catch (Throwable t) {
            // ★ `Throwable`, nicht `Exception`: Auf Geräten ohne die Fähigkeit
            // wirft der Konstruktor je nach Hersteller auch Fehler. Ein
            // fehlender System-Ton darf die Freigabe nie mitreißen.
            Log.e(TAG, "System-Ton konnte nicht starten", t);
            stoppeIntern();
            return false;
        }
    }

    private void stoppeIntern() {
        laeuft.set(false);
        Thread t = leser;
        leser = null;
        if (t != null) {
            try { t.join(300); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        }
        AudioRecord r = record;
        record = null;
        if (r != null) {
            try { r.stop(); } catch (Throwable ignored) { }
            try { r.release(); } catch (Throwable ignored) { }
        }
        synchronized (ringSchloss) { schreibPos = lesePos = gefuellt = 0; }
    }

    private void schreibeInRing(byte[] daten, int n) {
        synchronized (ringSchloss) {
            if (n > ring.length) {           // größer als der ganze Ring: nur das Ende
                System.arraycopy(daten, n - ring.length, ring, 0, ring.length);
                schreibPos = 0; lesePos = 0; gefuellt = ring.length;
                return;
            }
            for (int i = 0; i < n; i++) {
                ring[schreibPos] = daten[i];
                schreibPos = (schreibPos + 1) % ring.length;
            }
            gefuellt += n;
            if (gefuellt > ring.length) {
                // Übergelaufen: Der Lesezeiger rutscht mit. Wir verwerfen das
                // Älteste, nicht das Neueste — Ton, der zu spät kommt, ist
                // schlimmer als Ton, der fehlt.
                lesePos = schreibPos;
                gefuellt = ring.length;
            }
        }
    }

    /// Ein Sample begrenzen — weich, damit ein Anschlag nicht knackt.
    private static int deckel(int v) {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return v;
    }

    private boolean fuelleIntern(ByteBuffer puffer, int bytes, boolean mischen, float pegel) {
        if (!laeuft.get() || bytes <= 0) return false;
        // Messung: was das Mikrofon geliefert hat, BEVOR wir den Puffer anfassen.
        aufrufe++;
        bytesGewollt += bytes;
        letzterPegel = pegel;
        messe(puffer, puffer.position(), bytes, true);
        berichteWennFaellig(mischen);
        byte[] aus = new byte[bytes];
        int da;
        synchronized (ringSchloss) {
            da = Math.min(gefuellt, bytes);
            for (int i = 0; i < da; i++) {
                aus[i] = ring[lesePos];
                lesePos = (lesePos + 1) % ring.length;
            }
            gefuellt -= da;
        }
        if (da <= 0) { leer++; return false; }
        if (da < bytes) teilweise++;
        // Ein angebrochenes Sample-Paar würde als Knacken hörbar; auf ein
        // Vielfaches von 2 Byte (16 Bit) abrunden.
        da &= ~1;
        if (da <= 0) { leer++; return false; }
        bytesGeliefert += da;
        messe(aus, da);   // ROHpegel, vor der Dämpfung — so bleibt die Messung vergleichbar

        final int start = puffer.position();
        // ★★ GEMESSEN 18.09.2026 (Tracker #125, Tims Messlauf auf dem Odin 3):
        // Der System-Ton kam mit **−13 dBFS RMS** und Spitzen bis **0 dBFS**
        // im Haken an — Sprache liegt üblicherweise bei −20 bis −25 dBFS.
        // Er war also 8 bis 12 dB zu laut und schlug obendrein an die Grenze.
        // Die Medienlautstärke des Handys änderte daran nur rund 6 dB.
        // Deshalb wird er hier gedämpft, bevor WebRTC ihn zu sehen bekommt.
        for (int i = 0; i + 1 < da; i += 2) {
            final int p = start + i;
            int b = (short) ((aus[i] & 0xFF) | (aus[i + 1] << 8));
            int wert = Math.round(b * pegel);
            if (mischen) {
                // Mischen mit Sättigung: Stimme UND System-Ton gehen raus.
                int a = (short) ((puffer.get(p) & 0xFF) | (puffer.get(p + 1) << 8));
                wert += a;
            }
            wert = deckel(wert);
            puffer.put(p, (byte) (wert & 0xFF));
            puffer.put(p + 1, (byte) ((wert >> 8) & 0xFF));
        }
        // Was nicht gefüllt wurde, bleibt Mikrofon — bei einer Lücke ist das
        // ehrlicher als Stille.
        puffer.position(start);
        return true;
    }
}
