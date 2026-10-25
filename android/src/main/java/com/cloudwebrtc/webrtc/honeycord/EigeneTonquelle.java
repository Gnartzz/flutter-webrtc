package com.cloudwebrtc.webrtc.honeycord;

import android.util.Log;

import org.webrtc.AudioSource;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/**
 * HoneyCord A2 (Plan ANDROID-SCHIRMTON-A2-PLAN.md, M1): Zugang zur eigenen
 * Tonquelle {@code org.webrtc.HoneycordCustomAudioSource} — PER REFLEXION.
 *
 * <p>Warum nicht direkt: Der Fork-Pin gilt fuer ALLE Kanaele, die eigene
 * libwebrtc-AAR zieht aber nur Android-Alpha (Gradle-Substitution in
 * {@code app/android/build.gradle.kts}). Beta und Stable bauen gegen die
 * offizielle AAR, in der es die Klasse nicht gibt — ein {@code new
 * HoneycordCustomAudioSource()} wuerde dort nicht kompilieren. Fehlt die Klasse
 * zur Laufzeit (Beta/Stable oder ein falscher Bau), meldet {@link #verfuegbar()}
 * {@code false}, und der Client bleibt beim heutigen Weg A1.
 *
 * <p>Klasse, Konstruktor und Methoden werden EINMAL aufgeloest und gemerkt.
 *
 * <p>App-Regel N-4: {@link #erzeugen()} erst nach {@code
 * PeerConnectionFactory.initialize} (die native Bibliothek muss geladen sein).
 * {@code pushData} wirft {@link IllegalArgumentException} bei falschen Bloecken;
 * der Aufrufer faengt sie ab.
 */
public final class EigeneTonquelle {
  private static final String TAG = "FlutterWebRTCPlugin";

  // Name als String, nie als Typ: Beta/Stable kennen die Klasse nicht. Die
  // APK-Pruefung in build-android.sh sucht deshalb den Typ-Deskriptor
  // `Lorg/webrtc/HoneycordCustomAudioSource;`, den nur die eigene AAR mitbringt
  // — dieser String hier steht in JEDER APK.
  private static final String KLASSE = "org.webrtc.HoneycordCustomAudioSource";

  private static boolean aufgeloest = false;
  private static Constructor<?> konstruktor;
  private static Method mPushData;
  private static Method mStop;
  private static Method mDispose;
  private static String grund = "";

  private final AudioSource quelle;

  private EigeneTonquelle(AudioSource quelle) {
    this.quelle = quelle;
  }

  /** Gibt es die eigene Tonquelle in der geladenen libwebrtc? Protokolliert beim ersten Aufruf. */
  public static synchronized boolean verfuegbar() {
    aufloesen();
    return konstruktor != null;
  }

  /** Warum die Quelle fehlt (leer, wenn sie da ist). */
  public static synchronized String grund() {
    aufloesen();
    return grund;
  }

  private static void aufloesen() {
    if (aufgeloest) return;
    aufgeloest = true;
    try {
      Class<?> k = Class.forName(KLASSE);
      if (!AudioSource.class.isAssignableFrom(k)) {
        throw new ClassCastException(k.getName() + " ist keine AudioSource");
      }
      Constructor<?> c = k.getConstructor();
      Method p = k.getMethod("pushData", ByteBuffer.class, int.class, int.class, int.class);
      Method s = k.getMethod("stop");
      Method d = k.getMethod("dispose");
      konstruktor = c;
      mPushData = p;
      mStop = s;
      mDispose = d;
      grund = "";
      Log.i(TAG, "[a2] quelle verfuegbar=ja (reflexion)");
    } catch (Throwable t) {
      konstruktor = null;
      mPushData = null;
      mStop = null;
      mDispose = null;
      grund = t.getClass().getSimpleName();
      Log.i(TAG, "[a2] quelle verfuegbar=nein (reflexion) grund=" + grund);
    }
  }

  /**
   * Erzeugt eine neue Quelle, oder {@code null}, wenn es keine gibt bzw. das
   * Erzeugen scheitert (z. B. {@code UnsatisfiedLinkError} vor initialize).
   */
  public static EigeneTonquelle erzeugen() {
    if (!verfuegbar()) return null;
    try {
      return new EigeneTonquelle((AudioSource) konstruktor.newInstance());
    } catch (InvocationTargetException e) {
      Log.e(TAG, "[a2] quelle erzeugen fehlgeschlagen", e.getCause());
    } catch (Throwable t) {
      Log.e(TAG, "[a2] quelle erzeugen fehlgeschlagen", t);
    }
    return null;
  }

  /** Die Quelle fuer {@code PeerConnectionFactory.createAudioTrack}. */
  public AudioSource quelle() {
    return quelle;
  }

  /**
   * Ein 10-ms-Block int16, verschachtelt, native Byte-Reihenfolge, ab Position 0.
   *
   * @throws IllegalArgumentException wie {@code HoneycordCustomAudioSource.pushData}
   */
  public void pushData(ByteBuffer daten, int rate, int kanaele, int frames) {
    try {
      mPushData.invoke(quelle, daten, rate, kanaele, frames);
    } catch (InvocationTargetException e) {
      Throwable ursache = e.getCause();
      if (ursache instanceof RuntimeException) throw (RuntimeException) ursache;
      if (ursache instanceof Error) throw (Error) ursache;
      throw new IllegalStateException(ursache);
    } catch (IllegalAccessException e) {
      throw new IllegalStateException(e);
    }
  }

  /** Beendet die Quelle; danach verwirft sie alle Bloecke. Wirft nie. */
  public void stop() {
    rufen(mStop, "stop");
  }

  /** Gibt die Java-Referenz frei (ruft intern stop()). Wirft nie. */
  public void dispose() {
    rufen(mDispose, "dispose");
  }

  private void rufen(Method m, String name) {
    try {
      m.invoke(quelle);
    } catch (InvocationTargetException e) {
      Log.w(TAG, "[a2] quelle " + name + " fehlgeschlagen", e.getCause());
    } catch (Throwable t) {
      Log.w(TAG, "[a2] quelle " + name + " fehlgeschlagen", t);
    }
  }
}
