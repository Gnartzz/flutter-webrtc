package com.cloudwebrtc.webrtc.honeycord;

import android.os.Process;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/**
 * HoneyCord A2, Phase 2 (nur Alpha, Diagnose): Testton in die eigene Tonquelle.
 *
 * <p>Ein eigener Faden speist alle 10 ms einen Sinus 440 Hz, 48 kHz, Stereo,
 * int16 bei −20 dBFS in die Quelle. Der Takt folgt der MONOTONEN Uhr
 * ({@link System#nanoTime()}): Block n ist faellig bei {@code start + n·10 ms},
 * ein spaetes Aufwachen holt die fehlenden Bloecke sofort nach, statt die
 * Verspaetung wie ein {@code sleep(10)} aufzusummieren. Liegt der Faden mehr als
 * {@link #NEUANSATZ_NS} zurueck (Geraet schlief, Faden hing), setzt er den Takt
 * neu an und zaehlt die uebersprungenen Bloecke als Luecken.
 *
 * <p>Protokoll alle 5 s: {@code [a2] push 5s: bloecke=… geliefert=…% luecken=…
 * maxStau=…ms}. „geliefert" = angenommene Bloecke im Verhaeltnis zu denen, die
 * die Uhr im Zeitraum verlangt; „maxStau" = groesste Verspaetung eines Blocks
 * gegenueber seinem Soll-Zeitpunkt.
 */
public final class A2Probe {
  private static final String TAG = "FlutterWebRTCPlugin";

  static final int RATE = 48000;
  static final int KANAELE = 2;
  static final int FRAMES = RATE / 100;           // 10 ms
  private static final long BLOCK_NS = 10_000_000L;
  private static final long NEUANSATZ_NS = 200_000_000L;
  private static final long MELDUNG_NS = 5_000_000_000L;
  /** −20 dBFS: hoerbar, aber deutlich leiser als eine Stimme am Limit. */
  private static final double PEGEL = 0.1 * 32767.0;
  private static final double SCHRITT = 2.0 * Math.PI * 440.0 / RATE;

  private final EigeneTonquelle quelle;
  private final ByteBuffer puffer =
      ByteBuffer.allocateDirect(FRAMES * KANAELE * 2).order(ByteOrder.nativeOrder());
  private volatile boolean laeuft = false;
  private Thread faden;
  private double phase = 0.0;

  public A2Probe(EigeneTonquelle quelle) {
    this.quelle = quelle;
  }

  public synchronized void starten() {
    if (faden != null) return;
    laeuft = true;
    faden = new Thread(this::schleife, "hc-a2-probe");
    faden.start();
  }

  /** Faden beenden und abwarten (hoechstens 500 ms). Danach ruft niemand mehr pushData. */
  public synchronized void beenden() {
    laeuft = false;
    Thread f = faden;
    faden = null;
    if (f == null) return;
    LockSupport.unpark(f);
    try {
      f.join(500);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
    if (f.isAlive()) Log.w(TAG, "[a2] probe-faden endet nicht binnen 500 ms");
  }

  private void sinusFuellen() {
    puffer.clear();
    for (int i = 0; i < FRAMES; i++) {
      short w = (short) Math.round(Math.sin(phase) * PEGEL);
      for (int k = 0; k < KANAELE; k++) puffer.putShort(w);
      phase += SCHRITT;
      if (phase >= 2.0 * Math.PI) phase -= 2.0 * Math.PI;
    }
    puffer.rewind();
  }

  private void schleife() {
    try {
      Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
    } catch (Throwable ignoriert) {
      // nicht kritisch
    }
    long start = System.nanoTime();
    long n = 0;                  // Index des naechsten faelligen Blocks
    long meldungAb = start;
    long bloecke = 0, angenommen = 0, luecken = 0, fehler = 0, maxStauNs = 0;
    boolean fehlerGemeldet = false;
    while (laeuft) {
      long jetzt = System.nanoTime();
      long faellig = start + n * BLOCK_NS;
      long warte = faellig - jetzt;
      if (warte > 0) {
        LockSupport.parkNanos(warte);
        continue;                // nach dem Aufwachen neu pruefen (kann frueh sein)
      }
      long stau = -warte;
      if (stau > NEUANSATZ_NS) {
        long verpasst = stau / BLOCK_NS;
        luecken += verpasst;
        n += verpasst;
        continue;
      }
      if (stau > maxStauNs) maxStauNs = stau;
      sinusFuellen();
      try {
        quelle.pushData(puffer, RATE, KANAELE, FRAMES);
        angenommen++;
      } catch (IllegalArgumentException e) {
        fehler++;
        luecken++;
        if (!fehlerGemeldet) {
          fehlerGemeldet = true;
          Log.e(TAG, "[a2] pushData abgelehnt: " + e.getMessage());
        }
      } catch (Throwable t) {
        fehler++;
        luecken++;
        if (!fehlerGemeldet) {
          fehlerGemeldet = true;
          Log.e(TAG, "[a2] pushData fehlgeschlagen", t);
        }
      }
      bloecke++;
      n++;
      long seit = jetzt - meldungAb;
      if (seit >= MELDUNG_NS) {
        double erwartet = (double) seit / BLOCK_NS;
        double quote = erwartet > 0 ? 100.0 * angenommen / erwartet : 0.0;
        Log.i(TAG, String.format(Locale.ROOT,
            "[a2] push 5s: bloecke=%d geliefert=%.1f%% luecken=%d maxStau=%dms%s",
            bloecke, quote, luecken, maxStauNs / 1_000_000L,
            fehler > 0 ? " fehler=" + fehler : ""));
        meldungAb = jetzt;
        bloecke = 0;
        angenommen = 0;
        luecken = 0;
        fehler = 0;
        maxStauNs = 0;
        fehlerGemeldet = false;
      }
    }
  }
}
