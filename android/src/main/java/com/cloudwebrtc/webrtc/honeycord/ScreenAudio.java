package com.cloudwebrtc.webrtc.honeycord;

import android.media.projection.MediaProjection;
import android.os.Build;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * Bindeglied zwischen Bildschirmfreigabe, Audio-Modul und Dart (HoneyCord).
 *
 * <p>Drei Teile berühren den System-Ton, und keiner kennt die anderen:
 * <ul>
 *   <li>{@code OrientationAwareScreenCapturer} hat die {@link MediaProjection},
 *       weiß aber nichts von Audio.
 *   <li>Das {@code JavaAudioDeviceModule} hat den PCM-Puffer, weiß aber nichts
 *       von der Freigabe.
 *   <li>Dart entscheidet, ob der Ton überhaupt gewünscht ist.
 * </ul>
 * Diese Klasse hält die drei zusammen. Sie ist bewusst statisch: Es gibt genau
 * eine Freigabe und genau ein Audio-Modul.
 */
public final class ScreenAudio {
    private static final String TAG = "HoneycordScreenAudio";

    private ScreenAudio() {}

    /** Will Dart den Ton? Ohne das passiert nichts, auch bei laufender Freigabe. */
    private static volatile boolean gewuenscht = false;
    /** Soll zum Mikrofon gemischt werden (statt es zu ersetzen)? */
    private static volatile boolean mischen = false;
    private static volatile MediaProjection projection = null;
    /** Damit der Startversuch nicht bei jedem 10-ms-Puffer wiederholt wird. */
    private static volatile boolean startFehlgeschlagen = false;

    /**
     * Von Dart: Ton der Freigabe an oder aus.
     *
     * @param an      gewünscht?
     * @param mischen {@code true} = zusätzlich zum Mikrofon, {@code false} =
     *                statt seiner. Für „Spielton ja, Mikro nein" ist
     *                {@code false} richtig — dann trägt der Track nur den
     *                System-Ton, selbst wenn das Mikrofon technisch aufnimmt.
     */
    public static synchronized void setGewuenscht(boolean an, boolean mischen) {
        ScreenAudio.gewuenscht = an;
        ScreenAudio.mischen = mischen;
        ScreenAudio.startFehlgeschlagen = false;
        Log.i(TAG, "System-Ton " + (an ? "gewuenscht" : "aus")
                + (an ? (mischen ? " (mischen)" : " (statt Mikrofon)") : ""));
        if (!an) PlaybackCapture.stop();
    }

    public static boolean istGewuenscht() { return gewuenscht; }

    /** Läuft die Aufnahme wirklich? Für die Rückmeldung an Dart. */
    public static boolean laeuft() { return PlaybackCapture.istAktiv(); }

    /** Vom Screen-Capturer: die Projection steht. */
    public static synchronized void projectionBereit(MediaProjection mp) {
        projection = mp;
        startFehlgeschlagen = false;
        Log.i(TAG, "Projection da, System-Ton " + (gewuenscht ? "wird gestartet" : "nicht gewuenscht"));
    }

    /** Vom Screen-Capturer: die Freigabe endet. */
    public static synchronized void projectionWeg() {
        projection = null;
        PlaybackCapture.stop();
        Log.i(TAG, "Projection weg, System-Ton gestoppt");
    }

    /**
     * Vom Audio-Modul, für jeden aufgenommenen Puffer.
     *
     * <p>★ Der Start passiert HIER und nicht in {@link #projectionBereit}: Erst
     * dieser Aufruf nennt Abtastrate und Kanalzahl, die WebRTC tatsächlich
     * benutzt. Sie vorher zu raten hieße, den aufgenommenen Ton umrechnen zu
     * müssen — mit dieser Reihenfolge liefert {@code AudioRecord} von sich aus
     * das passende Format.
     *
     * <p>Diese Methode läuft auf dem Aufnahme-Faden von WebRTC und darf nie
     * blockieren; ein hängender Puffer-Haken legt die ganze Verbindung still.
     */
    public static void pufferHaken(ByteBuffer puffer, int kanaele, int rate, int bytes) {
        if (!gewuenscht || puffer == null || bytes <= 0) return;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return;

        if (!PlaybackCapture.istAktiv() || !PlaybackCapture.passt(rate, kanaele)) {
            if (startFehlgeschlagen) return;
            MediaProjection mp = projection;
            if (mp == null) return;                 // Freigabe läuft noch nicht
            synchronized (ScreenAudio.class) {
                if (startFehlgeschlagen) return;
                if (!PlaybackCapture.istAktiv() || !PlaybackCapture.passt(rate, kanaele)) {
                    if (!PlaybackCapture.start(mp, rate, kanaele)) {
                        // Einmal melden, dann Ruhe. Ohne die Sperre stünde bei
                        // einem Gerät ohne die Fähigkeit hundertmal je Sekunde
                        // dieselbe Zeile im Log.
                        startFehlgeschlagen = true;
                        Log.w(TAG, "System-Ton nicht verfuegbar - Mikrofon bleibt unveraendert");
                        return;
                    }
                }
            }
        }
        PlaybackCapture.fuelle(puffer, bytes, mischen);
    }
}
